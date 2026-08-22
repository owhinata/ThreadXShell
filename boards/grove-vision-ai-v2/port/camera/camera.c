/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * OV5647 camera driver: lifecycle, producer thread and error state machine
 * (issue #35).  See camera.h for the shape of the frame path.
 *
 * THE THREE THINGS THIS FILE IS CAREFUL ABOUT
 *
 * 1. The datapath callback runs in interrupt context and carries NO context but
 *    a status code -- no frame number, no generation, no handle.  So a late
 *    event from a stream that has already been stopped is indistinguishable, at
 *    the callback, from a fresh one.  That is why restarting is a barrier and
 *    not a flag: quiesce the hardware FIRST, then clear, then re-arm.
 *
 * 2. Errors have priority over frames.  A single wakeup can carry both a
 *    frame-ready flag and a terminal status; converting and publishing in that
 *    case would hand a sink a frame produced by a datapath that has already
 *    failed.  The error latch is sticky and is checked first.
 *
 * 3. There is exactly ONE stop sequence (cam_dp_full_stop), and every path
 *    that ends a stream goes through it: the user's stop, a bounded-wait
 *    timeout, a terminal datapath event, and a bring-up that failed half way.
 *    Multiple stop paths is how a peripheral ends up in a state nobody wrote
 *    down.
 */
#include <stddef.h>
#include <stdint.h>

#include "tx_api.h"

#include "WE2_core.h"          /* hx_InvalidateDCache_by_Addr */
#include "WE2_device.h"        /* NVIC                        */
#include "sensor_dp_lib.h"

#include "cli_config.h"

#include "camera.h"
#include "cam_auto.h"
#include "cam_convert.h"
#include "cam_dp.h"
#include "cam_edm.h"
#include "cam_sensor.h"
#include "cam_state.h"
#include "cam_wdma3.h"
#include "epk_irq_wrap.h"
#include "timer_seam.h"
#include "tx_glue.h"
#include "frame.h"
#include "frame_pipeline.h"

#define LOG_TAG "camera"
#include "log.h"

/* ---- tunables ------------------------------------------------------------ */

/*
 * The producer sits above the console (16) so that a frame is converted and
 * published promptly, and -- since issue #64 -- BELOW the panel thread the LCD
 * sink owns.  That blit sleeps on its DMA for nearly all of its 26 ms, so
 * letting it preempt this thread costs under a millisecond a frame, and it is
 * what lets the display actually use the period this thread produces.
 * CAM_PRODUCER_PRIO itself lives in camera.h, because that second thread
 * asserts against it.
 */
/*
 * [!] 8 KiB, not 4, since issue #48: this thread now runs INFERENCE.
 *
 * A sink's consume() may call TFLM's Invoke() -- the whole interpreter plus the
 * ethos-u driver plus the BlazeFace decoder -- and that call chain has only
 * ever run on a 4 KiB shell or background-job stack.  The producer's own peak
 * was measured at 568 B before this, so the old allocation was sized for a
 * thread that did almost nothing.
 *
 * MEASURED at issue #64: 908 B of 8192, after `nn preview` and five Ctrl+C
 * cancellations -- so putting Invoke() on this thread cost about 340 B over the
 * 568 B of before, and the allocation is nine times what the inference path
 * actually uses.
 *
 * [!] It is deliberately NOT cut to fit that, because 908 B is not this
 * thread's peak -- it is the peak of the paths measured.  The deepest chain
 * here is the timeout RESTART (cam_quiesce() plus cam_start_datapath(), i.e.
 * the vendor's mode-table writes), and every run so far reports `restarts 0`,
 * so nothing has walked it.  Nor is there pressure to: check_placement_budget
 * reports over 150 KB of DTCM gap.  Shrink this when a run that restarts has
 * been measured, not before.
 */
#define CAM_PRODUCER_STACK  8192u

/*
 * These stay in this PORT and not in shell/include/cli_config.h.  That header is
 * the shared shell core's configuration -- instances, the job pool, the
 * transport -- and all three boards include it; a Grove-only camera thread's
 * numbers in there would be board specifics in a board-independent header, which
 * is the layering rule backwards.  The precedent is the same on the other two
 * boards, whose camera producers carry their own numbers locally.
 *
 * What the shared header IS the authority on is where this thread has to sit
 * relative to the console, so that is asserted against it rather than restated.
 * The stack stays private to this file; only the priority is exported, because
 * only the priority is part of a relationship another file has to honour.
 */
_Static_assert(CAM_PRODUCER_PRIO < CLI_INSTANCE_PRIORITY,
               "the camera producer must outrank the console: at or below it, "
               "a frame waits behind whatever the shell is printing");
_Static_assert(CAM_PRODUCER_PRIO > 0u,
               "priority 0 would let the producer starve everything, console "
               "included, for as long as a frame takes");
_Static_assert((CAM_PRODUCER_STACK % 8u) == 0u,
               "ThreadX wants an 8-byte aligned stack size");
_Static_assert(CAM_PRODUCER_STACK >= 1024u,
               "the producer's deepest call is a sink's consume(), which runs "
               "TFLM's Invoke() under `nn preview`; anything this small has not "
               "been thought about");

/*
 * How long to wait for a frame before deciding the datapath has stopped
 * producing.  The sensor runs well above 10 fps even in the rev-C bounce case,
 * so 2 seconds is "something is wrong", not "the exposure is long".
 */
#define CAM_FRAME_TIMEOUT_TICKS (2u * TX_TIMER_TICKS_PER_SECOND)

/*
 * How long camera_stream_stop() waits for the producer to acknowledge.
 *
 * [!] THIS IS NOT A CORRECTNESS PARAMETER (issue #48).  It used to be, back
 * when a timeout was followed by an unconditional detach -- and it was already
 * too short for that job, since a sink may now run an NPU inference inside
 * consume().  Correctness moved to CAM_ST_LOST: whatever this number is, an
 * unacknowledged stop poisons the camera instead of racing the producer.
 *
 * So it only has to be long enough that a HEALTHY system never reaches it.
 * The producer's worst iteration after a stop request is bounded by the pieces
 * it can wait on -- up to two NPU semaphore waits (the ethos-u driver takes it
 * a second time on its timeout/interrupt race path) at NPU_INFERENCE_TIMEOUT_
 * TICKS each, plus the panel's DMA timeout, plus the vendor quiesce -- and the
 * frame wait itself is short-circuited by the semaphore the stop posts.  Six
 * seconds clears that sum with room; a real inference is 13 ms.
 */
#define CAM_STOP_JOIN_TICKS (6u * TX_TIMER_TICKS_PER_SECOND)

/*
 * How long camera_stream_stop() waits for the API MUTEX -- which is a different
 * question from the one above (issue #65).
 *
 * Every other entry point takes the mutex with TX_NO_WAIT, because for them a
 * refusal is cheap and honest.  The stop cannot do that: by camera.h's rule a
 * caller reads anything but CAM_OK as "the producer may still be inside my
 * sink", so a collision that lasted microseconds made a preview abandon its own
 * sink for ever.  Waiting is safe here -- the producer never takes this mutex,
 * and no holder waits on the producer or the panel thread, so there is no cycle.
 *
 * Bounded by the worst hold a HEALTHY system can have, which is the larger of:
 *
 *   - a concurrent stop, which holds the mutex for its whole join
 *     (CAM_STOP_JOIN_TICKS, 6 s), and
 *   - a one-shot capture, which holds it for a bring-up (the 100 ms Timer0
 *     delivery probe dominates) plus CAM_FRAME_TIMEOUT_TICKS (2 s) plus the
 *     quiesce -- under 3 s.
 *
 * So the join dominates, and 8 s clears it with room for scheduling.  Like the
 * join deadline this is NOT a correctness parameter: reaching it means a holder
 * is wedged, and the stop then reports that it never asked rather than guessing.
 */
#define CAM_STOP_LOCK_TICKS (8u * TX_TIMER_TICKS_PER_SECOND)

/*
 * Restart attempts after a timeout before giving up for good.  Bounded on
 * purpose: an endlessly retrying producer turns a dead camera into a board that
 * looks busy, and each retry is a full datapath reconfiguration.
 */
#define CAM_MAX_RETRIES 3u

/* Number of pipeline slots.  Two is the minimum that works: acquire() refuses
 * to return the slot it last published, so a single-slot ring stops after one
 * frame.  See the .cam_slots note in the linker script. */
#define CAM_SLOTS 2u

/* ---- memory -------------------------------------------------------------- */

/*
 * The converted frames.  SRAM and not TCM: the LCD sink hands slot memory
 * straight to the SPI DMA, and the DMA engines on this part cannot see TCM --
 * a slot in DTCM would not fault, it would simply never appear on the glass.
 * Its own linker section so check_placement_budget.py can pin the size.
 */
static uint8_t cam_slot_mem[CAM_SLOTS * CAM_FRAME_PIXELS * 2u]
	__attribute__((section(".cam_slots"), aligned(32)));

_Static_assert(sizeof cam_slot_mem == 307200u,
               "the slot arena is not two 320x240 RGB565 frames");

static TX_THREAD cam_producer;
static UCHAR     cam_producer_stack[CAM_PRODUCER_STACK]
	__attribute__((aligned(8)));

/* ---- state --------------------------------------------------------------- */

/*
 * The states, and the stop's decision over them, live in cam_state.h -- see the
 * note there for why the decision is a pure function and not inline below.
 * CAM_ST_LOST's contract is documented beside the enum.
 */

static struct frame_pipeline cam_pipe;
static TX_MUTEX     cam_pipe_mutex;
static TX_SEMAPHORE cam_frame_sem;   /* posted by the datapath callback   */
static TX_SEMAPHORE cam_start_sem;   /* thread -> producer: run           */
static TX_SEMAPHORE cam_stopped_sem; /* producer -> thread: idle again    */
static TX_MUTEX     cam_api_mutex;   /* serialises the public entry points */
static int          cam_objects_ok;

/*
 * [!] volatile, and for one specific reason (issue #65): camera_stream_stop()
 * reads this AGAIN after waiting on the API mutex, and that second read has to
 * be a real load.  This is a file-static whose address is never taken, so a
 * compiler is entitled to keep it in a register across the wait -- and a stale
 * read there would answer with the state from BEFORE another thread poisoned
 * the port, which is precisely the case the re-test exists to catch.  The
 * current build does emit the reload; volatile is what stops that from being a
 * property of this toolchain rather than of the program.
 */
static volatile enum cam_state cam_state;
static uint8_t  cam_stop_req;
static uint8_t  cam_datapath_configured;
static uint8_t  cam_rev_c;
/* Next datapath configuration uses the RAW leg (INP -> WDMA2, no demosaic). */
static volatile uint8_t cam_raw_mode;

/*
 * Interrupt-owned.  `cam_err` is STICKY: once a terminal status has been seen
 * it is never overwritten by a later frame-ready, which is what makes "check
 * errors first" a real precedence rule rather than a race.
 */
static volatile int32_t  cam_err;
static volatile uint8_t  cam_frame_ready;
static volatile uint32_t cam_evt_benign;

/* Counters (producer thread only, except cam_evt_benign above). */
static uint32_t cam_frames;
static uint32_t cam_timeouts;
static uint32_t cam_retries;
static uint32_t cam_csirx_errors;
static uint32_t cam_relock_fails;
static uint32_t cam_dp_errors;
static int32_t  cam_last_dp_status;

/*
 * How often a stop found the API mutex held, and the longest it then waited
 * (issue #65).
 *
 * NOT producer-owned, and not mutex-protected either: the whole point is that
 * they are written by a thread which has just failed to take the mutex, and two
 * stops can be at that boundary at once.  So the pair is updated -- and read
 * back in camera_stream_stats() -- inside a short TX_DISABLE, the same way the
 * panel sink's hand-off counters are.
 *
 * The wait includes the attempts that went on to TIME OUT: that is the
 * observation worth having, since it is the one the operator sees as a refusal.
 */
static uint32_t cam_lock_contended;
static uint32_t cam_lock_wait_max;

/* ---- per-stage profile (issue #38) --------------------------------------- */

/*
 * [!] MEASURE FIRST.  `camera preview` runs at about 10 fps and the reason was
 * an ESTIMATE: 153,600 bytes over a 48 MHz SPI link is 25.6 ms of DMA, so the
 * other ~75 ms went unaccounted for and the theory on file was that the CPU
 * touches every pixel twice.  Plausible is not measured, and the alternative --
 * that the SENSOR simply does not offer frames any faster, in which case
 * pipelining buys nothing at all -- is equally plausible from the armchair.
 * So the producer times its own stages and `camera stats` prints them.
 *
 * [!] TIMER2, NOT DWT CYCCNT.  The biggest single candidate is this thread
 * ASLEEP waiting for frame-ready, and CYCCNT stops in WFI (which BSP_ENABLE_WFI
 * makes the idle thread do).  A cycle counter would report that stage as free
 * and the picture would be exactly wrong.  TIMER2 is the EPK's free-running
 * source and keeps counting through sleep -- proven on hardware by `epk sleep`
 * in M-G2 -- so it is the one source that can time both halves of this loop.
 * It is read, never written: the kit owns it.
 *
 * Accumulated in ticks and converted only for display, because the frequency is
 * a runtime read-back and the division belongs where it can be reported.
 * Reset at every stream start, unlike the counters above, which are cumulative:
 * a mean that mixes two previews taken at different exposures is not a
 * measurement of either.
 */
struct cam_prof {
	uint64_t wait;    /* semaphore: sensor exposure + readout + datapath */
	uint64_t inval;   /* D-cache invalidate of one 225 KB landing buffer */
	uint64_t arm;     /* mask/disable/program/audit/retrigger (#59)      */
	uint64_t tune;    /* means, sensor read-back, white balance step     */
	uint64_t pack;    /* planar B/G/R -> RGB565 with wb/gamma/saturation */
	uint64_t sink;    /* publish -> sinks consume synchronously (LCD)    */
	uint64_t total;   /* loop top to loop top: the frame period          */
	uint32_t iters;   /* loop iterations counted into `total`            */
};
static struct cam_prof cam_prof;
static uint32_t cam_prof_last_top;
static uint8_t  cam_prof_have_top;

static uint32_t cam_now(void)
{
	return tx_glue_epk_timer_ticks();
}
static uint32_t cam_generation;
static const char *cam_fault;

/*
 * Software white balance, applied by the packer.  Unity by default: a preview
 * that has not been balanced shows what the sensor actually produced, which is
 * what makes `camera capture`'s channel statistics interpretable.  See
 * cam_convert.h for why this is in software at all.
 */
/*
 * Gamma defaults ON, with the black level at 16 beside it.
 *
 * The sensor produces LINEAR samples and the panel expects encoded ones: an
 * ordinary midtone of 49 linear reads as nearly black on the glass.  An earlier
 * build defaulted this OFF, because with the black level still at 0 the curve
 * lifted the pedestal to grey and the picture came out washed out -- which is
 * the pair below, not an argument against the curve.  With both in place it
 * settled on the bench.
 *
 * CAM_AE_TARGET_* follows this switch, because the loop aims at what the PANEL
 * shows and the curve is what sits in between.
 *
 * Neither the level nor the curve is taste, and with the curve on they are not
 * separable:
 *
 *  - the sensor produces LINEAR samples and the panel expects encoded ones, so
 *    linear is not a neutral starting point, it is the wrong units;
 *  - but the sRGB curve has a slope of 12.92 at the origin, so it multiplies
 *    whatever pedestal is still in the data.  With the black level left at 0, a
 *    pedestal of 16 encodes to 71 and the picture comes out visibly WASHED OUT
 *    -- blacks lifted to grey.  Observed exactly that way on hardware when
 *    gamma was turned on by itself.
 *
 * 16 is not a guess tuned to one scene -- but the reason this used to give was
 * the WRONG SENSOR's (issue #61).  It quoted a datasheet fixing the pedestal at
 * 64 in RAW10, which is the IMX219's, written when this port drove both parts
 * and "the datasheet" did not say which; the OV5647's has no such sentence, and
 * the IMX219 left in issue #54.  The value survives the correction because the
 * part and the bench agree on it:
 *
 *  - the OV5647's BLC target (0x4009) powers up at 0x10 = 16, and the SDK mode
 *    table this port includes writes 0x4000, 0x4001 and 0x4004 but never
 *    0x4009 -- so the part runs on that default;
 *  - measured here on a dark frame with `camera auto off`, the floor spans
 *    8..16 with per-plane means of 12.9 to 13.6.  `camera raw` says the same
 *    per Bayer phase (13.0..14.0) with the demosaic out of the path, so the two
 *    routes agree independently.
 *
 * The default is the TOP of that range rather than its mean, and that is the
 * choice: the curve multiplies whatever is LEFT, so subtracting the mean leaves
 * one to three counts that encode to 13..28 out of 255.  Confirmed on hardware
 * -- at `camera black 13` the blacks do not close.  The same measurement rules
 * out reading 0x4009 as a 10-bit number (a target of 4 would put the floor near
 * 4, and it does not).
 *
 * Raising it further trades shadow detail for contrast and is a scene decision
 * -- `camera black <n>`.
 */
#define CAM_BLACK_LEVEL_DEFAULT 16u

/*
 * Saturation stands in for the colour correction matrix this pipeline does not
 * have (see cam_convert.h).  1.8x is in the range a real CCM works out to for a
 * sensor of this class -- enough to undo the filter crosstalk that makes raw
 * sensor RGB look pale, and short of the point where skin goes lurid.  It is an
 * approximation of a measurement nobody here has taken, which is why it is a
 * knob and not a constant.
 */
#define CAM_SAT_DEFAULT 600u

static struct cam_wb cam_wb = { CAM_WB_UNITY, CAM_WB_UNITY, CAM_WB_UNITY,
                                CAM_BLACK_LEVEL_DEFAULT, 1u,
                                CAM_SAT_DEFAULT };

/*
 * Auto mode, ON by default: the sensor's own AEC/AGC plus this port's software
 * white balance.
 *
 * The datapath provides neither, so without this a preview is correct only in
 * the one lighting condition somebody last tuned it for -- which is not a preview,
 * it is a photograph of a settings file.  Off is available for measurements
 * that need the sensor held still (`camera capture` statistics, comparing two
 * Bayer phases), which is the one case where a loop quietly changing the
 * exposure underneath you invalidates the numbers.
 *
 * The loops run on the producer thread, between frames, from means measured on
 * the frame just published.  Every fourth frame rather than every frame: the
 * sensor applies exposure a frame or two late, so measuring after every change
 * would feed the loop its own transient.
 */
static uint8_t cam_auto_on = 1u;
static uint32_t cam_auto_phase;

#define CAM_AUTO_EVERY 4u
/* Subsample for the measurement: 4,800 pixels of 76,800 is ample for steering a
 * damped loop, and costs about a sixteenth of the memory traffic. */
#define CAM_AUTO_STEP 16u

/*
 * Pending sensor tuning, handed from the console to the producer.
 *
 * [!] WHY NOT JUST WRITE THE REGISTERS FROM THE COMMAND.  Sensor I2C goes
 * through the vendor CIS driver, which has no locking of its own, and the
 * producer uses it too -- stream on/off around a restart, and on a rev-C part
 * once per frame.  A `camera exposure` typed during a live preview would then
 * interleave two multi-register transactions in that driver, which corrupts
 * whichever one loses and can take the stream down with it.
 *
 * A mutex would work, but this is better: while a stream is running the
 * PRODUCER is the only thread that touches the sensor, and it applies pending
 * changes between frames.  That is also when a rolling-shutter sensor wants its
 * exposure changed -- a mid-frame write gives one torn frame with two exposures
 * in it.  When no stream is running the caller applies it directly, under the
 * API mutex, because then it is the only thread there is.
 */
static volatile uint8_t  cam_tune_req;      /* bit0 exposure, bit1 gains */
static volatile uint16_t cam_tune_exposure;
static volatile uint8_t  cam_tune_again;
static volatile uint16_t cam_tune_dgain;
static volatile uint8_t  cam_tune_auto;
static volatile uint16_t cam_tune_vts;

#define CAM_TUNE_EXPOSURE  0x1u
#define CAM_TUNE_GAINS     0x2u
#define CAM_TUNE_AUTO      0x10u
#define CAM_TUNE_VTS       0x20u

/* Producer side: take whatever is pending and apply it.  Thread context, and
 * only ever called between frames. */
static void cam_apply_tuning(void)
{
	TX_INTERRUPT_SAVE_AREA
	uint8_t req;

	if (cam_tune_req == 0u)
		return;

	/* Claim the request atomically against a console thread setting more
	 * bits: a plain read-then-clear can drop one that lands in between. */
	TX_DISABLE
	req = cam_tune_req;
	cam_tune_req = 0u;
	TX_RESTORE

	/*
	 * The sensor half of `camera auto` -- taking a part's own AEC/AGC off is
	 * an I2C write like any other, and the console may not make it itself
	 * while this thread owns the CIS driver.
	 *
	 * [!] FIRST, and it used to be last (issue #70).  Two things in one pass
	 * need it that way round.  The frame length caps a MANUAL exposure to
	 * fit, so it has to be applied after the pass has settled whose exposure
	 * it is -- with the old order, `camera auto off` followed by `camera vts`
	 * capped nothing, because the sensor's loop was still nominally running
	 * when the frame length went in.  And a manual exposure queued in the
	 * same pass now wins over an `auto on` queued with it, which is the rule
	 * issue #39 wrote down: a manual write takes auto off.  The old order
	 * had the opposite effect for that combination.
	 */
	if ((req & CAM_TUNE_AUTO) != 0u)
		(void)cam_sensor_set_auto(cam_tune_auto);
	if ((req & CAM_TUNE_EXPOSURE) != 0u)
		(void)cam_sensor_set_exposure(cam_tune_exposure);
	if ((req & CAM_TUNE_GAINS) != 0u)
		(void)cam_sensor_set_gains(cam_tune_again, cam_tune_dgain);
	if ((req & CAM_TUNE_VTS) != 0u)
		(void)cam_sensor_set_frame_length(cam_tune_vts);
}

/*
 * Console side: hand one change over, value and request bit together.
 *
 * [!] AND THE API MUTEX PROTECTS NONE OF THIS (issue #77).  Since #77 these are
 * called with it held, which makes it look as though they no longer need to
 * care -- but the thread they are racing is the PRODUCER, and the producer
 * never takes that mutex.  `cam_tune_req |= bit` is a read-modify-write, so a
 * producer that claims and clears the word between the load and the store has
 * its claim undone: the bit comes back and the change is applied a second time.
 * Harmless today, because the value fields still hold what was applied and the
 * writes are idempotent -- but it is a lost update, and the next person to
 * queue something that is NOT idempotent would inherit it.
 *
 * TX_DISABLE is what actually serialises the two, and it is the same mechanism
 * cam_apply_tuning() already uses to claim the word.  This port's ThreadX
 * critical sections are PRIMASK-based (TX_PORT_USE_BASEPRI is left undefined in
 * port/threadx/tx_user.h), so masking interrupts also masks PendSV: the
 * scheduler cannot run and the higher-priority producer thread cannot preempt
 * us mid-sequence.  Both stores are covered, not just the bit, so the producer
 * never sees a request bit standing over a value that has not landed yet.
 */
static void cam_tune_queue_exposure(uint16_t lines)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	cam_tune_exposure = lines;
	cam_tune_req |= CAM_TUNE_EXPOSURE;
	TX_RESTORE
}

