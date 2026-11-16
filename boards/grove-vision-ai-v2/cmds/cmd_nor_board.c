/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nor_board.c
 * @brief   The shared `nor`'s table on Grove Vision AI V2 (issue #250), and
 *          this board's own `nor scan` and `nor cycle` (issue #86).
 *
 *   nor info    what the lifecycle established, and what is true now
 *   nor scan    walk the window and report what is not erased
 *   nor cycle   take the window down and bring it back, touching no data
 *   nor erase / nor write   the shared ones, inside `blob` only
 *
 * The command is shell/cmds/cmd_nor.c; this file answers svc/nor/nor_svc.h.
 * Until issue #250 it was this board's cmds/cmd_nor.c.
 *
 * ERASE AND WRITE ARE ISSUE #88 PART E, AND THEY ARE THIN ON PURPOSE.  All the
 * judgement is in port/nor/nor_write.c and port/sdk_seam/nor_seam.c.  The
 * hold below is the writer reservation, and the one operation inside it is ONE
 * call of nor_write_erase() / nor_write_program() -- one transaction, which
 * nor_write.c runs from the claim to the commit without returning part way.
 * There is no raw-opcode subcommand and no way to name an address outside the
 * writable interval: the range below is the seam's own .rodata record, and
 * the writer refuses again before it claims the part.
 *
 * There is no `nor read` (the flash is memory-mapped: `devmem dump` reads it
 * under a lease) and no `nor test` (every `nor write` reads back what it wrote
 * and compares it); neither is compiled here (svc/nor_svc_config.h, #250).
 *
 * [!] AND "READ-ONLY" WAS ALREADY THE WRONG WORD FOR `info` AND `scan`, which
 * an adversarial review was right to say before any of this existed.  Neither
 * programs or erases the ARRAY -- and neither is free:
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
#include "nor_cmd_core.h"     /* nor_cmd_report: `nor cycle` says it as erase does */
#include "nor_console.h"      /* background jobs: the shared question (#203, #250) */
#include "nor_svc.h"
#include "nor_svc_config.h"   /* the declarations of scan and cycle */

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "fmt.h"
#include "nor_cmd.h"         /* nor_cmd_say_busy: shared with `blob` */
#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_span.h"
#include "nor_write.h"

#ifndef NOR_ERASE_GRAN
#error "NOR_ERASE_GRAN must come from board.cmake's measured geometry"
#endif
/* The shared `nor` erases whole 4 KB units; the seam permits exactly that. */
_Static_assert(NOR_ERASE_GRAN == NOR_SVC_ERASE_UNIT,
               "the seam's erase unit is the shared `nor`'s 4 KB");

/* The partition edges, from board.cmake -- the same variables
 * check_flash_partitions.py consumes, so the labels here and the layout the
 * host checks cannot drift apart (issues #45, #85).
 *
 * [!] NO FALLBACK VALUES (issue #88).  These used to be #ifndef defaults, and
 * defaults are a second declaration of a layout that only one file is allowed
 * to declare: when the granularity became 4 KB three of the five edges then
 * declared moved, and when the model partitions were folded into blob (issue
 * #94) three of them disappeared -- and a tree that had lost the compile
 * definitions would have gone on labelling `nor scan` output with the old map,
 * silently, because a plausible label is indistinguishable from a correct one.
 * A missing definition is a build error. */
#if !defined(NOR_PART_FW_END) || !defined(NOR_PART_BLOB_END)
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

/* [!] THREE, WHERE THERE WERE SIX (issue #94).  `model-cls`, `model-det` and
 * the `blob-tail` run above them were folded into blob once `nn open <name>`
 * stopped reading a fixed address (issue #93).  The bytes of the old copies are
 * still out there -- nothing erased them -- but they are inside blob now, and
 * `nor scan` says so rather than naming a partition that no longer exists. */
static const struct part parts[] = {
	{ NOR_PART_FW_END,   "firmware"    },
	{ NOR_PART_BLOB_END, "blob"        },
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
	case (1u << NOR_LEASE_NPU):    return "`nn` has a model open (nn model unload)";
	case (1u << NOR_LEASE_SCAN):   return "a `nor scan` is running";
	case (1u << NOR_LEASE_DEVMEM): return "a `devmem` read is in flight";
	case (1u << NOR_LEASE_BLOB):   return "a `blob` read is in flight";
	default:                       break;
	}
	return NULL;
}

