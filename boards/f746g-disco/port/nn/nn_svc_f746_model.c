/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_f746_model.c
 * @brief   This board's `nn info`, `nn model load` and `nn model unload`
 *          behind the shared order (issue #131 step 7d).
 *
 * The ORDER of a load and an unload is svc/nn/nn_core_model.c's -- one copy for
 * every board -- and where a load ends is svc/nn/nn_swap.c's table.  What is here
 * is only what this board is: where its claim is (the NN session), what it must
 * see before it (no stream, SDRAM up, the first open), how it reads a model off
 * the SD card, and how its backend replaces one model with another.
 *
 * WHY IT IS ITS OWN FILE.  The rest of the adapter (nn_svc_f746.c) needs the
 * camera, the stream machine and the decoder; this half needs only svc/nn/nn.c,
 * so test/test_nn_model_life.c builds it against the real nn.c over a stub
 * backend and walks what a console cannot produce on demand -- a refusal that
 * restores nothing, an `nn info` from another console in the middle of a swap.
 *
 * Like the rest of the adapter it adds no second copy of the model: the
 * singleton and the session gate live in svc/nn/nn.c.  The state here is the
 * shared order's transition counter, which nothing else had, and -- on tflm,
 * since issue #159 -- what the open model's container CLAIMS (below).
 */
#include "nn_svc.h"
#include "nn_svc_adapter.h" /* nn_detail_set, nn_result (#130) */
#include "nn_core_model.h"  /* the shared load / unload order (#131) */

#include <string.h>

#include "nn.h"
#include "nn_camera.h"
#include "sdram.h"
#include "tx_api.h"

#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"       /* nn_active_base(): what a started plugin is handed */
#include "nn_container.h"
#include "plugin_claims.h"   /* the open model's claims and `nn info` lines */
#include "plugin_lease.h"    /* the one lock into the plugin (#159 Step 4b) */
#include "plugin_load.h"
#include "plugin_run_core.h" /* the loader (#159 Step 4b)                   */
#endif

/* ---- the shared order's state -------------------------------------------- */

/*
 * [!] ODD WHILE A LOAD OR AN UNLOAD IS CHANGING WHAT IS OPEN (issue #131 step
 * 7d; wio-lite-ai's since its issue #122).  `nn info` takes no session -- it
 * must answer while a stream holds it -- so it reads this on both sides of its
 * copy and answers BUSY rather than a name and a size from two models.
 */
static struct nn_core_model nn_model_life;

static unsigned nn_life_cs_enter(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	return (unsigned)interrupt_save;
}

static void nn_life_cs_exit(unsigned posture)
{
	TX_INTERRUPT_SAVE_AREA

	interrupt_save = (UINT)posture;
	TX_RESTORE
}

/* ---- plugin containers (issue #159 = #78 Steps 4a and 4b) ---------------- */

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * This board's policy for a plugin image.  svc/plugin/plugin_load.c holds no
 * board fact; every one of them arrives here, from board.cmake.
 *
 * Since Step 4b the plugin section is RUN: a load copies it into .plugin and
 * calls its entry (the load order's plugin hooks below), and from then on the
 * plugin decodes, draws and reports.  Since Step 4c it is the only decoder:
 * with no plugin the outputs are reported as tensors.
 *
 * The reservation's address is stated independently in the linker script,
 * ldscript/plugin_memory.ld and cmake/check_f746_layout.py, on purpose; the
 * layout gate also holds this policy's link_addr (F746_PLUGIN_BASE) to the
 * linked __plugin_start (cmake/check_f746_layout.py --policy-link-addr).
 */
#if !defined(F746_PLUGIN_BASE) || !defined(F746_PLUGIN_MAX) ||               \
    !defined(F746_PLUGIN_TARGET_ID) || !defined(PLUGIN_VENEER_BASE_COST) ||    \
    !defined(F746_PLUGIN_STACK_NN_WORK) || !defined(F746_PLUGIN_STACK_GUIX) ||  \
    !defined(F746_PLUGIN_STACK_SHELL)
#error "board.cmake must define the F746_PLUGIN_* policy and veneer_cost_gate() PLUGIN_VENEER_BASE_COST for a tflm build"
#endif
/* The provisional c of Step 4a is gone; the checked one replaced it.  Two
 * numbers for one fact is how the loader would charge the unchecked one. */
#if defined(F746_PLUGIN_VENEER_COST_4A)
#error "F746_PLUGIN_VENEER_COST_4A is Step 4a's provisional c; the policy takes PLUGIN_VENEER_BASE_COST (issue #159 Step 4b)"
#endif

/* A container's model section starts on PLUGIN_MODEL_ALIGN and the backend
 * adopts it in place, so that must meet what the backend requires.  Stated as
 * MET, not assumed. */
_Static_assert(PLUGIN_MODEL_ALIGN % NN_MODEL_ALIGN == 0u,
               "a container's model section must meet the backend's alignment");

/*
 * c is PLUGIN_VENEER_BASE_COST, which veneer_cost_gate() derives and checks
 * against the shipped image and compiles in -- and the build reads the value
 * this policy ACTUALLY holds back out of shell.elf (PLUGIN_POLICY_PROBE below,
 * cmake/check_policy_probe.py), with the seven allowances.
 *
 * The slot -> allowance mapping is this board's (decode on the nn worker, draw
 * on the GUIX thread that draws the boxes, the rest on a console); the asserts
 * against each thread's stack are in nn_svc_f746.c and nn_camera.c.
 */
static const struct plugin_policy nn_plugin_policy = {
	.target_id      = F746_PLUGIN_TARGET_ID,
	.link_addr      = F746_PLUGIN_BASE,
	.capacity       = F746_PLUGIN_MAX,
	.image_align    = PLUGIN_IMAGE_ALIGN,
	.caps_supported = PLUGIN_CAP_KNOWN_MASK,
	.stack_limit    = {
		[PLUGIN_SLOT_ENTRY]     = F746_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_SHAPES_OK] = F746_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_DECODE]    = F746_PLUGIN_STACK_NN_WORK,
		[PLUGIN_SLOT_DRAW]      = F746_PLUGIN_STACK_GUIX,
		[PLUGIN_SLOT_REPORT]    = F746_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_PARAM_SET] = F746_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_PARAM_GET] = F746_PLUGIN_STACK_SHELL,
	},
	.veneer_cost    = PLUGIN_VENEER_BASE_COST,
	.stack_accounting = PLUGIN_STACK_ACCOUNTING,
};
/* Where the build finds the policy this firmware REALLY holds, whatever -D
 * reached this compile (cmake/check_policy_probe.py, issue #111). */
