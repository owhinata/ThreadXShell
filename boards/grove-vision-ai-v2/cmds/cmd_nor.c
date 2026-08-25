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
 *   nor write   program a byte pattern, inside `blob` only, 64 KB max
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

#include "tx_api.h"          /* tx_time_get: 1 ms ticks, for the elapsed line */

#include "blob_stage.h"      /* the staging buffer a long program runs out of */
#include "nor_cmd.h"         /* nor_cmd_writer_enter: shared with `blob write` */
#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_write.h"

#define LOG_TAG "nor"
#include "log.h"

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

/* Who is holding the window, for a refusal that can be acted on (issue #91).
 *
 * [!] NAMED ONLY WHEN EXACTLY ONE HOLDER IS OUT, and only when it is a slot
 * this file knows.  "busy" leaves the operator guessing; "busy -- `nn` holds
 * it" tells them what to close.  But a WRONG name is worse than none, so
 * anything else -- two holders, a slot added later that nobody taught this
 * function about -- degrades to the generic answer.  nor_flash.h hands out the
 * raw mask precisely so that this mapping lives with the thing that prints it.
 */
static const char *lease_holder(uint32_t live)
{
	switch (live) {
	case (1u << NOR_LEASE_NPU):    return "`nn` has a model open (nn close)";
	case (1u << NOR_LEASE_SCAN):   return "a `nor scan` is running";
	case (1u << NOR_LEASE_DEVMEM): return "a `devmem` read is in flight";
	case (1u << NOR_LEASE_BLOB):   return "a `blob` read is in flight";
	default:                       break;
	}
	return NULL;
}