static void cam_tune_queue_gains(uint8_t again, uint16_t dgain)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	cam_tune_again = again;
	cam_tune_dgain = dgain;
	cam_tune_req |= CAM_TUNE_GAINS;
	TX_RESTORE
}

static void cam_tune_queue_vts(uint16_t lines)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	cam_tune_vts = lines;
	cam_tune_req |= CAM_TUNE_VTS;
	TX_RESTORE
}

static void cam_tune_queue_auto(uint8_t on)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	cam_tune_auto = on;
	cam_tune_req |= CAM_TUNE_AUTO;
	TX_RESTORE
}

/*
 * The interrupts the vendor bring-up turned on, one undo log per measure-then-
 * wrap round -- see cam_wrapped_step() for why there is more than one.
 *
 * The rounds fall into two groups with DIFFERENT LIFETIMES, and conflating them
 * is how the camera becomes single-use:
 *
 *   [0 .. cam_wrapsets_core)          the bring-up: power and the CIS layer.
 *                                     Lives as long as the port is up.
 *   [cam_wrapsets_core .. used)       the datapath: CSI receiver, INP/WDMA,
 *                                     capture start.  Re-run by EVERY start and
 *                                     every timeout retry, so it must be
 *                                     unwound before it is rebuilt -- otherwise
 *                                     the second start finds no free slot and
 *                                     the retries the state machine advertises
 *                                     never happen.
 */
static struct epk_irq_wrapset cam_wrapset[4];
static uint32_t cam_wrapsets_used;
static uint32_t cam_wrapsets_core;

/* ---- error codes --------------------------------------------------------- */

const char *camera_strerror(int rc)
{
	switch (rc) {
	case CAM_OK:             return "ok";
	case CAM_ERR_PARAM:      return "bad argument";
	case CAM_ERR_HAL:        return "sensor or datapath call failed";
	case CAM_ERR_TIMEOUT:    return "no frame arrived";
	case CAM_ERR_STATE:      return "wrong state";
	case CAM_ERR_NO_SENSOR:  return "no sensor";
	case CAM_ERR_NO_FRAME:   return "no frame captured yet";
	case CAM_ERR_BUSY:       return "camera busy";
	case CAM_ERR_LOCKED:     return "the camera API stayed locked";
	default:                 return "unknown error";
	}
}

static void cam_fault_latch(const char *why)
{
	if (cam_fault == NULL)
		cam_fault = why;
}

/* ---- the datapath callback (INTERRUPT CONTEXT) --------------------------- */

/*
 * Registered with hx_dplib_register_cb(), i.e. straight into the library rather
 * than through the SDK's event_handler scheduler -- that layer exists to drive
 * a superloop and has nothing to offer a port with a real scheduler.
 *
 * Everything here has to be safe from an interrupt and must not touch the ring:
 * classify, latch, post.  The classification is deliberately CLOSED -- anything
 * negative that is not recognised is terminal, because the alternative is to
 * keep running a datapath that reported something this port has never seen.
 */
