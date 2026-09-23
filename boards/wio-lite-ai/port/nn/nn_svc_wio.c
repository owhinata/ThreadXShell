/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_wio.c
 * @brief   This board behind the shared `nn` command's contract (issue #50).
 *
 * svc/nn_svc.h is what shell/cmds/cmd_nn.c speaks; port/nn is what this board
 * has.  Like the f746 adapter and unlike the Grove one, this introduces no state
 * about the model: the singleton and the session gate live in port/nn/nn.c and
 * the worker's release authority lives in src/nn_camera.c, so this queries them.
 *
 * [!] THE CLAIM IS A PAIR HERE, AND THE ORDER IS NOT ARBITRARY.  Anything that
 * touches tensors or the arena takes the NN session (the software claim) and
 * THEN the OCTOSPI1/PSRAM guard (the hardware one).  Taking the hardware first
 * would mean holding a peripheral just to report that software was busy.  Both
 * stay inside this file, so a failure to take the second unwinds the first here
 * and the caller is told it holds NOTHING -- releasing what has already been
 * rolled back would free a claim that by then belongs to somebody else.
 *
 * [!] AND A STOP THAT DID NOT FINISH IS NOT A FAILURE TO SWALLOW.  See
 * nn_claim_of_stop(): this is the board whose one-shot used to discard it.
 */
#include "nn_svc.h"
#include "nn_report.h"

#include <stdarg.h>
#include <string.h>

#include <flashdb.h>       /* fdb_calc_crc32 -- see the note in the load path */

#include "blob.h"
#include "camera.h"
#include "cam_band.h"
#include "cam_preview.h"     /* CAM_PREVIEW_STACK_BYTES -- the draw allowance's ceiling */
#include "cli_config.h"      /* CLI_INSTANCE/BG_JOB_STACK_SIZE -- report's ceiling */
#include "fmt.h"
#include "plugin_info.h"
#include "nn.h"
#include "nn_camera.h"
#include "nn_desc.h"
#include "nn_active.h"
#include "plugin_load.h"
#include "plugin_lease.h"
#include "plugin_run.h"
#include "plugin_target.h"   /* the target word this build provides (#108) */
#include "psram.h"
#include "stm32h7xx_hal.h"   /* SystemCoreClock -- the DWT counter's clock */
#include "tx_api.h"

/*
 * [!] THE PLUGIN TARGET WORD IS CHECKED HERE, against this firmware's own build
 * (issue #108).  board.cmake hands one value to the packer, the host container
 * verifier and this firmware, so their agreeing proves nothing about the value.
 * This derives the word from the compiler's predefined macros instead -- and it
 * is the only check of the CMSE bit, which no plugin image records (this part
 * has no Security Extension, so the bit must be clear).
 */
#ifndef WIO_PLUGIN_TARGET_ID
#error "board.cmake must define WIO_PLUGIN_TARGET_ID"
#endif
_Static_assert(WIO_PLUGIN_TARGET_ID == PLUGIN_TARGET_ID_HERE,
               "WIO_PLUGIN_TARGET_ID does not describe this firmware's build "
               "(svc/plugin_target.h)");

/*
 * How long a console waits for the result lease before giving up (issue #110).
 * Finite, and with an answer on the other side of it: measuring how long a wait
 * took is not the same as bounding it, and a shell that stopped responding
 * behind a wedged worker with no line of output would be worse than a refusal.
 */
#define NN_PLUGIN_LEASE_WAIT_TICKS  50u

/* ---- plugin containers (issue #108 = #78 Step 3a) ------------------------- */

/*
 * This board's policy for a plugin image.  svc/plugin_load.c holds no board
 * address, so the reservation, the target identity and what each thread can
 * spare arrive from here.
 *
 * [!] EVERY NUMBER COMES FROM THE BUILD, NOT FROM HERE.  The host asset build
 * runs the same validator (verify_container links the very plugin_load.c this
 * board runs) with the same numbers, and if the two were declared separately a
 * container could pass on the host and be refused on the board -- the shape
 * issue #93 hit.  board.cmake defines them once and passes them both ways.  That
 * is not the issue #85 hazard: there, a layout and the gate CHECKING it came from
 * one variable.  Here the two consumers must AGREE; the reservation's address is
 * still verified independently, by the linker script and
 * cmake/check_plugin_reservation.py, which state it separately on purpose.
 *
 * [!] 3a NEVER CALLS THROUGH ANY OF THIS.  A container is validated and its
 * claims recorded; the plugin section is never copied and never branched into.
 */
#if defined(CONFIG_NN_BACKEND_TFLM)
#if !defined(WIO_PLUGIN_BASE) || !defined(WIO_PLUGIN_MAX) ||                  \
    !defined(WIO_PLUGIN_STACK_NN_WORK) || !defined(WIO_PLUGIN_STACK_PREVIEW) || \
    !defined(WIO_PLUGIN_STACK_SHELL)
#error "board.cmake must define the WIO_PLUGIN_* policy for a tflm build"
#endif

/*
 * [!] THE STACK LIMITS ARE PROVISIONAL, AND EACH IS STRICTLY BELOW ITS THREAD.
 * The honest allowance is "thread stack - depth already spent at the call site -
 * the asynchronous reserve - margin", and on this board the second term is what
 * Step 3a exists to MEASURE (`nn stream stats` prints it).  So these are
 * placeholders until 3b derives them -- but Grove's issue #103 found two of its
 * placeholders equal to the WHOLE thread stack, where a declaration of that size
 * would have been admitted and overflowed.  A limit that cannot be exceeded is not
 * a limit, so each one is pinned below its ceiling here, where both are visible.
 */
_Static_assert(WIO_PLUGIN_STACK_NN_WORK < NNCAM_STACK_BYTES,
               "decode's provisional allowance must be below nn_work's stack");
_Static_assert(WIO_PLUGIN_STACK_PREVIEW < CAM_PREVIEW_STACK_BYTES,
               "draw's provisional allowance must be below the preview stack");
_Static_assert(WIO_PLUGIN_STACK_SHELL < CLI_INSTANCE_STACK_SIZE &&
               WIO_PLUGIN_STACK_SHELL < CLI_BG_JOB_STACK_SIZE,
               "report's provisional allowance must be below a shell stack");
/*
 * The container's model section starts on PLUGIN_MODEL_ALIGN; the backend adopts
 * it in place and needs NN_MODEL_ALIGN.  Stated as MET, not assumed -- the Grove
 * lesson is that an alignment an old placement over-satisfied is invisible until
 * something places differently (svc/plugin_abi.h, PLUGIN_MODEL_ALIGN).
 */
_Static_assert(PLUGIN_MODEL_ALIGN % NN_MODEL_ALIGN == 0u,
               "a container's model section must meet the backend's alignment");

/*
 * [!] THE VENEER COST IS NOT THIS BOARD'S VARIABLE HERE (issue #111).  The loader
 * adds it to what a manifest declares, so it must be the number the build checked
 * against shell.elf -- and cmake/veneer_cost_gate.cmake defines it on this very
 * compile from that check's own DECLARED.  board.cmake does not pass it.
 */
#ifndef PLUGIN_VENEER_BASE_COST
#error "cmake/veneer_cost_gate.cmake defines PLUGIN_VENEER_BASE_COST; is veneer_cost_gate() registered?"
#endif

static const struct plugin_policy nn_plugin_policy = {
	.target_id      = WIO_PLUGIN_TARGET_ID,
	.link_addr      = WIO_PLUGIN_BASE,
	.capacity       = WIO_PLUGIN_MAX,
	.image_align    = PLUGIN_IMAGE_ALIGN,
	.caps_supported = PLUGIN_CAP_KNOWN_MASK,
	.stack_limit    = {
		[PLUGIN_SLOT_ENTRY]     = WIO_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_SHAPES_OK] = WIO_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_DECODE]    = WIO_PLUGIN_STACK_NN_WORK,
		[PLUGIN_SLOT_DRAW]      = WIO_PLUGIN_STACK_PREVIEW,
		[PLUGIN_SLOT_REPORT]    = WIO_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_PARAM_SET] = WIO_PLUGIN_STACK_SHELL,
		[PLUGIN_SLOT_PARAM_GET] = WIO_PLUGIN_STACK_SHELL,
	},
	.veneer_cost    = PLUGIN_VENEER_BASE_COST,     /* veneer_cost_gate() */
	.stack_accounting = PLUGIN_STACK_ACCOUNTING,
};
/* What the build reads back from shell.elf: the c and accounting this policy
 * really holds, whatever -D reached this compile (cmake/check_policy_probe.py,
 * issue #111). */
PLUGIN_POLICY_PROBE(nn_plugin_policy);
#endif /* CONFIG_NN_BACKEND_TFLM */

/*
 * What the container of the OPEN model claimed, or nothing.
 *
 * [!] THE CLAIMS FOLLOW THE MODEL THAT IS ACTUALLY OPEN.  They are settled only
 * after nn_model_reload() has adopted (or refused) that model -- never before,
 * because a reload can refuse and leave the PREVIOUS model active, and then these
 * must still be the previous model's.  A successful bare-model load clears them,
 * as do an unload and a reload that left nothing open.
 *
 * [!] AND THERE IS A WINDOW, SO IT IS NAMED RATHER THAN HIDDEN (the #108
 * adversarial review).  The backend makes the new model visible inside
 * nn_model_reload(); the claims are settled after it returns.  `nn info` does not
 * take the NN session -- it has to answer while a stream holds it -- so a console
 * that preempts a background `nn model load` between those two points would read
 * the new model beside the old claims.  `loading` is raised before the reload and
 * dropped in the same critical section that settles the claims, and a reader
 * that sees it says "a load is in progress" instead of reporting either set.
 * The claims also carry the slot and blob name they were read from: `nn info`
 * prints the model and the plugin lines from two separate calls, and naming the
 * model on the plugin line is what makes a load landing BETWEEN those calls
 * visible to whoever reads them, rather than a silent mismatch.
 *
 * PLAIN DATA: struct plugin_view carries integer offsets and copied bytes, no
 * callable pointer -- "3a does not execute a plugin" is a property of the type.
 *
 * Written by `nn model load` / `nn model unload` (which hold the NN session);
 * every read and write copies inside one interrupt-masked section, a couple of
 * hundred bytes, so a reader never sees half of one container's claims.
 */
struct nn_claims {
	struct plugin_view view;
	uint32_t           slot;                     /* the blob it came from */
	char               model[BLOB_NAME_MAX + 1u];
};

enum nn_claims_seen {
	NN_CLAIMS_NONE    = 0,   /* the open model is not a container's    */
	NN_CLAIMS_VALID   = 1,   /* *out describes the open model           */
	NN_CLAIMS_LOADING = 2,   /* a load/unload is between its two steps  */
};

static struct nn_claims nn_claims;
static uint8_t          nn_claims_valid;
static uint8_t          nn_claims_loading;

/* Raised immediately before nn_model_reload(); every path that raises it
 * settles it (nn_claims_settle) before giving the session back. */
static void nn_claims_begin(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nn_claims_loading = 1u;
	TX_RESTORE
}

/*
 * @p keep non-zero leaves the previous claims standing (a refused reload that
 * restored the previous model); otherwise @p c replaces them, or NULL clears.
 */
static void nn_claims_settle(int keep, const struct nn_claims *c)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (!keep) {
		if (c != NULL) {
			nn_claims       = *c;
			nn_claims_valid = 1u;
		} else {
			nn_claims_valid = 0u;
		}
	}
	nn_claims_loading = 0u;
	TX_RESTORE
}

