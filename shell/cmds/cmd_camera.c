/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera.c
 * @brief   The one `camera` command, shared by every board with a camera
 *          (issue #143, epic #141).
 *
 *   camera probe               power up and read the sensor's identity
 *   camera on | off            module power                  (HAS_POWER)
 *   camera info                driver / sensor state         (HAS_INFO)
 *   camera capture [test]      snapshot one frame + its statistics
 *   camera save [<medium>] <p> write the last capture to a file (HAS_SAVE)
 *   camera send [name]         the frame to the PC over YMODEM (HAS_SEND)
 *   camera stream start [test] | stop | stats
 *   ...                        the board's own subcommands, appended
 *                              (CAMERA_SVC_BOARD_SUBCMDS)
 *
 * The same layering as `nn` (shell/cmds/cmd_nn.c): this file owns the frame --
 * the grammar, the usage, what a refusal looks like, the holders line, the
 * save / send loops -- and speaks only svc/camera/camera_svc.h.  Each board puts its
 * camera behind that contract in boards/<board>/port/camera/camera_svc_<b>.c,
 * and the two things only the shell layer can do (send over YMODEM, resolve a
 * FileX medium) come from the board's cmds/ through shell/cmds/
 * camera_cmd_board.h.
 *
 * [!] THIS FILE OWNS NO STORAGE.  Line buffers, results and frame cursors are
 * on the caller's stack; whatever has to outlive a call is the adapter's.  A
 * static here would be memory no board placed and that two consoles share.
 *
 * [!] NO BOARD IN HERE: no `#ifdef <BOARD>`, no board header, no peripheral.
 * What a board HAS comes from its camera_svc_config.h as properties
 * (CAMERA_SVC_HAS_*), and a subcommand a board does not have is not
 * registered at all, so `help camera` describes this board and not the family.
 *
 * [!] NOTHING IS RELEASED HERE ON THE STRENGTH OF A RESULT.  Cleanup is the
 * adapter's (svc/camera/camera_svc.h); this file prints what happened and returns.
 */
#include "cli.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "camera_svc.h"
#include "camera_svc_config.h"   /* the board says what it has */
#include "camera_cmd_board.h"

/*
 * [!] EVERY CAPABILITY IS DECLARED BY THE BOARD, to 0 or 1.  Unlike nn_svc's
 * "undefined means absent", a missing one here is a build error: the
 * camera_svc_config.h files say so of themselves, and a macro that silently
 * defaulted would let a board lose `camera save` by a typo.
 */
#if !defined(CAMERA_SVC_HAS_POWER) || !defined(CAMERA_SVC_HAS_INFO) ||     \
    !defined(CAMERA_SVC_HAS_TEST) || !defined(CAMERA_SVC_HAS_SAVE) ||       \
    !defined(CAMERA_SVC_HAS_SEND) || !defined(CAMERA_SVC_BOARD_SUBCMDS) || \
    !defined(CAMERA_SVC_HELP)
#error "camera_svc_config.h must define every CAMERA_SVC_HAS_*, CAMERA_SVC_HELP and CAMERA_SVC_BOARD_SUBCMDS"
#endif
#if CAMERA_SVC_HAS_POWER && !defined(CAMERA_SVC_OFF_STOPS_STREAM)
#error "CAMERA_SVC_HAS_POWER needs CAMERA_SVC_OFF_STOPS_STREAM: say whether `camera off` stops a running stream first"
#endif
#if CAMERA_SVC_HAS_SAVE && !defined(CAMERA_SVC_SAVE_MEDIA)
#error "CAMERA_SVC_HAS_SAVE needs CAMERA_SVC_SAVE_MEDIA: say which media `camera save` takes"
#endif
#ifndef CAMERA_SVC_BOARD_DECLS
#define CAMERA_SVC_BOARD_DECLS
#endif

#if CAMERA_SVC_HAS_SAVE
#include "fs_cmd_core.h"         /* struct fs_device, fs_core_mount, FileX */
#endif
#if CAMERA_SVC_HAS_SEND
#include "ymodem.h"              /* struct ym_source */
#endif

/* The board's own subcommand handlers (see CAMERA_SVC_BOARD_SUBCMDS). */
CAMERA_SVC_BOARD_DECLS

/* ---- saying what happened ------------------------------------------------ */

/* The words for a status a board gave no sentence for. */
static const char *cam_status_name(int status)
{
	switch (status) {
	case CAMERA_SVC_ERR_ARG:      return "bad argument";
	case CAMERA_SVC_ERR_NOSUP:    return "not supported on this board";
	case CAMERA_SVC_ERR_STATE:    return "not in a state to do that";
	case CAMERA_SVC_ERR_BUSY:     return "busy -- try again";
	case CAMERA_SVC_ERR_IN_USE:   return "still in use";
	case CAMERA_SVC_ERR_NO_FRAME: return "no captured frame";
	case CAMERA_SVC_ERR_HW:       return "sensor or interface error";
	case CAMERA_SVC_ERR_TIMEOUT:  return "timed out (it may still finish)";
	case CAMERA_SVC_ERR_LOST:     return "lost -- only a reset clears it";
	default:                      return "unknown error";
	}
}

/* "still in use: nn[, ...]" for a refused stop or power-off (issue #142 D8):
   the operator's request changed nothing, and these are why. */
static void cam_print_in_use(struct cli_instance *sh,
                             const struct camera_op_result *res)
{
	char list[64];
	size_t len = 0;
	unsigned n = res->nusers;

	if (n > CAMERA_SVC_USERS_MAX)
		n = CAMERA_SVC_USERS_MAX;
	list[0] = '\0';
	for (unsigned i = 0; i < n; i++) {
		const char *name = res->users[i] ? res->users[i] : "?";
		size_t sep = i ? 2u : 0u;
		size_t w = strlen(name);

		if (len + sep + w >= sizeof list)
			break;
		if (sep)
			memcpy(list + len, ", ", sep);
		memcpy(list + len + sep, name, w);
		len += sep + w;
		list[len] = '\0';
	}
	cli_error(sh, "camera: still in use: %s\r\n", list);
}

/* A failed result, in the board's words when it gave any.  @p suffix follows
   the sentence ("" or " (file left partial)"). */
static void cam_print_fail(struct cli_instance *sh,
                           const struct camera_op_result *res,
                           const char *suffix)
{
	if (res->status == CAMERA_SVC_ERR_IN_USE) {
		cam_print_in_use(sh, res);
		return;
	}
	cli_error(sh, "camera: %s%s\r\n",
	          res->detail[0] ? res->detail : cam_status_name(res->status),
	          suffix);
}

/*
 * The writer a board's report goes through (svc/camera/camera_svc.h).  cli_write()
 * takes the whole piece under ONE output bracket, as one cli_print() did when
 * the board printed for itself: a line split across two calls can have a
 * background job's output land in the middle of it.  No copy, so no chunk
 * buffer on the console's stack either.
 */
static int cam_write(void *ctx, const char *s, size_t len)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (sh == NULL || (s == NULL && len != 0u))
		return -1;
	if (len == 0u)
		return 0;
	return (cli_write(sh, s, len) < 0) ? -1 : (int)len;
}