static void cam_dp_callback(SENSORDPLIB_STATUS_E status)
{
	switch (status) {
	case SENSORDPLIB_STATUS_XDMA_FRAME_READY:
		cam_frame_ready = 1u;
		break;

	case SENSORDPLIB_STATUS_XDMA_WDMA1_FINISH:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_FINISH:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_FINISH:
	case SENSORDPLIB_STATUS_XDMA_RDMA_FINISH:
	case SENSORDPLIB_STATUS_RSDMA_FINISH:
	case SENSORDPLIB_STATUS_HOGDMA_FINISH:
	case SENSORDPLIB_STATUS_CDM_MOTION_DETECT:
	case SENSORDPLIB_STATUS_TIMER_FIRE_APP_READY:
	case SENSORDPLIB_STATUS_TIMER_FIRE_APP_NOTREADY:
		/* Progress, not completion.  Counted so that "nothing at all is
		 * happening" and "frames start but never finish" are
		 * distinguishable from the shell. */
		cam_evt_benign++;
		return;

	default:
		/* Every abnormal WDMA status, every 1-bit-parser error
		 * (-100..-108), every EDM WATCHDOG timeout -- and anything
		 * unrecognised.
		 *
		 * [!] WATCHDOG, and only watchdog.  The vendor's own EDM
		 * callback is what turns WDT1/2/3 into the three negative
		 * statuses that land here; EDM TIMING violations do NOT arrive
		 * here and never have, because nothing in the SDK registers
		 * that callback and the datapath configuration masks every
		 * timing bit besides.  Issue #68 gives that branch an observer
		 * -- see cam_edm.h.  (An earlier version of this comment
		 * claimed both, which is how the gap survived being read.) */
		if (cam_err == 0)
			cam_err = (int32_t)status;
		break;
	}

	(void)tx_semaphore_put(&cam_frame_sem);
}

/* ---- the EDM observer (INTERRUPT CONTEXT) -------------------------------- */

/*
 * The other branch of the same vendor ISR (issue #68).  cam_edm.h has the whole
 * story; what matters here is that this branch was reaching nobody, and that the
 * one line it used to leave is the last thing in the ring before both observed
 * `nn preview` hangs.
 *
 * PURELY OBSERVATIONAL, on purpose.  No cam_err, no stop, no semaphore.  Nothing
 * has shown this event to be fatal -- it may well be an artefact -- and deciding
 * that it is would pre-empt the measurement.  It does not clear anything either:
 * the vendor acknowledges by writing back the status it read, so a zero status
 * clears nothing, and a full clear here would be a fix resting on a mechanism
 * that has not been established yet.
 *
 * [!] AND IT IS NOT TRANSPARENT.  Installing this removes the vendor's xprintf
 * from the exact path that precedes the hangs and puts different work there.  If
 * the hang stops happening after this change, that is not evidence of a fix --
 * only a recurrence, with its record, advances the issue.
 */
static struct cam_edm_state cam_edm;   /* zero == nothing seen (cam_edm.h) */

static void cam_edm_isr(uint32_t status)
{
	struct cam_edm_event ev;

	ev.status     = status;
	ev.tick       = tx_glue_epk_timer_ticks();
	ev.generation = cam_generation;
	cam_dp_edm_read(&ev.mask, ev.wdt);

	if (!cam_edm_note(&cam_edm, &ev))
		return;

	/*
	 * [!] SELF-CONTAINED, because the counters do not survive what recovers
	 * the board.  They are ordinary static storage and the reset wipes them;
	 * only this ring is .noinit.  Reading `camera stats` afterwards would
	 * show zeroes, so every number has to be in the record itself.
	 *
	 * And TERSE, because it has to fit LOG_MSG_MAX: an all-ones rendering of
	 * these eight fields runs to 98 characters against a limit of 104, and an
	 * overlong record is truncated from the END -- so the fields that would
	 * go first are the ones printed last, the tick and the generation.  Six
	 * characters of headroom is why nothing is lost today; adding a field
	 * without re-counting is how that would stop being true.
	 */
	LOG_WRN("edm #%lu s %08lx m %08lx w %lu/%lu/%lu t %lu g %lu",
	        (unsigned long)cam_edm.events, (unsigned long)ev.status,
	        (unsigned long)ev.mask, (unsigned long)ev.wdt[0],
	        (unsigned long)ev.wdt[1], (unsigned long)ev.wdt[2],
	        (unsigned long)ev.tick, (unsigned long)ev.generation);
}

/* ---- pipeline plumbing --------------------------------------------------- */

static void cam_pipe_lock(void *ctx)
{
	(void)ctx;
	(void)tx_mutex_get(&cam_pipe_mutex, TX_WAIT_FOREVER);
}

static void cam_pipe_unlock(void *ctx)
{
	(void)ctx;
	(void)tx_mutex_put(&cam_pipe_mutex);
}

static const struct frame_os cam_pipe_os = {
	NULL, cam_pipe_lock, cam_pipe_unlock,
};

/*
 * Not public, and that is the fix for issue #63: the only way to attach a sink
 * is camera_stream_start(), which does it under the API mutex in the same
 * transaction that decides the camera is free.  An out-of-band subscribe is the
 * hole -- it can put a sink into somebody else's running stream.
 */
static int cam_subscribe(struct frame_sink *sink)
{
	int rc;

	if (sink == NULL || !cam_objects_ok)
		return CAM_ERR_PARAM;
	rc = frame_pipeline_attach(&cam_pipe, sink);
	if (rc != 0) {
		/* Say WHICH (issue #72).  This board is the one the refusal actually
		   protects: the pipeline is initialised ONCE, at object create, so a
		   slot a sink never handed back is gone for good and the ring is short
		   from then on -- silently, until now. */
		LOG_ERR("sink '%s' attach refused: %s",
		        sink->name ? sink->name : "?", frame_pipeline_strerror(rc));
		return CAM_ERR_PARAM;
	}
	return CAM_OK;
}

int camera_unsubscribe(struct frame_sink *sink)
{
	int rc;

	if (sink == NULL || !cam_objects_ok)
		return CAM_ERR_PARAM;
	/*
	 * [!] REFUSED after a lost producer (issue #48).  Detaching is the one
	 * operation that is unsafe precisely when the producer may still be
	 * inside consume(): publish() has already pre-pinned this sink and
	 * called it with the pipeline lock released, so unlinking it now races
	 * a delivery in flight.  Callers are required to have a CONFIRMED stop
	 * first; this is the backstop for the one that forgets.
	 */
	if (cam_state == CAM_ST_LOST)
		return CAM_ERR_STATE;
	/*
	 * [!] AND A RUNNING PRODUCER REFUSES TOO (issue #65).
	 *
	 * The poison test above was the whole backstop, which made it a backstop
	 * for one way of forgetting the rule and not for the obvious one.
	 * Unlinking a sink while the producer is streaming races a delivery that
	 * is already pre-pinned and out of the pipeline's lock: the drain that
	 * follows can read a hand-off count from before that delivery, call the
	 * sink idle, and let its owner tear down state a consume() is about to
	 * use.  Nothing reaches here that way today -- the one caller detaches
	 * only after a confirmed stop -- but "the callers are careful" is the
	 * property this function exists to stop depending on, and CAM_ERR_LOCKED
	 * has just added another return a caller could get this wrong on.
	 */
	if (cam_state == CAM_ST_STREAMING)
		return CAM_ERR_BUSY;
	/*
	 * [!] AND THE CORE'S ANSWER IS NOT DISCARDED ANY MORE (issue #79).  It used
	 * to be, which made this the one place a "retry, a callback is still
	 * running" came back as CAM_OK -- and this board's only caller reads CAM_OK
	 * as "unlinked, go and tear down".
	 *
	 * The split matters more than the value: TRANSITION and QUIESCE mean ask
	 * again, everything else means this will not get better.  Reporting a
	 * retryable one as terminal strands the panel until reboot; reporting a
	 * terminal one as retryable makes an ownership bug retry forever and never
	 * be diagnosed.
	 */
	rc = frame_pipeline_detach(&cam_pipe, sink);
	if (rc >= 0)
		return CAM_OK;
	LOG_ERR("sink '%s' detach refused: %s", sink->name ? sink->name : "?",
	        frame_pipeline_strerror(rc));
	if (rc == FRAME_PIPELINE_ERR_TRANSITION || rc == FRAME_PIPELINE_ERR_QUIESCE)
		return CAM_ERR_BUSY;         /* retryable: ask again in a moment */
	return CAM_ERR_STATE;            /* terminal */
}

int camera_sink_pins(const struct frame_sink *sink)
{
	/* Passed through, negatives included (issue #79): the core answers a
	   negative when it cannot tell -- a transition, a callback that has not
	   returned -- and cam_drain_decide() already treats anything that is not
	   exactly zero as "not drained", so the fail-closed reading is free. */
	if (sink == NULL || !cam_objects_ok)
		return 0;
	return frame_pipeline_sink_pins(&cam_pipe, sink);
}

void camera_frame_put(struct frame_sink *sink, const struct frame_desc *f)
{
	frame_pipeline_put(&cam_pipe, sink, f);
}

/* ---- bring-up and teardown ----------------------------------------------- */

/* Unwind rounds back down to `keep`, newest first. */
static void cam_unwrap_to(uint32_t keep)
{
	while (cam_wrapsets_used > keep) {
		cam_wrapsets_used--;
		grove_epk_irq_unwrap_set(&cam_wrapset[cam_wrapsets_used]);
	}
}

static void cam_unwrap_all(void)
{
	cam_unwrap_to(0u);
	cam_wrapsets_core = 0u;
}

/*
 * Run one vendor step with the measure-then-wrap protocol around it: snapshot
 * the enabled set, let the vendor turn on whatever it turns on, then wrap and
 * register every line that appeared.
 *
 * WHY TWO ROUNDS AND NOT ONE.  The protocol needs interrupts masked between the
 * snapshot and the wrap, because a line that fires in that window would reach a
 * vendor vector the accounting does not know about.  But the sensor's mode
 * table is several hundred I2C register writes -- something like ten
 * milliseconds -- and masking interrupts across that would stall the console
 * and drop ThreadX ticks.  So the two hardware bring-up steps get a masked
 * round each, and the slow I2C traffic happens in between with interrupts
 * enabled and round one already accounted for.  Round two's snapshot is taken
 * after round one's lines are live, so it does not try to wrap them twice
 * (which grove_epk_irq_wrap_new() would refuse outright anyway).
 */
static int cam_wrapped_step(int (*step)(void))
{
	struct epk_irq_snapshot snap;
	TX_INTERRUPT_SAVE_AREA
	uint32_t i;
	int rc, wrapped;

	if (cam_wrapsets_used >= (sizeof cam_wrapset / sizeof cam_wrapset[0])) {
		cam_fault_latch("more camera bring-up rounds than wrapsets");
		return -1;
	}

	TX_DISABLE
	grove_epk_irq_snapshot(&snap);
	rc = step();
	wrapped = grove_epk_irq_wrap_new(&snap, &cam_wrapset[cam_wrapsets_used]);
	/*
	 * [!] And re-take anything an EARLIER round wrapped that this step has
	 * re-registered underneath us.  The measured-ISER protocol only sees
	 * lines that became ENABLED; a vendor call that installs a fresh ISR on
	 * a line it already had running is invisible to it, and the result is a
	 * line that still works and is no longer accounted -- `thread` then
	 * reports the whole cpu% column as untrustworthy for as long as the
	 * stream runs, which is exactly what hardware showed.
	 */
	for (i = 0u; i < cam_wrapsets_used; i++)
		if (!grove_epk_irq_reassert(&cam_wrapset[i]))
			wrapped = 0;
	TX_RESTORE

	/* Count the round even when the wrap failed: it may have wrapped some
	 * lines before failing, and those have to be undone. */
	cam_wrapsets_used++;

	if (!wrapped) {
		cam_fault_latch("a camera interrupt could not be accounted for");
		return -1;
	}
	return rc;
}

static int cam_step_power_on(void) { return cam_sensor_power_on(); }

/*
 * The datapath's two rounds.  Grouped this way because the vendor calls in each
 * are pure register programming (microseconds, no waiting), so they cost
 * nothing to run with interrupts masked -- while the sensor's I2C mode traffic
 * between them must not be.
 *
 * Everything that can turn an interrupt on has to be INSIDE a round.  The CSI
 * receiver is the obvious one, but the INP/WDMA configuration and the sensor
 * controller's start both enable lines of their own (the EDM watchdog among
 * them), and a line enabled outside a round is exactly the "enabled but
 * unwrapped" state AGENTS.md bars -- it would bill its runtime to whichever
 * thread it interrupted and turn every cpu% number the shell prints into
 * fiction.
 */
static int cam_step_dp(void)
{
	if (cam_dp_csirx_enable() != 0)
		return -1;
	return cam_raw_mode ? cam_dp_config_raw()
	                    : cam_dp_config();
}

/*
 * Capture start, and then the EDM observer (issue #68) -- IN THAT ORDER, INSIDE
 * THIS ROUND, and nowhere else.
 *
 * The start is what reaches the vendor's watchdog configuration, which is what
 * registers the vendor's EDM callback and ENABLES IRQ 143.  Installing straight
 * after it makes this port the last writer of that vector, and the round's
 * wrap-then-reassert step (see cam_wrapped_step) adopts the line either way: as
 * a fresh one if this round enabled it, or through the re-assert path if the
 * datapath round had already.  Outside a round this would be the "enabled but
 * unwrapped" state AGENTS.md bars -- see cam_dp_edm_observe().
 *
 * Only on success.  A failed start goes straight to the teardown, and installing
 * an observer on the way out could enable a line the vendor never did -- which
 * would spend an accounting slot to watch hardware that is being shut down.
 */
