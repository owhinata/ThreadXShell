/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's store code, src/blob.c (issue #194), compiled
 * unchanged against a 16 MB array that stands in for the QSPI NOR -- the same
 * shape as wio-lite-ai's test/test_blob_store.c.
 *
 * test_blob_slots.c pins the TABLE; the shared rules are tested in
 * shell/test/test_blob_*.c.  What is left is the part of the store that is this
 * board's and that a console cannot stage without spending flash cycles:
 *
 *   1. blob_erase_span(), the erase `blob write` runs: what it refuses before it
 *      erases anything (outside the interval, zero, a wrapping length,
 *      unaligned, reaching into the reserved first megabyte), that it erases
 *      exactly one slot and nothing either side, one 64 KB sector per call (4 KB
 *      subsectors only where a range does not cover a whole sector), and what
 *      it reports when the operator cancels or an erase fails part way;
 *   2. blob_erase_header(), which must touch the header subsector only;
 *   3. blob_find(), the lookup `nn model load <name>` resolves with;
 *   4. the range checks of blob_stat(), blob_read() and blob_verify(), and that
 *      blob_read() never asks the driver for more than BLOB_READ_CHUNK at once;
 *   5. the busy flag, taken and given back with interrupts masked.
 *
 * [!] THE NOR HERE IS AN ARRAY, AND THE CALLS ARE COUNTED.  The two erase
 * stand-ins erase the sector / subsector CONTAINING the address, as the real
 * driver does (port/qspi/qspi_flash.h), so a call on the wrong boundary shows
 * up as bytes erased outside the range.  Whether the real driver erases or
 * reads correctly is port/qspi's business, not this test's; what is tested is
 * what blob.c ASKS it to do.
 */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "blob.h"
#include "blob_map.h"
#include "blob_state.h"
#include "crc32.h"
#include "qspi_flash.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* ---- the stand-ins --------------------------------------------------------- */

uint32_t blob_test_primask;
unsigned blob_test_masked_sections;

#define NOR_SIZE_BYTES   QSPI_FLASH_SIZE
#define NOR_BLOCK_SIZE   QSPI_FLASH_SECTOR_SIZE

static uint8_t nor[NOR_SIZE_BYTES];
static int nor_up = 1;

#define MAX_CALLS 64
static struct { uint32_t addr, len; } erases[MAX_CALLS];
static unsigned erase_calls;
static int erase_fail_at = -1;          /* 0-based call that fails, or -1 */
static uint32_t read_fail_addr = 0xFFFFFFFFu;
static unsigned read_calls;
static uint32_t read_longest;

int qspi_flash_ready(void)
{
	return nor_up;
}

int qspi_flash_read(uint32_t addr, void *buf, uint32_t len)
{
	read_calls++;
	if (len > read_longest)
		read_longest = len;
	if (!nor_up)
		return QSPI_FLASH_ERR_STATE;
	if (addr >= NOR_SIZE_BYTES || len > NOR_SIZE_BYTES - addr)
		return QSPI_FLASH_ERR_PARAM;
	if (read_fail_addr >= addr && read_fail_addr - addr < len)
		return QSPI_FLASH_ERR_HAL;
	memcpy(buf, &nor[addr], len);
	return QSPI_FLASH_OK;
}

/* Both erase the unit CONTAINING @p addr, as the driver does. */
static int erase_unit(uint32_t addr, uint32_t unit)
{
	unsigned call = erase_calls++;
	uint32_t at = addr - (addr % unit);

	if (call < MAX_CALLS) {
		erases[call].addr = at;
		erases[call].len = unit;
	}
	if ((int)call == erase_fail_at)
		return QSPI_FLASH_ERR_FLASH;
	if (!nor_up)
		return QSPI_FLASH_ERR_STATE;
	if (at >= NOR_SIZE_BYTES)
		return QSPI_FLASH_ERR_PARAM;
	memset(&nor[at], 0xFF, unit);
	return QSPI_FLASH_OK;
}

int qspi_flash_erase_sector(uint32_t addr)
{
	return erase_unit(addr, QSPI_FLASH_SECTOR_SIZE);
}

int qspi_flash_erase_subsector(uint32_t addr)
{
	return erase_unit(addr, QSPI_FLASH_SUBSECTOR_SIZE);
}

static void reset_calls(void)
{
	read_calls = 0u;
	read_longest = 0u;
	erase_calls = 0u;
	erase_fail_at = -1;
	read_fail_addr = 0xFFFFFFFFu;
	nor_up = 1;
}

