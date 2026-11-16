/* shared-command: required */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nor.c
 * @brief   The one `nor` command for every board (issue #250, epic #213; the
 *          contract is svc/nor/nor_svc.h).
 *
 *   nor info                 the part, its state and geometry, the board's lines
 *   nor read <addr> [len]    hexdump 1 .. 1024 B (default 64)
 *   nor erase <addr> <len>   erase whole 4 KB sectors                   (danger)
 *   nor write <addr> <hex>   program 1 .. 256 B given in hex            (danger)
 *   nor test <addr>          acceptance test on one blank 4 KB sector   (danger)
 *   ... and the board's own (NOR_SVC_BOARD_SUBCMDS, svc/nor_svc_config.h).
 *
 * Until issue #250 each board had its own command: f746g-disco's `qspi`,
 * wio-lite-ai's and grove-vision-ai-v2's `nor`, with three grammars.
 *
 * WHAT GUARDS erase / write / test, in this order, before anything reaches the
 * device -- and a refusal before the hold reads nothing and writes nothing:
 *   1. the arguments: the address on the device; an erase's address and
 *      length both whole 4 KB sectors (nothing is rounded); the range not
 *      past the end of the device; a write 1 .. 256 bytes;
 *   2. the board's range (nor_svc_board.range): the kv partition on
 *      f746g-disco and wio-lite-ai, the seam's writable interval and the word
 *      a write starts on on grove-vision-ai-v2;
 *   3. the console: the local console's foreground only
 *      (shell/cmds/nor_console.c);
 *   4. the board's hold (nor_svc_board.hold), which also refuses a device
 *      that is down or faulted, and inside which the board's operations ask
 *      their own questions again.
 * Each erase, write and test ends with one line in dmesg (#91).
 *
 * `nor info` and `nor read` take no hold (the boards' reasons are in their
 * READMEs).
 *
 * [!] A SHARED TRANSLATION UNIT OWNS NO MUTABLE STORAGE
 * (cmake/check_no_mutable_storage.py, per board): the working areas are on
 * the caller's stack, and the operations a hold hands over are kept nowhere.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "nor_cmd_core.h"
#include "nor_console.h"
#include "nor_svc.h"
#include "nor_svc_config.h"   /* the board says what it has */

#include "tx_api.h"           /* tx_time_get: the elapsed lines */

#define LOG_TAG "nor"
#include "log.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(NOR_SVC_HAS_DEVICE) || !defined(NOR_SVC_HELP) || \
    !defined(NOR_SVC_BOARD_DECLS) || !defined(NOR_SVC_BOARD_SUBCMDS)
#error "nor_svc_config.h must define NOR_SVC_HAS_DEVICE, NOR_SVC_HELP, NOR_SVC_BOARD_DECLS and NOR_SVC_BOARD_SUBCMDS"
#endif

/* A build without the device (wio-lite-ai with BSP_ENABLE_KV off) compiles
 * this file, so the storage audit still covers it, and registers nothing. */
#if NOR_SVC_HAS_DEVICE

_Static_assert(TX_TIMER_TICKS_PER_SECOND == 1000,
               "cmd_nor.c prints ThreadX ticks of 1 ms as milliseconds");

/* The board's own subcommand handlers (see NOR_SVC_BOARD_SUBCMDS). */
NOR_SVC_BOARD_DECLS

#define NOR_READ_DEFAULT 64u
#define NOR_CHUNK        256u     /* the working area of a read or a test */

/* ---- the words ------------------------------------------------------------ */

static const char *nor_status_word(enum nor_svc_status s)
{
	switch (s) {
	case NOR_SVC_OK:           return "ok";
	case NOR_SVC_BUSY:         return "busy";
	case NOR_SVC_DOWN:         return "down";
	case NOR_SVC_REFUSED:      return "refused";
	case NOR_SVC_NO_TRANSPORT: return "no-transport";
	case NOR_SVC_IO:           return "io";
	case NOR_SVC_INCOMPLETE:   return "incomplete";
	case NOR_SVC_CANCELLED:    return "cancelled";
	case NOR_SVC_FAULTED:      return "faulted";
	default:                   return "?";
	}
}

static const char *nor_why(const struct nor_svc_result *r, const char *dflt)
{
	return (r->why != NULL) ? r->why : dflt;
}

/* A subcommand this board does not have: say so, call nothing. */
static int nor_absent(struct cli_instance *sh, const char *sub, const char *hint)
{
	if (hint == NULL)
		return 0;
	cli_error(sh, "nor: no `nor %s` on this board%s%s\r\n", sub,
	          hint[0] != '\0' ? " -- " : "", hint);
	return 1;
}

/* A device offset, on the device. */
static int nor_arg_addr(struct cli_instance *sh, const char *s, uint32_t *out)
{
	if (cli_parse_u32(s, out) != 0 || *out >= nor_svc_board.size) {
		cli_error(sh, "nor: bad address (0 .. 0x%06lX)\r\n",
		          (unsigned long)(nor_svc_board.size - 1u));
		return -1;
	}
	return 0;
}

/* [addr, addr+len) on the device; len is not 0 and addr is on it, so the
 * subtraction cannot wrap. */
static int nor_arg_fits(struct cli_instance *sh, uint32_t addr, uint32_t len)
{
	if (len <= nor_svc_board.size - addr)
		return 0;
	cli_error(sh, "nor: 0x%06lX + %lu B runs past the end of the device "
	              "(0x%06lX)\r\n", (unsigned long)addr, (unsigned long)len,
	          (unsigned long)nor_svc_board.size);
	return -1;
}

/* ---- info ----------------------------------------------------------------- */

static int nor_info_write(void *ctx, const char *s, size_t len)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (sh == NULL || (s == NULL && len != 0u))
		return -1;
	if (len == 0u)
		return 0;
	return (cli_write(sh, s, len) < 0) ? -1 : (int)len;
}

