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
 *
 * NO `nor write` AND NO `nor erase`, ON PURPOSE.  This flash holds the
 * bootloader in its first 2 MB and the bootloader's slot header in its last
 * block.  A bounded write path is issue #88; exposing a raw one here would make
 * the bounds check that issue exists to build pointless before it was written.
 *
 * [!] BUT "READ-ONLY" WOULD BE THE WRONG WORD, and an adversarial review was
 * right to say so.  Neither subcommand programs or erases the ARRAY -- and
 * neither is free:
 *
 *   - the first bring-up runs the vendor's quad-enable, which sets WEL and
 *     writes the QE bit of the NOR's non-volatile status register.  That is a
 *     flash write.  It is not new -- `nn open` has always done it, which is why
 *     setWriteEnable is a documented exception to the forbidden-symbol list --
 *     but it becomes reachable from a diagnostic here;
 *   - and bring-up permanently changes MPU state and takes one EPK slot for the
 *     rest of the session.  Also not new capacity, also newly reachable.
 *
 * Worth knowing before running one on a board about to need its 31/32
 * high-water mark, or on a part whose endurance is in question (issue #89).
 *
 * WHY `nor scan` EARNS ITS PLACE.  The 12.6 MB reserved for issue #49's blob
 * was surveyed with seventeen `devmem peek`s.  That is sampling, not a scan: it
 * can find an occupant but cannot say a region is empty.  Nothing should write
 * there on the strength of it.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "nor_flash.h"

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

CLI_SUBCMD_SET_CREATE(nor_subcmds,
	CLI_CMD_ARG_USAGE(info, NULL, "lifecycle, JEDEC id, wrapped IRQ, MPU/SCU",
	                  NULL, cmd_nor_info, 1, 0),
	CLI_CMD_ARG_USAGE(scan, NULL, "walk the window, report non-erased extents",
	                  NULL, cmd_nor_scan, 1, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nor, nor_subcmds,
                 "inspect the external QSPI NOR (no array writes)", NULL, 1, 0);
