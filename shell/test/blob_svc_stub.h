/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The stub store behind test_blob_cmd.c (issue #194): every svc/blob/blob_svc.h
 * function and the three shell/cmds/blob_cmd_board.h hooks, over a pretend
 * part and slot table, with each answer settable and each call counted.
 */
#ifndef BLOB_SVC_STUB_H
#define BLOB_SVC_STUB_H

#include <stdint.h>

#include "blob_state.h"

#define STUB_SLOTS 13u

struct blob_stub {
	/* what the slots hold (headers are decoded from these, not from bytes) */
	enum blob_slot_state state[STUB_SLOTS];
	char     name[STUB_SLOTS][BLOB_NAME_MAX + 1];
	uint32_t length[STUB_SLOTS], crc[STUB_SLOTS];
	/* answers */
	int stat_rc, read_rc, verify_rc, reserve_rc, erase_hdr_rc;
	int ready;               /* blob_svc_ready() */
	int raw_refuse;          /* blob_board_raw_console() refuses */
	int say_rc;              /* blob_board_say() speaks for this rc, or 0 */
	uint32_t verify_crc;     /* what blob_verify() computes */
	int corrupt_read_back;   /* the write's verify disagrees */
	int fail_magic;          /* the magic page's program fails */
	uint32_t send;           /* bytes the receive delivers */
	uint32_t drops;          /* bytes the receive "drops" (sh->rx_dropped) */
	int      level_rc;       /* blob_board_rx_level()'s answer */
	uint32_t level_high, level_size;
	/* what happened */
	unsigned reserves, unreserves, erase_headers, erase_spans, programs;
	unsigned receives, says;
	unsigned level_clears;   /* blob_board_rx_level_clear() calls */
	unsigned erases_at_clear;/* erase spans already run at the last clear */
	unsigned level_reads;
	unsigned store_calls;    /* blob_stat / _read / _verify / _slot_geometry */
	uint32_t read_bytes;     /* payload bytes blob_read() handed out */
};

extern struct blob_stub blob_stub;

void blob_stub_reset(void);

#endif /* BLOB_SVC_STUB_H */