static int nor_info_common(void *ctx, const struct nor_svc_info *in)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	cli_print(sh, "NOR: %s\r\n", nor_svc_board.part);
	cli_print(sh, "  state   : %s%s%s\r\n", in->state,
	          in->state_note[0] != '\0' ? " -- " : "", in->state_note);
	if (in->jedec_valid)
		cli_print(sh, "  jedec   : %02X %02X %02X", in->jedec[0], in->jedec[1],
		          in->jedec[2]);
	else
		cli_print(sh, "  jedec   : --");
	cli_print(sh, in->jedec_note[0] != '\0' ? " (%s)\r\n" : "%s\r\n",
	          in->jedec_note);
	cli_print(sh, "  size    : %lu MB (offsets 0 .. 0x%06lX)\r\n",
	          (unsigned long)(nor_svc_board.size >> 20),
	          (unsigned long)(nor_svc_board.size - 1u));
	cli_print(sh, "  erase   : %lu B unit\r\n",
	          (unsigned long)nor_svc_board.erase_unit);
	cli_print(sh, "  page    : %lu B\r\n", (unsigned long)nor_svc_board.page);
	return 0;
}

static int cmd_nor_info(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_svc_result r;
	int rc;

	(void)argc;
	(void)argv;
	memset(&r, 0, sizeof r);
	rc = nor_svc_board.info(nor_info_common, nor_info_write, sh, &r);
	if (rc > 0) {
		cli_error(sh, "nor: %s\r\n", nor_why(&r, "nothing could be sampled"));
		return 1;
	}
	return rc < 0 ? 1 : 0;
}

/* ---- read ----------------------------------------------------------------- */

static int cmd_nor_read(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t buf[NOR_CHUNK];
	uint32_t addr, len = NOR_READ_DEFAULT, done = 0u;

	if (nor_absent(sh, "read", nor_svc_board.no_read) || nor_svc_board.read == NULL)
		return 1;
	if (nor_arg_addr(sh, argv[1], &addr))
		return 1;
	if (argc > 2 && (cli_parse_u32(argv[2], &len) != 0 || len == 0u ||
	                 len > NOR_SVC_READ_MAX)) {
		cli_error(sh, "nor: bad length (1 .. %lu)\r\n",
		          (unsigned long)NOR_SVC_READ_MAX);
		return 1;
	}
	if (nor_arg_fits(sh, addr, len))
		return 1;

	while (done < len) {
		struct nor_svc_result r;
		uint32_t chunk = len - done;

		if (chunk > sizeof buf)
			chunk = sizeof buf;
		memset(&r, 0, sizeof r);
		nor_svc_board.read(addr + done, buf, chunk, &r);
		if (r.status != NOR_SVC_OK) {
			cli_error(sh, "nor: read failed at 0x%06lX: %s\r\n",
			          (unsigned long)(addr + done), nor_why(&r, "error"));
			return 1;
		}
		cli_hexdump_base(sh, buf, chunk, addr + done);
		done += chunk;
	}
	return 0;
}

/* ---- the report ----------------------------------------------------------- */

