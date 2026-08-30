/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_grove.c
 * @brief   This board behind the shared `nn` command's contract (issue #50).
 *
 * svc/nn_svc.h is what shell/cmds/cmd_nn.c speaks; port/npu is what this board
 * has.  This is the translation, and it is the THICK one of the three adapters
 * because open here is not "parse a model": it brings the NPU up and takes a
 * lease on the external NOR window, and it must keep doing so in that order.
 *
 * [!] THIS FILE OWNS THE STATE THE COMMAND USED TO OWN.  cmds/cmd_nn.c held six
 * statics -- the gate, the open flag, the model's address, length, source label
 * and resolved slot.  They moved HERE rather than into the shared command,
 * because the shared command is audited to own nothing (a static there lands in
 * memory no board placed, and one of the three boards has no residency gate that
 * would notice).  The dependency is one way: this file never reaches back into
 * cmds/, and the command reaches this state only through the queries below.
 *
 * [!] THE DIAGNOSTICS SURVIVED THE MOVE, DELIBERATELY.  A port adapter must not
 * take a shell instance, so the messages this board used to print at the point
 * of failure -- which slot, which CRC, which of two numbers is out of range --
 * would otherwise have flattened into a status code.  They are formatted into a
 * detail buffer here and copied into the result the shared command prints.
 * Losing them would have made every failure read the same.
 */
#include "nn_svc.h"

#include <stdarg.h>
#include <string.h>

#define LOG_TAG "nn"
#include "log.h"

#include "blob.h"
#include "camera.h"
#include "cam_dp.h"
#include "fmt.h"
#include "cam_lcd_sink.h"
#include "camera.h"
#include "nn_decoder.h"
#include "nn_overlay.h"
#include "nn_preproc.h"
#include "nn_stream_state.h"
#include "nor_flash.h"    /* NOR_XIP_BASE */
#include "npu.h"
#include "npu_hw.h"
#include "tx_api.h"       /* tx_time_get() -- ThreadX ticks, 1 ms here */

/* ---- state ---------------------------------------------------------------
 *
 * Plain .bss.  This board's decoder state is already here (nn_decoder.c) for the
 * same reason: nothing on this board needs a placement attribute, because
 * nothing here is touched by a bus master.  The other two boards' adapters own
 * nothing new at all -- they already have a model singleton and a session gate
 * of their own, and a second copy would be a second answer.
 */
static uint8_t  nn_busy;            /**< the transient claim                  */
static uint8_t  nn_owner;           /**< enum nn_owner -- WHO holds it        */
static uint8_t  nn_open_done;       /**< a model is active                    */
static uint32_t nn_model_addr;
static uint32_t nn_model_len;
static char     nn_model_from[BLOB_NAME_MAX + 1];  /**< label, never a key    */
static int      nn_model_slot;      /**< -1 for the raw form                  */

/** The geometry the last capture used.  Kept because a box has to be mapped
 *  back through the SAME transform the overlay draws with, or the console and
 *  the panel disagree about where a face is (issue #48). */
static struct nn_preproc_geom nn_geom;
static uint8_t nn_geom_valid;

/* ---- the transient claim -------------------------------------------------
 *
 * Single-instance, not a mutex: two consoles may both reach `nn`, and what has
 * to be prevented is two jobs inside the NPU at once.  The gate also covers open
 * and close, because a teardown rewrites the hardware state that `info` walks.
 *
 * [!] IT ALSO RECORDS WHO HOLDS IT (issue #99), and that is not the same thing
 * as @ref nn_claim -- which is the CALLER's cleanup authority and stays exactly
 * as it was.  This is internal, and it exists because a stream now holds the
 * claim across commands.  Without it `nn info` would answer "busy" for the whole
 * life of a stream, which is precisely when it is worth asking, and which the
 * other two boards deliberately do not do.  A transient op and a stream are
 * different holders: no unload can begin while the stream holds this, so the
 * identity below is safe to read; an op may be dismantling exactly that.
 */
enum nn_owner {
	NN_OWNER_NONE = 0,
	NN_OWNER_OP,      /**< one operation, which releases before it returns */
	NN_OWNER_STREAM,  /**< a running stream, until its stop               */
};

static int nn_try_acquire(void)
{
	int got;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	got = !nn_busy;
	if (got) {
		nn_busy  = 1u;
		nn_owner = (uint8_t)NN_OWNER_OP;
	}
	TX_RESTORE
	return got;
}

static void nn_release(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nn_owner = (uint8_t)NN_OWNER_NONE;
	nn_busy  = 0u;
	TX_RESTORE
}

/* ---- the stream lifecycle (issue #99) ------------------------------------
 *
 * The machine itself is svc/nn_stream_life.c's, shared with the other two boards
 * -- see there for why one implementation rather than three.  What is here is
 * only this port's critical section around it and the two baselines a poll
 * subtracts.
 */
static struct nn_stream_life nn_life;
static uint32_t nn_stream_frames0;   /**< camera frame count when it started  */
static uint32_t nn_stream_t0;        /**< ticks when it started               */
static uint32_t nn_stream_ms;        /**< frozen elapsed, once it has stopped */

/* Claim IDLE -> STARTING together with the transient claim.  ONE critical
   section, because they are one decision: a start that took the claim and then
   found the lifecycle busy would have to unwind a claim another job may have
   taken in between. */
static enum nn_stream_start_claim nn_stream_begin(void)
{
	enum nn_stream_start_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* The transient claim and the lifecycle are one decision here, so the gate
	   is tested first and reported as BUSY -- another nn job, not a stream. */
	if (nn_busy) {
		r = NN_STREAM_START_BUSY;
	} else {
		r = nn_stream_life_begin(&nn_life);
		if (r == NN_STREAM_START_GO) {
			nn_busy  = 1u;
			nn_owner = (uint8_t)NN_OWNER_STREAM;
		}
	}
	TX_RESTORE
	return r;
}

