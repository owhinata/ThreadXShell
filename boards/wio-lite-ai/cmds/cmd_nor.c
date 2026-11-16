/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    cmd_nor.c
 * @brief   `nor` shell command: external W25Q128 on OCTOSPI2 (owhinata/wio-lite-ai#37).
 *
 *   nor info               bring-up state, JEDEC id, geometry, device clock
 *   nor read <addr> [len]  hexdump from a device offset (default 64 B)
 *   nor erase <addr> [len] erase 4 KB sector(s)                        (danger)
 *   nor write <addr> <hex> program bytes given as a hex string         (danger)
 *   nor test [addr]        acceptance test for the partial-page programming that
 *                          the KV store's FDB_WRITE_GRAN = 8 depends on (danger)
 *
 * `nor test` is the reason this command exists.  The whole KV design rests on one
 * datasheet reading -- that "program at previously erased (FFh) memory locations"
 * (W25Q128JV sec 8.2.13) constrains the *state* of the target bytes and not the
 * *history* of the page, so several partial programs into disjoint bytes of one
 * page are in specification while re-programming an already-programmed byte is
 * not.  FlashDB with FDB_WRITE_GRAN = 8 only ever does the former.  The test
 * proves that on this device before any of it is trusted, and separately
 * *observes* (without requiring) what a same-byte overwrite does, because that is
 * the thing a future FDB_WRITE_GRAN = 1 would depend on.
 *
 * Addresses are device offsets (0 .. 16 MB), not 0x70000000 pointers: the window
 * is never memory-mapped and stays fenced off in the MPU (see port/nor).
 *
 * WHAT GUARDS erase / write / test (issue #249).  They rewrite flash content, so
 * they compile in only with CLI_ENABLE_DANGEROUS_CMDS, like devmem.  Each asks,
 * in this order and before it touches the device:
 *   1. its arguments, and whether the range lies on the device;
 *   2. whether the range it would ACTUALLY erase or program (erase: rounded out
 *      to whole sectors; test: the 4 KB sector) touches the kv partition
 *      (port/flashdb/fal_cfg.h) -- refused outright, `kv format yes` is the way
 *      to erase it.  The kv store does not take the busy flag on this board, so
 *      the range is what keeps a raw erase off FlashDB's sectors;
 *   3. whether it runs on the telnet console, in a background job launched from
 *      it, or in a background job of the local console -- refused: these run in
 *      the foreground of the local console only;
 *   4. whether the NOR is up;
 *   5. the blob store's busy flag (src/blob.h), held for the whole operation and
 *      given back at one place on every path once it was taken.  It is what
 *      orders a raw erase against a `blob write` / `blob erase` / `nn model load`
 *      and against another erase or test.  The blob slots themselves are NOT
 *      refused: under the flag a slot can be erased here, as on f746g-disco.
 * `nor test` further refuses a sector that does not read blank, BEFORE it erases
 * anything (a test is not a way to destroy data), and leaves the sector blank
 * when it passes.
 *
 * `nor info` and `nor read` take no flag: nothing on this board orders a read
 * against the flag (neither the blob reads nor kv take it), and `blob write`
 * holds it for a whole transfer, during which a read is exactly what is wanted.
 * A read interleaves with an erase or a program one driver call at a time under
 * the device mutex and disturbs neither.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "cli_instance.h"    /* sh->fg: is this a background job */
#include "nor_flash.h"
/* BLOB_REGION_END (the `nor test` default must stay clear of the asset region,
 * and the assert below keeps the two from drifting apart), the busy flag and
 * BLOB_BUSY_HOLDERS.  Both files are compiled only under BSP_ENABLE_KV
 * (board.cmake), so the include is always resolvable wherever this is built. */
#include "blob.h"
#include "fal_cfg.h"         /* KV_PART_OFFSET / KV_PART_LEN (issue #249) */
#include "net_tty.h"         /* net_tty_is_console, the other console's name */

#include <stdint.h>
#include <string.h>

/* Scratch for readbacks: one page, on the caller's stack. */
#define NOR_CHUNK  NOR_PAGE_SIZE

/* `nor read` bounds. */
#define NOR_READ_DEFAULT  64u
#define NOR_READ_MAX      1024u

/*
 * Default scratch sector for `nor test`.  At 8 MB -- the first sector ABOVE the
 * blob region (owhinata/wio-lite-ai#10 put asset slots at 1 MB .. 3 MB;
 * owhinata/wio-lite-ai#55 widened them to six 512 KB slots ending at 4 MB; issue
 * #193 re-carved them into thirteen slots of three sizes ending at 8 MB), in the
 * 8 MB that is still unallocated -- so running the acceptance test destroys
 * neither a live configuration nor a stored model.
 *
 * [!] THIS ADDRESS IS TIED TO THE BLOB REGION'S END, so it moves whenever the region
 * does.  It was 0x00300000 until owhinata/wio-lite-ai#55 and 0x00400000 until issue
 * #193 -- each time exactly where the next region put a slot header (0x400000 is
 * now the first 512 KB slot's).  Boards that ran `nor test` under an old default
 * may still have that residue there: it decodes as `invalid` rather than as a
 * blob, and one `blob erase <slot>` clears it.  Since issue #249 a `nor test`
 * also refuses a sector that is not blank, so a board whose 0x800000 still
 * holds an earlier test's pattern needs one `nor erase 0x800000` first.
 *
 * It used to be 0x00100000, chosen when everything past the KV partition was spare;
 * the blob region moved in on top of it, which would have made a bare `nor test`
 * erase slot 0's HEADER and silently retire the blob stored there.  The lesson is
 * in the default: it is the argument-less form, the one that gets typed without
 * thinking, that has to land somewhere harmless.
 */
#define NOR_TEST_DEFAULT_HEX   0x800000
#define NOR_TEST_DEFAULT_ADDR  ((uint32_t)NOR_TEST_DEFAULT_HEX)

/*
 * The help text at the bottom of this file QUOTES this address, and a help string
 * naming a different address than the code uses is worse than one naming none -- it
 * is confidently wrong, and it is read precisely by the person who has not looked at
 * the code.  So the string is built from the same token, and the assert keeps the
 * token itself above the region it is supposed to avoid.  (Two macros because
 * stringifying the uint32_t-cast form would print the cast, not the address.)
 */
#define NOR_STR_(x)            #x
#define NOR_STR(x)             NOR_STR_(x)
#define NOR_TEST_DEFAULT_STR   NOR_STR(NOR_TEST_DEFAULT_HEX)

_Static_assert(NOR_TEST_DEFAULT_ADDR >= BLOB_REGION_END,
               "the `nor test` default must stay above the blob region -- "
               "owhinata/wio-lite-ai#55 and issue #193 "
               "moved the region's end and this address has to follow it");

static const char *nor_strerror(int rc)
{
	switch (rc) {
	case NOR_ERR_PARAM:   return "bad argument";
	case NOR_ERR_IO:      return "OCTOSPI2 transaction did not complete";
	case NOR_ERR_TIMEOUT: return "device stayed busy";
	case NOR_ERR_STATE:   return "driver not initialized / device down";
	case NOR_ERR_BUSY:    return "device busy (another command holds it)";
	default:              return "unknown error";
	}
}

/* Refuse every hardware subcommand while the device is down, with one message. */
static int nor_gate(struct cli_instance *sh)
{
	if (!nor_flash_ready()) {
		cli_error(sh, "nor: device down (see `nor info`)\r\n");
		return -1;
	}
	return 0;
}

/* Parse a device offset and reject anything outside the flash. */
static int nor_parse_addr(struct cli_instance *sh, const char *s, uint32_t *out)
{
	if (cli_parse_u32(s, out) != 0 || *out >= NOR_SIZE_BYTES) {
		cli_error(sh, "nor: bad address (0 .. 0x%lX)\r\n",
		          (unsigned long)(NOR_SIZE_BYTES - 1u));
		return -1;
	}
	return 0;
}

/* ---- subcommands --------------------------------------------------------- */

static int cmd_nor_info(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t id[3];
	uint32_t regs[4];

	(void)argc; (void)argv;
	nor_flash_get_id(id);
	nor_flash_get_regs(regs);

	cli_print(sh, "NOR (OCTOSPI2 / W25Q128JV):\r\n");
	cli_print(sh, "  state   : %s\r\n",
	          nor_flash_ready() ? "ready (indirect)" : "NOT ready");
	cli_print(sh, "  jedec   : %02X %02X %02X\r\n", id[0], id[1], id[2]);
	cli_print(sh, "  size    : %lu MB (offsets 0 .. 0x%lX)\r\n",
	          (unsigned long)(NOR_SIZE_BYTES >> 20),
	          (unsigned long)(NOR_SIZE_BYTES - 1u));
	cli_print(sh, "  erase   : %lu B sector / %lu B block\r\n",
	          (unsigned long)NOR_SECTOR_SIZE, (unsigned long)NOR_BLOCK_SIZE);
	cli_print(sh, "  page    : %lu B\r\n", (unsigned long)NOR_PAGE_SIZE);
	cli_print(sh, "  clock   : %lu Hz (1-line SPI, Fast Read 0x0B)\r\n",
	          (unsigned long)nor_flash_clock_hz());
	cli_print(sh, "  mapping : none -- indirect only, window fenced off in the MPU\r\n");
	/* Register readback: a wrong controller configuration and a dead device look
	 * the same from the id alone (the first bring-up here read 00 00 00 with a
	 * clean CR.FSEL=0 -- transaction completed, wrong pins). */
	cli_print(sh, "  regs    : CR %08lX DCR1 %08lX DCR2 %08lX SR %08lX\r\n",
	          (unsigned long)regs[0], (unsigned long)regs[1],
	          (unsigned long)regs[2], (unsigned long)regs[3]);
	return 0;
}

static int cmd_nor_read(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t buf[NOR_CHUNK];
	uint32_t addr, len = NOR_READ_DEFAULT, done = 0u;

	if (nor_gate(sh) || nor_parse_addr(sh, argv[1], &addr))
		return 1;
	if (argc > 2 && (cli_parse_u32(argv[2], &len) != 0 || len == 0u ||
	                 len > NOR_READ_MAX)) {
		cli_error(sh, "nor: bad length (1 .. %lu)\r\n",
		          (unsigned long)NOR_READ_MAX);
		return 1;
	}
	if (len > NOR_SIZE_BYTES - addr)
		len = NOR_SIZE_BYTES - addr;

	while (done < len) {
		uint32_t chunk = len - done;
		int rc;

		if (chunk > sizeof buf)
			chunk = sizeof buf;
		rc = nor_read(addr + done, buf, chunk);
		if (rc != NOR_OK) {
			cli_error(sh, "nor: read failed at 0x%06lX: %s\r\n",
			          (unsigned long)(addr + done), nor_strerror(rc));
			return 1;
		}
		cli_hexdump_base(sh, buf, chunk, addr + done);
		done += chunk;
	}
	return 0;
}

#if CLI_ENABLE_DANGEROUS_CMDS

/* `nor write` bound: one page per invocation keeps the hex argument sane. */
#define NOR_WRITE_MAX     NOR_PAGE_SIZE

/* The end of the kv partition, one past its last byte. */
#define NOR_KV_END        (KV_PART_OFFSET + KV_PART_LEN)

/*
 * Refuse a range that touches the kv partition by even one byte.  @p len is
 * non-zero and the caller has already checked addr + len <= NOR_SIZE_BYTES, so
 * the sum cannot wrap.  Asked of the range the operation will ACTUALLY erase or
 * program: today the partition ends on a sector boundary, so the address the
 * user typed would give the same answer, but the range is the one that stays
 * right if the rounding ever goes.
 */
static int nor_refuse_kv(struct cli_instance *sh, uint32_t addr, uint32_t len)
{
	if (addr >= NOR_KV_END || addr + len <= KV_PART_OFFSET)
		return 0;
	cli_error(sh, "nor: 0x%06lX .. 0x%06lX is in the kv store (0x%06lX .. "
	              "0x%06lX) -- `kv format yes` erases it\r\n",
	          (unsigned long)addr, (unsigned long)(addr + len - 1u),
	          (unsigned long)KV_PART_OFFSET, (unsigned long)(NOR_KV_END - 1u));
	return -1;
}

/*
 * Erasing and programming run in the foreground of the local console only: not
 * on the telnet console, not in a background job launched from it, and not in a
 * background job (`... &`) of the local console.  No board fact here -- the
 * question about telnet is net_tty's (a job answers for its launcher) and the
 * other console's name is its table -- so this can move to shell/cmds as it is.
 * The reason given is only the rule: such an erase does not end the telnet
 * session, so net_tty_guard()'s sentence would not be true here.
 */
static int nor_refuse_console(struct cli_instance *sh)
{
	const char *here;

	if (net_tty_is_console(sh))
		here = (sh->fg != NULL) ? "a background job of the telnet console"
		                        : "the telnet console";
	else if (sh->fg != NULL)
		here = "a background job";
	else
		return 0;
	cli_error(sh, "nor: erase, write and test run only in the foreground of the "
	              "%s console; this is %s\r\n",
	          net_tty_board_facts.other_console, here);
	return -1;
}

/*
 * The blob store's busy flag, for the whole of one erase, write or test.  On
 * success the caller owns it and gives it back with blob_busy_release() at its
 * one exit; on refusal nothing was taken and nothing is to be given back.
 */
static int nor_busy_gate(struct cli_instance *sh)
{
	if (blob_busy_acquire() != BLOB_OK) {
		cli_error(sh, "nor: busy -- " BLOB_BUSY_HOLDERS " is running; "
		              "try again\r\n");
		return -1;
	}
	return 0;
}

/* Everything an erase, write or test asks once its range is known, in order
 * (see the file comment).  0 = go ahead, holding the busy flag. */
static int nor_admit(struct cli_instance *sh, uint32_t addr, uint32_t len)
{
	if (nor_refuse_kv(sh, addr, len) || nor_refuse_console(sh) ||
	    nor_gate(sh) || nor_busy_gate(sh))
		return -1;
	return 0;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* "DEADBEEF" -> bytes.  Returns the byte count, or -1 on a malformed string. */
static int hex_to_bytes(const char *s, uint8_t *out, uint32_t max)
{
	uint32_t n = 0u;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (*s != '\0') {
		int hi = hex_nibble(s[0]);
		int lo = (hi >= 0) ? hex_nibble(s[1]) : -1;

		if (hi < 0 || lo < 0 || n >= max)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
		s += 2;
	}
	return (n == 0u) ? -1 : (int)n;
}

static int cmd_nor_erase(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr, base, len = NOR_SECTOR_SIZE;
	int rc;

	if (nor_parse_addr(sh, argv[1], &addr))
		return 1;
	if (argc > 2 && (cli_parse_u32(argv[2], &len) != 0 || len == 0u)) {
		cli_error(sh, "nor: bad length\r\n");
		return 1;
	}
	/*
	 * Round the request out to whole sectors rather than refusing it: erase is a
	 * sector-granular operation and silently erasing less than asked would be
	 * worse than erasing the sector the caller pointed into.
	 *
	 * ORDER MATTERS.  The range check comes BEFORE any rounding, against the
	 * address the user actually gave: rounding first would let a huge length near
	 * the top of the device wrap the 32-bit sum, and the check would then pass a
	 * request it should have rejected -- turning an out-of-range argument into a
	 * short erase somewhere else.  After the check, addr + len <= NOR_SIZE_BYTES
	 * holds, and because base and NOR_SIZE_BYTES are both sector multiples the
	 * rounded span still fits inside the device with nothing to overflow.  The kv
	 * refusal (nor_admit) is then asked of the rounded span, the one erased.
	 */
	if (len > NOR_SIZE_BYTES - addr) {
		cli_error(sh, "nor: range runs past the end of the device\r\n");
		return 1;
	}
	base = addr - (addr % NOR_SECTOR_SIZE);
	len += addr - base;
	len = (len + NOR_SECTOR_SIZE - 1u) & ~(NOR_SECTOR_SIZE - 1u);
	addr = base;

	if (nor_admit(sh, addr, len))
		return 1;
	rc = nor_erase(addr, len);
	blob_busy_release();
	if (rc != NOR_OK) {
		cli_error(sh, "nor: erase failed: %s\r\n", nor_strerror(rc));
		return 1;
	}
	cli_print(sh, "erased 0x%06lX .. 0x%06lX (%lu KB)\r\n",
	          (unsigned long)addr, (unsigned long)(addr + len - 1u),
	          (unsigned long)(len >> 10));
	return 0;
}

static int cmd_nor_write(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t data[NOR_WRITE_MAX];
	uint32_t addr;
	int n, rc;

	(void)argc;
	if (nor_parse_addr(sh, argv[1], &addr))
		return 1;
	n = hex_to_bytes(argv[2], data, sizeof data);
	if (n < 0) {
		cli_error(sh, "nor: bad hex data (even number of hex digits, "
		          "max %lu bytes)\r\n", (unsigned long)sizeof data);
		return 1;
	}
	if ((uint32_t)n > NOR_SIZE_BYTES - addr) {
		cli_error(sh, "nor: range runs past the end of the device\r\n");
		return 1;
	}

	if (nor_admit(sh, addr, (uint32_t)n))
		return 1;
	rc = nor_write(addr, data, (uint32_t)n);
	blob_busy_release();
	if (rc != NOR_OK) {
		cli_error(sh, "nor: write failed: %s\r\n", nor_strerror(rc));
		return 1;
	}
	cli_print(sh, "wrote %d B at 0x%06lX (NOR: bits only clear -- erase first)\r\n",
	          n, (unsigned long)addr);
	return 0;
}

/* ---- nor test ------------------------------------------------------------ */

/*
 * Does [addr, addr+len) read all 0xFF, chunk by chunk?  0 = blank, 1 = not (the
 * first offending byte reported), -1 = a read failed (reported).
 */
static int nor_check_erased(struct cli_instance *sh, uint32_t addr, uint32_t len)
{
	uint8_t buf[NOR_CHUNK];
	uint32_t done = 0u;

	while (done < len) {
		uint32_t chunk = len - done;
		uint32_t i;
		int rc;

		if (chunk > sizeof buf)
			chunk = sizeof buf;
		rc = nor_read(addr + done, buf, chunk);
		if (rc != NOR_OK) {
			cli_error(sh, "  read failed at 0x%06lX: %s\r\n",
			          (unsigned long)(addr + done), nor_strerror(rc));
			return -1;
		}
		for (i = 0u; i < chunk; i++) {
			if (buf[i] != 0xFFu) {
				cli_error(sh, "  0x%06lX = 0x%02X, expected 0xFF\r\n",
				          (unsigned long)(addr + done + i), buf[i]);
				return 1;
			}
		}
		done += chunk;
	}
	return 0;
}

/*
 * The acceptance test for FDB_WRITE_GRAN = 8.  Three partial page programs land
 * in disjoint byte ranges of one page; after each, the whole page is read back
 * and checked so that (a) the new bytes took, (b) the previously written bytes
 * were not disturbed, and (c) everything else still reads 0xFF.  Point (c) is the
 * one that matters: it is what lets a later status byte be programmed into a page
 * that has already been programmed elsewhere.
 */
static int nor_test_partial(struct cli_instance *sh, uint32_t page)
{
	static const uint32_t off[3] = {0u, 16u, 32u};
	static const uint8_t  pat[3][4] = {
		{0xA5u, 0x5Au, 0x00u, 0xFFu},
		{0xDEu, 0xADu, 0xBEu, 0xEFu},
		{0x01u, 0x02u, 0x04u, 0x08u},
	};
	uint8_t back[NOR_CHUNK];
	uint32_t step;

	for (step = 0u; step < 3u; step++) {
		uint32_t i, w;
		int rc = nor_write(page + off[step], pat[step], sizeof pat[step]);

		if (rc != NOR_OK) {
			cli_error(sh, "  program %lu failed: %s\r\n",
			          (unsigned long)step, nor_strerror(rc));
			return -1;
		}
		rc = nor_read(page, back, sizeof back);
		if (rc != NOR_OK) {
			cli_error(sh, "  readback %lu failed: %s\r\n",
			          (unsigned long)step, nor_strerror(rc));
			return -1;
		}
		for (i = 0u; i < sizeof back; i++) {
			uint8_t want = 0xFFu;

			for (w = 0u; w <= step; w++) {
				if (i >= off[w] && i < off[w] + sizeof pat[w])
					want = pat[w][i - off[w]];
			}
			if (back[i] != want) {
				cli_error(sh, "  after program %lu: +%lu = 0x%02X, "
				          "expected 0x%02X\r\n", (unsigned long)step,
				          (unsigned long)i, back[i], want);
				return -1;
			}
		}
		cli_print(sh, "  program %lu (+%lu, %u B): page intact\r\n",
		          (unsigned long)step, (unsigned long)off[step],
		          (unsigned)sizeof pat[step]);
	}
	return 0;
}

/*
 * Observation only: re-program a byte that has already been programmed.  This is
 * what FDB_WRITE_GRAN = 1 would need and what the datasheet does not promise, so
 * what the byte reads is reported and never turned into a pass/fail -- the shipped
 * configuration does not depend on it.  Recording the answer here is what makes a
 * future decision about gran = 1 a measurement rather than a guess.  A write or
 * read that FAILS is another matter: that is the device not answering, and the
 * test fails on it (-1).
 */
static int nor_test_overwrite(struct cli_instance *sh, uint32_t addr)
{
	static const uint8_t seq[2] = {0xFEu, 0xFCu};
	uint8_t got[2] = {0u, 0u};
	uint32_t i;
	int rc;

	for (i = 0u; i < 2u; i++) {
		rc = nor_write(addr, &seq[i], 1u);
		if (rc == NOR_OK)
			rc = nor_read(addr, &got[i], 1u);
		if (rc != NOR_OK) {
			cli_error(sh, "  overwrite write/read %lu failed: %s\r\n",
			          (unsigned long)i, nor_strerror(rc));
			return -1;
		}
	}
	cli_print(sh, "  0xFF -> write 0xFE -> read 0x%02X -> write 0xFC -> read 0x%02X%s\r\n",
	          got[0], got[1],
	          (got[0] == 0xFEu && got[1] == 0xFCu) ? "  (accepted)"
	                                               : "  (NOT accepted)");
	return 0;
}

/*
 * The destructive part, on a sector that read blank: erase -> blank check ->
 * the partial page programs -> the overwrite observation -> erase -> blank
 * check.  0, or -1 after reporting what failed; the sector may then be left
 * programmed, which the caller says.
 */
static int nor_test_steps(struct cli_instance *sh, uint32_t addr)
{
	int rc = nor_erase(addr, NOR_SECTOR_SIZE);

	if (rc != NOR_OK) {
		cli_error(sh, "nor: FAIL -- erase failed: %s\r\n", nor_strerror(rc));
		return -1;
	}
	cli_print(sh, "erase: OK\r\n");
	if (nor_check_erased(sh, addr, NOR_SECTOR_SIZE) != 0) {
		cli_error(sh, "nor: FAIL -- sector is not blank after erase\r\n");
		return -1;
	}
	cli_print(sh, "blank check: OK (4096 B all 0xFF)\r\n");

	cli_print(sh, "partial page programs (what FDB_WRITE_GRAN=8 needs):\r\n");
	if (nor_test_partial(sh, addr)) {
		cli_error(sh, "nor: FAIL -- partial page programming disturbs the page\r\n");
		return -1;
	}

	cli_print(sh, "same-byte overwrite (observation only, gran=1 would need it):\r\n");
	if (nor_test_overwrite(sh, addr + 64u)) {
		cli_error(sh, "nor: FAIL -- the device did not complete a write or read "
		              "of the overwrite observation\r\n");
		return -1;
	}

	/* Leave it as it was found: blank. */
	rc = nor_erase(addr, NOR_SECTOR_SIZE);
	if (rc != NOR_OK) {
		cli_error(sh, "nor: FAIL -- final erase failed: %s\r\n", nor_strerror(rc));
		return -1;
	}
	if (nor_check_erased(sh, addr, NOR_SECTOR_SIZE) != 0) {
		cli_error(sh, "nor: FAIL -- sector is not blank after the final erase\r\n");
		return -1;
	}
	cli_print(sh, "final erase: OK (left blank)\r\n");
	return 0;
}

/* The whole test, under the busy flag the caller holds. */
static int nor_test_run(struct cli_instance *sh, uint32_t addr)
{
	uint8_t id[3];
	int rc;

	nor_flash_get_id(id);
	cli_print(sh, "nor test: sector 0x%06lX (must read blank; 4 KB will be "
	              "ERASED and left blank)\r\n", (unsigned long)addr);
	cli_print(sh, "jedec %02X %02X %02X @ %lu Hz\r\n", id[0], id[1], id[2],
	          (unsigned long)nor_flash_clock_hz());

	/* [!] Before anything is erased: refuse to test over data. */
	rc = nor_check_erased(sh, addr, NOR_SECTOR_SIZE);
	if (rc > 0) {
		cli_error(sh, "nor: 4 KB at 0x%06lX is not blank -- not testing over "
		              "data; `nor erase 0x%06lX` first if it is scratch\r\n",
		          (unsigned long)addr, (unsigned long)addr);
		return 1;
	}
	if (rc < 0) {
		cli_error(sh, "nor: FAIL -- could not read the sector; nothing was "
		              "erased\r\n");
		return 1;
	}
	cli_print(sh, "blank before the test: OK\r\n");

	if (nor_test_steps(sh, addr) != 0) {
		cli_error(sh, "nor: the 4 KB at 0x%06lX may not be blank now -- "
		              "`nor erase 0x%06lX` before the next `nor test`\r\n",
		          (unsigned long)addr, (unsigned long)addr);
		return 1;
	}
	cli_print(sh, "nor: PASS\r\n");
	return 0;
}

static int cmd_nor_test(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr = NOR_TEST_DEFAULT_ADDR;
	int rc;

	if (argc > 1 && nor_parse_addr(sh, argv[1], &addr))
		return 1;
	addr -= addr % NOR_SECTOR_SIZE;
	if (NOR_SIZE_BYTES - addr < NOR_SECTOR_SIZE) {
		cli_error(sh, "nor: no room for a sector at 0x%06lX\r\n",
		          (unsigned long)addr);
		return 1;
	}

	if (nor_admit(sh, addr, NOR_SECTOR_SIZE))
		return 1;
	rc = nor_test_run(sh, addr);
	blob_busy_release();
	return rc;
}

#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_SUBCMD_SET_CREATE(nor_subcmds,
	CLI_CMD_ARG(info,  NULL, "bring-up state, JEDEC id, geometry, clock", cmd_nor_info,  1, 0),
	CLI_CMD_ARG_USAGE(read,  NULL, "hexdump <addr> [len]",
	                  "<addr> [len]", cmd_nor_read, 2, 1),
#if CLI_ENABLE_DANGEROUS_CMDS
	CLI_CMD_ARG_USAGE(erase, NULL, "erase 4 KB sector(s) <addr> [len]",
	                  "<addr> [len]", cmd_nor_erase, 2, 1),
	CLI_CMD_ARG_USAGE(write, NULL, "program <addr> <hex> (erase first)",
	                  "<addr> <hex>", cmd_nor_write, 3, 0),
	CLI_CMD_ARG_USAGE(test,  NULL, "partial-page-program acceptance test on a blank "
	                               "sector [addr] (defaults to "
	                               NOR_TEST_DEFAULT_STR ", above kv+blob)",
	                  "[addr]", cmd_nor_test, 1, 1),
#endif
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nor, nor_subcmds,
                 "external NOR flash (OCTOSPI2 / W25Q128)", NULL, 1, 0);