static int cam_step_capture(void)
{
	int rc = cam_dp_capture_start();

	if (rc == 0)
		cam_dp_edm_observe(cam_edm_isr);
	return rc;
}

/*
 * Bring the port up.  A TRANSACTION: every failure path leaves the hardware and
 * the interrupt accounting exactly as they were, because the alternative --
 * lines wrapped and registered while the peripheral underneath them is being
 * torn down -- is what makes cpu% read "--" until the next reboot.
 *
 * What survives afterwards is only "brought up": power, interrupt wrapping,
 * a passed Timer0 probe and an identified sensor.  The DATAPATH configuration
 * deliberately does not survive, see cam_start_datapath().
 */
/*
 * Abandon a bring-up, through the same stop sequence as everything else.
 *
 * The ORDER is the point.  The full stop runs FIRST, while this port's
 * interrupts are still wrapped and registered, so whatever it quiesces is
 * accounted for while it happens; only then are the wrappers removed and the
 * module powered down.  Doing it the other way round -- unwrap, then stop --
 * would have the vendor's stop sequence touching a peripheral whose lines have
 * just had their vectors moved back, which is the same shape of bug the panel's
 * teardown was written to avoid.
 */
static int cam_bringup_fail(const char *why, int rc)
{
	cam_fault_latch(why);
	cam_dp_full_stop();
	cam_unwrap_all();
	cam_sensor_power_off();
	cam_state = CAM_ST_DOWN;
	return rc;
}

static int cam_bringup(void)
{
	struct camera_probe_info info;
	uint16_t id = 0u;

	if (cam_state == CAM_ST_READY || cam_state == CAM_ST_STREAMING)
		return CAM_OK;

	cam_fault = NULL;

	/*
	 * [!] UNWIND, do not just forget.  Reaching here from CAM_ST_FAULTED
	 * means a previous bring-up's wrappers are still installed and their
	 * lines still enabled; zeroing the counter would drop the undo log while
	 * leaving them registered.  They would then be absent from the next
	 * ISER delta -- already enabled, so not "fresh" -- and could never be
	 * unwound again, which breaks recovery and leaves the accounting
	 * registry describing a peripheral that has been torn down underneath
	 * it.  Recovery is meant to be "run `camera preview` again", not
	 * "reboot".
	 */
	cam_unwrap_all();

	if (cam_wrapped_step(cam_step_power_on) != 0)
		return cam_bringup_fail("the camera could not be powered up",
		                        CAM_ERR_HAL);

	/*
	 * The Timer0 interrupt-delivery probe (issue #35, carried over from
	 * M-G3a).  Here, and not inside the masked rounds above: it waits for
	 * its own interrupt, so under PRIMASK it could only ever time out and
	 * report the hardware broken.
	 *
	 * The datapath library paces capture with Timer0, so a timer that is
	 * armed and believed but never delivers is a stall with no diagnosis.
	 * Refuse the whole bring-up instead -- the reason is latched in the
	 * seam and `epk` prints it.
	 */
	if (!grove_timer_seam_probe_delivery()) {
		LOG_ERR("timer0 interrupt delivery probe failed: %s",
		        grove_timer_seam_fault() != NULL ?
		        grove_timer_seam_fault() : "(no reason latched)");
		return cam_bringup_fail("the Timer0 interrupt is not delivered",
		                        CAM_ERR_HAL);
	}

	/* Slow I2C, interrupts enabled, round one already accounted for. */
	if (cam_sensor_init() != 0)
		return cam_bringup_fail("the sensor did not accept its mode table",
		                        CAM_ERR_NO_SENSOR);
	/*
	 * [!] Re-state the auto mode at the sensor, every bring-up.
	 *
	 * The mode table just went back in, and for a part that runs its own
	 * AEC that table is what ENABLES it -- so a bring-up silently undoes an
	 * earlier `camera auto off` and manual exposure stops holding, with
	 * nothing said.  This is also the first point at which the request can
	 * be honoured at all when it arrived before any probe: until detection
	 * ran, "the sensor" was a default descriptor and not the fitted part.
	 */
	if (cam_sensor_set_auto((int)cam_auto_on) != 0)
		return cam_bringup_fail("the sensor kept its own exposure loop",
		                        CAM_ERR_HAL);
	/* Against the DETECTED part's ID, not a hard-coded one: detection has
	 * already established which sensor is in the connector, and re-checking
	 * against the other one's number is how a working OV5647 gets reported
	 * as "the sensor did not identify itself". */
	if (cam_sensor_read_id(&id) != 0 || id != cam_sensor_id()) {
		LOG_ERR("sensor model id 0x%04X, expected 0x%04X", id,
		        cam_sensor_id());
		return cam_bringup_fail("the sensor did not identify itself",
		                        CAM_ERR_NO_SENSOR);
	}

	info.chip_version = cam_dp_chip_version();
	cam_rev_c = cam_dp_needs_rev_c_bounce() ? 1u : 0u;

	/* hx_dplib_register_cb() and not the SDK's event_handler layer: this
	 * port has a scheduler and does not want the vendor's superloop. */
	hx_dplib_register_cb(cam_dp_callback, SENSORDPLIB_CB_FUNTYPE_DP);

	/* Everything wrapped so far belongs to the port, not to a stream. */
	cam_wrapsets_core = cam_wrapsets_used;

	cam_state = CAM_ST_READY;
	cam_datapath_configured = 0u;
	LOG_INF("camera up (chip %08lx%s)",
	        (unsigned long)info.chip_version, cam_rev_c ? ", rev C" : "");
	return CAM_OK;
}

/*
 * Configure the datapath and start capture.
 *
 * Re-run from scratch every time, because the stop sequence performs a datapath
 * software reset and NOTHING in the SDK says which of these registers survive
 * it.  Keeping a "already configured, just retrigger" fast path would be an
 * assumption with no evidence behind it and a failure mode (a datapath running
 * on half-reset configuration) that looks like bad wiring.  If it is ever shown
 * on hardware that the configuration does survive, that is an optimisation for
 * its own issue.
 */
static int cam_start_datapath(void)
{
	/*
	 * Give the previous start's datapath rounds back first.  They wrapped
	 * the same lines this one is about to bring up, and the wrap refuses a
	 * line that is already wrapped -- so without this, the second start (and
	 * the first timeout retry, which is the same code) runs out of rounds
	 * and fails, making the camera single-use and the retry budget
	 * decorative.  Unwinding also disables those lines, which is what makes
	 * them "fresh" again in the next round's ISER delta.
	 */
	cam_unwrap_to(cam_wrapsets_core);

	/* Clear the latched receiver status while the receiver is down: it
	 * latches, so a poll taken later without this reports the PREVIOUS
	 * link. */
	if (cam_dp_csirx_clear_errors() != 0) {
		cam_fault_latch("the CSI receiver error status would not clear");
		return CAM_ERR_HAL;
	}

	if (cam_wrapped_step(cam_step_dp) != 0) {
		cam_fault_latch("the datapath refused its configuration");
		return CAM_ERR_HAL;
	}
	cam_datapath_configured = 1u;

	/* Sensor I2C: outside the masked round, as in the bring-up. */
	if (cam_sensor_stream_on() != 0) {
		cam_fault_latch("the sensor would not start streaming");
		return CAM_ERR_HAL;
	}

	if (cam_wrapped_step(cam_step_capture) != 0) {
		cam_fault_latch("the sensor controller would not start");
		return CAM_ERR_HAL;
	}
	return CAM_OK;
}

/*
 * Quiesce: end a stream and leave the datapath cold.
 *
 * TWO ordering rules meet here, and getting either one backwards has been
 * observed on hardware.
 *
 * 1. STOP BEFORE CLEAR.  Clearing latched status and pending interrupts while
 *    the datapath is still running just means the next event lands after the
 *    clear -- and on a restart it arrives as a frame-ready belonging to the
 *    stream that was supposed to be over.  The callback carries no generation
 *    tag, so nothing downstream could tell the difference.
 *
 * 2. [!] UNWRAP BEFORE THE VENDOR TEARDOWN.  This is the lesson lcd_teardown()
 *    already carries -- "unwrap the accounted interrupts BEFORE closing the
 *    device" -- and the camera was written without it.  On hardware the result
 *    was immediate and permanent: after one `camera preview`, `thread` printed
 *    "cpu% unavailable (an accounted interrupt vector was replaced)" until the
 *    next reboot.  cam_dp_full_stop() IS the vendor's close, and it moves
 *    vectors the accounting registry is still holding pointers to.
 *
 * They compose, because masking is what makes the middle safe: the lines are
 * disabled first, so nothing can fire while the vectors are being handed back
 * or while the vendor tears the datapath down.  The lines are then left DOWN --
 * the next cam_start_datapath() re-enables them inside a measure-then-wrap
 * round, which is the only way they may come back up.
 */