PLUGIN_POLICY_PROBE(nn_plugin_policy);

/*
 * What the container of the OPEN model claimed, or nothing.
 *
 * The store, its snapshot rule and the `nn info` lines are
 * svc/plugin/plugin_claims.h's (issue #159 Step 4b: wio-lite-ai's, moved so
 * this board does not keep a second copy).  Settled only by the shared order's
 * commit / forget, i.e. only once the backend adopted (or refused) the model,
 * and while the transition counter is odd -- so `nn info`, which takes no lock,
 * reads the counter in the same masked section as these and says "a model load
 * is in progress" rather than pairing one model with another's claims.  "from"
 * is this board's: the file the claims came with, its length and the CRC the
 * load printed, so a load landing between `nn info`'s model line and its plugin
 * line shows up as a mismatch.
 */
#define NN_CLAIMS_PATH_MAX 48u
/* "<path>, 4294967295 B, file crc32 XXXXXXXX" must survive whole. */
_Static_assert((NN_CLAIMS_PATH_MAX - 1u) + 35u < PLUGIN_CLAIMS_FROM_MAX,
               "a container's path, length and CRC would be cut on `nn info`");

static struct plugin_claims_store nn_claims;

static void nn_claims_set(const struct plugin_claims *c)
{
	unsigned posture = nn_life_cs_enter();

	plugin_claims_set(&nn_claims, c);
	nn_life_cs_exit(posture);
}
#endif /* CONFIG_NN_BACKEND_TFLM */

/* ---- the backend ---------------------------------------------------------- */

/** One load's resolution, on the console's stack. */
struct nn_f746_job {
	nn_svc_read_fn read;
	void          *ctx;
	void          *buf;
	uint32_t       cap;
	uint32_t       len;
	/* What the backend is handed: the whole file, or -- for a container --
	 * its model section, in place inside the staging slot (issue #159). */
	const void    *model_at;
	uint32_t       model_len;
#if defined(CONFIG_NN_BACKEND_TFLM)
	int                  is_container;
	struct plugin_claims claims;
#endif
};