/* The cancellation question a board's long walk asks (svc/camera/camera_svc.h).  It
   is also what lets the console read the Ctrl+C at all: input is polled only
   while somebody asks. */
static int cam_cancelled(void *ctx)
{
	return cli_cancel_requested((struct cli_instance *)ctx) ? 1 : 0;
}

/* ---- power and identity --------------------------------------------------- */

static int cmd_camera_probe(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;

	(void)argc;
	(void)argv;
	camera_svc_probe(&res, cam_write, sh);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	return 0;
}

#if CAMERA_SVC_HAS_POWER
static int cmd_camera_on(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;

	(void)argc;
	(void)argv;
	camera_svc_on(&res, cam_write, sh);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	return 0;
}

/* How long `camera off` waits for the stream it stopped, in console ticks.
   A board whose stop already waits (wio, up to ~1 s) is not bounded by this:
   `camera off` can take that long before the wait here even starts. */
#define CAM_OFF_WAIT_TICKS 200u

#if CAMERA_SVC_OFF_STOPS_STREAM
/* The stop was asked for, the stream has not ended yet, and it may still: the
   power is left on and the request stays. */
static void cam_off_timed_out(struct cli_instance *sh)
{
	cli_error(sh, "camera: the stream has not stopped yet -- %s; power left "
	          "on, retry 'camera off'\r\n",
	          cam_status_name(CAMERA_SVC_ERR_TIMEOUT));
}
#endif

