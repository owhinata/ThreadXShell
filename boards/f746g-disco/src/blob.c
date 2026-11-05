/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob.c
 * @brief   The asset store's QSPI side (issue #194).
 *
 * Thin on purpose, and the same split as wio-lite-ai's src/blob.c.  The
 * decisions -- what a header says, whether a table is legal, which slot a name
 * lives in -- are svc/blob/, where a host test walks them; what is left here is
 * the part that needs this board: the interval, the QSPI reads and erases, and
 * the busy flag.  See blob.h for the layout and for what is serialised.
 * test/test_blob_store.c compiles this file unchanged against a 16 MB array.
 */
#include "blob.h"

#include <string.h>

#include "crc32.h"           /* crc32_update -- svc/crc32.c */
#include "qspi_flash.h"
#include "stm32f7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

/* The header's page is a fact of the format (svc/blob/blob_state.h), and each of
 * its two pages is programmed exactly once.  That holds only while one header
 * page is one program page of this part, so the build refuses a format and a
 * part that disagree rather than letting either follow the other. */
_Static_assert(BLOB_HDR_PAGE == QSPI_FLASH_PAGE_SIZE,
               "the blob header page is not this part's program page");
_Static_assert(BLOB_HDR_SPAN <= BLOB_ERASE_UNIT,
               "the two header pages must fit in the header subsector");
_Static_assert(BLOB_REGION_END <= QSPI_FLASH_SIZE,
               "the blob region must fit on the device");
_Static_assert((BLOB_REGION_BASE % QSPI_FLASH_SECTOR_SIZE) == 0u &&
               (BLOB_REGION_END % QSPI_FLASH_SECTOR_SIZE) == 0u,
               "the blob region must be whole 64 KB erase sectors");
_Static_assert((QSPI_FLASH_SECTOR_SIZE % BLOB_ERASE_UNIT) == 0u,
               "a sector must be whole header units");

/* ---- the table ------------------------------------------------------------- */

enum blob_map_verdict blob_check_map(unsigned *bad)
{
	return blob_map_check(BLOB_REGION_BASE, BLOB_REGION_END, BLOB_ERASE_UNIT,
	                      blob_map_table(), blob_map_count(), bad);
}

/* The slot, once the table has been checked, the index is in range and the
 * driver is up. */
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
	if (!qspi_flash_ready()) {
		*err = BLOB_ERR_STATE;
		return NULL;
	}
	return s;
}

int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max)
{
	const struct blob_slot *s;

	/* Not checked_slot(): where a slot is does not depend on the driver being
	 * up. */
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

/* [addr, addr + len) in pieces of at most BLOB_READ_CHUNK (blob.h says why). */
static int read_split(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint32_t off = 0u;

	while (off < len) {
		uint32_t take = len - off;

		if (take > BLOB_READ_CHUNK)
			take = BLOB_READ_CHUNK;
		if (qspi_flash_read(addr + off, buf + off, take) != QSPI_FLASH_OK)
			return BLOB_ERR_IO;
		off += take;
	}
	return BLOB_OK;
}

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
	 * defined struct. */
	memset(out, 0, sizeof *out);
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	if (qspi_flash_read(s->base, hdr, sizeof hdr) != QSPI_FLASH_OK)
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
	return read_split(blob_map_payload_addr(s, BLOB_ERASE_UNIT) + off,
	                  (uint8_t *)buf, len);
}

int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed)
{
	/* 256 B at a time, on the caller's stack.  Not a write's staging buffer:
	 * that belongs to whoever holds the busy flag, and a verify does not take
	 * it. */
	uint8_t buf[QSPI_FLASH_PAGE_SIZE];
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
		if (qspi_flash_read(addr + off, buf, take) != QSPI_FLASH_OK)
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
 * A plain flag test-set under a brief PRIMASK critical section:
 * interrupt-safe, thread-agnostic (acquire and release may run on different
 * threads) and needing no one-time init, which would itself race between the
 * two consoles.  PRIMASK is what this firmware's ThreadX port uses for its own
 * critical sections (TX_PORT_USE_BASEPRI is not defined), so it nests
 * consistently.  It guards the CALLERS, not the functions below.
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
	return (qspi_flash_erase_subsector(s->base) == QSPI_FLASH_OK)
	       ? BLOB_OK : BLOB_ERR_IO;
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
	 * near 2^32 is refused rather than wrapped into looking contained.  The
	 * reserved first megabyte and the unallocated top are `qspi erase`'s, not
	 * this one's. */
	if (len == 0u || addr < BLOB_REGION_BASE || addr >= BLOB_REGION_END ||
	    len > BLOB_REGION_END - addr ||
	    (addr % BLOB_ERASE_UNIT) != 0u || (len % BLOB_ERASE_UNIT) != 0u)
		return BLOB_ERR_PARAM;
	if (!qspi_flash_ready())
		return BLOB_ERR_STATE;

	while (off < len) {
		uint32_t at = addr + off;
		int whole = (at % QSPI_FLASH_SECTOR_SIZE) == 0u &&
		            len - off >= QSPI_FLASH_SECTOR_SIZE;
		uint32_t step = whole ? QSPI_FLASH_SECTOR_SIZE : BLOB_ERASE_UNIT;
		int rc;

		if (tick != NULL && tick(ctx, off, len) != 0) {
			if (cancelled != NULL)
				*cancelled = 1;
			return BLOB_ERR_CANCEL;
		}
		/* [!] ONE ERASE PER CALL.  qspi_flash_erase_sector() erases the
		 * sector CONTAINING its address, which is why it is only called on
		 * a sector boundary with a whole sector left to erase: anywhere
		 * else it would reach outside the range. */
		rc = whole ? qspi_flash_erase_sector(at)
		           : qspi_flash_erase_subsector(at);
		if (rc != QSPI_FLASH_OK)
			return BLOB_ERR_IO;
		off += step;
		if (done != NULL)
			*done = off;
	}
	return BLOB_OK;
}