/*
 * The backend's own transaction: nn_model_reload() closes, opens the new model,
 * and restores the previous one -- or leaves the backend EMPTY -- if that fails.
 *
 * [!] THE STATE IS THE RELOAD'S OWN OUTCOME, NOT A QUESTION ASKED AFTERWARDS
 * (issue #122 P1).  @p model_after is whether a MODEL is left (issue #131 P16):
 * the handle stays open empty, and asking nn_model_open() afterwards would not
 * be a question on a closed singleton.
 */
static int nn_life_swap(struct nn_core_model_job *j, int *model_after)
{
	struct nn_op_result *res = j->res;
	const struct nn_f746_job *f = j->board;
	int rc;

	rc = nn_model_reload(f->model_at, f->model_len, j->spec->path,
	                     model_after);
	if (rc == 0)
		return NN_SVC_OK;
	nn_detail_set("the model was refused (%d)", rc);
	return NN_SVC_ERR_ARG;
}

/*
 * An unload, and every load that ends EMPTY.  The singleton stays open with NO
 * model in it (issue #131 P16).  A backend that cannot swap (null, stedgeai)
 * has no release and keeps its model -- `nn model unload` still answers
 * "unloaded" there, the old answer, unchanged.
 */
static void nn_life_release(void)
{
	(void)nn_model_release();
}

static const struct nn_core_model_backend nn_life_backend = {
	.swap      = nn_life_swap,
	.release   = nn_life_release,
	.has_model = nn_model_loaded,
	.strerror  = NULL,     /* a refusal is worded by the swap itself */
	.reserved  = nn_arena_reserved,
	.used      = NULL,     /* `nn info` asks the open model */
};

/* ---- the board ------------------------------------------------------------ */

/* Every tag but a path, refused before anything is acquired.  `builtin` is
 * one of them since issue #159 Step 4c: no build of this board that can load
 * a model carries one in the image any more. */
static void nn_life_spec_refused(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	const struct nn_spec *spec = j->spec;

	if (spec == NULL)
		return;
	nn_detail_set("this board loads a model from the SD card (--path); it "
	              "has no %s",
	              spec->tag == NN_SPEC_NAME ? "asset store"
	              : spec->tag == NN_SPEC_SLOT ? "slot index"
	              : spec->tag == NN_SPEC_ADDR ? "raw model window"
	              : spec->tag == NN_SPEC_BUILTIN ? "built-in model"
	                                             : "such source");
}

/*
 * The preconditions.  The claim -- the NN session -- is nn_life_claim_take(),
 * which the shared order calls next (issue #138: until then the hook took no
 * job, so it could not word a refusal, and the session was taken here).
 *
 * [!] THE FIRST OPEN AFTER BOOT HAPPENS HERE, BEFORE THE SESSION (issue #131
 * P16).  On a backend with a model in the image (null, stedgeai) that open
 * adopts it and does not return until it is built -- if another console's
 * `nn info` got there first, this waits for that build -- so nothing under the
 * session runs beside it, and every later open builds nothing.  tflm has had
 * nothing to adopt since issue #159 Step 4c: its first open finds it empty.  An unload opens first and asks about the stream after, as
 * before the order was shared.
 */
static int nn_life_admit(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_model *m = NULL;
	int rc;

	/* An unload's open may be refused while `sdram test` holds .sdram.ai
	 * (issue #152); the claim below then refuses it as busy, worded there. */
	if (j->spec == NULL)
		(void)nn_model_open(&m);
	if (nn_camera_running()) {
		nn_detail_set("stop the inference stream first (`nn stream stop`)");
		return NN_SVC_ERR_BUSY;
	}
	if (j->spec != NULL) {
		if (!sdram_is_up()) {
			nn_detail_set("SDRAM is not up, and the model buffer lives "
			              "there");
			return NN_SVC_ERR_STATE;
		}
		rc = nn_model_open(&m);
		if (rc == NN_ERR_SCRATCH_HELD) {
			/* Transient, and nothing was built: BUSY, before the swap. */
			nn_detail_set(".sdram.ai is held by `sdram test`; try "
			              "again when it finishes");
			return NN_SVC_ERR_BUSY;
		}
		if (rc != 0 || m == NULL) {
			nn_detail_set("the model could not be opened");
			return NN_SVC_ERR_STATE;
		}
	}
	return NN_SVC_OK;
}