/* Everything came up: mint the generation and publish the baselines with it. */
static void nn_stream_commit(uint32_t frames0, uint32_t t0, uint32_t *gen)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* [!] Baselines published only once the generation exists, so a refused
	 * commit cannot leave this generation's numbers describing another's. */
	*gen = nn_stream_life_commit(&nn_life);
	if (*gen != NN_STREAM_GEN_ANY) {
		nn_stream_frames0 = frames0;
		nn_stream_t0      = t0;
		nn_stream_ms      = 0u;
	}
	TX_RESTORE
}

/* A start that failed after nn_stream_begin(): nothing is up, so give both the
   lifecycle and the claim back. */
static void nn_stream_abort(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (nn_stream_life_abort(&nn_life)) {
		nn_owner = (uint8_t)NN_OWNER_NONE;
		nn_busy  = 0u;
	}
	TX_RESTORE
}

/* [!] Test and claim in one call, under one critical section -- see the note in
   svc/nn_stream_life.h for the interleaving that separating them admits. */
static enum nn_stream_stop_claim nn_stream_claim_stop(uint32_t gen)
{
	enum nn_stream_stop_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	r = nn_stream_life_claim_stop(&nn_life, gen);
	TX_RESTORE
	return r;
}

/* Both halves confirmed. */
static void nn_stream_finish(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* [!] THE CLAIM IS RELEASED ONLY IF THE TRANSITION HAPPENED.  Clearing it
	 * regardless would hand the NPU back on exactly the invariant failure the
	 * guard exists to catch -- and something may still be inside it. */
	if (nn_stream_life_finish(&nn_life)) {
		nn_stream_ms = (uint32_t)(((uint32_t)tx_time_get() - nn_stream_t0) *
		                          1000u / TX_TIMER_TICKS_PER_SECOND);
		nn_owner = (uint8_t)NN_OWNER_NONE;
		nn_busy  = 0u;
	}
	TX_RESTORE
}

/* Retryable: stoppable again, same generation, claim still held. */
static void nn_stream_unclaim_stop(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nn_stream_life_retry(&nn_life);
	TX_RESTORE
}

/* Unconfirmed: the claim is never given back. */
static void nn_stream_poison(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (nn_stream_life_poison(&nn_life))
		nn_stream_ms = (uint32_t)(((uint32_t)tx_time_get() - nn_stream_t0) *
		                          1000u / TX_TIMER_TICKS_PER_SECOND);
	TX_RESTORE
}

static void nn_detail_to(char *dst, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)fmt_vsnformat(dst, cap, fmt, ap);
	va_end(ap);
}

/* Every failure path writes into the result it is about to return. */
#define nn_detail_set(...) \
	nn_detail_to(res->detail, sizeof res->detail, __VA_ARGS__)
#define nn_detail_clear()  (res->detail[0] = '\0')

/* Fill a result in one place, so no path can set a status and forget the
   disposition -- they are two answers and both are always given. */
/* [!] The detail is COPIED into the caller's result here, at the one place a
 * result is built.  nn_detail is this file's buffer and the next command on
 * another console overwrites it -- so a pointer to it would be printed after it
 * had already become somebody else's sentence. */
static void nn_result(struct nn_op_result *res, int status, enum nn_claim claim)
{
	res->status = status;
	res->claim  = (uint8_t)claim;
}

/* ---- info ---------------------------------------------------------------- */

void nn_svc_info(struct nn_svc_info *out)
{
	uint8_t owner_snap, open_snap;
	char    from_snap[BLOB_NAME_MAX + 1];
	TX_INTERRUPT_SAVE_AREA

	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, "ethos-u55 (tflm, secure)");
	out->arena_bytes = (uint32_t)npu_arena_bytes();

	/*
	 * THE COMMON CASE IS UNCHANGED: when nothing holds the gate, take it and
	 * answer everything, because a teardown running on another job rewrites
	 * exactly what this walks.
	 */
	if (nn_try_acquire()) {
		out->model_active = nn_open_done;
		out->arena_used   = nn_open_done ? (uint32_t)npu_arena_used() : 0u;
		if (nn_open_done) {
			nn_svc_str(out->model, sizeof out->model,
			           (nn_model_from[0] != '\0') ? nn_model_from : "(raw)");
			nn_svc_str(out->source, sizeof out->source,
			           npu_hw_ready() ? "npu up (secure, privileged)"
			                          : "npu down");
		} else {
			nn_svc_str(out->source, sizeof out->source,
			           npu_hw_fail_reason());
		}
		out->avail_identity = (uint8_t)NN_AVAIL_OK;
		out->avail_runtime  = (uint8_t)NN_AVAIL_OK;
		out->avail_tensors  = nn_open_done ? (uint8_t)NN_AVAIL_OK
		                                   : (uint8_t)NN_AVAIL_NA;
		nn_release();
		return;
	}

	/*
	 * [!] HELD -- AND BY WHOM DECIDES HOW MUCH CAN STILL BE SAID (issue #99).
	 *
	 * A stream holds this gate for its whole life, and `nn info` refusing for
	 * all of that is exactly backwards: a running stream is when the report is
	 * worth asking for, and the other two boards answer then.  No unload can be
	 * in flight while a STREAM holds it -- an unload would have to take this
	 * same gate -- so the identity is stable and safe to copy.  An ordinary
	 * operation is the opposite: it may be dismantling that very thing.
	 *
	 * [!] AND THE TEST AND THE COPY ARE ONE CRITICAL SECTION.  Testing "a
	 * stream owns it" and then copying afterwards lets the stream end in
	 * between and an unload race the copy -- the check would be describing a
	 * world that no longer exists by the time it is used.
	 */
	TX_DISABLE
	owner_snap = nn_owner;
	open_snap  = nn_open_done;
	memcpy(from_snap, nn_model_from, sizeof from_snap);
	TX_RESTORE

	if (owner_snap != (uint8_t)NN_OWNER_STREAM) {
		/* An operation has it.  Only what CANNOT be in flight is reported: the
		   arena reservation above is a link-time constant, and nothing else. */
		nn_svc_str(out->source, sizeof out->source,
		           "(busy -- another nn job holds it)");
		out->avail_identity = (uint8_t)NN_AVAIL_WITHHELD;
		out->avail_runtime  = (uint8_t)NN_AVAIL_WITHHELD;
		out->avail_tensors  = (uint8_t)NN_AVAIL_WITHHELD;
		return;
	}

	out->model_active   = open_snap;
	out->avail_identity = (uint8_t)NN_AVAIL_OK;
	if (open_snap)
		nn_svc_str(out->model, sizeof out->model,
		           (from_snap[0] != '\0') ? from_snap : "(raw)");
	nn_svc_str(out->source, sizeof out->source, "streaming (`nn stream stats`)");
	/*
	 * The rest needs the gate the stream is holding -- and would be misleading
	 * anyway: the arena is being rewritten every frame, so a tensor's CONTENTS
	 * are not a thing to report while this runs.  That is the line svc/nn_svc.h
	 * already draws between a descriptor and its buffer.
	 */
	out->avail_runtime = (uint8_t)NN_AVAIL_WITHHELD;
	out->avail_tensors = (uint8_t)NN_AVAIL_WITHHELD;
}

