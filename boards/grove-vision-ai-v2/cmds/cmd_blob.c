/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_blob.c
 * @brief   `blob` command: the asset store's read side (issue #92, #49 Step 2).
 *
 *   blob list           every slot: state, size, and what its header says
 *   blob info <slot>    one slot in full, including why an invalid one is
 *   blob read <slot>    hexdump payload bytes
 *   blob free           which slots can take something, and what cannot
 *
 * The reads take NOR_LEASE_BLOB, which is also what brings the window up, and
 * give it back before they print.  `write` and `erase` take the writer
 * RESERVATION instead (#91) and hold it across everything they do -- which is
 * also what serialises them against each other and against `nor erase`, so
 * there is no separate busy flag here: a second mutating command is refused by
 * the reservation, in the same words `nor erase` uses.
 *
 * [!] THE FIRST BRING-UP IS NOT FREE, and this file makes it reachable from
 * four more places.  Taking a lease on a port that has never been up runs the
 * vendor's quad-enable, which writes the QE bit of the NOR's non-volatile
 * status register (cmd_nor.c says the same about `nor info`).  Nothing here
 * touches the ARRAY.
 *
 * [!] AND "EMPTY" IS ABOUT THE HEADER.  Every listing says so, because it is
 * the one thing an operator will get wrong: this board ships with factory
 * SenseCraft data inside the region -- a FlashDB KVDB at 0x300000 and more at
 * 0x400000 and 0x500000 -- and a slot whose two header pages happen to be
 * erased reads `empty` with all of that still underneath it.
 */
#include <stdint.h>
#include <string.h>

#include "cli.h"
#include "cli_instance.h"

#include "blob.h"
#include "blob_write.h"
#include "cmd_xfer.h"
#include "log.h"
#include "nor_cmd.h"
#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_write.h"

/* Same cap as `devmem dump`, and for the same reason: the hexdump holds the
 * output path for as long as it takes to print, and a length nobody bounded
 * would be a way to make the console unusable from one command. */
#ifndef BLOB_READ_MAX_LEN
#define BLOB_READ_MAX_LEN  CLI_DEVMEM_DUMP_MAX_LEN
#endif

/* ---- shared reporting ----------------------------------------------------- */

/* One place for the refusals, so that a slot number, a broken table and a busy
 * part do not each get their own wording in four commands. */
static int blob_complain(struct cli_instance *sh, int rc)
{
	unsigned bad = 0u;
	enum blob_map_verdict v;

	switch (rc) {
	case BLOB_OK:
		return 0;
	case BLOB_ERR_PARAM:
		cli_error(sh, "blob: no such slot (0..%u), or a range outside "
		              "the payload\r\n", blob_map_count() - 1u);
		return 1;
	case BLOB_ERR_BUSY:
		/* `nor info` names the holder; repeating the mapping here would
		 * be a second copy of it to keep in step. */
		cli_error(sh, "blob: the NOR is busy -- `nor info` says who "
		              "holds it\r\n");
		return 1;
	case BLOB_ERR_FAULT:
		/* Not busy, and not worth retrying: the port latched a failure
		 * and only a reset clears it.  Sending an operator to look for
		 * a holder that does not exist is the wrong instruction. */
		cli_error(sh, "blob: the NOR port is faulted -- %s (reset "
		              "required)\r\n",
		          nor_fail_reason() ? nor_fail_reason() : "no reason "
		          "recorded");
		return 1;
	case BLOB_ERR_MAP:
		v = blob_check_map(&bad);
		if (blob_map_verdict_names_slot(v))
			cli_error(sh, "blob: slot table is not usable: %s "
			              "(slot %u)\r\n",
			          blob_map_verdict_name(v), bad);
		else
			cli_error(sh, "blob: slot table is not usable: %s\r\n",
			          blob_map_verdict_name(v));
		return 1;
	default:
		break;
	}
	cli_error(sh, "blob: failed (%d)\r\n", rc);
	return 1;
}

/* Sizes are printed in whole KB because every slot and every header is a whole
 * number of 4 KB erase units; a byte count would be four more columns of zeros
 * on every row. */
static void print_row(struct cli_instance *sh, unsigned slot,
                      const struct blob_slot *s, const struct blob_info *info)
{
	cli_print(sh, "%4u  0x%08lx  %5luK  %-10s", slot,
	          (unsigned long)s->base, (unsigned long)(s->size / 1024u),
	          blob_slot_state_name(info->state));
	if (info->state == BLOB_VALID || info->state == BLOB_INCOMPLETE)
		cli_print(sh, "%10lu  %08lX  %s\r\n",
		          (unsigned long)info->length,
		          (unsigned long)info->crc32, info->name);
	else
		cli_print(sh, "%10s  %8s  %s\r\n", "--", "--", "--");
}

/* ---- list ----------------------------------------------------------------- */

static int cmd_blob_list(struct cli_instance *sh, int argc, char **argv)
{
	unsigned i, count;

	(void)argc;
	(void)argv;

	if (blob_check_map(NULL) != BLOB_MAP_OK)
		return blob_complain(sh, BLOB_ERR_MAP);
	count = blob_map_count();

	/* Spelled out to match print_row()'s field widths exactly: 4, 10, 6 and
	 * 10 with two spaces between, then the length hard against the state
	 * column.  A header that drifted from the rows is the sort of thing
	 * nobody notices and everybody has to re-count. */
	cli_print(sh, "slot  base          size  state         length     "
	              "crc32  name\r\n");
	for (i = 0u; i < count; i++) {
		struct blob_info info;
		int rc = blob_stat(i, &info, NULL);

		if (rc != BLOB_OK)
			return blob_complain(sh, rc);
		print_row(sh, i, blob_map_slot(i), &info);
	}

	cli_print(sh, "[!] empty = no blob header.  The payload underneath may "
	              "still hold\r\n"
	              "    data -- this board ships with factory content in "
	              "this region.\r\n"
	              "    invalid = bytes this port cannot read; `blob info "
	              "<slot>` says why.\r\n");
	return 0;
}

/* ---- info ----------------------------------------------------------------- */

static int cmd_blob_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct blob_slot *s;
	struct blob_info info;
	enum blob_hdr_reject why = BLOB_REJECT_NONE;
	uint32_t slot, unit = nor_seam_limits.unit;
	int rc;

	(void)argc;

	if (cli_parse_u32(argv[1], &slot) != 0)
		return blob_complain(sh, BLOB_ERR_PARAM);

	rc = blob_stat((unsigned)slot, &info, &why);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	s = blob_map_slot((unsigned)slot);

	cli_print(sh, "slot     : %lu\r\n", (unsigned long)slot);
	cli_print(sh, "extent   : 0x%08lx .. 0x%08lx (%lu KB)\r\n",
	          (unsigned long)s->base, (unsigned long)(s->base + s->size),
	          (unsigned long)(s->size / 1024u));
	cli_print(sh, "header   : 0x%08lx .. 0x%08lx (one erase unit)\r\n",
	          (unsigned long)s->base, (unsigned long)(s->base + unit));
	cli_print(sh, "payload  : 0x%08lx .. 0x%08lx (%lu B)\r\n",
	          (unsigned long)blob_payload_addr((unsigned)slot),
	          (unsigned long)(s->base + s->size),
	          (unsigned long)blob_payload_max((unsigned)slot));
	cli_print(sh, "state    : %s\r\n", blob_slot_state_name(info.state));

	switch (info.state) {
	case BLOB_VALID:
	case BLOB_INCOMPLETE:
		cli_print(sh, "name     : %s\r\n", info.name);
		cli_print(sh, "length   : %lu B\r\n", (unsigned long)info.length);
		/* Said out loud because it is what `blob verify` will compare
		 * against, and it is a claim about the transfer rather than
		 * about what is in the flash now. */
		cli_print(sh, "crc32    : %08lX (of the payload as it was "
		              "sent)\r\n", (unsigned long)info.crc32);
		if (info.state == BLOB_INCOMPLETE)
			cli_print(sh, "         : the body is there and the "
			              "magic is not -- a transfer that did "
			              "not finish\r\n");
		break;
	case BLOB_INVALID:
		cli_print(sh, "reason   : %s\r\n", blob_hdr_reject_name(why));
		break;
	case BLOB_EMPTY:
	default:
		cli_print(sh, "         : no blob header.  The payload may "
		              "still hold data this port did not write\r\n");
		break;
	}
	return 0;
}