/*
 * THE CLAIM, worded.  claim_give gives it back.
 *
 * [!] THE SESSION IS CLAIMED BEFORE THE SLOT IS CHOSEN, so slot selection,
 * the SD read and the swap are atomic against another console: otherwise
 * a second shell could flip the chosen (inactive) slot to active between
 * load_region() and the read, and the live flatbuffer would be corrupted.
 */
static int nn_life_claim_take(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;

	int rc = nn_session_try_acquire();

	if (rc == NN_ERR_SCRATCH_HELD) {
		nn_detail_set(".sdram.ai is held by `sdram test`; try again when "
		              "it finishes");
		return NN_SVC_ERR_BUSY;
	}
	if (rc != 0) {
		nn_detail_set("NN busy (a stream, run or bench is active)");
		return NN_SVC_ERR_BUSY;
	}
	return NN_SVC_OK;
}

/* What the read needs, asked before the SD card is touched. */
static int nn_life_prepare(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;

	if (nn_model_load_region(&f->buf, &f->cap) != 0) {
		nn_detail_set("this backend has no runtime model loader");
		return NN_SVC_ERR_NOSUP;
	}
	if (f->read == NULL) {
		nn_detail_set("no filesystem reader was supplied for --path");
		return NN_SVC_ERR_NOSUP;
	}
	return NN_SVC_OK;
}

/*
 * The file, into the staging slot the backend handed out -- and, on tflm, split
 * if it is a plugin container (issue #159).
 *
 * A payload that is not a container is a bare model and takes the old path
 * unchanged.  A container goes through svc/plugin/plugin_load.c -- the same
 * validator the host asset build runs -- with this board's policy, and its
 * MODEL SECTION is handed to the backend IN PLACE, at its offset inside the
 * staged file (nn.h, NN_MODEL_ALIGN): copying it down to the slot's start would
 * overwrite the validated container with bytes nobody validated.  A refused
 * container is a refused load (REFUSED: nothing changed).
 *
 * j->bare = 0 for a container is the truth, and in Step 4a changes nothing:
 * the plugin hooks are unset, so the shared order touches no plugin either way
 * (nn_core_model.h).
 */
static int nn_life_fetch(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;

	j->bare = 1;
	/* The reader prints its own failure: it is the board's shell-layer file
	   and it has the console this command came in on. */
	if (f->read(f->ctx, j->spec->path, f->buf, f->cap, &f->len) != 0) {
		nn_detail_clear();
		return NN_SVC_ERR_ARG;
	}
	f->model_at  = f->buf;
	f->model_len = f->len;
#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE CRC IS OVER THE SAME BYTES THE READER PRINTED ITS CRC OVER: the
	 * reader (cmds/cmd_nn_board.c) calls nn_container_crc() on (f->buf, f->len)
	 * as it returns them, and this calls it again on exactly that range, before
	 * anything else writes the slot.  So the number `nn info` prints beside the
	 * claims is the number the load printed, for the file the operator copied.
	 */
	uint32_t crc;

	if (nn_container_crc(f->buf, f->len, &crc)) {
		enum plugin_result pr = plugin_parse(f->buf, f->len,
		                                     &nn_plugin_policy,
		                                     &f->claims.view);
		char path[NN_CLAIMS_PATH_MAX];
		size_t n;

		if (pr != PLUGIN_OK) {
			nn_detail_set("%s is a container this firmware refuses: %s",
			              j->spec->path, plugin_result_name(pr));
			return NN_SVC_ERR_ARG;
		}
		f->is_container  = 1;
		j->bare          = 0;
		n = strlen(j->spec->path);
		if (n >= sizeof path)
			n = sizeof path - 1u;
		memcpy(path, j->spec->path, n);
		path[n] = '\0';
		/* The whole container, as read off the card, and the CRC of exactly
		 * those bytes (nn_container.h). */
		nn_detail_to(f->claims.from, sizeof f->claims.from,
		             "%s, %lu B, file crc32 %08lX", path,
		             (unsigned long)f->len, (unsigned long)crc);
		f->model_at  = (const uint8_t *)f->buf + f->claims.view.model_off;
		f->model_len = f->claims.view.model_len;
	}
#endif
	return NN_SVC_OK;
}

/*
 * [!] THE LAST RESULT GOES WITH THE MODEL IT CAME FROM (issue #118), whenever
 * what is open changed -- decided by the shared table, not on every attempt.
 * Under the session, so no worker is publishing.
 */