static void cam_quiesce(void)
{
	int lines[GROVE_EPK_WRAP_MAX];
	uint32_t nlines = 0u;
	uint32_t s, i;

	/* The datapath rounds' lines, remembered before the undo logs are
	 * consumed below.  Only the datapath ones: the bring-up rounds belong
	 * to "the port is up" and outlive any single stream. */
	for (s = cam_wrapsets_core; s < cam_wrapsets_used; s++)
		for (i = 0u; i < cam_wrapset[s].count; i++)
			if (nlines < (uint32_t)GROVE_EPK_WRAP_MAX)
				lines[nlines++] = cam_wrapset[s].irqn[i];

	/* 1. mask, so nothing fires during any of what follows */
	for (i = 0u; i < nlines; i++)
		NVIC_DisableIRQ((IRQn_Type)lines[i]);
	__DSB();
	__ISB();

	/* 2. hand the vectors back while they are still ours to hand back */
	cam_unwrap_to(cam_wrapsets_core);

	/*
	 * 2b. and take the EDM observer out BEFORE the vendor stop, not after
	 * (issue #68).  That stop drops the vendor's own EDM callback, and the
	 * vendor only disables IRQ 143 when BOTH callbacks are NULL -- so an
	 * observer left installed here would quietly stop the stop from
	 * disabling the line.  The line is already masked by step 1, so this is
	 * about not changing vendor behaviour rather than about safety, which is
	 * exactly why it would never have been noticed.
	 *
	 * Every teardown this port has arrives here -- normal stop, producer
	 * error, timeout restart, start failure, retry exhaustion -- so this one
	 * placement covers them all.  The bring-up failure path needs nothing:
	 * it runs before the capture round, so nothing is installed yet.
	 */
	cam_dp_edm_observe(NULL);

	/* 3. now the vendor may do as it likes with them */
	cam_dp_full_stop();
	cam_datapath_configured = 0u;

	/* 4. and only now is clearing meaningful */
	(void)cam_dp_csirx_clear_errors();
	for (i = 0u; i < nlines; i++)
		NVIC_ClearPendingIRQ((IRQn_Type)lines[i]);

	/* 5. drain anything the callback posted before the lines went down */
	while (tx_semaphore_get(&cam_frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	cam_err = 0;
	cam_frame_ready = 0u;
	cam_generation++;
}

/* ---- the rev-C MIPI bounce ----------------------------------------------- */

/*
 * Revision C loses D-PHY lock across a frame boundary unless the receiver and
 * the sensor stream are cycled between frames.  Every donor application carries
 * the same workaround gated on the same version word.
 *
 * [!] WHAT "RE-LOCKED" CAN AND CANNOT MEAN HERE.  The SDK exposes no way to
 * read D-PHY lock back -- the helpers involved all return void.  So this does
 * not claim to verify the lock.  It establishes three weaker things and says so:
 * the sensor accepted its I2C writes, the receiver's LATCHED error status is
 * clean when read under the clear -> enable -> poll protocol (it latches, so a
 * poll without the clear reports the previous link), and -- the real evidence --
 * the next frame arrives inside the bounded wait.  The third is what actually
 * proves the link came back, and it is established after the fact by the
 * producer loop rather than here.
 */
static int cam_rev_c_off(void)
{
	if (cam_sensor_stream_off() != 0) {
		cam_fault_latch("the sensor would not stop for the rev-C bounce");
		return -1;
	}
	cam_dp_csirx_disable();
	/* Clear WHILE disabled: the status latches, so this is the only moment
	 * at which a later poll can mean anything about the new link. */
	if (cam_dp_csirx_clear_errors() != 0) {
		cam_fault_latch("the CSI error status would not clear on bounce");
		return -1;
	}
	return 0;
}

static int cam_rev_c_on(void)
{
	struct cam_csirx_errors err;

	if (cam_dp_csirx_enable() != 0) {
		cam_fault_latch("the CSI receiver would not come back up");
		return -1;
	}
	if (cam_sensor_stream_on() != 0) {
		cam_fault_latch("the sensor would not restart after the bounce");
		return -1;
	}

	cam_dp_csirx_errors(&err);
	if (!err.readable) {
		cam_fault_latch("the CSI error status could not be read");
		return -1;
	}
	if (err.err != 0u || err.dphyerr != 0u) {
		cam_csirx_errors++;
		LOG_ERR("csirx error after bounce: err %08lx dphy %08lx",
		        (unsigned long)err.err, (unsigned long)err.dphyerr);
		cam_fault_latch("the CSI receiver reported an error after a bounce");
		return -1;
	}
	return 0;
}

/* ---- publishing one frame ------------------------------------------------ */

/*
 * Re-take any of this port's lines that the vendor has re-registered.  Thread
 * context; see the call site in cam_service_one() for why this is per-frame.
 */
static void cam_reassert_wraps(void)
{
	TX_INTERRUPT_SAVE_AREA
	uint32_t i;

	TX_DISABLE
	for (i = 0u; i < cam_wrapsets_used; i++)
		(void)grove_epk_irq_reassert(&cam_wrapset[i]);
	TX_RESTORE
}

/*
 * Steer the white balance from the frame just published, and re-read what the
 * sensor's own exposure loop has done.
 *
 * Producer thread, between frames.  The sensor I2C goes through the same
 * cam_sensor_* calls the console uses -- this thread owns the CIS driver while
 * a stream runs, which is precisely why the console's own tuning commands queue
 * their writes for it rather than making them directly.
 *
 * [!] THE EXPOSURE HALF IS GONE (issue #54).  This used to run a software AE
 * loop for a sensor that had none; the part that needed it was the IMX219, and
 * it was removed.  The OV5647's on-chip AEC owns the exposure, so what is left
 * here is reading it back and the white balance, which the datapath has never
 * provided at all.
 */
static void cam_auto_step(void)
{
	uint32_t means[3];

	if (!cam_auto_on)
		return;
	if ((++cam_auto_phase % CAM_AUTO_EVERY) != 0u)
		return;

	cam_frame_means_x100(cam_dp_completed_buffer(), CAM_FRAME_PIXELS,
	                     CAM_AUTO_STEP, means);

	/*
	 * [!] Take the pedestal off BEFORE either loop sees the numbers.
	 *
	 * The black level is a constant added to all three channels, so leaving
	 * it in COMPRESSES the ratios grey world works from and the white
	 * balance under-corrects.  With a true signal of R30 G35 B20 the ratio
	 * green/blue is 1.75; measured with a pedestal of 16 still in it, the
	 * same scene reads R46 G51 B36 and the ratio is 1.42.  Blue does not get
	 * lifted far enough and a warm room stays visibly yellow -- which is
	 * exactly how this was found.
	 *
	 * The converter already subtracts it before applying the gains; the
	 * measurement has to agree with the correction, or the loop is steering
	 * one quantity and the picture is showing another.
	 */
	{
		uint32_t ped = (uint32_t)cam_wb.black * 100u;
		uint32_t c;

		for (c = 0u; c < 3u; c++)
			means[c] = (means[c] > ped) ? (means[c] - ped) : 0u;
	}

	/* The sensor is driving the exposure, so the reported values have to
	 * follow it, and this thread is the only one that may read them: it owns
	 * the CIS driver while a stream runs, which is why the console cannot do
	 * it itself.  Without this the console reports whatever was last written
	 * by hand while the real exposure moves underneath, which makes a working
	 * auto-exposure look broken. */
	(void)cam_sensor_refresh_exposure_gains();

	/* White balance is software, so it takes effect on the NEXT frame this
	 * thread packs -- no sensor round trip, no delay to design around. */
	(void)cam_awb_step(means[0], means[1], means[2], &cam_wb);
}

static void cam_publish(const uint8_t *raw)
{
	struct frame_desc *slot;
	uint32_t t1, t2;

	/* The invalidate happened in cam_service_one(), before the arm: the
	 * pixels must be readable before anything else is allowed to matter,
	 * and the arm must not wait for the packer. */
	t1 = cam_now();

	slot = frame_pipeline_acquire(&cam_pipe);
	if (slot == NULL)
		return;   /* the ring counts this as an overrun */

	cam_bgr_planar_to_rgb565_wb(raw, (uint16_t *)slot->data,
	                            CAM_FRAME_PIXELS, &cam_wb);
	t2 = cam_now();
	cam_prof.pack += (uint32_t)(t2 - t1);

	/* The sinks consume ON THIS THREAD, inside publish -- so this interval
	 * is the LCD blit (a byte-swap copy plus the SPI DMA wait), not a queue
	 * hand-off.  That is the whole reason it is worth timing separately. */
	frame_pipeline_publish(&cam_pipe, slot, CAM_FRAME_PIXELS * 2u,
	                       FRAME_FMT_RGB565, (uint16_t)CAM_FRAME_WIDTH,
	                       (uint16_t)CAM_FRAME_HEIGHT,
	                       (uint16_t)(CAM_FRAME_WIDTH * 2u));
	cam_prof.sink += (uint32_t)(cam_now() - t2);
	cam_frames++;
}

/* ---- the producer thread ------------------------------------------------- */

/*
 * One iteration of the stream.  Returns 0 to continue, <0 to stop for good.
 *
 * The ORDER of the checks is the state machine: stop request, then the sticky
 * error latch, then the frame.  A wakeup that carries both an error and a
 * frame-ready must not publish -- the frame was produced by a datapath that has
 * already reported a failure.
 */
static int cam_service_one(uint32_t *retries)
{
	uint32_t t_top = cam_now();
	UINT rc = tx_semaphore_get(&cam_frame_sem, CAM_FRAME_TIMEOUT_TICKS);
	uint32_t t_ready = cam_now();
	const uint8_t *raw;
	uint32_t t;

	/*
	 * Accounted before any of the exits below, and the period is closed at
	 * the NEXT top rather than here -- so the parts and the whole are
	 * measured over the same window and the breakdown sums by construction.
	 * The last, partial iteration is simply never closed, which is right: it
	 * did not produce a frame.
	 */
	cam_prof.wait += (uint32_t)(t_ready - t_top);
	if (cam_prof_have_top) {
		cam_prof.total += (uint32_t)(t_top - cam_prof_last_top);
		cam_prof.iters++;
	}
	cam_prof_last_top = t_top;
	cam_prof_have_top = 1u;

	if (cam_stop_req)
		return -1;

	if (cam_err != 0) {
		cam_last_dp_status = cam_err;
		cam_dp_errors++;
		LOG_ERR("datapath error %ld; stopping", (long)cam_err);
		cam_fault_latch("the datapath reported a terminal error");
		return -1;
	}

	if (rc != TX_SUCCESS) {
		/*
		 * Nothing arrived.  Retry, but through the SAME full stop and
		 * restart barrier as everything else: the previous capture may
		 * still be live, and clearing state underneath a running
		 * datapath is how a stale completion turns into the next
		 * stream's first frame.
		 */
		cam_timeouts++;
		if (++(*retries) > CAM_MAX_RETRIES) {
			LOG_ERR("no frame after %lu restarts; giving up",
			        (unsigned long)CAM_MAX_RETRIES);
			cam_fault_latch("no frame arrived, restarts exhausted");
			return -1;
		}
		cam_retries++;
		LOG_INF("frame timeout; restart %lu/%lu",
		        (unsigned long)*retries, (unsigned long)CAM_MAX_RETRIES);

		cam_quiesce();
		if (cam_start_datapath() != CAM_OK)
			return -1;
		return 0;
	}

	if (!cam_frame_ready)
		return 0;                    /* a benign event woke us */
	cam_frame_ready = 0u;

	/* A frame did arrive, so whatever went wrong before has recovered. */
	*retries = 0u;

	/*
	 * Verify and commit (issue #59): the channel registers must still
	 * describe the buffer that was armed, or the frame is somewhere this
	 * port cannot name.  Only after that is the buffer readable.
	 */
	if (cam_dp_frame_complete(&raw) != 0) {
		cam_fault_latch("the WDMA3 configuration moved under the stream");
		return -1;
	}

	/*
	 * [!] Invalidate BEFORE reading and only AFTER the write-DMA has
	 * stopped -- which frame-ready is exactly the statement of.  The
	 * shipping vendor glue never does this for the WDMA3 buffer (it
	 * invalidates only the 32-byte JPEG size word), so the omission is easy
	 * to inherit: 225 KB of DMA'd pixels read back through stale cache
	 * lines produces a picture that is mostly right and partly last frame.
	 * One buffer's worth, never the sibling's: the sibling is about to be
	 * handed to the DMA, and its lines are none of this thread's business.
	 */
	t = cam_now();
	hx_InvalidateDCache_by_Addr((volatile void *)(uintptr_t)raw,
	                            (int32_t)CAM_RAW_BYTES);
	cam_prof.inval += (uint32_t)(cam_now() - t);

	if (cam_rev_c) {
		/*
		 * Revision C keeps the SERIALISED order: the bounce drops the
		 * receiver and the sensor stream every frame, so there is no
		 * defined place to arm early.  Same transitions, later
		 * scheduling point -- there is no second flip mechanism.
		 * (Unreachable on the board in hand, which is rev D.)
		 */
		if (cam_rev_c_off() != 0) {
			cam_relock_fails++;
			return -1;
		}
		cam_publish(raw);
		if (cam_rev_c_on() != 0) {
			cam_relock_fails++;
			return -1;
		}
		t = cam_now();
		cam_auto_step();
		cam_apply_tuning();
		cam_prof.tune += (uint32_t)(cam_now() - t);

		t = cam_now();
		if (cam_dp_arm_next() != 0) {
			cam_fault_latch("the next capture could not be armed");
			return -1;
		}
		cam_prof.arm += (uint32_t)(cam_now() - t);
		cam_reassert_wraps();
		return 0;
	}

	/*
	 * [!] THE ARM GOES BEFORE THE WORK (issue #59).  That is the whole
	 * change: the capture of frame N+1 runs on the other landing buffer
	 * while this thread packs, infers and publishes frame N, so the active
	 * frame time leaves the critical path.  Its deadline is the sensor's
	 * blanking interval (~18 ms at the frame lengths in use) against the
	 * few hundred microseconds of invalidate above.
	 */
	t = cam_now();
	if (cam_dp_arm_next() != 0) {
		cam_fault_latch("the next capture could not be armed");
		return -1;
	}

	/*
	 * [!] AND TAKE THE INTERRUPTS BACK, IMMEDIATELY -- nothing may sit
	 * between the arm and this.
	 *
	 * Measured on hardware: irq 143 is wrapped at start-up with one vendor
	 * handler and is carrying a DIFFERENT one a frame later -- the
	 * retrigger path re-registers its ISR.  The measure-then-wrap protocol
	 * cannot see that (the line never changes its ENABLED state), so
	 * without this the line silently stops being accounted one frame into
	 * every stream, and `thread` blanks the whole cpu% column for as long
	 * as the preview runs.  With the arm now FIRST, frame-ready can fire
	 * during everything below; every instruction inserted before this call
	 * widens the window in which that fires through an unaccounted vector.
	 *
	 * Cheap: one vector read and a compare per wrapped line, a handful of
	 * lines, once per frame.  Under a critical section because it may
	 * write the vector table, and an interrupt taken mid-write would
	 * dispatch through a half-updated entry.
	 */
	cam_reassert_wraps();
	cam_prof.arm += (uint32_t)(cam_now() - t);

	/*
	 * Between frames -- and since the arm moved first, DURING the sensor's
	 * vertical blanking rather than at an arbitrary point of the next
	 * exposure: the one moment a rolling-shutter write cannot tear a
	 * frame, and the one moment this thread is provably the sole user of
	 * the vendor CIS driver.  After the arm, so the arm's deadline does
	 * not depend on how long the sensor I2C takes.
	 */
	t = cam_now();
	cam_auto_step();
	cam_apply_tuning();
	cam_prof.tune += (uint32_t)(cam_now() - t);

	/*
	 * [!] RE-READ THE STICKY LATCH before publishing (the error-first rule;
	 * see AGENTS.md).  Before issue #59 nothing was armed during publish,
	 * so "the datapath faulted" and "this frame came from that operation"
	 * were the same statement and the loop-top check covered both.  The
	 * early arm separates them -- and the arm itself can latch a status
	 * that belongs to THIS iteration.  Discarding a frame that was itself
	 * good is the intended consequence of fail-closed; it costs one frame,
	 * at teardown.
	 */
	if (cam_err != 0) {
		cam_last_dp_status = cam_err;
		cam_dp_errors++;
		LOG_ERR("datapath error %ld before publish; stopping",
		        (long)cam_err);
		cam_fault_latch("the datapath reported a terminal error");
		return -1;
	}

	cam_publish(raw);
	return 0;
}

static void cam_producer_entry(ULONG arg)
{
	(void)arg;

	for (;;) {
		uint32_t retries = 0u;

		(void)tx_semaphore_get(&cam_start_sem, TX_WAIT_FOREVER);

		while (cam_service_one(&retries) == 0)
			;

		/* Every exit from the loop lands here, and here is where the one
		 * stop sequence lives.  A producer that stopped differently
		 * depending on why it stopped is how a peripheral ends up in a
		 * state nobody wrote down. */
		cam_quiesce();
		/*
		 * [!] CAM_ST_LOST IS NOT OVERWRITTEN HERE (issue #48).
		 *
		 * This line is where the poison would otherwise be laundered.
		 * A join that timed out has already told its caller the camera
		 * is finished -- and left the sink attached and the NPU leased
		 * on the strength of that.  Arriving here late means this thread
		 * finally finished; it does NOT mean any of that was undone.
		 * Writing CAM_ST_READY would put the port back in service with
		 * a sink nobody detached, an owner nobody released, and a
		 * console that was told the opposite.
		 *
		 * It also keeps the stale cam_stopped_sem post below
		 * unreachable: with the state pinned, every later
		 * camera_stream_stop() is refused by its poison test before it
		 * can consume this count and report a join that never happened.
		 */
		if (cam_state != CAM_ST_LOST)
			cam_state = cam_stop_req ? CAM_ST_READY : CAM_ST_FAULTED;
		cam_stop_req = 0u;
		(void)tx_semaphore_put(&cam_stopped_sem);
	}
}

/* ---- public API ---------------------------------------------------------- */

void camera_create_objects(void)
{
	if (tx_mutex_create(&cam_pipe_mutex, "cam_pipe", TX_INHERIT) != TX_SUCCESS ||
	    tx_mutex_create(&cam_api_mutex, "cam_api", TX_INHERIT) != TX_SUCCESS ||
	    tx_semaphore_create(&cam_frame_sem, "cam_frame", 0) != TX_SUCCESS ||
	    tx_semaphore_create(&cam_start_sem, "cam_start", 0) != TX_SUCCESS ||
	    tx_semaphore_create(&cam_stopped_sem, "cam_stop", 0) != TX_SUCCESS) {
		LOG_ERR("camera object create failed");
		return;
	}

	if (frame_pipeline_init(&cam_pipe, &cam_pipe_os, cam_slot_mem,
	                        CAM_FRAME_PIXELS * 2u, CAM_SLOTS) != 0) {
		LOG_ERR("frame pipeline init failed");
		return;
	}
	frame_pipeline_set_format(&cam_pipe, FRAME_FMT_RGB565,
	                          (uint16_t)CAM_FRAME_WIDTH,
	                          (uint16_t)CAM_FRAME_HEIGHT);

	if (tx_thread_create(&cam_producer, "cam", cam_producer_entry, 0,
	                     cam_producer_stack, sizeof cam_producer_stack,
	                     CAM_PRODUCER_PRIO, CAM_PRODUCER_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("camera producer thread create failed");
		return;
	}

	cam_objects_ok = 1;
}

static void cam_api_exit(void)
{
	/*
	 * [!] AND THE ONE-SHOT RAW MODE ENDS HERE (issue #77).
	 *
	 * cam_raw_mode selects the Bayer datapath in cam_step_dp(), and the
	 * PRODUCER reads it -- on its own frame-timeout restart, which calls
	 * cam_start_datapath() from the producer thread.  So a value left
	 * standing outside a held-mutex section can reconfigure a LIVE stream
	 * for a one-shot capture.
	 *
	 * camera_capture_raw() used to set it before the API was entered at all,
	 * so even a REFUSED raw capture left a window in which a running
	 * preview's restart could pick it up -- the refusal was honest and the
	 * damage happened anyway.  It is now set only after cam_bus_enter() has
	 * answered CAM_BUS_DIRECT, which is what proves no producer is running
	 * and none can start.
	 *
	 * Clearing it here rather than at each exit makes the scope structural
	 * instead of remembered: nothing this port does needs it to survive a
	 * release, and a path added later cannot leak it.
	 */
	cam_raw_mode = 0u;
	(void)tx_mutex_put(&cam_api_mutex);
}

/* Record one contended stop, count and longest wait together (issue #65). */
static void cam_note_contention(ULONG waited)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	cam_lock_contended++;
	if ((uint32_t)waited > cam_lock_wait_max)
		cam_lock_wait_max = (uint32_t)waited;
	TX_RESTORE
}

/*
 * The stop's acquisition: try, and only then wait (issue #65).
 *
 * Try first so that the uncontended path -- every stop anyone has ever run --
 * is the same call it always was, and so that "had to wait" is an exact count
 * rather than an inference from elapsed time.
 *
 * The caller has already made the poison test that belongs BEFORE the mutex;
 * this returns only how the attempt ended, and cam_stop_decide() turns that,
 * with the state read while holding, into what the stop does.
 */
static enum cam_api_acquire cam_api_enter_stop(void)
{
	UINT st = tx_mutex_get(&cam_api_mutex, TX_NO_WAIT);
	ULONG t0;

	if (st == TX_SUCCESS)
		return CAM_ACQ_HELD;
	if (st != TX_NOT_AVAILABLE)
		return CAM_ACQ_ERROR;

	t0 = tx_time_get();
	st = tx_mutex_get(&cam_api_mutex, CAM_STOP_LOCK_TICKS);
	cam_note_contention(tx_time_get() - t0);

	if (st == TX_SUCCESS)
		return CAM_ACQ_HELD;
	/* TX_NOT_AVAILABLE here is the deadline; anything else is a mutex this
	 * port can no longer rely on, which is not the same answer. */
	return (st == TX_NOT_AVAILABLE) ? CAM_ACQ_UNAVAILABLE
	                                : CAM_ACQ_ERROR;
}

/*
 * [!] ENTER THE API AND FIND OUT WHO OWNS THE SENSOR BUS (issue #77).
 *
 * Every entry point that may reach the vendor CIS driver comes through here.
 * It exists because the test that keeps the console off that bus -- "is a
 * stream running?" -- is only worth anything if it is taken with this mutex
 * HELD.  camera_stream_start() publishes CAM_ST_STREAMING while holding it, so
 * a test taken before the acquire can be overtaken by an entire stream start
 * and the caller then walks in on a bus the producer owns.  TX_NO_WAIT does not
 * help: by then the starter has let go.
 *
 * Five entry points did exactly that until this existed, and camera_probe()'s
 * fix for issue #74 added a sixth ordering of its own.  So the ordering is
 * written once, and the decision it feeds is a pure function (cam_bus_decide())
 * because the race cannot be produced on this board's single console.
 *
 * THE CONTRACT IS DELIBERATELY NARROW, because the alternative is a mutex whose
 * ownership depends on which of four answers came back:
 *
 *   CAM_OK   -- the mutex IS held, and *owner is exactly CAM_BUS_DIRECT or
 *               CAM_BUS_PRODUCER.  The caller has ONE cleanup exit calling
 *               cam_api_exit().
 *   negative -- the mutex is NOT held and *owner is untouched.
 *
 * So a refusal can never be mistaken for a route, and an owner value added
 * later cannot quietly become a successful one.  That matters more than it
 * looks: ThreadX mutexes are recursive, so a nested acquire would not deadlock
 * loudly -- it would return success and leave the mutex held after one put.
 *
 * BRING-UP IS NOT DONE HERE.  The direct route's callers do it themselves,
 * holding the mutex.  Keeping it out is what lets camera_set_auto() keep
 * "a camera that will not come up is not a failure of this command" without a
 * special case, and it is safe because only the direct route ever calls it: the
 * producer route never brings up, and only camera_stream_start() can enter
 * CAM_ST_STREAMING, under this same mutex.
 */
static int cam_bus_enter(enum cam_bus_owner *owner)
{
	enum cam_api_acquire acq;
	enum cam_bus_owner who;
	UINT st;

	if (!cam_objects_ok)
		return CAM_ERR_STATE;
	/*
	 * [!] THE POISON CHECK, and it is deliberately BEFORE the mutex
	 * (issue #48).
	 *
	 * Every entry point that touches camera hardware comes through here, so
	 * this one test is what makes "the producer may still be running" safe:
	 * nothing can power the module, quiesce the datapath, unwrap an interrupt
	 * or rebuild the port while a thread that never acknowledged a stop is
	 * still in flight.
	 *
	 * Before the mutex because the lost producer may itself still hold
	 * things; making a poisoned call queue behind anything would turn a clear
	 * refusal into a hang.  camera_stream_stats() reads no hardware and takes
	 * no mutex, so `camera stats` still answers and still says what happened
	 * -- a refusal with no way to see why is not a diagnosis.
	 *
	 * [!] But it is NOT the last word, and treating it as one is what issue
	 * #77 found: poison can land between this test and the acquire below, so
	 * cam_bus_decide() tests CAM_ST_LOST again on the far side.  Both are
	 * needed -- this one so a doomed call never queues, that one because this
	 * one's answer can be stale by the time the mutex is in hand.
	 */
	if (!cam_api_may_acquire(cam_objects_ok, cam_state))
		return CAM_ERR_STATE;

	st = tx_mutex_get(&cam_api_mutex, TX_NO_WAIT);
	if (st == TX_SUCCESS)
		acq = CAM_ACQ_HELD;
	else if (st == TX_NOT_AVAILABLE)
		acq = CAM_ACQ_UNAVAILABLE;
	else
		acq = CAM_ACQ_ERROR;

	/*
	 * [!] cam_state is READ ONLY ON THE HELD PATH.  Not a micro-optimisation:
	 * an unlocked read is the very thing this function exists to stop, and
	 * passing one "for the table to ignore" would put it back at the point
	 * where a future edit is most likely to start believing it.
	 *
	 * And the placeholder is CAM_ST_LOST, the one state that refuses, rather
	 * than the zero value -- so that if the table's acquisition guard is ever
	 * weakened, what is standing behind it here still fails closed instead of
	 * reading as an idle port and handing back CAM_BUS_DIRECT.
	 */
	who = cam_bus_decide(acq, (acq == CAM_ACQ_HELD) ? cam_state
	                                                : CAM_ST_LOST);

	switch (who) {
	case CAM_BUS_DIRECT:
	case CAM_BUS_PRODUCER:
		*owner = who;
		return CAM_OK;
	case CAM_BUS_REFUSE_BUSY:
		/* Nothing was acquired, so there is nothing to release. */
		return CAM_ERR_BUSY;
	case CAM_BUS_REFUSE_STATE:
		break;
	}
	/*
	 * [!] AND HERE THE MUTEX MAY BE HELD.  A state refusal arrives two ways:
	 * a mutex that failed for a reason other than contention (nothing to
	 * release), or a POISONED PORT SEEN THROUGH A SUCCESSFUL ACQUIRE.  The
	 * second is the whole reason cam_bus_decide() re-tests CAM_ST_LOST, and it
	 * is the one place the "negative means not held" half of this contract can
	 * be broken.
	 */
	if (acq == CAM_ACQ_HELD)
		cam_api_exit();
	return CAM_ERR_STATE;
}

/*
 * [!] REFUSED WHILE A STREAM RUNS (issue #74).
 *
 * This reads the sensor's model ID over I2C, and the producer is doing its own
 * I2C at the same time from cam_auto_step() -- the exposure and gain read-back
 * and the queued tuning writes.  The API mutex does not serialise those two,
 * because THE PRODUCER NEVER TAKES IT: it is a rendezvous partner, not an API
 * caller, and that is deliberate.  So without this there are two threads on one
 * bus with nothing between them.
 *
 * camera_read_frame_length() has refused for exactly this reason all along, and
 * the tuning setters queue their writes for the producer instead of touching the
 * bus.  This entry point was simply missed.  Nothing has ever been observed --
 * the symptoms would be an occasional wrong ID or a disturbed frame, and this
 * link is slow enough that the window is narrow -- so this closes a hole found
 * by reading rather than one seen.
 *
 * The test that decides it is cam_bus_enter()'s, which is to say it is taken
 * with the mutex HELD.  Issue #74 made that placement here and issue #77 gave
 * the other six the same one; the note on cam_bus_enter() says why a test taken
 * any earlier is worth nothing.
 *
 * Refusing rather than answering from cache is the honest option.  The chip
 * version and the rev-C flag are cached and cam_sensor_id() returns the selected
 * descriptor's EXPECTED id, not a fresh observation -- so serving those would
 * quietly turn "go and look" into "what did the last bring-up establish?".  And
 * a running stream is not itself proof the module is still there: it stays
 * STREAMING until the producer notices a timeout or a fault, so the module could
 * have been unplugged since.  The live read that would settle it is the very
 * thing that is unsafe here, which is why the answer is a refusal.
 */
int camera_probe(struct camera_probe_info *out)
{
	enum cam_bus_owner owner;
	int rc;

	if (out == NULL)
		return CAM_ERR_PARAM;
	rc = cam_bus_enter(&owner);
	if (rc != CAM_OK)
		return rc;
	/* Tested for the one route that may act, not against the one that may
	 * not: an owner added later must not become a licence to read the bus. */
	if (owner != CAM_BUS_DIRECT) {
		cam_api_exit();
		return CAM_ERR_BUSY;    /* I2C; the producer owns that driver */
	}

	rc = cam_bringup();
	if (rc == CAM_OK) {
		out->chip_version = cam_dp_chip_version();
		out->rev_c = cam_rev_c;
		if (cam_sensor_read_id(&out->sensor_id) != 0) {
			out->sensor_id = 0u;
			rc = CAM_ERR_NO_SENSOR;
		}
	}

	cam_api_exit();
	return rc;
}

const uint8_t *camera_raw_frame(void)
{
	return cam_dp_completed_buffer();
}

/*
 * The one-shot capture, in the demosaiced or the raw Bayer datapath.
 *
 * [!] ONE IMPLEMENTATION TAKING THE MODE AS AN ARGUMENT, not a wrapper that
 * sets a flag around a call (issue #77).  The old shape set cam_raw_mode before
 * camera_capture() had entered the API, so the flag stood while the call was
 * still deciding whether it was allowed to run at all -- see cam_api_exit() for
 * what a producer restart does with it in that window.  Passing it in means it
 * cannot be set before the answer.
 */
static int cam_capture_one(uint8_t raw)
{
	enum cam_bus_owner owner;
	uint32_t waited;
	int rc;

	rc = cam_bus_enter(&owner);
	if (rc != CAM_OK)
		return rc;
	/* A one-shot capture drives the datapath as well as the bus, so the
	 * producer route is a refusal here too -- and tested the same way round
	 * as everywhere else. */
	if (owner != CAM_BUS_DIRECT) {
		cam_api_exit();
		return CAM_ERR_BUSY;
	}

	/* Only now, with the datapath owned: cam_step_dp() reads this, and until
	 * this mutex is released nobody else can reach it. */
	cam_raw_mode = raw;

	rc = cam_bringup();
	if (rc != CAM_OK) {
		cam_api_exit();
		return rc;
	}

	/* Start from a known-quiet datapath: a previous capture that ended in a
	 * fault, or a stream that stopped, may have left events latched. */
	cam_quiesce();

	rc = cam_start_datapath();
	if (rc != CAM_OK) {
		cam_quiesce();
		cam_state = CAM_ST_FAULTED;
		cam_api_exit();
		return rc;
	}

	/* Poll rather than block on the semaphore: this runs on the shell
	 * thread and the producer owns the semaphore during a stream.  Bounded
	 * either way. */
	rc = CAM_ERR_TIMEOUT;
	for (waited = 0u; waited < CAM_FRAME_TIMEOUT_TICKS; waited++) {
		if (cam_err != 0) {
			cam_last_dp_status = cam_err;
			cam_dp_errors++;
			rc = CAM_ERR_HAL;
			break;
		}
		if (cam_frame_ready) {
			cam_frame_ready = 0u;
			hx_InvalidateDCache_by_Addr(
				(volatile void *)(uintptr_t)
					cam_dp_completed_buffer(),
				(int32_t)CAM_RAW_BYTES);
			cam_frames++;
			rc = CAM_OK;
			break;
		}
		(void)tx_thread_sleep(1u);
	}
	if (rc == CAM_ERR_TIMEOUT)
		cam_timeouts++;

	cam_quiesce();
	/*
	 * A terminal datapath status is terminal for a one-shot capture too: the
	 * next call has to rebuild the port from bring-up rather than reuse
	 * whatever state the failure left behind.  A timeout is NOT terminal --
	 * no frame arrived, but nothing reported a fault -- so it stays READY and
	 * the next capture simply tries again.
	 */
	cam_state = (rc == CAM_ERR_HAL) ? CAM_ST_FAULTED : CAM_ST_READY;
	cam_api_exit();
	return rc;
}

int camera_capture(void)
{
	return cam_capture_one(0u);
}

int camera_capture_raw(void)
{
	return cam_capture_one(1u);
}

int camera_stream_start(struct frame_sink *sink)
{
	enum cam_bus_owner owner;
	int rc = cam_bus_enter(&owner);

	/*
	 * [!] THE EIGHTH ENTRY POINT, and the one with the most to lose (issue
	 * #77, found reviewing the other seven).
	 *
	 * This used to enter with cam_api_enter() and then test only for
	 * CAM_ST_STREAMING -- which is "not streaming, therefore go ahead", the
	 * shape cam_bus_decide() exists to refuse.  CAM_ST_LOST is also not
	 * streaming, and it IS reachable after the acquire: pass the pre-mutex
	 * poison test while a stream runs, be preempted, let a stop take the
	 * mutex and time out, and resume owning a poisoned port.
	 *
	 * Where the tuning setters would then have done one I2C transaction, this
	 * reaches cam_bringup(), which rebuilds everything that is not READY or
	 * STREAMING: power the module down, unwrap the interrupts, re-detect.
	 * Underneath a producer that never acknowledged a stop.  That is the
	 * single action CAM_ST_LOST exists to prevent, not merely a collision on
	 * a bus.
	 *
	 * The sink reservation below does not stand in for this.  It refuses a
	 * start while any sink is linked, so a preview or `nn preview` whose stop
	 * failed is covered by it -- but `camera bench` streams with NO sink, so
	 * on that path the registry is empty and there is nothing else in the way.
	 *
	 * Nothing about the answers changes: CAM_BUS_PRODUCER is the state this
	 * already refused as CAM_ERR_BUSY, and a poisoned port already reported
	 * CAM_ERR_STATE when it was visible before the mutex.  What changes is
	 * that it is now also refused when it becomes visible after.
	 */
	if (rc != CAM_OK)
		return rc;
	if (owner != CAM_BUS_DIRECT) {
		cam_api_exit();
		return CAM_ERR_BUSY;
	}

	/*
	 * [!] AND NOBODY ELSE'S SINK MAY STILL BE LINKED (issue #63).
	 *
	 * "The producer is not streaming" is NOT the same as "the camera is free",
	 * and the difference is a window with two ways in.  A sink stays linked
	 * across its owner's teardown -- unlinking is only safe after a confirmed
	 * stop, so the owner holds it from the moment stop() returns until it gets
	 * round to detaching -- and the producer can also leave STREAMING BY
	 * ITSELF (retries exhausted, terminal datapath event) while the owner is
	 * still asleep in its poll loop, which is not a tight interleaving at all:
	 * another console can simply type `camera bench` into it.
	 *
	 * Starting there would hand the old sink frames from a stream its owner
	 * does not own, and that owner is entitled to tear down what the sink
	 * points at.  Worse, the panel sink counts a delivery only AFTER its
	 * overlay has run -- the whole NPU inference under `nn preview` -- so a
	 * drain running concurrently sees "nothing outstanding" and returns
	 * success while an inference is in flight.
	 *
	 * So a linked sink reserves the camera, whatever the state says.  This is
	 * asked of the pipeline's own registry rather than of a flag here: the
	 * registry is the thing that decides who gets frames.  It cannot go stale
	 * in the dangerous direction -- every attach runs under this mutex, which
	 * this thread holds, so nothing can appear after the check.  A concurrent
	 * DETACH can make the answer stale, and that only costs an operator a
	 * refusal to retry.
	 *
	 * The reservation ends at the unlink, not at the end of the owner's
	 * teardown: an unlinked sink is unreachable no matter what its owner's own
	 * threads are still doing (which is why a panel drain that times out does
	 * not lock `camera bench` out for ever -- the sink is already gone from the
	 * registry by then).
	 */
	if (frame_pipeline_sink_count(&cam_pipe) != 0) {
		cam_api_exit();
		return CAM_ERR_BUSY;
	}

	rc = cam_bringup();
	if (rc != CAM_OK) {
		cam_api_exit();
		return rc;
	}

	cam_quiesce();
	rc = cam_start_datapath();
	if (rc != CAM_OK) {
		cam_quiesce();
		cam_state = CAM_ST_FAULTED;
		cam_api_exit();
		return rc;
	}

	/* The stage profile describes ONE stream (issue #38): a mean that mixed
	 * two previews taken at different exposures would be a measurement of
	 * neither.  The counters above stay cumulative, and the readout says
	 * which is which. */
	{
		struct cam_prof zero = { 0 };

		cam_prof = zero;
		cam_prof_have_top = 0u;
	}

	/*
	 * [!] Drain any stale stop acknowledgement first.
	 *
	 * The producer posts cam_stopped_sem on EVERY exit, including the ones it
	 * decides for itself (a timeout that exhausted its retries, a terminal
	 * datapath event).  Nobody consumes those: camera_stream_stop() sees a
	 * state that is no longer STREAMING and returns early.  Left behind, the
	 * token would satisfy the NEXT stop immediately -- which would return
	 * while the producer was still mid-frame, and the caller would then
	 * detach its sink underneath a running consume().  That is precisely the
	 * race camera_stream_stop() is synchronous to avoid.
	 *
	 * Here is where draining is safe: the producer is provably idle (the
	 * state is not STREAMING and this thread holds the API mutex), so any
	 * token present belongs to a stream that is already over.
	 */
	while (tx_semaphore_get(&cam_stopped_sem, TX_NO_WAIT) == TX_SUCCESS)
		;

	/*
	 * The subscribe goes LAST, after everything that can fail and before the
	 * producer is released.
	 *
	 * Nothing can deliver a frame in between: the producer is parked on
	 * cam_start_sem and it is the only thing that publishes -- the datapath ISR
	 * only latches a status and posts a semaphore.  So an armed datapath with
	 * no sink linked yet is simply a frame that nobody will be handed, and the
	 * unwind below throws it away with cam_quiesce().
	 *
	 * Going last is what keeps the unwind honest.  frame_pipeline_attach()
	 * either links the sink or fails with NOTHING linked and nothing touched --
	 * every refusal it makes is decided before the sink's open() is called
	 * (issue #72).  So the failures here are a geometry mismatch from the
	 * sink's own open(), or the core refusing a sink that is already attached
	 * or has not handed its frames back; none is a hardware fault.  The camera
	 * therefore stays READY (as after a capture that timed out) rather than
	 * taking the terminal treatment a datapath failure gets.
	 */
	if (sink != NULL) {
		rc = cam_subscribe(sink);
		if (rc != CAM_OK) {
			cam_quiesce();
			cam_state = CAM_ST_READY;
			cam_api_exit();
			return rc;
		}
	}

	cam_stop_req = 0u;
	cam_state = CAM_ST_STREAMING;
	(void)tx_semaphore_put(&cam_start_sem);

	cam_api_exit();
	return CAM_OK;
}

int camera_stream_stop(void)
{
	enum cam_api_acquire acq;

	/*
	 * [!] THE POISON TEST IS STILL BEFORE THE MUTEX (issue #48): a call that
	 * is going to be refused must never queue behind anything.
	 */
	if (!cam_api_may_acquire(cam_objects_ok, cam_state))
		return CAM_ERR_STATE;

	acq = cam_api_enter_stop();

	/*
	 * [!] AND THE DECISION IS cam_stop_decide()'s, ALL OF IT (issue #65).
	 *
	 * Nothing here may test the state ahead of it -- CAM_ST_LOST is also
	 * "not streaming", so a shortcut placed first would report a confirmed
	 * stop that never happened.  Keeping the whole table in one pure
	 * function is what lets the host test hold that ordering down; a copy of
	 * it here would be the copy that drifts.
	 */
	switch (cam_stop_decide(acq, cam_state)) {
	case CAM_STOP_REFUSE_LOCKED:
		/*
		 * The mutex never came free, so the producer was never asked --
		 * this call did not touch the port and did not confirm
		 * anything.  It says nothing about whether the stream is still
		 * running (the holder may be a stop that has already joined),
		 * and it does NOT poison: nothing was done to be undone.
		 */
		LOG_ERR("the camera API stayed locked; the stop was never "
		        "requested");
		return CAM_ERR_LOCKED;
	case CAM_STOP_REFUSE_STATE:
		if (acq == CAM_ACQ_HELD)
			cam_api_exit();
		return CAM_ERR_STATE;
	case CAM_STOP_ALREADY:
		cam_api_exit();
		return CAM_OK;         /* already stopped: not an error */
	case CAM_STOP_JOIN:
	default:
		break;
	}

	/*
	 * SYNCHRONOUS.  Ask, wake the producer in case it is parked on the
	 * frame semaphore, and wait for it to say it has been through the full
	 * stop.  Callers detach sinks straight after this, and consume() runs
	 * on the producer thread -- returning early would hand them a race for
	 * no benefit.
	 */
	cam_stop_req = 1u;
	(void)tx_semaphore_put(&cam_frame_sem);

	if (tx_semaphore_get(&cam_stopped_sem,
	                     CAM_STOP_JOIN_TICKS) != TX_SUCCESS) {
		/*
		 * [!] THE PRODUCER IS STILL OUT THERE (issue #48).
		 *
		 * Not CAM_ST_FAULTED: that state means "rebuild me", and
		 * rebuilding hardware a live producer is using is the worst
		 * thing that could happen next.  CAM_ST_LOST refuses everything
		 * instead, for good -- see the note on the enum.
		 *
		 * The caller is required to leave the sink attached and its
		 * ownership held; camera_unsubscribe() enforces the first, and
		 * CAM_ERR_TIMEOUT (never CAM_OK) is what tells the caller about
		 * the second.
		 */
		LOG_ERR("the producer did not acknowledge a stop; camera is "
		        "unusable until reboot");
		cam_fault_latch("the producer thread did not acknowledge a stop");
		cam_state = CAM_ST_LOST;
		cam_api_exit();
		return CAM_ERR_TIMEOUT;
	}

	cam_api_exit();
	return CAM_OK;
}

/*
 * [!] A MANUAL EXPOSURE OR GAIN TAKES `camera auto` OFF (issue #39).
 *
 * The sensor's own writers put its AEC/AGC into manual (0x3503) as part of the
 * write -- they have to, or the on-chip loop overwrites the value on its next
 * frame and the command appears to do nothing.  So after a successful write the
 * HARDWARE is already in manual, and leaving cam_auto_on set meant `camera auto`
 * reported "on" while the sensor's loop was stopped.  That is the half of this
 * which is the flag catching up with a register this port had already written,
 * not a state change invented here.
 *
 * The other half IS an added effect and is worth stating plainly: this flag also
 * gates the software white balance, so a manual exposure freezes the colour
 * correction as well.  Keeping one flag is deliberate -- reporting "auto: on"
 * while only one of the two loops is running is the exact confusion this issue
 * is about, and two flags would need `camera auto` to grow two answers.  The
 * commands print that it happened rather than doing it silently, and
 * `camera auto on` hands both back.
 *
 * Only on SUCCESS.  A refused write leaves the sensor's loop running, so the
 * flag must stay as it was -- and a camera that is absent, lost or busy never
 * reaches here, because cam_bus_enter() refused first.  The queued (producer)
 * path counts as success: the producer will apply it.
 *
 * [!] AND IT IS CALLED WITH THE API MUTEX STILL HELD (issue #77).  It used to
 * be called just after cam_api_exit(), which put the flag change outside the
 * transaction that earned it -- and tx_mutex_put() is a scheduling point, so
 * the gap is not theoretical.  A `camera auto on` acquiring in it would turn
 * the sensor's AEC back on and release, and then THIS call would set the flag
 * to 0 over the top: the sensor running auto exposure while camera_auto()
 * reports off and the software white balance stays frozen.  Exactly the
 * disagreement issue #39 exists to prevent, reached from the other side.
 *
 * With this and camera_set_auto() both writing under the mutex, cam_auto_on has
 * one rule and no exceptions: only a thread holding the API mutex writes it.
 */
static void cam_manual_control_taken(void)
{
	cam_auto_on = 0u;
}

/*
 * [!] THE FOUR SETTERS AND THE ONE GETTER BELOW ALL HAVE THE SAME SHAPE, and
 * that is the point of issue #77.  Enter, be told who owns the sensor bus, and
 * only then branch -- rather than deciding first and acquiring afterwards,
 * which is what let a stream start in between and leave the fall-through doing
 * I2C on a bus the producer owned.  Six entry points had six orderings; the
 * ordering now lives once, in cam_bus_enter().
 *
 * The visible cost is that queueing takes the API mutex where it used to take
 * nothing, so these can now answer CAM_ERR_BUSY during a stream start, stop,
 * capture or probe.  That is a real change and it is the honest one: while a
 * stream is merely RUNNING the mutex is free (the producer never takes it), so
 * what a refusal reports is a genuinely concurrent operation and not the
 * steady state.  cmd_camera.c names both causes.
 */
int camera_set_exposure(uint16_t lines)
{
	enum cam_bus_owner owner;
	int rc = cam_bus_enter(&owner);

	if (rc != CAM_OK)
		return rc;
	if (owner == CAM_BUS_PRODUCER)
		cam_tune_queue_exposure(lines);
	else if ((rc = cam_bringup()) == CAM_OK)
		rc = (cam_sensor_set_exposure(lines) == 0) ? CAM_OK
		                                           : CAM_ERR_HAL;
	if (rc == CAM_OK)
		cam_manual_control_taken();
	cam_api_exit();
	return rc;
}

int camera_set_gains(uint8_t again, uint16_t dgain)
{
	enum cam_bus_owner owner;
	int rc = cam_bus_enter(&owner);

	if (rc != CAM_OK)
		return rc;
	if (owner == CAM_BUS_PRODUCER)
		cam_tune_queue_gains(again, dgain);
	else if ((rc = cam_bringup()) == CAM_OK)
		rc = (cam_sensor_set_gains(again, dgain) == 0) ? CAM_OK
		                                               : CAM_ERR_HAL;
	if (rc == CAM_OK)
		cam_manual_control_taken();
	cam_api_exit();
	return rc;
}

/*
 * The sensor's frame length (issue #38).
 *
 * [!] Unlike the exposure setters beside it, this does NOT take `camera auto`
 * off.  VTS is not a manual exposure -- it is the frame the on-chip AEC gets to
 * work inside, and the AEC keeps running afterwards, just with a different
 * ceiling.  Conflating the two would turn "make the preview faster" into
 * "and stop the exposure adapting", which is not what was asked.
 */
int camera_set_frame_length(uint16_t lines)
{
	enum cam_bus_owner owner;
	int rc = cam_bus_enter(&owner);

	if (rc != CAM_OK)
		return rc;
	if (owner == CAM_BUS_PRODUCER)
		cam_tune_queue_vts(lines);
	else if ((rc = cam_bringup()) == CAM_OK)
		rc = (cam_sensor_set_frame_length(lines) == 0) ? CAM_OK
		                                               : CAM_ERR_HAL;
	cam_api_exit();
	return rc;
}

/*
 * Read it BACK from the sensor, not from the shadow.
 *
 * That is the whole point of having it: #38 was settled by discovering that the
 * part was running on a frame length nobody had written, so a getter that
 * reports what this port last wrote would have been unable to find the bug it
 * exists to expose.
 */
int camera_read_frame_length(uint16_t *lines)
{
	enum cam_bus_owner owner;
	int rc = cam_bus_enter(&owner);

	if (rc != CAM_OK)
		return rc;
	/* A READ cannot be handed to the producer -- there is nowhere for the
	   answer to come back to -- so this is the one of the five where the
	   producer route is a refusal rather than a queued request. */
	if (owner != CAM_BUS_DIRECT) {
		cam_api_exit();
		return CAM_ERR_BUSY;    /* I2C; the producer owns that driver */
	}
	if ((rc = cam_bringup()) == CAM_OK)
		rc = (cam_sensor_read_frame_length(lines) == 0) ? CAM_OK
		                                                : CAM_ERR_HAL;
	cam_api_exit();
	return rc;
}

void camera_set_wb(const struct cam_wb *wb)
{
	if (wb != NULL)
		cam_wb = *wb;
}

int camera_set_auto(int on)
{
	enum cam_bus_owner owner;
	int rc;

	/*
	 * [!] NOTHING IS RECORDED UNTIL A ROUTE IS GRANTED (issue #77).
	 *
	 * cam_auto_on used to be written before the mutex was even attempted,
	 * and every cam_api_enter_up() failure was then turned into CAM_OK.  For
	 * an absent camera that is right -- see below -- but cam_api_enter_up()
	 * also fails on lock contention, so a `camera auto off` that collided
	 * with a stream start FLIPPED THE FLAG, WROTE NO I2C, AND REPORTED
	 * SUCCESS.  That is precisely the lie issue #39 set out to remove: the
	 * port says "auto: off" while the sensor's own AEC is still running, and
	 * the software white balance -- which shares this flag -- is frozen with
	 * it.  A refusal now leaves the flag exactly as it was, so a retry is
	 * both meaningful and idempotent.
	 *
	 * The poison test still happens first; it is inside cam_bus_enter(), and
	 * before the mutex there as it must be (issue #48).  This used to need
	 * its own copy because the streaming path never entered the API at all.
	 * It does now, so the copy is gone -- and CAM_ST_LOST is refused whether
	 * it is seen before the acquire or, as it can be, after.
	 */
	rc = cam_bus_enter(&owner);
	if (rc != CAM_OK)
		return rc;

	cam_auto_on = on ? 1u : 0u;

	/*
	 * On a part with its own AEC, "auto" means THAT loop -- ours stands down
	 * for it.  Turning auto off therefore has to take the sensor's loop off
	 * as well, or `camera exposure` is overwritten a frame later and manual
	 * control silently does not exist.
	 *
	 * That is an I2C write, so it obeys the same rule as every other tuning
	 * setter here rather than going straight at the bus: when the producer
	 * owns the CIS driver this is queued for it, and otherwise it goes under
	 * the API mutex on a brought-up sensor.  Bringing up is also what makes
	 * the request meaningful before the first probe -- detection runs there,
	 * so the write lands on the sensor that is actually fitted rather than
	 * on the default descriptor.
	 */
	if (owner == CAM_BUS_PRODUCER) {
		cam_tune_queue_auto(cam_auto_on);
		cam_api_exit();
		return CAM_OK;
	}

	/*
	 * A camera that will not come up is NOT a failure of this command.  The
	 * mode is held here and cam_bringup() applies it to whatever sensor is
	 * found, so asking with nothing plugged in records a request that is
	 * kept -- reporting an error would be telling the user to retry
	 * something that has already taken.  Only a sensor that is up and
	 * refuses the write has actually failed.
	 *
	 * [!] This is now the ONLY failure turned into success, which is the
	 * whole reason bring-up is called here instead of inside cam_bus_enter():
	 * "could not reach the sensor" and "could not get in at all" have to end
	 * differently, and folding them into one helper made them one answer.
	 */
	if (cam_bringup() != CAM_OK) {
		cam_api_exit();
		return CAM_OK;
	}
	rc = (cam_sensor_set_auto((int)cam_auto_on) == 0) ? CAM_OK
	                                                  : CAM_ERR_HAL;
	cam_api_exit();
	return rc;
}

int camera_auto(void)
{
	return cam_auto_on;
}

void camera_get_wb(struct cam_wb *out)
{
	if (out != NULL)
		*out = cam_wb;
}

/* Ticks -> microseconds at the rate TIMER2 was actually brought up at, which is
 * a run-time read-back rather than a constant.  64-bit throughout: a long
 * preview accumulates more ticks than 32 bits hold at 6 MHz. */
static uint32_t cam_prof_us(uint64_t ticks, uint32_t hz)
{
	if (hz == 0u)
		return 0u;
	return (uint32_t)((ticks * 1000000u) / hz);
}

void camera_stream_stats(struct camera_stats *out)
{
	TX_INTERRUPT_SAVE_AREA
	struct frame_stats fs;
	struct cam_prof p;
	const char *why = NULL;
	uint32_t hz;
	uint32_t sum;
	uint32_t lock_contended, lock_wait_max;

	if (out == NULL)
		return;

	/*
	 * [!] SNAPSHOT THE PROFILE UNDER A CRITICAL SECTION.
	 *
	 * Its accumulators are 64-bit and the producer adds to them between
	 * frames, so a plain read from this thread can catch a half-updated
	 * one -- and half of a 64-bit add is not a slightly wrong number, it is
	 * a wildly wrong one.  This readout exists to be acted on; a garbage
	 * value here would send someone optimising the wrong stage, which is
	 * the exact failure #38 was filed to avoid.
	 *
	 * Interrupts off is enough and is what this port already uses for the
	 * same job (see cam_apply_tuning()): with them disabled the scheduler
	 * cannot switch, so the producer cannot be mid-update.  The API mutex is
	 * deliberately NOT taken here -- see the note on cam_bus_enter(); this
	 * function has to keep answering when the camera is refusing everything.
	 * The single-word counters below need none of this.
	 */
	TX_DISABLE
	p = cam_prof;
	/* The contention pair in the same breath, and for the same reason as
	 * the panel sink's counters: the two belong together, and a stop that is
	 * at that boundary right now owns no mutex to serialise it.  Copied
	 * raw -- the conversion to milliseconds is arithmetic and has no
	 * business running with interrupts off. */
	lock_contended = cam_lock_contended;
	lock_wait_max  = cam_lock_wait_max;
	/* And the EDM tally in the same breath (issue #68).  Each word is read
	 * atomically on its own, but {count, first, last, tick, generation} has
	 * to describe ONE instant: an event landing between two of these reads
	 * would produce a tuple that never existed, which is the one thing a
	 * five-field diagnostic must not do. */
	cam_edm_snapshot(&cam_edm, &out->edm);
	TX_RESTORE

	out->lock_contended   = lock_contended;
	out->lock_wait_max_ms = (lock_wait_max * 1000u) /
	                        TX_TIMER_TICKS_PER_SECOND;

	out->streaming      = (cam_state == CAM_ST_STREAMING);
	out->frames         = cam_frames;
	out->timeouts       = cam_timeouts;
	out->retries        = cam_retries;
	out->csirx_errors   = cam_csirx_errors;
	out->relock_fails   = cam_relock_fails;
	out->dp_errors      = cam_dp_errors;
	out->last_dp_status = cam_last_dp_status;

	/*
	 * The profile is only as good as its clock, and this port already has
	 * the predicate for that: tx_glue_profile_ok() re-verifies TIMER2's
	 * control and reload registers and that it is actually counting, every
	 * time it is asked.  Saying "not trustworthy, and why" beats publishing
	 * a breakdown nobody can act on -- the same rule `thread` follows for
	 * its cpu% column.
	 */
	hz = tx_glue_epk_timer_hz();
	out->prof_ok  = (tx_glue_profile_ok(&why) && hz != 0u);
	out->prof_why = out->prof_ok ? NULL : (why != NULL ? why
	                                                   : "the EPK time source is down");
	out->prof_iters    = p.iters;
	out->prof_total_us = cam_prof_us(p.total, hz);
	out->prof_wait_us  = cam_prof_us(p.wait,  hz);
	out->prof_inval_us = cam_prof_us(p.inval, hz);
	out->prof_arm_us   = cam_prof_us(p.arm,   hz);
	out->prof_pack_us  = cam_prof_us(p.pack,  hz);
	out->prof_sink_us  = cam_prof_us(p.sink,  hz);
	out->prof_tune_us  = cam_prof_us(p.tune,  hz);
	/* Saturating, not wrapping: rounding in the six conversions can put the
	 * sum a microsecond or two over the total, and a 4-billion "other" is a
	 * worse answer than a zero one. */
	sum = out->prof_wait_us + out->prof_inval_us + out->prof_arm_us +
	      out->prof_pack_us + out->prof_sink_us + out->prof_tune_us;
	out->prof_other_us = (out->prof_total_us > sum)
	                   ? (out->prof_total_us - sum) : 0u;
	out->fault          = cam_fault;

	/* The alternation evidence (issue #59): equal-ish per-buffer counts
	 * under a stream is what says the flip is real, and the premature-
	 * disable count is what says the mask around the arm's disable is
	 * doing its job.  Single-word reads of producer-owned counters. */
	out->buf_frames[0]       = cam_wdma3_completions(0u);
	out->buf_frames[1]       = cam_wdma3_completions(1u);
	out->premature_disables  = cam_wdma3_premature_disables();

	frame_pipeline_stats(&cam_pipe, &fs);
	out->overruns = fs.overruns;
}