/* ---- resolving a model in the asset store -------------------------------- */

/*
 * Every slot, under the caller's lease.
 *
 * [!] IT REFUSES ON THE FIRST SLOT IT CANNOT READ rather than resolving from
 * what it got.  A scan with a hole cannot say a name is unique, and "not found"
 * is exactly the wrong answer to give about a slot nobody looked at.
 */
static int nn_scan_slots(struct nn_op_result *res, uint32_t token,
                         const char *name, struct blob_slot_view *v,
                         unsigned *count)
{
	unsigned n = blob_map_count(), i;

	if (n == 0u || n > BLOB_MAX_SLOTS) {
		nn_detail_set("the slot table has %u slots, which this build cannot "
		              "scan", n);
		return -1;
	}
	for (i = 0u; i < n; i++) {
		struct blob_info info;
		int rc = blob_stat_leased(i, token, &info, NULL);

		if (rc != BLOB_OK) {
			nn_detail_set("slot %u unreadable (%s)", i,
			              rc == BLOB_ERR_FAULT ? "the NOR port is faulted; "
			                                     "a reset is required"
			              : rc == BLOB_ERR_BUSY ? "the flash is busy"
			              : rc == BLOB_ERR_MAP  ? "the slot table does not "
			                                      "fit the writable interval"
			                                    : "bad slot");
			return -1;
		}
		v[i].state      = info.state;
		v[i].name_match = (uint8_t)(info.state == BLOB_VALID &&
		                            strcmp(info.name, name) == 0);
	}
	*count = n;
	return 0;
}

/*
 * A blob name -> the address and length npu_open() will be given.
 *
 * [!] THE ORDER IS THE POINT (issue #93): the caller already holds the NPU lease
 * -> scan every slot -> resolve the name -> CHECK THE PAYLOAD'S CRC -> take the
 * length from the header that check ran against -> hand it to npu_open(), with
 * the lease never dropped in between.  BLOB_VALID is a statement about the
 * HEADER only; and blob_stat()'s lease is taken and returned per slot, so
 * resolving through it would leave a gap in which a background `blob write`
 * could replace the very slot that was chosen.
 */
static int nn_resolve_blob(struct nn_op_result *res, uint32_t token,
                           const char *name, uint32_t *addr, uint32_t *len)
{
	struct blob_slot_view v[BLOB_MAX_SLOTS];
	struct blob_info info;
	unsigned count = 0u, slot = 0u;
	uint32_t computed = 0u, payload = 0u;
	enum blob_lookup found;
	int rc;

	if (nn_scan_slots(res, token, name, v, &count) != 0)
		return -1;

	found = blob_resolve_name(v, count, &slot);
	if (found != BLOB_LOOKUP_FOUND) {
		nn_detail_set("'%s': %s%s", name, blob_lookup_name(found),
		              found == BLOB_LOOKUP_NONE
		                  ? " (blob list shows what is there)" : "");
		return -1;
	}

	/* [!] The CRC is checked with the same lease still out: the payload is
	 * read where it lies for as long as the model is open, so what is
	 * established is not "these bytes were right once" but "these bytes are
	 * right and cannot change while I hold this". */
	rc = blob_verify_leased(slot, token, &info, &computed);
	if (rc != BLOB_OK) {
		if (rc == BLOB_ERR_CRC)
			nn_detail_set("slot %u ('%s') fails its CRC (stored %08lx, "
			              "flash %08lx); refusing to parse it", slot, name,
			              (unsigned long)info.crc32,
			              (unsigned long)computed);
		else
			nn_detail_set("slot %u ('%s') could not be verified (%d)", slot,
			              name, rc);
		return -1;
	}

	/* The length comes from the header the CRC was compared under, not from a
	   second stat: those two could disagree. */
	payload = blob_payload_addr(slot);
	if (payload == 0u || info.length == 0u) {
		nn_detail_set("slot %u ('%s') has no payload to parse", slot, name);
		return -1;
	}
	*addr = NOR_XIP_BASE + payload;
	*len  = info.length;
	nn_model_slot = (int)slot;
	return 0;
}

/* ---- model lifecycle ----------------------------------------------------- */