/* ---- helpers --------------------------------------------------------------- */

/* Every byte of [lo, hi) equals @v; reports the first that does not. */
static int span_is(uint32_t lo, uint32_t hi, uint8_t v, uint32_t *bad)
{
	uint32_t a;

	for (a = lo; a < hi; a++) {
		if (nor[a] != v) {
			if (bad)
				*bad = a;
			return 0;
		}
	}
	return 1;
}

static void slot_span(unsigned slot, uint32_t *base, uint32_t *end)
{
	const struct blob_slot *s = blob_map_slot(slot);

	*base = s->base;
	*end = s->base + s->size;
}

/* Lay a header down at @slot the way the writer does: body page, then (when
 * @finished) the magic page.  The payload is whatever the array holds. */
static void put_header(unsigned slot, const char *name, uint32_t length,
                       uint32_t crc, int finished)
{
	const struct blob_slot *s = blob_map_slot(slot);
	uint8_t page[BLOB_HDR_PAGE];
	struct blob_info info;

	memset(&nor[s->base], 0xFF, BLOB_ERASE_UNIT);
	memset(&info, 0, sizeof info);
	memcpy(info.name, name, strlen(name));
	info.length = length;
	info.crc32 = crc;
	CHECK(blob_hdr_encode_body(page, sizeof page, s->base,
	                           blob_map_payload_max(s, BLOB_ERASE_UNIT),
	                           &info) == BLOB_REJECT_NONE,
	      "the body for '%s' would not encode", name);
	memcpy(&nor[s->base + BLOB_HDR_PAGE], page, BLOB_HDR_PAGE);
	if (finished) {
		CHECK(blob_hdr_encode_magic(page, sizeof page) == BLOB_REJECT_NONE,
		      "the magic would not encode");
		memcpy(&nor[s->base], page, BLOB_HDR_PAGE);
	}
}

/* ---- 1. blob_erase_span() -------------------------------------------------- */

static uint32_t cancel_at = 0xFFFFFFFFu;    /* `done` at which the tick stops */
static unsigned ticks;
static uint32_t tick_total;

static int tick(void *ctx, uint32_t done, uint32_t total)
{
	(void)ctx;
	ticks++;
	tick_total = total;
	return done == cancel_at;
}

static void t_erase_refusals(void)
{
	static const struct { uint32_t addr, len; const char *what; } bad[] = {
		{ 0x00000000u, 0x00001000u, "inside the reserved first 1 MB" },
		{ 0x000FF000u, 0x00002000u, "reaching from the reserve into the store" },
		{ 0x00100000u, 0x00000000u, "zero length" },
		{ 0x00100000u, 0xFFFFF000u, "a length that wraps" },
		{ 0x007FF000u, 0x00002000u, "past the interval's end" },
		{ 0x00800000u, 0x00001000u, "at the interval's end" },
		{ 0x00100800u, 0x00001000u, "an unaligned base" },
		{ 0x00100000u, 0x00000800u, "an unaligned length" },
		{ 0xFFFFF000u, 0x00002000u, "a base near 2^32" },
	};
	unsigned i;

	for (i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
		uint32_t done = 99u;
		int cancelled = 99, rc;

		reset_calls();
		ticks = 0u;
		rc = blob_erase_span(bad[i].addr, bad[i].len, tick, NULL, &done,
		                     &cancelled);
		CHECK(rc == BLOB_ERR_PARAM && erase_calls == 0u && ticks == 0u,
		      "%s (0x%08X+0x%X): rc %d, %u erase call(s)", bad[i].what,
		      (unsigned)bad[i].addr, (unsigned)bad[i].len, rc, erase_calls);
		CHECK(done == 0u && cancelled == 0,
		      "%s: done %u / cancelled %d not cleared", bad[i].what,
		      (unsigned)done, cancelled);
	}

	reset_calls();
	nor_up = 0;
	CHECK(blob_erase_span(0x00100000u, 0x00010000u, tick, NULL, NULL, NULL) ==
	      BLOB_ERR_STATE && erase_calls == 0u,
	      "a device that is down was erased");
}