static int cmd_nor_info(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_report r;

	(void)argc;
	(void)argv;

	/* [!] NO LEASE FOR THE REPORT (issue #91).  `nor info` reads the SCU and
	 * walks the MPU; it never touches the alias, so the lease it used to take
	 * bought nothing -- and cost everything, because a lease cannot be taken
	 * while a writer holds a reservation.  That made the one command that says
	 * WHY the part is busy unavailable exactly when it was.
	 *
	 * A lease is still taken for one purpose: OFF means nothing has been
	 * brought up, and bring-up is a reader's errand (nor_state.h).  Enumerated
	 * rather than "if it is not XIP", so a state added later does not silently
	 * start bringing hardware up from inside a diagnostic. */
	if (nor_lifecycle_state() == NOR_ST_OFF) {
		uint32_t token;

		if (nor_cmd_enter(sh, &token) != 0)
			return 1;
		(void)nor_release(token);
	}
	nor_report(&r);

	cli_print(sh, "state    : %s%s%s\r\n", nor_state_name(r.state),
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
	{
		const char *who = lease_holder(r.live);

		cli_print(sh, "leases   : %lu (mask 0x%lx)%s%s\r\n",
		          (unsigned long)r.readers, (unsigned long)r.live,
		          who ? " -- " : "", who ? who : "");
	}
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
	/* [!] SAY WHEN THEY WERE NOT TAKEN, rather than printing the last good
	 * copy (issue #91).  A transaction has the window down, so its SCU and MPU
	 * would describe the transition and not the mapping -- and a stale value
	 * printed without comment is indistinguishable from a current one, which is
	 * the failure this whole port is built to avoid. */
	if (!r.regs_sampled) {
		cli_print(sh, "scu xip  : 0x%08lx -> -- (window down; registers not "
		              "sampled)\r\n", (unsigned long)r.scu_xip_before);
		cli_print(sh, "mpu      : not sampled in state '%s'\r\n",
		          nor_state_name(r.state));
		return 0;
	}
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
 * That leaves a gap between the release and the reservation.  The reservation
 * is what closes it: if anything took a lease in between, it refuses rather
 * than waiting, and the user sees "busy" instead of a window pulled out from
 * under somebody.  Doing it the other way round -- having the writer bring the
 * part up itself -- is what would put two unrelated transactions in one path.
 *
 * [!] SINCE ISSUE #91 THE RESERVATION ALSO HOLDS THE GAP AFTERWARDS, which is
 * what a write made of several transactions needs and what these three do not:
 * a reader that arrives between two chunks of a `blob write` used to be handed
 * a lease that the next chunk would then refuse over. */
static int nor_cmd_bring_up(struct cli_instance *sh)
{
	uint32_t token;

	if (nor_cmd_enter(sh, &token) != 0)
		return -1;
	(void)nor_release(token);
	return 0;
}

/*
 * Bring the part up if nobody has, then take the writer reservation (#91).
 *
 * [!] THE TOKEN STAYS LOCAL TO THE CALLER AND EVERY EXIT MUST GIVE IT BACK.
 * A reservation that is taken and not returned leaves a port whose every
 * answer is "busy" for the rest of the session -- the same hazard
 * nor_write.h describes one level down, and easier to get wrong up here
 * because the code in between is a whole command.  The three subcommands below
 * are written with one exit each for that reason; a fourth that grew an early
 * `return` would be the bug.
 *
 * These three only ever run ONE transaction, so a reservation looks like
 * ceremony here.  It is not optional: nor_write_claim() now starts from
 * NOR_ST_RESERVED, and the reservation is what says which caller the claim
 * belongs to.  What it buys these commands is that a background job cannot
 * take a lease between the bring-up above and the transaction below.
 */
int nor_cmd_writer_enter(struct cli_instance *sh, uint32_t *token)
{
	struct nor_report r;
	const char *who;

	*token = 0u;
	if (nor_cmd_bring_up(sh) != 0)
		return -1;
	if (nor_reserve(token) == 0)
		return 0;

	if (nor_lifecycle_state() == NOR_ST_FAULTED) {
		cli_error(sh, "nor: %s\r\n",
		          nor_fail_reason() ? nor_fail_reason() : "faulted");
		return -1;
	}
	/* Name the holder when there is exactly one and we know it; the operator
	 * can act on "close `nn`" and cannot act on "busy". */
	nor_report(&r);
	who = lease_holder(r.live);
	if (who != NULL)
		cli_error(sh, "nor: busy -- %s\r\n", who);
	else if (r.readers != 0u)
		cli_error(sh, "nor: busy -- %lu reader(s) hold the window "
		              "(mask 0x%lx)\r\n", (unsigned long)r.readers,
		          (unsigned long)r.live);
	else
		cli_error(sh, "nor: busy -- another writer holds the part\r\n");
	return -1;
}

/* Everything a transaction reports, in one shape, so the three subcommands
 * cannot describe the same outcome differently.
 *
 * [!] AND IT ALSO GOES TO THE LOG RING, WHICH IS NOT BELT-AND-BRACES (issue
 * #91).  Once Ctrl+C is latched, cli_tx_send_blocking() returns -1 and
 * tx_failed drops every remaining byte of THIS command's output -- deliberately
 * (shell/core/cli_core.c), so a runaway handler cannot keep spewing after a
 * cancel.  The consequence here is that the one report an operator most needs
 * -- how much of an erase actually happened -- is exactly the one the console
 * can never show.  `dmesg` is where it can be read.
 *
 * Logging EVERY outcome rather than only the cancelled one is deliberate too:
 * on a part whose endurance is not documented (issue #89), a durable record of
 * how many transactions have run is worth having, and a log line that only
 * appears on failure is one nobody has seen the shape of. */
static void nor_cmd_log(enum nor_write_status st,
                        const struct nor_write_report *r)
{
	unsigned level = (st == NOR_WRITE_OK) ? LOG_LEVEL_INF
	               : (st == NOR_WRITE_FAULTED) ? LOG_LEVEL_ERR
	               : LOG_LEVEL_WRN;

	/* [!] COMPACT, AND THE FIELDS ARE IN THIS ORDER ON PURPOSE.  A record is
	 * LOG_MSG_MAX (104) bytes and the first version of this line overran it,
	 * losing the tail of the reason -- so the machine-readable part (status,
	 * span, done/verified, cancelled) comes first and the prose last, and a
	 * truncation can only ever cost the end of the prose.  The realistic worst
	 * case measures 92 bytes. */
	log_write(level, "nor", "%s 0x%lx+%lu: %lu/%lu%s%s%s",
	          nor_write_status_name(st), (unsigned long)r->span.addr,
	          (unsigned long)r->span.len, (unsigned long)r->done,
	          (unsigned long)r->verified,
	          r->cancelled ? " (cancelled)" : "",
	          r->fail ? " -- " : "", r->fail ? r->fail : "");
}

static int nor_cmd_report(struct cli_instance *sh, enum nor_write_status st,
                          const struct nor_write_report *r)
{
	nor_cmd_log(st, r);

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
		/* [!] TWO DIFFERENT EVENTS SHARE THIS STATUS and only one of them is
		 * the caller's doing.  Reporting a Ctrl+C as "the vendor returned 0"
		 * would read as a part that refused for no reason. */
		if (r->cancelled)
			cli_error(sh, "nor: cancelled after %lu B of %lu\r\n",
			          (unsigned long)r->done, (unsigned long)r->span.len);
		else
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
	uint32_t token;
	int rc;

	(void)argc;
	(void)argv;

	if (nor_cmd_writer_enter(sh, &token) != 0)
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
	st = nor_write_cycle(token, &r);
	rc = nor_cmd_report(sh, st, &r);
	(void)nor_unreserve(token);
	return rc;
}

/*
 * The erase tick (issue #91).  Runs with the memory-mapped window DOWN, so it
 * is held to what nor_write.h permits there: the cancel poll drains the RX ring
 * and reads shell state, and cli_print() goes to the UART -- neither reaches
 * the flash.  Nothing here may read the alias.
 *
 * Progress is printed sparsely.  A line per 4 KB sector would be 512 lines for
 * one slot and would itself become the slow part; one per 64 sectors is a line
 * every 256 KB, which is frequent enough to show the thing is alive.
 */
#define ERASE_TICK_EVERY  (64u * 0x1000u)

static int erase_tick(void *ctx, uint32_t done, uint32_t total)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (total > ERASE_TICK_EVERY && (done % ERASE_TICK_EVERY) == 0u)
		cli_print(sh, "erasing  : %lu / %lu B\r\n", (unsigned long)done,
		          (unsigned long)total);
	/* [!] Non-zero STOPS the erase, and the transaction ends INCOMPLETE with
	 * everything up to here genuinely erased.  Ctrl+C is available because
	 * this command has not claimed the console -- a caller that had would have
	 * to poll something else. */
	return cli_cancel_requested(sh) ? 1 : 0;
}

static int cmd_nor_erase(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_write_report r;
	struct nor_erase_progress prog = { NULL, erase_tick };
	enum nor_write_status st;
	struct nor_span span;
	uint32_t addr, len, token, t0, t1;
	int rc;

	(void)argc;
	if (cli_parse_u32(argv[1], &addr) != 0 ||
	    cli_parse_u32(argv[2], &len) != 0) {
		cli_error(sh, "nor: bad number\r\n");
		return 1;
	}
	prog.ctx = sh;
	if (nor_cmd_writer_enter(sh, &token) != 0)
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
	                   nor_seam_limits.unit, addr, len, &span) == NOR_SPAN_OK) {
		/* [!] SAID BEFORE THE ERASE STARTS, because it cannot be said after.
		 * A latched Ctrl+C drops the rest of this command's console output
		 * (see nor_cmd_report()), so the operator has to be told in advance
		 * where the result will be. */
		if (span.len > nor_seam_limits.unit)
			cli_print(sh, "         Ctrl+C stops it between sectors; the "
			              "result is in `dmesg`\r\n");
		cli_print(sh, "footprint: 0x%08lx..0x%08lx (%lu B, %lu sector(s))\r\n",
		          (unsigned long)span.addr,
		          (unsigned long)(span.addr + span.len),
		          (unsigned long)span.len,
		          (unsigned long)(span.len / nor_seam_limits.unit));
	} else
		cli_print(sh, "request  : 0x%08lx +%lu B\r\n",
		          (unsigned long)addr, (unsigned long)len);

	t0 = (uint32_t)tx_time_get();
	st = nor_write_erase(token, addr, len, &prog, &r);
	t1 = (uint32_t)tx_time_get();
	rc = nor_cmd_report(sh, st, &r);
	cli_print(sh, "elapsed  : %lu ms (%lu sector(s))\r\n",
	          (unsigned long)(t1 - t0),
	          (unsigned long)(r.span.len / nor_seam_limits.unit));
	(void)nor_unreserve(token);
	return rc;
}

