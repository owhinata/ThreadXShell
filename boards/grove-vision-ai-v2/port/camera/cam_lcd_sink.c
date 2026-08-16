/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Frame sink that puts camera frames on the ST7789 panel (issue #35),
 * asynchronously since issue #57.
 *
 * WHAT MOVED, AND WHAT DID NOT
 *
 * consume() used to do the whole blit on the producer thread: a 153,600-byte
 * byte-swap copy into the driver's framebuffer, then a ~26 ms wait for the SPI
 * DMA.  For all of it the producer could not re-arm WDMA3, and #38 measured that
 * as the largest single term in the frame period.  Now consume() hands the
 * pre-pinned descriptor to this file's panel thread and returns.
 *
 * The inference did NOT move.  process() still runs on the producer inside
 * consume(), because the raw WDMA3 buffer it reads is stable only until consume()
 * returns.  draw() did move, because it runs inside the blit -- see the note on
 * the hand-off below for why that is safe without a lock.
 *
 * THE THREE THINGS THIS FILE IS CAREFUL ABOUT
 *
 * 1. The hand-off is one frame deep, and that is not an arbitrary choice: the
 *    pipeline pre-pins one delivery per sink and, under FRAME_POLICY_DROP,
 *    refuses a second while the first is outstanding.  A deeper queue could only
 *    ever hold frames the policy has already said to discard.
 *
 * 2. The panel thread's step order is the whole safety argument, and "put last"
 *    is too coarse.  put() clears the core's busy flag, and the producer outranks
 *    this thread, so a new consume() can begin before the next instruction here
 *    executes.  See sink_panel_entry().
 *
 * 3. Detach unlinks BEFORE it drains.  The other order leaves the sink reachable
 *    across a window in which a fresh stream can start.
 */
#include <stddef.h>
#include <stdint.h>

#include "tx_api.h"

#include "cam_lcd_sink.h"
#include "camera.h"
#include "cam_dp.h"
#include "cli_config.h"
#include "frame.h"
#include "frame_pipeline.h"
#include "lcd_st7789.h"
#include "tx_glue.h"

#define LOG_TAG "camlcd"
#include "log.h"

/* ---- the panel thread ---------------------------------------------------- */

/*
 * Strictly between the producer and the console.
 *
 * Above the console because a 26 ms blit that waits behind whatever the shell is
 * printing would stutter the picture for no reason; below the producer because
 * the producer's iteration IS the frame period epic #56 is shrinking, and this
 * thread has ~26 ms of slack per frame and spends nearly all of it asleep on the
 * DMA semaphore.  The asserts below are the point of exporting CAM_PRODUCER_PRIO
 * from camera.h: the ordering is checked, not restated.
 */
#define CAM_PANEL_PRIO  11u
/*
 * The deepest call here is lcd_blit_le_overlay() -> lcd_blit() ->
 * lcd_dma_burst() -> the vendor SSPI driver, plus the overlay's draw() (box
 * geometry and lcd_rect_wire()).  No inference, no interpreter -- that stayed on
 * the producer -- so this is nothing like the producer's 8 KiB.  Still a
 * STARTING allocation: `thread` reports the high-water mark and that is the
 * acceptance test.
 */
#define CAM_PANEL_STACK 2048u

_Static_assert(CAM_PANEL_PRIO > CAM_PRODUCER_PRIO,
               "the panel thread must NOT outrank the camera producer: the "
               "producer's iteration is the frame period, and a 26 ms blit "
               "ahead of it would put the blit back into that period");
_Static_assert(CAM_PANEL_PRIO < CLI_INSTANCE_PRIORITY,
               "the panel thread must outrank the console, or a frame waits "
               "behind whatever the shell is printing");
_Static_assert((CAM_PANEL_STACK % 8u) == 0u,
               "ThreadX wants an 8-byte aligned stack size");
_Static_assert(CAM_PANEL_STACK >= 1024u,
               "the deepest call here is the vendor SPI driver; anything this "
               "small has not been thought about");

static TX_THREAD cam_panel_thread;
/* Named *_stack so check_placement_budget.py pins it to DTCM.  It has to be
 * there: this is CPU-only memory, and the gate refuses a thread stack anywhere
 * else. */
static UCHAR cam_panel_stack[CAM_PANEL_STACK] __attribute__((aligned(8)));

/* consume() -> panel thread.  Posted once per accepted frame. */
static TX_SEMAPHORE cam_panel_work;
/* panel thread -> a drain waiter.  Only ever shortens the wait: every decision
 * is taken from the counters below, never from this semaphore's count, which is
 * what makes a stale token harmless. */
static TX_SEMAPHORE cam_panel_idle;

static int cam_panel_ok;   /* the thread and its objects exist */

/*
 * How long detach() waits for the panel thread.
 *
 * A blit is bounded by the driver's own DMA timeout (one second) plus its
 * quiesce, and there can be at most one outstanding once the producer is idle.
 * Three seconds clears that with room; a healthy system is out in ~26 ms.  Like
 * CAM_STOP_JOIN_TICKS in camera.c this is NOT a correctness parameter -- missing
 * it poisons the sink rather than racing the thread.
 */
#define CAM_PANEL_DRAIN_TICKS (3u * TX_TIMER_TICKS_PER_SECOND)

/* ---- lifecycle ----------------------------------------------------------- */

/*
 * [!] A STATE, not the old plain `int sink_attached`.
 *
 * That flag was read and written with no exclusion, so two attaches could both
 * observe it clear and register the SAME struct frame_sink twice -- which makes
 * its _next point at itself and hangs frame_pipeline_publish()'s walk.  The
 * shell runs commands as background jobs, so the second caller is a real thread.
 * Claiming a transition inside a critical section is what makes "exclusive"
 * true rather than likely.
 */
enum sink_state {
	SINK_DETACHED = 0,
	SINK_ATTACHING,   /**< claimed; the panel may be being brought up   */
	SINK_ATTACHED,
	SINK_DETACHING,   /**< unlinked; draining the panel thread          */
	SINK_LOST,        /**< [!] UNRECOVERABLE -- see cam_lcd_sink_detach */
};

static enum sink_state sink_state;
static const char     *sink_fault;

static struct cam_lcd_sink_stats sink_stats;

/*
 * The optional overlay (issue #48).
 *
 * Written by attach() while no stream can be running (the state claim
 * guarantees it), read by process() on the producer and by draw() on the panel
 * thread.  Never cleared while the sink is linked or a frame is outstanding --
 * see the detach contract.
 */
static struct cam_lcd_overlay sink_overlay;
static int sink_has_overlay;

/* Defined below; consume() has to name it when balancing its delivery. */
static struct frame_sink cam_lcd_sink;

/*
 * [!] THE MAILBOX DEPENDS ON THIS POLICY.
 *
 * With FRAME_POLICY_DROP the core sets the sink's busy flag before delivering
 * and clears it only in frame_pipeline_put(), so a second consume() cannot start
 * while one frame is outstanding -- which is exactly what makes a single
 * descriptor slot sufficient.  Under FRAME_POLICY_LATEST the core would instead
 * keep a pending frame and re-enter consume() from INSIDE put(), on the panel
 * thread: the inference would leave the producer and #48's ordering would break.
 * So the policy is named here, and attach() refuses if it is ever anything else.
 */
#define CAM_LCD_SINK_POLICY FRAME_POLICY_DROP

/* The one outstanding delivery.  NULL when the panel thread has taken it. */
static const struct frame_desc *sink_mbox;
/* Travels with the frame: whether process() produced boxes for it. */
static int sink_mbox_overlay_ok;

/*
 * The drain handshake.
 *
 * accepted is bumped by consume() before it posts; done is bumped by the panel
 * thread after its put().  "Idle" is the STATE `done == accepted`, which covers
 * both "the mailbox is empty" and "no blit is running" in one comparison -- and
 * a drain that tests the state cannot lose a wakeup or consume a token left by
 * an earlier frame.  Once the sink is unlinked, accepted can no longer grow, so
 * the comparison is against a value that has stopped moving.
 */
static volatile uint32_t sink_accepted;
static volatile uint32_t sink_done;

/* Blit timing, panel-thread side (issue #57).  64-bit because a long preview
 * accumulates more than 32 bits hold at 6 MHz -- and therefore read only under
 * a critical section, as the producer's profile already is. */
static uint64_t sink_blit_ticks;
static uint32_t sink_blit_frames;

static void sink_fault_latch(const char *why)
{
	if (sink_fault == NULL)
		sink_fault = why;
}

/* Claim a transition, or fail.  The whole of the mutual exclusion. */
static int sink_claim(enum sink_state from, enum sink_state to)
{
	TX_INTERRUPT_SAVE_AREA
	int ok = 0;

	TX_DISABLE
	if (sink_state == from) {
		sink_state = to;
		ok = 1;
	}
	TX_RESTORE
	return ok;
}

static void sink_set_state(enum sink_state to)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	sink_state = to;
	TX_RESTORE
}

