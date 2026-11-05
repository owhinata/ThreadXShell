/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: this board's whole store -- src/blob.c, src/blob_writer.c and the
 * table -- under the shared write coordinator (svc/blob/blob_write.c), end to
 * end over a 16 MB array that stands in for the QSPI NOR (issue #194).
 *
 * test_blob_store.c tests src/blob.c's pieces one by one; this one runs a
 * `blob write` through them the way shell/cmds/cmd_blob.c does, with the same
 * operations wired to the same svc/blob/blob_svc.h functions, and asks what
 * the store then says:
 *
 *   - a write that completes leaves a slot that blob_stat() calls VALID under
 *     the name, and blob_verify() passes;
 *   - a read-back that disagrees leaves it INCOMPLETE (no magic, #194);
 *   - an erase cancelled part way leaves no header and erased only what it
 *     reported;
 *   - the busy flag is taken once and given back once, every time, and while
 *     it is held the public reads answer BUSY (`qspi test` holds it too) and
 *     the reservation is refused;
 *   - the writer never programs or reads back outside the store's interval.
 *
 * The console pieces are stand-ins: the receive drives the coordinator's sink
 * with a known stream, as the protocol would.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blob.h"
#include "blob_svc.h"
#include "blob_write.h"
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

static uint8_t nor[QSPI_FLASH_SIZE];
static int corrupt_payload_read;     /* flip a byte in a payload read-back */
static unsigned writes_outside, logs;

/* What to break (issue #194 4d): a program of one kind of page, the sector
 * erase, or a payload read.  A failed program writes nothing. */
enum fail_kind { FAIL_NONE, FAIL_PAYLOAD, FAIL_BODY, FAIL_MAGIC, FAIL_ERASE,
                 FAIL_READ };
static enum fail_kind fail;

/* Which page a program aims at: a slot's base is the magic page, one header
 * page in is the body, anything further is payload. */
static enum fail_kind page_kind(uint32_t addr)
{
	int slot = blob_map_index_of(addr - (addr % BLOB_ERASE_UNIT));

	if (slot >= 0 && addr == blob_map_slot((unsigned)slot)->base)
		return FAIL_MAGIC;
	if (slot >= 0 && addr == blob_map_slot((unsigned)slot)->base +
	    BLOB_HDR_PAGE)
		return FAIL_BODY;
	return FAIL_PAYLOAD;
}

int qspi_flash_ready(void) { return 1; }

int qspi_flash_read(uint32_t addr, void *buf, uint32_t len)
{
	if (addr >= QSPI_FLASH_SIZE || len > QSPI_FLASH_SIZE - addr)
		return QSPI_FLASH_ERR_PARAM;
	if (fail == FAIL_READ && blob_map_index_of(addr) < 0)
		return QSPI_FLASH_ERR_HAL;
	memcpy(buf, &nor[addr], len);
	/* A payload read (not a header read, which is at a slot base). */
	if (corrupt_payload_read && blob_map_index_of(addr) < 0 && len > 0u)
		((uint8_t *)buf)[0] ^= 0x01u;
	return QSPI_FLASH_OK;
}

int qspi_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	uint32_t i;

	if (addr < BLOB_REGION_BASE || addr + len > BLOB_REGION_END)
		writes_outside++;
	if (addr >= QSPI_FLASH_SIZE || len > QSPI_FLASH_SIZE - addr)
		return QSPI_FLASH_ERR_PARAM;
	if (fail != FAIL_NONE && fail == page_kind(addr))
		return QSPI_FLASH_ERR_FLASH;     /* P_ERR, nothing written */
	for (i = 0u; i < len; i++)
		nor[addr + i] &= ((const uint8_t *)buf)[i];   /* NOR: 1 -> 0 only */
	return QSPI_FLASH_OK;
}

static int erase_unit(uint32_t addr, uint32_t unit)
{
	uint32_t at = addr - (addr % unit);

	if (fail == FAIL_ERASE && unit == QSPI_FLASH_SECTOR_SIZE)
		return QSPI_FLASH_ERR_FLASH;
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

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level;
	(void)tag;
	(void)fmt;
	logs++;
}

/* ---- the run, wired as shell/cmds/cmd_blob.c wires it ---------------------- */

struct run_ctx {
	uint32_t send;
	unsigned cancel_after;     /* ticks before the erase is cancelled; 0 never */
	unsigned ticks;
	unsigned reserves, unreserves;
	int busy_seen_in_receive;  /* the flag was held while the file arrived */
};

static struct run_ctx rc;

static int tick(void *ctx, uint32_t done, uint32_t total)
{
	(void)ctx;
	(void)done;
	(void)total;
	rc.ticks++;
	return rc.cancel_after != 0u && rc.ticks > rc.cancel_after;
}

static int op_reserve(void *ctx, uint32_t *token)
{
	(void)ctx;
	rc.reserves++;
	return blob_svc_reserve(token) == BLOB_OK ? 0 : -1;
}

static void op_unreserve(void *ctx, uint32_t token)
{
	(void)ctx;
	rc.unreserves++;
	blob_svc_unreserve(token);
}

static int op_erase(void *ctx, uint32_t token, uint32_t addr, uint32_t len,
                    uint32_t *done, int *cancelled)
{
	return blob_svc_erase_span(token, addr, len, tick, ctx, done, cancelled);
}

static int op_program(void *ctx, uint32_t token, uint32_t addr,
                      const void *data, uint32_t len)
{
	(void)ctx;
	return blob_svc_program(token, addr, data, len);
}

static int op_ok(void *ctx) { (void)ctx; return 0; }
static void op_nothing(void *ctx) { (void)ctx; }

static int op_receive(void *ctx, const struct ym_sink *sink)
{
	static uint8_t block[1024];
	struct blob_info info;
	uint32_t off;

	(void)ctx;
	/* [!] While the file arrives the reservation is out: the public reads
	 * answer BUSY, and a second reservation is refused. */
	rc.busy_seen_in_receive =
		blob_stat(0u, &info, NULL) == BLOB_ERR_BUSY &&
		blob_busy_acquire() == BLOB_ERR_BUSY;
	if (sink->begin(sink->ctx, "f.bin", rc.send) != 0)
		return 1;
	for (off = 0u; off < rc.send; off += sizeof block) {
		uint32_t take = rc.send - off, i;

		if (take > sizeof block)
			take = sizeof block;
		for (i = 0u; i < take; i++)
			block[i] = (uint8_t)((off + i) * 2654435761u >> 24);
		if (sink->write(sink->ctx, block, take) != 0)
			return 1;
	}
	return 0;
}

static int op_read_back(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	(void)ctx;
	return blob_svc_read_back(addr, buf, len);
}

static unsigned op_count(void *ctx) { (void)ctx; return blob_map_count(); }

static int op_stat(void *ctx, unsigned slot, struct blob_info *info)
{
	(void)ctx;
	return blob_svc_stat_reserved(slot, info) == BLOB_OK ? 0 : -1;
}

static int op_geometry(void *ctx, unsigned slot, uint32_t *base,
                       uint32_t *pa, uint32_t *pm)
{
	(void)ctx;
	return blob_slot_geometry(slot, base, pa, pm) == BLOB_OK ? 0 : -1;
}

static const struct blob_write_ops OPS = {
	&rc, op_reserve, op_unreserve, op_erase, op_program,
	op_ok, op_ok, op_nothing, op_receive, op_read_back,
	NULL, NULL, op_count, op_stat, op_geometry,
};

static enum blob_write_result write_one(const char *name, int slot,
                                        uint32_t send,
                                        struct blob_write_report *rep)
{
	uint8_t *stage;
	uint32_t stage_len = 0u;

	memset(&rc, 0, sizeof rc);
	rc.send = send;
	stage = blob_svc_stage(&stage_len);
	return blob_write_run(&OPS, stage, stage_len, name, slot, rep);
}

static void check_released(const char *what)
{
	CHECK(rc.reserves == 1u && rc.unreserves == 1u,
	      "%s: %u reserve(s), %u unreserve(s)", what, rc.reserves,
	      rc.unreserves);
	CHECK(blob_busy_acquire() == BLOB_OK, "%s: the flag was not given back",
	      what);
	blob_busy_release();
}

/* ---- the cases ------------------------------------------------------------- */

static void t_write_valid(void)
{
	struct blob_write_report rep;
	struct blob_info info;
	uint32_t computed = 0u, stage_len = 0u;
	enum blob_write_result res;

	/* Every header erased (the slot is free), slot 12's payload area and the
	 * last byte of slot 11 not: the write has to erase what it uses, and only
	 * that. */
	memset(nor, 0xFF, sizeof nor);
	memset(&nor[blob_map_slot(12u)->base + BLOB_ERASE_UNIT], 0x00,
	       blob_map_slot(12u)->size - BLOB_ERASE_UNIT);
	nor[blob_map_slot(12u)->base - 1u] = 0x00u;
	res = write_one("model", 12, 5000u, &rep);
	CHECK(res == BLOB_WRITE_STORED, "a write ended %s",
	      blob_write_result_name(res));
	CHECK(rc.busy_seen_in_receive,
	      "the flag was not held, or the reads did not refuse, mid-transfer");
	check_released("stored");
	CHECK(blob_stat(12u, &info, NULL) == BLOB_OK && info.state == BLOB_VALID &&
	      strcmp(info.name, "model") == 0 && info.length == 5000u &&
	      info.crc32 == rep.crc, "the stored slot reads %s",
	      blob_slot_state_name(info.state));
	CHECK(blob_verify(12u, &info, &computed) == BLOB_OK &&
	      computed == rep.crc, "the stored slot does not verify");
	CHECK(writes_outside == 0u, "%u program(s) outside the store",
	      writes_outside);
	CHECK(blob_svc_stage(&stage_len) != NULL && stage_len == 1024u,
	      "the staging buffer is %u B", (unsigned)stage_len);
	/* Slot 11, just below, was not touched by slot 12's erase. */
	CHECK(nor[blob_map_slot(12u)->base - 1u] == 0x00u,
	      "the erase reached below slot 12");
}

static void t_write_verify_fails(void)
{
	struct blob_write_report rep;
	struct blob_info info;
	enum blob_write_result res;

	memset(nor, 0xFF, sizeof nor);
	corrupt_payload_read = 1;
	res = write_one("model", 9, 3000u, &rep);
	corrupt_payload_read = 0;
	CHECK(res == BLOB_WRITE_VERIFY_FAILED, "a bad read-back ended %s",
	      blob_write_result_name(res));
	check_released("verify failed");
	CHECK(blob_stat(9u, &info, NULL) == BLOB_OK &&
	      info.state == BLOB_INCOMPLETE && strcmp(info.name, "model") == 0,
	      "after a failed verify the slot reads %s",
	      blob_slot_state_name(info.state));
}

static void t_write_cancelled(void)
{
	struct blob_write_report rep;
	struct blob_info info;
	const struct blob_slot *s = blob_map_slot(0u);
	enum blob_write_result res;
	uint32_t a, bad = 0u;

	memset(nor, 0xFF, sizeof nor);
	memset(&nor[s->base + BLOB_ERASE_UNIT], 0x00, s->size - BLOB_ERASE_UNIT);
	memset(&rc, 0, sizeof rc);
	rc.send = 3000u;
	rc.cancel_after = 3u;       /* three 64 KB sectors, then Ctrl+C */
	{
		uint32_t stage_len = 0u;
		uint8_t *stage = blob_svc_stage(&stage_len);

		res = blob_write_run(&OPS, stage, stage_len, "model", 0, &rep);
	}
	CHECK(res == BLOB_WRITE_CANCELLED, "a cancelled erase ended %s",
	      blob_write_result_name(res));
	CHECK(rep.erased == 3u * QSPI_FLASH_SECTOR_SIZE, "%lu B reported erased",
	      (unsigned long)rep.erased);
	check_released("cancelled");
	for (a = s->base + rep.erased; a < s->base + s->size && a >= s->base +
	     BLOB_ERASE_UNIT; a++)
		if (nor[a] != 0x00u) {
			bad = a;
			break;
		}
	CHECK(bad == 0u, "0x%06X past what was reported was erased",
	      (unsigned)bad);
	CHECK(blob_stat(0u, &info, NULL) == BLOB_OK && info.state == BLOB_EMPTY,
	      "a cancelled write left a header (%s)",
	      blob_slot_state_name(info.state));
}

static void t_busy(void)
{
	struct blob_write_report rep;
	struct blob_info info;
	uint32_t token = 0u, computed;
	uint8_t b[4];

	/* `qspi test` holds the flag: every public read and the reservation are
	 * refused BUSY; the reserved stat still reads. */
	memset(nor, 0xFF, sizeof nor);
	CHECK(blob_busy_acquire() == BLOB_OK, "the flag was not free");
	CHECK(blob_stat(1u, &info, NULL) == BLOB_ERR_BUSY &&
	      blob_read(1u, 0u, b, sizeof b) == BLOB_ERR_BUSY &&
	      blob_verify(1u, &info, &computed) == BLOB_ERR_BUSY,
	      "a read went ahead while the flag was held");
	CHECK(blob_stat_reserved(1u, &info) == BLOB_OK &&
	      info.state == BLOB_EMPTY, "the reserved stat did not read");
	CHECK(blob_svc_reserve(&token) == BLOB_ERR_BUSY && token == 0u,
	      "a second reservation was granted");
	CHECK(write_one("model", 1, 100u, &rep) == BLOB_WRITE_BUSY &&
	      rc.unreserves == 0u, "a write went ahead while the flag was held");
	blob_busy_release();
}

static void t_writer_bounds(void)
{
	uint8_t b[4] = { 0, 0, 0, 0 };

	memset(nor, 0xFF, sizeof nor);
	writes_outside = 0u;
	CHECK(blob_svc_program(1u, BLOB_REGION_BASE - 4u, b, 4u) != 0 &&
	      blob_svc_program(1u, BLOB_REGION_END - 2u, b, 4u) != 0 &&
	      blob_svc_program(1u, BLOB_REGION_BASE, b, 0xFFFFFFFFu) != 0 &&
	      blob_svc_program(1u, BLOB_REGION_BASE, b, 0u) != 0,
	      "a program outside the store was accepted");
	CHECK(writes_outside == 0u && nor[BLOB_REGION_BASE - 4u] == 0xFFu,
	      "a refused program still reached the part");
	CHECK(blob_svc_read_back(BLOB_REGION_END - 2u, b, 4u) != 0,
	      "a read-back past the store was accepted");
	CHECK(blob_svc_program(1u, BLOB_REGION_BASE, b, 4u) == 0 &&
	      nor[BLOB_REGION_BASE] == 0x00u, "a program inside was refused");
}

static void t_erase_header(void)
{
	struct blob_write_report rep;
	struct blob_info info;
	uint32_t token = 0u;

	memset(nor, 0xFF, sizeof nor);
	CHECK(write_one("x", 4, 100u, &rep) == BLOB_WRITE_STORED, "set-up");
	CHECK(blob_svc_reserve(&token) == BLOB_OK, "the reservation was refused");
	CHECK(blob_svc_erase_header(token, 4u) == BLOB_OK, "the erase failed");
	blob_svc_unreserve(token);
	CHECK(blob_stat(4u, &info, NULL) == BLOB_OK && info.state == BLOB_EMPTY,
	      "the header survived its erase");
}

/* [!] EVERY WAY A STEP OF THE WRITE CAN FAIL, through the real writer: the
 * result, whether the magic is the one that failed, what the slot then reads
 * as, and the flag given back once.  A writer that swallowed a failed program
 * or erase would pass every case above and none of these (issue #194 4d). */
static void t_write_failures(void)
{
	static const struct {
		enum fail_kind f; enum blob_write_result res; int magic;
		enum blob_slot_state st; const char *what;
	} c[] = {
		{ FAIL_PAYLOAD, BLOB_WRITE_PROG_FAILED,   0, BLOB_EMPTY,
		  "a payload page program fails" },
		{ FAIL_BODY,    BLOB_WRITE_PROG_FAILED,   0, BLOB_EMPTY,
		  "the header body program fails" },
		{ FAIL_MAGIC,   BLOB_WRITE_PROG_FAILED,   1, BLOB_INCOMPLETE,
		  "the magic page program fails" },
		{ FAIL_ERASE,   BLOB_WRITE_ERASE_FAILED,  0, BLOB_EMPTY,
		  "the slot erase fails" },
		{ FAIL_READ,    BLOB_WRITE_VERIFY_FAILED, 0, BLOB_INCOMPLETE,
		  "the read-back itself fails" },
	};
	unsigned i;

	for (i = 0u; i < sizeof c / sizeof c[0]; i++) {
		struct blob_write_report rep;
		struct blob_info info;
		enum blob_write_result res;

		memset(nor, 0xFF, sizeof nor);
		fail = c[i].f;
		res = write_one("model", 7, 3000u, &rep);
		fail = FAIL_NONE;
		CHECK(res == c[i].res, "%s: the write ended %s", c[i].what,
		      blob_write_result_name(res));
		CHECK(rep.magic_failed == c[i].magic, "%s: magic_failed is %d",
		      c[i].what, rep.magic_failed);
		CHECK(blob_stat(7u, &info, NULL) == BLOB_OK && info.state == c[i].st,
		      "%s: the slot reads %s, expected %s", c[i].what,
		      blob_slot_state_name(info.state),
		      blob_slot_state_name(c[i].st));
		check_released(c[i].what);
	}
}

int main(void)
{
	t_write_valid();
	t_write_verify_fails();
	t_write_cancelled();
	t_busy();
	t_writer_bounds();
	t_erase_header();
	t_write_failures();

	if (failures != 0) {
		printf("test_blob_writer: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_writer: all passed\n");
	return 0;
}