int nor_board_scan(struct cli_instance *sh, int argc, char **argv)
{
	const volatile uint32_t *win = (const volatile uint32_t *)NOR_XIP_BASE;
	uint32_t token, off, run_start = 0u, records = 0u, occupied = 0u;
	int in_run = 0, truncated = 0, stopped = 0;

	(void)argc;
	(void)argv;

	if (nor_cmd_enter(sh, &token) != 0)
		return 1;

	cli_print(sh, "scan %lu B by %lu B (Ctrl+C stops)\r\n",
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
		cli_print(sh, "more than %lu extents; rest not printed\r\n",
		          (unsigned long)SCAN_MAX_RECORDS);
	cli_print(sh, "%lu extent(s), %lu B occupied of %lu B.\r\n",
	          (unsigned long)records, (unsigned long)occupied,
	          (unsigned long)NOR_SIZE);
	/* [!] Said out loud: every byte was read, so a gap here means every byte of
	 * it is 0xFF.  The one thing that remains indistinguishable from erased is
	 * a sector somebody deliberately wrote 0xFF into. */
	cli_print(sh, "every byte read; a gap is all 0xFF (or written 0xFF)\r\n");

	(void)nor_release(token);
	return stopped ? 1 : 0;
}

static int nor_bring_up(struct nor_svc_result *r);

/* ---- `nor info` ----------------------------------------------------------- */

static int nor_line(nor_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof line)
		n = (int)sizeof line - 1;
	return write(ctx, line, (size_t)n) < 0 ? -1 : 0;
}

static int nor_board_info(nor_svc_common_fn common, nor_svc_write_fn write,
                          void *ctx, struct nor_svc_result *res)
{
	struct nor_svc_info in = { 0 };
	struct nor_report r;
	const char *who;

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
	if (nor_lifecycle_state() == NOR_ST_OFF && nor_bring_up(res) != 0)
		return 1;
	nor_report(&r);

	in.state = nor_state_name(r.state);
	in.state_note = nor_fail_reason() ? nor_fail_reason() : "";
	in.jedec_valid = r.jedec_valid;
	memcpy(in.jedec, r.jedec, sizeof in.jedec);
	in.jedec_note = r.jedec_valid ? "read before XIP"
	                              : "unreadable under XIP";
	if (common(ctx, &in) != 0)
		return -1;

	who = lease_holder(r.live);
	/* What the write seam was COMPILED to allow (issue #88), read out of the
	 * seam's own .rodata record rather than restated here, so the interval
	 * this line shows is the one the wrappers enforce -- and so that
	 * cmake/check_nor_seam.py, which reads the same twelve bytes out of the
	 * linked image, is checking a fact somebody can also see on the device.
	 * It is also the only place `nor erase` and `nor write` get their bounds
	 * from (the range below). */
	if (nor_line(write, ctx, "  window  : 0x%08lx, %lu B\r\n",
	             (unsigned long)NOR_XIP_BASE, (unsigned long)NOR_SIZE) ||
	    nor_line(write, ctx, "  leases  : %lu (mask 0x%lx)%s%s\r\n",
	             (unsigned long)r.readers, (unsigned long)r.live,
	             who ? " -- " : "", who ? who : "") ||
	    nor_line(write, ctx, "  writable: 0x%08lx..0x%08lx, %lu B unit\r\n",
	             (unsigned long)nor_seam_limits.lo,
	             (unsigned long)nor_seam_limits.hi,
	             (unsigned long)nor_seam_limits.unit))
		return -1;
	/* [!] The probe reads the slot-header block, not `blob` (issue #90): a
	 * probe inside blob would be proving the window came back by reading bytes
	 * issue #88's writer is allowed to erase.  The magic is REPORTED, never
	 * required -- a corrupt slot header still boots (the bootloader falls back
	 * to slot 0), so bring-up must not turn somebody else's recoverable damage
	 * into our refusal. */
	if (nor_line(write, ctx, "  probe   : 0x%08lx = 0x%08lx  %s\r\n",
	             (unsigned long)r.probe_off, (unsigned long)r.probe_word,
	             r.probe_hdr ? "(slot-header magic)"
	                         : "(no slot-header magic)") ||
	    /* [!] The observable for issue #86: this line used to be part of the
	     * NPU's EPK wrapset, so `nn close` unwrapped it -- and unwrapping
	     * disables. */
	    nor_line(write, ctx, "  irq     : %d %s\r\n", r.irq,
	             r.irq < 0 ? "(none wrapped)"
	                       : (r.irq_enabled ? "wrapped, enabled" : "WRAPPED BUT OFF")))
		return -1;
	/* [!] SAY WHEN THEY WERE NOT TAKEN, rather than printing the last good
	 * copy (issue #91).  A transaction has the window down, so its SCU and MPU
	 * would describe the transition and not the mapping -- and a stale value
	 * printed without comment is indistinguishable from a current one. */
	if (!r.regs_sampled)
		return (nor_line(write, ctx, "  scu xip : 0x%08lx -> -- (window down, "
		                 "not sampled)\r\n",
		                 (unsigned long)r.scu_xip_before) ||
		        nor_line(write, ctx, "  mpu     : not sampled in state '%s'\r\n",
		                 nor_state_name(r.state))) ? -1 : 0;
	/* Raw, not decoded: the SVD names this register but gives no field
	 * breakdown, and this board does not guess at bits it cannot name. */
	if (nor_line(write, ctx, "  scu xip : 0x%08lx -> 0x%08lx (raw)\r\n",
	             (unsigned long)r.scu_xip_before, (unsigned long)r.scu_xip_after) ||
	    nor_line(write, ctx, "  mpu ctrl: S 0x%08lx  NS 0x%08lx\r\n",
	             (unsigned long)r.mpu_ctrl_s, (unsigned long)r.mpu_ctrl_ns))
		return -1;
	if (r.mpu_region >= 0) {
		if (nor_line(write, ctx, "  mpu rgn : %ld  rbar 0x%08lx  rlar 0x%08lx\r\n",
		             (long)r.mpu_region, (unsigned long)r.mpu_rbar,
		             (unsigned long)r.mpu_rlar))
			return -1;
	} else if (nor_line(write, ctx, "  mpu rgn : none covers the window\r\n")) {
		return -1;
	}
	return nor_line(write, ctx, "  mpu mair: 0x%08lx 0x%08lx\r\n",
	                (unsigned long)r.mpu_mair0, (unsigned long)r.mpu_mair1) ? -1 : 0;
}

