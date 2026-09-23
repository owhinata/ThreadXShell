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
#include "nn_report.h"

#include <stdarg.h>
#include <string.h>

#define LOG_TAG "nn"
#include "log.h"

#include "blob.h"
#include "camera.h"
#include "cam_dp.h"
#include "fmt.h"
#include "plugin_info.h"
#include "plugin_load.h"
#include "plugin_run.h"
#include "plugin_target.h"   /* the target word this build provides (#108) */
#include "cam_lcd_sink.h"
#include "camera.h"
#include "npu_desc.h"
#include "nn_active.h"
#include "nn_overlay.h"
#include "nn_plugin_stack.h"  /* after camera.h and cam_lcd_sink.h (#119) */
#include "nn_probe.h"
#include "nn_preproc.h"
#include "nn_rec.h"
#include "nn_stream_state.h"
#include "nor_flash.h"    /* NOR_XIP_BASE */
#include "npu.h"
#include "npu_hw.h"
#include "tx_api.h"       /* tx_time_get() -- ThreadX ticks, 1 ms here */

/* ---- state ---------------------------------------------------------------
 *
 * Plain .bss: nothing here needs a placement attribute, because nothing here is
 * touched by a bus master.  The other two boards' adapters own nothing new at
 * all -- they already have a model singleton and a session gate of their own,
 * and a second copy would be a second answer.
 */
static uint8_t  nn_busy;            /**< the transient claim                  */
static uint8_t  nn_owner;           /**< enum nn_owner -- WHO holds it        */
static uint8_t  nn_open_done;       /**< a model is active                    */
static uint32_t nn_model_addr;
static uint32_t nn_model_len;
static char     nn_model_from[BLOB_NAME_MAX + 1];  /**< label, never a key    */

/* [!] A legal name must survive both copies it takes through the shared
 * command: the parser's (`--name`) and `nn info`'s (issue #122 P10). */
_Static_assert(NN_SPEC_NAME_MAX >= BLOB_NAME_MAX,
               "nn model load --name cannot carry this board's longest blob name");
_Static_assert(NN_SVC_MODEL_MAX >= BLOB_NAME_MAX,
               "nn info would truncate this board's longest blob name");
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
/*
 * [!] A STREAM'S NUMBERS ARE LATCHED WHEN IT ENDS (issue #120).  Its frame
 * count is the camera's, which a `nn run` capture also advances, so a poll that
 * kept deriving it after the stream ended drifted with every `nn run`.  The
 * stop takes the final numbers; a poll of that generation reads them here.
 */
static struct nn_stream_stats nn_stream_final;
static uint32_t nn_stream_final_gen;   /**< whose they are; ANY = nobody's */
/* The record's epoch when `last` was latched: a model change since took the
 * result away, and the latched line follows it (issue #118). */
static uint32_t nn_stream_final_epoch;
/* The record's accepted count at this stream's boundary (issue #118): `last`
 * is the stream's only once the record has accepted a publish since. */
static uint32_t nn_stream_acc0;

/* Claim IDLE -> STARTING together with the transient claim.  ONE critical
   section, because they are one decision: a start that took the claim and then
   found the lifecycle busy would have to unwind a claim another job may have
   taken in between. */
/*
 * Why the gate is held, for a start that found it held.  Called under the
 * gate's own critical section, so the answer describes the holder that refused
 * it (issue #122).
 *
 * [!] THE HOLDER DECIDES THE WORDS.  Every refusal used to read "another nn
 * job", including a `nn run` over a running stream -- where the other two
 * boards say "a stream is already running".  The lifecycle knows which holder
 * it is; an idle lifecycle means an ordinary operation (a load, a bench) has
 * the gate.  Read without a transition, so a refused start moves nothing.
 */
static enum nn_stream_start_claim nn_gate_refusal(void)
{
	uint8_t phase = 0u, kind = 0u;

	nn_stream_life_snapshot(&nn_life, NULL, &phase, NULL, &kind);
	switch ((enum nn_stream_phase)phase) {
	case NN_STREAM_PHASE_LOST:
		return NN_STREAM_START_DEAD;
	case NN_STREAM_PHASE_IDLE:
		return NN_STREAM_START_BUSY;           /* an operation holds it */
	default:
		if (kind == (uint8_t)NN_STREAM_KIND_ONESHOT)
			return NN_STREAM_START_ONESHOT;
		return (phase == (uint8_t)NN_STREAM_PHASE_RUNNING)
		       ? NN_STREAM_START_RUNNING : NN_STREAM_START_BUSY;
	}
}

static enum nn_stream_start_claim nn_stream_begin(void)
{
	enum nn_stream_start_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* The transient claim and the lifecycle are one decision here, so the gate
	   is tested first -- and its holder decides how the refusal reads. */
	if (nn_busy) {
		r = nn_gate_refusal();
	} else {
		r = nn_stream_life_begin(&nn_life, NN_STREAM_KIND_STREAM);
		if (r == NN_STREAM_START_GO) {
			nn_busy  = 1u;
			nn_owner = (uint8_t)NN_OWNER_STREAM;
		}
	}
	TX_RESTORE
	return r;
}

/*
 * `nn run`: the gate, and the lifecycle as a one-shot, in ONE critical section
 * (issue #120) -- the same single decision the stream start makes above.
 *
 * On this board the one-shot is synchronous on the calling thread, so it is
 * begun and committed together: nothing asynchronous comes up in between that
 * could fail.  The gate is held as an ordinary OPERATION, which is what `nn
 * info` should say about it.
 *
 * @return the one-shot's generation, or NN_STREAM_GEN_ANY with nothing held;
 *         @p why says which refusal
 */
static uint32_t nn_oneshot_claim(enum nn_stream_start_claim *why)
{
	uint32_t g = NN_STREAM_GEN_ANY;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (nn_busy) {
		*why = nn_gate_refusal();
	} else {
		*why = nn_stream_life_begin(&nn_life, NN_STREAM_KIND_ONESHOT);
		if (*why == NN_STREAM_START_GO) {
			g = nn_stream_life_commit(&nn_life);
			if (g == NN_STREAM_GEN_ANY) {
				(void)nn_stream_life_abort(&nn_life);
				*why = NN_STREAM_START_BUSY;
			} else {
				nn_busy  = 1u;
				nn_owner = (uint8_t)NN_OWNER_OP;
			}
		}
	}
	TX_RESTORE
	return g;
}

