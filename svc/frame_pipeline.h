/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    frame_pipeline.h
 * @brief   Camera frame ring + sink dispatch (svc/ layer, owhinata/stm32f746g-disco#47 design /
 * owhinata/stm32f746g-disco#46 implementation; multi-sink cascade live since owhinata/stm32f746g-disco#100/#101).
 *
 * One producer (DCMI capture) publishes frames into an N-slot SDRAM ring; many
 * sinks consume them.  The base capture attaches an internal stats sink plus up
 * to three external subscribers (GUIX preview / nncam / MJPEG) that attach and
 * detach at runtime -- the "subscriber cascade" (Epic owhinata/stm32f746g-disco#99).  This is the @ref frame_desc
 * data contract (frame.h) plus the ownership/back-pressure mechanics, applied to frame distribution the same
 * way @ref fs_device (owhinata/stm32f746g-disco#34) abstracts media and @ref ym_source (owhinata/stm32f746g-disco#50)
 * abstracts a byte source.
 *
 * Layering (owhinata/stm32f746g-disco#43): this core is **freestanding** -- it depends only on
 * <stdint.h>/<stddef.h> and an injected mutual-exclusion vtable (@ref
 * frame_os).  It calls NO HAL and NO ThreadX, so it sits in svc/ next to
 * ymodem.c/fmt.c and is host-unit-testable (ring/refcount/policy logic with a
 * no-op lock and mock sinks).  All ThreadX -- the producer/sink threads, the
 * ISR -> thread notification, the TX_MUTEX behind @ref frame_os -- lives in the
 * glue (port/camera and each sink); the core is always entered from thread
 * context under the injected lock (the DCMI ISR only posts cam_done, never
 * touches the ring -- the existing discipline).  Unlike timebase.c (which
 * includes HAL), this header includes none.
 *
 * Two access styles coexist:
 *   - push sinks (streaming: LTDC / Ethernet / VCP preview) register via
 *     frame_pipeline_attach() and get consume() called on each publish;
 *   - pull access (snapshot: save / send / stats) reads the latest published
 *     slot via frame_pipeline_read_latest() (the generalised camera_frame_read),
 *     or pins it with frame_pipeline_pin_latest() to copy a whole frame out of the
 *     lock (camera save/send, owhinata/stm32f746g-disco#102).
 * A single on-demand `camera capture` is just the degenerate case: an N=1 ring,
 * no push sinks, one publish, pulled by read_latest -- so the existing
 * camera capture/frame_read/save/send keep their semantics unchanged.
 *
 * See docs/{ja,en}/architecture/frame-pipeline.md for the architecture, the
 * concurrency/lifetime contract (publish lock discipline, refcount pin/put,
 * LATEST pending transfer, detach quiesce) and the HW rationale.
 */
#ifndef FRAME_PIPELINE_H
#define FRAME_PIPELINE_H

#include <stdint.h>

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return values.  A closed set, and that is the point (issue #72): attach used
 * to hand back whatever a sink's own open() returned, so no value the core
 * picked for itself could be told apart from one a board invented.  Every
 * open() rejection is now reported as FRAME_PIPELINE_ERR_OPEN, and each reason
 * the CORE refuses has a name a caller can log.
 */
#define FRAME_PIPELINE_OK              0
#define FRAME_PIPELINE_ERR_PARAM      (-1) /* NULL pipeline or sink            */
#define FRAME_PIPELINE_ERR_FULL       (-2) /* no room for another sink         */
#define FRAME_PIPELINE_ERR_OPEN       (-3) /* the sink's own open() rejected   */
#define FRAME_PIPELINE_ERR_ATTACHED   (-4) /* already linked into this pipeline */
#define FRAME_PIPELINE_ERR_UNDRAINED  (-5) /* still holds pins -- see attach()  */

/**
 * Injected mutual exclusion.  The glue wires this to a ThreadX TX_MUTEX
 * (TX_INHERIT); a host unit test wires it to a no-op.  The core never blocks on
 * anything else: there is no wait primitive in svc/, so all waiting/notification
 * (producer DMA completion, sink wakeups, detach drain) is owned by the glue.
 * Held only for short bookkeeping critical sections -- never across consume().
 */
struct frame_os {
	void *ctx;
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
};

/**
 * Per-sink back-pressure policy.  Push sinks are non-blocking by construction --
 * the DCMI cannot be stalled mid-frame, so a sink that falls behind drops
 * frames.  A must-complete consumer (a save that must not lose data) is NOT a
 * push policy: it is expressed as pull access in snapshot mode (read_latest),
 * where the producer naturally blocks on a single frame.  Hence there is no
 * BLOCK policy.
 */
enum frame_policy {
	FRAME_POLICY_DROP = 0, /**< if the sink is busy, drop this frame (live default) */
	FRAME_POLICY_LATEST,   /**< coalesce: while busy keep only the newest as pending */
};

/**
 * A push sink: a thin vtable in the @ref fs_device / @ref ym_source idiom.  The
 * producer never knows the concrete sink.  Caller-allocated and caller-owned --
 * see the detach contract below before freeing one.
 *
 * consume() is called by frame_pipeline_publish() **outside** the pipeline lock,
 * with the slot already pre-pinned once on this sink's behalf.  The sink must
 * release that pin with exactly one frame_pipeline_put() -- a synchronous sink
 * does its work and puts before returning; an asynchronous sink hands the
 * descriptor to its own thread/queue (the pre-pin keeps the slot alive) and puts
 * from that thread when done.  This holds even on an error return.  Because
 * put() is called with the pipeline lock released, a non-recursive mutex never
 * self-deadlocks.
 */
struct frame_sink {
	const char *name;
	void       *ctx;
	uint8_t     policy;  /**< enum frame_policy */
	/** Negotiate format/geometry at attach; return <0 to reject (unsupported). */
	int  (*open)(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h);
	/** Consume one frame (read-only).  <0 = sink error (counted; producer keeps
	 *  running).  Must arrange exactly one frame_pipeline_put(p, s, f) of the
	 *  pre-pinned slot (synchronously, or from the sink's own thread). */
	int  (*consume)(void *ctx, const struct frame_desc *f);
	void (*close)(void *ctx);

	/* Statistics -- updated by the core under the lock; read-only to the owner. */
	uint32_t delivered;
	uint32_t dropped;
	uint32_t errors;

	/* Core-internal; the owner must not touch these.
	   [!] But it must ZERO them before the first attach (issue #72): attach now
	   READS `_pins` to decide whether the sink was ever drained, so a sink that
	   came up with garbage in it would be refused forever.  Every sink in this
	   project is static, which zeroes them for free -- this says so out loud
	   because "the owner must not touch these" used to mean the core never
	   looked at them before it owned them. */
	struct frame_sink       *_next;
	const struct frame_desc *_pending; /**< LATEST coalesce slot (pinned)         */
	int                      _busy;     /**< a consume() for this sink is in flight */
	int                      _pins;     /**< slots this sink currently holds (detach) */
};

/** Compile-time caps for the inline bookkeeping (owhinata/stm32f746g-disco#46 implementation).
 *  A producer statically allocates one struct frame_pipeline; nslots <= MAX. */
#ifndef FRAME_PIPELINE_MAX_SLOTS
#define FRAME_PIPELINE_MAX_SLOTS 8u
#endif
#ifndef FRAME_PIPELINE_MAX_SINKS
#define FRAME_PIPELINE_MAX_SINKS 4u
#endif

/** One ring slot: a descriptor over caller SDRAM plus its bookkeeping. */
struct frame_slot {
	struct frame_desc desc;     /**< .data fixed at init; geometry/gen at publish */
	int               state;    /**< 0 = free, 1 = filling (acquired, pre-publish) */
	int               refcount; /**< sink pins (0 = reusable when free)            */
};

/** Producer/pipeline counters (read via frame_pipeline_stats). */
struct frame_stats {
	uint32_t captured;  /**< frames the producer acquired/filled  */
	uint32_t published; /**< frames published (== last gen)        */
	uint32_t overruns;  /**< acquire() calls that found no free slot */
};

/**
 * Ring/dispatch engine.  Statically allocated by the producer (port/camera owns
 * one instance, the way it owns cam_frame[] today); pass its address to every
 * call.  Fields are implementation-internal -- callers treat it as opaque and
 * only keep the pointer.
 */
struct frame_pipeline {
	const struct frame_os *os;                          /* injected mutex          */
	struct frame_sink     *sinks;                       /* attached-sink list head */
	struct frame_slot      slots[FRAME_PIPELINE_MAX_SLOTS];
	uint32_t               nslots;
	uint32_t               slot_size;                   /* bytes per slot          */
	int                    latest;                      /* last published idx, -1  */
	uint32_t               gen;                          /* publish counter         */
	enum frame_format      fmt;                          /* current format (open)   */
	uint16_t               width, height;               /* current geometry (open) */
	struct frame_stats     stats;
};

/* ---- producer side ------------------------------------------------------- */

/**
 * Bind the engine to @p nslots ring slots carved from caller-owned SDRAM
 * (slot_mem is nslots * slot_size bytes, .sdram, non-cacheable).  The producer
 * owns the slot memory; the core owns only the bookkeeping (gen / refcount /
 * registry).  @p os supplies mutual exclusion.  Returns 0 or <0.
 */
int frame_pipeline_init(struct frame_pipeline *p, const struct frame_os *os,
                        void *slot_mem, uint32_t slot_size, uint32_t nslots);

/**
 * Claim a free slot to fill (refcount==0 and not the latest published one, so a
 * pull reader's current frame is never recycled under it).  Returns a writable
 * descriptor whose @ref frame_desc.data points into the slot, or NULL when every
 * slot is pinned (the producer then counts an overrun and drops the frame).
 */
struct frame_desc *frame_pipeline_acquire(struct frame_pipeline *p);

/**
 * Publish a filled slot: stamp the generation and the geometry, then fan out to
 * the attached sinks.  Lock discipline (the core of the design): the lock is
 * held only to stamp gen, snapshot the delivery set and pre-pin one reference
 * per delivering sink; consume() is then called with the lock released.  A busy
 * DROP sink is skipped (dropped++); a busy LATEST sink keeps @p f as its pending
 * (old pending, if any, is put first; the new one is pre-pinned).
 */
void frame_pipeline_publish(struct frame_pipeline *p, struct frame_desc *f,
                            uint32_t bytes, enum frame_format fmt,
                            uint16_t w, uint16_t h, uint16_t stride);

/* ---- sink registry ------------------------------------------------------- */

/**
 * Set the current frame format/geometry the producer will publish.  attach()
 * passes these to a sink's open(); a producer calls this before attaching sinks
 * (and, for owhinata/stm32f746g-disco#45, on a format change -- re-opening sinks is the caller's job).
 */
void frame_pipeline_set_format(struct frame_pipeline *p, enum frame_format fmt,
                               uint16_t w, uint16_t h);

/**
 * Register a push sink.  attach() calls s->open() with the pipeline's current
 * format/geometry to negotiate acceptance (<0 from open rejects the attach).
 * The current format is owned by the producer: fixed QVGA RGB565 today (set from
 * its init config); owhinata/stm32f746g-disco#45 makes it variable, re-opening sinks (close()+open()) on a
 * format change.
 *
 * Returns FRAME_PIPELINE_OK, or one of:
 *   ERR_PARAM       NULL argument;
 *   ERR_ATTACHED    @p s is already linked into @p p;
 *   ERR_UNDRAINED   @p s still holds pins from an earlier session;
 *   ERR_FULL        no room for another sink;
 *   ERR_OPEN        the sink's own open() rejected the format/geometry.
 *
 * [!] A REFUSAL BY THE CORE HAS NO SIDE EFFECTS: all four are decided before
 * open() is called, so the sink is not linked, its bookkeeping is untouched, and
 * its owner has not been told anything.  That ordering is not tidiness -- open()
 * is where a board resets the state its consume() reads, and doing that to a
 * sink whose owner never drained is exactly the hazard below.
 *
 * [!] ERR_UNDRAINED IS THE POINT OF ISSUE #72.  Attach used to zero `_pins`,
 * `_busy` and `_pending` unconditionally.  For a sink whose owner skipped the
 * drain that erased the only evidence: the count the owner was told to watch
 * went to zero while the SLOT refcounts the sink still held stayed up, so the
 * ring was permanently one slot short and nothing said so.  A sink that is
 * refused here has a callback that may still be inside it; the fix is to drain
 * it (see detach), not to attach again.  A pin that comes back late clears the
 * refusal by itself -- the next attach succeeds.
 *
 * [!] AND ERR_ATTACHED IS NOT PEDANTRY.  Re-linking a linked sink writes
 * `s->_next = p->sinks` while `s` is in that list; if it is the head, `s->_next`
 * becomes `s` and every later traversal of the registry never terminates.
 *
 * [!] SERIALISATION IS THE CALLER'S, and these are the exact conditions the
 * refusals above rest on (issue #72; the core will take them over):
 *   - attach() calls on one pipeline are mutually serialised;
 *   - a sink is not detached while its own attach() is in progress;
 *   - init() does not overlap any other operation on the pipeline.
 * Only attach raises the sink count, so serialising attach against attach is
 * what bounds capacity; a detach of a DIFFERENT sink during an open() only
 * lowers it.  What still needs an owner is the same-sink rule, because attach
 * releases the lock across open() and detach releases it across close().
 */
int frame_pipeline_attach(struct frame_pipeline *p, struct frame_sink *s);

/**
 * A short name for one of the FRAME_PIPELINE_* codes, for the boards' logs.
 *
 * It lives here so the three ports cannot drift apart on what a refusal is
 * called -- the reason a refusal is worth logging at all is that the failure it
 * reports is otherwise invisible, and three private tables would eventually
 * disagree about it.  Never NULL; an unknown code reads as "unknown".
 */
const char *frame_pipeline_strerror(int rc);

/**
 * Unlink a sink so that no LATER publish selects it, and report how many pins it
 * still holds in flight.  The core does NOT wait: before freeing @p s / its ctx,
 * the caller (which owns the sink's thread and queue) must drain that thread --
 * let the running consume() finish and every pin be put() -- until the in-flight
 * count reaches zero.  Returns the in-flight pin count at detach time.
 *
 * [!] THE COUNT EXCLUDES AN UNDELIVERED LATEST PENDING PIN, which this drops on
 * the way out: nothing will ever deliver that frame, so the sink can never put()
 * it and a caller draining towards zero would wait forever.  (This header said
 * "including" for as long as it has existed, and the code has never done that.)
 *
 * [!] "No later PUBLISH", not "no further consume()".  TWO windows already
 * scheduled a call before the unlink and still make it:
 *   - publish() copies the sinks it will deliver to into a local array, drops
 *     the lock, and only then calls consume() on each -- any policy, not just
 *     LATEST;
 *   - put() takes a LATEST sink's pending frame into a local, drops the lock,
 *     and calls consume() with it.
 * Both hold their pin across the call, so the count below never reports the sink
 * idle while one is happening; but a caller reading this as "consume() can no
 * longer run" would be wrong.
 *
 * [!] AND THE RETURNED NUMBER IS NOT THE ONE THAT MEANS ANYTHING.  A non-zero
 * count here is NORMAL -- the sink's thread may legitimately be mid-delivery.
 * The count that says whether the contract was honoured is the one taken AFTER
 * the caller has drained, which is what frame_pipeline_sink_pins() is for.
 * Every owner that ignored this built a private counter instead (issue #72).
 */
int frame_pipeline_detach(struct frame_pipeline *p, struct frame_sink *s);

/**
 * How many pins sink @p s holds right now.  Attached or detached alike: @p s is
 * the caller's storage and its bookkeeping survives the unlink.
 *
 * THIS IS THE DRAIN CHECK (issue #72).  Call it after draining the sink's
 * thread; a non-zero answer means the drain did not finish and the sink's ctx --
 * whatever consume() reads -- must NOT be torn down or reused yet.
 *
 * [!] FOR A DETACHED SINK, ZERO IS A DECISION AND NOT MERELY A SNAPSHOT, which
 * is what makes an after-the-drain check worth taking.  The reason is not the
 * obvious one: it is NOT that nothing can add a pin, because
 * frame_pipeline_get() has no attachment test and an in-flight consumer may
 * legitimately call it.  It is that such a consumer still owns the delivery pin
 * it was called with, and can only reach get() before releasing it -- so the
 * count cannot fall to zero and then rise again.  Zero means the owner is out.
 *
 * A non-zero answer IS only a snapshot: the sink may be about to put().  That
 * asymmetry is the right way round for a check that must not pass early.
 *
 * There is deliberately no busy accessor to go with this.  publish() sets a
 * sink's busy state and takes its pin inside one locked section, so a busy sink
 * always holds at least one pin; a second way to ask would add nothing except
 * the chance of reading the two at different instants.
 */
int frame_pipeline_sink_pins(struct frame_pipeline *p,
                             const struct frame_sink *s);

/**
 * How many sinks are attached right now.
 *
 * For producers that treat an attached sink as an OWNERSHIP RESERVATION: a sink
 * outlives the stream it was attached for (its owner may only unlink it once the
 * producer is confirmed idle), so "the producer has stopped" does not mean "this
 * sink is free".  A producer that starts a fresh stream while somebody else's
 * sink is still linked would deliver frames to a sink whose owner has finished
 * with it -- and that owner is entitled to tear down what the sink points at.
 * Asking the registry is what makes the rule checkable; reading a producer's own
 * "am I streaming" flag is the check that misses (issue #63).
 *
 * Takes the lock internally, so the answer is a snapshot.  It is a decision
 * rather than a snapshot only for a caller that holds whatever excludes attaches
 * -- for the Grove camera, its API mutex, which every attach now runs under.  In
 * the other direction there is no hazard: a concurrent detach can only make a
 * non-zero answer stale, which costs the caller a refusal it did not have to
 * make.
 */
int frame_pipeline_sink_count(struct frame_pipeline *p);

/* ---- slot reference counting (async sinks) ------------------------------- */

/**
 * Pin / release a slot on behalf of sink @p s.  publish() pre-pins once per
 * delivering sink; the sink balances that with exactly one put() of the same
 * (s, f).  put() also clears that sink's busy state and, for a LATEST sink with a
 * pending frame, transfers the pending pin into the next delivery (see the
 * design doc).  get() takes an ADDITIONAL pin when sink @p s re-queues a frame;
 * such pins also count toward the sink's in-flight total at detach.  The sink
 * argument is required because several sinks share one descriptor, so the slot
 * alone cannot identify whose pin/busy/pending this is.  Both take the lock
 * briefly internally, so they must be called with no pipeline lock held (from
 * consume()/sink-thread context, never nested) -- this keeps a non-recursive
 * mutex deadlock-free.  A slot becomes reusable by the producer when its refcount
 * returns to 0.
 */
void frame_pipeline_get(struct frame_pipeline *p, struct frame_sink *s,
                        const struct frame_desc *f);
void frame_pipeline_put(struct frame_pipeline *p, struct frame_sink *s,
                        const struct frame_desc *f);

/* ---- pull access (snapshot) ---------------------------------------------- */

/**
 * Copy @p len bytes at byte offset @p off out of the latest published frame into
 * @p dst (any alignment), under the lock, and return its generation in @p gen
 * (may be NULL).  The generalised camera_frame_read: a single call cannot tear
 * (the slot is not recycled mid-copy); a multi-call reader (row-by-row save)
 * compares @p gen across calls to detect a frame replaced between reads.  Fails
 * <0 when no frame has been published yet.
 */
int frame_pipeline_read_latest(struct frame_pipeline *p, uint32_t off,
                               void *dst, uint32_t len, uint32_t *gen);

/**
 * Pin the latest published slot (refcount++) and return its descriptor, or NULL
 * when no frame has been published yet.  Unlike frame_pipeline_read_latest(), the
 * whole-frame copy happens OUTSIDE the lock: the returned @ref frame_desc (its
 * data/bytes/gen) stays valid until the caller releases the pin with exactly one
 * frame_pipeline_put(p, NULL, desc) -- while pinned the slot is never re-acquired
 * (refcount != 0) even after a newer publish moves `latest` off it, so the copy is
 * tear-free without holding the pipeline lock across it.  Use for a one-shot
 * snapshot of a live streamed frame (camera save/send) without stalling the
 * producer's publish/DMA-repoint (owhinata/stm32f746g-disco#102).
 */
const struct frame_desc *frame_pipeline_pin_latest(struct frame_pipeline *p);

/* ---- statistics (struct frame_stats defined above frame_pipeline) -------- */

void frame_pipeline_stats(struct frame_pipeline *p, struct frame_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_PIPELINE_H */