void nn_svc_model_load(const struct nn_spec *spec, nn_svc_read_fn read,
                       void *ctx, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	uint32_t addr = 0u, len = 0u;
	const char *name = NULL;
	int rc;

	/* This board's models come from the asset store or a raw window; it never
	   reads a file, so the reader is not used. */
	(void)read;
	(void)ctx;

	nn_detail_clear();
	*state = NN_MODEL_EMPTY;

	/*
	 * [!] THE TAG IS REFUSED BEFORE ANYTHING IS ACQUIRED.  A source this board
	 * does not have is not a hardware failure and must not cost a bring-up --
	 * and the caller is told it holds nothing.
	 */
	switch (spec->tag) {
	case NN_SPEC_NAME:
		if (blob_name_check(spec->name, NULL) != BLOB_NAME_OK) {
			nn_detail_set("'%s' is not a blob name (%s)", spec->name,
			              blob_name_verdict_name(
			                      blob_name_check(spec->name, NULL)));
			*state = nn_open_done ? NN_MODEL_PREVIOUS : NN_MODEL_EMPTY;
			nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
			return;
		}
		name = spec->name;
		break;
	case NN_SPEC_ADDR:
		addr = spec->addr;
		len  = spec->len;
		/* Refused in this board's own words rather than as a bare error from
		 * three layers down, because here the operator can see WHICH of the
		 * two numbers is wrong. */
		if (len < npu_model_len_min() || len > npu_model_len_max(addr)) {
			nn_detail_set("length %lu is not between %lu and %lu for 0x%08lx",
			              (unsigned long)len,
			              (unsigned long)npu_model_len_min(),
			              (unsigned long)npu_model_len_max(addr),
			              (unsigned long)addr);
			*state = nn_open_done ? NN_MODEL_PREVIOUS : NN_MODEL_EMPTY;
			nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
			return;
		}
		break;
	default:
		nn_detail_set("this board loads a model by name or by --addr; it has "
		              "no %s",
		              spec->tag == NN_SPEC_SLOT    ? "slot index"
		              : spec->tag == NN_SPEC_PATH  ? "filesystem"
		              : spec->tag == NN_SPEC_BUILTIN ? "built-in model"
		                                             : "such source");
		*state = nn_open_done ? NN_MODEL_PREVIOUS : NN_MODEL_EMPTY;
		nn_result(res, NN_SVC_ERR_SPEC, NN_CLAIM_NONE);
		return;
	}

	if (!nn_try_acquire()) {
		*state = nn_open_done ? NN_MODEL_PREVIOUS : NN_MODEL_EMPTY;
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (nn_open_done) {
		nn_detail_set("a model is already open -- `nn model unload` first");
		*state = NN_MODEL_PREVIOUS;
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	/*
	 * [!] THE BRING-UP COMES BEFORE THE LOOKUP, and the whole load is built
	 * around that ordering (issue #93).  npu_hw_init() takes the flash reader
	 * lease, so from here to the end of the model's life the window is up and
	 * no writer can take the part.  Resolving a name first and bringing the
	 * hardware up afterwards would put a gap between the answer and the parse
	 * -- and the answer is an ADDRESS.
	 */
	if (npu_hw_init() != 0) {
		nn_detail_set("%s", npu_hw_fail_reason() ? npu_hw_fail_reason()
		                                         : "bring-up failed");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	nn_model_slot = -1;
	if (name != NULL && nn_resolve_blob(res, npu_hw_flash_lease(), name,
	                                    &addr, &len) != 0) {
		/* Every failure from here leaves the hardware DOWN: an NPU that is up
		 * with no model is a state nothing would use, and it would hold the
		 * flash lease against `blob write` for as long as it lasted. */
		npu_hw_deinit();
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	rc = npu_open(addr, len, npu_arena_base(), npu_arena_bytes());
	if (rc != NPU_OK) {
		nn_detail_set("%s (0x%08lx, %lu B)", npu_status_name(rc),
		              (unsigned long)addr, (unsigned long)len);
		npu_hw_deinit();
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	nn_open_done  = 1u;
	nn_model_addr = addr;
	nn_model_len  = len;
	nn_geom_valid = 0u;
	if (name != NULL) {
		(void)strncpy(nn_model_from, name, sizeof nn_model_from - 1u);
		nn_model_from[sizeof nn_model_from - 1u] = '\0';
	} else {
		nn_model_from[0] = '\0';
	}

	*state = NN_MODEL_NEW;
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_release();
}

void nn_svc_model_unload(struct nn_op_result *res)
{
	nn_detail_clear();

	if (!nn_try_acquire()) {
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* Idempotent: unloading nothing succeeds.  Order is model -> NPU -> lease,
	   and npu_hw_deinit() is what returns the flash lease. */
	npu_close();
	npu_hw_deinit();
	nn_open_done     = 0u;
	nn_model_addr    = 0u;
	nn_model_len     = 0u;
	nn_model_slot    = -1;
	nn_model_from[0] = '\0';
	nn_geom_valid    = 0u;
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_release();
}

/* ---- tensors ------------------------------------------------------------- */

int nn_svc_tensors_pin(void)
{
	if (!nn_try_acquire())
		return NN_SVC_ERR_BUSY;
	if (!nn_open_done) {
		nn_release();
		return NN_SVC_ERR_STATE;
	}
	return NN_SVC_OK;
}

void nn_svc_tensors_unpin(void)
{
	nn_release();
}

int nn_svc_output_count(void)
{
	if (!nn_open_done)
		return NN_SVC_ERR_STATE;
	return (int)npu_output_count();
}

int nn_svc_output(unsigned index, struct tensor_desc *out)
{
	struct npu_tensor t;

	if (!nn_open_done)
		return NN_SVC_ERR_STATE;
	if (npu_output(index, &t) != NPU_OK)
		return NN_SVC_ERR_ARG;
	nn_decoder_desc(out, &t);
	return NN_SVC_OK;
}

int nn_svc_input(struct tensor_desc *out)
{
	struct npu_tensor t;

	if (!nn_open_done)
		return NN_SVC_ERR_STATE;
	if (npu_input(&t) != NPU_OK)
		return NN_SVC_ERR_ARG;
	nn_decoder_desc(out, &t);
	return NN_SVC_OK;
}

/* ---- one shot ------------------------------------------------------------ */

/*
 * Does the model's input quantisation match what the preprocessor produces?
 *
 * [!] NOT A FORMALITY.  nn_preproc_fill() writes `pixel - 128` -- a fixed shift,
 * not a quantisation using the tensor's parameters -- which is exactly right for
 * scale 1/255 with zero point -128 and progressively wrong for anything else.
 * Being wrong here does not look like an error: the boxes are still boxes, just
 * in the wrong places, on an image nobody can see.  So it refuses rather than
 * warns.  Compared in millionths so no float formatting is needed.
 */
static int nn_input_quant_ok(struct nn_op_result *res,
                             const struct npu_tensor *in)
{
	long micro = (long)(in->scale * 1000000.0f + 0.5f);

	if (!npu_tensor_is_int8(in->type)) {
		nn_detail_set("detection needs an int8 input, this model has %s",
		              npu_type_name(in->type));
		return -1;
	}
	if (in->zero_point != -128 || micro < 3882 || micro > 3961) {
		nn_detail_set("this model wants scale %ld/1e6 zp %ld, but the frame is "
		              "filled as (pixel - 128), which is scale 3922/1e6 zp -128",
		              micro, (long)in->zero_point);
		return -1;
	}
	return 0;
}

static int nn_fill_input(struct nn_op_result *res, const uint8_t *raw,
                         const struct npu_tensor *in)
{
	uint32_t w, h;

	if (in->rank != 4 || in->dims[3] != 3) {
		nn_detail_set("model input is not HxWx3 (rank %u)", in->rank);
		return -1;
	}
	h = (uint32_t)in->dims[1];
	w = (uint32_t)in->dims[2];

	if (nn_preproc_geom(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, w, h, &nn_geom) != 0) {
		nn_detail_set("cannot fit a %lux%lu input to a %ux%u frame",
		              (unsigned long)w, (unsigned long)h,
		              CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);
		return -1;
	}
	if (in->bytes < (size_t)w * h * 3u) {
		nn_detail_set("input tensor is shorter than its own shape");
		return -1;
	}
	if (nn_preproc_fill(raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, &nn_geom,
	                    (uint8_t *)in->data) != 0) {
		nn_detail_set("preprocessing refused the frame");
		return -1;
	}
	nn_geom_valid = 1u;
	return 0;
}

/*
 * Decode whatever the outputs currently hold.
 *
 * [!] A NEGATIVE RETURN IS PUBLISHED AS IT IS, not folded into zero faces
 * (issue #57) and not folded into one code (issue #97): BF_ERR_MODEL is the one
 * the operator caused -- the open model is not a detector -- and the shared
 * command uses exactly that to fall back to reporting the top classes instead.
 * The others mean this firmware is wired wrong and must not read as either.
 */
static void nn_decode_into(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max)
{
	struct npu_tensor outs[NN_DECODER_MAX_OUTPUTS];
	struct bf_result bfr;
	unsigned n_out, i;
	int nd;

	memset(&bfr, 0, sizeof bfr);
	n_out = npu_output_count();
	if (n_out > NN_DECODER_MAX_OUTPUTS)
		n_out = NN_DECODER_MAX_OUTPUTS;
	for (i = 0u; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			snap->valid = 1;
			snap->ndet  = BF_ERR_ARG;
			snap->res   = bfr;
			return;
		}

	nd = nn_decoder_run(outs, n_out, dets, max, &bfr);
	snap->valid = 1;
	snap->ndet  = nd;
	snap->res   = bfr;
}

void nn_svc_run_once(struct nn_det_snapshot *snap, struct bf_det *dets, int max,
                     nn_svc_cancel_fn cancel, void *ctx,
                     struct nn_op_result *res)
{
	struct npu_tensor in;
	int rc;

	nn_detail_clear();
	/* Nothing here waits long enough to poll: camera_capture() and npu_invoke()
	   are each one blocking call into hardware.  Checked once so a Ctrl+C that
	   arrived before the work starts is still honoured. */
	if (nn_svc_cancelled(cancel, ctx)) {
		nn_result(res, NN_SVC_ERR_CANCEL, NN_CLAIM_NONE);
		return;
	}

	if (!nn_try_acquire()) {
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (!nn_open_done) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_release();
		return;
	}
	if (npu_input(&in) != NPU_OK) {
		nn_detail_set("the model has no input tensor");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	/*
	 * camera_capture() quiesces the datapath on BOTH the success and the
	 * failure path, and it refuses while a preview is running -- the
	 * camera/NPU concurrency question answered by the layer that owns it.
	 * That is why this is one hook and not "is it streaming?" then a capture:
	 * the sensor bus owner is decided under the camera API mutex, so an
	 * answer taken before it is stale before it is used (issue #77).
	 */
	rc = camera_capture();
	if (rc != 0) {
		nn_detail_set("capture failed (%d)", rc);
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_release();
		return;
	}
	if (nn_fill_input(res, camera_raw_frame(), &in) != 0) {
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	/* No cache maintenance here (issue #46): it lives in the port's own
	 * lifecycle callbacks, at the only two instants that are correct. */
	if (npu_invoke() != NPU_OK) {
		nn_detail_set("inference failed");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	/* The quantisation check happens only once there is something to decode:
	 * a classifier is a legitimate model here and must not be refused for not
	 * being a detector. */
	if (nn_input_quant_ok(res, &in) == 0) {
		nn_decode_into(snap, dets, max);
	} else {
		/* Not a detector's input -- say so through the decoder's own vocabulary
		 * so the shared command falls back to reporting classes. */
		snap->valid = 1;
		snap->ndet  = BF_ERR_MODEL;
		memset(&snap->res, 0, sizeof snap->res);
		nn_detail_clear();
	}

	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_release();
}

void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max, struct nn_op_result *res)
{
	nn_detail_clear();

	if (!nn_try_acquire()) {
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (!nn_open_done) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_release();
		return;
	}
	nn_decode_into(snap, dets, max);
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_release();
}

/* ---- bench --------------------------------------------------------------- */

void nn_svc_bench_prepare(struct nn_op_result *res)
{
	struct npu_tensor in;

	nn_detail_clear();

	if (!nn_try_acquire()) {
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (!nn_open_done || npu_input(&in) != NPU_OK) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_release();
		return;
	}
	/* A fixed pattern so every run measures the same work.  The value does not
	   matter; that there IS one does. */
	if (in.data != NULL)
		memset(in.data, 0x5A, in.bytes);
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_release();
}

void nn_svc_bench_run(uint32_t iters, struct nn_bench_stats *out,
                      nn_svc_cancel_fn cancel, void *ctx,
                      struct nn_op_result *res)
{
	uint32_t i;

	nn_detail_clear();
	memset(out, 0, sizeof *out);
	out->min_us = 0xFFFFFFFFu;

	if (!nn_try_acquire()) {
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (!nn_open_done) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	for (i = 0u; i < iters; i++) {
		uint32_t t0, t1, us;

		if (nn_svc_cancelled(cancel, ctx)) {
			nn_detail_set("cancelled after %lu of %lu run(s)",
			              (unsigned long)i, (unsigned long)iters);
			nn_result(res, NN_SVC_ERR_CANCEL, NN_CLAIM_NONE);
			nn_release();
			return;
		}
		t0 = (uint32_t)tx_time_get();
		if (npu_invoke() != NPU_OK) {
			nn_detail_set("inference failed on run %lu of %lu",
			              (unsigned long)i + 1u, (unsigned long)iters);
			nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
			nn_release();
			return;
		}
		t1 = (uint32_t)tx_time_get();

		/*
		 * [!] TICKS ARE 1 ms HERE, so a single inference resolves to whole
		 * milliseconds and the spread is coarse.  That is a property of this
		 * board's time source rather than of the measurement, and it is why
		 * the unit crossing the contract is microseconds: converting up is
		 * lossless, and only this file knows what a tick is worth.
		 */
		us = (t1 - t0) * 1000u;
		out->total_us += us;
		if (us < out->min_us)
			out->min_us = us;
		if (us > out->max_us)
			out->max_us = us;
		out->runs++;
	}

	if (out->runs == 0u)
		out->min_us = 0u;
	else
		out->avg_us = (uint32_t)(out->total_us / out->runs);
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_release();
}

/* ---- boxes and threshold ------------------------------------------------- */

int nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out)
{
	struct nn_preproc_box box;

	if (!nn_geom_valid)
		return NN_SVC_ERR_STATE;

	/* [!] Through nn_preproc_box(), which is the SAME transform the overlay
	 * draws with (issue #48).  This board shows the model the centre square of
	 * the frame, so scaling by 100 would put every box somewhere else -- and
	 * the console would disagree with the panel about where a face is. */
	if (nn_preproc_box(&nn_geom, in->x, in->y, in->w, in->h, &box) != 0)
		return NN_SVC_ERR_ARG;

	out->x     = (float)box.x0 / (float)CAM_FRAME_WIDTH;
	out->y     = (float)box.y0 / (float)CAM_FRAME_HEIGHT;
	out->w     = (float)(box.x1 - box.x0) / (float)CAM_FRAME_WIDTH;
	out->h     = (float)(box.y1 - box.y0) / (float)CAM_FRAME_HEIGHT;
	out->score = in->score;
	return NN_SVC_OK;
}

/* ---- live inference (issue #99) ------------------------------------------
 *
 * The work itself runs on the CAMERA PRODUCER THREAD, inside the panel sink's
 * consume() (nn_overlay.c).  What is here only starts it, reports on it and
 * stops it -- and, unlike the command file this replaces, it holds no shell
 * instance, because a port may not.  Everything that needs to print, wait or
 * notice Ctrl+C is the shared command's, above svc/nn_svc.h.
 */

/* [!] The table in nn_stream_state.c decides about NUMBERS, and it is compiled
 * on the host where camera.h cannot be included.  These are what tie the two
 * together: if the camera ever renumbers a code, this fails to build here rather
 * than leaving the table deciding about values nobody returns. */
_Static_assert(NN_STREAM_CAM_OK      == CAM_OK,          "CAM_OK moved");
_Static_assert(NN_STREAM_CAM_TIMEOUT == CAM_ERR_TIMEOUT, "CAM_ERR_TIMEOUT moved");
_Static_assert(NN_STREAM_CAM_STATE   == CAM_ERR_STATE,   "CAM_ERR_STATE moved");
_Static_assert(NN_STREAM_CAM_BUSY    == CAM_ERR_BUSY,    "CAM_ERR_BUSY moved");
_Static_assert(NN_STREAM_CAM_LOCKED  == CAM_ERR_LOCKED,  "CAM_ERR_LOCKED moved");

/*
 * Would this model actually annotate frames?
 *
 * [!] SETTLED BEFORE THE STREAM STARTS, where refusing costs nothing and can
 * say why.  A stream that starts and then fails on every frame is a panel
 * showing a live picture with no boxes and no explanation -- the exact failure
 * live inference exists to make visible.
 */
static int nn_detector_ready(struct nn_op_result *res)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_DECODER_MAX_OUTPUTS];
	unsigned n_out, i;

	if (npu_input(&in) != NPU_OK) {
		nn_detail_set("the model has no input tensor");
		return -1;
	}
	if (nn_input_quant_ok(res, &in) != 0)
		return -1;
	n_out = npu_output_count();
	if (n_out > NN_DECODER_MAX_OUTPUTS) {
		nn_detail_set("the model has %u outputs and this path reads %u",
		              n_out, (unsigned)NN_DECODER_MAX_OUTPUTS);
		return -1;
	}
	for (i = 0u; i < n_out; i++) {
		if (npu_output(i, &outs[i]) != NPU_OK) {
			nn_detail_set("output %u is unreadable", i);
			return -1;
		}
	}
	if (!nn_decoder_shapes_ok(outs, n_out)) {
		nn_detail_set("the loaded model is not BlazeFace-shaped");
		return -1;
	}
	return 0;
}

void nn_svc_stream_start(const struct nn_stream_spec *spec,
                         struct nn_op_result *res, uint32_t *gen)
{
	struct camera_stats cs;
	int rc;

	if (res == NULL)
		return;
	nn_detail_clear();
	if (spec == NULL || gen == NULL) {
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}
	if (spec->test) {
		/* No sensor test pattern on this board.  Refused before anything is
		   acquired, so nothing has to be unwound. */
		nn_detail_set("this board has no test pattern to stream");
		nn_result(res, NN_SVC_ERR_SPEC, NN_CLAIM_NONE);
		return;
	}

	switch (nn_stream_begin()) {
	case NN_STREAM_START_GO:
		break;
	case NN_STREAM_START_RUNNING:
		nn_detail_set("a stream is already running (`nn stream stats`)");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a reboot "
		              "clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_START_BUSY:
	default:
		nn_detail_set("busy -- another nn job, or a start or stop, holds it");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* From here every failure gives BOTH the lifecycle and the claim back. */
	if (!nn_open_done) {
		nn_detail_set("no model is loaded -- `nn model load --name det`");
		nn_stream_abort();
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	if (nn_detector_ready(res) != 0) {
		nn_stream_abort();
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] ONE CALL, AND THEREFORE ONE FAILURE (issue #63).  Attaching the sink
	 * and starting the stream used to be two steps, and a start that came back
	 * BUSY meant a stream was already running WITH THIS SINK ATTACHED -- a
	 * producer could be inside consume(), so the sink could not be detached and
	 * the NPU could not be released.  The camera does both under its API mutex,
	 * so a failure here means nothing was attached and nothing started.
	 */
	rc = cam_lcd_sink_attach_and_stream(nn_overlay_arm());
	if (rc != CAM_OK) {
		if (rc == CAM_ERR_BUSY)
			nn_detail_set("the camera is already streaming, or another "
			              "command owns it");
		else
			nn_detail_set("the camera would not start (%d)", rc);
		nn_stream_abort();
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		return;
	}

	camera_stream_stats(&cs);
	nn_stream_commit(cs.frames, (uint32_t)tx_time_get(), gen);
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
	nn_detail_set("inference stream started -- boxes are on the panel");
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

int nn_svc_stream_poll(uint32_t gen, struct nn_stream_stats *out)
{
	struct camera_stats cs;
	struct nn_overlay_stats os;
	uint32_t seq0, seq1, g, frames0, t0, ms;
	uint8_t  phase;
	TX_INTERRUPT_SAVE_AREA

	if (out == NULL)
		return NN_SVC_ERR_ARG;

	/* Phase 1: identity and baselines. */
	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, &g, &phase, &seq0);
	frames0 = nn_stream_frames0;
	t0      = nn_stream_t0;
	ms      = nn_stream_ms;
	TX_RESTORE

	if (g == NN_STREAM_GEN_ANY)
		return NN_SVC_ERR_STATE;                  /* nothing has ever run */
	if (gen != NN_STREAM_GEN_ANY && gen != g)
		return NN_SVC_ERR_GEN;

	/*
	 * [!] PHASE 2 IS OUTSIDE THE CRITICAL SECTION, AND IT HAS TO BE.
	 * camera_stream_stats() ends in the frame pipeline's mutex; waiting for a
	 * mutex with interrupts disabled is a deadlock, not a slow path.
	 */
	camera_stream_stats(&cs);
	nn_overlay_stats(&os);

	/*
	 * Phase 3: accept only if nothing moved.  The counter, not the generation
	 * and the state -- a retryable stop returns to both of those unchanged.
	 */
	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, NULL, NULL, &seq1);
	TX_RESTORE
	if (seq1 != seq0)
		return NN_SVC_ERR_STALE;

	memset(out, 0, sizeof *out);
	out->running        = (phase == (uint8_t)NN_STREAM_PHASE_RUNNING) ? 1u : 0u;
	out->frames         = cs.frames - frames0;
	out->skipped        = os.skipped;
	out->infers         = os.inferences;
	out->errors         = os.errors;
	out->model_errors   = os.model_errors;
	out->decoder_errors = os.decoder_errors;
	out->last_us        = os.last_ms * 1000u;
	out->elapsed_ms     = out->running
	                    ? (uint32_t)(((uint32_t)tx_time_get() - t0) * 1000u /
	                                 TX_TIMER_TICKS_PER_SECOND)
	                    : ms;
	/* [!] Nothing decoded yet is not "decoded nobody". */
	out->last_valid = (os.inferences != 0u) ? 1u : 0u;
	out->last_ndet  = (int32_t)os.last_ndet;
	return NN_SVC_OK;
}

/* The sentence an operator gets.  They are not interchangeable -- two of these
   mean "nothing was touched" and "something is still running in there". */
static const char *nn_stream_why_text(unsigned char why)
{
	switch ((enum nn_stream_why)why) {
	case NN_STREAM_WHY_CAM_LOCKED:
		return "the camera API stayed locked, so the stop was never requested "
		       "and nothing was touched -- run `nn stream stop` again";
	case NN_STREAM_WHY_CAM_LOST:
		return "the producer never acknowledged the stop; the camera is "
		       "unusable until reboot";
	case NN_STREAM_WHY_CAM_STATE:
		return "the camera refused the stop; it is unusable until reboot";
	case NN_STREAM_WHY_SINK_BUSY:
		return "the panel has not finished with this stream's frames -- run "
		       "`nn stream stop` again";
	case NN_STREAM_WHY_SINK_LOST:
		return "the panel thread did not finish; the preview is unusable "
		       "until reboot";
	case NN_STREAM_WHY_OK:
	default:
		return "stopped";
	}
}

void nn_svc_stream_stop(uint32_t gen, struct nn_op_result *res)
{
	struct nn_stream_verdict v;
	int cam_rc, detach_rc = 0, attempted = 0;

	if (res == NULL)
		return;
	nn_detail_clear();

	switch (nn_stream_claim_stop(gen)) {
	case NN_STREAM_STOP_GO:
		break;
	case NN_STREAM_STOP_IDLE:
		nn_detail_set("no stream is running");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_WRONG_GEN:
		nn_detail_set("that stream has already been replaced by another");
		nn_result(res, NN_SVC_ERR_GEN, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a reboot "
		              "clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_STOP_BUSY:
	default:
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
	}

	/* [!] BEFORE the camera stop, always: it is what keeps the frame in flight
	 * from starting an inference the join would then have to wait out. */
	nn_overlay_request_stop();
	cam_rc = camera_stream_stop();

	/* [!] AND THE DETACH IS THE SECOND HALF OF THE STOP (issue #57), reached
	 * only on a confirmed producer stop -- the blit runs on the panel thread,
	 * so a confirmed stop alone does not prove nothing is using the frame. */
	if (nn_stream_may_detach(cam_rc)) {
		attempted = 1;
		detach_rc = cam_lcd_sink_detach();
	}
	nn_stream_stop_decide(cam_rc, attempted, detach_rc, &v);

	switch ((enum nn_stream_act)v.act) {
	case NN_STREAM_ACT_DONE:
		nn_stream_finish();
		nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
		return;
	case NN_STREAM_ACT_RETRY:
		nn_stream_unclaim_stop();
		nn_detail_set("%s", nn_stream_why_text(v.why));
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_RETRYABLE);
		return;
	case NN_STREAM_ACT_TERMINAL:
	default:
		nn_stream_poison();
		nn_detail_set("%s", nn_stream_why_text(v.why));
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}
}

int nn_svc_stream_lines(enum nn_stream_lines_ctx ctx, unsigned index,
                        char *buf, size_t cap)
{
	struct nn_overlay_stats os;

	if (buf == NULL || cap == 0u)
		return NN_SVC_ERR_ARG;
	buf[0] = '\0';
	if (ctx != NN_STREAM_LINES_STATS)
		return 0;                        /* nothing to add at start */

	nn_overlay_stats(&os);
	switch (index) {
	case 0u:
		nn_detail_to(buf, cap, "faces   : %lu drawn since the stream started",
		             (unsigned long)os.detections);
		return 1;
	case 1u:
		/* The producer-side split (issue #60).  Only when the clock behind it
		   is trusted -- an untrusted number here would be read as a
		   measurement. */
		if (!os.prof_ok || os.prof_frames == 0u)
			return 0;
		nn_detail_to(buf, cap,
		             "producer: %lu us prep, %lu us invoke, %lu us decode "
		             "(%lu frames)",
		             (unsigned long)(os.prep_us / os.prof_frames),
		             (unsigned long)(os.invoke_us / os.prof_frames),
		             (unsigned long)(os.decode_us / os.prof_frames),
		             (unsigned long)os.prof_frames);
		return 1;
	default:
		return 0;
	}
}

unsigned nn_svc_thresh_get(void)
{
	return nn_decoder_get_thresh_milli();
}

int nn_svc_thresh_set(unsigned milli)
{
	return (nn_decoder_set_thresh_milli(milli) == BF_OK) ? NN_SVC_OK
	                                                     : NN_SVC_ERR_ARG;
}

/* ---- `nn info` extras (issue #101) --------------------------------------- */

/*
 * The plugin reservation, from the linker.  Declared as arrays so a bare
 * reference is already the address.
 */
extern uint8_t __plugin_start[], __plugin_end[];

/*
 * A bounded line builder.  The writer is length-bearing, so this port formats
 * its own text and hands over the length -- no formatter crosses the boundary,
 * which is what keeps a %f out of three firmwares.
 *
 * fmt_vsnformat() is svc/fmt.c's bounded formatter, the same one cli_print uses
 * underneath, so what appears here and what the shell prints elsewhere are
 * formatted by one implementation.
 */
static int nn_info_line(nn_svc_write_fn write, void *ctx, const char *f, ...)
{
	char line[80];
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
 * What this board adds: where a loaded plugin would live, and whether one is
 * there.
 *
 * [!] NOTHING IS HELD WHILE THIS PRINTS.  The reservation's bounds are linker
 * constants and need no claim at all; when Step 1b has a manifest to report it
 * must SNAPSHOT the fields under its claim, release, and only then write.  A
 * console line takes as long as a UART takes, and holding the inference gate
 * across one would stall the camera producer for exactly that long.
 */
void nn_svc_info_extra(nn_svc_write_fn write, void *ctx)
{
	uint32_t base = (uint32_t)(uintptr_t)__plugin_start;
	uint32_t size = (uint32_t)(__plugin_end - __plugin_start);

	if (write == NULL)
		return;

	/* Step 1a loads nothing, and says so rather than leaving the line out: a
	 * missing line reads as a board with no plugin support at all, which is a
	 * different and wrong fact. */
	(void)nn_info_line(write, ctx,
	                   "plugin  : (none) -- reservation %lu B at 0x%08lx\r\n",
	                   (unsigned long)size, (unsigned long)base);
}