static void nn_life_invalidate(void)
{
	nn_camera_record_invalidate();
}

/* The backend owns the model's identity (its name).  What is kept here is the
 * container's claims (tflm, issue #159): no model is open, so none describe
 * it. */
static void nn_life_forget(void)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	nn_claims_set(NULL);
#endif
}

/*
 * The job's model is now the open one: its container's claims, or none for a
 * bare model.  Called by the shared order only once the backend adopted it,
 * while the transition counter is odd.  A refusal that restored the previous
 * model does not commit, so the previous claims stay with the previous model.
 */
static void nn_life_commit(struct nn_core_model_job *j)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	const struct nn_f746_job *f = j->board;

	nn_claims_set((f != NULL && f->is_container) ? &f->claims : NULL);
#else
	(void)j;
#endif
}

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * [!] THE LEASE IS TAKEN BEFORE ANYTHING CHANGES, AND ITS FAILURE IS AN ANSWER
 * (issue #159, wio-lite-ai's #110).  The NN session keeps the WORKER out, but
 * not another CONSOLE's plugin callback: `nn thresh` takes no session.  Without
 * this, a background job could be inside a plugin's param_set while this
 * overwrote the reservation under it.  Taken before the first change, a
 * timeout costs nothing: the model and the plugin are both as they were.
 */
static int nn_life_lease_take(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;

	if (plugin_lease_take())
		return 1;
	nn_detail_set("the decoder is busy -- try again");
	return 0;
}

/*
 * The plugin half of a load, run only once the backend has taken the new model
 * and only for a container (a bare model unloads instead, in the shared order).
 *
 * [!] A REFUSED PLUGIN IS A FAILED LOAD OF A MODEL THAT IS NOW OPEN (issue #122
 * D6).  The copy into the reservation has already destroyed the previous
 * plugin, so there is no rollback: the model stays open with nothing reading
 * it (Step 4c: no resident decoder), and the status says the load did not
 * give what was asked for.  plugin_run_load() also logs its own reason.
 *
 * [!] THE STAGING REGION IS PASSED, NOT LOOKED UP: the slot the reader filled
 * (f->buf, f->cap) is the region the container lies in, and the backend's
 * load_region() would now answer with the other slot.
 */
static int nn_life_plugin_start(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;
	enum plugin_run_result pr;

	/* entry()'s depth is sampled in the loader's exec_ok hook. */
	pr = plugin_run_load(&f->claims.view, f->buf, f->buf, f->cap,
	                     nn_active_base());
	if (pr == PLUGIN_RUN_OK || pr == PLUGIN_RUN_NO_PLUGIN)
		return NN_SVC_OK;
	nn_detail_set("%s: %s", j->spec->path, plugin_run_why(pr));
	return NN_SVC_ERR_ARG;
}

/* Unpublish: a plugin left loaded would be a decoder for a model that is gone,
 * waiting to interpret the next one. */
static void nn_life_plugin_unload(void)
{
	plugin_run_unload();
}
#endif /* CONFIG_NN_BACKEND_TFLM */

/*
 * The plugin hooks exist only in a tflm build (issue #159 Step 4b): the other
 * backends cannot carry a container, so lease_take is NULL there and the
 * shared order calls no plugin hook at all (nn_core_model.h).  The counter
 * still brackets the swap.  hw_down is NULL: what an EMPTY ending must bring
 * down is the backend's model, and the order releases that itself.
 */
static const struct nn_core_model_board nn_life_board = {
	.cs_enter      = nn_life_cs_enter,
	.cs_exit       = nn_life_cs_exit,
	.backend       = &nn_life_backend,
	.tags          = NN_CORE_MODEL_TAG(NN_SPEC_PATH),
	.spec_refused  = nn_life_spec_refused,
	.check_spec    = NULL,     /* a path has no argument to range-check */
	.admit         = nn_life_admit,
	.claim_take    = nn_life_claim_take,
	.claim_give    = nn_session_release,
	.prepare       = nn_life_prepare,
	.fetch         = nn_life_fetch,
#if defined(CONFIG_NN_BACKEND_TFLM)
	.lease_take    = nn_life_lease_take,
	.lease_give    = plugin_lease_give,
	.plugin_start  = nn_life_plugin_start,
	.plugin_unload = nn_life_plugin_unload,
#else
	.lease_take    = NULL,
	.lease_give    = NULL,
	.plugin_start  = NULL,
	.plugin_unload = NULL,
#endif
	.hw_down       = NULL,
	.invalidate    = nn_life_invalidate,
	.forget        = nn_life_forget,
	.commit        = nn_life_commit,
	.geom_clear    = NULL,
};

/* ---- info ---------------------------------------------------------------- */

/*
 * The transition counter for `nn info`, read under this board's critical
 * section -- what nn_core_model_seq() does, without naming the order's board
 * table: `nn info` is in every build, and a build with no `nn model load` (the
 * null backend) must not keep the whole load path alive through it.
 */
static uint32_t nn_life_seq(void)
{
	unsigned posture = nn_life_cs_enter();
	uint32_t v       = nn_model_life.seq;

	nn_life_cs_exit(posture);
	return v;
}

void nn_svc_info(struct nn_svc_info *out)
{
	const struct nn_backend_info *bi = nn_backend();
	struct nn_model *m = NULL;
	uint32_t seq0, seq1;
	int held, rc;

	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, bi ? bi->name : NULL);
	nn_svc_str(out->version, sizeof out->version, bi ? bi->version : NULL);

	/* [!] THE RESERVATION, NOT THE USE (issue #131 P3).  This line used to
	 * carry what the model's activations take, under the word "reserved"; that
	 * is `used` now, as on grove-vision-ai-v2.  A fixed fact of the build, so it
	 * needs no claim and stands even when the rest is busy. */
	out->arena_bytes = nn_arena_reserved();

	rc = nn_model_open(&m);
	if (rc == NN_ERR_SCRATCH_HELD) {
		/* `sdram test` holds .sdram.ai and nothing was opened (issue
		 * #152): a transient answer, not "nothing is loaded". */
		out->avail_identity = (uint8_t)NN_AVAIL_BUSY;
		out->avail_runtime  = (uint8_t)NN_AVAIL_BUSY;
		out->avail_tensors  = (uint8_t)NN_AVAIL_BUSY;
		return;
	}
	if (rc != 0 || m == NULL)
		return;

	/* Copied, not borrowed: the backend owns this name and a reload replaces
	   it, so the caller must not hold a pointer into it while printing. */
	/*
	 * [!] THE NAME IS COPIED UNDER THE SESSION WHEN THE SESSION IS FREE, and
	 * copied anyway when it is not.  That is a deliberate middle, not an
	 * oversight:
	 *
	 *   Taking the session unconditionally would make `nn info` REFUSE while a
	 *   stream runs -- the worker holds it for the whole stream -- and that is
	 *   exactly when the report is worth asking for.
	 *
	 *   Not taking it at all leaves the copy able to race a reload, which
	 *   rewrites the backend's name buffer byte by byte.
	 *
	 * So: try, and when the session is held, ask the transition counter
	 * whether the copy describes one model (below).
	 */
	seq0 = nn_life_seq();
	held = (nn_session_try_acquire() == 0);
	/* [!] "A MODEL IS THERE", NOT "THE SINGLETON OPENED" (issue #131 P16, as
	 * wio-lite-ai since its issue #122 P2).  The singleton stays open after
	 * `nn model unload` with nothing in it. */
	out->model_active = nn_model_present(m) ? 1u : 0u;
	nn_svc_str(out->model, sizeof out->model, nn_model_name(m));
	out->arena_used  = out->model_active ? nn_activations_bytes(m) : 0u;
	if (held)
		nn_session_release();
	seq1 = nn_life_seq();

	/*
	 * [!] A LOAD IN FLIGHT IS BUSY, NOT A NAME AND A SIZE FROM TWO MODELS
	 * (issue #131 step 7d; wio-lite-ai's rule since its issue #122).  The
	 * backend rewrites the name and the tensors while it swaps -- the new
	 * model, or the previous one on a rollback -- so a copy taken then can
	 * name one model beside another's size.  When the session was free no load
	 * can have run; when it was not, the copy stands only if no load or unload
	 * was between its two steps at either end of it, nor ran whole in between
	 * (svc/nn/nn_core_model.c).  A stream holds the session and never moves the
	 * counter, so its report is what it was.
	 */
	if (!nn_core_model_copy_stands(seq0, seq1, held)) {
		out->model_active   = 0u;
		out->model[0]       = '\0';
		out->arena_used     = 0u;
		out->avail_identity = (uint8_t)NN_AVAIL_BUSY;
		out->avail_runtime  = (uint8_t)NN_AVAIL_BUSY;
		out->avail_tensors  = (uint8_t)NN_AVAIL_BUSY;
		return;
	}

	/* [!] Say plainly when nothing is being inferred, so a latency from
	 * `nn bench` is never mistaken for a model's. */
	if (bi && strcmp(bi->name, "null") == 0)
		nn_svc_str(out->source, sizeof out->source,
		           "synthetic workload, not inference");

	/* Every section is answered here, streaming or not -- which is the point of
	   the copy above (issue #99 made this explicit rather than implied). */
	out->avail_identity = (uint8_t)NN_AVAIL_OK;
	out->avail_runtime  = (uint8_t)NN_AVAIL_OK;
	out->avail_tensors  = out->model_active ? (uint8_t)NN_AVAIL_OK
	                                        : (uint8_t)NN_AVAIL_NA;
}