static void nor_log(const char *what, uint32_t addr, uint32_t len,
                    const struct nor_svc_result *r)
{
	unsigned level = (r->status == NOR_SVC_OK) ? LOG_LEVEL_INF
	               : (r->status == NOR_SVC_FAULTED) ? LOG_LEVEL_ERR
	               : LOG_LEVEL_WRN;

	/* The numbers first and the prose last: a record is LOG_MSG_MAX bytes,
	 * and a cut can then only cost the end of the reason. */
	log_write(level, "nor", "%s %s 0x%lx+%lu: %lu/%lu%s%s", what,
	          nor_status_word(r->status), (unsigned long)addr,
	          (unsigned long)len, (unsigned long)r->done,
	          (unsigned long)r->verified, r->why ? " -- " : "",
	          r->why ? r->why : "");
}

int nor_cmd_not_held(struct cli_instance *sh, const char *what, uint32_t addr,
                     uint32_t len, const struct nor_svc_result *r)
{
	nor_log(what, addr, len, r);
	if (r->status == NOR_SVC_BUSY)
		cli_error(sh, "nor: busy -- %s\r\n", nor_why(r, "try again"));
	else
		cli_error(sh, "nor: %s\r\n", nor_why(r, "device down (see `nor info`)"));
	return 1;
}

int nor_cmd_report(struct cli_instance *sh, const char *what, uint32_t addr,
                   uint32_t len, const struct nor_svc_result *r)
{
	nor_log(what, addr, len, r);
	if (r->jedec_valid)
		cli_print(sh, "jedec    : %02X %02X %02X (read again with the window "
		              "down)\r\n", r->jedec[0], r->jedec[1], r->jedec[2]);

	switch (r->status) {
	case NOR_SVC_OK:
		if (len == 0u)
			cli_print(sh, "ok       : no data touched\r\n");
		else if (r->verified != 0u)
			cli_print(sh, "ok       : %lu B done, %lu B read back and "
			              "compared\r\n", (unsigned long)r->done,
			          (unsigned long)r->verified);
		else
			cli_print(sh, "ok       : %lu B done\r\n", (unsigned long)r->done);
		return 0;
	case NOR_SVC_BUSY:
	case NOR_SVC_DOWN:
		cli_error(sh, r->status == NOR_SVC_BUSY ? "nor: busy -- %s\r\n"
		                                         : "nor: %s\r\n",
		          nor_why(r, "try again"));
		return 1;
	case NOR_SVC_REFUSED:
		cli_error(sh, "nor: refused -- %s\r\n",
		          nor_why(r, "outside what this board may write"));
		return 1;
	case NOR_SVC_NO_TRANSPORT:
		cli_error(sh, "nor: the part did not answer; nothing was sent\r\n");
		return 1;
	case NOR_SVC_IO:
		cli_error(sh, "nor: %s failed: %s\r\n", what, nor_why(r, "I/O error"));
		return 1;
	case NOR_SVC_CANCELLED:
		cli_error(sh, "nor: cancelled after %lu B of %lu (all of it "
		              "erased)\r\n", (unsigned long)r->done,
		          (unsigned long)len);
		return 1;
	case NOR_SVC_INCOMPLETE:
		cli_error(sh, "nor: stopped after %lu B of %lu (%s); %lu B read back "
		              "and compared, nothing is claimed about the rest\r\n",
		          (unsigned long)r->done, (unsigned long)len,
		          nor_why(r, "the device stopped"),
		          (unsigned long)r->verified);
		return 1;
	case NOR_SVC_FAULTED:
	default:
		cli_error(sh, "nor: FAULTED -- %s\r\n",
		          nor_why(r, "what was read back is not what was written"));
		if (r->bad_valid)
			cli_print(sh, "         first at 0x%06lX: read 0x%02X, wanted "
			              "0x%02X\r\n", (unsigned long)r->bad_off,
			          r->bad_got, r->bad_want);
		cli_print(sh, "         the device refuses everything until the "
		              "board is reset\r\n");
		return 1;
	}
}

#if CLI_ENABLE_DANGEROUS_CMDS

_Static_assert(NOR_CHUNK == NOR_SVC_WRITE_MAX,
               "a write's bytes and a test's page share one working size");

