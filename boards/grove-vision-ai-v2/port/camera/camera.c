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
 * 3. There is exactly ONE stop sequence (cam_imx219_full_stop), and every path
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
#include "cam_imx219.h"
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
 * blitted promptly, and below nothing else -- there is no other application
 * thread on this board.  4 KB of stack is generous for a loop whose deepest
 * call is the LCD sink's blit; the high-water mark is worth measuring on
 * hardware before trimming it, which is why it is a named constant.
 */
#define CAM_PRODUCER_PRIO   10u
/*
 * [!] 8 KiB, not 4, since issue #48: this thread now runs INFERENCE.
 *
 * A sink's consume() may call TFLM's Invoke() -- the whole interpreter plus the
 * ethos-u driver plus the BlazeFace decoder -- and that call chain has only
 * ever run on a 4 KiB shell or background-job stack.  The producer's own peak
 * was measured at 568 B before this, so the old allocation was sized for a
 * thread that did almost nothing.
 *
 * This is a STARTING allocation, not a proof.  `thread` reports the high-water
 * mark from ThreadX's fill pattern; the acceptance test is that mark with real
 * margin, measured on the success path and on both cancellation paths.
 */
#define CAM_PRODUCER_STACK  8192u

/*
 * These stay HERE and not in shell/include/cli_config.h.  That header is the
 * shared shell core's configuration -- instances, the job pool, the transport --
 * and all three boards include it; a Grove-only camera thread's numbers in
 * there would be board specifics in a board-independent header, which is the
 * layering rule backwards.  The precedent is the same on the other two boards,
 * whose camera producers define their own CAM_PRODUCER_PRIO / _STACK locally.
 *
 * What the shared header IS the authority on is where this thread has to sit
 * relative to the console, so that is asserted against it rather than restated.
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
               "the producer's deepest call is the LCD sink's blit; anything "
               "this small has not been thought about");

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

enum cam_state {
	CAM_ST_DOWN = 0,   /**< nothing brought up                        */
	CAM_ST_READY,      /**< powered, wrapped, sensor identified       */
	CAM_ST_STREAMING,
	CAM_ST_FAULTED,    /**< terminal; the next open() rebuilds it all */
	CAM_ST_LOST,       /**< [!] UNRECOVERABLE -- see below            */
};

/*
 * [!] CAM_ST_LOST: the producer never acknowledged a stop (issue #48).
 *
 * Every other state describes hardware this thread controls.  This one
 * describes hardware that MAY STILL BE IN USE by a producer thread which did
 * not come back inside the join deadline -- so the one thing that must not
 * happen is another command rebuilding, quiescing or powering it underneath
 * that thread.
 *
 * CAM_ST_FAULTED cannot express this: it is explicitly the recoverable
 * terminal state, and cam_bringup() treats it as "rebuild everything", which is
 * exactly the action that would be catastrophic here.  So this is separate, and
 * it is reached only from the join timeout.
 *
 * WHY REBOOT AND NOT A RETRY.  There is nothing to wait for.  The producer is
 * blocked somewhere the deadline already proved is longer than expected -- a
 * lost NPU interrupt, a wedged vendor driver -- and no later call can learn
 * whether it has finished, because the acknowledgement it would have used is
 * the very thing that did not arrive.  Refusing forever is the only honest
 * answer, and it is cheap: this state is unreachable unless something is
 * already badly wrong.
 */

static struct frame_pipeline cam_pipe;
static TX_MUTEX     cam_pipe_mutex;
static TX_SEMAPHORE cam_frame_sem;   /* posted by the datapath callback   */
static TX_SEMAPHORE cam_start_sem;   /* thread -> producer: run           */
static TX_SEMAPHORE cam_stopped_sem; /* producer -> thread: idle again    */
static TX_MUTEX     cam_api_mutex;   /* serialises the public entry points */
static int          cam_objects_ok;