/* [!] `nor write` PROGRAMS OUT OF THE BLOB STAGING BUFFER (issue #92), and it
 * used to have a 256-byte pattern of its own.  Not because a console needs to
 * write 64 KB of one byte -- it does not -- but because implementation order
 * item 6 of #49 Step 2 has to MEASURE how long one program transaction takes at
 * 1, 4, 16, 32 and 64 KB before the transfer's chunk size can be chosen, and a
 * measurement taken through a different buffer would be a measurement of a
 * different path.  This is the buffer the transfer will use.
 *
 * Sharing it is safe for the same reason blob_stage.h gives: everything that
 * touches it holds the NOR reservation, and `nor write` holds one from
 * nor_cmd_writer_enter() to nor_unreserve() below. */

/* [!] A PATTERN, NOT A CONSTANT, and this is the default for a reason learned
 * the expensive way (issue #92).  `nor write` used to fill with 0xA5 and a
 * 64 KB program of it read back perfectly -- while the first real transfer
 * faulted on the same path.  A constant is invariant under everything that can
 * go wrong between a buffer and an array: a page written twice, pages written
 * out of order, bytes swapped within a word, a stale copy re-sent.  It cannot
 * fail, so it proved nothing.
 *
 * Seeded from the address so that two writes to different places differ, and so
 * that repeating a write at the same address is reproducible.  The period is
 * long: a repeated 256-byte page shows up as a mismatch, which a pattern with
 * period 256 would have hidden.
 */