/* ---- model lifecycle ----------------------------------------------------- */

/* Point the model at @p spec -- over an open model too.  The order and its
 * endings: svc/nn/nn_core_model.h. */
void nn_svc_model_load(const struct nn_spec *spec, nn_svc_read_fn read,
                       void *ctx, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	struct nn_f746_job f;
	struct nn_core_model_job j;

	nn_detail_clear();
	memset(&f, 0, sizeof f);
	f.read  = read;
	f.ctx   = ctx;
	j.spec  = spec;
	j.res   = res;
	j.board = &f;
	nn_core_model_load(&nn_model_life, &nn_life_board, &j, state);
}

/* Idempotent: unloading nothing succeeds (svc/nn/nn_core_model.h). */
void nn_svc_model_unload(struct nn_op_result *res)
{
	struct nn_core_model_job j;

	nn_detail_clear();
	j.spec  = NULL;
	j.res   = res;
	j.board = NULL;
	nn_core_model_unload(&nn_model_life, &nn_life_board, &j);
}

/* ---- what `nn info` adds -------------------------------------------------- */

/*
 * What the open model's container CLAIMS about its plugin (issue #159 = #78
 * Step 4a), and where a plugin would live.
 *
 * [!] CLAIMS, AND THE LINE SAYS SO.  Every field comes from a manifest
 * plugin_parse() validated; "running" is the one word about what the loader
 * actually branched into (since Step 4b), and "validated, not loaded" a
 * container whose plugin was refused or never reached.
 *
 * Two CRCs, which are different things: `crc` on the plugin line is the
 * plugin-section digest the container carries (it moves when the plugin's
 * bytes do, not when the model does); `file crc32` on the from-line is the CRC
 * of the WHOLE container as read off the card -- the number the load printed
 * and the asset receipt names (nn_container.h).
 *
 * [!] NOTHING IS HELD WHILE THIS PRINTS.  The claims and the transition counter
 * are copied out in one masked section and written afterwards, by
 * svc/plugin/plugin_claims.c (issue #159 Step 4b).
 */
