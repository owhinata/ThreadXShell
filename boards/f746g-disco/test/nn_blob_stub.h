/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A stand-in for this board's blob store (src/blob.h), for the nn adapter's
 * host tests (test_nn_model_life.c, test_nn_model_container.c; issue #194).
 *
 * The functions the adapter calls -- the busy flag, blob_find() and
 * blob_read_reserved() -- over a handful of pretend slots, with each answer
 * settable and each call counted.  The name lookup is the shared rule
 * (svc/blob/blob_state.c's blob_resolve_name()), as the real blob_find() runs
 * it, so "two VALID slots under one name" answers DUPLICATE here as there.
 */
#ifndef NN_BLOB_STUB_H
#define NN_BLOB_STUB_H

#include <stdint.h>

#include "blob.h"

#define STUB_BLOB_SLOTS  4u
#define STUB_BLOB_BYTES  8192u

struct nn_blob_stub {
	struct blob_info info[STUB_BLOB_SLOTS];
	uint8_t  data[STUB_BLOB_SLOTS][STUB_BLOB_BYTES];
	int      find_rc;        /* a header read that fails (BLOB_ERR_IO, ...)  */
	int      read_rc;        /* a payload read that fails                    */
	int      held_elsewhere; /* the flag is held by somebody else             */
	int      held;           /* held by the adapter now                      */
	unsigned acquires, releases, finds, reads;
	uint32_t last_read_len;
	void   (*on_read)(void);  /* called as the payload read begins          */
};

extern struct nn_blob_stub nn_blob_stub;

void nn_blob_stub_reset(void);
/* Store @p len bytes of @p data in slot @p slot under @p name, VALID, with the
 * CRC of exactly those bytes (as `blob write` stamps it). */
void nn_blob_stub_put(unsigned slot, const char *name, const void *data,
                      uint32_t len);

#endif /* NN_BLOB_STUB_H */
