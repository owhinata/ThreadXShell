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
 * see before it (no stream, SDRAM up, the first open), how it reads a model out
 * of a QSPI asset slot into the staging buffer and checks it (issue #194; until
 * then a file on the SD card), and how its backend replaces one model with
 * another.  The reading is wio-lite-ai's (port/nn/nn_svc_wio.c), over this
 * board's store (src/blob.c).
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
#include "nn_svc_common.h"  /* nn info's model section (shared, #174) */

#include <string.h>

#include "blob.h"           /* the QSPI asset store (issue #194)       */
#include "crc32.h"          /* the CRC of the copy                      */
#include "nn.h"
#include "nn_camera.h"
#include "sdram.h"
#include "tx_api.h"

#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"
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
 * this policy ACTUALLY holds back out of shell.elf (cmake/check_policy_probe.py,
 * by the name plugin_board_policy -- the one object plugin_parse() reads, issue
 * #165), with the seven allowances.
 *
 * The slot -> allowance mapping is this board's (decode on the nn worker, draw
 * on the GUIX thread that draws the boxes, the rest on a console); the asserts
 * against each thread's stack are in nn_svc_f746.c and nn_camera.c.
 */
const struct plugin_policy plugin_board_policy = {
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
 * is this board's: the slot and the blob the claims came with, so a load landing
 * between `nn info`'s model line and its plugin line shows up as a mismatch.
 * (Until issue #194 it was the SD path, the length and the CRC the load
 * printed; the CRC is now the store's, in `blob list`.)
 */
/* "slot 4294967295, blob '" + a name + "'" must survive whole. */
_Static_assert(23u + BLOB_NAME_MAX + 1u < PLUGIN_CLAIMS_FROM_MAX,
               "a container's slot and blob name would be cut on `nn info`");

static struct plugin_claims_store nn_claims;

static void nn_claims_set(const struct plugin_claims *c)
{
	unsigned posture = nn_life_cs_enter();

	plugin_claims_set(&nn_claims, c);
	nn_life_cs_exit(posture);
}
#endif /* CONFIG_NN_BACKEND_TFLM */

/* ---- the backend ---------------------------------------------------------- */

/** One load's resolution, on the console's stack: nothing in it is the
 *  adapter's until commit(). */
struct nn_f746_job {
	struct blob_info info;
	unsigned         slot;   /**< where the name resolved, under the blob flag */
	void            *buf;
	uint32_t         cap;
	/* What the backend is handed: the whole payload, or -- for a container --
	 * its model section, in place inside the staging buffer (issue #159). */
	const void      *model_at;
	uint32_t         model_len;
#if defined(CONFIG_NN_BACKEND_TFLM)
	int                  is_container;
	struct plugin_claims claims;
#endif
};

/*
 * [!] THE BLOB FLAG OF A LOAD, HELD FROM prepare()'s NAME LOOKUP TO fetch()'s
 * PAYLOAD READ (issue #194, wio-lite-ai's since #191), so the slot the name
 * resolved to and the length and CRC checked before the open model went down
 * belong to the same generation of the slot as the bytes read after.  A `blob
 * write` / `blob erase` and `qspi erase` / `qspi test` are refused meanwhile.
 * fetch() gives it back as soon as the QSPI is done with; every other exit -- a
 * refusal in prepare(), a lease the order could not take, any failure in
 * between -- gives it back in claim_give().  Written and read only by the
 * holder of the NN session.  The flag is a try-acquire with interrupts masked,
 * never a wait, so holding it while the order waits for the plugin lease orders
 * no lock against another.
 */
static uint8_t nn_life_blob;

static void nn_life_blob_give(void)
{
	if (nn_life_blob) {
		nn_life_blob = 0u;
		blob_busy_release();
	}
}

/*
 * The backend's reload: nn_model_reload() adopts the new model, or -- if it is
 * refused -- restores the previous one (stedgeai_reloc, whose two slots keep
 * it apart) or leaves the backend EMPTY (tflm, issue #191: the shared order
 * took the previous model down before its one buffer was written).
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

	rc = nn_model_reload(f->model_at, f->model_len, f->info.name,
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

/*
 * [!] staging_is_open IS THE BACKEND'S ANSWER (issues #191, #200), 1 on tflm
 * only: its one staging buffer is where the open model lives, so the shared
 * order takes that model down before fetch() reads the payload over it.
 * stedgeai_reloc keeps its own two slots and its restore; null and stedgeai
 * stage nothing.  The fact is in the backend's table, not written here.
 */
static const struct nn_core_model_backend nn_life_backend = {
	.swap      = nn_life_swap,
	.release   = nn_life_release,
	.has_model = nn_model_loaded,
	.strerror  = NULL,     /* a refusal is worded by the swap itself */
	.reserved  = nn_arena_reserved,
	.used      = NULL,     /* `nn info` asks the open model */
	.staging_is_open = nn_model_staging_is_open,
};

/* ---- the board ------------------------------------------------------------ */

/* The name's own rules, before anything is acquired: a name the store could
 * never have stored is not looked for (the rules `blob write` applied). */
static int nn_life_check_spec(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	enum blob_name_verdict v = blob_name_check(j->spec->name, NULL);

	if (v != BLOB_NAME_OK) {
		nn_detail_set("'%s' is not a blob name (%s)", j->spec->name,
		              blob_name_verdict_name(v));
		return NN_SVC_ERR_ARG;
	}
	return NN_SVC_OK;
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
 * [!] THE SESSION IS CLAIMED BEFORE THE BUFFER IS ASKED FOR, so the answer,
 * the payload read and the swap are atomic against another console: otherwise a
 * second shell could open a model in the buffer handed out (tflm's one buffer,
 * or stedgeai_reloc's inactive slot) between load_region() and the read, and
 * the live flatbuffer would be corrupted.
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

/* The blob flag, if a load still holds it, then the session. */
static void nn_life_claim_give(void)
{
	nn_life_blob_give();
	nn_session_release();
}

/*
 * Everything a load can be refused for WITHOUT CHANGING ANYTHING, before the
 * shared order takes the open model down (issue #191): the backend's capacity,
 * and the name resolved to exactly one slot holding a valid blob that fits.  A
 * refusal here leaves the previous model as it was (the order answers
 * PREVIOUS) and gives back what it took.  On success the blob flag stays held
 * for fetch(); claim_give() returns it if fetch() never runs.
 *
 * [!] On tflm the buffer is NOT asked for here (issue #191): it is the open
 * model's until the shared order takes that model down, and the backend refuses
 * it until then, so the capacity is asked apart (nn_model_stage_capacity()) and
 * fetch() asks for the buffer.  The other backends hand out their region here,
 * as they always did -- null's NOSUP refusal stays where it was -- and
 * stedgeai_reloc's region is its inactive 448 KB slot, so a blob longer than
 * that is refused here, at load (issue #194 D4).
 */
static int nn_life_prepare(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;
	const struct nn_spec *spec = j->spec;
	int rc;

#if defined(CONFIG_NN_BACKEND_TFLM)
	rc = nn_model_stage_capacity(&f->cap);
#else
	rc = nn_model_load_region(&f->buf, &f->cap);
#endif
	if (rc != 0) {
		nn_detail_set("this backend has no runtime model loader");
		return NN_SVC_ERR_NOSUP;
	}

	/* Hold the blob flag from the name lookup to the payload read
	 * (nn_life_blob). */
	if (blob_busy_acquire() != BLOB_OK) {
		nn_detail_set("blob busy (" BLOB_BUSY_HOLDERS " is running)");
		return NN_SVC_ERR_BUSY;
	}
	nn_life_blob = 1u;
	{
		enum blob_lookup why = BLOB_LOOKUP_REFUSE;

		rc = blob_find(spec->name, &f->slot, &f->info, &why);
		if (rc == BLOB_ERR_EMPTY || rc == BLOB_ERR_DUPLICATE) {
			/* Two answers, two sentences: "no slot holds that name"
			 * and "more than one slot holds that name" ask for
			 * different things from the operator. */
			nn_detail_set("'%s': %s (`blob list` shows what is where)",
			              spec->name, blob_lookup_name(why));
			nn_life_blob_give();
			return NN_SVC_ERR_ARG;
		}
		if (rc != BLOB_OK) {
			nn_detail_set("cannot read the blob headers (%d, see `qspi "
			              "info` and `blob list`)", rc);
			nn_life_blob_give();
			return NN_SVC_ERR_HW;
		}
	}
	/* blob_find() answers FOUND only for a VALID slot; asked again here so
	 * that what this function relies on is stated where it relies on it. */
	if (f->info.state != BLOB_VALID) {
		nn_detail_set("slot %u holds no valid blob -- `blob list`", f->slot);
		nn_life_blob_give();
		return NN_SVC_ERR_ARG;
	}
	if (f->info.length == 0u || f->info.length > f->cap) {
		nn_detail_set("slot %lu is %lu B, staging holds %lu B",
		              (unsigned long)f->slot,
		              (unsigned long)f->info.length, (unsigned long)f->cap);
		nn_life_blob_give();
		return NN_SVC_ERR_ARG;
	}
	return NN_SVC_OK;
}

/*
 * The bytes: into the staging buffer, the CRC over the copy, the container
 * split (tflm, issue #159).  On a tflm build the open model is already down
 * (issue #191), so a failure here ends with no model; the operator loads the
 * previous one again by its name.
 *
 * A payload that is not a container is a bare model.  A container goes through
 * svc/plugin/plugin_load.c -- the same validator the host asset build runs --
 * with this board's policy, and its MODEL SECTION is handed to the backend IN
 * PLACE, at its offset inside the staged copy (nn.h, NN_MODEL_ALIGN): copying
 * it down to the buffer's start would overwrite the validated container with
 * bytes nobody validated.
 */
static int nn_life_fetch(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;
	uint32_t crc;
	int rc;

	j->bare = 1;
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* [!] ONLY NOW, WITH NO MODEL OPEN, IS THE BUFFER HANDED OUT (issue
	 * #191): the backend refuses it while a model is open, which stops a load
	 * that skipped the take-down before a byte is written. */
	if (nn_model_load_region(&f->buf, &f->cap) != 0) {
		nn_detail_set("the model buffer was not handed out");
		return NN_SVC_ERR_HW;
	}
	if (f->info.length > f->cap) {
		/* prepare() checked against the same object; a different answer
		 * now is the backend's fault, not the slot's. */
		nn_detail_set("the staging capacity changed (%lu B)",
		              (unsigned long)f->cap);
		return NN_SVC_ERR_HW;
	}
#endif

	/* Under the flag, so the read does not look at it (src/blob.h). */
	rc = blob_read_reserved(f->slot, 0u, f->buf, f->info.length);
	nn_life_blob_give();          /* the QSPI is done with; the rest is SDRAM */
	if (rc != BLOB_OK) {
		nn_detail_set("QSPI read failed (%d)", rc);
		return NN_SVC_ERR_HW;
	}

	/*
	 * [!] CRC THE COPY, NOT THE FLASH.  `blob verify` re-reads the QSPI and
	 * compares against the stored value, which says nothing about the bytes
	 * that are about to be interpreted: a fault between the QSPI and this
	 * buffer -- driver, bus, or the SDRAM itself -- would pass that check and
	 * still hand a corrupt flatbuffer to the runtime.
	 */
	crc = crc32_update(0u, f->buf, f->info.length);
	if (crc != f->info.crc32) {
		nn_detail_set("CRC32 mismatch -- stored %08lX, in memory %08lX; the "
		              "blob is intact on the QSPI only if `blob verify %lu` "
		              "passes, the copy is not",
		              (unsigned long)f->info.crc32, (unsigned long)crc,
		              (unsigned long)f->slot);
		return NN_SVC_ERR_HW;
	}

	f->model_at  = f->buf;
	f->model_len = f->info.length;
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* [!] THE SPLIT IS DONE ON THE COPY THE CRC WAS JUST CHECKED AGAINST:
	 * deciding what the bytes mean on a second read would decide it about
	 * bytes nobody checked. */
	if (plugin_probe(f->buf, f->info.length) == PLUGIN_KIND_CONTAINER) {
		enum plugin_result pr;

		pr = plugin_parse(f->buf, f->info.length, &f->claims.view);
		if (pr != PLUGIN_OK) {
			nn_detail_set("slot %lu is a container this firmware refuses: %s",
			              (unsigned long)f->slot, plugin_result_name(pr));
			return NN_SVC_ERR_ARG;
		}
		f->is_container = 1;
		j->bare         = 0;
		{
			char name[BLOB_NAME_MAX + 1u];

			(void)memcpy(name, f->info.name, sizeof name);
			name[sizeof name - 1u] = '\0';
			nn_detail_to(f->claims.from, sizeof f->claims.from,
			             "slot %lu, blob '%s'", (unsigned long)f->slot,
			             name);
		}
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
 * give what was asked for.  plugin_run_start() also logs its own reason.
 *
 * [!] THE STAGING REGION IS PASSED, NOT LOOKED UP: the buffer fetch() filled
 * (f->buf, f->cap) is the region the container lies in, and the backend's
 * load_region() would now refuse -- its one buffer is the open model's (issue
 * #191; with two slots it answered with the other one).
 */
static int nn_life_plugin_start(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;
	enum plugin_run_result pr;

	/* entry()'s depth is sampled in the loader's exec_ok hook. */
	pr = plugin_run_start(&f->claims.view, f->buf, f->buf, f->cap);
	if (pr == PLUGIN_RUN_OK || pr == PLUGIN_RUN_NO_PLUGIN)
		return NN_SVC_OK;
	nn_detail_set("slot %lu: %s", (unsigned long)f->slot,
	              plugin_run_why(pr));
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
	.check_spec    = nn_life_check_spec,
	.admit         = nn_life_admit,
	.claim_take    = nn_life_claim_take,
	.claim_give    = nn_life_claim_give,
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

/* The body is svc/nn/nn_svc_common.c's, shared with wio-lite-ai (issue #174);
 * what is this board's is how it reads the order's counter. */
void nn_svc_info(struct nn_svc_info *out)
{
	nn_svc_common_info(out, nn_life_seq);
}

/* Whether a copy still describes what is open (issue #199): the same read. */
int nn_svc_info_stands(const struct nn_svc_info *info)
{
	return nn_svc_common_info_stands(info, nn_life_seq);
}

/* ---- model lifecycle ----------------------------------------------------- */

/* Point the model at @p spec -- over an open model too.  The order and its
 * endings: svc/nn/nn_core_model.h. */
void nn_svc_model_load(const struct nn_spec *spec, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	struct nn_f746_job f;
	struct nn_core_model_job j;

	nn_detail_clear();
	memset(&f, 0, sizeof f);
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
 * `crc` on the plugin line is the plugin-section digest the container carries
 * (it moves when the plugin's bytes do, not when the model does).  The CRC of
 * the WHOLE container -- what the asset receipt names -- is the store's, in
 * `blob list` (issue #194; until then the from-line carried it).
 *
 * [!] NOTHING IS HELD WHILE THIS PRINTS.  The claims and the transition counter
 * are copied out in one masked section and written afterwards, by
 * svc/plugin/plugin_claims.c (issue #159 Step 4b).
 */
#if defined(CONFIG_NN_BACKEND_TFLM)
extern uint8_t __plugin_start[], __plugin_end[];   /* the linker's reservation */

void nn_svc_info_extra(const struct nn_svc_info *info, nn_svc_write_fn write,
                       void *ctx)
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
	/* checked against the value `nn info` printed from (issue #199) */
	seen = plugin_claims_get(&nn_claims, nn_model_life.seq, info, &c);
	running = plugin_run_active();
	nn_life_cs_exit(posture);
	plugin_claims_info(write, ctx, seen, &c, running,
	                   (uint32_t)(uintptr_t)__plugin_start,
	                   (uint32_t)(__plugin_end - __plugin_start),
	                   plugin_board_policy.veneer_cost);
}
#else
/* The other backends cannot take a container, so they have nothing to add.
 * Silence is a legal answer: these lines are the board's own subject. */
void nn_svc_info_extra(const struct nn_svc_info *info, nn_svc_write_fn write,
                       void *ctx)
{
	(void)info;
	(void)write;
	(void)ctx;
}
#endif
