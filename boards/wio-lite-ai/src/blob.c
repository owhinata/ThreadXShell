/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    blob.c
 * @brief   The asset store's NOR side (owhinata/wio-lite-ai#10 / owhinata/wio-lite-ai#9
 * P2b; on the shared svc/blob parts since issue #193).
 *
 * Thin on purpose.  The decisions -- what a header says, whether a table is
 * legal, which slot a name lives in -- are svc/blob/, where a host test walks
 * them; what is left here is the part that needs this board: the interval, the
 * indirect NOR reads and erases, and the busy flag.  See blob.h for the layout
 * and for why the device lock is not held across a transfer.
 */
#include "blob.h"

#include <string.h>

#include "crc32.h"           /* crc32_update -- FlashDB's, port/nn/crc32_fdb.c */
#include "nor_flash.h"
#include "stm32h7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

/* The header's page is a fact of the format (svc/blob/blob_state.h), and each of
 * its two pages is programmed exactly once.  That holds only while one header
 * page is one program page of this part, so the build refuses a format and a
 * part that disagree rather than letting either follow the other. */
_Static_assert(BLOB_HDR_PAGE == NOR_PAGE_SIZE,
               "the blob header page is not this part's program page");
_Static_assert(BLOB_HDR_SPAN <= BLOB_ERASE_UNIT,
               "the two header pages must fit in the header sector");
_Static_assert(BLOB_REGION_END <= NOR_SIZE_BYTES,
               "the blob region must fit on the device");
_Static_assert(BLOB_REGION_BASE >= 1024u * 1024u,
               "the blob region must start above the kv partition");
_Static_assert((BLOB_REGION_BASE % NOR_BLOCK_SIZE) == 0u &&
               (BLOB_REGION_END % NOR_BLOCK_SIZE) == 0u,
               "the blob region must be whole 64 KB erase blocks");

/* ---- the table ------------------------------------------------------------- */

enum blob_map_verdict blob_check_map(unsigned *bad)
{
	return blob_map_check(BLOB_REGION_BASE, BLOB_REGION_END, BLOB_ERASE_UNIT,
	                      blob_map_table(), blob_map_count(), bad);
}

/* The slot, once the table has been checked, the index is in range and the
 * device answers. */
static const struct blob_slot *checked_slot(unsigned slot, int *err)
{
	const struct blob_slot *s;

	if (blob_check_map(NULL) != BLOB_MAP_OK) {
		*err = BLOB_ERR_MAP;
		return NULL;
	}
	s = blob_map_slot(slot);
	if (s == NULL) {
		*err = BLOB_ERR_PARAM;
		return NULL;
	}
	if (!nor_flash_ready()) {
		*err = BLOB_ERR_STATE;
		return NULL;
	}
	return s;
}

int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max)
{
	const struct blob_slot *s;

	/* Not checked_slot(): where a slot is does not depend on the device being
	 * up, and `blob info` prints it either way. */
	if (blob_check_map(NULL) != BLOB_MAP_OK)
		return BLOB_ERR_MAP;
	s = blob_map_slot(slot);
	if (s == NULL)
		return BLOB_ERR_PARAM;
	if (base != NULL)
		*base = s->base;
	if (payload_addr != NULL)
		*payload_addr = blob_map_payload_addr(s, BLOB_ERASE_UNIT);
	if (payload_max != NULL)
		*payload_max = blob_map_payload_max(s, BLOB_ERASE_UNIT);
	return BLOB_OK;
}

/* ---- read side ------------------------------------------------------------- */

int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why)
{
	/* The two header pages, on the caller's stack: 512 B against a 4 KB
	 * console stack.  Not static: two consoles can list at once, and nothing
	 * here serialises readers. */
	uint8_t hdr[BLOB_HDR_SPAN];
	const struct blob_slot *s;
	int err = BLOB_OK;

	if (why != NULL)
		*why = BLOB_REJECT_NONE;
	if (out == NULL)
		return BLOB_ERR_PARAM;
	/* Zero FIRST, so every failure path still leaves the caller with a
	 * defined struct: callers stat a slot and print from the result on an
	 * error path. */
	memset(out, 0, sizeof *out);
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	if (nor_read(s->base, hdr, sizeof hdr) != NOR_OK)
		return BLOB_ERR_IO;
	(void)blob_hdr_decode(hdr, sizeof hdr, s->base,
	                      blob_map_payload_max(s, BLOB_ERASE_UNIT), out, why);
	return BLOB_OK;
}

int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len)
{
	const struct blob_slot *s;
	uint32_t cap;
	int err = BLOB_OK;

	if (buf == NULL || len == 0u)
		return BLOB_ERR_PARAM;
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	cap = blob_map_payload_max(s, BLOB_ERASE_UNIT);
	if (off >= cap || len > cap - off)
		return BLOB_ERR_PARAM;
	return (nor_read(blob_map_payload_addr(s, BLOB_ERASE_UNIT) + off, buf,
	                 len) == NOR_OK) ? BLOB_OK : BLOB_ERR_IO;
}

int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed)
{
	/* 256 B at a time, on the caller's stack (cmd_nor.c keeps to the same
	 * page-sized chunk throughout).  Not the write's staging buffer: that one
	 * belongs to whoever holds the busy flag, and a verify does not take it. */
	uint8_t buf[NOR_PAGE_SIZE];
	struct blob_info info;
	uint32_t addr = 0u, off = 0u, crc = 0u;
	int rc;

	if (computed != NULL)
		*computed = 0u;
	rc = blob_stat(slot, &info, NULL);
	if (out != NULL)
		*out = info;
	if (rc != BLOB_OK)
		return rc;
	if (info.state != BLOB_VALID && info.state != BLOB_INCOMPLETE)
		return BLOB_ERR_EMPTY;
	rc = blob_slot_geometry(slot, NULL, &addr, NULL);
	if (rc != BLOB_OK)
		return rc;

	while (off < info.length) {
		uint32_t take = info.length - off;

		if (take > sizeof buf)
			take = sizeof buf;
		if (nor_read(addr + off, buf, take) != NOR_OK)
			return BLOB_ERR_IO;
		crc = crc32_update(crc, buf, take);
		off += take;
	}
	if (computed != NULL)
		*computed = crc;
	return (crc == info.crc32) ? BLOB_OK : BLOB_ERR_CRC;
}