static enum cam_state cam_state;
static uint8_t  cam_stop_req;
static uint8_t  cam_datapath_configured;
static uint8_t  cam_rev_c;
/* Next datapath configuration uses the RAW leg (INP -> WDMA2, no demosaic). */
static uint8_t  cam_raw_mode;

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
 * 16 is not a guess tuned to one scene: the datasheet fixes the sensor's black
 * level at 64 in RAW10 (which is 16 once the receiver has taken the top eight
 * bits) and at 16 natively in RAW8, so it is the same number either way.
 * Raising it further trades shadow detail for contrast and is a scene decision
 * -- `camera wb <r> <g> <b> <black>`.
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

#define CAM_TUNE_EXPOSURE  0x1u
#define CAM_TUNE_GAINS     0x2u
#define CAM_TUNE_AUTO      0x10u

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

	if ((req & CAM_TUNE_EXPOSURE) != 0u)
		(void)cam_imx219_set_exposure(cam_tune_exposure);
	if ((req & CAM_TUNE_GAINS) != 0u)
		(void)cam_imx219_set_gains(cam_tune_again, cam_tune_dgain);
	/* The sensor half of `camera auto` -- taking a part's own AEC/AGC off is
	 * an I2C write like any other, and the console may not make it itself
	 * while this thread owns the CIS driver. */
	if ((req & CAM_TUNE_AUTO) != 0u)
		(void)cam_imx219_set_sensor_auto(cam_tune_auto);
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
		 * (-100..-108), every EDM timing error and watchdog timeout --
		 * and anything unrecognised. */
		if (cam_err == 0)
			cam_err = (int32_t)status;
		break;
	}

	(void)tx_semaphore_put(&cam_frame_sem);
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

int camera_subscribe(struct frame_sink *sink)
{
	if (sink == NULL || !cam_objects_ok)
		return CAM_ERR_PARAM;
	return (frame_pipeline_attach(&cam_pipe, sink) == 0) ? CAM_OK
	                                                     : CAM_ERR_PARAM;
}

int camera_unsubscribe(struct frame_sink *sink)
{
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
	(void)frame_pipeline_detach(&cam_pipe, sink);
	return CAM_OK;
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

static int cam_step_power_on(void) { return cam_imx219_power_on(); }

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
	if (cam_imx219_csirx_enable() != 0)
		return -1;
	return cam_raw_mode ? cam_imx219_datapath_config_raw()
	                    : cam_imx219_datapath_config();
}

static int cam_step_capture(void) { return cam_imx219_capture_start(); }

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
	cam_imx219_full_stop();
	cam_unwrap_all();
	cam_imx219_power_off();
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
	if (cam_imx219_sensor_init() != 0)
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
	if (cam_imx219_set_sensor_auto((int)cam_auto_on) != 0)
		return cam_bringup_fail("the sensor kept its own exposure loop",
		                        CAM_ERR_HAL);
	/* Against the DETECTED part's ID, not a hard-coded one: detection has
	 * already established which sensor is in the connector, and re-checking
	 * against the other one's number is how a working OV5647 gets reported
	 * as "the sensor did not identify itself". */
	if (cam_imx219_read_id(&id) != 0 || id != cam_imx219_sensor_id()) {
		LOG_ERR("sensor model id 0x%04X, expected 0x%04X", id,
		        cam_imx219_sensor_id());
		return cam_bringup_fail("the sensor did not identify itself",
		                        CAM_ERR_NO_SENSOR);
	}

	info.chip_version = cam_imx219_chip_version();
	cam_rev_c = cam_imx219_needs_rev_c_bounce() ? 1u : 0u;

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
	if (cam_imx219_csirx_clear_errors() != 0) {
		cam_fault_latch("the CSI receiver error status would not clear");
		return CAM_ERR_HAL;
	}

	if (cam_wrapped_step(cam_step_dp) != 0) {
		cam_fault_latch("the datapath refused its configuration");
		return CAM_ERR_HAL;
	}
	cam_datapath_configured = 1u;

	/* Sensor I2C: outside the masked round, as in the bring-up. */
	if (cam_imx219_stream_on() != 0) {
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
 *    next reboot.  cam_imx219_full_stop() IS the vendor's close, and it moves
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

	/* 3. now the vendor may do as it likes with them */
	cam_imx219_full_stop();
	cam_datapath_configured = 0u;

	/* 4. and only now is clearing meaningful */
	(void)cam_imx219_csirx_clear_errors();
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
	if (cam_imx219_stream_off() != 0) {
		cam_fault_latch("the sensor would not stop for the rev-C bounce");
		return -1;
	}
	cam_imx219_csirx_disable();
	/* Clear WHILE disabled: the status latches, so this is the only moment
	 * at which a later poll can mean anything about the new link. */
	if (cam_imx219_csirx_clear_errors() != 0) {
		cam_fault_latch("the CSI error status would not clear on bounce");
		return -1;
	}
	return 0;
}