static void t_erase_each_slot(void)
{
	unsigned s, c;

	for (s = 0u; s < blob_map_count(); s++) {
		uint32_t base, end, bad = 0u, done = 0u;
		int cancelled = 0, rc;

		memset(nor, 0x00, sizeof nor);
		slot_span(s, &base, &end);
		reset_calls();
		ticks = 0u;
		cancel_at = 0xFFFFFFFFu;
		rc = blob_erase_span(base, end - base, tick, NULL, &done, &cancelled);
		CHECK(rc == BLOB_OK && done == end - base && !cancelled,
		      "slot %u: rc %d, done %u", s, rc, (unsigned)done);
		CHECK(span_is(base, end, 0xFF, &bad),
		      "slot %u: 0x%06X was not erased", s, (unsigned)bad);
		CHECK(span_is(0u, base, 0x00, &bad),
		      "slot %u: 0x%06X below the slot was erased", s, (unsigned)bad);
		CHECK(span_is(end, NOR_SIZE_BYTES, 0x00, &bad),
		      "slot %u: 0x%06X above the slot was erased", s, (unsigned)bad);
		/* One 64 KB sector per call, each on a sector boundary, and the
		 * tick asked before every one of them. */
		CHECK(erase_calls == (end - base) / NOR_BLOCK_SIZE,
		      "slot %u: %u erase calls for %u blocks", s, erase_calls,
		      (unsigned)((end - base) / NOR_BLOCK_SIZE));
		for (c = 0u; c < erase_calls && c < MAX_CALLS; c++)
			CHECK(erases[c].len == NOR_BLOCK_SIZE &&
			      erases[c].addr == base + c * NOR_BLOCK_SIZE,
			      "slot %u call %u: 0x%06X+0x%X", s, c,
			      (unsigned)erases[c].addr, (unsigned)erases[c].len);
		CHECK(ticks == erase_calls && tick_total == end - base,
		      "slot %u: %u ticks (total %u) for %u calls", s, ticks,
		      (unsigned)tick_total, erase_calls);
	}
}

static void t_erase_cancel_and_fail(void)
{
	uint32_t base = 0x00100000u, len = 0x00100000u, done, bad = 0u;
	int cancelled, rc;

	/* Before the first block: nothing erased, nothing reported erased. */
	memset(nor, 0x00, sizeof nor);
	reset_calls();
	cancel_at = 0u;
	done = 99u;
	cancelled = 0;
	rc = blob_erase_span(base, len, tick, NULL, &done, &cancelled);
	CHECK(rc == BLOB_ERR_CANCEL && cancelled && done == 0u &&
	      erase_calls == 0u,
	      "cancel before the first block: rc %d done %u calls %u", rc,
	      (unsigned)done, erase_calls);
	CHECK(span_is(0u, NOR_SIZE_BYTES, 0x00, &bad),
	      "cancel before the first block erased 0x%06X", (unsigned)bad);

	/* After two blocks: exactly those two. */
	memset(nor, 0x00, sizeof nor);
	reset_calls();
	cancel_at = 2u * NOR_BLOCK_SIZE;
	rc = blob_erase_span(base, len, tick, NULL, &done, &cancelled);
	CHECK(rc == BLOB_ERR_CANCEL && cancelled && done == 2u * NOR_BLOCK_SIZE &&
	      erase_calls == 2u,
	      "cancel after two blocks: rc %d done %u calls %u", rc,
	      (unsigned)done, erase_calls);
	CHECK(span_is(base, base + done, 0xFF, &bad) &&
	      span_is(base + done, NOR_SIZE_BYTES, 0x00, &bad),
	      "cancel after two blocks: 0x%06X is wrong", (unsigned)bad);

	/* The fourth block fails: three reported, nothing after it tried. */
	memset(nor, 0x00, sizeof nor);
	reset_calls();
	cancel_at = 0xFFFFFFFFu;
	erase_fail_at = 3;
	rc = blob_erase_span(base, len, tick, NULL, &done, &cancelled);
	CHECK(rc == BLOB_ERR_IO && !cancelled && done == 3u * NOR_BLOCK_SIZE &&
	      erase_calls == 4u,
	      "a failing fourth block: rc %d done %u calls %u", rc,
	      (unsigned)done, erase_calls);
	CHECK(span_is(base + 4u * NOR_BLOCK_SIZE, NOR_SIZE_BYTES, 0x00, &bad),
	      "an erase went on past a failed block (0x%06X)", (unsigned)bad);
}

/* A range that does not start on a sector boundary: 4 KB subsectors up to the
 * boundary, then whole sectors, and nothing outside it either way. */
