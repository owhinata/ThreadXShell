/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The stub store behind test_blob_cmd.c (issue #194).  See blob_svc_stub.h.
 *
 * The write path is REAL: shell/cmds/cmd_blob.c drives svc/blob/blob_write.c,
 * which drives these through the vtable the command builds -- so a write's
 * output is the output of the coordinator's actual outcome, not of a result
 * the test made up.  The pretend part holds the bytes the coordinator
 * programs, so the read-back compares what it really wrote.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "cli_instance.h"
#include "blob_cmd_board.h"
#include "blob_svc.h"
#include "blob_svc_stub.h"
#include "log.h"
#include "ymodem.h"

struct blob_stub blob_stub;

/* The table: three classes of slot, as the boards ship, so that slot numbers
 * reach two digits.  Bases from 0x100000. */
const struct blob_slot blob_map_slots[STUB_SLOTS] = {
	{ 0x00100000u, 0x00100000u }, { 0x00200000u, 0x00100000u },
	{ 0x00300000u, 0x00100000u }, { 0x00400000u, 0x00080000u },
	{ 0x00480000u, 0x00080000u }, { 0x00500000u, 0x00080000u },
	{ 0x00580000u, 0x00080000u }, { 0x00600000u, 0x00080000u },
	{ 0x00680000u, 0x00080000u }, { 0x00700000u, 0x00040000u },
	{ 0x00740000u, 0x00040000u }, { 0x00780000u, 0x00040000u },
	{ 0x007C0000u, 0x00040000u },
};
const unsigned blob_map_slots_count = STUB_SLOTS;
const struct blob_slot *blob_map_table(void) { return blob_map_slots; }
unsigned blob_map_count(void) { return blob_map_slots_count; }

#define LO    0x00100000u
#define HI    0x00800000u
#define UNIT  0x00001000u

/* The pretend part, for the write: only what a test sends is kept. */
static uint8_t part[HI];
static uint8_t stage[1024];

void blob_stub_reset(void)
{
	unsigned i;

	memset(&blob_stub, 0, sizeof blob_stub);
	for (i = 0u; i < STUB_SLOTS; i++)
		blob_stub.state[i] = BLOB_EMPTY;
	blob_stub.ready = 1;
	blob_stub.send = 3000u;
	memset(part, 0xFF, sizeof part);
	blob_test_log_clear();
}

/* ---- the store ------------------------------------------------------------ */

enum blob_map_verdict blob_check_map(unsigned *bad)
{
	return blob_map_check(LO, HI, UNIT, blob_map_table(), blob_map_count(),
	                      bad);
}

int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max)
{
	const struct blob_slot *s = blob_map_slot(slot);

	if (s == NULL)
		return BLOB_ERR_PARAM;
	if (base)         *base = s->base;
	if (payload_addr) *payload_addr = blob_map_payload_addr(s, UNIT);
	if (payload_max)  *payload_max = blob_map_payload_max(s, UNIT);
	return BLOB_OK;
}

void blob_svc_interval(uint32_t *lo, uint32_t *hi, uint32_t *unit)
{
	*lo = LO;
	*hi = HI;
	*unit = UNIT;
}

int blob_svc_ready(void)
{
	return blob_stub.ready;
}

static void fill(unsigned slot, struct blob_info *out)
{
	memset(out, 0, sizeof *out);
	out->state = blob_stub.state[slot];
	memcpy(out->name, blob_stub.name[slot], sizeof out->name);
	out->length = blob_stub.length[slot];
	out->crc32 = blob_stub.crc[slot];
}

int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why)
{
	if (why)
		*why = BLOB_REJECT_NONE;
	if (slot >= STUB_SLOTS)
		return BLOB_ERR_PARAM;
	if (blob_stub.stat_rc != BLOB_OK)
		return blob_stub.stat_rc;
	fill(slot, out);
	if (out->state == BLOB_INVALID && why)
		*why = BLOB_REJECT_MAGIC;
	return BLOB_OK;
}

int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len)
{
	(void)slot;
	if (blob_stub.read_rc != BLOB_OK)
		return blob_stub.read_rc;
	memset(buf, (int)(off & 0xFFu), len);
	return BLOB_OK;
}

int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed)
{
	if (slot >= STUB_SLOTS)
		return BLOB_ERR_PARAM;
	fill(slot, out);
	*computed = blob_stub.verify_crc;
	if (blob_stub.verify_rc != BLOB_OK)
		return blob_stub.verify_rc;
	return (blob_stub.verify_crc == out->crc32) ? BLOB_OK : BLOB_ERR_CRC;
}

int blob_svc_reserve(uint32_t *token)
{
	blob_stub.reserves++;
	if (blob_stub.reserve_rc != BLOB_OK) {
		*token = 0u;
		return blob_stub.reserve_rc;
	}
	*token = 7u;
	return BLOB_OK;
}

void blob_svc_unreserve(uint32_t token)
{
	(void)token;
	blob_stub.unreserves++;
}