/* ---- read ----------------------------------------------------------------- */

static int cmd_blob_read(struct cli_instance *sh, int argc, char **argv)
{
	/* On the stack, not static: the lease serialises the READ, but not the
	 * print that follows it, so a background `blob read` could refill a
	 * shared buffer while the foreground one was still printing out of it.
	 * BLOB_READ_MAX_LEN is 256 against a 4 KB thread stack. */
	uint8_t buf[BLOB_READ_MAX_LEN];
	uint32_t slot, off = 0u, len = 64u;
	int rc;

	if (cli_parse_u32(argv[1], &slot) != 0 ||
	    (argc >= 3 && cli_parse_u32(argv[2], &off) != 0) ||
	    (argc >= 4 && cli_parse_u32(argv[3], &len) != 0))
		return blob_complain(sh, BLOB_ERR_PARAM);
	if (len == 0u || len > sizeof buf) {
		cli_error(sh, "blob: length must be 1..%lu\r\n",
		          (unsigned long)sizeof buf);
		return 1;
	}

	rc = blob_read((unsigned)slot, off, buf, len);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);

	/* The offset column counts from the payload, not from the flash, so
	 * what it prints lines up with the length and CRC in `blob info`. */
	cli_hexdump_base(sh, buf, len, off);
	return 0;
}