static enum sink_state sink_get_state(void)
{
	TX_INTERRUPT_SAVE_AREA
	enum sink_state s;

	TX_DISABLE
	s = sink_state;
	TX_RESTORE
	return s;
}

/* ---- the panel thread ---------------------------------------------------- */

/* Trampoline: the LCD driver's callback signature is its own, and the sink's
 * overlay contract is the sink's.  Only reached with an overlay armed. */
static void cam_lcd_draw(void *ctx, uint16_t *fb, uint16_t fb_w, uint16_t fb_h)
{
	(void)ctx;
	sink_overlay.draw(sink_overlay.ctx, fb, fb_w, fb_h);
}

/*
 * One frame, on the panel thread.
 *
 * [!] THE STEP ORDER IS THE SAFETY ARGUMENT, and it is finer than "put last".
 *
 *   1. take the mailbox and EMPTY IT, so the next delivery has somewhere to go;
 *   2. blit, with draw() inside the panel guard;
 *   3. put -- and after this line nothing belonging to that frame, the overlay
 *      or the detections may be touched, because put() clears the core's busy
 *      flag and the producer OUTRANKS this thread: a new consume() can run
 *      before the next instruction here does;
 *   4. publish idle, which is what a drain is waiting for.
 *
 * Step 4 is deliberately after step 3 and deliberately not part of it.  What it
 * publishes is the state "done has caught up with accepted", not the event "one
 * finished" -- between 3 and 4 the producer may already have handed over another
 * frame, and a drain that returned on "one finished" would return with a blit
 * still queued.
 */
