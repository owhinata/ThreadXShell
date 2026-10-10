/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_f746_model.c
 * @brief   This board's `nn info`, `nn model load` and `nn model unload`
 *          behind the shared order (issue #131 step 7d).
 *
 * The ORDER of a load and an unload is svc/nn_core_model.c's -- one copy for
 * every board -- and where a load ends is svc/nn_swap.c's table.  What is here
 * is only what this board is: where its claim is (the NN session), what it must
 * see before it (no stream, SDRAM up, the first open), how it reads a model off
 * the SD card, and how its backend replaces one model with another.
 *
 * WHY IT IS ITS OWN FILE.  The rest of the adapter (nn_svc_f746.c) needs the
 * camera, the stream machine and the decoder; this half needs only port/nn/nn.c,
 * so test/test_nn_model_life.c builds it against the real nn.c over a stub
 * backend and walks what a console cannot produce on demand -- a refusal that
 * restores nothing, an `nn info` from another console in the middle of a swap.
 *
 * Like the rest of the adapter it adds no second copy of the model: the
 * singleton and the session gate live in port/nn/nn.c.  The one piece of state
 * here is the shared order's transition counter, which nothing else had.
 */
#include "nn_svc.h"
#include "nn_svc_adapter.h" /* nn_detail_set, nn_result (#130) */
#include "nn_core_model.h"  /* the shared load / unload order (#131) */

#include <string.h>

#include "nn.h"
#include "nn_camera.h"
#include "sdram.h"
#include "tx_api.h"

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

/* ---- the backend ---------------------------------------------------------- */

/** One load's resolution, on the console's stack. */
struct nn_f746_job {
	nn_svc_read_fn read;
	void          *ctx;
	void          *buf;
	uint32_t       cap;
	uint32_t       len;
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

	if (j->spec->tag == NN_SPEC_BUILTIN)
		rc = nn_model_reload(NULL, 0u, NULL, model_after);
	else
		rc = nn_model_reload(f->buf, f->len, j->spec->path, model_after);
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

/* Every tag but the two this board loads from, refused before anything is
 * acquired. */
static void nn_life_spec_refused(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	const struct nn_spec *spec = j->spec;

	if (spec == NULL)
		return;
	nn_detail_set("this board loads a model from the SD card (--path) or "
	              "uses the one built into the image (builtin); it has no "
	              "%s",
	              spec->tag == NN_SPEC_NAME ? "asset store"
	              : spec->tag == NN_SPEC_SLOT ? "slot index"
	              : spec->tag == NN_SPEC_ADDR ? "raw model window"
	                                          : "such source");
}

static int nn_life_check_spec(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;

	if (j->spec->tag == NN_SPEC_BUILTIN && !nn_model_has_builtin()) {
		/* [!] NOT AN UNLOAD (issue #131 P16).  On the SD-only backend this
		 * word used to empty the model; `nn model unload` does that. */
		nn_detail_set("this backend loads a model from the SD card (--path); "
		              "it has no built-in model");
		return NN_SVC_ERR_SPEC;
	}
	return NN_SVC_OK;
}

/*
 * The preconditions, and then THE CLAIM ITSELF -- the NN session.
 *
 * [!] THE SESSION IS TAKEN HERE, AS ADMISSION'S LAST STEP, NOT IN claim_take.
 * The shared order's claim hook takes no result, so it cannot say WHY it was
 * refused, and this board words that ("a stream, run or bench is active").
 * Taken last, a refusal before it holds nothing; taken, the shared order's
 * claim_take is a formality that cannot fail, so nothing runs between this and
 * the claim the order reads had_open under.  claim_give gives it back.
 *
 * [!] THE FIRST OPEN AFTER BOOT HAPPENS HERE, BEFORE THE SESSION (issue #131
 * P16).  That open adopts the built-in model and does not return until it is
 * built -- if another console's `nn info` got there first, this waits for that
 * build -- so nothing under the session runs beside it, and every later open
 * builds nothing.  An unload opens first and asks about the stream after, as
 * before the order was shared.
 */
static int nn_life_admit(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_model *m = NULL;

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
		if (nn_model_open(&m) != 0 || m == NULL) {
			nn_detail_set("the model could not be opened");
			return NN_SVC_ERR_STATE;
		}
	}
	/*
	 * [!] THE SESSION IS CLAIMED BEFORE THE SLOT IS CHOSEN, so slot selection,
	 * the SD read and the swap are atomic against another console: otherwise
	 * a second shell could flip the chosen (inactive) slot to active between
	 * load_region() and the read, and the live flatbuffer would be corrupted.
	 */
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (a stream, run or bench is active)");
		return NN_SVC_ERR_BUSY;
	}
	return NN_SVC_OK;
}