#if defined(CONFIG_NN_BACKEND_TFLM)
extern uint8_t __plugin_start[], __plugin_end[];   /* the linker's reservation */

void nn_svc_info_extra(nn_svc_write_fn write, void *ctx)
{
	struct plugin_claims c;
	enum plugin_claims_seen seen;
	unsigned posture;
	int running;

	if (write == NULL)
		return;
	/* [!] ONE SECTION FOR THE CLAIMS AND THE LOADER'S STATE (issue #110's
	 * rule): asked later, a load landing between would print one container's
	 * manifest beside another's runtime state.  plugin_run_active() reads one
	 * word. */
	posture = nn_life_cs_enter();
	seen = plugin_claims_get(&nn_claims, nn_model_life.seq, &c);
	running = plugin_run_active();
	nn_life_cs_exit(posture);
	plugin_claims_info(write, ctx, seen, &c, running,
	                   (uint32_t)(uintptr_t)__plugin_start,
	                   (uint32_t)(__plugin_end - __plugin_start),
	                   nn_plugin_policy.veneer_cost);
}
#else
/* The other backends cannot take a container, so they have nothing to add.
 * Silence is a legal answer: these lines are the board's own subject. */
void nn_svc_info_extra(nn_svc_write_fn write, void *ctx)
{
	(void)write;
	(void)ctx;
}
#endif