static int cmd_camera_off(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;

	(void)argc;
	(void)argv;

#if CAMERA_SVC_OFF_STOPS_STREAM
	/*
	 * Power cannot be cut mid-stream (it wrecks the interface), so the
	 * operator's own stream is stopped first and waited for: the producer
	 * tears it down (every subscriber gets a close()), then the power goes.
	 * The board re-checks the stream and its holders under its own lock
	 * before it cuts anything (camera_svc_off()).
	 *
	 * [!] NOTHING IS POWERED DOWN UNLESS THE STOP WAS ACCEPTED AND ITS STREAM
	 * IS CONFIRMED OVER.  A refusal (another holder: IN_USE), any other
	 * failure of the stop, a wait that runs out (TIMEOUT -- the stream may
	 * still stop later) and a Ctrl+C during the wait all leave the power on;
	 * the stop request, once made, stays made.  The wait is on THE STREAM THE
	 * STOP NAMED (its generation), never on "is anything streaming": a start
	 * from another console after it is not its end, and neither is one that
	 * withdrew the stop and kept the stream.
	 */
	struct camera_svc_state st;

	/* Only "is it streaming" is needed here; a state the board could not
	   sample still answers that, and the power-off below reports. */
	camera_svc_stream_state(&st, &res);
	if (st.streaming) {
		uint32_t gen = CAMERA_SVC_GEN_NONE;
		unsigned i;
		int what;

		/* Refused first, announced after: a stop another holder refuses
		   changed nothing, so nothing is "stopping". */
		what = camera_svc_stream_stop(&res, &gen);
		if (res.status == CAMERA_SVC_ERR_TIMEOUT) {
			cam_off_timed_out(sh);
			return 1;
		}
		if (res.status != CAMERA_SVC_OK) {
			cam_print_fail(sh, &res, "");
			return 1;
		}
		cli_print(sh, "camera: stopping stream before power off\r\n");
		if (what == CAMERA_SVC_STOP_REQUESTED) {
			for (i = 0; i < CAM_OFF_WAIT_TICKS &&
			            !camera_svc_stream_ended(gen); i++) {
				if (cli_sleep(sh, 1) != 0)
					return 1;     /* Ctrl+C: the core prints ^C */
			}
			if (!camera_svc_stream_ended(gen)) {
				cam_off_timed_out(sh);
				return 1;
			}
		}
		/* A Ctrl+C typed while a board's stop waited inside the adapter
		   (wio: up to ~1 s, which these 200 ticks do not bound) is still a
		   cancel: the stop stands, the power stays on. */
		if (cli_cancel_requested(sh))
			return 1;
	}
#endif /* CAMERA_SVC_OFF_STOPS_STREAM: otherwise the board refuses a
          running stream itself (CAMERA_SVC_ERR_BUSY) */

	camera_svc_off(&res, cam_write, sh);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	return 0;
}
#endif /* CAMERA_SVC_HAS_POWER */

#if CAMERA_SVC_HAS_INFO
static int cmd_camera_info(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;

	(void)argc;
	(void)argv;
	camera_svc_info(&res, cam_write, sh);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	return 0;
}
#endif

/* ---- the `test` word ----------------------------------------------------- */

/*
 * Parse an optional `test`.  0 = no test, 1 = test, -1 = refused (printed).
 * A board without a test pattern still recognises the word, so the refusal says
 * why instead of "unknown option" -- the same shape as `nn stream start`.
 */