/* The range the board refuses, said in its words. */
static int nor_refuse_range(struct cli_instance *sh, enum nor_svc_op op,
                            uint32_t addr, uint32_t len)
{
	struct nor_svc_range g;

	memset(&g, 0, sizeof g);
	nor_svc_board.range(op, addr, len, &g);
	switch (g.kind) {
	case NOR_SVC_RANGE_OK:
		return 0;
	case NOR_SVC_RANGE_INSIDE:
		cli_error(sh, "nor: 0x%06lX .. 0x%06lX is in the %s (0x%06lX .. "
		              "0x%06lX)%s\r\n", (unsigned long)addr,
		          (unsigned long)(addr + len - 1u), g.name,
		          (unsigned long)g.lo, (unsigned long)(g.hi - 1u), g.hint);
		return -1;
	case NOR_SVC_RANGE_ALIGN:
		cli_error(sh, "nor: a write starts on a multiple of %lu B here "
		              "(0x%06lX does not)%s\r\n", (unsigned long)g.lo,
		          (unsigned long)addr, g.hint);
		return -1;
	case NOR_SVC_RANGE_OUTSIDE:
	default:
		cli_error(sh, "nor: 0x%06lX .. 0x%06lX is not inside what this board "
		              "writes (0x%06lX .. 0x%06lX)%s\r\n", (unsigned long)addr,
		          (unsigned long)(addr + len - 1u), (unsigned long)g.lo,
		          (unsigned long)(g.hi - 1u), g.hint);
		return -1;
	}
}

/* Everything an erase, write or test asks before the hold (the file comment). */
static int nor_admit(struct cli_instance *sh, enum nor_svc_op op, uint32_t addr,
                     uint32_t len)
{
	return (nor_refuse_range(sh, op, addr, len) || nor_console_refuse(sh)) ? -1 : 0;
}

/* Is a request a whole number of 4 KB sectors? */
static int nor_arg_sectors(struct cli_instance *sh, const char *sub,
                           uint32_t addr, uint32_t len)
{
	if (addr % NOR_SVC_ERASE_UNIT == 0u && len % NOR_SVC_ERASE_UNIT == 0u &&
	    len != 0u && nor_svc_board.erase_unit == NOR_SVC_ERASE_UNIT)
		return 0;
	cli_error(sh, "nor: %s takes whole 4 KB sectors -- the address and the "
	              "length must be multiples of 0x1000\r\n", sub);
	return -1;
}

/* ---- erase ---------------------------------------------------------------- */

#define NOR_TICK_EVERY (64u * NOR_SVC_ERASE_UNIT)

/*
 * Called by the board after each 4 KB it erased.  [!] On grove-vision-ai-v2
 * it runs with the memory-mapped window DOWN (port/nor/nor_write.h), so it
 * prints and asks for Ctrl+C and does nothing else: no log, no blob, no
 * devmem, no wait.
 */
static int nor_erase_tick(void *ctx, uint32_t done, uint32_t total)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (total > NOR_TICK_EVERY && (done % NOR_TICK_EVERY) == 0u)
		cli_print(sh, "erasing  : %lu / %lu B\r\n", (unsigned long)done,
		          (unsigned long)total);
	return cli_cancel_requested(sh) ? 1 : 0;
}

struct nor_op_ctx {
	struct cli_instance *sh;
	uint32_t addr, len;
	const uint8_t *data;
	struct nor_svc_result r;
	uint32_t ms;
};

static void nor_erase_body(void *ctx, const struct nor_svc_ops *ops)
{
	struct nor_op_ctx *c = (struct nor_op_ctx *)ctx;
	struct nor_svc_progress p = { c->sh, nor_erase_tick };
	uint32_t t0;

	/* The span lost, BEFORE it is lost: after a Ctrl+C the console drops
	 * the rest of this command's output, so it has to be said first. */
	cli_print(c->sh, "erasing  : 0x%06lX .. 0x%06lX (%lu KB, %lu sector(s))\r\n",
	          (unsigned long)c->addr, (unsigned long)(c->addr + c->len - 1u),
	          (unsigned long)(c->len >> 10),
	          (unsigned long)(c->len / NOR_SVC_ERASE_UNIT));
	if (nor_svc_board.erase_cancels && c->len > NOR_SVC_ERASE_UNIT)
		cli_print(c->sh, "         Ctrl+C stops it between sectors; the "
		              "result is in `dmesg`\r\n");
	t0 = (uint32_t)tx_time_get();
	ops->erase(ops->dev, c->addr, c->len, &p, &c->r);
	c->ms = (uint32_t)tx_time_get() - t0;
}