/* ---- who holds the part --------------------------------------------------- */

/* [!] ONE SENTENCE FOR THE CAMERA REFUSAL (issue #203), said by both places
 * that can meet it: the reservation (nor_busy_why(), for `nor` and `blob`
 * alike) and a transaction claim (nor_wr_answer()).  It names the commands
 * to type: the camera's own stop, after nn's when nn holds the camera too --
 * `camera stream stop` is refused while nn does. */
#define NOR_CAMERA_WHY \
	"the camera is streaming (camera stream stop; nn stream stop first if " \
	"nn holds it)"

/*
 * Why a reservation was refused, in the words both `nor` and `blob` say.
 * Name the holder when there is exactly one and we know it; the operator can
 * act on "close `nn`" and cannot act on "busy".  Two or more readers are not
 * counted here (`nor info` lists the mask, issue #250).
 */
static const char *nor_busy_why(void)
{
	struct nor_report r;
	const char *who;

	nor_report(&r);
	who = lease_holder(r.live);
	if (who != NULL)
		return who;
	if (r.readers != 0u)
		return "readers hold the window (nor info)";
	/* In the decision's own order: readers first, then the camera
	 * (nor_reserve_decide()) -- and the camera ONLY FROM XIP, the one state
	 * where nothing else refuses a reservation.  RESERVED or WRITING is
	 * another writer whatever the camera is doing, and naming the camera
	 * there would send the operator to stop something that is not in the
	 * way.  Asked again for the words only; the refusal was decided under
	 * the reservation's critical section. */
	if (r.state == NOR_ST_XIP && nor_write_camera_live())
		return NOR_CAMERA_WHY;
	return "another writer holds the part";
}

void nor_cmd_say_busy(struct cli_instance *sh, const char *who_asks)
{
	cli_error(sh, "%s: busy -- %s\r\n", who_asks, nor_busy_why());
}

/* ---- the hold: the writer reservation -------------------------------------- */

/* [!] A WRITE CANNOT BRING THE WINDOW UP -- that is a reader's errand, and
 * nor_write_decide() answers NOR_WR_BUSY from NOR_ST_OFF for a reason worth
 * keeping (nor_state.h).  So the hold does the reader's part first,
 * explicitly, and hands the lease straight back.
 *
 * That leaves a gap between the release and the reservation.  The reservation
 * is what closes it: if anything took a lease in between, it refuses rather
 * than waiting, and the user sees "busy" instead of a window pulled out from
 * under somebody. */