static void cam_panel_entry(ULONG arg)
{
	(void)arg;

	for (;;) {
		TX_INTERRUPT_SAVE_AREA
		const struct frame_desc *f;
		uint32_t t0;
		int overlay_ok;
		int blit;

		(void)tx_semaphore_get(&cam_panel_work, TX_WAIT_FOREVER);

		/* 1. take it and empty the mailbox */
		TX_DISABLE
		f          = sink_mbox;
		overlay_ok = sink_mbox_overlay_ok;
		sink_mbox  = NULL;
		TX_RESTORE

		if (f == NULL)
			continue;    /* a stale post; nothing was handed over */

		/* 2. the blit.  TX_NO_WAIT inside lcd_acquire(), and a skipped
		 * frame if the panel is taken: blocking here would hold the
		 * pipeline slot pinned behind whatever `lcd fill` a user
		 * happened to type, and a live preview that drops a frame is
		 * better than one that stalls the ring.  The count says it
		 * happened. */
		t0 = tx_glue_epk_timer_ticks();
		if (lcd_acquire() != 0) {
			sink_stats.busy++;
		} else {
			/* lcd_blit_le_overlay: the slot is little-endian RGB565
			 * and the panel wants wire order.  The driver owns that
			 * swap, and calls the draw hook between the swap and the
			 * DMA. */
			blit = lcd_blit_le_overlay(
				0u, 0u, (uint16_t)CAM_FRAME_WIDTH,
				(uint16_t)CAM_FRAME_HEIGHT,
				(const uint16_t *)f->data,
				(overlay_ok && sink_overlay.draw != NULL)
					? cam_lcd_draw : NULL,
				NULL, NULL, NULL);
			lcd_release();

			if (blit == 0)
				sink_stats.delivered++;
			else
				sink_stats.errors++;
		}

		/* 3. THE put, on every path above.  It must name THIS sink:
		 * frame_pipeline_put() clears the sink's busy flag only when it
		 * is given the owning sink, and a NULL there would leave the
		 * sink busy for ever and its DROP policy discarding every frame
		 * after the first.  (NULL is for releasing a
		 * frame_pipeline_pin_latest() pin, which has no sink.) */
		camera_frame_put(&cam_lcd_sink, f);

		/* Timing is taken here rather than around the blit alone so it
		 * covers everything this thread does with the frame -- which is
		 * what "the 26 ms moved here" has to mean. */
		TX_DISABLE
		sink_blit_ticks += (uint32_t)(tx_glue_epk_timer_ticks() - t0);
		sink_blit_frames++;
		sink_done++;
		TX_RESTORE

		/* 4. and only now, wake anyone draining */
		(void)tx_semaphore_put(&cam_panel_idle);
	}
}