/*
 * @param running  optional; whether a plugin is LOADED AND STARTED, taken in
 *                 the same masked section as the claims.
 *
 * [!] ONE SECTION FOR BOTH (issue #110).  Asking the loader afterwards would
 * be a second question at a later moment, and a load landing between the two
 * would print one container's manifest beside another's runtime state -- or
 * beside no plugin at all.  plugin_run_active() reads one word, so taking it
 * here costs nothing.
 */
static enum nn_claims_seen nn_claims_snapshot(struct nn_claims *out,
                                              int *running)
{
	enum nn_claims_seen seen;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (nn_claims_loading) {
		seen = NN_CLAIMS_LOADING;
	} else if (nn_claims_valid) {
		*out = nn_claims;
		seen = NN_CLAIMS_VALID;
	} else {
		seen = NN_CLAIMS_NONE;
	}
	if (running != NULL)
		*running = plugin_run_active();
	TX_RESTORE
	return seen;
}

/*
 * [!] THERE IS NO SHARED DIAGNOSTIC BUFFER, and that is the fix for a hazard the
 * first version of this file had.  A port adapter cannot print -- it holds no
 * shell instance -- so it writes WHY something failed, and the shared command
 * prints that.  Keeping those words in one static here meant two consoles
 * building results at once would overwrite each other's explanation: the second
 * console's sentence would appear under the first console's command.  Writing
 * straight into the caller's result removes the sharing instead of locking it,
 * so the words a command prints are the words that command produced.
 */

static void nn_detail_to(char *dst, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)fmt_vsnformat(dst, cap, fmt, ap);
	va_end(ap);
}

/* [!] A legal asset name must survive `nn info`'s copy whole (issue #122 P10):
 * a truncated one can read as a different, equally legal, name. */
_Static_assert(NN_SVC_MODEL_MAX >= BLOB_NAME_MAX,
               "nn info would truncate this board's longest asset name");

/* Every failure path writes into the result it is about to return. */
/* [!] Its format is checked against NN_SVC_DETAIL_MAX at build time (issue
 * #122 P15): the copy truncates, and a truncated sentence does not look it. */
#define nn_detail_set(...)                                                  \
	((void)NN_SVC_DETAIL_CHECK_FMT(__VA_ARGS__),                        \
	 nn_detail_to(res->detail, sizeof res->detail, __VA_ARGS__))
#define nn_detail_clear()  (res->detail[0] = '\0')

/* [!] The detail is COPIED into the caller's result here, at the one place a
 * result is built.  nn_detail is this file's buffer and the next command on
 * another console overwrites it -- so a pointer to it would be printed after it
 * had already become somebody else's sentence. */
static void nn_result(struct nn_op_result *res, int status, enum nn_claim claim)
{
	res->status = status;
	res->claim  = (uint8_t)claim;
}

/*
 * This board's stop codes, and nothing else (issue #99).
 *
 * [!] EXHAUSTIVE, WITH THE DEFAULT ELSEWHERE.  This used to end in a catch-all
 * "retryable", which quietly promised that ANY unrecognised return could be
 * settled by asking again -- a value carrying no evidence at all about whether
 * the worker or its guards are quiescent.  nn_stream_disp_of() supplies the
 * fail-closed default now, so adding a code to the worker without adding it here
 * reports terminal rather than looping an operator for ever.
 *
 * nn_camera_stop() returns exactly these three.
 */
static const struct nn_stream_disp nn_stop_disp[] = {
	{ NNCAM_OK,           NN_STREAM_CLAIM_NONE      },
	{ NNCAM_ERR_NOTRUN,   NN_STREAM_CLAIM_NONE      },
	/* Still tearing down: a band callback or an inference has not returned.
	   The release is idempotent and repeating the stop is what settles it. */
	{ NNCAM_ERR_TEARING,  NN_STREAM_CLAIM_RETRYABLE },
};

static enum nn_claim nn_claim_of_stop(int stop_rc)
{
	return (enum nn_claim)nn_stream_disp_of(stop_rc, nn_stop_disp,
	                                        (unsigned)(sizeof nn_stop_disp /
	                                                   sizeof nn_stop_disp[0]));
}

/* ---- the claim pair ------------------------------------------------------ */

/*
 * Software claim first, hardware claim second.
 *
 * @return NN_SVC_OK, or a status with NOTHING held -- every failure unwinds what
 *         it took, so the caller's disposition is always NN_CLAIM_NONE.
 */
static int nn_guards_take(struct nn_op_result *res)
{
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (another nn command is running)");
		return NN_SVC_ERR_BUSY;
	}
	if (!psram_ready()) {
		nn_detail_set("PSRAM is not ready -- the arena and the model live "
		              "there (see `psram info`)");
		nn_session_release();
		return NN_SVC_ERR_STATE;
	}
	if (!psram_acquire_shared()) {
		nn_detail_set("OCTOSPI1 busy (a psram/membench/wifi flash command holds "
		              "it, or `nn stream` is running)");
		nn_session_release();
		return NN_SVC_ERR_BUSY;
	}
	return NN_SVC_OK;
}

static void nn_guards_give(void)
{
	psram_release();
	nn_session_release();
}

/* ---- info ---------------------------------------------------------------- */

void nn_svc_info(struct nn_svc_info *out)
{
	const struct nn_backend_info *bi = nn_backend();
	struct nn_model *m = NULL;
	int held;

	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, bi ? bi->name : NULL);
	nn_svc_str(out->version, sizeof out->version, bi ? bi->version : NULL);

	if (nn_model_open(&m) != 0 || m == NULL)
		return;

	/* Copied, not borrowed: the backend owns this name and a reload replaces
	   it, so the caller must not hold a pointer into it while printing. */
	out->model_active = 1u;
	out->arena_used  = 0u;   /* this backend reports only the reservation */
	/*
	 * [!] THE NAME IS COPIED UNDER THE SESSION WHEN THE SESSION IS FREE, and
	 * copied anyway when it is not.  That is a deliberate middle, not an
	 * oversight:
	 *
	 *   Taking the session unconditionally would make `nn info` REFUSE while a
	 *   stream runs -- the worker holds it for the whole stream -- and that is
	 *   exactly when the report is worth asking for.  Both boards' previous
	 *   `nn info` read this name with no claim at all for that reason.
	 *
	 *   Not taking it at all leaves the copy able to race a reload, which
	 *   rewrites the backend's name buffer byte by byte, and produce a torn
	 *   name.  Cosmetic and self-correcting, but avoidable.
	 *
	 * So: try.  A reload cannot be in flight while the session is free, so the
	 * common case is now provably clean; when it is held the behaviour is what
	 * it always was, and no diagnostic is lost.
	 */
	held = (nn_session_try_acquire() == 0);
	nn_svc_str(out->model, sizeof out->model, nn_model_name(m));
	out->arena_bytes = nn_activations_bytes(m);
	if (held)
		nn_session_release();

	/* Every section is answered here, streaming or not -- which is the point of
	   the copy above (issue #99 made this explicit rather than implied). */
	out->avail_identity = (uint8_t)NN_AVAIL_OK;
	out->avail_runtime  = (uint8_t)NN_AVAIL_OK;
	out->avail_tensors  = (uint8_t)NN_AVAIL_OK;


	/* [!] Say plainly when nothing is being inferred, so a latency from
	 * `nn bench` is never mistaken for a model's. */
	if (bi && strcmp(bi->name, "null") == 0)
		nn_svc_str(out->source, sizeof out->source,
		           "synthetic workload, not inference");
}

/* ---- model lifecycle ----------------------------------------------------- */