static int cam_parse_test(struct cli_instance *sh, const char *arg,
                          const char *unknown_fmt, const char *what)
{
	if (strcmp(arg, "test") != 0) {
		cli_error(sh, unknown_fmt, arg);
		return -1;
	}
#if CAMERA_SVC_HAS_TEST
	(void)what;
	return 1;
#else
	cli_error(sh, "camera: this board has no test pattern to %s\r\n", what);
	return -1;
#endif
}

#if CAMERA_SVC_HAS_TEST
#define CAM_CAPTURE_UNKNOWN "camera: unknown option '%s' (try: test)\r\n"
#define CAM_STREAM_UNKNOWN  "camera: bad option '%s' (test)\r\n"
#else
#define CAM_CAPTURE_UNKNOWN "camera: unknown option '%s'\r\n"
#define CAM_STREAM_UNKNOWN  "camera: bad option '%s'\r\n"
#endif

/* ---- one-shot capture ----------------------------------------------------- */

static int cmd_camera_capture(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_svc_capture_report rep;
	struct camera_op_result res;
	int test = 0;

	if (argc > 1) {
		test = cam_parse_test(sh, argv[1], CAM_CAPTURE_UNKNOWN, "capture");
		if (test < 0)
			return 1;
	}

	/* The capture says what it paid to warm up; the report must not touch
	   that (svc/camera/camera_svc.h), so it starts out as "nothing". */
	memset(&rep, 0, sizeof rep);
	rep.warmup_unit = (uint8_t)CAMERA_SVC_WARMUP_NONE;
	camera_svc_capture(test, &rep, &res, cam_write, cam_cancelled, sh);
	if (res.status == CAMERA_SVC_ERR_CANCELLED)
		return 1;                 /* Ctrl+C: the core prints ^C */
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	camera_svc_capture_report(&rep, &res, cam_write, cam_cancelled, sh);
	if (res.status == CAMERA_SVC_ERR_CANCELLED)
		return 1;                 /* Ctrl+C: the core prints ^C */
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	/*
	 * [!] WHAT THE CAPTURE PAID TO WARM UP, IN ONE SHAPE ON EVERY BOARD
	 * (issue #143 decision 6): the frames it let go, or the time it slept.
	 * Printed when the board counts it at all, ZERO INCLUDED: a second
	 * capture on a settled sensor paying nothing is the answer to "did it
	 * wait this time", and a line that disappears cannot be told from a
	 * board that does not count.  A board that does not (NONE) prints none.
	 */
	if (rep.warmup_unit == (uint8_t)CAMERA_SVC_WARMUP_FRAMES)
		cli_print(sh, "warm-up  : %lu frame%s dropped\r\n",
		          (unsigned long)rep.warmup, rep.warmup == 1u ? "" : "s");
	else if (rep.warmup_unit == (uint8_t)CAMERA_SVC_WARMUP_MS)
		cli_print(sh, "warm-up  : %lu ms\r\n", (unsigned long)rep.warmup);
	/* The first bytes, from the same capture as the board's lines -- a quick
	   eyeball check. */
	if (rep.head_len != 0u) {
		uint32_t n = rep.head_len;

		if (n > CAMERA_SVC_HEAD_MAX)
			n = CAMERA_SVC_HEAD_MAX;
		cli_print(sh, "bytes[0..%lu]:\r\n", (unsigned long)(n - 1u));
		cli_hexdump(sh, rep.head, n);
	}
	return 0;
}

/* ---- reading the last capture out (save / send) -------------------------- */

#if CAMERA_SVC_HAS_SAVE || CAMERA_SVC_HAS_SEND
/* An open frame's failure, in the board's words.  @p res is the caller's, so
   one result serves the whole command (it is ~200 B of console stack). */
static int cam_open_frame(struct cli_instance *sh, struct camera_svc_frame *f,
                          struct camera_op_result *res)
{
	camera_svc_frame_open(f, res);
	if (res->status != CAMERA_SVC_OK) {
		cam_print_fail(sh, res, "");
		return -1;
	}
	return 0;
}
#endif

#if CAMERA_SVC_HAS_SAVE
/*
 * Bounded scratch for `save`: the frame is streamed in fixed chunks so no
 * full-frame staging buffer is needed.  On the console's stack.
 */