static int cam_rev_c_on(void)
{
	struct cam_csirx_errors err;

	if (cam_imx219_csirx_enable() != 0) {
		cam_fault_latch("the CSI receiver would not come back up");
		return -1;
	}
	if (cam_imx219_stream_on() != 0) {
		cam_fault_latch("the sensor would not restart after the bounce");
		return -1;
	}

	cam_imx219_csirx_errors(&err);
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
 * cam_imx219_* calls the console uses -- this thread owns the CIS driver while
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

	cam_frame_means_x100(cam_imx219_raw_buffer(), CAM_FRAME_PIXELS,
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
	(void)cam_imx219_refresh_exposure_gains();

	/* White balance is software, so it takes effect on the NEXT frame this
	 * thread packs -- no sensor round trip, no delay to design around. */
	(void)cam_awb_step(means[0], means[1], means[2], &cam_wb);
}

static void cam_publish(void)
{
	struct frame_desc *slot;
	uint8_t *raw = cam_imx219_raw_buffer();

	/*
	 * [!] Invalidate BEFORE reading and only AFTER the write-DMA has
	 * stopped -- which frame-ready is exactly the statement of.  The
	 * shipping vendor glue never does this for the WDMA3 buffer (it
	 * invalidates only the 32-byte JPEG size word), so the omission is easy
	 * to inherit: 225 KB of DMA'd pixels read back through stale cache
	 * lines produces a picture that is mostly right and partly last frame.
	 */
	hx_InvalidateDCache_by_Addr((volatile void *)raw, (int32_t)CAM_RAW_BYTES);

	slot = frame_pipeline_acquire(&cam_pipe);
	if (slot == NULL)
		return;   /* the ring counts this as an overrun */

	cam_bgr_planar_to_rgb565_wb(raw, (uint16_t *)slot->data,
	                            CAM_FRAME_PIXELS, &cam_wb);

	frame_pipeline_publish(&cam_pipe, slot, CAM_FRAME_PIXELS * 2u,
	                       FRAME_FMT_RGB565, (uint16_t)CAM_FRAME_WIDTH,
	                       (uint16_t)CAM_FRAME_HEIGHT,
	                       (uint16_t)(CAM_FRAME_WIDTH * 2u));
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
	UINT rc = tx_semaphore_get(&cam_frame_sem, CAM_FRAME_TIMEOUT_TICKS);

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

	if (cam_rev_c && cam_rev_c_off() != 0) {
		cam_relock_fails++;
		return -1;
	}

	cam_publish();

	if (cam_rev_c && cam_rev_c_on() != 0) {
		cam_relock_fails++;
		return -1;
	}

	/* Between frames: the only moment at which changing the sensor's
	 * exposure cannot tear one, and the only moment at which this thread is
	 * provably the sole user of the vendor CIS driver. */
	cam_auto_step();
	cam_apply_tuning();

	/* WDMA3 is a single buffer, so the next capture is armed only now that
	 * the pixels have been consumed -- that is what makes tearing
	 * impossible rather than unlikely. */
	cam_imx219_retrigger();

	/*
	 * [!] AND TAKE THE INTERRUPTS BACK, every frame.
	 *
	 * Measured on hardware: irq 143 is wrapped at start-up with one vendor
	 * handler and is carrying a DIFFERENT one a frame later -- the
	 * retrigger path re-registers its ISR.  The measure-then-wrap protocol
	 * cannot see that (the line never changes its ENABLED state), so
	 * without this the line silently stops being accounted one frame into
	 * every stream, and `thread` blanks the whole cpu% column for as long
	 * as the preview runs.
	 *
	 * Cheap: one vector read and a compare per wrapped line, a handful of
	 * lines, once per 26 ms frame.  Under a critical section because it may
	 * write the vector table, and an interrupt taken mid-write would
	 * dispatch through a half-updated entry.
	 */
	cam_reassert_wraps();
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
		 * camera_stream_stop() is refused by cam_api_enter() before it
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

static int cam_api_enter(void)
{
	if (!cam_objects_ok)
		return CAM_ERR_STATE;
	/*
	 * [!] THE POISON CHECK, and it is deliberately BEFORE the mutex
	 * (issue #48).
	 *
	 * Every entry point that touches camera hardware comes through here, so
	 * this one test is what makes "the producer may still be running"
	 * safe: nothing can power the module, quiesce the datapath, unwrap an
	 * interrupt or rebuild the port while a thread that never acknowledged
	 * a stop is still in flight.
	 *
	 * Before the mutex because the lost producer may itself still hold
	 * things; making a poisoned call queue behind anything would turn a
	 * clear refusal into a hang.  camera_stream_stats() reads no hardware
	 * and takes no mutex, so `camera stats` still answers and still says
	 * what happened -- a refusal with no way to see why is not a diagnosis.
	 */
	if (cam_state == CAM_ST_LOST)
		return CAM_ERR_STATE;
	if (tx_mutex_get(&cam_api_mutex, TX_NO_WAIT) != TX_SUCCESS)
		return CAM_ERR_BUSY;
	return CAM_OK;
}

static void cam_api_exit(void)
{
	(void)tx_mutex_put(&cam_api_mutex);
}

/*
 * Enter the API AND make sure the port is up.
 *
 * The tuning entry points all talk to the sensor over I2C, which needs the
 * module powered and the CIS layer initialised.  Requiring the operator to have
 * run `camera probe` first is an ordering rule with no reason behind it that
 * they can see -- `camera capture` brings the port up on its own, so `camera
 * gain` refusing until something else has run just reads as a broken command.
 */
static int cam_api_enter_up(void)
{
	int rc = cam_api_enter();

	if (rc != CAM_OK)
		return rc;
	rc = cam_bringup();
	if (rc != CAM_OK) {
		cam_api_exit();
		return rc;
	}
	return CAM_OK;
}

int camera_probe(struct camera_probe_info *out)
{
	int rc;

	if (out == NULL)
		return CAM_ERR_PARAM;
	rc = cam_api_enter();
	if (rc != CAM_OK)
		return rc;

	rc = cam_bringup();
	if (rc == CAM_OK) {
		out->chip_version = cam_imx219_chip_version();
		out->rev_c = cam_rev_c;
		if (cam_imx219_read_id(&out->sensor_id) != 0) {
			out->sensor_id = 0u;
			rc = CAM_ERR_NO_SENSOR;
		}
	}

	cam_api_exit();
	return rc;
}

const uint8_t *camera_raw_frame(void)
{
	return cam_imx219_raw_buffer();
}

int camera_capture_raw(void)
{
	int rc;

	/*
	 * The same one-shot capture, with the demosaic taken out of the path so
	 * that what lands in the buffer is the Bayer mosaic itself.  The flag is
	 * cleared before returning, so a later preview is unaffected however
	 * this one ends.
	 */
	cam_raw_mode = 1u;
	rc = camera_capture();
	cam_raw_mode = 0u;
	return rc;
}

int camera_capture(void)
{
	uint32_t waited;
	int rc;

	rc = cam_api_enter();
	if (rc != CAM_OK)
		return rc;
	if (cam_state == CAM_ST_STREAMING) {
		cam_api_exit();
		return CAM_ERR_BUSY;
	}

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
				(volatile void *)cam_imx219_raw_buffer(),
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

int camera_stream_start(void)
{
	int rc = cam_api_enter();

	if (rc != CAM_OK)
		return rc;
	if (cam_state == CAM_ST_STREAMING) {
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

	cam_stop_req = 0u;
	cam_state = CAM_ST_STREAMING;
	(void)tx_semaphore_put(&cam_start_sem);

	cam_api_exit();
	return CAM_OK;
}

int camera_stream_stop(void)
{
	int rc = cam_api_enter();

	if (rc != CAM_OK)
		return rc;
	if (cam_state != CAM_ST_STREAMING) {
		cam_api_exit();
		return CAM_OK;         /* already stopped: not an error */
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

int camera_set_exposure(uint16_t lines)
{
	int rc;

	if (cam_state == CAM_ST_STREAMING) {
		cam_tune_exposure = lines;
		cam_tune_req |= CAM_TUNE_EXPOSURE;
		return CAM_OK;
	}

	rc = cam_api_enter_up();
	if (rc != CAM_OK)
		return rc;
	rc = (cam_imx219_set_exposure(lines) == 0) ? CAM_OK : CAM_ERR_HAL;
	cam_api_exit();
	return rc;
}

int camera_set_gains(uint8_t again, uint16_t dgain)
{
	int rc;

	if (cam_state == CAM_ST_STREAMING) {
		cam_tune_again = again;
		cam_tune_dgain = dgain;
		cam_tune_req |= CAM_TUNE_GAINS;
		return CAM_OK;
	}

	rc = cam_api_enter_up();
	if (rc != CAM_OK)
		return rc;
	rc = (cam_imx219_set_gains(again, dgain) == 0) ? CAM_OK : CAM_ERR_HAL;
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
	int rc;

	/*
	 * [!] THE POISON CHECK HAS TO BE FIRST HERE, and only here (issue #48).
	 *
	 * Every other setter reaches hardware through cam_api_enter(), which
	 * refuses.  This one is different twice over: it writes cam_auto_on
	 * BEFORE going near the mutex, and it deliberately converts a bring-up
	 * failure into CAM_OK (the request is kept and applied later).  Both are
	 * right for a camera that is merely absent and wrong for one whose
	 * producer never came back -- the flag is read by cam_auto_step() on
	 * that very thread, and reporting success would be a lie about a device
	 * that is finished.
	 */
	if (cam_state == CAM_ST_LOST)
		return CAM_ERR_STATE;

	cam_auto_on = on ? 1u : 0u;

	/*
	 * On a part with its own AEC, "auto" means THAT loop -- ours stands down
	 * for it.  Turning auto off therefore has to take the sensor's loop off
	 * as well, or `camera exposure` is overwritten a frame later and manual
	 * control silently does not exist.
	 *
	 * That is an I2C write, so it obeys the same two rules as every other
	 * tuning setter here rather than going straight at the bus: while a
	 * stream runs the producer owns the CIS driver and this is queued for
	 * it, and otherwise it goes under the API mutex on a brought-up sensor.
	 * Bringing up is also what makes the request meaningful before the first
	 * probe -- detection runs there, so the write lands on the sensor that
	 * is actually fitted rather than on the default descriptor.
	 */
	if (cam_state == CAM_ST_STREAMING) {
		cam_tune_auto = cam_auto_on;
		cam_tune_req |= CAM_TUNE_AUTO;
		return CAM_OK;
	}

	/*
	 * A camera that will not come up is NOT a failure of this command.  The
	 * mode is held here and cam_bringup() applies it to whatever sensor is
	 * found, so asking with nothing plugged in records a request that is
	 * kept -- reporting an error would be telling the user to retry
	 * something that has already taken.  Only a sensor that is up and
	 * refuses the write has actually failed.
	 */
	rc = cam_api_enter_up();
	if (rc != CAM_OK)
		return CAM_OK;
	rc = (cam_imx219_set_sensor_auto((int)cam_auto_on) == 0) ? CAM_OK
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

void camera_stream_stats(struct camera_stats *out)
{
	struct frame_stats fs;

	if (out == NULL)
		return;

	out->streaming      = (cam_state == CAM_ST_STREAMING);
	out->frames         = cam_frames;
	out->timeouts       = cam_timeouts;
	out->retries        = cam_retries;
	out->csirx_errors   = cam_csirx_errors;
	out->relock_fails   = cam_relock_fails;
	out->dp_errors      = cam_dp_errors;
	out->last_dp_status = cam_last_dp_status;
	out->fault          = cam_fault;

	frame_pipeline_stats(&cam_pipe, &fs);
	out->overruns = fs.overruns;
}
