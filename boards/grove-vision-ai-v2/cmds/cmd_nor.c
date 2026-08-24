/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nor.c
 * @brief   `nor` command: inspect the external QSPI NOR (issue #86).
 *
 *   nor info    what the lifecycle established, and what is true now
 *   nor scan    walk the window and report what is not erased
 *   nor cycle   take the window down and bring it back, touching no data
 *   nor erase   erase the flash covering a range, inside `blob` only
 *   nor write   program a byte pattern, inside `blob` only
 *
 * THE LAST THREE ARE ISSUE #88 PART E, AND THEY ARE THIN ON PURPOSE.  All the
 * judgement is in port/nor/nor_write.c and port/sdk_seam/nor_seam.c; this file
 * parses two numbers, prints the footprint before it is destroyed and prints
 * what came back.  There is no raw-opcode subcommand and no way to name an
 * address outside the writable interval -- the interval is the seam's, read out
 * of its own .rodata record, and the writer refuses before it claims the part.
 *
 * [!] AND "READ-ONLY" WAS ALREADY THE WRONG WORD FOR THE FIRST TWO, which an
 * adversarial review was right to say before any of this existed.  Neither
 * `info` nor `scan` programs or erases the ARRAY -- and neither is free:
 *
 *   - the first bring-up runs the vendor's quad-enable, which sets WEL and
 *     writes the QE bit of the NOR's non-volatile status register.  That is a
 *     flash write.  It is not new -- `nn open` has always done it, which is why
 *     setWriteEnable is a documented exception to the forbidden-symbol list --
 *     but it becomes reachable from a diagnostic here;
 *   - and bring-up permanently changes MPU state and takes one EPK slot for the
 *     rest of the session.  Also not new capacity, also newly reachable.
 *
 * `nor cycle` is the same story, twice per run: taking the window down clears
 * that QE bit and bringing it back sets it again.  So the one subcommand that
 * touches no data still costs two status-register writes, which is worth
 * knowing on a part whose endurance is not documented (issue #89).
 *
 * WHY `nor scan` EARNS ITS PLACE.  The 12.6 MB reserved for issue #49's blob
 * was surveyed with seventeen `devmem peek`s.  That is sampling, not a scan: it
 * can find an occupant but cannot say a region is empty.  Nothing should write
 * there on the strength of it -- and now something can.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_write.h"

/* The partition edges, from board.cmake -- the same variables
 * check_flash_partitions.py consumes, so the labels here and the layout the
 * host checks cannot drift apart (issues #45, #85).
 *
 * [!] NO FALLBACK VALUES (issue #88).  These used to be #ifndef defaults, and
 * defaults are a second declaration of a layout that only one file is allowed
 * to declare: when the granularity became 4 KB, three of the five moved, and a
 * tree that had lost the compile definitions would have gone on labelling
 * `nor scan` output with the old map -- silently, because a plausible label is
 * indistinguishable from a correct one.  A missing definition is a build
 * error. */
#if !defined(NOR_PART_FW_END) || !defined(NOR_PART_BLOB_END) || \
    !defined(NOR_PART_CLS_END) || !defined(NOR_PART_DET_END) || \
    !defined(NOR_PART_TAIL_END)
#error "NOR_PART_*_END must come from board.cmake's partition map"
#endif

/* [!] The scan step is the erase unit the resident 2nd bootloader actually
 * uses, measured by disassembling it (issue #88): it walks `addr & ~0xFFF` in
 * 4 KB steps and never issues a 32 KB, 64 KB or chip erase.  The layout check
 * rounds destroyed footprints to the same 4 KB, so a survey and the layout it
 * surveys now agree on what one write can disturb. */
#define SCAN_STEP        0x1000u
#define SCAN_MAX_RECORDS 64u
/* Yield often enough that a 16 MB walk cannot monopolise the console. */
#define SCAN_YIELD_EVERY 64u

/* [!] EVERY WORD OF THE SECTOR, not a sample of it.  Sampling one word per
 * sector was tried first and it manufactured FALSE GAPS: the detection model is
 * contiguous, and two of its sectors happened to begin with 0xFFFFFFFF, so the
 * survey reported holes in the middle of it.
 *
 * That is the dangerous direction of error.  A survey that under-reports
 * occupancy makes a region look freer than it is, and the whole reason this
 * command exists is to decide whether 12.6 MB is safe to write.  So a sector is
 * called erased only when all of it reads 0xFF -- and an occupied sector still
 * costs one read, because the loop stops at the first word that is not.
 *
 * The residual limit is inherent and unfixable: a sector deliberately written
 * with 0xFF everywhere is indistinguishable from an erased one. */
static int sector_erased(const volatile uint32_t *win, uint32_t off)
{
	for (uint32_t i = 0u; i < SCAN_STEP / 4u; i++)
		if (win[(off / 4u) + i] != 0xFFFFFFFFu)
			return 0;
	return 1;
}

struct part {
	uint32_t    end;
	const char *name;
};

static const struct part parts[] = {
	{ NOR_PART_FW_END,   "firmware"    },
	{ NOR_PART_BLOB_END, "blob"        },
	{ NOR_PART_CLS_END,  "model-cls"   },
	{ NOR_PART_DET_END,  "model-det"   },
	{ NOR_PART_TAIL_END, "blob-tail"   },
	{ NOR_SIZE,          "slot-header" },
};

static const char *part_of(uint32_t off)
{
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
		if (off < parts[i].end)
			return parts[i].name;
	return "?";
}

/* One lease for the whole of a subcommand, released on every exit. */
static int nor_cmd_enter(struct cli_instance *sh, uint32_t *token)
{
	if (nor_acquire(NOR_LEASE_SCAN, token) == 0)
		return 0;

	if (nor_lifecycle_state() == NOR_ST_FAULTED)
		cli_error(sh, "nor: %s\r\n",
		          nor_fail_reason() ? nor_fail_reason() : "faulted");
	else
		cli_error(sh, "nor: busy\r\n");
	return -1;
}

static int cmd_nor_info(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_report r;
	uint32_t token;

	(void)argc;
	(void)argv;

	if (nor_cmd_enter(sh, &token) != 0)
		return 1;
	nor_report(&r);

	cli_print(sh, "state    : %s%s%s\r\n", nor_state_name(nor_lifecycle_state()),
	          nor_fail_reason() ? " -- " : "",
	          nor_fail_reason() ? nor_fail_reason() : "");
	if (r.jedec_valid)
		cli_print(sh, "jedec    : %02x %02x %02x (read before XIP)\r\n",
		          r.jedec[0], r.jedec[1], r.jedec[2]);
	else
		cli_print(sh, "jedec    : -- (unreadable; the vendor refuses once "
		              "XIP is on)\r\n");
	cli_print(sh, "window   : 0x%08lx, %lu B\r\n",
	          (unsigned long)NOR_XIP_BASE, (unsigned long)NOR_SIZE);
	cli_print(sh, "leases   : %lu\r\n", (unsigned long)r.readers);
	/* What the write seam was COMPILED to allow (issue #88).  Read out of the
	 * seam's own .rodata record rather than restated here, so the interval this
	 * line shows is the one the wrappers enforce -- and so that
	 * cmake/check_nor_seam.py, which reads the same twelve bytes out of the
	 * linked image, is checking a fact somebody can also see on the device.
	 * It is also the only place `nor erase` and `nor write` get their bounds
	 * from -- this file holds no copy of them. */
	cli_print(sh, "writable : 0x%08lx..0x%08lx, %lu B unit\r\n",
	          (unsigned long)nor_seam_limits.lo,
	          (unsigned long)nor_seam_limits.hi,
	          (unsigned long)nor_seam_limits.unit);
	/* [!] The probe reads the slot-header block, not `blob` (issue #90): a
	 * probe inside blob would be proving the window came back by reading bytes
	 * issue #88's writer is allowed to erase.
	 * The magic is REPORTED, never required -- a corrupt slot header still
	 * boots (the bootloader falls back to slot 0), so bring-up must not turn
	 * somebody else's recoverable damage into our refusal. */
	cli_print(sh, "probe    : 0x%08lx = 0x%08lx  %s\r\n",
	          (unsigned long)r.probe_off, (unsigned long)r.probe_word,
	          r.probe_hdr ? "(slot-header magic)"
	                      : "(no slot-header magic -- observation only)");
	/* [!] The observable for issue #86: this line used to be part of the NPU's
	 * EPK wrapset, so `nn close` unwrapped it -- and unwrapping disables. */
	cli_print(sh, "irq      : %d %s\r\n", r.irq,
	          r.irq < 0 ? "(none wrapped)"
	                    : (r.irq_enabled ? "wrapped, enabled" : "WRAPPED BUT OFF"));
	/* Raw, not decoded: the SVD names this register but gives no field
	 * breakdown, and this board does not guess at bits it cannot name. */
	cli_print(sh, "scu xip  : 0x%08lx -> 0x%08lx (raw)\r\n",
	          (unsigned long)r.scu_xip_before, (unsigned long)r.scu_xip_after);
	cli_print(sh, "mpu ctrl : S 0x%08lx  NS 0x%08lx\r\n",
	          (unsigned long)r.mpu_ctrl_s, (unsigned long)r.mpu_ctrl_ns);
	if (r.mpu_region >= 0)
		cli_print(sh, "mpu rgn  : %ld  rbar 0x%08lx  rlar 0x%08lx\r\n",
		          (long)r.mpu_region, (unsigned long)r.mpu_rbar,
		          (unsigned long)r.mpu_rlar);
	else
		cli_print(sh, "mpu rgn  : none covers the window\r\n");
	cli_print(sh, "mpu mair : 0x%08lx 0x%08lx\r\n",
	          (unsigned long)r.mpu_mair0, (unsigned long)r.mpu_mair1);

	(void)nor_release(token);
	return 0;
}

static int cmd_nor_scan(struct cli_instance *sh, int argc, char **argv)
{
	const volatile uint32_t *win = (const volatile uint32_t *)NOR_XIP_BASE;
	uint32_t token, off, run_start = 0u, records = 0u, occupied = 0u;
	int in_run = 0, truncated = 0, stopped = 0;

	(void)argc;
	(void)argv;

	if (nor_cmd_enter(sh, &token) != 0)
		return 1;

	cli_print(sh, "scanning %lu B in %lu B steps; Ctrl+C to stop\r\n",
	          (unsigned long)NOR_SIZE, (unsigned long)SCAN_STEP);
	cli_print(sh, "%-10s %-10s %10s  %s\r\n", "from", "to", "bytes", "partition");

	for (off = 0u; off < NOR_SIZE; off += SCAN_STEP) {
		int erased = sector_erased(win, off);
		/* [!] A run is BROKEN AT A PARTITION EDGE, not just at an erased
		 * sector.  Without this, occupancy that spans a boundary is reported
		 * under whichever partition it started in -- which is how the first
		 * run of this scan swallowed the detection model's first sectors and
		 * labelled them `model-cls`.  A survey whose labels are wrong at
		 * exactly the boundaries it exists to check is worse than none. */
		int edge = in_run && (part_of(off) != part_of(run_start));

		if (!erased)
			occupied += SCAN_STEP;

		if (in_run && (erased || edge)) {
			in_run = 0;
			if (records < SCAN_MAX_RECORDS) {
				cli_print(sh, "0x%08lx 0x%08lx %10lu  %s\r\n",
				          (unsigned long)run_start, (unsigned long)off,
				          (unsigned long)(off - run_start),
				          part_of(run_start));
				records++;
			} else {
				truncated = 1;
			}
		}
		if (!erased && !in_run) {
			in_run = 1;
			run_start = off;
		}

		if ((off / SCAN_STEP) % SCAN_YIELD_EVERY == 0u) {
			if (cli_cancel_requested(sh)) {
				stopped = 1;
				break;
			}
			if (cli_sleep(sh, 1u) != 0) {
				stopped = 1;
				break;
			}
		}
	}

	if (in_run && !stopped) {
		if (records < SCAN_MAX_RECORDS) {
			cli_print(sh, "0x%08lx 0x%08lx %10lu  %s\r\n",
			          (unsigned long)run_start, (unsigned long)NOR_SIZE,
			          (unsigned long)(NOR_SIZE - run_start),
			          part_of(run_start));
			records++;
		} else {
			truncated = 1;
		}
	}

	if (stopped)
		cli_print(sh, "stopped at 0x%08lx\r\n", (unsigned long)off);
	if (truncated)
		cli_print(sh, "[!] more than %lu extents; the rest were not printed\r\n",
		          (unsigned long)SCAN_MAX_RECORDS);
	cli_print(sh, "%lu extent(s), %lu B occupied of %lu B.\r\n",
	          (unsigned long)records, (unsigned long)occupied,
	          (unsigned long)NOR_SIZE);
	/* [!] Said out loud: every byte was read, so a gap here means every byte of
	 * it is 0xFF.  The one thing that remains indistinguishable from erased is
	 * a sector somebody deliberately wrote 0xFF into. */
	cli_print(sh, "every byte read; a gap is 0xFF throughout, which is also "
	              "what a sector written all-0xFF looks like\r\n");

	(void)nor_release(token);
	return stopped ? 1 : 0;
}

/* --- the write path (issue #88 Part E) ------------------------------------
 *
 * Three subcommands over one transaction.  What each of them is allowed to do
 * is decided in port/nor/nor_write.c and checked again in the seam; what is
 * here is argument parsing, the footprint printed BEFORE it is destroyed, and
 * the report printed after.
 */

/* [!] A WRITE CANNOT BRING THE WINDOW UP -- that is a reader's errand, and
 * nor_write_decide() answers NOR_WR_BUSY from NOR_ST_OFF for a reason worth
 * keeping (nor_state.h).  So a write subcommand does the reader's part first,
 * explicitly, and hands the lease straight back.
 *
 * That leaves a gap between the release and the claim.  The claim is what
 * closes it: if anything took a lease in between, it refuses rather than
 * waiting, and the user sees "busy" instead of a window pulled out from under
 * somebody.  Doing it the other way round -- having the writer bring the part
 * up itself -- is what would put two unrelated transactions in one path. */
static int nor_cmd_bring_up(struct cli_instance *sh)
{
	uint32_t token;

	if (nor_cmd_enter(sh, &token) != 0)
		return -1;
	(void)nor_release(token);
	return 0;
}

/* Everything a transaction reports, in one shape, so the three subcommands
 * cannot describe the same outcome differently. */
static int nor_cmd_report(struct cli_instance *sh, enum nor_write_status st,
                          const struct nor_write_report *r)
{
	if (r->jedec_ok)
		cli_print(sh, "jedec    : %02x %02x %02x (re-read with the window "
		              "down)\r\n", r->jedec[0], r->jedec[1], r->jedec[2]);

	switch (st) {
	case NOR_WRITE_OK:
		if (r->span.len == 0u)
			cli_print(sh, "ok       : the window went down and came back, "
			              "re-probed; no data touched\r\n");
		else
			cli_print(sh, "ok       : %lu B done, %lu B read back\r\n",
			          (unsigned long)r->done, (unsigned long)r->verified);
		return 0;
	case NOR_WRITE_BUSY:
		cli_error(sh, "nor: busy -- a reader holds the window, or it has "
		              "never been brought up\r\n");
		return 1;
	case NOR_WRITE_REFUSED:
		cli_error(sh, "nor: refused -- %s\r\n",
		          r->fail ? r->fail : "outside what this port may write");
		cli_print(sh, "writable : 0x%08lx..0x%08lx, %lu B unit\r\n",
		          (unsigned long)nor_seam_limits.lo,
		          (unsigned long)nor_seam_limits.hi,
		          (unsigned long)nor_seam_limits.unit);
		return 1;
	case NOR_WRITE_NO_TRANSPORT:
		cli_error(sh, "nor: the part did not answer with the window down; "
		              "nothing was sent\r\n");
		return 1;
	case NOR_WRITE_INCOMPLETE:
		cli_error(sh, "nor: stopped after %lu B of %lu (vendor returned "
		              "%ld)\r\n", (unsigned long)r->done,
		          (unsigned long)r->span.len, (long)r->vendor_rc);
		cli_print(sh, "         %lu B read back and verified; nothing is "
		              "claimed about the rest\r\n",
		          (unsigned long)r->verified);
		return 1;
	case NOR_WRITE_FAULTED:
	default:
		cli_error(sh, "nor: FAULTED -- %s\r\n",
		          r->fail ? r->fail : "the transaction could not be verified");
		if (r->bad_valid)
			cli_print(sh, "         first at 0x%08lx: read 0x%02x, wanted "
			              "0x%02x\r\n", (unsigned long)r->bad_off,
			          r->bad_got, r->bad_want);
		/* Terminal by design: a mismatch cannot be told from a window that is
		 * lying about the array, and every later read would inherit that.  See
		 * port/nor/nor_write.c. */
		cli_print(sh, "         the port refuses everything until the board "
		              "is reset\r\n");
		return 1;
	}
}

static int cmd_nor_cycle(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_write_report r;
	enum nor_write_status st;

	(void)argc;
	(void)argv;

	if (nor_cmd_bring_up(sh) != 0)
		return 1;
	/* [!] SAID BEFORE IT RUNS, and phrased as the COST OF THE OPERATION rather
	 * than as an announcement that it is happening.  This is the subcommand
	 * somebody reaches for to "check without changing anything", so the cost
	 * belongs in front of it -- but the transaction can still be refused (a
	 * reader lease), and a line that said "this is now writing the status
	 * register" followed by "busy" would be describing something that did not
	 * happen. */
	cli_print(sh, "a cycle costs two status-register writes and touches no "
	              "data\r\n");
	st = nor_write_cycle(&r);
	return nor_cmd_report(sh, st, &r);
}

static int cmd_nor_erase(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_write_report r;
	enum nor_write_status st;
	struct nor_span span;
	uint32_t addr, len;

	(void)argc;
	if (cli_parse_u32(argv[1], &addr) != 0 ||
	    cli_parse_u32(argv[2], &len) != 0) {
		cli_error(sh, "nor: bad number\r\n");
		return 1;
	}
	if (nor_cmd_bring_up(sh) != 0)
		return 1;

	/* [!] THE FOOTPRINT, AND BEFORE THE ERASE RATHER THAN WITH THE REPORT.
	 * An erase destroys whole units, so naming 16 bytes loses 4 KB and the
	 * range that is LOST is what a reader of this output needs.  Printing it
	 * first also matters because the vendor's write path is built on a poll
	 * with no timeout (nor_write.h): if this ever wedges, the last line on the
	 * console is what was in flight.
	 *
	 * Computed by the same pure function the writer uses, from the same
	 * interval, so the two cannot disagree -- and this call decides nothing.
	 * The writer computes it again and its answer is the one that governs. */
	if (nor_span_erase(nor_seam_limits.lo, nor_seam_limits.hi,
	                   nor_seam_limits.unit, addr, len, &span) == NOR_SPAN_OK)
		cli_print(sh, "footprint: 0x%08lx..0x%08lx (%lu B, %lu sector(s))\r\n",
		          (unsigned long)span.addr,
		          (unsigned long)(span.addr + span.len),
		          (unsigned long)span.len,
		          (unsigned long)(span.len / nor_seam_limits.unit));
	else
		cli_print(sh, "request  : 0x%08lx +%lu B\r\n",
		          (unsigned long)addr, (unsigned long)len);

	st = nor_write_erase(addr, len, &r);
	return nor_cmd_report(sh, st, &r);
}

/* The pattern `nor write` programs.  One program page, which is the unit the
 * part itself programs in and is enough to prove the path in both directions:
 * write a sentinel that is not 0xFF, read it through the restored window, then
 * erase and see 0xFF.  Bulk data does not come through a console -- issue #49's
 * blob writer calls nor_write_program() directly. */
static uint8_t write_pattern[NOR_PROGRAM_PAGE];

static int cmd_nor_write(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_write_report r;
	enum nor_write_status st;
	uint32_t addr, len, byte = 0xA5u;

	if (cli_parse_u32(argv[1], &addr) != 0 ||
	    cli_parse_u32(argv[2], &len) != 0 ||
	    (argc >= 4 && cli_parse_u32(argv[3], &byte) != 0)) {
		cli_error(sh, "nor: bad number\r\n");
		return 1;
	}
	if (len == 0u || len > sizeof(write_pattern)) {
		cli_error(sh, "nor: length must be 1..%lu (one program page)\r\n",
		          (unsigned long)sizeof(write_pattern));
		return 1;
	}
	if (byte > 0xFFu) {
		cli_error(sh, "nor: pattern must be a byte\r\n");
		return 1;
	}
	memset(write_pattern, (int)byte, len);

	if (nor_cmd_bring_up(sh) != 0)
		return 1;

	cli_print(sh, "program  : 0x%08lx +%lu B of 0x%02lx\r\n",
	          (unsigned long)addr, (unsigned long)len, (unsigned long)byte);
	/* [!] No erase first, deliberately.  Programming only clears bits, so
	 * writing over something that is not erased leaves the AND of the two --
	 * and the read-back catches that, terminally.  An erase hidden inside this
	 * command would destroy 4 KB to write 256 bytes, without saying so. */
	st = nor_write_program(addr, write_pattern, len, &r);
	return nor_cmd_report(sh, st, &r);
}

CLI_SUBCMD_SET_CREATE(nor_subcmds,
	CLI_CMD_ARG_USAGE(info, NULL, "lifecycle, JEDEC id, wrapped IRQ, MPU/SCU",
	                  NULL, cmd_nor_info, 1, 0),
	CLI_CMD_ARG_USAGE(scan, NULL, "walk the window, report non-erased extents",
	                  NULL, cmd_nor_scan, 1, 0),
	CLI_CMD_ARG_USAGE(cycle, NULL, "window down and back up; no data touched",
	                  NULL, cmd_nor_cycle, 1, 0),
	CLI_CMD_ARG_USAGE(erase, NULL, "erase <addr> <len> (whole 4 KB sectors)",
	                  "<addr> <len>", cmd_nor_erase, 3, 0),
	CLI_CMD_ARG_USAGE(write, NULL, "write <addr> <len> [byte] (one page max)",
	                  "<addr> <len> [byte]", cmd_nor_write, 3, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nor, nor_subcmds,
                 "inspect and write the external QSPI NOR", NULL, 1, 0);