static void t_erase_partial(void)
{
	uint32_t base = 0x00101000u, len = 0x0001F000u, done = 0u, bad = 0u, c;
	int cancelled = 0, rc;

	memset(nor, 0x00, sizeof nor);
	reset_calls();
	cancel_at = 0xFFFFFFFFu;
	rc = blob_erase_span(base, len, tick, NULL, &done, &cancelled);
	CHECK(rc == BLOB_OK && done == len && erase_calls == 16u,
	      "a part-sector range: rc %d done %u calls %u", rc, (unsigned)done,
	      erase_calls);
	for (c = 0u; c < 15u && c < erase_calls; c++)
		CHECK(erases[c].len == QSPI_FLASH_SUBSECTOR_SIZE &&
		      erases[c].addr == base + c * QSPI_FLASH_SUBSECTOR_SIZE,
		      "part-sector call %u: 0x%06X+0x%X", (unsigned)c,
		      (unsigned)erases[c].addr, (unsigned)erases[c].len);
	CHECK(erase_calls < 16u || (erases[15].addr == 0x00110000u &&
	      erases[15].len == QSPI_FLASH_SECTOR_SIZE),
	      "the whole sector after the boundary was not one sector erase");
	CHECK(span_is(base, base + len, 0xFF, &bad) &&
	      span_is(0u, base, 0x00, &bad) &&
	      span_is(base + len, NOR_SIZE_BYTES, 0x00, &bad),
	      "a part-sector range: 0x%06X is wrong", (unsigned)bad);

	/* A range shorter than a sector, starting on a boundary: subsectors only
	 * -- a sector erase there would take the rest of the sector with it. */
	memset(nor, 0x00, sizeof nor);
	reset_calls();
	rc = blob_erase_span(0x00110000u, 0x2000u, tick, NULL, &done, &cancelled);
	CHECK(rc == BLOB_OK && erase_calls == 2u &&
	      span_is(0x00110000u, 0x00112000u, 0xFF, &bad) &&
	      span_is(0x00112000u, NOR_SIZE_BYTES, 0x00, &bad) &&
	      span_is(0u, 0x00110000u, 0x00, &bad),
	      "a short range on a sector boundary: rc %d calls %u bad 0x%06X",
	      rc, erase_calls, (unsigned)bad);
}

/* ---- 2. blob_erase_header() ------------------------------------------------ */

static void t_erase_header(void)
{
	unsigned s;

	CHECK(blob_erase_header(blob_map_count()) == BLOB_ERR_PARAM,
	      "a slot past the table had its header erased");
	for (s = 0u; s < blob_map_count(); s++) {
		uint32_t base, end, bad = 0u;

		memset(nor, 0x00, sizeof nor);
		slot_span(s, &base, &end);
		reset_calls();
		CHECK(blob_erase_header(s) == BLOB_OK && erase_calls == 1u,
		      "slot %u: header erase failed or took %u calls", s,
		      erase_calls);
		CHECK(span_is(base, base + BLOB_ERASE_UNIT, 0xFF, &bad) &&
		      span_is(0u, base, 0x00, &bad) &&
		      span_is(base + BLOB_ERASE_UNIT, NOR_SIZE_BYTES, 0x00, &bad),
		      "slot %u: header erase touched 0x%06X wrongly", s,
		      (unsigned)bad);
	}
}

/* ---- 3. blob_find() -------------------------------------------------------- */