static int cmd_nor_erase(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_op_ctx c;
	struct nor_svc_result h;
	int rc;

	(void)argc;
	memset(&c, 0, sizeof c);
	c.sh = sh;
	if (nor_arg_addr(sh, argv[1], &c.addr))
		return 1;
	if (cli_parse_u32(argv[2], &c.len) != 0) {
		cli_error(sh, "nor: bad length\r\n");
		return 1;
	}
	if (nor_arg_sectors(sh, "erase", c.addr, c.len) ||
	    nor_arg_fits(sh, c.addr, c.len) ||
	    nor_admit(sh, NOR_SVC_OP_ERASE, c.addr, c.len))
		return 1;

	memset(&h, 0, sizeof h);
	nor_svc_board.hold(nor_erase_body, &c, &h);
	if (h.status != NOR_SVC_OK)
		return nor_cmd_not_held(sh, "erase", c.addr, c.len, &h);
	rc = nor_cmd_report(sh, "erase", c.addr, c.len, &c.r);
	cli_print(sh, "elapsed  : %lu ms\r\n", (unsigned long)c.ms);
	return rc;
}

/* ---- write ---------------------------------------------------------------- */

static int nor_hex_nibble(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

/* "DEADBEEF" (or "0xDEADBEEF") -> bytes.  The count, or -1. */
static int nor_hex_bytes(const char *s, uint8_t *out, uint32_t max)
{
	uint32_t n = 0u;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (*s != '\0') {
		int hi = nor_hex_nibble(s[0]);
		int lo = (hi >= 0) ? nor_hex_nibble(s[1]) : -1;

		if (hi < 0 || lo < 0 || n >= max)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
		s += 2;
	}
	return (n == 0u) ? -1 : (int)n;
}

static void nor_write_body(void *ctx, const struct nor_svc_ops *ops)
{
	struct nor_op_ctx *c = (struct nor_op_ctx *)ctx;
	uint32_t t0;

	cli_print(c->sh, "program  : 0x%06lX + %lu B (bits only clear -- erase "
	              "first)\r\n", (unsigned long)c->addr, (unsigned long)c->len);
	t0 = (uint32_t)tx_time_get();
	ops->program(ops->dev, c->addr, c->data, c->len, &c->r);
	c->ms = (uint32_t)tx_time_get() - t0;
}

static int cmd_nor_write(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t data[NOR_SVC_WRITE_MAX];
	struct nor_op_ctx c;
	struct nor_svc_result h;
	int n, rc;

	(void)argc;
	if (nor_absent(sh, "write", nor_svc_board.no_write))
		return 1;
	memset(&c, 0, sizeof c);
	c.sh = sh;
	c.data = data;
	if (nor_arg_addr(sh, argv[1], &c.addr))
		return 1;
	n = nor_hex_bytes(argv[2], data, sizeof data);
	if (n < 0) {
		cli_error(sh, "nor: bad hex data (an even number of hex digits, "
		              "1 .. %lu B)\r\n", (unsigned long)sizeof data);
		return 1;
	}
	c.len = (uint32_t)n;
	if (nor_arg_fits(sh, c.addr, c.len) ||
	    nor_admit(sh, NOR_SVC_OP_WRITE, c.addr, c.len))
		return 1;

	memset(&h, 0, sizeof h);
	nor_svc_board.hold(nor_write_body, &c, &h);
	if (h.status != NOR_SVC_OK)
		return nor_cmd_not_held(sh, "write", c.addr, c.len, &h);
	rc = nor_cmd_report(sh, "write", c.addr, c.len, &c.r);
	cli_print(sh, "elapsed  : %lu ms\r\n", (unsigned long)c.ms);
	return rc;
}

/* ---- test ----------------------------------------------------------------- */

/*
 * The acceptance test, on one 4 KB sector that reads blank (issues #194, #214,
 * #249, #250): erase -> blank -> a counting pattern page by page, read back
 * and compared -> erase -> blank -> the partial-page writes the kv store
 * makes -> the same-byte overwrite, observed -> erase -> blank -> PASS.
 *
 * The partial-page writes: FlashDB at an 8-bit write granularity
 * (svc/kv/fdb_cfg.h) programs single status bytes one after another into the
 * same page, and headers and values of odd lengths at odd offsets, some across
 * a page boundary -- every byte once, never one that is not 0xFF.  Each entry
 * is one program at that offset, the entries never overlap, and after each one
 * both pages are read back whole.
 *
 * The overwrite is OBSERVED, never a pass or a fail: re-programming a byte
 * already programmed is what a write granularity of 1 would need and what the
 * datasheets do not promise.  A program or read that fails is the device not
 * answering, and fails the test.
 *
 * Ctrl+C is asked before every page (decided for issue #250).  Cancelled or
 * failed once anything was erased, the sector may be left programmed, and the
 * next test refuses it until it is erased; the line says so.
 */
static const struct { uint16_t off, len; } nor_partial[] = {
	{   0u,   1u }, {   1u,   1u }, {   2u,   1u }, {   3u,   1u },  /* statuses */
	{   8u,  24u },                                                 /* a header  */
	{  32u,  13u },                                                 /* a record  */
	{   4u,   1u }, {  45u,   3u }, {   5u,   1u },
	{ 250u,  12u },                                  /* across the page boundary */
	{ 262u,   1u }, {   6u,   1u }, {   7u,   1u },
	{ 300u, 100u },
};
#define NOR_PARTIAL_N    (sizeof nor_partial / sizeof nor_partial[0])
#define NOR_PARTIAL_SPAN (2u * NOR_CHUNK)
#define NOR_OVERWRITE_AT 64u    /* untouched by the partial writes */

/* Never 0xFF, so a program that did not happen cannot read as one that did. */
static uint8_t nor_partial_byte(unsigned k, uint32_t i)
{
	uint8_t v = (uint8_t)(0x5Au ^ (k * 17u) ^ i);

	return (v == 0xFFu) ? 0x00u : v;
}

static uint8_t nor_partial_want(unsigned last, uint32_t at)
{
	unsigned k;

	for (k = 0; k <= last; k++)
		if (at >= nor_partial[k].off && at - nor_partial[k].off < nor_partial[k].len)
			return nor_partial_byte(k, at - nor_partial[k].off);
	return 0xFFu;
}

enum nor_test_end { NOR_T_PASS = 0, NOR_T_FAIL, NOR_T_CANCEL, NOR_T_NOT_BLANK };

struct nor_test_ctx {
	struct cli_instance *sh;
	const struct nor_svc_ops *ops;
	uint32_t addr;
	uint8_t buf[NOR_CHUNK];
	enum nor_test_end end;
	uint8_t erased;          /* anything erased yet */
	const char *step;        /* where it ended       */
};

static int nor_t_cancel(struct nor_test_ctx *t)
{
	if (!cli_cancel_requested(t->sh))
		return 0;
	t->end = NOR_T_CANCEL;
	return 1;
}

/* An operation's answer; 0 when it did what it was asked. */
static int nor_t_op(struct nor_test_ctx *t, const struct nor_svc_result *r,
                    const char *step)
{
	if (r->status == NOR_SVC_OK)
		return 0;
	t->end = NOR_T_FAIL;
	t->step = step;
	cli_error(t->sh, "nor: FAIL -- %s: %s %s\r\n", step,
	          nor_status_word(r->status), nor_why(r, ""));
	return 1;
}

static int nor_t_read(struct nor_test_ctx *t, uint32_t off, const char *step)
{
	struct nor_svc_result r;

	memset(&r, 0, sizeof r);
	t->ops->read(t->ops->dev, t->addr + off, t->buf, NOR_CHUNK, &r);
	return nor_t_op(t, &r, step);
}

static int nor_t_program(struct nor_test_ctx *t, uint32_t off,
                         const uint8_t *data, uint32_t len, const char *step)
{
	struct nor_svc_result r;

	memset(&r, 0, sizeof r);
	t->ops->program(t->ops->dev, t->addr + off, data, len, &r);
	return nor_t_op(t, &r, step);
}

/* All 0xFF, page by page?  0 yes; 1 no, with t->end said. */
static int nor_t_blank(struct nor_test_ctx *t, const char *step)
{
	uint32_t off, i;

	for (off = 0u; off < NOR_SVC_ERASE_UNIT; off += NOR_CHUNK) {
		if (nor_t_cancel(t) || nor_t_read(t, off, step))
			return 1;
		for (i = 0u; i < NOR_CHUNK; i++) {
			if (t->buf[i] == 0xFFu)
				continue;
			if (t->erased) {
				t->end = NOR_T_FAIL;
				t->step = step;
				cli_error(t->sh, "nor: FAIL -- %s: 0x%06lX = 0x%02X, "
				              "expected 0xFF\r\n", step,
				          (unsigned long)(t->addr + off + i), t->buf[i]);
			} else {
				t->end = NOR_T_NOT_BLANK;
			}
			return 1;
		}
	}
	return 0;
}

static int nor_t_erase_blank(struct nor_test_ctx *t, const char *step)
{
	struct nor_svc_result r;

	if (nor_t_cancel(t))
		return 1;
	memset(&r, 0, sizeof r);
	t->erased = 1u;
	t->ops->erase(t->ops->dev, t->addr, NOR_SVC_ERASE_UNIT, NULL, &r);
	return nor_t_op(t, &r, step) || nor_t_blank(t, step);
}

static int nor_t_pattern(struct nor_test_ctx *t)
{
	uint32_t off, i, t0;

	for (off = 0u; off < NOR_SVC_ERASE_UNIT; off += NOR_CHUNK) {
		if (nor_t_cancel(t))
			return 1;
		for (i = 0u; i < NOR_CHUNK; i++)
			t->buf[i] = (uint8_t)(i ^ (off >> 8));
		if (nor_t_program(t, off, t->buf, NOR_CHUNK, "pattern program"))
			return 1;
	}
	t0 = (uint32_t)tx_time_get();
	for (off = 0u; off < NOR_SVC_ERASE_UNIT; off += NOR_CHUNK) {
		if (nor_t_cancel(t) || nor_t_read(t, off, "pattern read"))
			return 1;
		for (i = 0u; i < NOR_CHUNK; i++) {
			uint8_t want = (uint8_t)(i ^ (off >> 8));

			if (t->buf[i] != want) {
				t->end = NOR_T_FAIL;
				t->step = "pattern compare";
				cli_error(t->sh, "nor: FAIL -- 0x%06lX read 0x%02X, want "
				              "0x%02X\r\n", (unsigned long)(t->addr + off + i),
				          t->buf[i], want);
				return 1;
			}
		}
	}
	cli_print(t->sh, "pattern  : 4096 B programmed, read back and compared "
	              "(%lu ms to read)\r\n",
	          (unsigned long)((uint32_t)tx_time_get() - t0));
	return 0;
}

static int nor_t_partial(struct nor_test_ctx *t)
{
	unsigned k;
	uint32_t at, i;

	for (k = 0; k < NOR_PARTIAL_N; k++) {
		uint32_t off = nor_partial[k].off, len = nor_partial[k].len;

		if (nor_t_cancel(t))
			return 1;
		/* The one page buffer: the bytes are written out of it, then the
		 * pages are read back into it (one 256 B area on the stack). */
		for (i = 0u; i < len; i++)
			t->buf[i] = nor_partial_byte(k, i);
		if (nor_t_program(t, off, t->buf, len, "partial write"))
			return 1;
		for (at = 0u; at < NOR_PARTIAL_SPAN; at += NOR_CHUNK) {
			if (nor_t_read(t, at, "partial read"))
				return 1;
			for (i = 0u; i < NOR_CHUNK; i++) {
				uint8_t want = nor_partial_want(k, at + i);

				if (t->buf[i] != want) {
					t->end = NOR_T_FAIL;
					t->step = "partial compare";
					cli_error(t->sh, "nor: FAIL -- after partial write %u "
					              "(%lu B at +%lu): 0x%06lX read 0x%02X, want "
					              "0x%02X\r\n", k, (unsigned long)len,
					          (unsigned long)off,
					          (unsigned long)(t->addr + at + i), t->buf[i],
					          want);
					return 1;
				}
			}
		}
	}
	cli_print(t->sh, "partial  : %u writes into 2 pages, both pages compared "
	              "after each\r\n", (unsigned)NOR_PARTIAL_N);
	return 0;
}

static int nor_t_overwrite(struct nor_test_ctx *t)
{
	static const uint8_t seq[2] = { 0xFEu, 0xFCu };
	uint8_t got[2];
	unsigned i;

	for (i = 0u; i < 2u; i++) {
		if (nor_t_cancel(t) ||
		    nor_t_program(t, NOR_OVERWRITE_AT, &seq[i], 1u, "overwrite program") ||
		    nor_t_read(t, 0u, "overwrite read"))
			return 1;
		got[i] = t->buf[NOR_OVERWRITE_AT];
	}
	cli_print(t->sh, "overwrite: 0xFF -> write 0xFE -> read 0x%02X -> write 0xFC "
	              "-> read 0x%02X%s (observed, not judged)\r\n", got[0], got[1],
	          (got[0] == 0xFEu && got[1] == 0xFCu) ? "  accepted"
	                                               : "  NOT accepted");
	return 0;
}

static void nor_test_body(void *ctx, const struct nor_svc_ops *ops)
{
	struct nor_test_ctx *t = (struct nor_test_ctx *)ctx;

	t->ops = ops;
	if (ops->read == NULL) {             /* the table and the ops disagree */
		t->end = NOR_T_FAIL;
		t->step = "no read";
		return;
	}
	/* [!] Before anything is erased: a test is not a way to destroy data. */
	if (nor_t_blank(t, "blank before"))
		return;
	cli_print(t->sh, "blank    : 4096 B read 0xFF before the test\r\n");
	if (nor_t_erase_blank(t, "erase") || nor_t_pattern(t) ||
	    nor_t_erase_blank(t, "erase") || nor_t_partial(t) ||
	    nor_t_overwrite(t) || nor_t_erase_blank(t, "final erase"))
		return;
	cli_print(t->sh, "final    : erased and read blank (left as found)\r\n");
	t->end = NOR_T_PASS;
}

static int cmd_nor_test(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_test_ctx t;
	struct nor_svc_result h;

	(void)argc;
	if (nor_absent(sh, "test", nor_svc_board.no_test))
		return 1;
	memset(&t, 0, sizeof t);
	t.sh = sh;
	if (nor_arg_addr(sh, argv[1], &t.addr))
		return 1;
	if (nor_arg_sectors(sh, "test", t.addr, NOR_SVC_ERASE_UNIT) ||
	    nor_admit(sh, NOR_SVC_OP_ERASE, t.addr, NOR_SVC_ERASE_UNIT))
		return 1;

	cli_print(sh, "nor test : 4 KB at 0x%06lX (must read blank; it is erased, "
	              "written and left blank)\r\n", (unsigned long)t.addr);
	memset(&h, 0, sizeof h);
	nor_svc_board.hold(nor_test_body, &t, &h);
	if (h.status != NOR_SVC_OK)
		return nor_cmd_not_held(sh, "test", t.addr, NOR_SVC_ERASE_UNIT, &h);

	switch (t.end) {
	case NOR_T_PASS:
		log_write(LOG_LEVEL_INF, "nor", "test 0x%lx: pass", (unsigned long)t.addr);
		cli_print(sh, "nor: PASS\r\n");
		return 0;
	case NOR_T_NOT_BLANK:
		log_write(LOG_LEVEL_WRN, "nor", "test 0x%lx: not blank, nothing erased",
		          (unsigned long)t.addr);
		cli_error(sh, "nor: 4 KB at 0x%06lX is not blank -- not testing over "
		              "data; `nor erase 0x%06lX 4096` first if it is scratch\r\n",
		          (unsigned long)t.addr, (unsigned long)t.addr);
		return 1;
	case NOR_T_CANCEL:
	case NOR_T_FAIL:
	default:
		log_write(LOG_LEVEL_WRN, "nor", "test 0x%lx: %s at %s%s",
		          (unsigned long)t.addr,
		          t.end == NOR_T_CANCEL ? "cancelled" : "fail",
		          t.step ? t.step : "-", t.erased ? ", may not be blank" : "");
		if (t.end == NOR_T_CANCEL)
			cli_error(sh, "nor: cancelled\r\n");
		if (t.erased)
			cli_error(sh, "nor: the 4 KB at 0x%06lX may not be blank now -- "
			              "`nor erase 0x%06lX 4096` before the next `nor test`\r\n",
			          (unsigned long)t.addr, (unsigned long)t.addr);
		else
			cli_error(sh, "nor: nothing was erased\r\n");
		return 1;
	}
}

#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_SUBCMD_SET_CREATE(nor_subcmds,
	CLI_CMD_ARG(info, NULL, "the part, its state and geometry, the board's lines",
	            cmd_nor_info, 1, 0),
	CLI_CMD_ARG_USAGE(read, NULL, "hexdump 1 .. 1024 B (default 64)",
	                  "<addr> [len]", cmd_nor_read, 2, 1),
#if CLI_ENABLE_DANGEROUS_CMDS
	CLI_CMD_ARG_USAGE(erase, NULL, "erase whole 4 KB sectors (both 4 KB aligned)",
	                  "<addr> <len>", cmd_nor_erase, 3, 0),
	CLI_CMD_ARG_USAGE(write, NULL, "program 1 .. 256 B given in hex (erase first)",
	                  "<addr> <hex>", cmd_nor_write, 3, 0),
	CLI_CMD_ARG_USAGE(test, NULL, "acceptance test on one blank 4 KB sector",
	                  "<addr>", cmd_nor_test, 2, 0),
#endif
	NOR_SVC_BOARD_SUBCMDS
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nor, nor_subcmds, NOR_SVC_HELP, NULL, 1, 0);

#else  /* no device: nothing registered */

/* ISO C wants a declaration in every translation unit. */
typedef int nor_cmd_not_registered_t;

#endif /* NOR_SVC_HAS_DEVICE */
