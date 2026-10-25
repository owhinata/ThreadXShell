/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A stub camera adapter (svc/camera_svc.h) and stub board services
 * (shell/cmds/camera_cmd_board.h, FileX) for the shared `camera` command's
 * host tests (issue #143).  The test sets what each call answers through
 * camera_stub (camera_svc_stub.h) and reads back what was called.
 *
 * Shared by shell/test/test_camera_cmd.c (stand-in configs) and the boards'
 * table tests (their REAL configs), so it defines every camera_svc_* whatever
 * the shape: a definition the command never calls costs nothing here.
 */
#include <string.h>

#include "camera_svc.h"
#include "camera_svc_config.h"
#include "camera_cmd_board.h"
#include "camera_svc_stub.h"
#include "ymodem.h"

#if CAMERA_SVC_HAS_SAVE
#include "fs_cmd_core.h"
#endif

struct camera_stub camera_stub;

void camera_stub_reset(void)
{
	memset(&camera_stub, 0, sizeof camera_stub);
	camera_stub.stop_what = CAMERA_SVC_STOP_REQUESTED;
	camera_stub.frame_len = 8u;
}

static void stub_result(struct camera_op_result *res)
{
	memset(res, 0, sizeof *res);
	res->status = camera_stub.status;
	strncpy(res->detail, camera_stub.detail, sizeof res->detail - 1u);
	for (unsigned i = 0; i < camera_stub.nusers && i < CAMERA_SVC_USERS_MAX; i++)
		res->users[i] = camera_stub.users[i];
	res->nusers = camera_stub.nusers;
}

static void stub_line(camera_svc_write_fn write, void *ctx, const char *s)
{
	(void)write(ctx, s, strlen(s));
}

void camera_svc_probe(struct camera_op_result *res,
                      camera_svc_write_fn write, void *ctx)
{
	camera_stub.calls_probe++;
	stub_result(res);
	if (res->status == CAMERA_SVC_OK)
		stub_line(write, ctx, "stub: probed\r\n");
}

void camera_svc_on(struct camera_op_result *res,
                   camera_svc_write_fn write, void *ctx)
{
	camera_stub.calls_on++;
	stub_result(res);
	if (res->status == CAMERA_SVC_OK)
		stub_line(write, ctx, "stub: on\r\n");
}

void camera_svc_off(struct camera_op_result *res,
                    camera_svc_write_fn write, void *ctx)
{
	camera_stub.calls_off++;
	stub_result(res);
	if (res->status == CAMERA_SVC_OK)
		stub_line(write, ctx, "stub: off\r\n");
}

void camera_svc_info(struct camera_op_result *res,
                     camera_svc_write_fn write, void *ctx)
{
	camera_stub.calls_info++;
	stub_result(res);
	if (res->status == CAMERA_SVC_OK)
		stub_line(write, ctx, "stub: info\r\n");
}

void camera_svc_capture(int test, struct camera_svc_capture_report *rep,
                        struct camera_op_result *res,
                        camera_svc_write_fn write,
                        int (*cancelled)(void *ctx), void *ctx)
{
	stub_line(write, ctx, test ? "stub: capturing test\r\n"
	                           : "stub: capturing live\r\n");
	camera_stub.calls_capture++;
	camera_stub.last_test = test;
	stub_result(res);
	if (res->status != CAMERA_SVC_OK)
		return;
	if (camera_stub.capture_cancels && cancelled(ctx)) {
		res->status = CAMERA_SVC_ERR_CANCELLED;
		return;
	}
	rep->warmup = camera_stub.warmup;
	rep->warmup_unit = camera_stub.warmup_unit;
}

void camera_svc_capture_report(struct camera_svc_capture_report *rep,
                               struct camera_op_result *res,
                               camera_svc_write_fn write,
                               int (*cancelled)(void *ctx), void *ctx)
{
	/* Never the warm-up: that is the capture's (svc/camera_svc.h). */
	memset(rep->head, 0, sizeof rep->head);
	rep->head_len = 0u;
	stub_result(res);
	if (res->status != CAMERA_SVC_OK)
		return;
	/* A walk of two chunks, asking between them as a board must. */
	for (int chunk = 0; chunk < 2; chunk++) {
		camera_stub.cancel_asked++;
		if (cancelled(ctx)) {
			res->status = CAMERA_SVC_ERR_CANCELLED;
			return;
		}
	}
	stub_line(write, ctx, "stub: frame stats\r\n");
	rep->head_len = camera_stub.head_len;
	for (uint32_t i = 0; i < rep->head_len && i < CAMERA_SVC_HEAD_MAX; i++)
		rep->head[i] = (uint8_t)(0xA0u + i);
}

void camera_svc_frame_open(struct camera_svc_frame *f,
                           struct camera_op_result *res)
{
	camera_stub.calls_open++;
	stub_result(res);
	memset(f, 0, sizeof *f);
	f->gen = 7u;
	f->len = camera_stub.frame_len;
	f->width = 2u;
	f->height = 2u;
	f->format = "rgb565";
	f->convert = "scripts/rgb565_to_png.py";
	if (res->status == CAMERA_SVC_OK)
		camera_stub.opened++;
}

void camera_svc_frame_read(const struct camera_svc_frame *f, uint32_t off,
                           void *dst, uint32_t n, struct camera_op_result *res)
{
	(void)f;
	camera_stub.calls_read++;
	memset(res, 0, sizeof *res);
	if (camera_stub.read_fail ||
	    (camera_stub.read_fail_at != 0 &&
	     camera_stub.calls_read >= camera_stub.read_fail_at)) {
		res->status = CAMERA_SVC_ERR_NO_FRAME;
		strcpy(res->detail, "stub: frame changed");
		return;
	}
	memset(dst, (int)(off & 0xFFu), n);
}

void camera_svc_frame_close(struct camera_svc_frame *f)
{
	(void)f;
	camera_stub.opened--;
}

void camera_svc_stream_start(int test, struct camera_op_result *res,
                             int *already,
                             camera_svc_write_fn write, void *ctx)
{
	camera_stub.calls_start++;
	camera_stub.last_test = test;
	*already = camera_stub.already;
	stub_result(res);
	if (res->status == CAMERA_SVC_OK)
		stub_line(write, ctx, *already ? "stub: already streaming\r\n"
		                               : "stub: streaming\r\n");
}

int camera_svc_stream_stop(struct camera_op_result *res, uint32_t *gen)
{
	camera_stub.calls_stop++;
	stub_result(res);
	*gen = 42u;                        /* the stream this stop names */
	if (res->status != CAMERA_SVC_OK)
		return CAMERA_SVC_STOP_NONE;
	if (!camera_stub.stop_never_ends)
		camera_stub.done_gen = 42u;    /* ...and it ends */
	if (!camera_stub.another_starts)
		camera_stub.streaming = 0;
	return camera_stub.stop_what;
}

/* By generation, as the contract says -- never by "is anything streaming", so
   a command that waited on the wrong thing would disagree with this stub. */
int camera_svc_stream_ended(uint32_t gen)
{
	camera_stub.ended_gen = gen;
	camera_stub.ended_calls++;
	return gen == CAMERA_SVC_GEN_NONE ||
	       (int32_t)(camera_stub.done_gen - gen) >= 0;
}

void camera_svc_stream_state(struct camera_svc_state *st,
                             struct camera_op_result *res)
{
	camera_stub.calls_state++;
	memset(st, 0, sizeof *st);
	memset(res, 0, sizeof *res);
	res->status = camera_stub.state_status;
	if (res->status != CAMERA_SVC_OK)
		strcpy(res->detail, "stub: state unavailable");
	st->streaming = (uint8_t)camera_stub.streaming;
	st->stop_reason = camera_stub.stop_reason;
	for (unsigned i = 0; i < camera_stub.nholders && i < CAMERA_SVC_USERS_MAX; i++)
		st->users[i] = camera_stub.holders[i];
	st->nusers = camera_stub.nholders;
	st->lost = camera_stub.lost;
	st->transition = (uint8_t)camera_stub.transition;
}

void camera_svc_stream_report(struct camera_op_result *res,
                              camera_svc_write_fn write,
                              camera_svc_state_fn head, void *ctx)
{
	/* Every sample is told apart: with tag_samples set the state's reason
	   and the board's line both name the sample they came from, so a
	   command that printed them from two samples could not match them. */
	static const char *const tag[] = { "sample-1", "sample-2", "sample-3" };
	struct camera_svc_state st;
	struct camera_op_result sres;
	const char *t;

	camera_stub.samples++;
	t = tag[(camera_stub.samples - 1) % 3];
	if (camera_stub.never_started) {
		memset(res, 0, sizeof *res);
		memset(&st, 0, sizeof st);
		st.never_started = 1u;
		head(ctx, &st);
		return;                        /* and no board lines */
	}
	stub_result(res);
	if (res->status == CAMERA_SVC_OK)
		res->status = camera_stub.state_status;
	if (res->status != CAMERA_SVC_OK) {
		if (res->detail[0] == '\0')
			strcpy(res->detail, "stub: state unavailable");
		return;
	}
	camera_svc_stream_state(&st, &sres);
	camera_stub.calls_state--;          /* the stub's own use, not the command's */
	if (camera_stub.tag_samples && !st.streaming)
		st.stop_reason = t;
	head(ctx, &st);
	stub_line(write, ctx, "stub: board stats ");
	stub_line(write, ctx, camera_stub.tag_samples ? t : "");
	stub_line(write, ctx, "\r\n");
}

/* ---- the board's shell-layer services ------------------------------------ */

int camera_board_send(struct cli_instance *sh, const struct ym_source *src)
{
	(void)sh;
	camera_stub.calls_send++;
	camera_stub.send_name = src->name;
	camera_stub.send_size = src->size;
	return 0;
}

#if CAMERA_SVC_HAS_SAVE
static UINT stub_op_begin(void) { camera_stub.calls_op_begin++; return FX_SUCCESS; }
static void stub_op_end(void)   { camera_stub.calls_op_end++; }

static const struct fs_device stub_sd = { .name = "sd", .op_begin = stub_op_begin,
                                          .op_end = stub_op_end };
static const struct fs_device stub_fs = { .name = "fs", .op_begin = stub_op_begin,
                                          .op_end = stub_op_end };

const struct fs_device *camera_board_medium(const char *name)
{
	if (strcmp(name, "sd") == 0)
		return &stub_sd;
	if (strcmp(name, "fs") == 0)
		return &stub_fs;
	return NULL;
}

static FX_MEDIA stub_media;

const char *fs_strerror(UINT status) { (void)status; return "stub fs error"; }
FX_MEDIA *fs_core_mount(const struct fs_device *dev, struct cli_instance *sh)
{
	(void)dev;
	(void)sh;
	return &stub_media;
}
UINT fx_file_create(FX_MEDIA *m, CHAR *name)
{
	(void)m; (void)name;
	camera_stub.created_file++;
	return FX_SUCCESS;
}
UINT fx_file_open(FX_MEDIA *m, FX_FILE *f, CHAR *name, UINT type)
{
	(void)m; (void)f; (void)name; (void)type;
	return FX_SUCCESS;
}
UINT fx_file_truncate(FX_FILE *f, ULONG size) { (void)f; (void)size; return FX_SUCCESS; }
UINT fx_file_write(FX_FILE *f, VOID *buf, ULONG size)
{
	(void)f; (void)buf;
	camera_stub.written += (uint32_t)size;
	return FX_SUCCESS;
}
UINT fx_file_close(FX_FILE *f) { (void)f; camera_stub.closed_file++; return FX_SUCCESS; }
UINT fx_media_flush(FX_MEDIA *m) { (void)m; camera_stub.flushed++; return FX_SUCCESS; }
#endif
