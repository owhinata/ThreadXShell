/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_sink.c
 * @brief   The camera sink and its steps (issue #142).  See cam_sink.h for the
 *          design and cam_step.h for the step machine.
 */
#include "cam_sink.h"

#include <stddef.h>

#include "tx_api.h"
#include "stm32f7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include "camera.h"          /* camera_subscribe / camera_frame_put / CAM_ERR_* */
#include "frame_pipeline.h"  /* struct frame_sink / FRAME_POLICY_DROP */

#define LOG_TAG "camsink"
#include "log.h"

static struct cam_sink_step *cs_steps[CAM_SINK_MAX_STEPS];
static unsigned              cs_nsteps;
/* Guards every step's `want`, the arming and disarming (with the PRIMASK
 * section inside), and cs_attached / cs_fmt / cs_w / cs_h. */
static TX_MUTEX              cs_lock;
static int                   cs_ready;
/* The base has this sink attached, and the geometry it attached with -- so a
 * step enabled while the base runs can be opened the way the attach would
 * have opened it. */
static int                   cs_attached;
static enum frame_format     cs_fmt;
static uint16_t              cs_w, cs_h;
/* Set once camera_subscribe() accepted the sink.  A race between two first
 * enables only calls camera_subscribe() twice, which is idempotent. */
static volatile int          cs_subscribed;

static struct frame_sink cam_sink;

/* ---- the PRIMASK half of the step machine -------------------------------- */

static int step_enter(struct cam_sink_step *s)
{
	uint32_t primask = __get_PRIMASK();
	int in;

	__disable_irq();
	in = cam_step_enter(&s->st);
	__set_PRIMASK(primask);
	return in;
}

static void step_leave(struct cam_sink_step *s)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	(void)cam_step_leave(&s->st);
	__set_PRIMASK(primask);
}

static int step_may_arm(struct cam_sink_step *s)
{
	uint32_t primask = __get_PRIMASK();
	int may;

	__disable_irq();
	may = cam_step_may_arm(&s->st);
	__set_PRIMASK(primask);
	return may;
}

static int step_arm(struct cam_sink_step *s)
{
	uint32_t primask = __get_PRIMASK();
	int rc;

	__disable_irq();
	rc = cam_step_arm(&s->st);
	__set_PRIMASK(primask);
	return rc;
}

/* Returns whether the step WAS armed: its close() is then owed. */
static int step_disarm(struct cam_sink_step *s)
{
	uint32_t primask = __get_PRIMASK();
	int was;

	__disable_irq();
	was = s->st.armed != 0u;
	cam_step_disarm(&s->st);
	__set_PRIMASK(primask);
	return was;
}

/* Open and arm @p s for the attached geometry.  cs_lock held.  Not armed, and
 * nothing owed, if the producer is still inside from before (a disable whose
 * drain has not finished) or the step's open() refused. */
static int step_open_locked(struct cam_sink_step *s)
{
	if (!step_may_arm(s))
		return CAM_ERR_BUSY;
	if (s->ops->open(s->ctx, cs_fmt, cs_w, cs_h) != 0)
		return CAM_ERR_BUSY;
	/* The step is disarmed, so the producer cannot have entered since the
	   check above: the arm cannot be refused.  If it ever were, undo the open
	   rather than leave a step initialised and unreachable. */
	if (step_arm(s) != CAM_STEP_OK) {
		s->ops->close(s->ctx);
		return CAM_ERR_BUSY;
	}
	return 0;
}

static void step_close_locked(struct cam_sink_step *s)
{
	if (step_disarm(s))
		s->ops->close(s->ctx);
}

/* ---- the sink ------------------------------------------------------------- */

/* The base attached the sink (under the camera lock): open every enabled step.
   A step that refuses this geometry stays enabled and unarmed until the next
   attach; the sink itself never refuses an RGB565 base, because the other step
   may still want it. */
static int cam_sink_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx;
	if (fmt != FRAME_FMT_RGB565)
		return -1;
	tx_mutex_get(&cs_lock, TX_WAIT_FOREVER);
	cs_attached = 1;
	cs_fmt = fmt;
	cs_w = w;
	cs_h = h;
	for (unsigned i = 0; i < cs_nsteps; i++) {
		struct cam_sink_step *s = cs_steps[i];

		if (s->want && step_open_locked(s) != 0)
			LOG_WRN("step '%s' not opened for %ux%u", s->name,
			        (unsigned)w, (unsigned)h);
	}
	tx_mutex_put(&cs_lock);
	return 0;
}

/* [!] PUT LAST.  camera_frame_put() must stay the LAST STATEMENT here, as in
   every consume() on this board (issue #72): once the pin reaches zero this
   callback touches nothing the sink's owners own.  Each step's own proof is its
   count (cam_step.h), entered and left around the step in a PRIMASK section --
   the step runs outside it. */
static int cam_sink_consume(void *ctx, const struct frame_desc *f)
{
	int rc = 0;

	(void)ctx;
	for (unsigned i = 0; i < cs_nsteps; i++) {
		struct cam_sink_step *s = cs_steps[i];

		if (step_enter(s)) {
			int src = s->ops->consume(s->ctx, f);

			step_leave(s);
			if (src < 0 && rc == 0)
				rc = src;
		}
	}
	camera_frame_put(&cam_sink, f);             /* LAST -- see the rule above  */
	return rc;
}