#define CAM_SAVE_CHUNK 512u

/* The media list, spelled three ways from the board's one X-macro. */
#define CAM_MEDIUM_FIRST_(s) s
#define CAM_MEDIUM_BAR_(s)   "|" s
#define CAM_MEDIUM_OR_(s)    " or " s
#define CAM_SAVE_MEDIA_BAR   CAMERA_SVC_SAVE_MEDIA(CAM_MEDIUM_FIRST_, CAM_MEDIUM_BAR_)
#define CAM_SAVE_MEDIA_OR    CAMERA_SVC_SAVE_MEDIA(CAM_MEDIUM_FIRST_, CAM_MEDIUM_OR_)
#define CAM_MEDIUM_DROP_(s)
/* The default medium: the first the board names (issue #143 decision 3). */
#define CAM_SAVE_MEDIUM_DEFAULT CAMERA_SVC_SAVE_MEDIA(CAM_MEDIUM_FIRST_, CAM_MEDIUM_DROP_)
#define CAM_SAVE_USAGE       "[" CAM_SAVE_MEDIA_BAR "] <path>"

/* The PC-side conversion hint for the saved frame's format. */
static void cam_convert_hint(struct cli_instance *sh,
                             const struct camera_svc_frame *f, const char *path)
{
	if (f->convert == NULL)
		cli_print(sh, "PC: %s is a JPEG stream -- open it directly\r\n", path);
	else
		cli_print(sh, "PC: python3 %s --width %u --height %u %s out.png\r\n",
		          f->convert, (unsigned)f->width, (unsigned)f->height, path);
}

/*
 * Write the open frame to <path> on @p dev, raw in its format, chunk by chunk.
 * Runs under the medium's shared op slot (the same gate as the fs/sd command
 * bodies), so format/umount cannot yank the media mid-save.  A read that finds
 * the frame replaced fails the save rather than mixing two frames -- the
 * adapter checks and copies in one step -- and the file is left partial.
 */
/* [!] ONCE THE FILE IS OPEN, EVERY WAY OUT CLOSES IT AND FLUSHES THE MEDIUM,
   so a partial file is at least a consistent one on the card. */
static void cam_save_close(FX_FILE *file, FX_MEDIA *media)
{
	(void)fx_file_close(file);
	(void)fx_media_flush(media);
}

static int cam_save_body(const struct fs_device *dev, struct cli_instance *sh,
                         const struct camera_svc_frame *f, const char *path,
                         struct camera_op_result *res)
{
	uint8_t buf[CAM_SAVE_CHUNK];
	FX_MEDIA *media = fs_core_mount(dev, sh);
	FX_FILE file;
	uint32_t off;
	UINT status;

	if (media == NULL)
		return 1;

	/* [!] THE FRAME IS ASKED AGAIN AFTER THE MOUNT AND BEFORE THE FILE IS
	   TOUCHED.  The mount can take a while, and another console may replace
	   or drop the frame meanwhile (`camera res`, a failed capture); finding
	   that out on the first chunk would be after the create/truncate, with an
	   existing file already emptied.  One byte through the same read, so the
	   validity and the generation are the read's own. */
	camera_svc_frame_read(f, 0u, buf, 1u, res);
	if (res->status != CAMERA_SVC_OK) {
		cam_print_fail(sh, res, "");
		return 1;
	}