/*
 * ...and its end: claim the stop by its own generation, settle it, and give the
 * gate back -- one critical section, so nothing can be admitted between the
 * lifecycle going IDLE and the NPU being free.
 *
 * [!] THE GATE IS RELEASED ONLY IF THE TRANSITION HAPPENED.  Nothing else can
 * have claimed this one-shot's stop, so a refusal is an invariant failure; the
 * gate then stays held rather than handing the NPU back on it.
 *
 * @return non-zero if it settled
 */
static int nn_oneshot_end(uint32_t gen)
{
	int ok = 0;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (nn_stream_life_claim_stop(&nn_life, gen) == NN_STREAM_STOP_GO &&
	    nn_stream_life_finish(&nn_life)) {
		nn_owner = (uint8_t)NN_OWNER_NONE;
		nn_busy  = 0u;
		ok = 1;
	}
	TX_RESTORE
	return ok;
}

/* Everything came up: mint the generation and publish the baselines with it. */
static void nn_stream_commit(uint32_t frames0, uint32_t t0, uint32_t acc0,
                             uint32_t *gen)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* [!] Baselines published only once the generation exists, so a refused
	 * commit cannot leave this generation's numbers describing another's. */
	*gen = nn_stream_life_commit(&nn_life);
	if (*gen != NN_STREAM_GEN_ANY) {
		nn_stream_frames0 = frames0;
		nn_stream_t0      = t0;
		nn_stream_acc0    = acc0;
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

/* Publish an ended stream's numbers.  Called inside the settle's critical
   section, only when the settle took. */
static void nn_stream_latch(uint32_t ending, const struct nn_stream_stats *final,
                            uint32_t epoch)
{
	nn_stream_final = *final;
	nn_stream_final.elapsed_ms = nn_stream_ms;
	nn_stream_final_gen = ending;
	nn_stream_final_epoch = epoch;
}

/* Both halves confirmed. */
static void nn_stream_finish(const struct nn_stream_stats *final,
                             uint32_t epoch)
{
	uint32_t ending;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	ending = nn_life.gen;
	/* [!] THE CLAIM IS RELEASED ONLY IF THE TRANSITION HAPPENED.  Clearing it
	 * regardless would hand the NPU back on exactly the invariant failure the
	 * guard exists to catch -- and something may still be inside it. */
	if (nn_stream_life_finish(&nn_life)) {
		nn_stream_ms = (uint32_t)(((uint32_t)tx_time_get() - nn_stream_t0) *
		                          1000u / TX_TIMER_TICKS_PER_SECOND);
		nn_stream_latch(ending, final, epoch);
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
static void nn_stream_poison(const struct nn_stream_stats *final,
                             uint32_t epoch)
{
	uint32_t ending;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	ending = nn_life.gen;
	if (nn_stream_life_poison(&nn_life)) {
		nn_stream_ms = (uint32_t)(((uint32_t)tx_time_get() - nn_stream_t0) *
		                          1000u / TX_TIMER_TICKS_PER_SECOND);
		nn_stream_latch(ending, final, epoch);
	}
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
/* [!] Its format is checked against NN_SVC_DETAIL_MAX at build time (issue
 * #122 P15): the copy truncates, and a truncated sentence does not look it. */
#define nn_detail_set(...)                                                  \
	((void)NN_SVC_DETAIL_CHECK_FMT(__VA_ARGS__),                        \
	 nn_detail_to(res->detail, sizeof res->detail, __VA_ARGS__))
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
		   arena reservation above is a link-time constant, and nothing else.
		   [!] BUSY, NOT WITHHELD (issue #122 P4): WITHHELD tells the operator
		   to go and stop a stream, and none is running -- a load, a run or a
		   bench is, and it lets go by itself. */
		out->avail_identity = (uint8_t)NN_AVAIL_BUSY;
		out->avail_runtime  = (uint8_t)NN_AVAIL_BUSY;
		out->avail_tensors  = (uint8_t)NN_AVAIL_BUSY;
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

/* ---- containers (issue #101) ---------------------------------------------- */

/*
 * This board's policy for a plugin image.  svc/plugin_load.c holds no board
 * address, so the reservation, the target identity and what each thread can
 * spare arrive from here.
 *
 * [!] THE STACK LIMITS ARE nn_plugin_stack.h's, slot by slot (issue #119).  That
 * header says which thread each slot runs on and asserts every allowance below
 * each of those threads' stacks; it is a header so that the host test compiles
 * the same asserts and the same table this initialiser uses.  Until #119 three
 * slots were declared against the producer's figure while running on a shell
 * stack the same size as the allowance.
 */
/*
 * [!] EVERY NUMBER COMES FROM THE BUILD, NOT FROM HERE.  The host sender runs
 * the same validator (verify_container links this very file's plugin_load.c)
 * with the same policy, and if the two were declared separately a container
 * could pass on the host and be refused on the board -- which is exactly the
 * shape issue #93 hit when its host gate used the flatbuffer verifier's default
 * limits and the firmware used its own.  board.cmake defines these once and
 * passes them both ways.
 *
 * This is NOT the hazard issue #85 names.  There, a layout and the gate that
 * checked it came from one CACHE variable, so a single -D moved the rule and
 * its verification together.  Here the two consumers must AGREE -- it is one
 * rule applied in two places, not a rule and its check -- and the reservation's
 * address is still verified independently, by the linker script and
 * check_placement_budget.py, which state it separately on purpose.
 */
#ifndef GROVE_PLUGIN_TARGET_ID
#error "board.cmake must define GROVE_PLUGIN_TARGET_ID"
#endif
/*
 * [!] THE WORD IS CHECKED HERE, against this firmware's own build (issue #108).
 * board.cmake hands the same value to the packer, the host verifier and this
 * policy, so those three agreeing proved nothing about the value.  This derives
 * it from the compiler's predefined macros instead -- and it is the ONLY check
 * of the CMSE bit, which says this base runs Secure (the -mcmse on shell_objs)
 * and which no plugin image records.  The image gate checks the other bits
 * against the plugin ELF.
 */
_Static_assert(GROVE_PLUGIN_TARGET_ID == PLUGIN_TARGET_ID_HERE,
               "GROVE_PLUGIN_TARGET_ID does not describe this firmware's build "
               "(svc/plugin_target.h)");
#ifndef GROVE_PLUGIN_BASE
#error "board.cmake must define GROVE_PLUGIN_BASE"
#endif
#ifndef GROVE_PLUGIN_MAX
#error "board.cmake must define GROVE_PLUGIN_MAX"
#endif
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
	.target_id      = GROVE_PLUGIN_TARGET_ID,
	.link_addr      = GROVE_PLUGIN_BASE,
	.capacity       = GROVE_PLUGIN_MAX,
	.image_align    = PLUGIN_IMAGE_ALIGN,
	.caps_supported = PLUGIN_CAP_KNOWN_MASK,
	.stack_limit    = GROVE_PLUGIN_STACK_LIMITS,   /* nn_plugin_stack.h */
	.veneer_cost    = PLUGIN_VENEER_BASE_COST,     /* veneer_cost_gate() */
	.stack_accounting = PLUGIN_STACK_ACCOUNTING,
};
/* What the build reads back from shell.elf: the c and accounting this policy
 * really holds, whatever -D reached this compile (cmake/check_policy_probe.py,
 * issue #111). */
PLUGIN_POLICY_PROBE(nn_plugin_policy);

/*
 * What this board offers a plugin (issue #103).
 *
 * [!] TWO ENTRIES, NOT SEVEN.  The reviewed plan listed preprocessing, invoke
 * and tensor fetching here as well.  Reading the code they would have wrapped
 * settles it: nn_overlay.c drives the sequence -- it counts the outputs, fills
 * the input, invokes the NPU and then CALLS the decoder.  A decoder is called;
 * it never calls up.  Exporting machinery no plugin can reach for would be
 * three more indirect call sites for the stack analysis to account for and
 * nothing gained.
 */
/*
 * The producer thread has no console, so this is the only way a plugin can
 * explain itself.
 *
 * [!] THE BYTES GO TO THE LOG WITHOUT THE FORMATTER (issue #112).  This said
 * `LOG_INF("plugin: %.*s", (int)len, s)`, and svc/fmt.c implements neither a
 * precision nor `*` -- deliberately, it is a clean-room minimal formatter -- so
 * a plugin's explanation came out as that format string plus whatever the
 * varargs were read as (present since issue #103).  log_write_bytes() takes
 * the bytes by length, so they need no terminator and no copy; a text too long
 * for the record is cut and ends " ...", and an empty or NULL one writes
 * nothing -- the shape the wio-lite-ai adapter already had.
 *
 * It also takes the formatter out from below the log veneer.  The formatter
 * was the deepest chain behind it and the only indirect calls there (its
 * putters), so what the plugin veneer charges for is now derived by
 * cmake/check_veneer_base_cost.py without an exception.
 */
static void nn_plugin_log(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	if (s == NULL || len == 0u)
		return;
	if (LOG_LEVEL_INF <= LOG_COMPILE_LEVEL)
		log_write_bytes(LOG_LEVEL_INF, LOG_TAG, "plugin: ", s, len, " ...");
}

static const struct plugin_base_api nn_plugin_base = {
	.version  = PLUGIN_ABI_VERSION,
	.size     = (uint32_t)sizeof(struct plugin_base_api),
	.ctx      = NULL,
	.log      = nn_plugin_log,
	.to_frame = nn_active_to_frame,
};

/*
 * The manifest of the container the open model came from, or zeroed.
 *
 * [!] PLAIN DATA, AND NOTHING HERE IS EVER CALLED.  struct plugin_view carries
 * integer offsets, not function pointers -- that is what makes "Step 1a does not
 * execute a plugin" a property of the types rather than a rule someone keeps.
 */
static struct plugin_view nn_container;
static int                nn_has_container;

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
	/*
	 * [!] THE CONTAINER SPLIT HAPPENS HERE, INSIDE THE SAME LEASE (issue #101).
	 * Everything above established that these bytes are what their CRC says
	 * they are and cannot change while the lease is held; deciding what they
	 * MEAN has to happen before that grip is released, or a background
	 * `blob write` could replace the slot between the decision and the open.
	 *
	 * A payload that is not a container is a bare model, exactly as before.
	 * That is not a fallback for tidiness -- `det` and `cls` are on the device
	 * now, sent before containers existed, and a change that required
	 * re-sending them would cost two erases of a part whose endurance is not
	 * documented.
	 */
	nn_has_container = 0;
	if (plugin_probe((const void *)(uintptr_t)(NOR_XIP_BASE + payload),
	                 info.length) == PLUGIN_KIND_CONTAINER) {
		enum plugin_result pr;

		pr = plugin_parse((const void *)(uintptr_t)(NOR_XIP_BASE + payload),
		                  info.length, &nn_plugin_policy, &nn_container);
		if (pr != PLUGIN_OK) {
			nn_detail_set("slot %u ('%s') is a container this firmware "
			              "refuses: %s", slot, name,
			              plugin_result_name(pr));
			return -1;
		}
		nn_has_container = 1;

		/*
		 * [!] STILL INSIDE THE LEASE.  The image is read from the XIP window,
		 * and the window is only pinned while this lease is live -- which is
		 * exactly why the load happens here rather than after the caller has
		 * finished with the model.  plugin_run_load() checks it too, because a
		 * later caller may not know that.
		 *
		 * A container with no plugin is not a failure: the model half of it is
		 * still perfectly usable, and NO_PLUGIN says so.
		 */
		{
			enum plugin_run_result pr;

			pr = plugin_run_load(&nn_container,
			                     (const void *)(uintptr_t)(NOR_XIP_BASE +
			                                               payload),
			                     token, &nn_plugin_base);
			if (pr != PLUGIN_RUN_OK && pr != PLUGIN_RUN_NO_PLUGIN) {
				nn_detail_set("slot %u ('%s'): %s", slot, name,
				              plugin_run_strerror(pr));
				return -1;
			}
		}
		/* The MODEL section, not the container: npu_open() parses a
		 * flatbuffer and the rest of the payload is not one. */
		*addr = NOR_XIP_BASE + payload + nn_container.model_off;
		*len  = nn_container.model_len;
		nn_model_slot = (int)slot;
		return 0;
	}

	*addr = NOR_XIP_BASE + payload;
	*len  = info.length;
	nn_model_slot = (int)slot;
	return 0;
}

/* ---- model lifecycle ----------------------------------------------------- */

/*
 * Undo a load that got part way.
 *
 * [!] THE PLUGIN IS PART OF THE LOAD, SO IT IS PART OF THE UNWIND (issue #103).
 * nn_resolve_blob() starts the plugin before npu_open() is even called -- it has
 * to, because the image is read from the XIP window and only this lease pins it
 * -- so a load that fails AFTER that point left one running.  `nn_open_done`
 * stays 0, the next `nn model load` is therefore allowed, and if that one is a
 * bare model or a container with no plugin then nothing calls plugin_run_load()
 * at all: the previous model's decoder reads the new model's outputs.
 *
 * That was wrong before this issue and is worse after it, because
 * nn_active_is_plugin() now also waives the resident decoder's input-quantisation
 * precondition -- so the one check that would have noticed the mismatch is the
 * one the stale plugin switches off.
 *
 * Written as the mirror of nn_svc_model_unload()'s order, plugin first, so the
 * two cannot drift.
 */
static void nn_load_undo(void)
{
	plugin_run_unload();
	nn_has_container = 0;
	npu_hw_deinit();
}

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
	/* [!] From here the plugin and the model can change, whatever this load
	 * then reports, so the last result goes now (issue #118) -- under the
	 * gate, before any new identity is visible.  Nothing is open at this point
	 * (a load over an open model is refused above), so nothing is lost. */
	nn_rec_invalidate();

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
		nn_load_undo();
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		nn_release();
		return;
	}

	rc = npu_open(addr, len, npu_arena_base(), npu_arena_bytes());
	if (rc != NPU_OK) {
		nn_detail_set("%s (0x%08lx, %lu B)", npu_status_name(rc),
		              (unsigned long)addr, (unsigned long)len);
		nn_load_undo();
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
	/* Idempotent: unloading nothing succeeds.  Order is plugin -> model -> NPU
	   -> lease, and npu_hw_deinit() is what returns the flash lease.
	   [!] THE PLUGIN GOES FIRST.  It was loaded from the window this lease
	   pins, and its code is about to stop being the code anyone should enter;
	   unpublishing before the model is closed means no window exists in which
	   the fault reporter names a plugin whose model is already gone. */
	plugin_run_unload();
	nn_has_container = 0;
	npu_close();
	npu_hw_deinit();
	/* The model and its decoder are gone, and the last result with them
	 * (issue #118) -- under the gate. */
	nn_rec_invalidate();
	nn_open_done     = 0u;
	nn_model_addr    = 0u;
	nn_model_len     = 0u;
	nn_model_slot    = -1;
	nn_model_from[0] = '\0';
	nn_geom_valid    = 0u;
	nn_active_clear_geom();
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
	npu_desc_of(out, &t);
	return NN_SVC_OK;
}

int nn_svc_input(struct tensor_desc *out)
{
	struct npu_tensor t;

	if (!nn_open_done)
		return NN_SVC_ERR_STATE;
	if (npu_input(&t) != NPU_OK)
		return NN_SVC_ERR_ARG;
	npu_desc_of(out, &t);
	return NN_SVC_OK;
}

/* ---- one shot ------------------------------------------------------------ */

/*
 * Can this board fill the model's input at all?
 *
 * [!] THE BOARD'S PRECONDITION, AND THE ONLY ONE LEFT.  nn_preproc_fill()
 * writes ONE BYTE per element: on a tensor of any other element type it fills a
 * fraction of the buffer with values of the wrong width, and the length check in
 * nn_fill_input() passes, because a float32 tensor of the same shape is LARGER
 * than the bytes being written.  The NPU is then invoked on a mostly-untouched
 * arena.
 *
 * It sat inside a QUANTISATION check until issue #103 split the two, and that
 * split is why issue #104 could delete the other half without taking this with
 * it: the quantisation question belonged to the resident decoder and went when
 * that decoder did, while this one is about whether the frame can be written at
 * all and is true whoever reads the result -- or whether anyone does.
 */
static int nn_input_fillable(struct nn_op_result *res,
                             const struct npu_tensor *in)
{
	(void)res;
	if (!npu_tensor_is_int8(in->type)) {
		nn_detail_set("this board fills the input a byte at a time, so it "
		              "needs an int8 input; this model has %s",
		              npu_type_name(in->type));
		return -1;
	}
	return 0;
}

static int nn_fill_input(struct nn_op_result *res, const uint8_t *raw,
                         const struct npu_tensor *in)
{
	uint32_t w, h;

	if (nn_input_fillable(res, in) != 0)
		return -1;
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
	nn_active_set_geom(&nn_geom);   /* the plugin's transform, issue #103 */
	return 0;
}

/*
 * Decode what `nn run` just inferred -- or say that nothing did -- and PUBLISH
 * it to the record `nn dets` reads (issue #118).
 *
 * [!] THE ONE PLACE THIS BOARD DECIDES (issue #104), and since issue #118 the
 * only caller is `nn run`: `nn dets` reads the record and decodes nothing.  The
 * plugin-or-raw choice lives here, so the two routes cannot be taken
 * differently -- the shape issue #103 got wrong once.
 *
 * [!] AND WITH NO PLUGIN THERE IS NO DECODER AT ALL.  The outputs are reported
 * as the tensors they are (NN_DET_RAW_TENSORS), never as a BF_ERR_* code:
 * BF_ERR_MODEL routes to the shared class report, which would print the top 5
 * of a detector's regression tensor as though the numbers were class scores.
 *
 * A plugin's negative return is published as it is (issues #57, #97, #118).
 *
 * @return 0 when a result was published; -1 when no decode could be run (an
 *         output is unreadable), with the detail set and nothing published
 */
/*
 * The output shapes of the model that ran, taken under the gate that keeps it
 * open (issue #121) -- not at print time.
 *
 * [!] NOT INLINED, AND THAT IS A STACK DECISION.  The descriptors are ~300 B,
 * and inlined into nn_decode_publish() they would sit in the frame the plugin's
 * decode() is entered below -- a slot declared against the shell's ceiling.
 * Only the path with no plugin needs them.
 */
static __attribute__((noinline)) void nn_publish_raw_outputs(uint32_t gen)
{
	struct nn_raw_outputs raw;
	unsigned n = npu_output_count(), i;

	memset(&raw, 0, sizeof raw);
	raw.count = (int32_t)n;
	for (i = 0u; i < n && i < NN_RAW_OUTPUTS_MAX; i++) {
		struct npu_tensor t;

		if (npu_output(i, &t) != NPU_OK)
			break;
		npu_desc_of(&raw.out[i], &t);
		raw.n = (uint8_t)(i + 1u);
	}
	(void)nn_rec_publish_raw(gen, &raw);
}

static int nn_decode_publish(uint32_t gen, struct nn_op_result *res)
{
	struct npu_tensor outs[NPU_DESC_MAX_OUTPUTS];
	unsigned n_out, i;

	if (!nn_active_is_plugin()) {
		nn_publish_raw_outputs(gen);
		return 0;
	}

	n_out = npu_output_count();
	if (n_out > NPU_DESC_MAX_OUTPUTS)
		n_out = NPU_DESC_MAX_OUTPUTS;
	for (i = 0u; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			/* No decode ran, so there is nothing to publish -- and the
			 * record keeps describing what the plugin last decoded. */
			nn_detail_set("output %u of the model is unreadable", i);
			return -1;
		}

	(void)nn_rec_publish_external(nn_active_decode(outs, n_out), gen);
	return 0;
}

/*
 * Ask the plugin for its account of the result the record holds.
 *
 * [!] ONLY UNDER THE GATE (issue #110), which is what excludes every other
 * decode -- `nn run` holds it, and a stream holds it for its whole life.  So
 * the snapshot is taken here, under it too: taken before, a `nn run` finishing
 * in between would leave this pairing its count with the next frame's account.
 */
static void nn_capture_report(const struct nn_det_snapshot *snap,
                              struct nn_report_capture *rep)
{
	if (rep == NULL)
		return;
	if (!snap->valid || snap->kind != (uint8_t)NN_DET_PLUGIN_REPORT) {
		nn_report_set(rep, NN_REPORT_NONE);
		return;
	}
	if (!snap->reportable) {
		/* A decode ran whose publish was dropped (issue #118) -- not a stop
		 * on this board (see nn_rec.h), but the record's rule holds anyway. */
		nn_report_set(rep, NN_REPORT_SUPERSEDED);
		return;
	}
	nn_report_begin(rep);
	if (!nn_active_can_report())
		nn_report_set(rep, NN_REPORT_UNSUPPORTED);
	else
		nn_report_end(rep, nn_active_report(nn_report_write, rep));
}

/* Every exit of `nn run` after its claim: settle the one-shot, and if that is
 * refused -- an invariant failure -- say so over whatever the run concluded,
 * because the gate is still held and nothing will release it. */
static void nn_oneshot_finish(uint32_t gen, struct nn_op_result *res)
{
	if (nn_oneshot_end(gen))
		return;
	nn_detail_set("the stream lifecycle moved underneath this run; the NPU "
	              "stays held");
	nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
}

void nn_svc_run_once(struct nn_det_snapshot *snap, struct bf_det *dets, int max,
                     struct nn_report_capture *rep, struct nn_result_extra *ext,
                     nn_svc_cancel_fn cancel, void *ctx,
                     struct nn_op_result *res)
{
	struct npu_tensor in;
	enum nn_stream_start_claim why = NN_STREAM_START_BUSY;
	uint32_t gen, rgen, base;
	int rc;

	nn_detail_clear();
	/* Nothing here waits long enough to poll: camera_capture() and npu_invoke()
	   are each one blocking call into hardware.  Checked once so a Ctrl+C that
	   arrived before the work starts is still honoured. */
	if (nn_svc_cancelled(cancel, ctx)) {
		nn_result(res, NN_SVC_ERR_CANCEL, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] THE LIFECYCLE IS CLAIMED WITH THE GATE, BEFORE THE CAMERA (issue
	 * #120).  `nn run` is a stream of one inference: while it runs the
	 * lifecycle says so, so `nn stream stop` is refused rather than told "no
	 * stream is running" about a camera this command holds.
	 */
	gen = nn_oneshot_claim(&why);
	if (gen == NN_STREAM_GEN_ANY) {
		switch (why) {
		case NN_STREAM_START_DEAD:
			nn_detail_set("a previous teardown was never confirmed; only "
			              "a reboot clears it");
			nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
			return;
		case NN_STREAM_START_RUNNING:
			/* The same words as the other two boards (issue #122). */
			nn_detail_set("a stream is already running -- `nn stream "
			              "stats`");
			break;
		case NN_STREAM_START_ONESHOT:
			nn_detail_set("another `nn run` holds the NPU -- retry when it "
			              "returns");
			break;
		default:
			break;
		}
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* The run's record boundary and the base it counts from, under the gate
	 * that excludes every other publisher (issue #118). */
	base = nn_rec_boundary_base();
	rgen = nn_rec_gen();
	if (!nn_open_done) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_oneshot_finish(gen, res);
		return;
	}
	if (npu_input(&in) != NPU_OK) {
		nn_detail_set("the model has no input tensor");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		nn_oneshot_finish(gen, res);
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
		nn_oneshot_finish(gen, res);
		return;
	}
	if (nn_fill_input(res, camera_raw_frame(), &in) != 0) {
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_oneshot_finish(gen, res);
		return;
	}

	/* No cache maintenance here (issue #46): it lives in the port's own
	 * lifecycle callbacks, at the only two instants that are correct. */
	if (npu_invoke() != NPU_OK) {
		nn_detail_set("inference failed");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_oneshot_finish(gen, res);
		return;
	}

	/*
	 * WHO DECODES.
	 *
	 * [!] AND NOTHING ASKS ABOUT THE INPUT QUANTISATION ANY MORE (issue #104).
	 * That check belonged to the resident decoder: nn_preproc_fill() writes
	 * `pixel - 128` and BlazeFace's arithmetic assumed the model read that as
	 * scale 1/255 zero point -128.  With that decoder gone the question has no
	 * owner -- a plugin ships WITH its model, and shipping it is the statement
	 * that the two agree (the vendor's own CIFAR-10 app writes `pixel - 128`
	 * into an input recorded at scale 0.0203 zero point -8 and is right to).
	 *
	 * What that costs is a diagnostic, and it is worth naming: a bare model
	 * whose input quantisation is not this board's convention is still fed, and
	 * the tensors reported below are then a faithful reading of a meaningless
	 * inference.  The convention is stated in the board README rather than
	 * enforced by a check no decoder stands behind.
	 */
	if (nn_decode_publish(rgen, res) != 0) {
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
		nn_oneshot_finish(gen, res);
		return;
	}
	/* [!] READ BACK FROM THE RECORD, AND ONLY THIS RUN'S (issue #118): the
	 * answer `nn dets` will give next is the one printed now.  The account is
	 * captured before the gate goes. */
	nn_rec_snapshot(snap, ext);
	snap->valid = nn_det_last_valid(snap, base);
	if (snap->valid) {
		nn_capture_report(snap, rep);
	} else {
		nn_report_set(rep, NN_REPORT_NONE);
		if (ext != NULL)
			ext->what = (uint8_t)NN_EXTRA_NONE;
	}

	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
	nn_oneshot_finish(gen, res);
}

void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max, struct nn_report_capture *rep,
                           struct nn_result_extra *ext,
                           struct nn_op_result *res)
{
	int gated;

	(void)dets;
	(void)max;
	nn_detail_clear();

	/*
	 * [!] THE RECORD, NOT A DECODE (issue #118).  This used to take the gate
	 * and decode whatever the outputs held -- a second decode of a frame, on
	 * the shell thread, that `nn run` had already decoded; the other two
	 * boards read their record.  Now all three do: the last result `nn run`
	 * or a stream published, which a stop does not clear and a model change
	 * does.
	 *
	 * [!] THE PLUGIN'S ACCOUNT NEEDS THE GATE, AND A STREAM HOLDS IT (decision
	 * D2).  The count is read regardless; the account is taken only when the
	 * gate is free, and otherwise said to be unreachable (STALE) -- the
	 * producer is decoding the next frame over it.  Not waited for: the
	 * stream holds the gate until its stop.
	 */
	gated = nn_try_acquire();
	nn_rec_snapshot(snap, ext);
	if (gated) {
		nn_capture_report(snap, rep);
		nn_release();
	} else if (snap->valid && snap->kind == (uint8_t)NN_DET_PLUGIN_REPORT) {
		nn_report_set(rep, NN_REPORT_STALE);
	} else {
		nn_report_set(rep, NN_REPORT_NONE);
	}
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
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
	struct npu_tensor outs[NPU_DESC_MAX_OUTPUTS];
	unsigned n_out, i;

	if (npu_input(&in) != NPU_OK) {
		nn_detail_set("the model has no input tensor");
		return -1;
	}
	/* [!] THE BOARD'S PRECONDITION FIRST, AND FOR EVERY DECODER.  The producer
	 * fills this tensor a byte at a time on every frame; a plugin cannot see
	 * the input tensor and so cannot check it, which makes this the last place
	 * anything can. */
	if (nn_input_fillable(res, &in) != 0)
		return -1;
	/*
	 * [!] IS THERE A DECODER AT ALL, ASKED FIRST (issue #104).  Ahead of the
	 * shape question and ahead of the draw question, because with no plugin
	 * loaded neither of those has anything to be about -- and because the answer
	 * sends the operator somewhere the others do not: not to a different model,
	 * but to a container that carries a decoder for this one.  `nn run` still
	 * works here and reports the raw outputs; a stream cannot, because there
	 * would be nothing to put on the panel.
	 */
	if (!nn_active_is_plugin()) {
		nn_detail_set("no decoder is loaded, so a stream would annotate "
		              "nothing -- load a container that carries one; "
		              "`nn run` reports the raw outputs");
		return -1;
	}
	n_out = npu_output_count();
	if (n_out > NPU_DESC_MAX_OUTPUTS) {
		nn_detail_set("the model has %u outputs and this path reads %u",
		              n_out, (unsigned)NPU_DESC_MAX_OUTPUTS);
		return -1;
	}
	for (i = 0u; i < n_out; i++) {
		if (npu_output(i, &outs[i]) != NPU_OK) {
			nn_detail_set("output %u is unreadable", i);
			return -1;
		}
	}
	if (!nn_active_shapes_ok(outs, n_out)) {
		nn_detail_set("the loaded plugin cannot read this model's outputs");
		return -1;
	}
	/*
	 * [!] AND WOULD IT ANNOTATE ANYTHING?  A plugin need not draw -- the
	 * classifier container carries no draw(), because a label on the panel needs
	 * a font and that is #78 Step 2.  Starting a stream for it would light the
	 * camera and the panel and show a live picture that is never marked, which
	 * is the exact failure this function exists to refuse before anything is
	 * acquired.  `nn run` still reports its classes.
	 */
	if (!nn_active_can_draw()) {
		nn_detail_set("the loaded plugin does not draw, so a stream would "
		              "annotate nothing -- `nn run` reports its result");
		return -1;
	}
	return 0;
}

void nn_svc_stream_start(const struct nn_stream_spec *spec,
                         struct nn_op_result *res, uint32_t *gen)
{
	struct camera_stats cs;
	uint32_t acc0;
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
	case NN_STREAM_START_ONESHOT:
		/* The gate is tested first above, so a live `nn run` reads as BUSY;
		 * this is the lifecycle saying the same thing (issue #120). */
		nn_detail_set("a `nn run` holds the NPU -- retry when it returns");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
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
		nn_detail_set("no model is loaded -- `nn model load --name <name>`");
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
	/*
	 * [!] THE RECORD BOUNDARY COMES BEFORE THE ATTACH (issue #118), and the
	 * base this stream counts its own publishes from is taken in the same
	 * critical section.  From here only this stream's producer can publish,
	 * and it cannot start until the attach below -- so nothing it produces is
	 * absorbed into the base, and nothing before it is counted as its own.
	 */
	acc0 = nn_rec_boundary_base();
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
	nn_stream_commit(cs.frames, (uint32_t)tx_time_get(), acc0, gen);
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
	nn_detail_set("inference stream started -- the decoder is annotating "
	              "the panel");
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* The per-stream counts -- the one computation a poll and a stop's latch
   share. */
static void nn_stream_counts(const struct camera_stats *cs,
                             const struct nn_overlay_stats *os,
                             const struct nn_det_snapshot *rec,
                             uint32_t frames0, uint32_t acc0,
                             struct nn_stream_stats *out)
{
	out->frames         = cs->frames - frames0;
	out->skipped        = os->skipped;
	out->infers         = os->inferences;
	out->errors         = os->errors;
	out->model_errors   = os->model_errors;
	out->decoder_errors = os->decoder_errors;
	out->last_us        = os->last_ms * 1000u;
	/* [!] Nothing decoded yet is not "decoded nobody".  And `last` is the
	 * RECORD's, counted against this stream's base (issue #118): the producer
	 * publishes every decode there, refusals included, and a model change
	 * clears it -- which the producer's own counter cannot know. */
	out->last_valid = nn_det_last_valid(rec, acc0) ? 1u : 0u;
	out->last_ndet  = (int32_t)rec->ndet;
}

/* A stopping stream's final numbers.  Outside any critical section: the
   camera's stats end in the frame pipeline's mutex. */
static uint32_t nn_stream_take_final(struct nn_stream_stats *final)
{
	struct camera_stats cs;
	struct nn_overlay_stats os;
	struct nn_det_snapshot rec;
	uint32_t frames0, acc0;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	frames0 = nn_stream_frames0;
	acc0    = nn_stream_acc0;
	TX_RESTORE
	camera_stream_stats(&cs);
	nn_overlay_stats(&os);
	nn_rec_snapshot(&rec, NULL);
	memset(final, 0, sizeof *final);
	nn_stream_counts(&cs, &os, &rec, frames0, acc0, final);
	return rec.epoch;     /* latched with the line it qualifies */
}

int nn_svc_stream_poll(uint32_t gen, struct nn_stream_stats *out)
{
	struct camera_stats cs;
	struct nn_overlay_stats os;
	struct nn_det_snapshot rec;
	uint32_t seq0, seq1, g, frames0, t0, ms, acc0;
	uint8_t  phase, kind;
	TX_INTERRUPT_SAVE_AREA

	if (out == NULL)
		return NN_SVC_ERR_ARG;

	/* Phase 1: identity and baselines. */
	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, &g, &phase, &seq0, &kind);
	/* [!] An ended stream answers from its latch, taken in the same critical
	 * section as the generation it belongs to -- see nn_stream_final. */
	if (g != NN_STREAM_GEN_ANY && g == nn_stream_final_gen &&
	    (gen == NN_STREAM_GEN_ANY || gen == g)) {
		uint32_t ep = nn_stream_final_epoch;

		*out = nn_stream_final;
		TX_RESTORE
		/* [!] ...except that a model change since the stop took its last
		 * result away (issue #118).  The record's lock is its own critical
		 * section, so the epoch is compared just after. */
		nn_rec_snapshot(&rec, NULL);
		if (rec.epoch != ep)
			out->last_valid = 0u;
		return NN_SVC_OK;
	}
	frames0 = nn_stream_frames0;
	t0      = nn_stream_t0;
	ms      = nn_stream_ms;
	acc0    = nn_stream_acc0;
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
	nn_rec_snapshot(&rec, NULL);

	/*
	 * Phase 3: accept only if nothing moved.  The counter, not the generation
	 * and the state -- a retryable stop returns to both of those unchanged.
	 */
	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, NULL, NULL, &seq1, NULL);
	TX_RESTORE
	if (seq1 != seq0)
		return NN_SVC_ERR_STALE;

	memset(out, 0, sizeof *out);
	/* [!] A `nn run` holding the lifecycle is not a running STREAM (#120);
	 * the baselines below are still the last stream's. */
	out->running        = (phase == (uint8_t)NN_STREAM_PHASE_RUNNING &&
	                       kind == (uint8_t)NN_STREAM_KIND_STREAM) ? 1u : 0u;
	nn_stream_counts(&cs, &os, &rec, frames0, acc0, out);
	/* [!] NEVER WHILE THE LIFECYCLE NAMES A ONE-SHOT (issue #118, review):
	 * the base is the last STREAM's, and `nn run` publishes into the same
	 * record -- see the same guard in the wio adapter. */
	if (kind != (uint8_t)NN_STREAM_KIND_STREAM)
		out->last_valid = 0u;
	out->elapsed_ms     = out->running
	                    ? (uint32_t)(((uint32_t)tx_time_get() - t0) * 1000u /
	                                 TX_TIMER_TICKS_PER_SECOND)
	                    : ms;
	return NN_SVC_OK;
}

/* The sentence an operator gets.  They are not interchangeable -- two of these
   mean "nothing was touched" and "something is still running in there". */
static const char *nn_stream_why_text(unsigned char why)
{
	/* Each sentence is checked against NN_SVC_DETAIL_MAX at build time -- it
	 * is copied whole into a result's detail (issue #122 P15). */
	switch ((enum nn_stream_why)why) {
	case NN_STREAM_WHY_CAM_LOCKED:
		return NN_SVC_DETAIL_LIT(
			"the camera API stayed locked, so the stop was never "
			"requested and nothing was touched -- run `nn stream stop` "
			"again");
	case NN_STREAM_WHY_CAM_LOST:
		return NN_SVC_DETAIL_LIT(
			"the producer never acknowledged the stop; the camera is "
			"unusable until reboot");
	case NN_STREAM_WHY_CAM_STATE:
		return NN_SVC_DETAIL_LIT(
			"the camera refused the stop; it is unusable until reboot");
	case NN_STREAM_WHY_SINK_BUSY:
		return NN_SVC_DETAIL_LIT(
			"the panel has not finished with this stream's frames -- "
			"run `nn stream stop` again");
	case NN_STREAM_WHY_SINK_LOST:
		return NN_SVC_DETAIL_LIT(
			"the panel thread did not finish; the preview is unusable "
			"until reboot");
	case NN_STREAM_WHY_OK:
	default:
		return NN_SVC_DETAIL_LIT("stopped");
	}
}

void nn_svc_stream_stop(uint32_t gen, struct nn_op_result *res)
{
	struct nn_stream_stats final;
	struct nn_stream_verdict v;
	uint32_t epoch;
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
	case NN_STREAM_STOP_ONESHOT:
		/* [!] Refused, not "no stream is running" (issue #120): a `nn run`
		 * holds the camera and the NPU and ends by itself. */
		nn_detail_set("a `nn run` holds the camera and stops it itself -- "
		              "retry when it returns");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
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

	/*
	 * [!] THE RECORD BOUNDARY ONLY ONCE THE PRODUCER IS CONFIRMED OUT (issue
	 * #118).  It publishes inside consume(), immediately after each decode, so
	 * after a confirmed stop every decode it ran has been published and none
	 * can follow -- the stream's last result keeps an account the plugin can
	 * still give.  A boundary taken earlier would drop the frame in flight
	 * AFTER its decode had rewritten the plugin's result, which is what wio
	 * had to close with its lease.  Unconfirmed, there is no boundary: the
	 * lifecycle goes DEAD and nothing admits a new session.
	 */
	if (nn_stream_may_detach(cam_rc))
		nn_rec_boundary();

	/* [!] AND THE DETACH IS THE SECOND HALF OF THE STOP (issue #57), reached
	 * only on a confirmed producer stop -- the blit runs on the panel thread,
	 * so a confirmed stop alone does not prove nothing is using the frame. */
	if (nn_stream_may_detach(cam_rc)) {
		attempted = 1;
		detach_rc = cam_lcd_sink_detach();
	}
	nn_stream_stop_decide(cam_rc, attempted, detach_rc, &v);
	/* The stream's final numbers, latched only if the settle below takes. */
	epoch = nn_stream_take_final(&final);

	switch ((enum nn_stream_act)v.act) {
	case NN_STREAM_ACT_DONE:
		nn_stream_finish(&final, epoch);
		nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
		return;
	case NN_STREAM_ACT_RETRY:
		nn_stream_unclaim_stop();
		nn_detail_set("%s", nn_stream_why_text(v.why));
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_RETRYABLE);
		return;
	case NN_STREAM_ACT_TERMINAL:
	default:
		nn_stream_poison(&final, epoch);
		nn_detail_set("%s", nn_stream_why_text(v.why));
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}
}

/*
 * The stack report (issue #119): one line per plugin slot, in slot order, from
 * port/npu/nn_probe_rtos.c.  The label and the note are nn_probe.c's (one
 * table, which the widest-line test reads too); the threads are the ones
 * nn_plugin_stack.h says the slot runs on.
 *
 * [!] NOT THE THREAD'S PEAK.  `thread` reports the deepest a thread ever got,
 * anywhere; what an allowance is derived from is how much is ALREADY SPENT at
 * the instant a plugin is entered (issue #103), which nothing else prints.
 */
static const uint8_t nn_slot_runs[PLUGIN_SLOT_COUNT] = GROVE_PLUGIN_STACK_RUNS;

int nn_svc_stream_lines(enum nn_stream_lines_ctx ctx, unsigned index,
                        char *buf, size_t cap)
{
	struct nn_overlay_stats os;

	if (buf == NULL || cap == 0u)
		return NN_SVC_ERR_ARG;
	buf[0] = '\0';
	if (ctx != NN_STREAM_LINES_STATS)
		return 0;                        /* nothing to add at start */

	/*
	 * [!] THE ORDER IS LOAD-BEARING.  The caller stops at the first line a
	 * board declines to emit, so an index that returns 0 hides every index
	 * after it.  The stack lines therefore come before the profile split --
	 * which declines whenever the EPK clock is not trusted, a condition that
	 * has nothing to do with them -- and every one of them returns a line,
	 * "not measured" included.  Ten lines at most, against a cap of twelve,
	 * so the caller's "more than N lines" warning never fires on a report
	 * that ended normally.
	 */
	if (index >= 1u && index <= (unsigned)PLUGIN_SLOT_COUNT) {
		struct nn_probe_row row;
		unsigned slot = index - 1u;

		nn_probe_snapshot(slot, &row);
		(void)nn_probe_line(buf, cap, nn_probe_slot_label(slot), &row,
		                    nn_slot_runs[slot], nn_probe_slot_note(slot));
		return 1;
	}

	nn_overlay_stats(&os);
	switch (index) {
	case 0u:
		/* [!] ITEMS, NOT FACES (issue #105).  This is the sum of what the
		 * decoder returned, and since a classifier plugin can hold the panel
		 * the decoder need not be a detector.  The firmware has no decoder at
		 * all (issue #104), so it cannot name what it counted. */
		nn_detail_to(buf, cap, "items   : %lu decoded since the stream started",
		             (unsigned long)os.detections);
		return 1;
	case 1u + (unsigned)PLUGIN_SLOT_COUNT:
		/* What the plugin's draw() spent of its painter budget, so the cap
		 * can be judged rather than argued about (issue #103).  Always a
		 * line, for the same reason as the stack lines above. */
		if (os.draw_spent != 0u || os.draw_refused != 0u)
			nn_detail_to(buf, cap,
			             "painter : at most %lu px in one frame, %lu refused",
			             (unsigned long)os.draw_spent,
			             (unsigned long)os.draw_refused);
		else
			nn_detail_to(buf, cap, "painter : nothing drawn yet");
		return 1;
	case 2u + (unsigned)PLUGIN_SLOT_COUNT:
		/* The producer-side split (issue #60).  Only when the clock behind it
		   is trusted -- an untrusted number here would be read as a
		   measurement.  Last, because it may decline. */
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
_Static_assert(3u + (unsigned)PLUGIN_SLOT_COUNT < (unsigned)NN_STREAM_LINES_MAX,
               "the stream report must end before the caller's line cap");

int nn_svc_thresh_get(unsigned *milli)
{
	/* Always answers: nothing is taken here, so there is nothing to be busy
	 * on (whether that is safe is issue #122 P5, not this contract). */
	if (milli == NULL)
		return NN_SVC_ERR_ARG;
	*milli = nn_active_get_thresh_milli();
	return NN_SVC_OK;
}

int nn_svc_thresh_set(unsigned milli)
{
	switch (nn_active_set_thresh_milli(milli)) {
	case NN_ACTIVE_THRESH_OK:
		return NN_SVC_OK;
	case NN_ACTIVE_THRESH_NO_DECODER:
		/* Not an argument error: the value was fine and there is nothing here
		 * to hold it (issue #104). */
		return NN_SVC_ERR_STATE;
	default:
		return NN_SVC_ERR_ARG;
	}
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
	/* [!] 80 WAS TOO SHORT, AND TRUNCATION ATE THE LINE ENDING.  The image line
	 * ran past it on the hardware and came out as
	 *   "... (reservation 131072 B at 0x  code 2400 B ..."
	 * -- cut mid-number AND missing its CRLF, so the next line ran on.  A
	 * bounded formatter drops what does not fit, and what did not fit was the
	 * terminator that separates this line from the next. */
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

	if (!nn_has_container) {
		/* Says so rather than leaving the line out: a missing line reads as a
		 * board with no plugin support at all, which is a different and wrong
		 * fact. */
		(void)nn_info_line(write, ctx,
		                   "plugin  : (none) -- reservation %lu B at 0x%08lx\r\n",
		                   (unsigned long)size, (unsigned long)base);
		return;
	}

	/*
	 * [!] THE LAST FIELD IS THE ONE TO READ.  The rest come from a validated
	 * manifest, which says what the container CLAIMS; "loaded" is the only word
	 * here that means anything branched into the image.  A container can be
	 * parsed and reported and still not be running -- a load that failed after
	 * the parse, or one that carries no plugin at all.
	 *
	 * And the CRC is the identity that moves.  The build id is a source
	 * revision stamped at configure time, so editing a plugin and rebuilding
	 * does not change it, and the image size often does not change either.
	 */
	if (nn_info_line(write, ctx, "plugin  : %s (build %s, crc %08lx), %s\r\n",
	                 nn_container.name, nn_container.build_id,
	                 (unsigned long)nn_container.digest,
	                 plugin_run_active() ? "loaded" : "not loaded") < 0)
		return;
	if (nn_info_line(write, ctx, "  image : %lu B file / %lu B mem, link 0x%08lx\r\n",
	                 (unsigned long)nn_container.file_size,
	                 (unsigned long)nn_container.mem_size,
	                 (unsigned long)nn_container.link_addr) < 0)
		return;
	if (nn_info_line(write, ctx, "  where : %lu B reserved at 0x%08lx\r\n",
	                 (unsigned long)size, (unsigned long)base) < 0)
		return;
	if (nn_info_line(write, ctx,
	                 "  code %lu B  data %lu B  bss %lu B\r\n",
	                 (unsigned long)nn_container.code_len,
	                 (unsigned long)nn_container.data_seg_len,
	                 (unsigned long)nn_container.bss_len) < 0)
		return;
	/* What each slot needs at this firmware's c, and what the manifest
	 * declared (issue #111).  See svc/plugin_info.h.  A container with no
	 * plugin section has no stack to describe. */
	if (nn_container.has_plugin)
		(void)plugin_info_stack(write, ctx, &nn_container,
		                        nn_plugin_policy.veneer_cost);
}

/* The active decoder's own report is no longer a call the shared command makes
 * after this one returns (issue #110): it is captured in nn_capture_report(),
 * under the gate, beside the record snapshot it describes (issue #118). */
