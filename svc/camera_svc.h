/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc.h
 * @brief   The board-neutral contract one shared `camera` command speaks
 *          (issue #143, epic #141).
 *
 * Three boards each grew their own `camera` command (boards/<board>/cmds/
 * cmd_camera_board.c), and after issue #142 settled what `camera stream`
 * means, what is left between them is mostly the same frame -- the argument
 * grammar, the holders line, the save / send read loop -- written three times.
 * This is the seam that lets ONE command in shell/cmds/ serve all three, in the
 * same shape as svc/nn_svc.h: the declarations here are implemented by a board
 * adapter (boards/<board>/port/camera/camera_svc_<board>.c), and the board's
 * own richer camera API stays below it, unreachable from shell/.
 *
 * WHICH OPERATIONS A BUILD HAS is not decided here.  It comes from the board's
 * camera_svc_config.h (CAMERA_SVC_HAS_*), read by the shared command, which
 * registers a subcommand only under its macro, and by the adapter, which
 * implements only what its board has.  Everything is declared unconditionally:
 * a declaration costs nothing, and a call to one a board does not implement is
 * a link error rather than a silent no-op.
 *
 * [!] THIS HEADER INCLUDES NOTHING FROM A BOARD AND NOT tx_api.h.  Like every
 * other shared header in svc/ it is self-contained, so a host test can compile
 * the shared command against a stub adapter with no board on the include path.
 *
 * [!] NO MUTABLE STORAGE, here or in the shared command above it.  Line
 * buffers, results and frame cursors live on the caller's stack; whatever has
 * to be remembered between calls (the last capture, its generation, a guard
 * held from frame_open() to frame_close()) belongs to the board adapter.  The
 * shared-storage audit (cmake/check_no_mutable_storage.py) checks the shared
 * command per board for exactly that.
 *
 * [!] CLEANUP BELONGS TO THE ADAPTER.  Unlike nn_svc.h there is no claim
 * field: the shared command never releases, detaches or tears anything down on
 * the strength of a result.  A board that started something and could not
 * finish it keeps whatever is left and finishes it itself (or on the next
 * call), and says which of the statuses below describes the situation.
 *
 * Used by shell/cmds/cmd_camera.c and, so far, the f746g-disco adapter
 * (issue #143 commit 2); the wio-lite-ai and Grove adapters follow.
 */
#ifndef CAMERA_SVC_H
#define CAMERA_SVC_H

#include <stddef.h>
#include <stdint.h>

#include "cam_holders.h"   /* CAM_HOLDERS_MAX: the bound on named holders */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what an operation reports ------------------------------------------- */

/**
 * Status of one operation.  Zero is success; every failure is one of these.
 *
 * [!] THEY ARE NOT FOLDED INTO EACH OTHER, because each one tells the operator
 * something different to do next, and a status that has been folded cannot be
 * unfolded later by reading the detail text:
 *
 *  - BUSY is "type it again and it will move".  It is NOT "nothing changed":
 *    a board may have done part of the work (Grove's release can stop the
 *    producer and still have its unlink outstanding) and keeps the rest to
 *    finish itself.
 *  - TIMEOUT is "a wait ran out and the thing waited for may still happen"
 *    (f746 / wio waiting for a requested stream stop).  NOT LOST.
 *  - LOST is only for a board that has decided the state is terminal -- that
 *    nothing short of a reset will settle it.  A board that merely cannot tell
 *    yet answers TIMEOUT or BUSY, not this.
 *  - IN_USE names its holders in @ref camera_op_result::users.
 *
 * These are a camera vocabulary and NOT the nn one (NN_SVC_ERR_* in
 * nn_svc.h).  The numbers are kept apart from nn's and from a backend's so a
 * misrouted one cannot pass for the other, but nothing relies on that: a
 * camera status is only ever compared with these names.
 */
#define CAMERA_SVC_OK              0
#define CAMERA_SVC_ERR_ARG      (-90)  /**< a bad argument (null, out of range)  */
#define CAMERA_SVC_ERR_NOSUP    (-91)  /**< this board does not have it at all  */
#define CAMERA_SVC_ERR_STATE    (-92)  /**< wrong state: not powered, not
                                        *   streaming, already streaming, ...   */
#define CAMERA_SVC_ERR_BUSY     (-93)  /**< another operation holds the camera;
                                        *   retry -- see the note above          */
#define CAMERA_SVC_ERR_IN_USE   (-94)  /**< refused because named holders still
                                        *   need the stream (see `users`)        */
#define CAMERA_SVC_ERR_NO_FRAME (-95)  /**< no valid captured frame to read     */
#define CAMERA_SVC_ERR_HW       (-96)  /**< the sensor or interface refused     */
#define CAMERA_SVC_ERR_TIMEOUT  (-97)  /**< a wait expired; it may still finish */
#define CAMERA_SVC_ERR_LOST     (-98)  /**< terminal; only a reset clears it    */
#define CAMERA_SVC_ERR_CANCELLED (-99) /**< the operator cancelled (the shared
                                        *   command's callback said so); the
                                        *   shared command prints nothing      */

/**
 * Longest board-written explanation carried back with a result.
 *
 * [!] AN EXPLANATION MAY BE SEVERAL LINES ("\r\n" + an indent between them),
 * printed as one failure: Grove's failures name the reason the driver latched
 * and what to do about it on the lines under the first ("camera: probe failed:
 * camera busy" / "        a stream is running ..."), and those lines are what
 * tells an operator to retry rather than reboot.  319 holds Grove's longest
 * (about 290 with every optional line); 159 did not (issue #143 commit 4).
 */
#define CAMERA_SVC_DETAIL_MAX 319

/** Most holder names one IN_USE result (or one state snapshot) carries. */
#define CAMERA_SVC_USERS_MAX  CAM_HOLDERS_MAX

/*
 * [!] A BOARD'S LITERAL WORDING IS CHECKED AGAINST THE BOUND AT BUILD TIME, the
 * same way as nn's (svc/nn_detail.h, issue #122 P15): the copy into
 * camera_op_result::detail truncates, and a truncated sentence does not look
 * truncated.  CAMERA_SVC_DETAIL_CHECK(s) is a compile-time zero that fails the
 * build when the string LITERAL @p s is longer than CAMERA_SVC_DETAIL_MAX;
 * CAMERA_SVC_DETAIL_LIT(s) is @p s itself, checked; CAMERA_SVC_DETAIL_CHECK_FMT
 * checks the format of a printf-shaped call.  `"" s` refuses anything that is
 * not a literal.  Text a conversion expands at run time is not seen and
 * remains the author's arithmetic.
 */
#ifndef __cplusplus
#define CAMERA_SVC_DETAIL_CHECK(s)                                             \
	(0u * sizeof(struct {                                                  \
		_Static_assert(sizeof("" s) <=                                 \
		               (size_t)CAMERA_SVC_DETAIL_MAX + 1u,             \
		               "detail literal is longer than "                \
		               "CAMERA_SVC_DETAIL_MAX");                       \
		char camera_detail_check_;                                     \
	}))
#define CAMERA_SVC_DETAIL_LIT(s)  (CAMERA_SVC_DETAIL_CHECK(s) + ("" s))
#define CAMERA_SVC_DETAIL_FIRST_(fmt, ...) fmt
#define CAMERA_SVC_DETAIL_CHECK_FMT(...) \
	CAMERA_SVC_DETAIL_CHECK(CAMERA_SVC_DETAIL_FIRST_(__VA_ARGS__, 0))
#endif /* !__cplusplus */

/**
 * The result of one operation.  The caller owns it (on its stack) and the
 * adapter fills all of it before returning.
 */
struct camera_op_result {
	int status;                               /**< CAMERA_SVC_OK or _ERR_*  */
	/** Why, in the board's own words, or empty.  A COPY: nothing here points
	 *  into adapter storage that a second console could overwrite. */
	char detail[CAMERA_SVC_DETAIL_MAX + 1];
	/** With CAMERA_SVC_ERR_IN_USE: who still holds the stream.  The names are
	 *  the holders' static strings (struct cam_holder::name), so they outlive
	 *  the call. */
	const char *users[CAMERA_SVC_USERS_MAX];
	unsigned nusers;                          /**< entries valid in users[] */
};

/**
 * The sink a board's report is written to, one line (or part of one) at a
 * time.  The same shape as nn_svc_write_fn: length-bearing, so no formatter
 * crosses the boundary, and failing -- a sink that said no has said no, and a
 * writer that keeps going after a negative return turns one refused line into
 * a run of them.
 *
 * [!] AN ADAPTER MUST NOT STORE @p write, AND MUST NOT CALL IT WITH INTERRUPTS
 * MASKED OR A CAMERA LOCK HELD.  A report is SAMPLED ONCE under the board's
 * lock into the adapter's stack frame, the lock is dropped, and then the
 * lines are written from that one sample -- so lines computed from one
 * snapshot (a frame count and the fps derived from it) cannot disagree, and a
 * slow console never stalls the producer.
 *
 * @return the bytes taken, or negative on failure.
 */
typedef int (*camera_svc_write_fn)(void *ctx, const char *s, size_t len);

/* ---- power and identity --------------------------------------------------- */

/*
 * [!] WHERE AN OPERATION TAKES @p write, THE BOARD SAYS ITS OWN WORDS THROUGH
 * IT -- a progress line before a slow bring-up ("takes ~1s"), and what it did
 * on success -- and the shared command adds none of its own for that case.
 * What went WRONG is never written: it comes back in @p res, because the shared
 * command prints a failure differently (in red) from a report, and a board
 * cannot tell the two apart through one writer.  (Issue #143 commit 2 gave the
 * power, info, report and stream-start calls a writer; commit 1 had drafted
 * them without one, which would have put every board's wording into the
 * shared command.)
 */

/**
 * Power the sensor up (as deep a bring-up as `probe` needs), read its
 * identity, and write what was found.  A board whose bring-up is slow may write
 * one progress line first, before it takes any lock; the identity is written
 * after the bring-up from one sample.  A failure is in @p res and nothing is
 * written for it.
 */
void camera_svc_probe(struct camera_op_result *res,
                      camera_svc_write_fn write, void *ctx);

/** CAMERA_SVC_HAS_POWER: bring the module to its powered, ready state, and
 *  write what was done (as for probe, a progress line may come first). */
void camera_svc_on(struct camera_op_result *res,
                   camera_svc_write_fn write, void *ctx);

/**
 * CAMERA_SVC_HAS_POWER: take the module to the board's DEEPEST power state,
 * and write what was done.
 *
 * The board re-checks the stream and its holders under its own lock
 * immediately before cutting power; with anyone but the operator holding the
 * stream it answers CAMERA_SVC_ERR_IN_USE and changes nothing.  Stopping the
 * operator's own stream first is the shared command's (it prints and waits).
 */
void camera_svc_off(struct camera_op_result *res,
                    camera_svc_write_fn write, void *ctx);

/** CAMERA_SVC_HAS_INFO: write the driver / sensor state.  Each line is written
 *  with no camera lock held.  Touches no sensor bus that a running stream
 *  depends on.  A failure is in @p res. */
void camera_svc_info(struct camera_op_result *res,
                     camera_svc_write_fn write, void *ctx);

/* ---- one-shot capture ----------------------------------------------------- */

/**
 * Capture one frame into the board's capture buffer, replacing the last one.
 * @p test != 0 asks for the sensor's test pattern (CAMERA_SVC_HAS_TEST; a
 * board without one answers CAMERA_SVC_ERR_NOSUP).
 *
 * Whatever warm-up the board pays (frames discarded, or a settle time) is
 * part of the capture and is reported by camera_svc_capture_report().
 *
 * The board writes its own "capturing ..." line through @p write before the
 * capture starts -- after any refusal it can give at once (wio: the OCTOSPI1
 * guard), so a refused capture is not announced -- because what a capture
 * costs is the board's to say (wio: "~1 s of warm-up").
 */
void camera_svc_capture(int test, struct camera_op_result *res,
                        camera_svc_write_fn write, void *ctx);

/** Most leading bytes of a frame a capture report hands back. */
#define CAMERA_SVC_HEAD_MAX 32u

/** How a board's warm-up is counted. */
enum camera_svc_warmup_unit {
	CAMERA_SVC_WARMUP_NONE = 0,   /**< nothing was paid                   */
	CAMERA_SVC_WARMUP_FRAMES,     /**< whole frames read and discarded    */
	CAMERA_SVC_WARMUP_MS,         /**< a settle time, in milliseconds     */
};

/** What a capture report hands back for the shared command to print. */
struct camera_svc_capture_report {
	uint8_t  head[CAMERA_SVC_HEAD_MAX]; /**< the frame's first bytes        */
	uint32_t head_len;                  /**< valid bytes in head[]          */
	uint32_t warmup;                    /**< frames or ms, per warmup_unit  */
	uint8_t  warmup_unit;               /**< enum camera_svc_warmup_unit     */
};

/**
 * Report on the LAST capture: write the board's statistics lines and hand back
 * its leading bytes (the shared command hexdumps them, after the lines) and
 * the warm-up it paid.  Statistics, head and warm-up all come from the same
 * capture generation; if the frame is replaced part way the report fails
 * rather than mixing two frames (a head that cannot be read from that
 * generation comes back with head_len 0).
 *
 * The status is in @p res: CAMERA_SVC_OK or a CAMERA_SVC_ERR_* (NO_FRAME when
 * nothing has been captured), with the board's words in its detail.
 *
 * [!] A LONG WALK ASKS @p cancelled BETWEEN CHUNKS, and when it says yes
 * (non-zero) the board stops, writes NOTHING, and answers
 * CAMERA_SVC_ERR_CANCELLED.  The board holds no shell instance, so it cannot
 * see a Ctrl+C itself; and the console only reads its input while somebody
 * asks, so a walk that never asks runs to the end and prints lines nobody
 * wanted.  @p cancelled gets the same @p ctx as @p write.
 */
void camera_svc_capture_report(struct camera_svc_capture_report *rep,
                               struct camera_op_result *res,
                               camera_svc_write_fn write,
                               int (*cancelled)(void *ctx), void *ctx);

/* ---- reading the last capture out (save / send) -------------------------- */

/**
 * A cursor over the last capture, filled by camera_svc_frame_open().  The
 * caller owns it (on its stack) and passes it back unchanged.
 */
struct camera_svc_frame {
	uint32_t gen;          /**< the capture this cursor reads             */
	uint32_t len;          /**< valid bytes (variable for a JPEG stream)  */
	uint16_t width;
	uint16_t height;
	const char *format;    /**< static name of the pixel format           */
	/** The PC-side converter for this format, as a repository path such as
	 *  "scripts/rgb565_to_png.py", or NULL when the file is already a format a
	 *  PC opens directly (a JPEG stream).  Static storage. */
	const char *convert;
	uintptr_t adapter;     /**< the adapter's own, opaque to the caller   */
};

/**
 * Open the last capture for reading.  CAMERA_SVC_ERR_NO_FRAME when there is
 * none.  On success the board may hold a resource until camera_svc_frame_close()
 * (wio keeps its OCTOSPI1 share guard from open to close); on failure it holds
 * nothing and close must not be called.
 */
void camera_svc_frame_open(struct camera_svc_frame *f,
                           struct camera_op_result *res);

/**
 * Copy @p n bytes at @p off of the frame @p f opened into @p dst.
 *
 * [!] THE VALIDITY AND GENERATION CHECK AND THE COPY ARE ONE STEP, under the
 * same exclusion as every path that starts overwriting that buffer (a capture,
 * a stream start, Grove's `camera raw`).  A check followed by a copy under a
 * separate lock leaves a window in which a capture starts writing and the copy
 * reads half of each frame.  When the frame is no longer the one opened, the
 * read fails (save leaves a partial file, send aborts).  The bytes may
 * already be in @p dst by then (a board may copy first and compare the
 * generation of what it copied); after a failed read they must not be used.
 */
void camera_svc_frame_read(const struct camera_svc_frame *f, uint32_t off,
                           void *dst, uint32_t n, struct camera_op_result *res);

/** Release what camera_svc_frame_open() took.  Exactly once per successful
 *  open, also after a failed read. */
void camera_svc_frame_close(struct camera_svc_frame *f);

/* ---- the stream ----------------------------------------------------------- */

/**
 * Register the operator as a holder of the stream, starting it if nobody was
 * streaming.  @p test != 0 streams the test pattern (CAMERA_SVC_HAS_TEST).
 * @p already is set non-zero when the operator was already a holder and
 * nothing changed.  A board may write a note through @p write before it
 * starts (f746: 30 fps clamped while the LCD scans out), and on success it
 * writes the outcome line itself ("streaming live started" / "... already
 * running"), whose wording each board's operators already read; the shared
 * command prints only a failure.
 */
void camera_svc_stream_start(int test, struct camera_op_result *res,
                             int *already,
                             camera_svc_write_fn write, void *ctx);

/** What a stop request did. */
enum camera_svc_stop {
	CAMERA_SVC_STOP_NONE = 0,   /**< nothing changed (see the status)            */
	CAMERA_SVC_STOP_DONE,       /**< the stream is confirmed stopped            */
	CAMERA_SVC_STOP_REQUESTED,  /**< a stop was requested and finishes later;
	                             *   wait on camera_svc_stream_ended(*gen)      */
};

/**
 * Remove the operator's hold.  With other holders still present the answer is
 * CAMERA_SVC_ERR_IN_USE with their names, and nothing changes.  Otherwise the
 * stream is stopped (CAMERA_SVC_STOP_DONE) or a stop is requested
 * (CAMERA_SVC_STOP_REQUESTED, f746 stops asynchronously) and @p gen is set to
 * THAT stream's generation, so the caller waits for that stream to end and
 * cannot mistake a later start for it.
 *
 * @return enum camera_svc_stop
 */
int camera_svc_stream_stop(struct camera_op_result *res, uint32_t *gen);

/** Non-zero once the stream of generation @p gen has ended.  Waiting (and the
 *  operator cancelling the wait) belongs to the caller; a cancelled wait leaves
 *  the stop request in place. */
int camera_svc_stream_ended(uint32_t gen);

/** A one-sample view of the stream, for the shared command's own lines. */
struct camera_svc_state {
	uint8_t  streaming;        /**< producer running                          */
	uint8_t  transition;       /**< a start or stop is in flight (Grove)      */
	const char *stop_reason;   /**< static text why it last stopped, or NULL  */
	const char *users[CAMERA_SVC_USERS_MAX];  /**< current holders           */
	unsigned nusers;
	uint32_t lost;             /**< holders dropped when the stream ended     */
	/** No stream has run since boot: there is nothing to say about one, and
	 *  the shared command prints that alone ("state: never started") -- no
	 *  holders line, and the board writes no lines of its own. */
	uint8_t  never_started;
};

/** Fill @p st from one sample taken under the board's lock.  When the board
 *  cannot take that sample (driver not up), @p res says why and @p st still
 *  holds the board's best answer to "is it streaming" (zero otherwise), so a
 *  caller that only needs that -- `camera off` -- may go on. */
void camera_svc_stream_state(struct camera_svc_state *st,
                             struct camera_op_result *res);

/** The shared command's own `camera stream stats` lines (state, holders),
 *  handed the board's sample.  Called with no camera lock held. */
typedef void (*camera_svc_state_fn)(void *ctx, const struct camera_svc_state *st);

/**
 * `camera stream stats`: take ONE sample, hand its state to @p head (the
 * shared command prints the state and holders lines from it), then write the
 * board's own lines from THE SAME sample.
 *
 * [!] ONE SAMPLE FOR THE STATE AND THE NUMBERS.  Two samples -- the state from
 * camera_svc_stream_state() and the numbers from a second one -- let another
 * console's `camera stream start` land in between, and "state: stopped" would
 * sit above the new stream's elapsed time and fps.  The holders may be a
 * separate look (they always were).  camera_svc_stream_state() stays for a
 * caller that needs the state alone (`camera off`).
 *
 * A failure is in @p res, and then neither @p head nor @p write is called.
 */
void camera_svc_stream_report(struct camera_op_result *res,
                              camera_svc_write_fn write,
                              camera_svc_state_fn head, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_SVC_H */