void cam_lcd_sink_create_objects(void)
{
	if (tx_semaphore_create(&cam_panel_work, "cam_lcd", 0) != TX_SUCCESS ||
	    tx_semaphore_create(&cam_panel_idle, "cam_lcdi", 0) != TX_SUCCESS) {
		LOG_ERR("panel sink semaphore create failed");
		return;
	}

	if (tx_thread_create(&cam_panel_thread, "camlcd", cam_panel_entry, 0,
	                     cam_panel_stack, sizeof cam_panel_stack,
	                     CAM_PANEL_PRIO, CAM_PANEL_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("panel sink thread create failed");
		return;
	}

	cam_panel_ok = 1;
}

/* ---- the sink vtable ----------------------------------------------------- */

static int cam_lcd_open(void *ctx, enum frame_format fmt, uint16_t w,
                        uint16_t h)
{
	(void)ctx;

	/*
	 * Reject rather than adapt.  A sink that quietly coped with an
	 * unexpected format would turn a producer change into a picture that is
	 * subtly wrong, which is the failure this whole path is worst at
	 * diagnosing.
	 */
	if (fmt != FRAME_FMT_RGB565) {
		LOG_ERR("sink refused format %d (wants RGB565)", (int)fmt);
		return -1;
	}
	if (w != (uint16_t)CAM_FRAME_WIDTH || h != (uint16_t)CAM_FRAME_HEIGHT) {
		LOG_ERR("sink refused %ux%u (wants %ux%u)", w, h,
		        (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT);
		return -1;
	}
	if (w > lcd_width() || h > lcd_height()) {
		LOG_ERR("the panel is %ux%u; rotate it to landscape first",
		        lcd_width(), lcd_height());
		return -1;
	}
	return 0;
}

/*
 * Called on the PRODUCER thread, with the slot pre-pinned once.
 *
 * Everything that must see the frame while the datapath is still holding it
 * happens here; the blit is handed over.  The pre-pin is what keeps the slot
 * alive across that hand-off, and the panel thread balances it with exactly one
 * frame_pipeline_put() -- which is the asynchronous-sink contract
 * frame_pipeline.h already specifies.
 */
static int cam_lcd_consume(void *ctx, const struct frame_desc *f)
{
	TX_INTERRUPT_SAVE_AREA
	int overlay_ok = 0;

	(void)ctx;

	/*
	 * [!] ON THE PRODUCER, and that ordering is the design (issue #48).
	 *
	 * This is where a whole NPU inference happens under `nn preview`.  It
	 * reads the raw WDMA3 buffer, which is stable only until this function
	 * returns -- the datapath is re-armed after publish -- so it cannot be
	 * moved to the panel thread with the blit.  It runs with the panel
	 * released so that `lcd` commands on other threads keep working.  A
	 * failure means no boxes on this frame, not a blank preview: the
	 * picture is worth more than the annotation.
	 */
	if (sink_has_overlay && sink_overlay.process != NULL) {
		if (sink_overlay.process(sink_overlay.ctx, f->data,
		                         (uint16_t)CAM_FRAME_WIDTH,
		                         (uint16_t)CAM_FRAME_HEIGHT) == 0)
			overlay_ok = 1;
		else
			sink_stats.overlay_errors++;
	}

	/*
	 * Hand over.  The mailbox is provably free: the core would not have
	 * called consume() again while the previous frame was outstanding.
	 * accepted is bumped INSIDE the critical section with the store to the
	 * mailbox, so a drain can never observe a frame that is neither counted
	 * nor visible.
	 */
	TX_DISABLE
	sink_mbox            = f;
	sink_mbox_overlay_ok = overlay_ok;
	sink_accepted++;
	TX_RESTORE

	(void)tx_semaphore_put(&cam_panel_work);
	return 0;
}

/*
 * [!] DELIBERATELY DOES NOTHING (issue #57).
 *
 * frame_pipeline_detach() calls this after unlinking, which is the middle of
 * this module's teardown and not the end of it: the panel thread may still be
 * inside a blit, reading the overlay and the detections.  Tearing anything down
 * here would mutate state a live thread is using -- so the state machine lives
 * in cam_lcd_sink_detach(), which knows whether the drain succeeded, and this
 * hook stays as a marker that the omission is deliberate.
 */
static void cam_lcd_close(void *ctx)
{
	(void)ctx;
}

static struct frame_sink cam_lcd_sink = {
	.name    = "lcd",
	.ctx     = NULL,
	.policy  = CAM_LCD_SINK_POLICY,
	.open    = cam_lcd_open,
	.consume = cam_lcd_consume,
	.close   = cam_lcd_close,
};

/* ---- attach / detach ----------------------------------------------------- */

int cam_lcd_sink_attach(const struct cam_lcd_overlay *ov)
{
	TX_INTERRUPT_SAVE_AREA
	int rc;

	if (!cam_panel_ok) {
		/* No worker: a delivery would pin its slot for ever and stall
		 * the ring.  Refusing is the only safe answer. */
		LOG_ERR("the panel thread does not exist");
		return CAM_ERR_STATE;
	}

	/*
	 * [!] The policy is load-bearing -- see CAM_LCD_SINK_POLICY.  Checked
	 * rather than assumed because the failure it guards against is silent:
	 * a LATEST sink would keep working and simply run the inference on the
	 * wrong thread.
	 */
	if (cam_lcd_sink.policy != (uint8_t)CAM_LCD_SINK_POLICY) {
		LOG_ERR("the sink policy is not DROP; the one-frame hand-off "
		        "is not valid");
		return CAM_ERR_STATE;
	}

	/*
	 * [!] BUSY, not "already done".  The shell runs commands as background
	 * jobs, so a second `camera preview` is a real second caller -- and if
	 * it were told the attach succeeded it would go on to fail at
	 * camera_stream_start() and then detach on its way out, taking the
	 * FIRST preview's sink with it.  The first preview would keep capturing
	 * while the panel silently stopped updating.
	 */
	if (!sink_claim(SINK_DETACHED, SINK_ATTACHING)) {
		enum sink_state s = sink_get_state();

		return (s == SINK_LOST) ? CAM_ERR_STATE : CAM_ERR_BUSY;
	}

	/* Outside the critical section on purpose: lcd_init() is a reset pulse,
	 * an init table and a priming DMA transfer.  Only the claim above and
	 * the transition below are indivisible. */
	if (!lcd_ready()) {
		if (lcd_init() != 0) {
			LOG_ERR("panel bring-up failed");
			sink_set_state(SINK_DETACHED);
			return CAM_ERR_HAL;
		}
	}

	/*
	 * Landscape.  The camera delivers 320x240 and the panel is natively
	 * 240x320; issue #31 established by measurement that MADCTL really
	 * rotates THIS panel over 4-wire SPI (unlike the Wio's RGB-parallel
	 * one), so the controller transposes for free and no CPU-side rotation
	 * -- svc/gfx_rot and its 150 KB round trip -- is involved.
	 */
	if (lcd_width() < (uint16_t)CAM_FRAME_WIDTH ||
	    lcd_height() < (uint16_t)CAM_FRAME_HEIGHT) {
		if (lcd_set_rotation(90u) != 0) {
			LOG_ERR("could not rotate the panel to landscape");
			sink_set_state(SINK_DETACHED);
			return CAM_ERR_HAL;
		}
	}

	sink_stats.delivered      = 0u;
	sink_stats.dropped        = 0u;
	sink_stats.errors         = 0u;
	sink_stats.busy           = 0u;
	sink_stats.overlay_errors = 0u;

	/* The hand-off counters are normalised per attach, so a drain never
	 * compares against a previous preview's numbers.  Safe here: nothing is
	 * linked yet, so the panel thread has nothing to do. */
	TX_DISABLE
	sink_mbox            = NULL;
	sink_mbox_overlay_ok = 0;
	sink_accepted        = 0u;
	sink_done            = 0u;
	sink_blit_ticks      = 0u;
	sink_blit_frames     = 0u;
	TX_RESTORE
	while (tx_semaphore_get(&cam_panel_work, TX_NO_WAIT) == TX_SUCCESS)
		;
	while (tx_semaphore_get(&cam_panel_idle, TX_NO_WAIT) == TX_SUCCESS)
		;

	/* Published before the subscribe, so the producer can never see a sink
	 * that is attached with a half-written overlay. */
	if (ov != NULL) {
		sink_overlay     = *ov;
		sink_has_overlay = 1;
	} else {
		sink_has_overlay = 0;
	}

	rc = camera_subscribe(&cam_lcd_sink);
	if (rc != CAM_OK) {
		sink_has_overlay = 0;
		sink_set_state(SINK_DETACHED);
		return rc;
	}

	sink_set_state(SINK_ATTACHED);
	return CAM_OK;
}

/*
 * Wait for the panel thread to have caught up with everything handed to it.
 *
 * Tests the STATE (done == accepted), never the semaphore's count: the token
 * only shortens the wait, so one left over from an earlier frame cannot make
 * this return early and a missed one cannot make it hang.
 *
 * [!] AND THE DEADLINE IS WALL CLOCK, NOT ITERATIONS.  Nothing consumes
 * cam_panel_idle while a preview runs, so by the time anyone drains there is one
 * stale token per frame shown -- hundreds of them.  Counting iterations, each
 * "at most one tick", would then burn the whole budget in microseconds and time
 * out on a panel thread that was blitting perfectly normally: a preview long
 * enough to bank CAM_PANEL_DRAIN_TICKS tokens (about a minute) would poison
 * itself on exit.  tx_time_get() cannot be fooled that way, and the stale tokens
 * are cleared first so the loop does not spin through them.
 */
static int cam_panel_drain(void)
{
	ULONG start = tx_time_get();

	while (tx_semaphore_get(&cam_panel_idle, TX_NO_WAIT) == TX_SUCCESS)
		;

	for (;;) {
		TX_INTERRUPT_SAVE_AREA
		uint32_t acc, don;

		TX_DISABLE
		acc = sink_accepted;
		don = sink_done;
		TX_RESTORE

		if (don == acc)
			return CAM_OK;
		if ((ULONG)(tx_time_get() - start) >= CAM_PANEL_DRAIN_TICKS)
			return CAM_ERR_TIMEOUT;

		(void)tx_semaphore_get(&cam_panel_idle, 1u);
	}
}

int cam_lcd_sink_detach(void)
{
	int rc;

	switch (sink_get_state()) {
	case SINK_DETACHED:
		return CAM_OK;              /* nothing to do; not an error */
	case SINK_LOST:
		return CAM_ERR_STATE;
	default:
		break;
	}

	if (!sink_claim(SINK_ATTACHED, SINK_DETACHING))
		return CAM_ERR_BUSY;        /* someone else is mid-transition */

	/*
	 * [!] UNLINK FIRST (issue #57).
	 *
	 * This is svc/frame_pipeline's own contract -- detach stops further
	 * consume() and reports what is still in flight; the owner of the sink's
	 * thread then drains it.  Doing it the other way round leaves the sink
	 * reachable across a window in which another command can start a stream:
	 * camera_stream_stop() releases the camera API mutex before it returns,
	 * and `camera bench` starts a stream while owning no sink.  The sink
	 * would then be delivered frames from a stream its owner had already
	 * finished with.
	 */
	rc = camera_unsubscribe(&cam_lcd_sink);
	if (rc != CAM_OK) {
		/*
		 * Refused -- which is what a lost producer gets.  The sink is
		 * STILL LINKED and a consume() may still be running in it, so
		 * nothing here may be torn down and nothing may be handed back.
		 */
		LOG_ERR("the camera refused the unsubscribe (%d)", rc);
		sink_fault_latch("the camera refused to unsubscribe the panel");
		sink_set_state(SINK_LOST);
		return CAM_ERR_STATE;
	}

	rc = cam_panel_drain();
	if (rc != CAM_OK) {
		/*
		 * [!] THE PANEL THREAD IS STILL OUT THERE.
		 *
		 * The same answer #48 gave for a producer that never
		 * acknowledged a stop: refuse for good rather than tear down
		 * underneath a live thread.  The overlay is NOT cleared -- the
		 * thread may be inside draw(), reading the detections -- the
		 * frame stays pinned, and the caller must keep whatever it was
		 * holding on this sink's behalf (`nn preview` keeps its lease).
		 * All of this state is static, so keeping it costs nothing.
		 */
		LOG_ERR("the panel thread did not finish; the preview sink is "
		        "unusable until reboot");
		sink_fault_latch("the panel thread did not release a frame");
		sink_set_state(SINK_LOST);
		return CAM_ERR_TIMEOUT;
	}

	/* Provably idle: no delivery outstanding and none can start. */
	sink_has_overlay = 0;
	sink_set_state(SINK_DETACHED);
	return CAM_OK;
}

/* ---- statistics ---------------------------------------------------------- */

void cam_lcd_sink_stats(struct cam_lcd_sink_stats *out)
{
	TX_INTERRUPT_SAVE_AREA
	const char *why = NULL;
	uint64_t ticks;
	uint32_t frames;
	uint32_t hz;

	if (out == NULL)
		return;

	/*
	 * [!] SNAPSHOT UNDER A CRITICAL SECTION, and the 64-bit accumulator is
	 * why it is not optional.
	 *
	 * The panel thread adds to it after every frame, so a plain read from
	 * the console thread can catch a half-updated one -- and half of a
	 * 64-bit add is not a slightly wrong number, it is a wildly wrong one.
	 * The producer's profile already does exactly this, for exactly this
	 * reason.  The counters are single words and could not tear, but they
	 * are now written from TWO threads (overlay_errors on the producer, the
	 * rest on the panel), so they are taken in the same breath rather than
	 * as a mixture of two instants.
	 */
	TX_DISABLE
	*out   = sink_stats;
	ticks  = sink_blit_ticks;
	frames = sink_blit_frames;
	TX_RESTORE

	/* The pipeline counts drops (policy DROP, sink busy with a previous
	 * frame); the sink counts what it did with the ones it got.  Reading
	 * the drop count from the ring rather than keeping a second one here is
	 * what stops the two disagreeing. */
	out->dropped = cam_lcd_sink.dropped;

	/*
	 * The measurement is only as good as its clock, and this port has the
	 * predicate for that: tx_glue_profile_ok() re-verifies TIMER2 every
	 * time it is asked.  Saying "not trustworthy, and why" beats publishing
	 * a number nobody can act on -- the rule `thread` and `camera stats`
	 * already follow.
	 */
	hz = tx_glue_epk_timer_hz();
	out->prof_ok  = (tx_glue_profile_ok(&why) && hz != 0u);
	out->prof_why = out->prof_ok ? NULL
	                             : (why != NULL ? why
	                                            : "the EPK time source is down");
	out->blit_frames = frames;
	out->blit_us     = out->prof_ok
	                 ? (uint32_t)((ticks * 1000000u) / hz) : 0u;

	/* Separate from prof_why: one says the CLOCK is untrustworthy, the
	 * other says the SINK is finished.  Reporting either through the other
	 * would make a poisoned sink look like a timer problem. */
	out->fault = sink_fault;
}
