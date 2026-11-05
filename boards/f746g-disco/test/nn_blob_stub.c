/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * See nn_blob_stub.h.
 */
#include "nn_blob_stub.h"

#include <assert.h>
#include <string.h>

#include "crc32.h"

struct nn_blob_stub nn_blob_stub;

void nn_blob_stub_reset(void)
{
	memset(&nn_blob_stub, 0, sizeof nn_blob_stub);
}

void nn_blob_stub_put(unsigned slot, const char *name, const void *data,
                      uint32_t len)
{
	struct blob_info *i = &nn_blob_stub.info[slot];

	assert(slot < STUB_BLOB_SLOTS && len <= STUB_BLOB_BYTES);
	memset(i, 0, sizeof *i);
	i->state = BLOB_VALID;
	strncpy(i->name, name, BLOB_NAME_MAX);
	i->length = len;
	memcpy(nn_blob_stub.data[slot], data, len);
	i->crc32 = crc32_update(0u, data, len);
}

int blob_busy_acquire(void)
{
	if (nn_blob_stub.held_elsewhere || nn_blob_stub.held)
		return BLOB_ERR_BUSY;
	nn_blob_stub.held = 1;
	nn_blob_stub.acquires++;
	return BLOB_OK;
}

void blob_busy_release(void)
{
	assert(nn_blob_stub.held);
	nn_blob_stub.held = 0;
	nn_blob_stub.releases++;
}

int blob_find(const char *name, unsigned *slot, struct blob_info *out,
              enum blob_lookup *why)
{
	struct blob_slot_view v[STUB_BLOB_SLOTS];
	unsigned i, found = 0u;
	enum blob_lookup l;

	/* The adapter holds the flag across the lookup (src/blob.h). */
	assert(nn_blob_stub.held);
	nn_blob_stub.finds++;
	if (why)
		*why = BLOB_LOOKUP_REFUSE;
	if (nn_blob_stub.find_rc != BLOB_OK)
		return nn_blob_stub.find_rc;
	for (i = 0u; i < STUB_BLOB_SLOTS; i++) {
		v[i].state = nn_blob_stub.info[i].state;
		v[i].name_match = (uint8_t)(v[i].state == BLOB_VALID &&
		                            strcmp(nn_blob_stub.info[i].name, name)
		                            == 0);
	}
	l = blob_resolve_name(v, STUB_BLOB_SLOTS, &found);
	if (why)
		*why = l;
	if (l == BLOB_LOOKUP_FOUND) {
		*slot = found;
		*out = nn_blob_stub.info[found];
		return BLOB_OK;
	}
	return l == BLOB_LOOKUP_DUPLICATE ? BLOB_ERR_DUPLICATE : BLOB_ERR_EMPTY;
}

int blob_read_reserved(unsigned slot, uint32_t off, void *buf, uint32_t len)
{
	/* Only the holder may read the reserved way. */
	assert(nn_blob_stub.held);
	assert(slot < STUB_BLOB_SLOTS && off == 0u && len <= STUB_BLOB_BYTES);
	nn_blob_stub.reads++;
	nn_blob_stub.last_read_len = len;
	if (nn_blob_stub.on_read)
		nn_blob_stub.on_read();
	if (nn_blob_stub.read_rc != BLOB_OK)
		return nn_blob_stub.read_rc;
	memcpy(buf, nn_blob_stub.data[slot], len);
	return BLOB_OK;
}