/* ---- free ----------------------------------------------------------------- */

static int cmd_blob_free(struct cli_instance *sh, int argc, char **argv)
{
	unsigned i, count, usable = 0u;
	uint32_t largest = 0u, spare;

	(void)argc;
	(void)argv;

	if (blob_check_map(NULL) != BLOB_MAP_OK)
		return blob_complain(sh, BLOB_ERR_MAP);
	count = blob_map_count();

	cli_print(sh, "slot  base          size  state\r\n");
	for (i = 0u; i < count; i++) {
		const struct blob_slot *s = blob_map_slot(i);
		struct blob_info info;
		int rc = blob_stat(i, &info, NULL);

		if (rc != BLOB_OK)
			return blob_complain(sh, rc);
		/* What `blob write` will take without an erase first: a slot
		 * with no header, or one holding a header this port wrote and
		 * never finished.  Kept in step with blob_choose_target() by
		 * saying so here rather than by re-deriving its rule. */
		if (info.state != BLOB_EMPTY && info.state != BLOB_INCOMPLETE)
			continue;
		cli_print(sh, "%4u  0x%08lx  %5luK  %s\r\n", i,
		          (unsigned long)s->base,
		          (unsigned long)(s->size / 1024u),
		          blob_slot_state_name(info.state));
		usable++;
		if (blob_payload_max(i) > largest)
			largest = blob_payload_max(i);
	}

	cli_print(sh, "free     : %u slot(s), largest payload %lu B\r\n",
	          usable, (unsigned long)largest);

	/* [!] Printed apart from the free slots, and never added into them.
	 * Nothing can be put here: no slot names these bytes, so no command
	 * can aim at them and the writer would refuse an address it cannot
	 * resolve to a slot. */
	spare = blob_map_uncarved(nor_seam_limits.lo, nor_seam_limits.hi,
	                          blob_map_table(), count);
	cli_print(sh, "uncarved : %lu B above 0x%08lx -- reserved by the seam, "
	              "named by no slot\r\n", (unsigned long)spare,
	              (unsigned long)(nor_seam_limits.hi - spare));
	return 0;
}


/* ---- write ----------------------------------------------------------------- */

/* Everything the coordinator needs from the board, and nothing else.  It is a
 * vtable because the unwinding is what has to be tested and hardware can
 * demonstrate about two of the seven ways out (blob_write.h). */
struct wr_ctx {
	struct cli_instance *sh;
	int console_bg;      /**< the claim was refused because we are a bg job */
};

/* Runs with the memory-mapped window DOWN, so it is held to what nor_write.h
 * permits there: the cancel poll reads the RX ring and shell state, and
 * cli_print() goes to the UART.  Nothing here may touch the alias. */
#define BLOB_ERASE_TICK_EVERY  (64u * 0x1000u)

static int wr_erase_tick(void *ctx, uint32_t done, uint32_t total)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (total > BLOB_ERASE_TICK_EVERY && (done % BLOB_ERASE_TICK_EVERY) == 0u)
		cli_print(sh, "erasing  : %lu / %lu B\r\n", (unsigned long)done,
		          (unsigned long)total);
	return cli_cancel_requested(sh) ? 1 : 0;
}

static int op_reserve(void *ctx, uint32_t *token)
{
	struct wr_ctx *c = (struct wr_ctx *)ctx;

	return nor_cmd_writer_enter(c->sh, token);
}