static void fill_pattern(uint8_t *buf, uint32_t len, uint32_t seed)
{
	uint32_t x = seed | 1u;

	for (uint32_t i = 0u; i < len; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		buf[i] = (uint8_t)(x & 0xFFu);
	}
}

static int cmd_nor_write(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_write_report r;
	enum nor_write_status st;
	uint32_t addr, len, token, byte = 0u, t0, t1;
	int rc, constant = 0;

	if (cli_parse_u32(argv[1], &addr) != 0 ||
	    cli_parse_u32(argv[2], &len) != 0 ||
	    (argc >= 4 && cli_parse_u32(argv[3], &byte) != 0)) {
		cli_error(sh, "nor: bad number\r\n");
		return 1;
	}
	constant = (argc >= 4);
	if (len == 0u || len > BLOB_STAGE_BYTES) {
		cli_error(sh, "nor: length must be 1..%lu (the staging buffer)\r\n",
		          (unsigned long)BLOB_STAGE_BYTES);
		return 1;
	}
	if (byte > 0xFFu) {
		cli_error(sh, "nor: pattern must be a byte\r\n");
		return 1;
	}
	if (constant)
		memset(blob_stage_buf, (int)byte, len);
	else
		fill_pattern(blob_stage_buf, len, addr);

	if (nor_cmd_writer_enter(sh, &token) != 0)
		return 1;

	if (constant)
		cli_print(sh, "program  : 0x%08lx +%lu B of 0x%02lx\r\n",
		          (unsigned long)addr, (unsigned long)len,
		          (unsigned long)byte);
	else
		cli_print(sh, "program  : 0x%08lx +%lu B of a varying pattern\r\n",
		          (unsigned long)addr, (unsigned long)len);
	/* [!] No erase first, deliberately.  Programming only clears bits, so
	 * writing over something that is not erased leaves the AND of the two --
	 * and the read-back catches that, terminally.  An erase hidden inside this
	 * command would destroy 4 KB to write 256 bytes, without saying so. */
	t0 = (uint32_t)tx_time_get();
	st = nor_write_program(token, addr, blob_stage_buf, len, &r);
	t1 = (uint32_t)tx_time_get();
	rc = nor_cmd_report(sh, st, &r);
	/* [!] THE WHOLE TRANSACTION, not the vendor call: the window down, the
	 * JEDEC canary, the pages, the window back up, the re-probe and the
	 * read-back.  That is what a transfer pays per chunk, so it is what the
	 * chunk size has to be chosen against. */
	cli_print(sh, "elapsed  : %lu ms (whole transaction, %lu B)\r\n",
	          (unsigned long)(t1 - t0), (unsigned long)len);
	(void)nor_unreserve(token);
	return rc;
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
	CLI_CMD_ARG_USAGE(write, NULL, "write <addr> <len> [byte] (pattern if no byte)",
	                  "<addr> <len> [byte]", cmd_nor_write, 3, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nor, nor_subcmds,
                 "inspect and write the external QSPI NOR", NULL, 1, 0);