int blob_find(const char *name, unsigned *slot, struct blob_info *out,
              enum blob_lookup *why)
{
	struct blob_slot_view v[BLOB_MAX_SLOTS];
	struct blob_info info, hit;
	unsigned count, i, found = 0u;
	enum blob_lookup l;
	int rc;

	if (why != NULL)
		*why = BLOB_LOOKUP_REFUSE;
	if (name == NULL || slot == NULL || out == NULL)
		return BLOB_ERR_PARAM;
	if (blob_check_map(NULL) != BLOB_MAP_OK)
		return BLOB_ERR_MAP;
	count = blob_map_count();
	/* blob_check_map() has no upper bound on the count; the views array has. */
	if (count > BLOB_MAX_SLOTS)
		return BLOB_ERR_MAP;

	memset(&hit, 0, sizeof hit);
	for (i = 0u; i < count; i++) {
		rc = blob_stat(i, &info, NULL);
		if (rc != BLOB_OK)
			return rc;
		v[i].state = info.state;
		v[i].name_match = (uint8_t)(info.state == BLOB_VALID &&
		                            strcmp(info.name, name) == 0);
		/* The first match is kept; FOUND means it is the only one. */
		if (v[i].name_match && hit.state != BLOB_VALID)
			hit = info;
	}

	l = blob_resolve_name(v, count, &found);
	if (why != NULL)
		*why = l;
	switch (l) {
	case BLOB_LOOKUP_FOUND:
		*slot = found;
		*out = hit;
		return BLOB_OK;
	case BLOB_LOOKUP_NONE:
		return BLOB_ERR_EMPTY;
	case BLOB_LOOKUP_DUPLICATE:
		return BLOB_ERR_DUPLICATE;
	default:
		break;
	}
	return BLOB_ERR_MAP;
}

/* ---- mutating side --------------------------------------------------------- */

/*
 * Serialises blob-mutating operations against each other.  A plain flag
 * test-set under a brief PRIMASK critical section: interrupt-safe,
 * thread-agnostic (acquire and release may run on different threads) and
 * needing no one-time init, which would itself race between the two consoles.
 * Same idiom as the NN session guard in svc/nn/nn.c, and PRIMASK is what this
 * firmware's ThreadX port uses for its own critical sections
 * (TX_PORT_USE_BASEPRI is not defined), so it nests consistently.
 *
 * It guards the CALLERS, not the functions below: `blob write` holds it from
 * before it reads the headers it chooses a slot from until its verify has
 * finished, which is a span only the command knows.
 */
static volatile int blob_busy;

int blob_busy_acquire(void)
{
	uint32_t primask = __get_PRIMASK();
	int ok;

	__disable_irq();
	if (!blob_busy) { blob_busy = 1; ok = 1; } else { ok = 0; }
	__set_PRIMASK(primask);
	return ok ? BLOB_OK : BLOB_ERR_BUSY;
}

void blob_busy_release(void)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	blob_busy = 0;
	__set_PRIMASK(primask);
}

int blob_erase_header(unsigned slot)
{
	const struct blob_slot *s;
	int err = BLOB_OK;

	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	return (nor_erase(s->base, BLOB_ERASE_UNIT) == NOR_OK) ? BLOB_OK
	                                                        : BLOB_ERR_IO;
}

int blob_erase_span(uint32_t addr, uint32_t len, blob_erase_tick_fn tick,
                    void *ctx, uint32_t *done, int *cancelled)
{
	uint32_t off = 0u;

	if (done != NULL)
		*done = 0u;
	if (cancelled != NULL)
		*cancelled = 0;
	/* Inside the interval and whole units, checked by subtraction so a length
	 * near 2^32 is refused rather than wrapped into looking contained.  Nothing
	 * outside the store is reachable through here: the kv partition and the
	 * unallocated top are `nor erase`'s, not this one's. */
	if (len == 0u || addr < BLOB_REGION_BASE || addr >= BLOB_REGION_END ||
	    len > BLOB_REGION_END - addr ||
	    (addr % BLOB_ERASE_UNIT) != 0u || (len % BLOB_ERASE_UNIT) != 0u)
		return BLOB_ERR_PARAM;
	if (!nor_flash_ready())
		return BLOB_ERR_STATE;

	while (off < len) {
		/* Up to the next 64 KB boundary: one block erase where the range
		 * allows it, and nor_erase() falls back to 4 KB sectors itself for
		 * a range that is not whole blocks. */
		uint32_t step = NOR_BLOCK_SIZE - ((addr + off) % NOR_BLOCK_SIZE);

		if (step > len - off)
			step = len - off;
		if (tick != NULL && tick(ctx, off, len) != 0) {
			if (cancelled != NULL)
				*cancelled = 1;
			return BLOB_ERR_CANCEL;
		}
		/* [!] ONE CALL PER BLOCK, so the NOR mutex is given back between
		 * them: nor_erase() holds it for its whole range, and a 1 MB slot
		 * in one call kept the kv store waiting for up to ~32 s. */
		if (nor_erase(addr + off, step) != NOR_OK)
			return BLOB_ERR_IO;
		off += step;
		if (done != NULL)
			*done = off;
	}
	return BLOB_OK;
}
