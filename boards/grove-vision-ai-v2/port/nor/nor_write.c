/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_write.c
 * @brief   The bounded NOR write transaction (issue #88 Part C).
 *
 * The one translation unit allowed to call the vendor's erase and program
 * entry points; board.cmake names this object as cmake/check_nor_seam.py's
 * single authorised caller, and nor_write.h describes the transaction it runs.
 *
 * WHAT IS DECIDED WHERE, because three files are involved on purpose:
 *
 *   nor_span.c   which bytes a request names, and what an erase rounds them to
 *   nor_state.c  whether a write may start at all, from the lifecycle
 *   nor_seam.c   the same bounds again, at the door to the vendor
 *
 * This file asks nor_span before it claims the part -- so that a refusal costs
 * nothing and can say why -- and the seam asks again on every single call that
 * gets through.  The second check is not redundant: it is the one that holds
 * when the caller is not this file.
 *
 * [!] A READ-BACK MISMATCH IS TERMINAL, AND THAT IS THE DELIBERATE PART.
 * Faulting the port over a few bytes in `blob` looks disproportionate -- it
 * stops `nn open` reading a model that has nothing to do with the range that
 * failed, until the board is reset.  The reason is that the mismatch has two
 * explanations and this code cannot tell them apart:
 *
 *   - the array did not take what the vendor said it took, or
 *   - the window is not telling the truth about the array.
 *
 * The second one makes every subsequent read of this part suspect, including
 * the model `nn` parses in place.  A port that carried on would be serving
 * reads it has just been given evidence against.
 *
 * The cases that do NOT fault are the ones where that ambiguity is absent: a
 * vendor entry point that refused before putting anything on the wire, and a
 * transport that never answered.  Those are reported as incomplete, with the
 * window brought back and re-probed, and the port stays usable.
 */
#include "nor_write.h"

#include <stddef.h>
#include <string.h>

#include "WE2_device.h"             /* __DSB / __ISB                        */

#include "nor_flash.h"
#include "nor_seam.h"
/* The vendor prototypes.  The INNER (qspi) forms, deliberately: those are the
 * four names board.cmake redirects with -Wl,--wrap, so a call here goes through
 * port/sdk_seam/nor_seam.c.  Calling the outer hx_lib_spi_eeprom_* forwarders
 * instead would reach the same code with the seam bypassed, which is why they
 * stay on check_placement_budget.py's absence list and why check_nor_seam.py
 * has a rule of its own (N11/N16) about them being dead. */
#include "qspi_eeprom_interface.h"

#define LOG_TAG "nor"
#include "log.h"

/*
 * The staging buffer one program page goes through.
 *
 * [!] IT IS NOT HERE FOR DMA REACHABILITY, and an earlier reading of this board
 * had it backwards.  The vendor's write memcpy's the caller's bytes into its
 * own pool in DTCM before any transfer starts, and the bring-up read that
 * identified the fitted die used a DTCM stack buffer in both directions -- so
 * this port's "DMA cannot see TCM" rule is about SSPI and WDMA3, not this path.
 *
 * It is here because hx_lib_qspi_eeprom_write() takes its payload as
 * `uint8_t *` and, on the word_switch path this port refuses, byte-swaps it IN
 * PLACE.  Handing it a caller's buffer would mean a prebuilt archive holds a
 * writable pointer into memory this file promised not to change -- and the
 * read-back afterwards compares the array against that same buffer, so it would
 * be comparing against whatever came back rather than against what was sent.
 */
static uint8_t page_buf[NOR_PROGRAM_PAGE];

/* Which of the three shapes one transaction has.  The sequence is identical;
 * only step 4 differs, and for a cycle it is empty. */
enum txn_op {
	TXN_CYCLE = 0,
	TXN_ERASE,
	TXN_PROGRAM,
};

const char *nor_write_status_name(enum nor_write_status s)
{
	switch (s) {
	case NOR_WRITE_OK:           return "ok";
	case NOR_WRITE_BUSY:         return "busy";
	case NOR_WRITE_REFUSED:      return "refused";
	case NOR_WRITE_NO_TRANSPORT: return "no transport";
	case NOR_WRITE_INCOMPLETE:   return "incomplete";
	case NOR_WRITE_FAULTED:      return "faulted";
	default:                     break;
	}
	return "?";
}

/* [!] THE FIRST REASON WINS, and it is the same rule nor_fail_reason() follows.
 * A transaction can fail more than once -- a window that would not go down is
 * usually a window that will not come back either -- and the LAST reason is the
 * consequence, not the cause.  Latching the first also keeps this report and
 * `nor info` from naming two different failures for one event. */
static void note(struct nor_write_report *r, const char *why)
{
	if (r->fail == NULL)
		r->fail = why;
}

/*
 * Step 4, erase.  One unit per call, because that is the only unit this die has
 * been seen erasing (issue #88) and the seam refuses the others.
 *
 * [!] A ZERO RETURN PROVES NOTHING.  hx_lib_qspi_eeprom_erase_sector returns
 * hx_lib_spi_eeprom_clear_write_protect's result and discards everything after
 * it -- and that helper has exactly one exit, `movs r0,#0`, so the success path
 * is a constant.  The read-back is what settles whether the sector went.  What
 * a NON-zero return does mean is definite, though: -28 for a window that is
 * still up, -50 for a write-enable latch that would not set after 21 tries.
 * Both are refusals issued before an erase opcode reaches the wire, so the loop
 * stops there rather than carrying on into sectors that would refuse too.
 */
static void erase_run(struct nor_span span, struct nor_write_report *r)
{
	uint32_t unit = nor_seam_limits.unit;

	for (uint32_t off = 0u; off < span.len; off += unit) {
		int32_t rc = hx_lib_qspi_eeprom_erase_sector(span.addr + off,
		                                             FLASH_SECTOR);
		if (rc != 0) {
			r->vendor_rc = rc;
			return;
		}
		r->done = off + unit;
	}
}

/*
 * Step 4, program.  Split on the part's program page, through page_buf.
 *
 * The vendor splits on the same boundary internally, so this is not what makes
 * the transfer legal.  It buys two things.  One: each vendor call reads out of
 * a buffer this file owns.  Two -- and this is what the read-back depends on --
 * a chunk that does not cross a page boundary is exactly ONE iteration of the
 * vendor's own loop, so a refusal from it is a refusal of that page and of
 * nothing before it.  Handing the vendor a long buffer instead would let it
 * program several pages and then return -50, and `done` would be a lie.
 *
 * hx_lib_qspi_eeprom_write returns a hard-coded 0, so again only a negative
 * says anything.
 */
static void program_run(struct nor_span span, const uint8_t *src,
                        struct nor_write_report *r)
{
	uint32_t done = 0u;

	while (done < span.len) {
		uint32_t n = nor_span_page_chunk(span.addr + done, span.len - done,
		                                 (uint32_t)sizeof(page_buf));
		int32_t rc;

		/* Cannot happen with a non-zero cap and a non-zero remainder; a zero
		 * would be a loop that does not advance, so it stops here instead. */
		if (n == 0u) {
			note(r, "the payload could not be split into program pages");
			return;
		}
		memcpy(page_buf, src + done, n);
		rc = hx_lib_qspi_eeprom_write(span.addr + done, page_buf, n, 0u);
		if (rc != 0) {
			r->vendor_rc = rc;
			return;
		}
		done += n;
		r->done = done;
	}
}

/*
 * Step 6: read back, through the window that has just been brought up, exactly
 * the prefix the vendor accepted.
 *
 * [!] EXACTLY THE PREFIX, not the whole span.  Verifying bytes the vendor
 * already said it refused would turn a refusal this code understands into a
 * mismatch it does not, and mismatches are terminal.  Nothing is claimed about
 * the remainder, and NOR_WRITE_INCOMPLETE is how the caller is told so.
 */
static int verify(enum txn_op op, struct nor_span span, const uint8_t *src,
                  struct nor_write_report *r)
{
	const volatile uint8_t *win;

	if (r->done == 0u)
		return 0;

	/* The vendor's XIP restore invalidates 512 bytes at the base of the window
	 * and nothing else, so without this the comparison could be answered out of
	 * lines cached before the window went down -- by `nor scan`, or by `nn`
	 * parsing a model -- and never reach the part at all. */
	nor_alias_invalidate(span.addr, r->done);
	__DSB();
	__ISB();

	win = (const volatile uint8_t *)(uintptr_t)(NOR_XIP_BASE + span.addr);
	for (uint32_t i = 0u; i < r->done; i++) {
		uint8_t want = (op == TXN_ERASE) ? 0xFFu : src[i];
		uint8_t got  = win[i];

		if (got != want) {
			r->bad_off   = span.addr + i;
			r->bad_got   = got;
			r->bad_want  = want;
			r->bad_valid = 1u;
			r->verified  = i;
			note(r, (op == TXN_ERASE)
			        ? "a sector did not read back erased"
			        : "the array did not read back what was written");
			return -1;
		}
	}
	r->verified = r->done;
	return 0;
}

/* The transaction.  nor_write.h has the numbered sequence; this is it. */
static enum nor_write_status run(enum txn_op op, struct nor_span span,
                                 const uint8_t *src,
                                 struct nor_write_report *r)
{
	enum nor_write_status st = NOR_WRITE_OK;
	enum nor_write claim;
	int commit_ok;

	/* 1. Claim.  Nothing below this point may return without committing. */
	claim = nor_write_claim();
	if (claim == NOR_WR_BUSY)
		return NOR_WRITE_BUSY;
	if (claim != NOR_WR_GO)
		return NOR_WRITE_FAULTED;

	r->span = span;

	/* 2. Drop the window, established by reading the SCU back. */
	if (nor_window_drop() != 0) {
		note(r, "the flash window could not be taken down");
		st = NOR_WRITE_FAULTED;
	/* 3. The canary: the DMA receive path every part of the vendor's write
	 *    path is built on, on a call that changes nothing.  See nor_write.h. */
	} else if (nor_jedec_recheck(r->jedec) != 0) {
		note(r, "the part did not answer with the flash window down");
		st = NOR_WRITE_NO_TRANSPORT;
	} else {
		r->jedec_ok = 1u;
		/* 4. The operation, if this shape has one. */
		if (op == TXN_ERASE)
			erase_run(span, r);
		else if (op == TXN_PROGRAM)
			program_run(span, src, r);
		if (r->done < span.len) {
			note(r, "the vendor refused part-way through");
			st = NOR_WRITE_INCOMPLETE;
		}
	}

	/* 5. Every reader is owed a window back, whatever happened above.  This is
	 *    also where a quad-enable bit that did not come back gets caught: the
	 *    restore's probe reads the firmware header through the continuous quad
	 *    read, which a part still in single mode cannot answer. */
	if (nor_window_restore() != 0) {
		note(r, "the flash window did not come back");
		st = NOR_WRITE_FAULTED;
	} else if (st != NOR_WRITE_FAULTED) {
		/* 6. Read back what was accepted. */
		if (verify(op, span, src, r) != 0)
			st = NOR_WRITE_FAULTED;
	}

	/* 7. Commit.  NOR_ST_XIP only for the outcomes that leave this port able to
	 *    answer for what it is serving; see the file comment for why a mismatch
	 *    is not one of them. */
	commit_ok = (st == NOR_WRITE_OK || st == NOR_WRITE_INCOMPLETE ||
	             st == NOR_WRITE_NO_TRANSPORT);
	nor_write_commit(commit_ok, r->fail);
	if (!commit_ok)
		return NOR_WRITE_FAULTED;
	return st;
}

/* Zero the report before anything can be recorded in it, so that a refusal
 * leaves a caller reading zeroes rather than a previous transaction's numbers. */
static void report_init(struct nor_write_report *r)
{
	memset(r, 0, sizeof(*r));
}

enum nor_write_status nor_write_cycle(struct nor_write_report *r)
{
	struct nor_span span = { 0u, 0u };

	if (r == NULL)
		return NOR_WRITE_REFUSED;
	report_init(r);
	return run(TXN_CYCLE, span, NULL, r);
}

enum nor_write_status nor_write_erase(uint32_t addr, uint32_t len,
                                      struct nor_write_report *r)
{
	struct nor_span span;
	enum nor_span_verdict v;

	if (r == NULL)
		return NOR_WRITE_REFUSED;
	report_init(r);

	/* Asked BEFORE the claim: a request this port may not act on should cost
	 * nothing, and should be able to say which rule it broke.  The seam asks
	 * the same question again on every call that gets through. */
	v = nor_span_erase(nor_seam_limits.lo, nor_seam_limits.hi,
	                   nor_seam_limits.unit, addr, len, &span);
	if (v != NOR_SPAN_OK) {
		r->span.addr = addr;
		r->span.len  = len;
		r->fail = nor_span_verdict_name(v);
		return NOR_WRITE_REFUSED;
	}
	return run(TXN_ERASE, span, NULL, r);
}

enum nor_write_status nor_write_program(uint32_t addr, const void *data,
                                        uint32_t len,
                                        struct nor_write_report *r)
{
	struct nor_span span;
	enum nor_span_verdict v;

	if (r == NULL)
		return NOR_WRITE_REFUSED;
	report_init(r);

	if (data == NULL) {
		r->span.addr = addr;
		r->span.len  = len;
		r->fail = "no payload";
		return NOR_WRITE_REFUSED;
	}
	v = nor_span_program(nor_seam_limits.lo, nor_seam_limits.hi, addr, len,
	                     &span);
	if (v != NOR_SPAN_OK) {
		r->span.addr = addr;
		r->span.len  = len;
		r->fail = nor_span_verdict_name(v);
		return NOR_WRITE_REFUSED;
	}
	return run(TXN_PROGRAM, span, (const uint8_t *)data, r);
}