static void t_find(void)
{
	struct blob_info info;
	enum blob_lookup why;
	unsigned slot;
	int rc;

	memset(nor, 0xFF, sizeof nor);
	reset_calls();

	/* Nothing stored. */
	slot = 99u;
	rc = blob_find("model", &slot, &info, &why);
	CHECK(rc == BLOB_ERR_EMPTY && why == BLOB_LOOKUP_NONE && slot == 99u,
	      "an empty store: rc %d why %d", rc, (int)why);

	/* One VALID match. */
	put_header(5u, "model", 1000u, 0x12345678u, 1);
	put_header(9u, "other", 2000u, 0x9ABCDEF0u, 1);
	rc = blob_find("model", &slot, &info, &why);
	CHECK(rc == BLOB_OK && why == BLOB_LOOKUP_FOUND && slot == 5u &&
	      info.state == BLOB_VALID && info.length == 1000u &&
	      info.crc32 == 0x12345678u && strcmp(info.name, "model") == 0,
	      "one match: rc %d slot %u len %u", rc, slot,
	      (unsigned)info.length);

	/* A name in an INCOMPLETE slot is not found, nor in an INVALID one. */
	put_header(0u, "half", 1000u, 1u, 0);
	rc = blob_find("half", &slot, &info, &why);
	CHECK(rc == BLOB_ERR_EMPTY && why == BLOB_LOOKUP_NONE,
	      "a name in an incomplete slot resolved: rc %d", rc);
	put_header(1u, "broken", 1000u, 1u, 1);
	nor[blob_map_slot(1u)->base + BLOB_HDR_PAGE + BLOB_BODY_OFF_BODYCRC] ^= 1u;
	rc = blob_find("broken", &slot, &info, &why);
	CHECK(rc == BLOB_ERR_EMPTY && why == BLOB_LOOKUP_NONE,
	      "a name in an invalid slot resolved: rc %d", rc);

	/* Two VALID slots under one name: its own answer, not "none". */
	put_header(12u, "model", 3000u, 0x0u, 1);
	slot = 99u;
	rc = blob_find("model", &slot, &info, &why);
	CHECK(rc == BLOB_ERR_DUPLICATE && why == BLOB_LOOKUP_DUPLICATE &&
	      slot == 99u,
	      "a duplicate: rc %d why %d slot %u", rc, (int)why, slot);

	/* A slot that cannot be read is a failure, not "none" -- even when the
	 * name sits in a slot the scan would have reached later. */
	memset(nor, 0xFF, sizeof nor);
	put_header(11u, "late", 1000u, 1u, 1);
	read_fail_addr = blob_map_slot(7u)->base;
	rc = blob_find("late", &slot, &info, &why);
	CHECK(rc == BLOB_ERR_IO, "a failed read answered %d", rc);
	rc = blob_find("absent", &slot, &info, &why);
	CHECK(rc == BLOB_ERR_IO, "a failed read on a missing name answered %d",
	      rc);
	read_fail_addr = 0xFFFFFFFFu;
	rc = blob_find("late", &slot, &info, &why);
	CHECK(rc == BLOB_OK && slot == 11u, "the same store, readable: rc %d", rc);

	CHECK(blob_find(NULL, &slot, &info, &why) == BLOB_ERR_PARAM &&
	      blob_find("x", NULL, &info, &why) == BLOB_ERR_PARAM &&
	      blob_find("x", &slot, NULL, &why) == BLOB_ERR_PARAM,
	      "a NULL argument was not refused");
	nor_up = 0;
	CHECK(blob_find("late", &slot, &info, &why) == BLOB_ERR_STATE,
	      "a device that is down was searched");
	nor_up = 1;
}

/* ---- 4. the range checks of the reads -------------------------------------- */

