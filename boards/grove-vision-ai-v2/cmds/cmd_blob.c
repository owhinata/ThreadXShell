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
 * The write side -- `blob write`, `erase` and `verify` -- is items 6 and 7 of
 * #49 Step 2 and is not here yet.  Until it is, everything this file can reach
 * is a read: it takes NOR_LEASE_BLOB, which is also what brings the window up,
 * and gives it back before it prints.
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
#include "nor_flash.h"
#include "nor_seam.h"

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

CLI_SUBCMD_SET_CREATE(blob_subcmds,
	CLI_CMD_ARG_USAGE(list, NULL, "every slot: state, size, length, crc32, name",
	                  NULL, cmd_blob_list, 1, 0),
	CLI_CMD_ARG_USAGE(info, NULL, "one slot in full, and why it is invalid",
	                  "<slot>", cmd_blob_info, 2, 0),
	CLI_CMD_ARG_USAGE(read, NULL, "hexdump payload bytes",
	                  "<slot> [off] [len]", cmd_blob_read, 2, 2),
	CLI_CMD_ARG_USAGE(free, NULL, "slots that can take something, and what cannot",
	                  NULL, cmd_blob_free, 1, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(blob, blob_subcmds,
                 "assets stored in the external NOR", NULL, 1, 0);