	status = fx_file_create(media, (CHAR *)path);
	if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
		cli_error(sh, "camera: create %s: %s (0x%02x)\r\n",
		          path, fs_strerror(status), status);
		return 1;
	}
	status = fx_file_open(media, &file, (CHAR *)path, FX_OPEN_FOR_WRITE);
	if (status != FX_SUCCESS) {
		cli_error(sh, "camera: open %s: %s (0x%02x)\r\n",
		          path, fs_strerror(status), status);
		return 1;
	}

	status = fx_file_truncate(&file, 0);
	for (off = 0; status == FX_SUCCESS && off < f->len; ) {
		uint32_t n = (f->len - off < CAM_SAVE_CHUNK) ? f->len - off
		                                            : CAM_SAVE_CHUNK;

		camera_svc_frame_read(f, off, buf, n, res);
		if (res->status != CAMERA_SVC_OK) {
			cam_save_close(&file, media);
			cam_print_fail(sh, res, " (file left partial)");
			return 1;
		}
		status = fx_file_write(&file, buf, n);
		off += n;
		if (cli_cancel_requested(sh)) {
			cam_save_close(&file, media);
			cli_error(sh, "camera: cancelled (file left partial)\r\n");
			return 1;
		}
	}
	(void)fx_file_close(&file);
	if (status == FX_SUCCESS) {
		status = fx_media_flush(media);
	} else {
		(void)fx_media_flush(media);   /* what was written, a write failed */
	}

	if (status != FX_SUCCESS) {
		cli_error(sh, "camera: write failed: %s (0x%02x)\r\n",
		          fs_strerror(status), status);
		return 1;
	}
	cli_print(sh, "wrote %lu bytes (%ux%u %s) to %s:%s\r\n",
	          (unsigned long)f->len, (unsigned)f->width, (unsigned)f->height,
	          f->format, dev->name, path);
	cam_convert_hint(sh, f, path);
	return 0;
}

static int cmd_camera_save(struct cli_instance *sh, int argc, char **argv)
{
	const struct fs_device *dev;
	const char *path;
	struct camera_svc_frame f;
	struct camera_op_result res;
	UINT status;
	int rc;

	/* `save [<medium>] <path>`: the medium may be left out, and is then the
	   board's first.  [!] A lone word that names a medium is refused rather
	   than taken as a file name: `camera save sd` is a forgotten path far more
	   often than a file called "sd". */
	if (argc < 3) {
		if (camera_board_medium(argv[1]) != NULL) {
			cli_error(sh, "camera: '%s' names a medium -- give the path: "
			          "camera save " CAM_SAVE_USAGE "\r\n", argv[1]);
			return 1;
		}
		dev = camera_board_medium(CAM_SAVE_MEDIUM_DEFAULT);
		path = argv[1];
	} else {
		dev = camera_board_medium(argv[1]);
		path = argv[2];
		if (dev == NULL) {
			cli_error(sh, "camera: unknown medium '%s' (" CAM_SAVE_MEDIA_OR
			          ")\r\n", argv[1]);
			return 1;
		}
	}
	if (dev == NULL) {   /* the board's own default it cannot resolve */
		cli_error(sh, "camera: no medium '%s' on this board\r\n",
		          CAM_SAVE_MEDIUM_DEFAULT);
		return 1;
	}
	/* The frame first, the medium after: no file is created when there is
	   nothing to put in it, and "no captured frame" comes before any medium
	   error, as it always has. */
	if (cam_open_frame(sh, &f, &res) != 0)
		return 1;

	status = dev->op_begin();
	if (status != FX_SUCCESS) {
		cli_error(sh, "camera: %s: %s\r\n", dev->name, fs_strerror(status));
		camera_svc_frame_close(&f);
		return 1;
	}
	rc = cam_save_body(dev, sh, &f, path, &res);
	dev->op_end();
	camera_svc_frame_close(&f);
	return rc;
}
#endif /* CAMERA_SVC_HAS_SAVE */

#if CAMERA_SVC_HAS_SEND
/* The YMODEM source over the open frame: sequential, each chunk read (checked
   and copied in one step by the adapter) straight into ymodem's block. */
struct cam_send_cursor {
	const struct camera_svc_frame *f;
	struct camera_op_result *res;   /* the command's one result */
	uint32_t off;
	int read_failed;                /* res holds the failed read's words */
};

static int cam_send_read(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got)
{
	struct cam_send_cursor *c = (struct cam_send_cursor *)ctx;
	uint32_t n;

	if (c->off >= c->f->len) {
		*got = 0;
		return 0;                       /* EOF */
	}
	n = c->f->len - c->off;
	if (want < n)
		n = want;
	camera_svc_frame_read(c->f, c->off, dst, n, c->res);
	if (c->res->status != CAMERA_SVC_OK) {
		c->read_failed = 1;
		return -1;                      /* frame changed mid-send, or gone */
	}
	c->off += n;
	*got = n;
	return 0;
}