static void op_unreserve(void *ctx, uint32_t token)
{
	(void)ctx;
	(void)nor_unreserve(token);
}

static int op_erase(void *ctx, uint32_t token, uint32_t addr, uint32_t len,
                    uint32_t *done, int *cancelled)
{
	struct wr_ctx *c = (struct wr_ctx *)ctx;
	struct nor_erase_progress prog = { c->sh, wr_erase_tick };
	struct nor_write_report r;
	enum nor_write_status st = nor_write_erase(token, addr, len, &prog, &r);

	*done = r.done;
	*cancelled = r.cancelled ? 1 : 0;
	if (st != NOR_WRITE_OK)
		log_write(LOG_LEVEL_ERR, "blob", "erase 0x%lx+%lu: %s (%lu done)",
		          (unsigned long)addr, (unsigned long)len,
		          nor_write_status_name(st), (unsigned long)r.done);
	return (int)st;
}

static int op_program(void *ctx, uint32_t token, uint32_t addr,
                      const void *data, uint32_t len)
{
	struct wr_ctx *c = (struct wr_ctx *)ctx;
	struct nor_write_report r;
	enum nor_write_status st = nor_write_program(token, addr, data, len, &r);

	(void)c;
	if (st != NOR_WRITE_OK) {
		log_write(LOG_LEVEL_ERR, "blob", "program 0x%lx+%lu: %s (%lu done)",
		          (unsigned long)addr, (unsigned long)len,
		          nor_write_status_name(st), (unsigned long)r.done);
		/* [!] THE MISMATCHING BYTE, not just the verdict.  This runs with
		 * the console handed to the protocol, so the log is the only place
		 * it can be said -- and "read 0x%02x, wanted 0x%02x" at a known
		 * offset is the difference between knowing what went wrong and
		 * spending a flash cycle guessing. */
		if (r.bad_valid)
			log_write(LOG_LEVEL_ERR, "blob",
			          "first bad at 0x%lx: read %02x, wanted %02x",
			          (unsigned long)r.bad_off, r.bad_got, r.bad_want);
	}
	return (int)st;
}

static int op_claim_console(void *ctx)
{
	struct wr_ctx *c = (struct wr_ctx *)ctx;
	int rc = cli_console_claim(c->sh);

	c->console_bg = (rc == -2);
	return rc == 0 ? 0 : -1;
}

static void op_release_console(void *ctx)
{
	cli_console_release(((struct wr_ctx *)ctx)->sh);
}

static int op_receive(void *ctx, const struct ym_sink *sink)
{
	return xfer_recv_sink_locked(((struct wr_ctx *)ctx)->sh, sink);
}

static int op_read_back(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	(void)ctx;
	return blob_read_reserved(addr, buf, len) == BLOB_OK ? 0 : -1;
}

/* ~80 ms per 4 KB sector, measured on this die (#49 Step 2 item 6; the table is
 * in the board README).  Only ever used to set an expectation before a wait
 * that can reach 40 s -- nothing decides anything from it. */
#define BLOB_ERASE_MS_PER_SECTOR  80u

static void op_announce(void *ctx, unsigned slot, uint32_t base, uint32_t bytes)
{
	struct cli_instance *sh = ((struct wr_ctx *)ctx)->sh;
	unsigned long secs = ((unsigned long)(bytes / nor_seam_limits.unit) *
	                      BLOB_ERASE_MS_PER_SECTOR + 999u) / 1000u;

	/* Said here rather than at the top of the command: everything that can
	 * refuse has refused by now, so this is only printed when a slot really is
	 * about to be destroyed -- and it can name which one. */
	cli_print(sh, "slot %u   : 0x%08lx, %lu KB -- erasing it first, so whatever "
	              "is there\r\n", slot, (unsigned long)base,
	          (unsigned long)(bytes / 1024u));
	cli_print(sh, "           is gone even if the transfer fails.  About "
	              "%lu s; Ctrl+C stops it between sectors\r\n", secs);
	cli_print(sh, "           then start the sender: `sb -k <file>` (lrzsz "
	              "YMODEM batch), or Ctrl+A Ctrl+S in picocom\r\n");
	cli_print(sh, "           Ctrl+C does NOT abort the transfer -- cancel the "
	              "sender instead.  Result also in `dmesg`\r\n");
}