void nn_svc_model_load(const struct nn_spec *spec, nn_svc_read_fn read,
                       void *ctx, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	struct blob_info info;
	struct nn_claims claims;
	struct nn_model *m = NULL;
	void     *stage = NULL;
	const void *model_at;
	uint32_t  cap = 0u, crc, model_len;
	int is_container = 0;
	int open_after = 0;
	int rc;

	/* This board reads its model out of the NOR asset store itself; it never
	   needs a filesystem reader. */
	(void)read;
	(void)ctx;

	nn_detail_clear();
	*state = (nn_model_open(&m) == 0 && m != NULL) ? NN_MODEL_PREVIOUS
	                                              : NN_MODEL_EMPTY;

	if (spec->tag != NN_SPEC_SLOT) {
		nn_detail_set("this board loads a model from a NOR asset slot "
		              "(--slot); it has no %s",
		              spec->tag == NN_SPEC_NAME ? "lookup by name"
		              : spec->tag == NN_SPEC_PATH ? "filesystem"
		              : spec->tag == NN_SPEC_ADDR ? "raw model window"
		              : spec->tag == NN_SPEC_BUILTIN ? "built-in model"
		                                             : "such source");
		nn_result(res, NN_SVC_ERR_SPEC, NN_CLAIM_NONE);
		return;
	}
	if (spec->slot >= BLOB_SLOT_COUNT) {
		nn_detail_set("slot must be 0 .. %u (see `blob list`)",
		              (unsigned)BLOB_SLOT_COUNT - 1u);
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] THE SESSION IS TAKEN BEFORE load_region(), NOT AFTER.  Which staging
	 * slot is "the inactive one" is a function of backend state, so a slot
	 * number handed out before the claim can be stale by the time it is used:
	 * two consoles both ask, both are told slot 1, the first wins the session
	 * and makes slot 1 ACTIVE, and the second then writes its download straight
	 * over the flatbuffer the live interpreter is reading.
	 *
	 * The OCTOSPI1 guard is NOT taken yet: everything up to the read is backend
	 * state or NOR traffic, and holding the PSRAM across a header decode buys
	 * nothing while refusing an `lcd on` for the duration.
	 */
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (another nn command is running)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	/* Ask the backend for staging BEFORE touching the NOR, so a backend that
	   cannot swap models costs no flash traffic to find out about -- and it is
	   the only honest way to learn the capacity. */
	rc = nn_model_load_region(&stage, &cap);
	if (rc != 0) {
		nn_detail_set("%s", nn_model_strerror(rc));
		nn_session_release();
		nn_result(res, NN_SVC_ERR_NOSUP, NN_CLAIM_NONE);
		return;
	}

	/*
	 * Hold the blob mutation lock across the header decode AND the payload
	 * read, so the length and CRC validated against belong to the same
	 * generation of the slot as the bytes read.  The CRC below would catch a
	 * mid-sequence `blob erase` anyway -- this turns a mysterious "CRC32
	 * mismatch" into an accurate "blob busy".
	 */
	if (blob_busy_acquire() != BLOB_OK) {
		nn_detail_set("blob busy (a blob write or erase is running)");
		nn_session_release();
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (blob_stat(spec->slot, &info) != BLOB_OK) {
		nn_detail_set("cannot read slot %lu's header (see `nor info`)",
		              (unsigned long)spec->slot);
		blob_busy_release();
		nn_session_release();
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		return;
	}
	if (info.state != BLOB_VALID) {
		nn_detail_set("slot %lu holds no valid blob -- `blob list`",
		              (unsigned long)spec->slot);
		blob_busy_release();
		nn_session_release();
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}
	if (info.length == 0u || info.length > cap) {
		nn_detail_set("slot %lu is %lu B, staging holds %lu B",
		              (unsigned long)spec->slot, (unsigned long)info.length,
		              (unsigned long)cap);
		blob_busy_release();
		nn_session_release();
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}

	/* From here the PSRAM is written and then interpreted, so take the hardware
	   guard too -- software claim first, as nn_guards_take() explains. */
	if (!psram_ready() || !psram_acquire_shared()) {
		nn_detail_set("OCTOSPI1 busy or PSRAM not ready");
		blob_busy_release();
		nn_session_release();
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	rc = blob_read(spec->slot, 0u, stage, info.length);
	blob_busy_release();          /* the NOR is done with; the rest is PSRAM */
	if (rc != BLOB_OK) {
		nn_detail_set("NOR read failed (%d) -- the previous model is "
		              "untouched", rc);
		nn_guards_give();
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] CRC THE COPY IN PSRAM, NOT THE FLASH.  `blob verify` re-reads the NOR
	 * and compares against the stored value, which says nothing about the bytes
	 * that are about to be interpreted: a fault anywhere between the NOR and
	 * this buffer -- driver, bus, or the PSRAM itself -- would pass that check
	 * and still hand a corrupt flatbuffer to the runtime.
	 *
	 * fdb_calc_crc32() inverts at entry and exit itself, so this IS standard
	 * CRC-32/ISO-HDLC and wrapping it would double-invert.
	 */
	crc = fdb_calc_crc32(0u, stage, info.length);
	if (crc != info.crc32) {
		nn_detail_set("CRC32 mismatch -- stored %08lX, in memory %08lX; the "
		              "blob is intact on the NOR only if `blob verify %lu` "
		              "passes, the copy is not",
		              (unsigned long)info.crc32, (unsigned long)crc,
		              (unsigned long)spec->slot);
		nn_guards_give();
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] THE CONTAINER SPLIT HAPPENS HERE, ON THE COPY THE CRC WAS JUST CHECKED
	 * AGAINST (issue #108).  Everything above established that these bytes are the
	 * bytes that were stored; deciding what they MEAN has to be done on those same
	 * bytes, not on a second read.  A payload that is not a container is a bare
	 * model and takes the path it always took -- the models already in the store
	 * were sent before containers existed and must not need re-sending.
	 *
	 * A container's MODEL section is handed to the backend IN PLACE, at its offset
	 * inside the staged container: the backend adopts any 16-byte-aligned range
	 * inside the slot it handed out (nn.h, NN_MODEL_ALIGN).  Copying it down to the
	 * slot's start would overwrite the checked container with unchecked bytes.
	 *
	 * [!] SINCE ISSUE #110 THE PLUGIN SECTION IS ACTUALLY RUN.  Step 3a validated
	 * and recorded it and stopped there; 3b copies it into the reservation and
	 * branches into it, and 3c (issue #116) took the firmware's own decoder away
	 * -- so a container's plugin is now the ONLY thing that reads a model's
	 * outputs, and a bare model's are reported as the tensors they are.
	 */
	model_at  = stage;
	model_len = info.length;
#if defined(CONFIG_NN_BACKEND_TFLM)
	if (plugin_probe(stage, info.length) == PLUGIN_KIND_CONTAINER) {
		enum plugin_result pr;

		pr = plugin_parse(stage, info.length, &nn_plugin_policy,
		                  &claims.view);
		if (pr != PLUGIN_OK) {
			nn_detail_set("slot %lu is a container this firmware refuses: %s "
			              "-- the previous model is untouched",
			              (unsigned long)spec->slot, plugin_result_name(pr));
			nn_guards_give();
			nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
			return;
		}
		is_container = 1;
		claims.slot = spec->slot;
		(void)memcpy(claims.model, info.name, sizeof claims.model);
		claims.model[sizeof claims.model - 1u] = '\0';
		model_at  = (const uint8_t *)stage + claims.view.model_off;
		model_len = claims.view.model_len;
	}
#endif

#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE LEASE IS TAKEN BEFORE ANYTHING CHANGES, AND ITS FAILURE IS AN
	 * ANSWER (issue #110).
	 *
	 * The NN session keeps the WORKER out -- a stream holds it for its
	 * lifetime, a one-shot for its duration -- but it does not keep another
	 * CONSOLE's plugin callback out: `nn thresh` takes no session.  Without
	 * this, a background job could be inside a plugin's param_set while this
	 * overwrote the reservation under it, and unpublishing a slot table does
	 * not revoke a pointer somebody already holds.
	 *
	 * The first version took it just before the replacement and DISCARDED the
	 * result, which is the same as not taking it: after the timeout the load
	 * proceeded anyway.  Priority inheritance schedules the holder; it does
	 * not promise the holder finishes.  Taken here, before nn_claims_begin()
	 * and before the backend is touched, a timeout costs nothing -- the model
	 * and the plugin are both exactly as they were.  Refusing a load because a
	 * console is mid-`nn thresh` is the right outcome.
	 */
	if (!plugin_lease_take(NN_PLUGIN_LEASE_WAIT_TICKS)) {
		nn_detail_set("the decoder is busy -- try again");
		nn_guards_give();
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
#endif

	nn_claims_begin();
	rc = nn_model_reload(model_at, model_len, info.name, &open_after);

	/*
	 * [!] THE RESULTING MODEL STATE IS READ BACK, NOT ASSUMED.  This board's
	 * dispatcher adopts whatever handle the backend ended up with and clears
	 * `open` when that is NULL -- which is the documented case where even the
	 * PREVIOUS model could not be rebuilt.  Reporting "rejected, previous
	 * unchanged" there would leave an operator believing a model is loaded.
	 *
	 * [!] AND IT IS THE RELOAD'S OWN OUTCOME, NOT A QUESTION ASKED AFTERWARDS.
	 * This used to ask nn_model_open(), which on a closed singleton opens a
	 * fresh EMPTY one and succeeds -- so exactly the case above was reported as
	 * PREVIOUS, and since issue #108 it also kept the previous container's
	 * claims beside a model that was gone.  A non-mutating query does not fix
	 * it either: `nn info` on another console takes no session and calls
	 * nn_model_open() itself, so it can re-open the singleton between the
	 * reload returning and any read here (the #108 review, rounds 2 and 3).
	 * nn_model_reload() reports what it left.
	 */
	if (open_after)
		*state = (rc == 0) ? NN_MODEL_NEW : NN_MODEL_PREVIOUS;
	else
		*state = NN_MODEL_EMPTY;

	/*
	 * [!] THE LAST RESULT GOES WITH THE MODEL IT CAME FROM (issue #118), and
	 * it goes whenever what is open changed -- a new model, a new model whose
	 * plugin is then refused below, or a rollback that left nothing -- not
	 * only when the load reports success.  Here, under the session and the
	 * lease and before the claims settle, so no reader can see the new
	 * identity beside the old result.  PREVIOUS changed nothing and keeps it.
	 */
	if (*state != NN_MODEL_PREVIOUS)
		nn_camera_record_invalidate();

	/*
	 * The claims follow what is now open, and are settled BEFORE the session is
	 * given back -- after it, another console's load could publish its own and
	 * then have them overwritten with this one's.  A refused reload that restored
	 * the previous model keeps the previous claims (NN_MODEL_PREVIOUS changes
	 * nothing here).
	 */
#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE PLUGIN IS REPLACED ONLY AFTER THE BACKEND SUCCEEDED (issue #110),
	 * and the order is the whole of it.  Loading first would destroy the
	 * previous plugin's state at the fixed reservation before knowing whether
	 * the model that needs it can be built -- and a backend that then restored
	 * the PREVIOUS model would be left with no decoder for it.  So: the
	 * previous plugin is untouched while nn_model_reload() runs above, and only
	 * the outcomes that changed what is open change what decodes it.
	 *
	 * NN_MODEL_PREVIOUS is deliberately absent: nothing moved, so nothing here
	 * moves either.
	 *
	 * All of it is still inside nn_claims_begin()/settle() and before
	 * nn_guards_give(), so `nn info` on another console sees "a load is in
	 * progress" rather than a model from one load beside a plugin from
	 * another.
	 */
	if (*state == NN_MODEL_EMPTY) {
		plugin_run_unload();
	} else if (*state == NN_MODEL_NEW) {
		if (!is_container) {
			/* A bare model is a legal thing to load, and since issue #116 it
			 * means NOTHING reads its outputs -- `nn run` reports the tensors
			 * themselves and a live overlay is refused.  Whatever was loaded
			 * before must go: a new model with an old model's decoder is the
			 * exact accident this ordering exists to prevent. */
			plugin_run_unload();
		} else {
			/* The deepest of the shell-thread call sites: a plugin's entry()
			 * is reached from here, several frames below the command. */
			nn_camera_note_depth(NNCAM_SITE_SHELL);
			/* [!] THE STAGING REGION IS PASSED, NOT LOOKED UP.  The backend
			 * is double-slotted: nn_model_load_region() hands out the
			 * INACTIVE slot, so now that the reload above has adopted the
			 * staged model, asking again would answer with the OTHER slot.
			 * `stage` and `cap` are what this function was handed before any
			 * of that happened. */
			(void)plugin_run_load(&claims.view, stage, stage, cap,
			                      nn_active_base());
		}
		/* [!] A CONTAINER WHOSE PLUGIN WOULD NOT LOAD LEAVES THE MODEL OPEN
		 * WITH NOTHING READING IT (issue #116).  Until Step 3c that landed on
		 * the resident decoder, so a refused plugin still produced boxes of a
		 * sort; now the model behaves exactly like a bare one -- `nn run`
		 * reports its output tensors and `nn stream start` is refused.
		 * plugin_run_load() has already unpublished whatever it refused and
		 * logs its own reason, which is the only place that says WHY.
		 *
		 * [!] THIS BRANCH IS NOT COVERED BY A TEST.  There is no way to build
		 * a container whose plugin the device refuses -- the host packer runs
		 * the device's own validator over what it packs -- so what is written
		 * here is a claim about an unexercised path.  See the board README. */
	}
#endif

	/*
	 * [!] SETTLED AFTER THE DECODER MOVED, NOT BEFORE (issue #110).  This ran
	 * first, and the comment beside the block above said the replacement was
	 * "inside the claims window" while settling had already closed it -- so
	 * another console could read the NEW container's claims beside the OLD
	 * plugin.  Settling last is what makes `a model load is in progress` cover
	 * the whole of it.
	 */
	if (*state == NN_MODEL_NEW)
		nn_claims_settle(0, is_container ? &claims : NULL);
	else if (*state == NN_MODEL_EMPTY)
		nn_claims_settle(0, NULL);
	else
		nn_claims_settle(1, NULL);     /* the previous model, its claims */
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* Held from before the first change to after the last one. */
	plugin_lease_give();
#endif
	nn_guards_give();

	if (rc != 0) {
		nn_detail_set("%s", nn_model_strerror(rc));
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

void nn_svc_model_unload(struct nn_op_result *res)
{
	struct nn_model *m = NULL;
	int rc;

	nn_detail_clear();

	if (nn_camera_running()) {
		nn_detail_set("stop the inference stream first (`nn stream stop`)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	rc = nn_guards_take(res);
	if (rc != NN_SVC_OK) {
		nn_result(res, rc, NN_CLAIM_NONE);
		return;
	}
	/* Idempotent: the model is a singleton that is rebuilt rather than
	   destroyed, so unloading returns it to the built-in one. */
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* Before anything changes, and its failure is an answer -- see the load
	 * path for why the session is not enough on its own. */
	if (!plugin_lease_take(NN_PLUGIN_LEASE_WAIT_TICKS)) {
		nn_detail_set("the decoder is busy -- try again");
		nn_guards_give();
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
#endif
	nn_claims_begin();
	if (nn_model_open(&m) == 0 && m != NULL)
		(void)nn_model_reload(NULL, 0u, NULL, NULL);
	/* The model and its decoder are gone, and the last result with them
	 * (issue #118) -- under the lease, before anything is settled. */
	nn_camera_record_invalidate();
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* No decoder either (issue #110): a plugin left loaded would be a decoder
	   for a model that is gone, waiting to interpret the next one.  BEFORE the
	   claims are settled, like the load path -- settling first would publish
	   "no container" beside a plugin that was still live. */
	plugin_run_unload();
#endif
	/* No model is open now, so no container's claims describe it (issue #108).
	   Under the session, for the same reason as in the load path. */
	nn_claims_settle(0, NULL);
#if defined(CONFIG_NN_BACKEND_TFLM)
	plugin_lease_give();
#endif
	nn_guards_give();
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- tensors ------------------------------------------------------------- */

int nn_svc_tensors_pin(void)
{
	struct nn_model *m = NULL;
	/* The pin reports a status only; nobody prints a detail for it, so the
	   words nn_guards_take() would write land here and are discarded. */
	struct nn_op_result probe;
	struct nn_op_result *res = &probe;

	(void)res;
	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	/* [!] BOTH guards: reading tensor bodies walks the arena, and the arena is
	 * in the PSRAM behind OCTOSPI1 -- the same reason `nn bench` and `nn dets`
	 * are guarded here and `nn info` is not. */
	return nn_guards_take(&probe);
}

void nn_svc_tensors_unpin(void)
{
	nn_guards_give();
}

int nn_svc_output_count(void)
{
	struct nn_model *m = NULL;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	return nn_output_count(m);
}

int nn_svc_output(unsigned index, struct tensor_desc *out)
{
	struct nn_model *m = NULL;
	struct nn_tensor *t;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	t = nn_output(m, (int)index);
	if (t == NULL)
		return NN_SVC_ERR_ARG;
	nn_desc_of(out, t);
	return NN_SVC_OK;
}

int nn_svc_input(struct tensor_desc *out)
{
	struct nn_model *m = NULL;
	struct nn_tensor *t;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	t = nn_input(m, 0);
	if (t == NULL)
		return NN_SVC_ERR_ARG;
	nn_desc_of(out, t);
	return NN_SVC_OK;
}

/* ---- one shot ------------------------------------------------------------ */

/** How long `nn run` waits for its one inference, in seconds. */
#define NN_RUN_WAIT_S 3u

static const char *nn_nncam_strerror(int rc);
/* The lifecycle helpers, defined with the lifecycle below (issue #120). */
static enum nn_stream_start_claim nn_oneshot_admit(void);
static uint32_t nn_oneshot_commit(void);
static void nn_stream_unadmit(void);
static enum nn_stream_stop_claim nn_stream_claim_stop(uint32_t gen);
struct nn_stream_end;
static void nn_stream_settle(enum nn_claim claim,
                             const struct nn_stream_end *final);

/*
 * The worker's snapshot, as the shared command reads it.  [!] EVERY FIELD,
 * carried rather than asserted (issues #104, #110, #118): a projection that
 * drops one hands the caller whatever its initialiser left there.
 */
static void nn_snap_of(const struct nn_camera_decode *dec,
                       struct nn_det_snapshot *snap)
{
	snap->valid      = dec->valid;
	snap->ndet       = dec->ndet;
	snap->kind       = dec->kind;
	snap->res        = dec->res;
	snap->reportable = dec->reportable;
	snap->current    = dec->current;
	snap->accepted   = dec->accepted;
	snap->epoch      = dec->epoch;
}

void nn_svc_run_once(struct nn_det_snapshot *snap, struct bf_det *dets, int max,
                     struct nn_report_capture *rep,
                     nn_svc_cancel_fn cancel, void *ctx,
                     struct nn_op_result *res)
{
	struct nn_camera_stats st;
	struct nn_camera_decode dec;
	uint32_t base, gen;
	int rc, stop_rc;
	ULONG deadline;
	/* Why the wait ended -- the run's status, decided below (issue #122 P7). */
	enum { RUN_INFERRED, RUN_LOST, RUN_CANCELLED, RUN_TIMEOUT } why = RUN_TIMEOUT;

	nn_detail_clear();

	/*
	 * [!] THE LIFECYCLE IS CLAIMED BEFORE THE WORKER IS TOUCHED (issue #120).
	 * `nn run` drives the same worker as `nn stream`, and it used to do so
	 * with the lifecycle saying IDLE: a `nn stream start` from the other
	 * console was admitted over it, re-armed this session without asking
	 * whether anything could draw, and this run's stop then tore down a
	 * stream it never started.  As a one-shot it can be neither re-armed nor
	 * stopped by anyone else, and it stops itself by its own generation.
	 */
	switch (nn_oneshot_admit()) {
	case NN_STREAM_START_GO:
		break;
	case NN_STREAM_START_RUNNING:
		nn_detail_set("a stream is already running -- `nn stream stats`");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_ONESHOT:
		nn_detail_set("another `nn run` holds the camera, or one returned "
		              "with its teardown unfinished (`nn stream stop` "
		              "finishes it)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a "
		              "reboot clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_START_BUSY:
	default:
		nn_detail_set("a start or a stop is already in progress -- retry");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	rc = nn_camera_start(0, 0);   /* no panel: a report-only decoder serves this */
	if (rc != NNCAM_OK) {
		nn_stream_unadmit();
		/* [!] The worker's own words and the stream start's status: one
		 * refusal from one worker reads the same from either command.  This
		 * used to guess "NN busy, PSRAM down, or no model loaded?" for every
		 * code, including a DCMI owned by a frame stream. */
		nn_detail_set("%s", nn_nncam_strerror(rc));
		nn_result(res, (rc == NNCAM_ERR_RUNNING) ? NN_SVC_ERR_STATE
		                                         : NN_SVC_ERR_HW,
		          NN_CLAIM_NONE);
		return;
	}
	gen = nn_oneshot_commit();
	if (gen == NN_STREAM_GEN_ANY) {
		/* Unreachable under the transitions admission allows; fail CLOSED,
		 * exactly as a refused stream commit does -- a teardown now would
		 * be ownerless. */
		nn_detail_set("the stream lifecycle moved underneath this run; what "
		              "owns the hardware now cannot be established");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}

	/*
	 * [!] WHAT THIS RUN PRODUCED IS COUNTED BY THE RECORD (issue #118).  The
	 * record keeps the last result across sessions now, so "valid" says
	 * nothing about this run; the count of accepted publishes does, and it is
	 * taken under the same lock as the publish.  The base is sampled after
	 * nn_camera_start()'s boundary: from there on the generation rule admits
	 * only this run's publishes.
	 */
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, NULL);
	base = dec.accepted;

	/* [!] Wall-clock deadline, not a count of completed sleeps: a sleep that
	 * returns early on an already-pending tick would burn the budget instantly
	 * and report a timeout that never happened. */
	deadline = tx_time_get() + (NN_RUN_WAIT_S * TX_TIMER_TICKS_PER_SECOND);
	for (;;) {
		(void)nn_camera_decode_get(&dec, NULL);
		if (dec.accepted != base) {
			why = RUN_INFERRED;
			break;
		}
		nn_camera_stats_get(&st);
		if (st.stream_lost) {
			why = RUN_LOST;
			break;
		}
		if (nn_svc_cancelled(cancel, ctx)) {
			why = RUN_CANCELLED;
			break;
		}
		if ((LONG)(tx_time_get() - deadline) >= 0) {
			why = RUN_TIMEOUT;
			break;
		}
		tx_thread_sleep(1u);
	}

	/* [!] The result is taken BEFORE the stop.  The record keeps it across the
	 * stop now (issue #118), but a plugin's account of it is only this
	 * result's while the session that produced it is the one in force. */
	memset(&dec, 0, sizeof dec);
	/* [!] AND THE CAPTURE HAPPENS IN HERE (issue #110), under the result lease,
	   because a plugin's account of its result has to be taken while that
	   result is still the one the snapshot describes.  By the time this
	   function returns the session is gone.
	   [!] AND NO BOXES COME BACK (issue #116).  The shared command reads its
	   own array only for NN_DET_CALLER_BOXES, and nothing on this board
	   publishes that kind any more -- a plugin keeps its result, a bare model
	   has none -- so the array it lends us is left exactly as it arrived. */
	(void)dets;
	(void)max;
	(void)nn_camera_decode_get(&dec, rep);
	nn_snap_of(&dec, snap);
	/* [!] THIS RUN'S, OR NOT VALID (issue #118).  The record's own `valid`
	 * would be true of whatever ran last -- a stream stopped an hour ago --
	 * and the shared command prints a valid snapshot as this run's result. */
	snap->valid = nn_det_last_valid(snap, base);

	/* [!] A RESULT THAT MADE IT IS NOT A TIMEOUT (issue #122, review).  The
	 * deadline can pass in the same moment the inference publishes; the record
	 * was read after that, so reporting a timeout would throw away an answer
	 * that is sitting right here.  ONLY a timeout is promoted: a cancel is the
	 * operator's decision and a lost stream is a hardware fact, and a valid
	 * record does not overrule either.  "Made it" is the accepted count
	 * against this run's base (issue #118), not the record's `valid`. */
	if (why == RUN_TIMEOUT && snap->valid)
		why = RUN_INFERRED;

	nn_camera_stats_get(&st);

	/* [!] STOPPED BY ITS OWN GENERATION, claimed like any stop (issue #120).
	 * Nothing else can have claimed it -- an operator's stop is refused while
	 * this runs -- so a refusal here is an invariant failure, and it fails
	 * closed: the worker is left exactly as it is. */
	if (nn_stream_claim_stop(gen) != NN_STREAM_STOP_GO) {
		nn_detail_set("the stream lifecycle moved underneath this run; what "
		              "owns the hardware now cannot be established");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}
	stop_rc = nn_camera_stop();
	/* A retryable teardown leaves the one-shot RUNNING and the operator's to
	 * finish with `nn stream stop` -- see nn_stream_life.h. */
	nn_stream_settle(nn_claim_of_stop(stop_rc), NULL);

	/*
	 * [!] THIS RETURN USED TO BE DISCARDED, AND THAT WAS THE BUG (issue #50).
	 * The one-shot threw away `nn_camera_stop()`'s result on both the normal
	 * and the cancelled path, so a teardown that did not finish -- with the NN
	 * session and the OCTOSPI1 guard still held -- was reported as a clean run.
	 * The next start would then be refused for a reason nobody had been told.
	 */
	/*
	 * [!] AND WHY THE WAIT ENDED IS THE STATUS (issue #122 P7).  A cancelled or
	 * timed-out run used to come back NN_SVC_OK with nothing published, which
	 * the shared command could only print as "no decode was published for that
	 * frame" -- the same words for an operator's Ctrl+C, a worker that never
	 * got a frame, and a stream that died.  The disposition stays its own
	 * field: an unfinished teardown is reported beside any of them.
	 */
	switch (why) {
	case RUN_INFERRED:
		nn_result(res, NN_SVC_OK, nn_claim_of_stop(stop_rc));
		if (res->claim != NN_CLAIM_NONE)
			nn_detail_set("the teardown did not finish (%d); the claims "
			              "are still held", stop_rc);
		else if (st.stream_lost)
			nn_detail_set("the band stream was lost during the run");
		return;
	case RUN_LOST:
		nn_result(res, NN_SVC_ERR_HW, nn_claim_of_stop(stop_rc));
		nn_detail_set("the band stream was lost before an inference "
		              "completed%s", (res->claim != NN_CLAIM_NONE)
		              ? "; the teardown did not finish either (`nn stream "
		                "stop` finishes it)" : "");
		return;
	case RUN_CANCELLED:
		nn_result(res, NN_SVC_ERR_CANCEL, nn_claim_of_stop(stop_rc));
		nn_detail_set("cancelled before an inference completed%s",
		              (res->claim != NN_CLAIM_NONE)
		              ? "; the teardown did not finish either (`nn stream "
		                "stop` finishes it)" : "");
		return;
	case RUN_TIMEOUT:
	default:
		nn_result(res, NN_SVC_ERR_TIMEOUT, nn_claim_of_stop(stop_rc));
		nn_detail_set("no inference completed within %u s%s",
		              (unsigned)NN_RUN_WAIT_S,
		              (res->claim != NN_CLAIM_NONE)
		              ? "; the teardown did not finish either (`nn stream "
		                "stop` finishes it)" : "");
		return;
	}
}

void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max, struct nn_report_capture *rep,
                           struct nn_op_result *res)
{
	struct nn_camera_decode dec;

	nn_detail_clear();
	memset(&dec, 0, sizeof dec);
	/* No boxes -- see nn_svc_run_once. */
	(void)dets;
	(void)max;
	/* [!] THE LAST RESULT, WHOEVER PRODUCED IT (issue #118): a `nn run`, a
	 * running stream, or one that has stopped.  Only a model change clears
	 * it, so `valid` is the record's own and is not narrowed to a session. */
	(void)nn_camera_decode_get(&dec, rep);
	nn_snap_of(&dec, snap);
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- bench --------------------------------------------------------------- */

void nn_svc_bench_prepare(struct nn_op_result *res)
{
	struct nn_model *m = NULL;
	int rc, i;

	nn_detail_clear();

	if (nn_model_open(&m) != 0 || m == NULL) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	rc = nn_guards_take(res);
	if (rc != NN_SVC_OK) {
		nn_result(res, rc, NN_CLAIM_NONE);
		return;
	}
	/* [!] The carve-out is NOLOAD, so an input holds whatever survived the last
	 * reset until something fills it.  A fixed pattern makes every run
	 * comparable; which pattern does not matter, that there is one does. */
	for (i = 0; i < nn_input_count(m); i++) {
		struct nn_tensor *t = nn_input(m, i);

		if (t && t->data)
			memset(t->data, 0x5A, t->bytes);
	}
	nn_guards_give();
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

void nn_svc_bench_run(uint32_t iters, struct nn_bench_stats *out,
                      nn_svc_cancel_fn cancel, void *ctx,
                      struct nn_op_result *res)
{
	struct nn_model *m = NULL;
	uint32_t i;
	int rc;

	nn_detail_clear();
	memset(out, 0, sizeof *out);
	out->min_us = 0xFFFFFFFFu;

	if (nn_model_open(&m) != 0 || m == NULL) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	rc = nn_guards_take(res);
	if (rc != NN_SVC_OK) {
		nn_result(res, rc, NN_CLAIM_NONE);
		return;
	}

	for (i = 0u; i < iters; i++) {
		uint32_t us;

		if (nn_svc_cancelled(cancel, ctx)) {
			nn_detail_set("cancelled after %lu of %lu run(s)",
			              (unsigned long)i, (unsigned long)iters);
			nn_guards_give();
			nn_result(res, NN_SVC_ERR_CANCEL, NN_CLAIM_NONE);
			return;
		}
		if (nn_run(m) != 0) {
			nn_detail_set("inference failed on run %lu of %lu",
			              (unsigned long)i + 1u, (unsigned long)iters);
			nn_guards_give();
			nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
			return;
		}
		{
			/* The DWT counter runs at the core clock on this board, and
			   the core clock is INHERITED from the bootloader -- so it is
			   read at run time rather than assumed. */
			uint32_t mhz = SystemCoreClock / 1000000u;

			us = mhz ? (nn_last_cycles(m) / mhz) : 0u;
		}
		out->total_us += us;
		if (us < out->min_us)
			out->min_us = us;
		if (us > out->max_us)
			out->max_us = us;
		out->runs++;
	}
	nn_guards_give();

	if (out->runs == 0u)
		out->min_us = 0u;
	else
		out->avg_us = (uint32_t)(out->total_us / out->runs);
	/* Say what the cycle -> microsecond conversion assumed. */
	out->clock_mhz = SystemCoreClock / 1000000u;
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- norm, overlay, boxes and threshold ---------------------------------- */

/* The worker's codes in words.  Moved here from this board's own `nn stream`
   command file when issue #99 replaced it with the shared one: the port is
   where the codes are produced, and it is the port that can name them. */
static const char *nn_nncam_strerror(int rc)
{
	/* [!] EVERY SENTENCE IS CHECKED AGAINST NN_SVC_DETAIL_MAX AT BUILD TIME
	 * (issue #122 P15).  Each is copied whole into a result's detail, and one
	 * of them used to be longer than that: on hardware it ended "or see `d",
	 * the half the copy cut off being the advice. */
	switch (rc) {
	case NNCAM_ERR_RUNNING:
		return NN_SVC_DETAIL_LIT(
			"a stream is already running (`nn stream stats`)");
	case NNCAM_ERR_NOTRUN:
		return NN_SVC_DETAIL_LIT(
			"not running");
	case NNCAM_ERR_MODEL:
		return NN_SVC_DETAIL_LIT(
			"no model loaded, or it has no usable input "
			"tensor (`blob list`, then `nn model load --slot <n>`)");
	case NNCAM_ERR_SESSION:
		return NN_SVC_DETAIL_LIT(
			"the NN session is busy (`nn bench` or "
			"`nn model load` is running)");
	case NNCAM_ERR_PSRAM:
		return NN_SVC_DETAIL_LIT(
			"PSRAM not ready, or OCTOSPI1 is held by a "
			"psram/membench/devmem/wifi flash command");
	case NNCAM_ERR_BAND:
		return NN_SVC_DETAIL_LIT(
			"the camera would not start a band stream: a frame "
			"stream may own the DCMI (`camera stream stop`), or the "
			"other console is starting/stopping one; see `dmesg`");
	case NNCAM_ERR_GEOM:
		return NN_SVC_DETAIL_LIT(
			"the model input does not tile onto the camera's "
			"4 bands, or its dtype is neither int8 nor "
			"float32 (`nn info`)");
	case NNCAM_ERR_QUANT:
		return NN_SVC_DETAIL_LIT(
			"the int8 input carries no per-tensor quantization "
			"scale (`nn info` shows q(s=0.000000)) -- a "
			"per-axis quantized input is not supported");
	case NNCAM_ERR_SHAPES:
		return NN_SVC_DETAIL_LIT(
			"the container's decoder cannot read this "
			"model's outputs -- its two halves do not "
			"belong together (`nn info`)");
	/*
	 * [!] IT ALSO COVERS "THERE IS NO DECODER AT ALL" (issue #116).  This
	 * firmware carries none, so a bare model -- or a container whose plugin
	 * was refused -- reaches the same refusal as a plugin with no draw()
	 * slot, and the words have to fit all three.
	 *
	 * [!] AND THAT FOLDING IS DELIBERATE, unlike grove-vision-ai-v2, which
	 * words the two separately.  Three paths arrive at this ONE code -- the
	 * answer they share is `nn_active_can_draw() == 0`, and the admission
	 * cannot tell an absent decoder from a present one with no draw() slot
	 * without asking a second question it has no reason to ask.  So the
	 * sentence names both possibilities and points at `nn info`, which does
	 * know which it is.  Splitting the code would mean splitting the
	 * question.
	 */
	case NNCAM_ERR_NODRAW:
		return NN_SVC_DETAIL_LIT(
			"nothing would annotate a live preview: no "
			"decoder is loaded, or the one that is "
			"draws nothing (`nn info`); `nn run` still "
			"works");
	case NNCAM_ERR_DECBUSY:
		return NN_SVC_DETAIL_LIT(
			"the decoder could not be held still long "
			"enough to ask it -- try again");
	case NNCAM_ERR_INIT:
		return NN_SVC_DETAIL_LIT(
			"the worker thread or its objects could not be "
			"created");
	case NNCAM_ERR_TEARING:
		return NN_SVC_DETAIL_LIT(
			"still tearing down (a callback or an inference "
			"has not returned) -- run `nn stream stop` again");
	case NNCAM_ERR_REARM:
		return NN_SVC_DETAIL_LIT(
			"the stream could not be re-armed (the DCMI may "
			"be owned elsewhere) -- run `nn stream stop`, "
			"then `nn stream start`");
	default:
		return NN_SVC_DETAIL_LIT("unknown error");
	}
}

/* ---- live inference (issue #99) ------------------------------------------
 *
 * The worker in src/nn_camera.c is unchanged and still owns the real lifecycle,
 * the NN session and the OCTOSPI1 guard.  What this adds is an IDENTITY, so a
 * `--frames` waiter on one console cannot stop a stream that a second console
 * started after its own had gone.
 *
 * [!] A RE-ARM MINTS A NEW GENERATION.  Re-arming after a lost band stream is a
 * successful start that deliberately keeps the guards and the counters running
 * -- but it is a new stream as far as authority goes, and if it inherited the
 * generation, a waiter from before the outage would still be entitled to stop
 * it.
 */
/* The lifecycle itself is svc/nn_stream_life.c's -- see there for why one
   machine rather than three, and for the rule that every call below is made
   under this port's own critical section. */
static struct nn_stream_life nn_life;
static uint32_t nn_stream_t0;
static uint32_t nn_stream_ms;
/*
 * [!] A STREAM'S NUMBERS ARE LATCHED WHEN IT ENDS (issue #120).  The worker's
 * counters are reset by every nn_camera_start(), and `nn run` starts the worker
 * too -- so a poll that kept reading them after the stream ended reported the
 * last `nn run` as though it were the stream ("infers 1" after thirty frames).
 * The stop takes the stream's final numbers once the worker is quiet; a poll of
 * that generation reads them from here and never the worker again.
 */
/*
 * ...AND SO ARE THE BOARD LINES THAT DESCRIBE THE STREAM.  `ingest`, `tensor`,
 * `at call` and `plugin` come from the same worker counters (or from the panel's,
 * which the worker re-arms), so they moved with every `nn run` too.  What is
 * kept live is only what is not about the stream: the session, a lost band,
 * and the norm/overlay settings.
 */
struct nn_stream_end {
	struct nn_stream_stats stats;
	struct nn_camera_stats cam;          /* the worker's, at the stop       */
	uint32_t spent, refused, miss, run;  /* the panel's plugin numbers      */
	/* The record's epoch when `last` was latched: a model change since then
	 * took the result away, and the latched line must say so (issue #118). */
	uint32_t epoch;
};
static struct nn_stream_end nn_stream_final;
static uint32_t nn_stream_final_gen;   /**< whose they are; ANY = nobody's */
/*
 * [!] THE WORKER'S COUNTERS DO NOT RESET ON A RE-ARM, DELIBERATELY -- it keeps
 * them running across an outage so the outage does not hide in them.  But
 * nn_stream_stats is defined PER GENERATION, and a re-arm mints a new one: with
 * the cumulative numbers passed straight through, `nn stream start --frames 10`
 * after a hundred frames satisfied the shared waiter's test immediately and
 * stopped the stream it had just re-armed.  So the baselines are latched here
 * and subtracted; the lifetime totals stay available in the board lines.
 */
static uint32_t nn_stream_frames0;
static uint32_t nn_stream_skipped0;
static uint32_t nn_stream_infers0;
static uint32_t nn_stream_errors0;
/*
 * [!] AND THE RECORD'S ACCEPTED COUNT (issue #118).  The last result outlives
 * a stop now, so a new generation opens with the previous one's result still
 * in the record; `last` is this generation's only if the record has accepted a
 * publish since this base.  Sampled after the start's boundary and before the
 * commit -- see nn_det_last_valid() for why that order errs only towards "none
 * yet".
 */
static uint32_t nn_stream_acc0;

/* Counters only ever move forwards, but the latch is taken outside the critical
   section that commits, so a frame can land in between.  Clamp rather than
   wrap: a moment of zero is honest, 4 billion is not. */
static uint32_t nn_since(uint32_t now, uint32_t base)
{
	return (now >= base) ? (now - base) : 0u;
}

/*
 * Admit a start BEFORE the worker is touched.
 *
 * [!] FROM IDLE OR FROM RUNNING, and the second is not laxity: this board can
 * RE-ARM a stream whose capture died under it, which succeeds while the stream
 * is still up.  Both are refused from STARTING, STOPPING and LOST -- so a
 * re-arm can no longer overwrite a stop that already owns the teardown, nor
 * resurrect a lifecycle that was latched terminal.
 */
static enum nn_stream_start_claim nn_stream_admit(void)
{
	enum nn_stream_start_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	r = nn_stream_life_begin(&nn_life, NN_STREAM_KIND_STREAM);
	/* [!] A re-arm is admitted only where a re-arm is possible: from RUNNING.
	 * Trying it after any refusal would let it through from STOPPING too --
	 * and after NN_STREAM_START_ONESHOT it would take over a `nn run`'s
	 * session, which is issue #120.  rearm() refuses a one-shot as well. */
	if (r == NN_STREAM_START_RUNNING && nn_stream_life_rearm(&nn_life))
		r = NN_STREAM_START_GO;
	TX_RESTORE
	return r;
}

/* `nn run`'s admission: a one-shot, never re-armed (issue #120). */
static enum nn_stream_start_claim nn_oneshot_admit(void)
{
	enum nn_stream_start_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	r = nn_stream_life_begin(&nn_life, NN_STREAM_KIND_ONESHOT);
	TX_RESTORE
	return r;
}

/* ...and its commit.  [!] NO BASELINES: those describe the last STREAM, which
 * `nn stream stats` keeps reporting; a one-shot is not a stream to report on. */
static uint32_t nn_oneshot_commit(void)
{
	uint32_t g;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	g = nn_stream_life_commit(&nn_life);
	TX_RESTORE
	return g;
}

static void nn_stream_unadmit(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	(void)nn_stream_life_abort(&nn_life);
	TX_RESTORE
}

static void nn_stream_mint(const struct nn_camera_stats *base, uint32_t acc0,
                           uint32_t *gen)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* [!] Baselines published only once the generation exists, so a refused
	 * commit cannot leave this generation's numbers describing another's. */
	*gen = nn_stream_life_commit(&nn_life);
	if (*gen != NN_STREAM_GEN_ANY) {
		nn_stream_frames0  = base->frames;
		nn_stream_skipped0 = base->skipped;
		nn_stream_infers0  = base->infers;
		nn_stream_errors0  = base->errors;
		nn_stream_acc0     = acc0;
		nn_stream_t0 = (uint32_t)tx_time_get();
		nn_stream_ms = 0u;
	}
	TX_RESTORE
}

/*
 * [!] TEST AND CLAIM IN ONE CALL, under one critical section.  Comparing the
 * generation and then acting is what let two callers both be admitted for one
 * stream -- and the second of them then ran its teardown against whatever was
 * there by the time it got round to it, which on this board is an untagged
 * nn_camera_stop().
 */
static enum nn_stream_stop_claim nn_stream_claim_stop(uint32_t gen)
{
	enum nn_stream_stop_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	r = nn_stream_life_claim_stop(&nn_life, gen);
	TX_RESTORE
	return r;
}

/*
 * Settle a claimed teardown by its disposition -- the one place both stops
 * (`nn stream stop` and `nn run`'s own) do it.
 *
 * [!] The elapsed freeze rides on the transition, not beside it: if the settle
 * were refused, freezing anyway would date a generation that is still running.
 * And only a STREAM's end is dated: the clock is the last stream's, which
 * `nn stream stats` reports, and a one-shot ending must not overwrite it.
 */
static void nn_stream_settle(enum nn_claim claim,
                             const struct nn_stream_end *final)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (claim == NN_CLAIM_RETRYABLE) {
		(void)nn_stream_life_retry(&nn_life);  /* stoppable again, same gen */
	} else {
		int stream = (nn_life.kind == (uint8_t)NN_STREAM_KIND_STREAM);
		uint32_t ending = nn_life.gen;
		int took = (claim == NN_CLAIM_TERMINAL)
		         ? nn_stream_life_poison(&nn_life)
		         : nn_stream_life_finish(&nn_life);  /* the generation is kept */

		if (took && stream) {
			nn_stream_ms = (uint32_t)(((uint32_t)tx_time_get() -
			                           nn_stream_t0) * 1000u /
			                          TX_TIMER_TICKS_PER_SECOND);
			/* The ended stream's numbers, for every poll from here on. */
			if (final != NULL) {
				nn_stream_final = *final;
				nn_stream_final.stats.elapsed_ms = nn_stream_ms;
				nn_stream_final_gen = ending;
			}
		}
	}
	TX_RESTORE
}


static uint32_t nn_cyc_to_us(uint32_t cyc)
{
	uint32_t mhz = SystemCoreClock / 1000000u;

	return mhz ? (cyc / mhz) : 0u;
}

void nn_svc_stream_start(const struct nn_stream_spec *spec,
                         struct nn_op_result *res, uint32_t *gen)
{
	int rearm, rc;

	if (res == NULL)
		return;
	res->detail[0] = '\0';
	if (spec == NULL || gen == NULL) {
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}

	/* [!] THE DRAW-CAPABILITY CHECK IS NOT HERE ANY MORE (issue #110).  It was,
	 * and it answered before the NN session was held -- so a load landing in
	 * between changed the decoder after the question, and a timed-out lease
	 * was read as "yes".  nn_camera_start() asks it under the session and the
	 * lease, which is the decoder that will actually run.
	 */

	/* Sampled BEFORE the call, because a successful re-arm clears the latch. */
	rearm = nn_camera_running() && cam_band_stream_lost();

	/* [!] ADMITTED BEFORE THE WORKER IS TOUCHED.  Recording the start afterwards
	 * left a window in which this said IDLE while the board was already
	 * streaming -- and on a re-arm it overwrote a stop that had already claimed
	 * the teardown. */
	switch (nn_stream_admit()) {
	case NN_STREAM_START_GO:
		break;
	case NN_STREAM_START_RUNNING:
		nn_detail_set("a stream is already running (`nn stream stats`)");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_ONESHOT:
		/* [!] NOT A RE-ARM (issue #120): the session is a `nn run`'s. */
		nn_detail_set("a `nn run` holds the camera, or one returned with its "
		              "teardown unfinished (`nn stream stop` finishes it)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a "
		              "reboot clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_START_BUSY:
	default:
		nn_detail_set("a start or a stop is already in progress -- retry");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	rc = nn_camera_start(spec->test ? 1 : 0, 1);   /* a panel: DRAW is required */
	if (rc != NNCAM_OK) {
		nn_stream_unadmit();      /* a failed re-arm goes back to RUNNING */
		nn_detail_set("%s", nn_nncam_strerror(rc));
		nn_result(res, (rc == NNCAM_ERR_RUNNING) ? NN_SVC_ERR_STATE
		                                         : NN_SVC_ERR_HW,
		          NN_CLAIM_NONE);
		return;
	}
	{
		/* Latched AFTER the start, so a re-arm's carried-over totals become
		   this generation's zero. */
		struct nn_camera_stats base;
		struct nn_camera_decode rec;

		nn_camera_stats_get(&base);
		/* After the start's record boundary, before the commit: the two
		 * interleavings are in svc/nn_det_record.h (issue #118).  The
		 * record's lock is taken here, outside the commit's critical
		 * section, which may not take it. */
		memset(&rec, 0, sizeof rec);
		(void)nn_camera_decode_get(&rec, NULL);
		nn_stream_mint(&base, rec.accepted, gen);
	}
	if (*gen == NN_STREAM_GEN_ANY) {
		/*
		 * [!] REFUSED, WHICH MEANS THIS CALLER NO LONGER OWNS THE START -- and
		 * therefore does not know who does.  Unreachable under the transitions
		 * admission allows, so this is invariant-failure handling; it fails
		 * CLOSED rather than tidying up.  Issuing a stop here would be an
		 * ownerless teardown that could collide with one already in progress,
		 * and releasing the claim could free something a live thread is inside.
		 * Report terminal and leave everything exactly as it is.
		 */
		nn_detail_set("the stream lifecycle moved underneath this start; what "
		              "owns the hardware now cannot be established");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}

	/* Say WHICH of the two happened: "started" over a stream that was only
	   re-armed would hide that an outage occurred at all, and the counters
	   deliberately keep running across it, so they do not show it either. */
	if (rearm)
		nn_detail_set("stream re-armed after a lost stream (counters continue)");
	else
		nn_detail_set("inference stream started (worker prio 18%s)",
		              spec->test ? ", colorbar" : "");
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* The per-generation counts from the worker's cumulative ones -- the one
   computation a poll and a stop's latch share. */
static void nn_stream_counts(const struct nn_camera_stats *st, uint32_t f0,
                             uint32_t sk0, uint32_t in0, uint32_t er0,
                             struct nn_stream_stats *out)
{
	/* [!] OFFERED, not ingested: this board counts the two apart, and `skipped`
	 * must be a subset of `frames` or the pair cannot be read. */
	out->skipped = nn_since(st->skipped, sk0);
	out->frames  = nn_since(st->frames,  f0) + out->skipped;
	out->infers  = nn_since(st->infers,  in0);
	out->errors  = nn_since(st->errors,  er0);
	out->last_us = nn_cyc_to_us(st->infer_last_cyc);
}

/* A stopping stream's final numbers, taken after the worker is stopped and
   before the lifecycle is settled. */
static void nn_stream_take_final(struct nn_stream_end *final)
{
	struct nn_camera_decode dec;
	struct nn_det_snapshot snap;
	uint32_t f0, sk0, in0, er0, acc0;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	f0   = nn_stream_frames0;
	sk0  = nn_stream_skipped0;
	in0  = nn_stream_infers0;
	er0  = nn_stream_errors0;
	acc0 = nn_stream_acc0;
	TX_RESTORE
	memset(final, 0, sizeof *final);
	nn_camera_stats_get(&final->cam);  /* takes its own locks: outside */
	nn_stream_counts(&final->cam, f0, sk0, in0, er0, &final->stats);
	/*
	 * [!] THE STREAM'S LAST RESULT IS LATCHED WITH ITS NUMBERS (issue #118).
	 * The stop no longer retires the record, so there is a last result to
	 * report -- and it is taken now, after the stop's boundary, so a later
	 * `nn run` cannot show through it.  Its epoch goes with it: a model change
	 * afterwards takes the result away and the latched line must follow.
	 */
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, NULL);
	nn_snap_of(&dec, &snap);
	final->stats.last_valid = nn_det_last_valid(&snap, acc0) ? 1u : 0u;
	final->stats.last_ndet  = (int32_t)snap.ndet;
	final->epoch            = snap.epoch;
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_LCD
	cam_preview_plugin_draw_stats(&final->spent, &final->refused);
	plugin_lease_misses(&final->miss, &final->run);
#endif
}

int nn_svc_stream_poll(uint32_t gen, struct nn_stream_stats *out)
{
	struct nn_camera_stats st;
	struct nn_camera_decode dec, dec0;
	struct nn_det_snapshot snap;
	uint32_t seq0, seq1, g, t0, ms;
	uint32_t f0, sk0, in0, er0, acc0;
	uint8_t  phase, kind;
	int      live;
	TX_INTERRUPT_SAVE_AREA

	if (out == NULL)
		return NN_SVC_ERR_ARG;

	/* The record's epoch, for a latched answer below.  Taken first because
	 * the record's lock cannot be taken inside the critical section; a model
	 * change landing after it is caught by the next poll. */
	memset(&dec0, 0, sizeof dec0);
	(void)nn_camera_decode_get(&dec0, NULL);

	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, &g, &phase, &seq0, &kind);
	/* [!] An ended stream answers from its latch, taken in the same critical
	 * section as the generation it belongs to -- see nn_stream_final. */
	if (g != NN_STREAM_GEN_ANY && g == nn_stream_final_gen &&
	    (gen == NN_STREAM_GEN_ANY || gen == g)) {
		*out = nn_stream_final.stats;
		/* [!] ...except that a model change since the stop took its last
		 * result away (issue #118): the latch is history, but `last` says
		 * what is still there to read with `nn dets`. */
		if (dec0.epoch != nn_stream_final.epoch)
			out->last_valid = 0u;
		TX_RESTORE
		return NN_SVC_OK;
	}
	t0   = nn_stream_t0;
	ms   = nn_stream_ms;
	f0   = nn_stream_frames0;
	sk0  = nn_stream_skipped0;
	in0  = nn_stream_infers0;
	er0  = nn_stream_errors0;
	acc0 = nn_stream_acc0;
	TX_RESTORE

	if (g == NN_STREAM_GEN_ANY)
		return NN_SVC_ERR_STATE;                  /* nothing has ever run */
	/* [!] Only a DIFFERENT generation is "somebody else's".  Our own, finished,
	 * still answers -- that is how a waiter tells "mine ended" from "a
	 * successor is running", and they call for different words. */
	if (gen != NN_STREAM_GEN_ANY && gen != g)
		return NN_SVC_ERR_GEN;

	/* Outside the critical section: these take their own locks. */
	nn_camera_stats_get(&st);
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, NULL);

	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, NULL, NULL, &seq1, NULL);
	TX_RESTORE
	if (seq1 != seq0)
		return NN_SVC_ERR_STALE;

	/* [!] A `nn run` holding the lifecycle is not a running STREAM (issue
	 * #120): it is reported as not running, and its end does not date one. */
	live = (phase == (uint8_t)NN_STREAM_PHASE_RUNNING &&
	        kind == (uint8_t)NN_STREAM_KIND_STREAM);

	memset(out, 0, sizeof *out);
	out->running    = (live && st.running) ? 1u : 0u;
	/* Per GENERATION, not per worker lifetime -- see the note on the baselines. */
	nn_stream_counts(&st, f0, sk0, in0, er0, out);
	/* [!] The generation's own clock, never the worker's: that one also carries
	 * across a re-arm, and the field's contract says "since this generation
	 * started". */
	out->elapsed_ms = live
	                ? (uint32_t)(((uint32_t)tx_time_get() - t0) * 1000u /
	                             TX_TIMER_TICKS_PER_SECOND)
	                : ms;
	/*
	 * [!] "Nothing decoded yet" is not "decoded nobody" -- the worker's own
	 * contract already required these apart, and -1 means "not a BlazeFace
	 * model", which is a third answer again.
	 *
	 * [!] AND IT IS THIS GENERATION'S ONLY IF THE RECORD SAYS SO (issue
	 * #118).  The record keeps the previous stream's result across the
	 * boundary now, so `valid` alone would open this generation with the last
	 * one's items; and this board's inference counter is bumped outside the
	 * record's lock, so it cannot vouch for the record either.  The record's
	 * own accepted count, against the base the commit latched, can.
	 */
	nn_snap_of(&dec, &snap);
	out->last_valid = nn_det_last_valid(&snap, acc0) ? 1u : 0u;
	out->last_ndet  = (int32_t)dec.ndet;
	return NN_SVC_OK;
}

void nn_svc_stream_stop(uint32_t gen, struct nn_op_result *res)
{
	struct nn_stream_end final;
	enum nn_claim claim;
	int rc;

	if (res == NULL)
		return;
	res->detail[0] = '\0';

	switch (nn_stream_claim_stop(gen)) {
	case NN_STREAM_STOP_GO:
		break;
	case NN_STREAM_STOP_WRONG_GEN:
		nn_detail_set("that stream has already been replaced by another");
		nn_result(res, NN_SVC_ERR_GEN, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_BUSY:
		/* [!] NOTHING WAS ATTEMPTED, SO NOTHING IS THE CALLER'S TO RELEASE
		 * (issue #99, bench).  This used to report RETRYABLE, which made the
		 * shared reporter add "teardown did not finish; nn is still held" --
		 * said to a background waiter whose stream another console was at that
		 * moment tearing down perfectly well.  The claim is somebody else's and
		 * they are settling it; the retry advice belongs in the detail, not in
		 * a warning about a teardown this caller never began. */
		nn_detail_set("a start or another stop owns the stream -- retry");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a reboot "
		              "clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_STOP_ONESHOT:
		/* [!] Refused, not "not running" (issue #120): a `nn run` owns the
		 * worker and stops it itself.  Nothing was attempted, so nothing is
		 * the caller's to release. */
		nn_detail_set("a `nn run` owns the camera and stops it itself -- "
		              "retry when it returns");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_IDLE:
	default:
		nn_detail_set("not running");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] THE LIFECYCLE IS SETTLED ON EVERY EXIT FROM HERE, because the claim
	 * above left it in STOPPING and nothing else can start or stop until it is
	 * handed back.  A path that returned without doing so would wedge the
	 * stream for good -- the failure the retryable answer exists to avoid.
	 */
	rc = nn_camera_stop();
	claim = nn_claim_of_stop(rc);
	nn_stream_take_final(&final);
	nn_stream_settle(claim, &final);

	if (rc == NNCAM_ERR_NOTRUN) {
		nn_detail_set("not running");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	if (rc != NNCAM_OK) {
		/* [!] The incomplete teardowns are RETRYABLE, not failures: the release
		 * is idempotent and repeating the stop is what settles it.  Anything
		 * this board does not document is TERMINAL -- see nn_stop_disp[]. */
		nn_detail_set("%s", nn_nncam_strerror(rc));
		nn_result(res, NN_SVC_ERR_HW, claim);
		return;
	}
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/*
 * This board's extra lines.  They are not decoration: this port deleted the
 * donor's staging machinery on the argument that the inference-to-ingest ratio
 * made it unnecessary, and `raced` and `ingest max` are how it says whether that
 * actually held.  Both would fail silently otherwise.
 *
 * Optional lines are SKIPPED rather than left as holes, because the caller
 * stops at the first index that reports nothing.
 */
int nn_svc_stream_lines(enum nn_stream_lines_ctx ctx, unsigned index,
                        char *buf, size_t cap)
{
	struct nn_camera_stats st;
	unsigned i, n;
	int ended = 0;
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_LCD
	uint32_t fin_spent = 0u, fin_refused = 0u, fin_miss = 0u, fin_run = 0u;
#endif

	if (buf == NULL || cap == 0u)
		return NN_SVC_ERR_ARG;
	buf[0] = '\0';

	if (ctx == NN_STREAM_LINES_STARTED) {
#if BSP_ENABLE_LCD
		/* The ordering an operator has to know BEFORE they wonder why there
		   are no boxes: the guard is held for the stream's whole lifetime. */
		if (index == 0u && !cam_band_claimed(CAM_BAND_PREVIEW)) {
			nn_detail_to(buf, cap,
			             "note    : the OCTOSPI1 guard is held until "
			             "`nn stream stop`, so start `camera preview on` "
			             "FIRST if you want to see the boxes");
			return 1;
		}
#else
		(void)index;
#endif
		return 0;
	}

	nn_camera_stats_get(&st);
	/* [!] AN ENDED STREAM IS DESCRIBED BY ITS LATCH (issue #120) -- see
	 * nn_stream_end.  Only what is not about the stream stays live. */
	{
		uint32_t g;
		TX_INTERRUPT_SAVE_AREA

		TX_DISABLE
		nn_stream_life_snapshot(&nn_life, &g, NULL, NULL, NULL);
		ended = (g != NN_STREAM_GEN_ANY && g == nn_stream_final_gen);
		if (ended) {
			struct nn_camera_stats live = st;

			st = nn_stream_final.cam;
			st.holds_guards = live.holds_guards;
			st.stream_lost  = live.stream_lost;
			st.norm_signed  = live.norm_signed;
			st.overlay      = live.overlay;
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_LCD
			fin_spent   = nn_stream_final.spent;
			fin_refused = nn_stream_final.refused;
			fin_miss    = nn_stream_final.miss;
			fin_run     = nn_stream_final.run;
#endif
		}
		TX_RESTORE
	}
	(void)ended;   /* read only by the plugin line, which some builds lack */
	for (i = 0u, n = 0u; i < 7u; i++) {
		if (i == 1u && !st.stream_lost)
			continue;                       /* only worth a line when true */
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_LCD
		/* [!] THE PANEL'S PLUGIN NUMBERS, and only when a plugin is what
		 * draws.  Without a line nobody can read them, and counters nobody
		 * reads are counters nobody can hold to a threshold -- which is the
		 * whole of the acceptance criteria this board's README states. */
		if (i == 6u && !nn_active_is_plugin())
			continue;
#else
		if (i == 6u)
			continue;
#endif
		/* Nothing has reached either site yet: no number, so no line. */
		if (i == 5u && st.depth_decode == 0u && st.depth_draw == 0u &&
		    st.depth_shell == 0u)
			continue;
		if (n++ != index)
			continue;
		switch (i) {
		case 0u:
			nn_detail_to(buf, cap, "session : %s",
			             st.holds_guards ? "held (NN + OCTOSPI1)" : "free");
			return 1;
		case 1u:
			nn_detail_to(buf, cap,
			             "stream  : LOST -- re-issue `nn stream start` to "
			             "re-arm");
			return 1;
		case 2u:
			/* The ownership invariant, reported rather than assumed
			   (owhinata/wio-lite-ai#54).  `raced` must be 0; anything else
			   means part of the tensor the model saw was activations. */
			nn_detail_to(buf, cap,
			             "tensor  : %lu raced (must be 0), %lu stale post(s)",
			             (unsigned long)st.raced,
			             (unsigned long)st.stale_posts);
			return 1;
		case 3u:
			nn_detail_to(buf, cap,
			             "ingest  : last %lu us  max %lu us  (band deadline "
			             "~18500 us)",
			             (unsigned long)nn_cyc_to_us(st.ingest_last_cyc),
			             (unsigned long)nn_cyc_to_us(st.ingest_max_cyc));
			return 1;
		case 4u:
			nn_detail_to(buf, cap, "norm    : %s   overlay: %s",
			             st.norm_signed ? "[-1,1]" : "[0,1]",
			             st.overlay ? "on" : "off");
			return 1;
		default:
			/*
			 * How much stack is already spent where a plugin will be CALLED
			 * (issue #108 = #78 Step 3a) -- the term Step 3b's allowances are
			 * computed from, and one nothing else prints: `thread` gives a
			 * PEAK, the deepest a thread ever got anywhere, which is not this
			 * question (issue #101 made exactly that mistake).  Each is printed
			 * over its thread's stack, so the headroom is read, not recalled.
			 * A site that has not run reads 0 -- the draw site runs only while
			 * the preview is on.
			 *
			 * [!] THE SHELL SITE IS HERE BECAUSE FOUR OF THE SEVEN SLOTS ARE
			 * CALLED ON IT (issue #110): entry, shapes_ok, report and the
			 * parameters.  3a measured the other two and board.cmake declared
			 * the WORKER's allowance for these -- a bound on the wrong
			 * thread's stack.
			 */
			nn_detail_to(buf, cap,
			             "at call : %lu/%lu nn_work (decode), %lu/%lu "
			             "cam_prev (draw), %lu/%lu shell (report); high-water",
			             (unsigned long)st.depth_decode,
			             (unsigned long)NNCAM_STACK_BYTES,
			             (unsigned long)st.depth_draw,
			             (unsigned long)CAM_PREVIEW_STACK_BYTES,
			             (unsigned long)st.depth_shell,
			             (unsigned long)CLI_INSTANCE_STACK_SIZE);
			return 1;
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_LCD
		case 6u: {
			uint32_t spent = 0u, refused = 0u, miss = 0u, run = 0u;

			/*
			 * What the loaded decoder cost the panel: the high-water pixel
			 * charge of any one frame against its cap, primitives it refused
			 * for want of budget, and the frames it was not let near at all.
			 *
			 * [!] A RUN OF MISSES IS THE ONE THAT SHOWS.  One skipped overlay
			 * is invisible; a run of them is a panel that has stopped
			 * annotating, and the preview's own counters cannot see it --
			 * they count a frame that was PRESENTED, not one presented bare.
			 */
			if (ended) {
				spent   = fin_spent;
				refused = fin_refused;
				miss    = fin_miss;
				run     = fin_run;
			} else {
				cam_preview_plugin_draw_stats(&spent, &refused);
				plugin_lease_misses(&miss, &run);
			}
			nn_detail_to(buf, cap,
			             "plugin  : drew %lu px max/frame of %lu, %lu "
			             "refused; %lu frame(s) missed (run of %lu)",
			             (unsigned long)spent,
			             (unsigned long)(NN_ACTIVE_FRAME_W *
			                             NN_ACTIVE_FRAME_H / 4u),
			             (unsigned long)refused,
			             (unsigned long)miss, (unsigned long)run);
			return 1;
		}
#endif
		}
	}
	return 0;
}

void nn_svc_norm_set(int signed_range)
{
	nn_camera_set_norm(signed_range);
}

int nn_svc_norm_get(void)
{
	return nn_camera_get_norm();
}

void nn_svc_overlay_set(int on)
{
	nn_camera_set_overlay(on);
}

int nn_svc_overlay_get(void)
{
	return nn_camera_get_overlay();
}

int nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out)
{
	/*
	 * [!] THE IDENTITY, AND THAT IS A FACT ABOUT THIS BOARD.  The band ingest
	 * tiles the WHOLE frame onto the model input -- nn_camera_start() checks
	 * that the tiling covers it exactly -- so the input's normalised space and
	 * the frame's are the same space, and the percentages this board has always
	 * printed were right.  Grove crops the centre square before scaling, so
	 * there they differ; that is why this is a board operation rather than a
	 * multiplication in the shared command.
	 */
	*out = *in;
	return NN_SVC_OK;
}

/*
 * [!] THROUGH THE SHIM, NOT STRAIGHT TO THE RESIDENT DECODER (issue #110).  A
 * loaded plugin carries its OWN threshold; reaching past nn_active would change
 * a firmware number the decoder in force never reads, and `nn thresh 700` would
 * report success while deciding nothing.  This is the divergence an operator
 * meets in the first minute, and the one a differential test that gives both
 * decoders the same threshold cannot see.
 */
int nn_svc_thresh_get(unsigned *milli)
{
	if (milli == NULL)
		return NN_SVC_ERR_ARG;
	*milli = NN_SVC_THRESH_NONE;
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* [!] UNDER THE LEASE (issue #110).  This reaches a plugin's param_get,
	 * which reads plugin state that a decode may be rewriting and that a
	 * concurrent `nn model load` may be REPLACING -- and this command takes no
	 * NN session, so nothing else keeps either out.  If the lease cannot be
	 * had, say there is no answer rather than reading one from a plugin
	 * somebody else is in the middle of.
	 *
	 * [!] AND "NO ANSWER" IS BUSY, NOT "NONE" (issue #122 P6).  Returning
	 * NN_SVC_THRESH_NONE here told the operator the decoder held no threshold
	 * when it merely could not be asked. */
	if (!plugin_lease_take(NN_PLUGIN_LEASE_WAIT_TICKS))
		return NN_SVC_ERR_BUSY;
	*milli = nn_active_get_thresh_milli();
	plugin_lease_give();
	return NN_SVC_OK;
#else
	/* [!] NO PLUGIN MECHANISM AND NO DECODER (issue #116).  The `null` backend
	 * cannot load a container at all, so nothing in this build holds a
	 * threshold -- the same answer the TFLM build gives with none loaded. */
	return NN_SVC_OK;
#endif
}

int nn_svc_thresh_set(unsigned milli)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	int r;

	/* No detail to set: this entry point returns a status only, and the
	 * shared command has a line for BUSY (issue #122 P6 gave it one). */
	if (!plugin_lease_take(NN_PLUGIN_LEASE_WAIT_TICKS))
		return NN_SVC_ERR_BUSY;
	r = nn_active_set_thresh_milli(milli);
	plugin_lease_give();
	switch (r) {
	case NN_ACTIVE_THRESH_OK:
		return NN_SVC_OK;
	case NN_ACTIVE_THRESH_NO_DECODER:
		/* The slot the shared command already has for it: the loaded decoder
		 * holds no threshold, which is not the same as refusing the value. */
		return NN_SVC_ERR_STATE;
	default:
		return NN_SVC_ERR_ARG;
	}
#else
	/* Nothing holds one here either -- and "there is nothing to set" is not
	 * "that value is out of range" (issue #116). */
	(void)milli;
	return NN_SVC_ERR_STATE;
#endif
}

/* One `nn info` line through a bounded formatter.  128 and not 80: Grove's
 * image line once ran past 80 and truncation ate its CRLF, so the next line ran
 * on (issue #103). */
static int nn_info_line(nn_svc_write_fn write, void *ctx, const char *f, ...)
{
	char line[128];
	va_list ap;
	int n;

	va_start(ap, f);
	n = fmt_vsnformat(line, sizeof line, f, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof line)
		n = (int)sizeof line - 1;
	return write(ctx, line, (size_t)n);
}

/*
 * What this board adds: what the open model's container CLAIMS about its plugin
 * (issue #108 = #78 Step 3a), and where a plugin would live.
 *
 * [!] CLAIMS, AND THE LINE SAYS SO.  Every field below comes from a manifest
 * plugin_parse() validated -- structure, target, link address, sizes, digest --
 * which is what the container SAYS about itself.  "not executed" is the only word
 * that describes this board's behaviour, and it is true of every container in
 * Step 3a: the plugin section is never copied and never branched into.
 *
 * The CRC is the identity that moves when the bytes do; the build id is a source
 * revision stamped at configure time and does not.
 *
 * [!] NOTHING IS HELD WHILE THIS PRINTS.  The claims are copied out under a
 * short interrupt-masked section (nn_claims_snapshot) and written afterwards; a
 * console line takes as long as the USB CDC takes.
 */
extern uint8_t __plugin_start[], __plugin_end[];   /* the linker's reservation */

void nn_svc_info_extra(nn_svc_write_fn write, void *ctx)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	struct nn_claims c;
	const struct plugin_view *v = &c.view;
	enum nn_claims_seen seen;
	int running = 0;
	const uint32_t base = (uint32_t)(uintptr_t)__plugin_start;
	const uint32_t size = (uint32_t)(__plugin_end - __plugin_start);

	if (write == NULL)
		return;

	/* One snapshot, and every word below is read from it: re-reading the flag
	 * afterwards could describe a load another console finished in between. */
	seen = nn_claims_snapshot(&c, &running);
	if (seen == NN_CLAIMS_LOADING) {
		(void)nn_info_line(write, ctx,
		                   "plugin  : (a model load is in progress -- ask "
		                   "again)\r\n");
		return;
	}
	if (seen == NN_CLAIMS_NONE || !v->has_plugin) {
		/* Says so rather than leaving the line out: a missing line reads as a
		 * board with no plugin support at all, which is a different and wrong
		 * fact. */
		(void)nn_info_line(write, ctx,
		                   "plugin  : (none%s) -- reservation %lu B at 0x%08lx"
		                   "\r\n",
		                   seen == NN_CLAIMS_VALID ? " in this container" : "",
		                   (unsigned long)size, (unsigned long)base);
		return;
	}
	/*
	 * [!] THE RUNTIME STATUS COMES FROM THE SAME SNAPSHOT AS THE CLAIMS
	 * (issue #110).  Reading it here would be a second question asked later,
	 * and a load landing between the two would print one container's manifest
	 * beside another's -- or beside no plugin at all.  `running` is what this
	 * board's loader actually has branched into; `validated` is a container
	 * whose plugin was refused or never reached -- since issue #116 that means
	 * nothing is reading its model.
	 */
	if (nn_info_line(write, ctx,
	                 "plugin  : %s (build %s, crc %08lx), %s\r\n",
	                 v->name, v->build_id, (unsigned long)v->digest,
	                 running ? "running" : "validated, not loaded") < 0)
		return;
	/* Which model these claims came with -- see struct nn_claims. */
	if (nn_info_line(write, ctx, "  from  : slot %lu, blob '%s'\r\n",
	                 (unsigned long)c.slot, c.model) < 0)
		return;
	if (nn_info_line(write, ctx,
	                 "  image : %lu B file / %lu B mem (code %lu, data %lu, "
	                 "bss %lu), link 0x%08lx\r\n",
	                 (unsigned long)v->file_size, (unsigned long)v->mem_size,
	                 (unsigned long)v->code_len, (unsigned long)v->data_seg_len,
	                 (unsigned long)v->bss_len,
	                 (unsigned long)v->link_addr) < 0)
		return;
	/* What each slot needs at this firmware's c, and what the manifest
	 * declared -- compared with the call-site depths `nn stream stats`
	 * measures.  See svc/plugin_info.h. */
	(void)plugin_info_stack(write, ctx, v, nn_plugin_policy.veneer_cost);
#else
	/* The null backend cannot load a model, so no container can be open. */
	(void)write;
	(void)ctx;
#endif
}

/* There is no separate report call any more (issue #110): a board captures an
 * external decoder's account of its result beside the snapshot that describes
 * it, into the shared command's own buffer.  On this board that capture happens
 * inside nn_camera_decode_get(), under the same lease as the snapshot, and it
 * is a LOADED PLUGIN that produces it -- with none loaded there is no result to
 * describe and the capture states NN_REPORT_NONE. */