static int cmd_camera_send(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_svc_frame f;
	struct camera_op_result res;
	struct cam_send_cursor c;
	struct ym_source src;
	int rc;

	if (cam_open_frame(sh, &f, &res) != 0)
		return 1;
	c.f = &f;
	c.res = &res;
	c.off = 0u;
	c.read_failed = 0;
	src.ctx  = &c;
	src.name = (argc >= 2) ? argv[1] : "frame.raw";
	src.size = f.len;
	src.read = cam_send_read;
	rc = camera_board_send(sh, &src);
	/* The transfer's own line said "source read error"; this says why, in
	   the board's words (the frame was replaced, the camera was busy). */
	if (c.read_failed)
		cam_print_fail(sh, &res, "");
	camera_svc_frame_close(&f);
	return rc;
}
#endif /* CAMERA_SVC_HAS_SEND */

/* ---- the stream ----------------------------------------------------------- */

static int cmd_stream_start(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;
	int test = 0, already = 0;
	int i;

	/* No --frames / --secs (issue #142): a stream runs while somebody holds
	   it, and only `nn stream start --frames` bounds anything. */
	for (i = 1; i < argc; i++) {
		int t = cam_parse_test(sh, argv[i], CAM_STREAM_UNKNOWN, "stream");

		if (t < 0)
			return 1;
		test = 1;
	}

	/* The board writes its outcome line (svc/camera/camera_svc.h). */
	camera_svc_stream_start(test, &res, &already, cam_write, sh);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	return 0;
}

static int cmd_stream_stop(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;
	uint32_t gen = 0u;
	int what;

	(void)argc;
	(void)argv;
	what = camera_svc_stream_stop(&res, &gen);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	if (what == CAMERA_SVC_STOP_DONE)
		cli_print(sh, "camera: stream stopped\r\n");
	else
		cli_print(sh, "camera: stream stop requested\r\n");
	return 0;
}

/* The shared head of `camera stream stats`: the state and who holds the
   stream (issue #142), from the sample the board's lines are written from. */
static void cam_stats_head(void *ctx, const struct camera_svc_state *st)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;
	unsigned n;

	if (st->never_started) {
		cli_print(sh, "state    : never started\r\n");
		return;
	}
	if (st->streaming)
		cli_print(sh, "state    : streaming\r\n");
	else if (st->stop_reason != NULL)
		cli_print(sh, "state    : stopped (%s)\r\n", st->stop_reason);
	else
		cli_print(sh, "state    : stopped\r\n");

	/* One line, one print: a background job's output cannot land inside it. */
	n = (st->nusers < CAMERA_SVC_USERS_MAX) ? st->nusers : CAMERA_SVC_USERS_MAX;
	{
		char list[64];
		size_t len = 0;

		list[0] = '\0';
		for (unsigned i = 0; i < n; i++) {
			const char *name = st->users[i] ? st->users[i] : "?";
			size_t w = strlen(name);

			if (len + 2u + w >= sizeof list)
				break;
			list[len++] = i ? ',' : ' ';
			if (i)
				list[len++] = ' ';
			memcpy(list + len, name, w + 1u);
			len += w;
		}
		/* Mid-transition there is no list, but the lost count still
		   stands (it is the last confirmed end's), as the boards' own
		   heads have always printed it. */
		if (st->transition && st->lost)
			cli_print(sh, "holders  : -- (a stream start or stop is in "
			          "progress)  (%d lost when the stream ended)\r\n",
			          (int)st->lost);
		else if (st->transition)
			cli_print(sh, "holders  : -- (a stream start or stop is in "
			          "progress)\r\n");
		else if (st->lost)
			cli_print(sh, "holders  :%s  (%d lost when the stream ended)\r\n",
			          n ? list : " none", (int)st->lost);
		else
			cli_print(sh, "holders  :%s\r\n", n ? list : " none");
	}
}