static void op_note_name(void *ctx, const char *name, uint32_t size)
{
	(void)ctx;
	/* [!] TO THE LOG, NOT THE CONSOLE: the PC's terminal belongs to `sb`
	 * while this runs.  And noted rather than stored -- the key is the name
	 * the operator typed. */
	log_write(LOG_LEVEL_INF, "blob", "sender offered '%s' (%lu B)",
	          name ? name : "", (unsigned long)size);
}

static unsigned op_slot_count(void *ctx)
{
	(void)ctx;
	return blob_map_count();
}

static int op_stat(void *ctx, unsigned slot, struct blob_info *info)
{
	(void)ctx;
	return blob_stat_reserved(slot, info, NULL) == BLOB_OK ? 0 : -1;
}

static int op_geometry(void *ctx, unsigned slot, uint32_t *base,
                       uint32_t *payload_addr, uint32_t *payload_max)
{
	(void)ctx;
	return blob_slot_geometry(slot, base, payload_addr, payload_max) ==
	       BLOB_OK ? 0 : -1;
}

static int cmd_blob_write(struct cli_instance *sh, int argc, char **argv)
{
	struct wr_ctx ctx = { sh, 0 };
	const struct blob_write_ops ops = {
		&ctx, op_reserve, op_unreserve, op_erase, op_program,
		op_claim_console, op_release_console, op_receive, op_read_back,
		op_note_name, op_announce, op_slot_count, op_stat, op_geometry,
	};
	struct blob_write_report rep;
	enum blob_write_result res;
	enum blob_name_verdict nv;
	uint32_t want32, drops0;
	int want = -1;

	nv = blob_name_check(argv[1], NULL);
	if (nv != BLOB_NAME_OK) {
		cli_error(sh, "blob: %s\r\n", blob_name_verdict_name(nv));
		return 1;
	}
	if (argc >= 3) {
		if (cli_parse_u32(argv[2], &want32) != 0)
			return blob_complain(sh, BLOB_ERR_PARAM);
		want = (int)want32;
	}

	/* [!] Nothing is printed here.  What this command has to say -- that a
	 * slot is about to be erased and the operator should start the sender --
	 * is only true once a slot has been CHOSEN, and choosing happens inside
	 * the reservation.  op_announce() says it at that moment. */
	drops0 = sh->rx_dropped;
	res = blob_write_run(&ops, argv[1], want, &rep);

	/* The post-mortem goes to the log as well: after a cancel the console
	 * shows none of this, and during the transfer the PC could not see it. */
	log_write(res == BLOB_WRITE_STORED ? LOG_LEVEL_INF : LOG_LEVEL_ERR, "blob",
	          "write '%s' slot %u: %s, %lu B, %lu tx, drops %lu",
	          argv[1], rep.slot, blob_write_result_name(res),
	          (unsigned long)rep.received, (unsigned long)rep.transactions,
	          (unsigned long)(sh->rx_dropped - drops0));

	if (res == BLOB_WRITE_STORED) {
		cli_print(sh, "stored   : '%s' in slot %u, %lu B, crc32 %08lX\r\n",
		          argv[1], rep.slot, (unsigned long)rep.received,
		          (unsigned long)rep.crc);
		cli_print(sh, "cost     : %lu NOR transaction(s), rx_drop +%lu\r\n",
		          (unsigned long)rep.transactions,
		          (unsigned long)(sh->rx_dropped - drops0));
		return 0;
	}

	if (res == BLOB_WRITE_NO_SLOT) {
		cli_error(sh, "blob: %s\r\n", blob_choice_name(rep.choice));
		if (rep.choice == BLOB_CHOICE_NEED_SLOT)
			cli_error(sh, "      say which one: `blob write %s <slot>` "
			              "(`blob free` lists them)\r\n", argv[1]);
		else if (rep.choice == BLOB_CHOICE_OCCUPIED ||
		         rep.choice == BLOB_CHOICE_DUPLICATE)
			cli_error(sh, "      `blob erase <slot>` first, on purpose\r\n");
		return 1;
	}
	if (res == BLOB_WRITE_NO_CONSOLE && ctx.console_bg)
		cli_error(sh, "blob: cannot run in the background -- drop the "
		              "trailing '&'\r\n");
	else
		cli_error(sh, "blob: %s (%lu B erased, %lu B received, %lu tx)\r\n",
		          blob_write_result_name(res), (unsigned long)rep.erased,
		          (unsigned long)rep.received,
		          (unsigned long)rep.transactions);
	return 1;
}

