/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_blob.c
 * @brief   The one `blob` command for every board's asset store (issue #194).
 *
 *   blob list                    every slot: base, size, state, length, CRC32, name
 *   blob info <slot>             one slot in full, and why an invalid one is
 *   blob read <slot> [off] [len] hexdump of the payload (debugging)
 *   blob free                    which slots `blob write` can take without an erase
 *   blob write <name> [slot]     erase a slot, receive a file over YMODEM, verify
 *   blob verify <slot>           re-read the payload and check it against the CRC32
 *   blob erase <slot>            retire a blob: erase its header unit
 *
 * Until issue #194 wio-lite-ai and grove-vision-ai-v2 each had their own copy of
 * this file; the words are wio-lite-ai's.  The store is reached through
 * svc/blob/blob_svc.h, the board's three console pieces through
 * shell/cmds/blob_cmd_board.h, and the order of one write is
 * svc/blob/blob_write.c.
 *
 * [!] THE NAME IS THE OPERATOR'S, NOT THE SENDER'S.  `blob write <name> [slot]`
 * stores what was typed; the YMODEM filename only goes to the log.  A name
 * already stored goes back to its own slot, a new name has to say which slot,
 * and a slot holding a blob under another name -- or bytes this firmware cannot
 * read -- is refused until `blob erase <slot>` clears it on purpose.
 *
 * [!] A SHARED TRANSLATION UNIT OWNS NO MUTABLE STORAGE
 * (cmake/check_no_mutable_storage.py, per board).  The read buffer and the
 * header pages are on the caller's stack, the staging buffer is the board's
 * (blob_svc_stage()), and the write's state is the coordinator's frame.
 *
 * The board's words, from its svc/blob_svc_config.h:
 *   BLOB_SVC_PART          "NOR" / "QSPI": the part, in messages
 *   BLOB_SVC_ADDR_FMT      how a flash address prints ("0x%06lx")
 *   BLOB_SVC_BASE_HEAD     the "base" column header, padded to that width
 *   BLOB_SVC_ERASE_MS_TYP  a 64 KB erase, typical ms (an expectation only)
 *   BLOB_SVC_ERASE_MS_MAX  ... and at most, or 0 when not known
 *   BLOB_SVC_CANCEL_UNIT   what Ctrl+C stops an erase between
 *   BLOB_SVC_SENDER        how to start the sender on the PC
 *   BLOB_SVC_READ_MAX      the longest `blob read`
 *   BLOB_SVC_LIST_NOTE     the board's note under `blob list`
 *   BLOB_SVC_HELP          the command's one line in `help`
 */
#include "blob_svc_config.h"

#if BLOB_SVC_HAS_STORE && BLOB_SVC_HAS_RECEIVE

#include <stdint.h>
#include <string.h>

#include "cli.h"
/* The full struct cli_instance: `blob write` reads sh->rx_dropped (the one loss
 * YMODEM's counters cannot attribute) and sh->fg (a background job can never
 * claim the console). */
#include "cli_instance.h"
#include "blob_cmd_board.h"
#include "blob_svc.h"
#include "blob_write.h"
#include "log.h"
#include "ymodem.h"

/* One `blob read` piece on the stack; the whole read is at most
 * BLOB_SVC_READ_MAX. */
#define BLOB_CMD_READ_CHUNK    256u
#define BLOB_CMD_READ_DEFAULT  64u

/* Erase progress every 256 KB, so a 1 MB slot says something four times. */
#define BLOB_CMD_ERASE_TICK_EVERY  (256u * 1024u)

/* ---- shared reporting ----------------------------------------------------- */

/* One place for the refusals, so that a slot number, a broken table and a busy
 * store do not each get their own wording in seven subcommands.  The board's
 * sentence comes first where it has one (blob_board_say()). */