static int nor_bring_up(struct nor_svc_result *r)
{
	uint32_t token;

	if (nor_acquire(NOR_LEASE_SCAN, &token) == 0) {
		(void)nor_release(token);
		return 0;
	}
	if (nor_lifecycle_state() == NOR_ST_FAULTED) {
		r->status = NOR_SVC_DOWN;
		r->why = nor_fail_reason() ? nor_fail_reason() : "faulted";
	} else {
		r->status = NOR_SVC_BUSY;
		r->why = "window not up (a writer holds it)";
	}
	return -1;
}

/* One transaction's report, in the shared shape.  Every field is copied. */
static void nor_wr_answer(enum nor_write_status st,
                          const struct nor_write_report *w,
                          struct nor_svc_result *r)
{
	r->done = w->done;
	r->verified = w->verified;
	r->bad_valid = w->bad_valid;
	r->bad_off = w->bad_off;
	r->bad_got = w->bad_got;
	r->bad_want = w->bad_want;
	r->jedec_valid = w->jedec_ok;
	memcpy(r->jedec, w->jedec, sizeof r->jedec);
	r->why = w->fail;
	switch (st) {
	case NOR_WRITE_OK:           r->status = NOR_SVC_OK;           break;
	case NOR_WRITE_BUSY:
		r->status = NOR_SVC_BUSY;
		r->why = "a reader holds the window";
		break;
	case NOR_WRITE_CAMERA:
		/* Nothing was claimed and nothing went on the wire (#203). */
		r->status = NOR_SVC_BUSY;
		r->why = NOR_CAMERA_WHY;
		break;
	case NOR_WRITE_REFUSED:      r->status = NOR_SVC_REFUSED;      break;
	case NOR_WRITE_NO_TRANSPORT: r->status = NOR_SVC_NO_TRANSPORT; break;
	case NOR_WRITE_INCOMPLETE:
		/* [!] TWO DIFFERENT EVENTS SHARE THIS STATUS and only one of them is
		 * the caller's doing; a Ctrl+C is not "the vendor returned 0". */
		r->status = w->cancelled ? NOR_SVC_CANCELLED : NOR_SVC_INCOMPLETE;
		if (!w->cancelled && r->why == NULL)
			r->why = "the vendor refused part way";
		break;
	case NOR_WRITE_FAULTED:
	default:
		/* Terminal by design: a mismatch cannot be told from a window that
		 * is lying about the array (port/nor/nor_write.c). */
		r->status = NOR_SVC_FAULTED;
		break;
	}
}

/* The XIP-down progress (nor_write.h): the shared tick prints and polls
 * Ctrl+C and nothing else; this only passes it on. */
static int nor_wr_tick(void *ctx, uint32_t done, uint32_t total)
{
	const struct nor_svc_progress *p = (const struct nor_svc_progress *)ctx;

	return p->tick(p->ctx, done, total);
}

static void nor_op_erase(void *dev, uint32_t addr, uint32_t len,
                         const struct nor_svc_progress *p, struct nor_svc_result *r)
{
	struct nor_erase_progress prog = { (void *)p, nor_wr_tick };
	struct nor_write_report w;
	enum nor_write_status st;

	memset(&w, 0, sizeof w);
	st = nor_write_erase(*(const uint32_t *)dev, addr, len,
	                     p != NULL ? &prog : NULL, &w);
	nor_wr_answer(st, &w, r);
}

static void nor_op_program(void *dev, uint32_t addr, const uint8_t *data,
                           uint32_t len, struct nor_svc_result *r)
{
	struct nor_write_report w;
	enum nor_write_status st;

	memset(&w, 0, sizeof w);
	st = nor_write_program(*(const uint32_t *)dev, addr, data, len, &w);
	nor_wr_answer(st, &w, r);
}

/*
 * Bring the part up if nobody has, take the writer reservation (#91), run
 * @p body, give it back.
 *
 * [!] THE TOKEN STAYS LOCAL AND THIS IS ITS ONE EXIT.  A reservation that is
 * taken and not returned leaves a port whose every answer is "busy" for the
 * rest of the session -- the hazard nor_write.h describes one level down.
 *
 * The operations take the token from @p ops->dev, which points at this
 * frame: valid for the call of body only, as the contract says.
 */
static void nor_board_hold(nor_svc_body_fn body, void *ctx,
                           struct nor_svc_result *r)
{
	uint32_t token = 0u;
	struct nor_svc_ops ops = { .dev = &token, .erase = nor_op_erase,
	                           .program = nor_op_program };