/* The base detached the sink (stop / overrun / cascade; under the camera
   lock): every armed step is disarmed and told -- a pause, as each subscriber's
   close() used to be. */
static void cam_sink_close(void *ctx)
{
	(void)ctx;
	tx_mutex_get(&cs_lock, TX_WAIT_FOREVER);
	cs_attached = 0;
	for (unsigned i = 0; i < cs_nsteps; i++)
		step_close_locked(cs_steps[i]);
	tx_mutex_put(&cs_lock);
}

static struct frame_sink cam_sink = {
	.name    = "camera",
	.ctx     = NULL,
	.policy  = FRAME_POLICY_DROP,
	.open    = cam_sink_open,
	.consume = cam_sink_consume,
	.close   = cam_sink_close,
};

/* ---- public API ----------------------------------------------------------- */

int camera_sink_init(struct cam_sink_step *const *steps, unsigned n)
{
	if (cs_ready)
		return CAM_ERR_STATE;
	if (steps == NULL || n == 0u || n > CAM_SINK_MAX_STEPS)
		return CAM_ERR_PARAM;
	for (unsigned i = 0; i < n; i++) {
		if (steps[i] == NULL || steps[i]->ops == NULL ||
		    steps[i]->ops->open == NULL || steps[i]->ops->consume == NULL ||
		    steps[i]->ops->close == NULL)
			return CAM_ERR_PARAM;
	}
	if (tx_mutex_create(&cs_lock, "camsink", TX_INHERIT) != TX_SUCCESS)
		return CAM_ERR_STATE;
	for (unsigned i = 0; i < n; i++)
		cs_steps[i] = steps[i];
	cs_nsteps = n;
	cs_ready = 1;
	return 0;
}

int camera_sink_step_enable(struct cam_sink_step *s)
{
	int rc;

	if (!cs_ready || s == NULL)
		return CAM_ERR_STATE;
	/* Subscribe once, OUTSIDE the mutex: camera_subscribe() takes the camera
	   lock and, with the base running, attaches -- which calls cam_sink_open(),
	   which takes the mutex.  This step is not wanted yet, so that attach does
	   not open it; the block below does. */
	if (!cs_subscribed) {
		rc = camera_subscribe(&cam_sink, CAM_FMT_RGB565);
		if (rc != 0)
			return rc;
		cs_subscribed = 1;
	}

	tx_mutex_get(&cs_lock, TX_WAIT_FOREVER);
	if (s->want) {
		tx_mutex_put(&cs_lock);
		return CAM_ERR_BUSY;
	}
	/* [!] Refused while the producer is still inside from a disable whose drain
	   did not finish: the step's open() initialises what the producer reads. */
	if (!step_may_arm(s)) {
		tx_mutex_put(&cs_lock);
		return CAM_ERR_BUSY;
	}
	rc = 0;
	if (cs_attached)
		rc = step_open_locked(s);
	if (rc == 0)
		s->want = 1;
	tx_mutex_put(&cs_lock);
	return rc;
}

void camera_sink_step_disable(struct cam_sink_step *s)
{
	if (!cs_ready || s == NULL)
		return;
	tx_mutex_get(&cs_lock, TX_WAIT_FOREVER);
	s->want = 0;
	step_close_locked(s);
	tx_mutex_put(&cs_lock);
}

int camera_sink_step_busy(struct cam_sink_step *s)
{
	uint32_t primask = __get_PRIMASK();
	int busy;

	__disable_irq();
	busy = (int)s->st.busy;
	__set_PRIMASK(primask);
	return busy;
}

enum cam_drain_step camera_sink_step_drain(struct cam_sink_step *s,
                                           uint32_t ticks)
{
	ULONG start = tx_time_get();

	/* Wall clock, not a count of sleeps (issue #65), and the count decides
	   before the clock does (cam_drain.h). */
	for (;;) {
		enum cam_drain_step step =
			cam_sink_drain_step(camera_sink_step_busy(s),
			                    (tx_time_get() - start) >= (ULONG)ticks);

		if (step != CAM_DRAIN_WAIT)
			return step;
		tx_thread_sleep(1);
	}
}

int camera_sink_others_live(const struct cam_sink_step *self)
{
	int live = 0;

	/* The camera lock first (inside camera_other_subscribers_attached()), and
	   released before cs_lock is taken: the two are never held together here. */
	if (camera_other_subscribers_attached(&cam_sink))
		return 1;
	if (!cs_ready)
		return 0;
	/* [!] UNDER cs_lock, not a bare read of `armed`.  An enable holds cs_lock
	   across the step's open() and arms only at the end of it, so a lock-free
	   read could see "nothing armed" in the middle of an nn start and let the
	   resolution button restart the base underneath it.  Taking the lock waits
	   that enable out.  A step that is enabled while the sink is attached counts
	   even if it is not armed (an open() that refused); one enabled with the
	   base off does not, as an enabled-but-unattached subscriber did not. */
	tx_mutex_get(&cs_lock, TX_WAIT_FOREVER);
	for (unsigned i = 0; i < cs_nsteps; i++) {
		const struct cam_sink_step *s = cs_steps[i];

		if (s != self && (s->st.armed || (s->want && cs_attached))) {
			live = 1;
			break;
		}
	}
	tx_mutex_put(&cs_lock);
	return live;
}