/* Held since nn_life_admit(); see there. */
static int nn_life_claim_take(void)
{
	return 1;
}

/* What the read needs, asked before the SD card is touched. */
static int nn_life_prepare(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;

	if (j->spec->tag == NN_SPEC_BUILTIN)
		return NN_SVC_OK;
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

/* The file, into the staging slot the backend handed out.  This board carries
 * no plugin, so every model is bare. */
static int nn_life_fetch(struct nn_core_model_job *j)
{
	struct nn_op_result *res = j->res;
	struct nn_f746_job *f = j->board;

	j->bare = 1;
	if (j->spec->tag == NN_SPEC_BUILTIN)
		return NN_SVC_OK;
	/* The reader prints its own failure: it is the board's shell-layer file
	   and it has the console this command came in on. */
	if (f->read(f->ctx, j->spec->path, f->buf, f->cap, &f->len) != 0) {
		nn_detail_clear();
		return NN_SVC_ERR_ARG;
	}
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

/* The backend owns the model's identity (its name); there is none here to
 * keep or clear. */
static void nn_life_forget(void)
{
}

static void nn_life_commit(struct nn_core_model_job *j)
{
	(void)j;
}

/*
 * [!] NO PLUGIN, SO NO LEASE: lease_take is NULL and the shared order then
 * calls no plugin hook at all (nn_core_model.h).  The counter still brackets
 * the swap.  hw_down is NULL: what an EMPTY ending must bring down is the
 * backend's model, and the order releases that itself.
 */
static const struct nn_core_model_board nn_life_board = {
	.cs_enter      = nn_life_cs_enter,
	.cs_exit       = nn_life_cs_exit,
	.backend       = &nn_life_backend,
	.tags          = NN_CORE_MODEL_TAG(NN_SPEC_PATH) |
	                 NN_CORE_MODEL_TAG(NN_SPEC_BUILTIN),
	.spec_refused  = nn_life_spec_refused,
	.check_spec    = nn_life_check_spec,
	.admit         = nn_life_admit,
	.claim_take    = nn_life_claim_take,
	.claim_give    = nn_session_release,
	.prepare       = nn_life_prepare,
	.fetch         = nn_life_fetch,
	.lease_take    = NULL,
	.lease_give    = NULL,
	.plugin_start  = NULL,
	.plugin_unload = NULL,
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
	int held;

	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, bi ? bi->name : NULL);
	nn_svc_str(out->version, sizeof out->version, bi ? bi->version : NULL);

	/* [!] THE RESERVATION, NOT THE USE (issue #131 P3).  This line used to
	 * carry what the model's activations take, under the word "reserved"; that
	 * is `used` now, as on grove-vision-ai-v2.  A fixed fact of the build, so it
	 * needs no claim and stands even when the rest is busy. */
	out->arena_bytes = nn_arena_reserved();

	if (nn_model_open(&m) != 0 || m == NULL)
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
	 * (svc/nn_core_model.c).  A stream holds the session and never moves the
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
 * endings: svc/nn_core_model.h. */
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

/* Idempotent: unloading nothing succeeds (svc/nn_core_model.h). */
void nn_svc_model_unload(struct nn_op_result *res)
{
	struct nn_core_model_job j;

	nn_detail_clear();
	j.spec  = NULL;
	j.res   = res;
	j.board = NULL;
	nn_core_model_unload(&nn_model_life, &nn_life_board, &j);
}
