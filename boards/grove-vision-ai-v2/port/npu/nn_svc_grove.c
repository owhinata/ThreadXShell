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
 * detail buffer here and the shared command prints them through
 * nn_svc_strerror().  Losing them would have made every failure read the same.
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
#include "nn_decoder.h"
#include "nn_svc_grove.h"
#include "nn_preproc.h"
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

/** Why the last operation failed, in this board's own words. */
static char nn_detail[160];

/* ---- the transient claim -------------------------------------------------
 *
 * Single-instance, not a mutex: two consoles may both reach `nn`, and what has
 * to be prevented is two jobs inside the NPU at once.  The gate also covers open
 * and close, because a teardown rewrites the hardware state that `info` walks.
 */
static int nn_try_acquire(void)
{
	int got;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	got = !nn_busy;
	if (got)
		nn_busy = 1u;
	TX_RESTORE
	return got;
}

static void nn_release(void)
{
	nn_busy = 0u;
}

static void nn_detail_clear(void)
{
	nn_detail[0] = '\0';
}

static void nn_detail_set(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)fmt_vsnformat(nn_detail, sizeof nn_detail, fmt, ap);
	va_end(ap);
}

/* Fill a result in one place, so no path can set a status and forget the
   disposition -- they are two answers and both are always given. */
static void nn_result(struct nn_op_result *res, int status, enum nn_claim claim)
{
	res->status = status;
	res->claim  = (uint8_t)claim;
}

const char *nn_svc_strerror(int status)
{
	(void)status;
	return (nn_detail[0] != '\0') ? nn_detail : NULL;
}

/* ---- info ---------------------------------------------------------------- */

void nn_svc_info(struct nn_svc_info *out)
{
	out->backend      = "ethos-u55 (tflm, secure)";
	out->arena_bytes  = (uint32_t)npu_arena_bytes();

	/*
	 * [!] BEHIND THE GATE, and when the gate refuses, only facts that CANNOT
	 * be in flight are reported (issue #45).  The arena reservation is a
	 * link-time constant; whether a model is open is not, and a teardown
	 * running on another job rewrites exactly what this would walk.
	 */
	if (!nn_try_acquire()) {
		out->model_active = 0u;
		out->model        = NULL;
		out->source       = "(busy -- another nn job holds it)";
		return;
	}

	out->model_active = nn_open_done;
	out->arena_used   = nn_open_done ? (uint32_t)npu_arena_used() : 0u;
	if (nn_open_done) {
		out->model  = (nn_model_from[0] != '\0') ? nn_model_from : "(raw)";
		out->source = npu_hw_ready() ? "npu up (secure, privileged)"
		                             : "npu down";
	} else {
		out->model  = NULL;
		out->source = npu_hw_fail_reason();
	}
	nn_release();
}

/* ---- resolving a model in the asset store -------------------------------- */

/*
 * Every slot, under the caller's lease.
 *
 * [!] IT REFUSES ON THE FIRST SLOT IT CANNOT READ rather than resolving from
 * what it got.  A scan with a hole cannot say a name is unique, and "not found"
 * is exactly the wrong answer to give about a slot nobody looked at.
 */
static int nn_scan_slots(uint32_t token, const char *name,
                         struct blob_slot_view *v, unsigned *count)
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
static int nn_resolve_blob(uint32_t token, const char *name, uint32_t *addr,
                           uint32_t *len)
{
	struct blob_slot_view v[BLOB_MAX_SLOTS];
	struct blob_info info;
	unsigned count = 0u, slot = 0u;
	uint32_t computed = 0u, payload = 0u;
	enum blob_lookup found;
	int rc;

	if (nn_scan_slots(token, name, v, &count) != 0)
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
	if (name != NULL && nn_resolve_blob(npu_hw_flash_lease(), name, &addr,
	                                    &len) != 0) {
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
static int nn_input_quant_ok(const struct npu_tensor *in)
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

static int nn_fill_input(const uint8_t *raw, const struct npu_tensor *in)
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
	if (nn_fill_input(camera_raw_frame(), &in) != 0) {
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
	if (nn_input_quant_ok(&in) == 0) {
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

/* ---- what this board's own `nn preview` needs ----------------------------
 *
 * Declared in nn_svc_grove.h; see there for why preview stays a board command
 * while its state lives here.
 */
int nn_svc_grove_acquire(void)
{
	return nn_try_acquire();
}

void nn_svc_grove_release(void)
{
	nn_release();
}

int nn_svc_grove_model_open(void)
{
	return nn_open_done;
}

int nn_svc_grove_detector_ready(const char **why)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_DECODER_MAX_OUTPUTS];
	unsigned n_out, i;

	nn_detail_clear();
	if (npu_input(&in) != NPU_OK) {
		nn_detail_set("the model has no input tensor");
		*why = nn_detail;
		return -1;
	}
	if (nn_input_quant_ok(&in) != 0) {
		*why = nn_detail;
		return -1;
	}
	n_out = npu_output_count();
	if (n_out > NN_DECODER_MAX_OUTPUTS) {
		nn_detail_set("the model has %u outputs and this path reads %u",
		              n_out, (unsigned)NN_DECODER_MAX_OUTPUTS);
		*why = nn_detail;
		return -1;
	}
	for (i = 0u; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			nn_detail_set("output %u is unreadable", i);
			*why = nn_detail;
			return -1;
		}
	if (!nn_decoder_shapes_ok(outs, n_out)) {
		nn_detail_set("the loaded model is not BlazeFace-shaped");
		*why = nn_detail;
		return -1;
	}
	return 0;
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