static int blob_complain(struct cli_instance *sh, int rc)
{
	unsigned bad = 0u;
	enum blob_map_verdict v;

	if (rc == BLOB_OK)
		return 0;
	if (blob_board_say(sh, rc))
		return 1;
	switch (rc) {
	case BLOB_ERR_PARAM:
		cli_error(sh, "blob: no such slot (0..%u), or a range outside the "
		              "payload\r\n", blob_map_count() - 1u);
		return 1;
	case BLOB_ERR_IO:
		cli_error(sh, "blob: " BLOB_SVC_PART " read/program/erase "
		              "failed\r\n");
		return 1;
	case BLOB_ERR_STATE:
		cli_error(sh, "blob: " BLOB_SVC_PART " device down\r\n");
		return 1;
	case BLOB_ERR_BUSY:
		cli_error(sh, "blob: another blob operation is running\r\n");
		return 1;
	case BLOB_ERR_FAULT:
		cli_error(sh, "blob: the " BLOB_SVC_PART " port is faulted "
		              "(reset required)\r\n");
		return 1;
	case BLOB_ERR_EMPTY:
		cli_error(sh, "blob: that slot holds no header to check against\r\n");
		return 1;
	case BLOB_ERR_MAP:
		v = blob_check_map(&bad);
		if (blob_map_verdict_names_slot(v))
			cli_error(sh, "blob: slot table is not usable: %s (slot %u)\r\n",
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

static int blob_parse_slot(struct cli_instance *sh, const char *s, unsigned *out)
{
	uint32_t n;

	/* Range-checked while it is still unsigned, against the shipped table:
	 * `want` below carries a negative sentinel for "no slot given", so
	 * 4294967295 must never become -1 and read as "reuse its slot". */
	if (cli_parse_u32(s, &n) != 0 || n >= (uint32_t)blob_map_count()) {
		(void)blob_complain(sh, BLOB_ERR_PARAM);
		return -1;
	}
	*out = (unsigned)n;
	return 0;
}

/* Sizes in whole KB: every slot is a whole number of 4 KB units. */
static void print_row(struct cli_instance *sh, unsigned slot,
                      const struct blob_slot *s, const struct blob_info *info)
{
	cli_print(sh, "%4u  " BLOB_SVC_ADDR_FMT "  %5luK  %-10s", slot,
	          (unsigned long)s->base, (unsigned long)(s->size / 1024u),
	          blob_slot_state_name(info->state));
	if (info->state == BLOB_VALID || info->state == BLOB_INCOMPLETE)
		cli_print(sh, "%10lu  %08lX  %s\r\n", (unsigned long)info->length,
		          (unsigned long)info->crc32, info->name);
	else
		cli_print(sh, "%10s  %8s  %s\r\n", "--", "--", "--");
}

/* ---- list ----------------------------------------------------------------- */

static int cmd_blob_list(struct cli_instance *sh, int argc, char **argv)
{
	unsigned i, count, used = 0u;

	(void)argc;
	(void)argv;
	if (blob_check_map(NULL) != BLOB_MAP_OK)
		return blob_complain(sh, BLOB_ERR_MAP);
	count = blob_map_count();

	/* Spelled out to match print_row()'s field widths: 4, the address, 6 and
	 * 10 with two spaces between, then the length hard against the state. */
	cli_print(sh, "slot  " BLOB_SVC_BASE_HEAD "    size  state         length"
	              "     crc32  name\r\n");
	for (i = 0u; i < count; i++) {
		struct blob_info info;
		int rc = blob_stat(i, &info, NULL);

		if (rc != BLOB_OK)
			return blob_complain(sh, rc);
		print_row(sh, i, blob_map_slot(i), &info);
		if (info.state == BLOB_VALID)
			used++;
	}
	cli_print(sh, "%u of %u slot%s hold a blob\r\n", used, count,
	          (count == 1u) ? "" : "s");
	cli_print(sh, BLOB_SVC_LIST_NOTE);
	return 0;
}

/* ---- info ----------------------------------------------------------------- */

static int cmd_blob_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct blob_slot *s;
	struct blob_info info;
	enum blob_hdr_reject why = BLOB_REJECT_NONE;
	uint32_t payload_addr = 0u, payload_max = 0u, lo, hi, unit;
	unsigned slot;
	int rc;

	(void)argc;
	if (blob_parse_slot(sh, argv[1], &slot) != 0)
		return 1;
	rc = blob_stat(slot, &info, &why);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	rc = blob_slot_geometry(slot, NULL, &payload_addr, &payload_max);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	blob_svc_interval(&lo, &hi, &unit);
	s = blob_map_slot(slot);

	cli_print(sh, "slot     : %u\r\n", slot);
	cli_print(sh, "extent   : " BLOB_SVC_ADDR_FMT " .. " BLOB_SVC_ADDR_FMT
	              " (%lu KB)\r\n",
	          (unsigned long)s->base, (unsigned long)(s->base + s->size),
	          (unsigned long)(s->size / 1024u));
	cli_print(sh, "header   : " BLOB_SVC_ADDR_FMT " .. " BLOB_SVC_ADDR_FMT
	              " (one sector)\r\n",
	          (unsigned long)s->base, (unsigned long)(s->base + unit));
	cli_print(sh, "payload  : " BLOB_SVC_ADDR_FMT " .. " BLOB_SVC_ADDR_FMT
	              " (%lu B)\r\n",
	          (unsigned long)payload_addr, (unsigned long)(s->base + s->size),
	          (unsigned long)payload_max);
	cli_print(sh, "state    : %s\r\n", blob_slot_state_name(info.state));

	switch (info.state) {
	case BLOB_VALID:
	case BLOB_INCOMPLETE:
		cli_print(sh, "name     : %s\r\n", info.name);
		cli_print(sh, "length   : %lu B\r\n", (unsigned long)info.length);
		/* Said out loud because it is what `blob verify` compares against,
		 * and it is a claim about the transfer rather than about what is in
		 * the flash now: `valid` means the HEADER decodes, not that the
		 * payload was re-read. */
		cli_print(sh, "crc32    : %08lX (of the payload as it was sent -- "
		              "`blob verify %u` checks the flash)\r\n",
		          (unsigned long)info.crc32, slot);
		if (info.state == BLOB_INCOMPLETE)
			cli_print(sh, "         : the body is there and the magic is "
			              "not -- a transfer that did not finish\r\n");
		break;
	case BLOB_INVALID:
		cli_print(sh, "reason   : %s\r\n", blob_hdr_reject_name(why));
		cli_print(sh, "         : not written by this firmware, or damaged; "
		              "`blob erase %u` clears it\r\n", slot);
		break;
	case BLOB_EMPTY:
	default:
		cli_print(sh, "         : no blob header.  The payload may still hold "
		              "data this firmware did not write\r\n");
		break;
	}
	return 0;
}

/* ---- read ----------------------------------------------------------------- */

static int cmd_blob_read(struct cli_instance *sh, int argc, char **argv)
{
	/* On the stack, not static: nothing serialises readers, so a background
	 * `blob read` could refill a shared buffer while the foreground one was
	 * still printing out of it. */
	uint8_t buf[BLOB_CMD_READ_CHUNK];
	uint32_t off = 0u, len = BLOB_CMD_READ_DEFAULT, done = 0u, cap = 0u;
	unsigned slot;
	int rc;

	if (blob_parse_slot(sh, argv[1], &slot) != 0)
		return 1;
	if ((argc >= 3 && cli_parse_u32(argv[2], &off) != 0) ||
	    (argc >= 4 && cli_parse_u32(argv[3], &len) != 0))
		return blob_complain(sh, BLOB_ERR_PARAM);
	if (len == 0u || len > (uint32_t)BLOB_SVC_READ_MAX) {
		cli_error(sh, "blob: length must be 1..%lu\r\n",
		          (unsigned long)BLOB_SVC_READ_MAX);
		return 1;
	}
	rc = blob_slot_geometry(slot, NULL, NULL, &cap);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	if (off >= cap)
		return blob_complain(sh, BLOB_ERR_PARAM);
	if (len > cap - off)
		len = cap - off;

	while (done < len) {
		uint32_t chunk = len - done;

		if (chunk > sizeof buf)
			chunk = sizeof buf;
		rc = blob_read(slot, off + done, buf, chunk);
		if (rc != BLOB_OK)
			return blob_complain(sh, rc);
		/* The offset column counts from the payload, not from the flash,
		 * so it lines up with the length in `blob info`. */
		cli_hexdump_base(sh, buf, chunk, off + done);
		done += chunk;
	}
	return 0;
}

/* ---- free ----------------------------------------------------------------- */

static int cmd_blob_free(struct cli_instance *sh, int argc, char **argv)
{
	unsigned i, count, usable = 0u;
	uint32_t largest = 0u, spare, lo, hi, unit;

	(void)argc;
	(void)argv;
	if (blob_check_map(NULL) != BLOB_MAP_OK)
		return blob_complain(sh, BLOB_ERR_MAP);
	count = blob_map_count();
	blob_svc_interval(&lo, &hi, &unit);

	cli_print(sh, "slot  " BLOB_SVC_BASE_HEAD "    size  state\r\n");
	for (i = 0u; i < count; i++) {
		const struct blob_slot *s = blob_map_slot(i);
		struct blob_info info;
		uint32_t cap;
		int rc = blob_stat(i, &info, NULL);

		if (rc != BLOB_OK)
			return blob_complain(sh, rc);
		/* What `blob write` takes without an erase first: no header, or a
		 * header this firmware wrote and never finished.  Kept in step with
		 * blob_choose_target() by saying so here rather than re-deriving
		 * its rule. */
		if (info.state != BLOB_EMPTY && info.state != BLOB_INCOMPLETE)
			continue;
		cli_print(sh, "%4u  " BLOB_SVC_ADDR_FMT "  %5luK  %s\r\n", i,
		          (unsigned long)s->base, (unsigned long)(s->size / 1024u),
		          blob_slot_state_name(info.state));
		usable++;
		cap = blob_map_payload_max(s, unit);
		if (cap > largest)
			largest = cap;
	}
	cli_print(sh, "free     : %u slot(s), largest payload %lu B\r\n", usable,
	          (unsigned long)largest);

	/* Printed apart from the free slots, never added into them: no slot names
	 * these bytes, so nothing can be stored there. */
	spare = blob_map_uncarved(lo, hi, blob_map_table(), count);
	cli_print(sh, "uncarved : %lu B of " BLOB_SVC_ADDR_FMT " .. "
	              BLOB_SVC_ADDR_FMT " named by no slot\r\n",
	          (unsigned long)spare, (unsigned long)lo, (unsigned long)hi);
	return 0;
}

/* ---- write ---------------------------------------------------------------- */

struct wr_ctx {
	struct cli_instance *sh;
	int console_bg;      /**< the claim was refused because we are a bg job */
	int reserve_rc;      /**< why the reservation was refused (BUSY / FAULT) */
};

/* Before each erase unit.  The console has not been handed to the protocol
 * yet, so Ctrl+C is the ordinary cancel and the print goes to the operator.
 * [!] On grove-vision-ai-v2 this runs with the memory-mapped window DOWN: it
 * may print and poll the cancel, and must not read the store. */
static int wr_erase_tick(void *ctx, uint32_t done, uint32_t total)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (done != 0u && total > BLOB_CMD_ERASE_TICK_EVERY &&
	    (done % BLOB_CMD_ERASE_TICK_EVERY) == 0u)
		cli_print(sh, "erasing  : %lu / %lu B\r\n", (unsigned long)done,
		          (unsigned long)total);
	return cli_cancel_requested(sh) ? 1 : 0;
}

static int op_reserve(void *ctx, uint32_t *token)
{
	struct wr_ctx *c = (struct wr_ctx *)ctx;

	c->reserve_rc = blob_svc_reserve(token);
	return c->reserve_rc == BLOB_OK ? 0 : -1;
}

static void op_unreserve(void *ctx, uint32_t token)
{
	(void)ctx;
	blob_svc_unreserve(token);
}

static int op_erase(void *ctx, uint32_t token, uint32_t addr, uint32_t len,
                    uint32_t *done, int *cancelled)
{
	return blob_svc_erase_span(token, addr, len, wr_erase_tick,
	                           ((struct wr_ctx *)ctx)->sh, done, cancelled);
}

static int op_program(void *ctx, uint32_t token, uint32_t addr,
                      const void *data, uint32_t len)
{
	(void)ctx;
	return blob_svc_program(token, addr, data, len);
}

static int op_console_ready(void *ctx)
{
	struct wr_ctx *c = (struct wr_ctx *)ctx;

	/* The same condition cli_console_claim() refuses on, asked without
	 * claiming anything: a background job's worker does not own the RX ring,
	 * and finding that out after the erase would destroy a slot to say so. */
	if (c->sh->fg != NULL) {
		c->console_bg = 1;
		return -1;
	}
	return 0;
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
	return blob_board_receive(((struct wr_ctx *)ctx)->sh, sink);
}

static int op_read_back(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	(void)ctx;
	return blob_svc_read_back(addr, buf, len);
}

static void op_announce(void *ctx, unsigned slot, uint32_t base, uint32_t bytes)
{
	struct cli_instance *sh = ((struct wr_ctx *)ctx)->sh;
	uint32_t blocks = (bytes + 0xFFFFu) / 0x10000u;
	unsigned long typ_s = ((unsigned long)blocks * BLOB_SVC_ERASE_MS_TYP +
	                       999u) / 1000u;
	unsigned long max_s = ((unsigned long)blocks * BLOB_SVC_ERASE_MS_MAX +
	                       999u) / 1000u;
	uint32_t lo, hi, unit;

	blob_svc_interval(&lo, &hi, &unit);
	/* Said here rather than at the top of the command: everything that can
	 * refuse has refused by now, so this is printed only when a slot really
	 * is about to be destroyed -- and it can name which one.  [!] "slot %-4u"
	 * keeps the colon in the column of every other label, 2-digit slots too. */
	cli_print(sh, "slot %-4u: " BLOB_SVC_ADDR_FMT ", %lu KB -- erasing it "
	              "first, so whatever is there\r\n", slot,
	          (unsigned long)base, (unsigned long)(bytes / 1024u));
	if (max_s != 0u)
		cli_print(sh, "           is gone even if the transfer fails.  About "
		              "%lu s (at most %lu s);\r\n", typ_s, max_s);
	else
		cli_print(sh, "           is gone even if the transfer fails.  About "
		              "%lu s;\r\n", typ_s);
	cli_print(sh, "           Ctrl+C stops it between " BLOB_SVC_CANCEL_UNIT
	              ".  The file may be up to %lu B\r\n",
	          (unsigned long)(bytes - unit));
	/* The one piece of operating advice that is not obvious and cannot be
	 * guessed: a receive has no local Ctrl+C at all (the stream is the file,
	 * so 0x03 is data). */
	cli_print(sh, "           then start the sender: " BLOB_SVC_SENDER ",\r\n"
	              "           or Ctrl+A Ctrl+S in picocom.  Ctrl+C does NOT "
	              "abort the transfer --\r\n"
	              "           cancel the sender instead.  Result also in "
	              "`dmesg`\r\n");
}

static void op_note_name(void *ctx, const char *name, uint32_t size)
{
	(void)ctx;
	/* [!] TO THE LOG, NOT THE CONSOLE: the PC's terminal belongs to the
	 * sender while this runs.  And noted rather than stored -- the key is
	 * the name the operator typed. */
	log_write(LOG_LEVEL_INF, "blob", "sender offered '%s' (%lu B)",
	          name ? name : "", (unsigned long)size);
}

static unsigned op_slot_count(void *ctx)
{
	(void)ctx;
	return blob_map_count();
}

/* Called by the coordinator with the reservation held, which is what makes the
 * choice it takes from these headers still true when the erase runs. */
static int op_stat(void *ctx, unsigned slot, struct blob_info *info)
{
	(void)ctx;
	return blob_svc_stat_reserved(slot, info) == BLOB_OK ? 0 : -1;
}

static int op_geometry(void *ctx, unsigned slot, uint32_t *base,
                       uint32_t *payload_addr, uint32_t *payload_max)
{
	(void)ctx;
	return blob_slot_geometry(slot, base, payload_addr, payload_max) ==
	       BLOB_OK ? 0 : -1;
}

/*
 * `blob write <name> [slot]`, through the shared coordinator.
 *
 * Refused before anything is taken: a name the rules refuse, a slot number past
 * the table, a console that may not carry a raw transfer, and a part that never
 * came up.  A file that turns out empty or larger than the slot is found only
 * once the sender's block 0 arrives, after the erase.
 */
static int cmd_blob_write(struct cli_instance *sh, int argc, char **argv)
{
	struct wr_ctx ctx = { sh, 0, BLOB_OK };
	const struct blob_write_ops ops = {
		&ctx, op_reserve, op_unreserve, op_erase, op_program,
		op_console_ready, op_claim_console, op_release_console,
		op_receive, op_read_back,
		op_note_name, op_announce, op_slot_count, op_stat, op_geometry,
	};
	struct blob_write_report rep;
	enum blob_write_result res;
	enum blob_name_verdict nv;
	uint32_t drops0, drops, stage_len = 0u;
	uint8_t *stage;
	unsigned slot;
	int want = -1;

	nv = blob_name_check(argv[1], NULL);
	if (nv != BLOB_NAME_OK) {
		cli_error(sh, "blob: %s (printable ASCII, no space, at most %u "
		              "characters)\r\n", blob_name_verdict_name(nv),
		          (unsigned)BLOB_NAME_MAX);
		return 1;
	}
	if (argc >= 3) {
		if (blob_parse_slot(sh, argv[2], &slot) != 0)
			return 1;
		want = (int)slot;
	}
	if (blob_board_raw_console(sh) != 0)
		return 1;
	if (!blob_svc_ready())
		return blob_complain(sh, BLOB_ERR_STATE);

	/* Nothing is printed here: that a slot is about to be erased is only true
	 * once one has been CHOSEN, under the reservation; op_announce() says it
	 * at that moment. */
	stage = blob_svc_stage(&stage_len);
	drops0 = sh->rx_dropped;
	res = blob_write_run(&ops, stage, stage_len, argv[1], want, &rep);
	drops = sh->rx_dropped - drops0;

	/* The post-mortem goes to the log as well: during the transfer the PC's
	 * terminal belonged to the sender, and after a Ctrl+C the shell discards
	 * what the command prints, so `dmesg` is where it can be read.  [!] A
	 * slot number only when one was chosen: a refusal before the choice
	 * leaves rep.slot at 0, which is a real slot. */
	if (rep.slot_chosen)
		log_write(res == BLOB_WRITE_STORED ? LOG_LEVEL_INF : LOG_LEVEL_ERR,
		          "blob", "write '%s' slot %u: %s, %lu B erased, %lu B "
		          "received, %lu tx, rx_drops %lu", argv[1], rep.slot,
		          blob_write_result_name(res), (unsigned long)rep.erased,
		          (unsigned long)rep.received,
		          (unsigned long)rep.transactions, (unsigned long)drops);
	else
		log_write(LOG_LEVEL_ERR, "blob", "write '%s': %s, no slot chosen",
		          argv[1], blob_write_result_name(res));

	if (res == BLOB_WRITE_STORED || res == BLOB_WRITE_XFER_FAILED ||
	    res == BLOB_WRITE_PROG_FAILED || res == BLOB_WRITE_VERIFY_FAILED) {
		/* A transfer ran: the YMODEM counters say which layer broke, and
		 * the RX drop count is the one loss they cannot see. */
		const struct ym_recv_diag *d = ymodem_recv_diag();

		cli_print(sh, "ymodem   : %lu blocks ok, %lu bad-crc, %lu bad-seq, "
		              "%lu short-read, %lu header-timeouts\r\n",
		          (unsigned long)d->blocks, (unsigned long)d->bad_crc,
		          (unsigned long)d->bad_seq, (unsigned long)d->short_read,
		          (unsigned long)d->timeouts);
		cli_print(sh, "rx drops : %lu%s\r\n", (unsigned long)drops,
		          drops ? "  <-- NOT CLEAN" : "  (clean)");
	}

	switch (res) {
	case BLOB_WRITE_STORED:
		cli_print(sh, "stored   : '%s' in slot %u, %lu B, crc32 %08lX\r\n",
		          argv[1], rep.slot, (unsigned long)rep.received,
		          (unsigned long)rep.crc);
		/* The coordinator re-read the payload against the CRC of the
		 * stream that arrived before it wrote the magic. */
		cli_print(sh, "verify   : PASS (read back %08lX)\r\n",
		          (unsigned long)rep.verified);
		return 0;
	case BLOB_WRITE_NO_SLOT:
		cli_error(sh, "blob: %s\r\n", blob_choice_name(rep.choice));
		if (rep.choice == BLOB_CHOICE_NEED_SLOT)
			cli_error(sh, "      say which one: `blob write %s <slot>` "
			              "(`blob free` lists them)\r\n", argv[1]);
		else if (rep.choice == BLOB_CHOICE_OCCUPIED ||
		         rep.choice == BLOB_CHOICE_DUPLICATE)
			cli_error(sh, "      `blob erase <slot>` first, on purpose "
			              "(`blob list` shows what is where)\r\n");
		return 1;
	case BLOB_WRITE_BUSY:
		/* The reservation was refused: busy, or -- a different
		 * instruction -- a faulted port.  What the store answered. */
		return blob_complain(sh, ctx.reserve_rc != BLOB_OK ? ctx.reserve_rc
		                                                   : BLOB_ERR_BUSY);
	case BLOB_WRITE_NO_CONSOLE:
		if (ctx.console_bg) {
			cli_error(sh, "blob: cannot run in the background -- drop "
			              "the trailing '&'\r\n");
			return 1;
		}
		break;
	case BLOB_WRITE_CANCELLED:
	case BLOB_WRITE_ERASE_FAILED:
		/* Nothing was sent and no header written.  What the slot holds now
		 * depends on how far the erase got, so say how far and point at
		 * the listing rather than guessing at its state. */
		cli_error(sh, "blob: %s after %lu B of slot %u -- nothing was sent; "
		              "`blob list` shows what is left\r\n",
		          blob_write_result_name(res), (unsigned long)rep.erased,
		          rep.slot);
		return 1;
	case BLOB_WRITE_VERIFY_FAILED:
		/* [!] The magic was NOT written (#194): the slot is left
		 * incomplete, never valid, and its body still says what was meant
		 * to be there. */
		cli_error(sh, "blob: verify FAIL -- read back %08lX, the stream was "
		              "%08lX.  Slot %u is left\r\n"
		              "      incomplete (not valid); `blob write %s %u` sends "
		              "it again\r\n",
		          (unsigned long)rep.verified, (unsigned long)rep.crc,
		          rep.slot, argv[1], rep.slot);
		return 1;
	case BLOB_WRITE_PROG_FAILED:
		if (rep.magic_failed) {
			/* The payload was read back and matched; only the page that
			 * makes the slot valid did not report OK, and a program that
			 * reports failure says nothing certain about the page. */
			cli_error(sh, "blob: the last header page did not take -- "
			              "slot %u may read valid, invalid or\r\n"
			              "      incomplete; `blob list` says which (the "
			              "payload was read back and matched)\r\n",
			          rep.slot);
			return 1;
		}
		break;
	default:
		break;
	}
	/* Includes "every byte arrived but the batch never closed", which ymodem.h
	 * is explicit is NOT a complete file: no magic is written, so the slot
	 * cannot be mistaken for a stored blob. */
	cli_error(sh, "blob: %s (%lu B erased, %lu B received of %lu declared, "
	              "%lu tx)\r\n", blob_write_result_name(res),
	          (unsigned long)rep.erased, (unsigned long)rep.received,
	          (unsigned long)rep.declared, (unsigned long)rep.transactions);
	return 1;
}

/* ---- erase ---------------------------------------------------------------- */

static int cmd_blob_erase(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t base = 0u, token = 0u;
	unsigned slot;
	int rc;

	(void)argc;
	if (blob_parse_slot(sh, argv[1], &slot) != 0)
		return 1;
	rc = blob_slot_geometry(slot, &base, NULL, NULL);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	/*
	 * No `confirm` token: a blob is an asset that came from a file the PC
	 * still has, in a slot the operator named explicitly on the line.
	 * [!] THE RESERVATION, so a `blob write` or `nn model load` in flight on
	 * another console is never erased under, and given back on every path.
	 */
	rc = blob_svc_reserve(&token);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	/* [!] THE HEADER UNIT ONLY.  Retiring a blob is one 4 KB erase rather
	 * than the whole slot: the next `blob write` erases the payload anyway, so
	 * this is fast and `blob list` stops showing the blob at once -- while the
	 * bytes are still there for anyone who reads the flash directly. */
	cli_print(sh, "erasing  : slot %u header at " BLOB_SVC_ADDR_FMT
	              " (one sector)\r\n", slot, (unsigned long)base);
	rc = blob_svc_erase_header(token, slot);
	blob_svc_unreserve(token);
	if (rc != BLOB_OK)
		return blob_complain(sh, rc);
	cli_print(sh, "erased   : the header is gone; the payload is still in the "
	              "flash until something writes over it\r\n");
	return 0;
}

/* ---- verify --------------------------------------------------------------- */

static int cmd_blob_verify(struct cli_instance *sh, int argc, char **argv)
{
	struct blob_info info;
	uint32_t crc = 0u;
	unsigned slot;
	int rc;

	(void)argc;
	if (blob_parse_slot(sh, argv[1], &slot) != 0)
		return 1;
	/* One call: what is printed below is the header the CRC was compared
	 * against, not a second read of the same slot. */
	rc = blob_verify(slot, &info, &crc);
	switch (rc) {
	case BLOB_OK:
		cli_print(sh, "slot %-4u: PASS, %lu B, crc32 %08lX\r\n", slot,
		          (unsigned long)info.length, (unsigned long)crc);
		return 0;
	case BLOB_ERR_CRC:
		/* The stored CRC is of the stream that arrived, so a mismatch
		 * means the flash and the PC's file differ. */
		cli_error(sh, "slot %-4u: FAIL, read back %08lX, header says "
		              "%08lX\r\n", slot, (unsigned long)crc,
		          (unsigned long)info.crc32);
		return 1;
	default:
		break;
	}
	return blob_complain(sh, rc);
}

/* ---- registration --------------------------------------------------------- */

CLI_SUBCMD_SET_CREATE(blob_subcmds,
	CLI_CMD_ARG_USAGE(list, NULL, "every slot: state, size, length, crc32, name",
	                  NULL, cmd_blob_list, 1, 0),
	CLI_CMD_ARG_USAGE(info, NULL, "one slot in full, and why it is invalid",
	                  "<slot>", cmd_blob_info, 2, 0),
	CLI_CMD_ARG_USAGE(read, NULL, "hexdump payload bytes",
	                  "<slot> [off] [len]", cmd_blob_read, 2, 2),
	CLI_CMD_ARG_USAGE(free, NULL, "slots that can take something, and what cannot",
	                  NULL, cmd_blob_free, 1, 0),
	CLI_CMD_ARG_USAGE(write, NULL, "ERASE a slot, then receive a file (YMODEM `sb`)",
	                  "<name> [slot]", cmd_blob_write, 2, 1),
	CLI_CMD_ARG_USAGE(verify, NULL, "re-read a slot and check its stored crc32",
	                  "<slot>", cmd_blob_verify, 2, 0),
	CLI_CMD_ARG_USAGE(erase, NULL, "retire a blob: erase its header sector",
	                  "<slot>", cmd_blob_erase, 2, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(blob, blob_subcmds, BLOB_SVC_HELP, NULL, 1, 0);

#else  /* no store, or no way to fill one from the console: nothing registered */

/* ISO C wants a declaration in every translation unit. */
typedef int blob_cmd_not_registered_t;

#endif