int blob_svc_erase_header(uint32_t token, unsigned slot)
{
	(void)token;
	blob_stub.erase_headers++;
	if (blob_stub.reserves != blob_stub.unreserves + 1u)
		printf("STUB: a header was erased outside the reservation\n");
	if (blob_stub.erase_hdr_rc != BLOB_OK)
		return blob_stub.erase_hdr_rc;
	blob_stub.state[slot] = BLOB_EMPTY;
	return BLOB_OK;
}

int blob_svc_erase_span(uint32_t token, uint32_t addr, uint32_t len,
                        blob_svc_tick_fn tick, void *ctx, uint32_t *done,
                        int *cancelled)
{
	(void)token;
	blob_stub.erase_spans++;
	*cancelled = tick(ctx, 0u, len) ? 1 : 0;
	if (*cancelled) {
		*done = 0u;
		return 1;
	}
	memset(&part[addr], 0xFF, len);
	*done = len;
	return 0;
}

int blob_svc_program(uint32_t token, uint32_t addr, const void *data,
                     uint32_t len)
{
	(void)token;
	blob_stub.programs++;
	/* The magic page is the slot's first page: programmed last, alone. */
	if (blob_stub.fail_magic && blob_map_index_of(addr) >= 0 &&
	    len == BLOB_MAGIC_SIZE)
		return 9;
	memcpy(&part[addr], data, len);
	return 0;
}

int blob_svc_read_back(uint32_t addr, void *buf, uint32_t len)
{
	memcpy(buf, &part[addr], len);
	if (blob_stub.corrupt_read_back && len > 0u)
		((uint8_t *)buf)[0] ^= 1u;
	return 0;
}

int blob_svc_stat_reserved(unsigned slot, struct blob_info *out)
{
	return blob_stat(slot, out, NULL);
}

uint8_t *blob_svc_stage(uint32_t *len)
{
	*len = sizeof stage;
	return stage;
}

/* ---- the board's console pieces ------------------------------------------- */

int blob_board_receive(struct cli_instance *sh, const struct ym_sink *sink)
{
	static uint8_t block[1024];
	uint32_t off;

	blob_stub.receives++;
	sh->rx_dropped += blob_stub.drops;
	if (sink->begin(sink->ctx, "sent.bin", blob_stub.send) != 0)
		return 1;
	for (off = 0u; off < blob_stub.send; off += sizeof block) {
		uint32_t take = blob_stub.send - off, i;

		if (take > sizeof block)
			take = sizeof block;
		for (i = 0u; i < take; i++)
			block[i] = (uint8_t)((off + i) * 2654435761u >> 24);
		if (sink->write(sink->ctx, block, take) != 0)
			return 1;
	}
	return 0;
}

int blob_board_raw_console(struct cli_instance *sh)
{
	if (blob_stub.raw_refuse) {
		cli_error(sh, "test: not on this console\r\n");
		return -1;
	}
	return 0;
}

int blob_board_say(struct cli_instance *sh, int rc)
{
	if (blob_stub.say_rc != 0 && rc == blob_stub.say_rc) {
		blob_stub.says++;
		cli_error(sh, "blob: the board's own sentence for %d\r\n", rc);
		return 1;
	}
	return 0;
}

void blob_board_rx_level_clear(struct cli_instance *sh)
{
	(void)sh;
	blob_stub.level_clears++;
	blob_stub.erases_at_clear = blob_stub.erase_spans;
}

int blob_board_rx_level(struct cli_instance *sh, uint32_t *high,
                        uint32_t *size)
{
	(void)sh;
	blob_stub.level_reads++;
	if (blob_stub.level_rc != 0)
		return blob_stub.level_rc;
	*high = blob_stub.level_high;
	*size = blob_stub.level_size;
	return 0;
}

/* ---- what the firmware's shell and protocol provide ------------------------ */

int cli_console_claim(struct cli_instance *sh)
{
	return sh->fg != NULL ? -2 : 0;
}

void cli_console_release(struct cli_instance *sh)
{
	(void)sh;
}

const struct ym_recv_diag *ymodem_recv_diag(void)
{
	static const struct ym_recv_diag d = { .blocks = 3u, .first_kind = -1,
	                                       .first_seq = -1, .first_nseq = -1 };
	return &d;
}

static char logbuf[4096];
static size_t loglen;

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)level;
	n = snprintf(logbuf + loglen, sizeof logbuf - loglen, "%s: ", tag);
	if (n > 0 && (size_t)n < sizeof logbuf - loglen)
		loglen += (size_t)n;
	va_start(ap, fmt);
	n = vsnprintf(logbuf + loglen, sizeof logbuf - loglen, fmt, ap);
	va_end(ap);
	if (n > 0 && (size_t)n < sizeof logbuf - loglen)
		loglen += (size_t)n;
	if (loglen + 1u < sizeof logbuf)
		logbuf[loglen++] = '\n';
	logbuf[loglen] = '\0';
}

const char *blob_test_log(void)
{
	return logbuf;
}

void blob_test_log_clear(void)
{
	loglen = 0u;
	logbuf[0] = '\0';
}
