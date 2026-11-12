/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_write.h
 * @brief   One `blob write`, from the erase to the magic page (#92; shared
 *          since #192).
 *
 * The coordinator, and it takes its operations as a VTABLE.  Not for
 * abstraction -- there is exactly one production implementation and it is fifty
 * lines below the test that matters -- but because the thing worth checking
 * about this file cannot be produced on a board: that whatever goes wrong, the
 * board writer's reservation and the console claim are each given back EXACTLY
 * ONCE.
 *
 * A `blob write` holds two things at once.  The reservation is what keeps a
 * reader from taking the part between the erase and the last program, and the
 * console claim is what stops the shell's line editor eating the sender's
 * bytes.  Between them there are eight ways to leave: the reservation refused,
 * the erase cancelled, the erase incomplete, the console busy, the transfer
 * timed out or cancelled, a program failed, the header failed, the read-back
 * disagreed.  Each has to
 * unwind exactly what it took, and hardware can demonstrate perhaps two of them
 * -- so shell/test/test_blob_write.c drives all of them through this vtable and
 * counts.
 *
 * THE ORDER IS THE DESIGN, and each step is here because of something learned
 * elsewhere:
 *
 *   1. reserve the part.  A token that stays LOCAL until it is committed: a
 *      caller that fails at step 3 must not leave a token anybody could
 *      mistake for live.
 *   2. erase the whole slot, in ONE transaction, with a progress callback that
 *      can cancel between sectors.  A slot erase can take tens of seconds on a
 *      board's flash, which is why it happens here rather than inside the
 *      protocol.
 *   3. [!] ONLY IF THE ERASE REPORTED OK.  Programming into flash whose erase
 *      was cut short reads back as the AND of the two, and a board writer that
 *      reads its programs back may treat that as a TERMINAL fault -- so an
 *      ordinary Ctrl+C could kill the port.
 *   4. claim the console -- AFTER the erase, so that the erase's progress and
 *      its cancel work normally, and so a long erase does not happen with the
 *      line editor already handed over.
 *   5. receive.  The sink accumulates into the staging buffer and programs a
 *      chunk at a time; a program that does not report OK latches, and no
 *      further program or header follows it.  A receive refused because another
 *      transfer runs (YM_ERR_BUSY, #230) comes after the erase (#197).
 *   6. release the console, then write the body page.  Whether the board
 *      writer reads it back depends on the board (issue #194):
 *        grove-vision-ai-v2  yes -- every program is one port/nor/nor_write.c
 *                            transaction, which reads what it wrote back and
 *                            treats a mismatch as a terminal fault;
 *        wio-lite-ai,        no -- nor_write() / qspi_flash_write() report a
 *        f746g-disco         failed page (f746 from the flag status
 *                            register) but do not re-read it.
 *      Where it is not read back, the body is still not trusted on its word:
 *      it carries its own CRC, which the decoder checks, so a body that went
 *      down wrong reads INVALID, never VALID -- and the payload under it is
 *      read back whole against the stream's CRC in step 7, before the magic.
 *   7. verify the payload against the CRC of the stream that arrived.
 *   8. [!] ONLY IF IT MATCHED, write the magic page -- the one thing that
 *      makes the slot VALID.  So every outcome is either "no magic, so not
 *      valid" or "magic, so the body was written and the payload was read
 *      back and matched".  A read-back that disagrees, or cannot be done,
 *      leaves the slot INCOMPLETE with the body still readable (#194; until
 *      then the magic went down first and a mismatch left a slot that read
 *      valid).  A magic program that FAILS leaves the page in a state the
 *      coordinator cannot know -- still erased (INCOMPLETE), part-written
 *      (INVALID) or whole (VALID, over a payload that did match) -- so the
 *      caller says so and points at `blob list`.
 *   9. give the reservation back.  ONE exit, whatever happened.
 *
 * The ops are built by the shared command (shell/cmds/cmd_blob.c) over each
 * board's src/blob_writer.c (svc/blob/blob_svc.h); a board's measurements are
 * in its README's store section.
 *
 * [!] WHAT THE CONTRACT DOES NOT COVER: tx_thread_terminate().  The shell's
 * kill is co-operative -- cli_read_byte() returns -2 and this unwinds -- but a
 * thread torn down from outside leaves C with nothing to run, and the
 * reservation with it.  That is the limit of the guarantee.
 */
#ifndef BLOB_WRITE_H
#define BLOB_WRITE_H

#include <stdint.h>

#include "blob_state.h"
#include "ymodem.h"    /* struct ym_sink -- the receive vtable this drives */

#ifdef __cplusplus
extern "C" {
#endif

/** How one `blob write` ended. */
enum blob_write_result {
	BLOB_WRITE_STORED = 0,   /**< header written and the payload verified   */
	BLOB_WRITE_BUSY,         /**< the part is somebody else's just now      */
	BLOB_WRITE_NO_CONSOLE,   /**< background job, or the console is claimed */
	BLOB_WRITE_CANCELLED,    /**< the erase was interrupted between sectors */
	BLOB_WRITE_ERASE_FAILED, /**< the erase did not finish; nothing sent    */
	BLOB_WRITE_XFER_FAILED,  /**< the transfer did not complete             */
	BLOB_WRITE_PROG_FAILED,  /**< a program or the header did not take; if
	                              it was the magic, the slot's state is not
	                              known (`blob list` reads it)             */
	BLOB_WRITE_VERIFY_FAILED,/**< the read-back disagrees or failed; no magic
	                              was written, the slot reads INCOMPLETE   */
	BLOB_WRITE_NO_SLOT,      /**< nothing to write into; see @ref choice     */
	BLOB_WRITE_REFUSED,      /**< the request never got as far as the part  */
};

/** Short name, for the console and the log. */
const char *blob_write_result_name(enum blob_write_result r);

/**
 * What one `blob write` did, filled in on every outcome.
 *
 * [!] READ @ref slot ONLY WHEN @ref slot_chosen IS SET.  The report starts
 * zeroed, and zero is a real slot number -- and BLOB_CHOICE_REUSE is 0 too --
 * so a write refused before the choice (background job, busy part, no table,
 * an unreadable header) would otherwise read as "slot 0, reused" (#194).
 * @ref choice is meaningful when a slot was chosen or the result is
 * BLOB_WRITE_NO_SLOT.
 */
struct blob_write_report {
	unsigned slot;        /**< the slot chosen; see @ref slot_chosen       */
	int      slot_chosen; /**< non-zero once @ref slot names a chosen slot */
	enum blob_choice choice; /**< why it was chosen, or why nothing was    */
	uint32_t erased;      /**< bytes the erase reported done                */
	uint32_t received;    /**< payload bytes the sink accepted              */
	uint32_t declared;    /**< bytes block 0 announced, 0 if it never came  */
	uint32_t crc;         /**< CRC-32 of what arrived                       */
	uint32_t verified;    /**< CRC-32 read back, when a verify ran          */
	uint32_t transactions;/**< board write transactions this run spent      */
	int      vendor_rc;   /**< the last writer status, for the log          */
	/** Non-zero when the result is BLOB_WRITE_PROG_FAILED because the MAGIC
	 *  page's program failed -- after the payload was read back and matched.
	 *  The slot may then read incomplete, invalid or valid, and the caller
	 *  has to say so rather than guess (#194). */
	int      magic_failed;
};

/**
 * The operations one write performs.  Every one of them can fail, and the test
 * fails each in turn.
 *
 * [!] @ref reserve HANDS BACK A TOKEN, and @ref unreserve is called exactly
 * once with it -- never with zero, never twice.  The coordinator keeps it in a
 * local until the run is over.
 */
struct blob_write_ops {
	void *ctx;
	/** Take the writer reservation.  0 and a non-zero token, or <0. */
	int  (*reserve)(void *ctx, uint32_t *token);
	/** Give it back.  Called once, on every path that reserved. */
	void (*unreserve)(void *ctx, uint32_t token);
	/** Erase [addr, addr+len).  Returns the writer's status; the caller
	 *  treats anything but 0 as "do not send".  @p cancelled receives
	 *  non-zero when it stopped because the operator asked. */
	int  (*erase)(void *ctx, uint32_t token, uint32_t addr, uint32_t len,
	              uint32_t *done, int *cancelled);
	/** Program @p len bytes.  0 on success. */
	int  (*program)(void *ctx, uint32_t token, uint32_t addr,
	                const void *data, uint32_t len);
	/**
	 * Could a console claim ever succeed?  0 yes, <0 never.
	 *
	 * [!] ASKED FIRST, BEFORE THE RESERVATION, AND NOT OPTIONAL.  A
	 * background job can never claim the console -- the RX ring is a strict
	 * SPSC pipe owned by the foreground thread, so cli_console_claim()
	 * refuses on `sh->fg != NULL` alone.  That is knowable with no side
	 * effects, and the order used to be: reserve, choose, ERASE THE SLOT,
	 * then discover it.  `blob write existing 0 &` destroyed two megabytes
	 * and printed "drop the trailing '&'".
	 *
	 * [!] THE SAME ANSWER AS THE CLAIM, AND IT STAYS THE SAME UNTIL THE CLAIM
	 * (issue #229).  The command asks cli_console_claimable(), the function
	 * cli_console_claim() itself decides by (a background job, a claim count
	 * at its limit).  The count changes only when this thread claims or
	 * releases, and nothing called between this question and claim_console()
	 * -- slot_count, reserve, the state and position reads, the notice, the
	 * erase -- claims or releases a console; keep it so, or this question
	 * stops speaking for the claim.  What it cannot ask is the lock itself
	 * failing, as before.
	 */
	int  (*console_ready)(void *ctx);
	/** Claim the console for raw byte IO.  0, or <0 (background / busy). */
	int  (*claim_console)(void *ctx);
	/** Give the console back.  Called once, on every path that claimed. */
	void (*release_console)(void *ctx);
	/** Run one YMODEM receive into @p sink.  0 when the batch completed. */
	int  (*receive)(void *ctx, const struct ym_sink *sink);
	/** Read @p len payload bytes back for the verify.  0 on success. */
	int  (*read_back)(void *ctx, uint32_t addr, void *buf, uint32_t len);
	/** Optional: told what the sender called the file, for the log.  The
	 *  stored name is the CLI's argument and never this. */
	void (*note_sender_name)(void *ctx, const char *name, uint32_t size);
	/**
	 * Optional: the slot has been chosen and is about to be erased.
	 *
	 * [!] THE POINT IS WHEN IT IS CALLED.  A `blob write` destroys a slot
	 * before the transfer starts, so an operator has to be told -- but every
	 * refusal above happens FIRST, and printing "whatever is in it is gone"
	 * ahead of "that name is in another slot" says something untrue about a
	 * slot nothing touched.  It was doing exactly that until the messages
	 * were read on hardware.
	 */
	void (*announce)(void *ctx, unsigned slot, uint32_t base, uint32_t bytes);
	/** How many slots the table has. */
	unsigned (*slot_count)(void *ctx);
	/** Decode one slot's header.  [!] CALLED WITH THE RESERVATION HELD, and
	 *  that is the whole reason the choice happens in here: a scan taken
	 *  before the reservation describes a part somebody else may write next,
	 *  so a slot that was EMPTY when it was chosen could hold a blob by the
	 *  time it is erased.  0 on success. */
	int  (*stat)(void *ctx, unsigned slot, struct blob_info *info);
	/** Where a slot is.  0 on success. */
	int  (*geometry)(void *ctx, unsigned slot, uint32_t *base,
	                 uint32_t *payload_addr, uint32_t *payload_max);
};

/**
 * @brief  Store the file the PC is about to send, under @p name.
 *
 * @param stage      the caller's staging buffer: the payload accumulates here
 *                   and is programmed one full buffer per transaction, the two
 *                   header pages are laid out here, and the verify reads the
 *                   payload back through it
 * @param stage_len  its size in bytes: at least one header page
 *                   (BLOB_HDR_PAGE) and a whole number of them
 * @param want       the slot the operator named, or negative for none
 * @param rep        filled in on every outcome; may not be NULL
 *
 * [!] THE BUFFER IS CHECKED BEFORE ANYTHING IS TAKEN.  A NULL @p stage, or a
 * @p stage_len that is not a non-zero multiple of the header page, is
 * BLOB_WRITE_REFUSED with the console never asked about, never claimed and the
 * part never reserved -- so a caller that got the buffer wrong has destroyed
 * nothing and has nothing to give back.  The coordinator keeps no buffer of
 * its own and no size: the chunk one program transaction takes is whatever the
 * caller hands over (grove-vision-ai-v2: blob_stage_buf, BLOB_STAGE_BYTES).
 *
 * The caller has validated the name (blob_name_check()); everything from the
 * reservation onwards is here, INCLUDING which slot the write goes to.  That
 * decision needs the reservation to already be held -- see @ref
 * blob_write_ops::stat -- so it cannot live in the caller.
 */
enum blob_write_result blob_write_run(const struct blob_write_ops *ops,
                                      uint8_t *stage, uint32_t stage_len,
                                      const char *name, int want,
                                      struct blob_write_report *rep);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_WRITE_H */