static int cmd_stream_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_op_result res;

	(void)argc;
	(void)argv;

	/* One sample for the state and the numbers (svc/camera/camera_svc.h). */
	camera_svc_stream_report(&res, cam_write, cam_stats_head, sh);
	if (res.status != CAMERA_SVC_OK) {
		cam_print_fail(sh, &res, "");
		return 1;
	}
	return 0;
}

static int cmd_camera_stream(struct cli_instance *sh, int argc, char **argv)
{
	if (argc > 1)
		cli_error(sh, "camera: unknown stream subcommand '%s'\r\n", argv[1]);
#if CAMERA_SVC_HAS_TEST
	cli_print(sh, "usage: camera stream <start [test] | stop | stats>\r\n");
#else
	cli_print(sh, "usage: camera stream <start | stop | stats>\r\n");
#endif
	return argc > 1 ? 1 : 0;
}

/* ---- registration -------------------------------------------------------- */

/*
 * [!] `test` IS ACCEPTED BY THE GRAMMAR EVEN WITHOUT A TEST PATTERN (one
 * optional word), so the refusal can say why; only the help and the usage
 * spelling drop it.
 */
#if CAMERA_SVC_HAS_TEST
#define CAM_CAPTURE_HELP  "snapshot the current mode + stats ('test' = colorbar)"
#define CAM_TEST_USAGE    "[test]"
#define CAM_START_HELP    "begin continuous capture (test = colorbar)"
#else
#define CAM_CAPTURE_HELP  "snapshot the current mode + stats"
#define CAM_TEST_USAGE    NULL
#define CAM_START_HELP    "begin continuous capture"
#endif

CLI_SUBCMD_SET_CREATE(camera_stream_subcmds,
	CLI_CMD_ARG_USAGE(start, NULL, CAM_START_HELP, CAM_TEST_USAGE,
	                  cmd_stream_start, 1, 1),
	CLI_CMD(stop,  NULL, "stop the running stream", cmd_stream_stop),
	CLI_CMD(stats, NULL, "FPS / frame / overrun counters", cmd_stream_stats),
	CLI_SUBCMD_SET_END);

/*
 * [!] A SUBCOMMAND A BOARD DOES NOT HAVE IS NOT REGISTERED, rather than
 * registered as a stub that answers "not supported" (the same rule as `nn`).
 * The order is the shared one -- probe / on / off / info / capture / save /
 * send / stream -- and the board's own subcommands come last.
 */
CLI_SUBCMD_SET_CREATE(camera_subcmds,
	CLI_CMD(probe, NULL, "power up + read the sensor chip ID", cmd_camera_probe),
#if CAMERA_SVC_HAS_POWER
	CLI_CMD(on,    NULL, "power on the camera module (bring-up, no ID report)",
	        cmd_camera_on),
	CLI_CMD(off,   NULL, "power down the camera module (this board's deepest state)",
	        cmd_camera_off),
#endif
#if CAMERA_SVC_HAS_INFO
	CLI_CMD(info,  NULL, "driver / sensor state", cmd_camera_info),
#endif
	CLI_CMD_ARG_USAGE(capture, NULL, CAM_CAPTURE_HELP, CAM_TEST_USAGE,
	                  cmd_camera_capture, 1, 1),
#if CAMERA_SVC_HAS_SAVE
	CLI_CMD_ARG_USAGE(save, NULL,
	                  "write the last capture raw " CAM_SAVE_USAGE " (default "
	                  CAM_SAVE_MEDIUM_DEFAULT ")",
	                  CAM_SAVE_USAGE, cmd_camera_save, 2, 1),
#endif
#if CAMERA_SVC_HAS_SEND
	CLI_CMD_ARG_USAGE(send, NULL, "send the last capture to the PC over YMODEM [name]",
	                  "[name]", cmd_camera_send, 1, 1),
#endif
	CLI_CMD_ARG_USAGE(stream, camera_stream_subcmds,
	                  "continuous capture (start/stop/stats)",
	                  "[start|stop|stats]", cmd_camera_stream, 1, 1),
	CAMERA_SVC_BOARD_SUBCMDS
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(camera, camera_subcmds, CAMERA_SVC_HELP, NULL, 1, 0);