/* ---- erase ----------------------------------------------------------------- */

static int cmd_blob_erase(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_erase_progress prog = { sh, wr_erase_tick };
	struct nor_write_report r;
	enum nor_write_status st;
	uint32_t slot, base = 0u, token = 0u;
	int rc;

	(void)argc;
	if (cli_parse_u32(argv[1], &slot) != 0)
		return blob_complain(sh, BLOB_ERR_PARAM);
	rc = blob_slot_geometry((unsigned)slot, &base, NULL, NULL);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);

	if (nor_cmd_writer_enter(sh, &token) != 0)
		return 1;

	/* [!] THE HEADER SECTOR ONLY.  Retiring a blob is one 4 KB erase rather
	 * than the whole slot: the payload underneath is left alone and the next
	 * `blob write` erases it anyway.  So this is fast, and `blob list` stops
	 * showing the blob immediately -- which is what "erase" means to an
	 * operator -- while the bytes are still there for anyone who reads the
	 * flash directly.  Said out loud below for that reason. */
	cli_print(sh, "erasing  : slot %lu header at 0x%08lx (one sector)\r\n",
	          (unsigned long)slot, (unsigned long)base);
	st = nor_write_erase(token, base, nor_seam_limits.unit, &prog, &r);
	(void)nor_unreserve(token);

	if (st != NOR_WRITE_OK) {
		cli_error(sh, "blob: erase %s\r\n", nor_write_status_name(st));
		return 1;
	}
	cli_print(sh, "erased   : the header is gone; the payload is still in the "
	              "flash until something writes over it\r\n");
	return 0;
}

/* ---- verify ---------------------------------------------------------------- */

static int cmd_blob_verify(struct cli_instance *sh, int argc, char **argv)
{
	struct blob_info info;
	uint32_t slot, crc = 0u;
	int rc;

	(void)argc;
	if (cli_parse_u32(argv[1], &slot) != 0)
		return blob_complain(sh, BLOB_ERR_PARAM);
	if (blob_stat((unsigned)slot, &info, NULL) != BLOB_OK)
		return blob_complain(sh, BLOB_ERR_BUSY);

	rc = blob_verify((unsigned)slot, &crc);
	switch (rc) {
	case BLOB_OK:
		cli_print(sh, "slot %lu  : PASS, %lu B, crc32 %08lX\r\n",
		          (unsigned long)slot, (unsigned long)info.length,
		          (unsigned long)crc);
		return 0;
	case BLOB_ERR_CRC:
		/* The stored CRC is of the stream that arrived, so a mismatch
		 * means the flash and the PC's file differ -- not that the flash
		 * disagrees with itself. */
		cli_error(sh, "slot %lu  : FAIL, read back %08lX, header says "
		              "%08lX\r\n", (unsigned long)slot,
		          (unsigned long)crc, (unsigned long)info.crc32);
		return 1;
	case BLOB_ERR_EMPTY:
		cli_error(sh, "blob: slot %lu holds no header to check against\r\n",
		          (unsigned long)slot);
		return 1;
	default:
		break;
	}
	return blob_complain(sh, rc);
}

CLI_SUBCMD_SET_CREATE(blob_subcmds,
	CLI_CMD_ARG_USAGE(list, NULL, "every slot: state, size, length, crc32, name",
	                  NULL, cmd_blob_list, 1, 0),
	CLI_CMD_ARG_USAGE(info, NULL, "one slot in full, and why it is invalid",
	                  "<slot>", cmd_blob_info, 2, 0),
	CLI_CMD_ARG_USAGE(read, NULL, "hexdump payload bytes",
	                  "<slot> [off] [len]", cmd_blob_read, 2, 2),
	CLI_CMD_ARG_USAGE(free, NULL, "slots that can take something, and what cannot",
	                  NULL, cmd_blob_free, 1, 0),
	CLI_CMD_ARG_USAGE(write, NULL, "receive a file over YMODEM into a slot",
	                  "<name> [slot]", cmd_blob_write, 2, 1),
	CLI_CMD_ARG_USAGE(verify, NULL, "re-read a slot and check its stored crc32",
	                  "<slot>", cmd_blob_verify, 2, 0),
	CLI_CMD_ARG_USAGE(erase, NULL, "retire a blob: erase its header sector",
	                  "<slot>", cmd_blob_erase, 2, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(blob, blob_subcmds,
                 "assets stored in the external NOR", NULL, 1, 0);