	if (nor_bring_up(r) != 0)
		return;
	if (nor_reserve(&token) != 0) {
		if (nor_lifecycle_state() == NOR_ST_FAULTED) {
			r->status = NOR_SVC_DOWN;
			r->why = nor_fail_reason() ? nor_fail_reason() : "faulted";
		} else {
			r->status = NOR_SVC_BUSY;
			r->why = nor_busy_why();
		}
		return;
	}
	body(ctx, &ops);
	(void)nor_unreserve(token);
	r->status = NOR_SVC_OK;
}

/* ---- the range: the seam's writable interval ------------------------------ */

/* [!] FROM THE SEAM'S OWN RECORD (nor_seam_limits), the interval `nor info`
 * prints and the seam enforces on every vendor call; this file holds no copy.
 * A write starts on a whole 32-bit word (the transport reverses the bytes of
 * each word, nor_span.h); its length may be anything, the writer pads the
 * last word.  An early answer: nor_write.c and the seam decide again. */
static void nor_board_range(enum nor_svc_op op, uint32_t addr, uint32_t len,
                            struct nor_svc_range *out)
{
	out->lo = nor_seam_limits.lo;
	out->hi = nor_seam_limits.hi;
	out->name = "writable interval";
	out->hint = "";
	if (addr < nor_seam_limits.lo || addr >= nor_seam_limits.hi ||
	    len > nor_seam_limits.hi - addr) {
		out->kind = NOR_SVC_RANGE_OUTSIDE;
		return;
	}
	if (op == NOR_SVC_OP_WRITE && (addr % 4u) != 0u) {
		out->kind = NOR_SVC_RANGE_ALIGN;
		out->lo = 4u;
		out->hi = 0u;
		return;
	}
	out->kind = NOR_SVC_RANGE_OK;
}

/* ---- `nor cycle` ---------------------------------------------------------- */

struct nor_cycle_ctx {
	struct cli_instance *sh;
	struct nor_svc_result r;
};

static void nor_cycle_body(void *ctx, const struct nor_svc_ops *ops)
{
	struct nor_cycle_ctx *c = (struct nor_cycle_ctx *)ctx;
	struct nor_write_report w;
	enum nor_write_status st;

	/* [!] SAID BEFORE IT RUNS, and phrased as the COST OF THE OPERATION rather
	 * than as an announcement that it is happening.  This is the subcommand
	 * somebody reaches for to "check without changing anything", so the cost
	 * belongs in front of it -- but the transaction can still be refused (a
	 * reader lease), and a line that said "this is now writing the status
	 * register" followed by "busy" would be describing something that did not
	 * happen. */
	cli_print(c->sh, "cycle: 2 status-register writes, no data\r\n");
	memset(&w, 0, sizeof w);
	st = nor_write_cycle(*(const uint32_t *)ops->dev, &w);
	nor_wr_answer(st, &w, &c->r);
}

int nor_board_cycle(struct cli_instance *sh, int argc, char **argv)
{
	struct nor_cycle_ctx c;
	struct nor_svc_result h;

	(void)argc;
	(void)argv;
	/* [!] FOREGROUND ONLY (issue #203), the shared question.  The writer
	 * refuses while the camera streams, and the check holds only because
	 * nothing can start the camera between a transaction's claim and its
	 * commit -- true for a writer on the foreground shell thread, which
	 * background jobs run below and cannot preempt, and false for one in a
	 * background job: a foreground `camera stream start`, woken by the RX
	 * interrupt, preempts it mid-program. */
	if (nor_console_refuse(sh) != 0)
		return 1;
	memset(&c, 0, sizeof c);
	memset(&h, 0, sizeof h);
	c.sh = sh;
	nor_board_hold(nor_cycle_body, &c, &h);
	if (h.status != NOR_SVC_OK)
		return nor_cmd_not_held(sh, "cycle", 0u, 0u, &h);
	return nor_cmd_report(sh, "cycle", 0u, 0u, &c.r);
}

const struct nor_svc_board nor_svc_board = {
	.part          = "QSPI NOR at 0x3A000000",
	.size          = NOR_SIZE,
	.erase_unit    = NOR_ERASE_GRAN,
	.page          = NOR_PROGRAM_PAGE,
	.erase_cancels = 1u,
	.info          = nor_board_info,
	.range         = nor_board_range,
	.hold          = nor_board_hold,
};