static void t_reads(void)
{
	const unsigned n = blob_map_count();
	const struct blob_slot *s = blob_map_slot(3u);
	uint32_t pa = blob_map_payload_addr(s, BLOB_ERASE_UNIT);
	uint32_t pm = blob_map_payload_max(s, BLOB_ERASE_UNIT);
	struct blob_info info;
	uint8_t b[4];
	uint32_t computed = 0u, i;

	memset(nor, 0xFF, sizeof nor);
	reset_calls();

	CHECK(blob_stat(n, &info, NULL) == BLOB_ERR_PARAM,
	      "blob_stat took a slot past the table");
	CHECK(blob_stat(0u, NULL, NULL) == BLOB_ERR_PARAM,
	      "blob_stat took a NULL out");
	nor_up = 0;
	CHECK(blob_stat(0u, &info, NULL) == BLOB_ERR_STATE,
	      "blob_stat read a device that is down");
	nor_up = 1;

	CHECK(blob_read(n, 0u, b, 1u) == BLOB_ERR_PARAM,
	      "blob_read took a slot past the table");
	CHECK(blob_read(3u, 0u, NULL, 1u) == BLOB_ERR_PARAM &&
	      blob_read(3u, 0u, b, 0u) == BLOB_ERR_PARAM,
	      "blob_read took a NULL buffer or a zero length");
	CHECK(blob_read(3u, pm, b, 1u) == BLOB_ERR_PARAM,
	      "blob_read started at the end of the payload area");
	CHECK(blob_read(3u, pm - 2u, b, 4u) == BLOB_ERR_PARAM,
	      "blob_read ran past the end of the payload area");
	CHECK(blob_read(3u, 0u, b, 0xFFFFFFFFu) == BLOB_ERR_PARAM,
	      "blob_read took a wrapping length");
	nor[pa + pm - 1u] = 0x5Au;
	CHECK(blob_read(3u, pm - 1u, b, 1u) == BLOB_OK && b[0] == 0x5Au,
	      "the last payload byte did not read from 0x%06X",
	      (unsigned)(pa + pm - 1u));

	/* [!] A WHOLE 1 MB PAYLOAD IS READ IN PIECES OF AT MOST BLOB_READ_CHUNK,
	 * and the bytes still come back in order (blob.h says why: the HAL's
	 * receive timeout covers a whole call). */
	{
		static uint8_t big[1044480u];
		const struct blob_slot *s0 = blob_map_slot(0u);
		uint32_t pa0 = blob_map_payload_addr(s0, BLOB_ERASE_UNIT);
		uint32_t pm0 = blob_map_payload_max(s0, BLOB_ERASE_UNIT), k, wrong = 0u;

		for (k = 0u; k < pm0; k++)
			nor[pa0 + k] = (uint8_t)(k * 2654435761u >> 24);
		reset_calls();
		CHECK(pm0 == sizeof big, "the 1 MB slot's payload is %u B",
		      (unsigned)pm0);
		CHECK(blob_read(0u, 0u, big, pm0) == BLOB_OK,
		      "a whole-payload read failed");
		CHECK(read_longest <= BLOB_READ_CHUNK &&
		      read_calls == (pm0 + BLOB_READ_CHUNK - 1u) / BLOB_READ_CHUNK,
		      "a whole-payload read: %u call(s), the longest %u B",
		      read_calls, (unsigned)read_longest);
		for (k = 0u; k < pm0; k++)
			if (big[k] != nor[pa0 + k])
				wrong++;
		CHECK(wrong == 0u, "%u byte(s) of a whole-payload read are wrong",
		      (unsigned)wrong);
		read_fail_addr = pa0 + 5u * BLOB_READ_CHUNK + 7u;
		CHECK(blob_read(0u, 0u, big, pm0) == BLOB_ERR_IO,
		      "a piece that failed part way did not fail the read");
		read_fail_addr = 0xFFFFFFFFu;
		memset(nor, 0xFF, sizeof nor);
	}

	/* verify: an empty slot has nothing to check against; a payload that
	 * matches passes, one flipped byte fails. */
	CHECK(blob_verify(n, &info, &computed) == BLOB_ERR_PARAM,
	      "blob_verify took a slot past the table");
	CHECK(blob_verify(3u, &info, &computed) == BLOB_ERR_EMPTY,
	      "blob_verify passed an empty slot");
	for (i = 0u; i < 3000u; i++)
		nor[pa + i] = (uint8_t)(i * 7u);
	put_header(3u, "data", 3000u, crc32_update(0u, &nor[pa], 3000u), 1);
	CHECK(blob_verify(3u, &info, &computed) == BLOB_OK &&
	      computed == info.crc32 && info.length == 3000u,
	      "a matching payload did not verify");
	nor[pa + 2999u] ^= 0x01u;
	CHECK(blob_verify(3u, &info, &computed) == BLOB_ERR_CRC &&
	      computed != info.crc32,
	      "a flipped last byte verified");
}

/* ---- 5. the busy flag ------------------------------------------------------ */

static void t_busy(void)
{
	unsigned before = blob_test_masked_sections;

	blob_test_primask = 0u;
	CHECK(blob_busy_acquire() == BLOB_OK, "the free flag was refused");
	CHECK(blob_busy_acquire() == BLOB_ERR_BUSY, "the flag was taken twice");
	blob_busy_release();
	CHECK(blob_busy_acquire() == BLOB_OK, "the released flag was refused");
	blob_busy_release();
	CHECK(blob_test_masked_sections - before == 5u,
	      "%u masked sections for 5 flag operations",
	      blob_test_masked_sections - before);
	CHECK(blob_test_primask == 0u, "the interrupt mask was not restored");
	/* Called with interrupts already masked, it leaves them masked. */
	blob_test_primask = 1u;
	CHECK(blob_busy_acquire() == BLOB_OK && blob_test_primask == 1u,
	      "an outer mask was dropped");
	blob_busy_release();
	CHECK(blob_test_primask == 1u, "an outer mask was dropped on release");
	blob_test_primask = 0u;
}

int main(void)
{
	t_erase_refusals();
	t_erase_each_slot();
	t_erase_cancel_and_fail();
	t_erase_partial();
	t_erase_header();
	t_find();
	t_reads();
	t_busy();

	if (failures != 0) {
		printf("test_blob_store: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_store: all passed\n");
	return 0;
}
