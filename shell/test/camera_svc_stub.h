/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * What the stub camera adapter (camera_svc_stub.c) answers, and what it saw
 * (issue #143).  Test-only state; the shared command under test owns none.
 */
#ifndef CAMERA_SVC_STUB_H
#define CAMERA_SVC_STUB_H

#include <stdint.h>

#include "camera_svc.h"

struct camera_stub {
	/* answers */
	int status;                       /* every op_result's status        */
	char detail[CAMERA_SVC_DETAIL_MAX + 1];
	const char *users[CAMERA_SVC_USERS_MAX];
	unsigned nusers;
	int streaming;
	int stop_what;                    /* enum camera_svc_stop on success  */
	int already;
	const char *stop_reason;
	const char *holders[CAMERA_SVC_USERS_MAX];
	unsigned nholders;
	uint32_t lost;
	uint32_t head_len;
	uint32_t frame_len;
	int read_fail;                    /* every read fails               */
	int read_fail_at;                 /* the Nth read (1-based) and on  */
	int state_status;                 /* camera_svc_stream_state()'s    */
	int tag_samples;                  /* name each stats sample          */
	/* what was called */
	int calls_probe, calls_on, calls_off, calls_info, calls_capture;
	int calls_open, calls_read, calls_start, calls_stop, calls_send;
	int calls_op_begin, calls_op_end, closed_file, created_file;
	int cancel_asked;
	int calls_state;                  /* camera_svc_stream_state()      */
	int samples;                      /* camera_svc_stream_report()     */
	int opened;                       /* open minus close: 0 when balanced */
	int last_test;
	uint32_t ended_gen;
	const char *send_name;
	uint32_t send_size;
	uint32_t written;
};

extern struct camera_stub camera_stub;

void camera_stub_reset(void);

#endif /* CAMERA_SVC_STUB_H */
