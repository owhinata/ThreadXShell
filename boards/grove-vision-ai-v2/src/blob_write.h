/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_write.h
 * @brief   One `blob write`, from the erase to the magic page (#92).
 *
 * The coordinator, and it takes its operations as a VTABLE.  Not for
 * abstraction -- there is exactly one production implementation and it is fifty
 * lines below the test that matters -- but because the thing worth checking
 * about this file cannot be produced on a board: that whatever goes wrong, the
 * NOR reservation and the console claim are each given back EXACTLY ONCE.
 *
 * A `blob write` holds two things at once.  The reservation (#91) is what keeps
 * a reader from taking the part between the erase and the last program, and the
 * console claim is what stops the shell's line editor eating the sender's
 * bytes.  Between them there are seven ways to leave: the reservation refused,
 * the erase cancelled, the erase incomplete, the console busy, the transfer
 * timed out or cancelled, a program failed, the header failed.  Each has to
 * unwind exactly what it took, and hardware can demonstrate perhaps two of them
 * -- so test/test_blob_write.c drives all seven through this vtable and counts.
 *
 * THE ORDER IS THE DESIGN, and each step is here because of something learned
 * elsewhere:
 *
 *   1. reserve the part.  A token that stays LOCAL until it is committed, the
 *      way npu_hw_init() does it: a caller that fails at step 3 must not leave
 *      a token anybody could mistake for live.
 *   2. erase the whole slot, in ONE transaction, with a progress callback that
 *      can cancel between sectors.  ~40 s for a 2 MB slot on this die, which is
 *      why it happens here rather than inside the protocol.
 *   3. [!] ONLY IF THE ERASE REPORTED OK.  Programming into flash whose erase
 *      was cut short reads back as the AND of the two, and nor_write.c makes
 *      that a TERMINAL fault -- so an ordinary Ctrl+C would kill the port.
 *   4. claim the console -- AFTER the erase, so that the erase's progress and
 *      its cancel work normally, and so a 40 s wait does not happen with the
 *      line editor already handed over.
 *   5. receive.  The sink accumulates into the staging buffer and programs a
 *      chunk at a time; a program that does not report OK latches, and no
 *      further program or header follows it.
 *   6. release the console, then write the body page, read it back and only
 *      then write the magic page.  Split so that every outcome is either "no
 *      magic, so not valid" or "magic, so the body was read back and matched".
 *   7. verify the payload against the CRC of the stream that arrived, and give
 *      the reservation back.  ONE exit, whatever happened.
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
	BLOB_WRITE_PROG_FAILED,  /**< a program or the header did not take      */
	BLOB_WRITE_VERIFY_FAILED,/**< stored, and the read-back disagrees       */
	BLOB_WRITE_NO_SLOT,      /**< nothing to write into; see @ref choice     */
	BLOB_WRITE_REFUSED,      /**< the request never got as far as the part  */
};

/** Short name, for the console and the log. */
const char *blob_write_result_name(enum blob_write_result r);

/** What one `blob write` did, filled in on every outcome. */
struct blob_write_report {
	unsigned slot;        /**< the slot chosen, when one was               */
	enum blob_choice choice; /**< why it was chosen, or why nothing was    */
	uint32_t erased;      /**< bytes the erase reported done                */
	uint32_t received;    /**< payload bytes the sink accepted              */
	uint32_t declared;    /**< bytes block 0 announced, 0 if it never came  */
	uint32_t crc;         /**< CRC-32 of what arrived                       */
	uint32_t verified;    /**< CRC-32 read back, when a verify ran          */
	uint32_t transactions;/**< NOR write transactions this run spent        */
	int      vendor_rc;   /**< the last writer status, for the log          */
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
 * @param want  the slot the operator named, or negative for none
 * @param rep   filled in on every outcome; may not be NULL
 *
 * The caller has validated the name (blob_name_check()); everything from the
 * reservation onwards is here, INCLUDING which slot the write goes to.  That
 * decision needs the reservation to already be held -- see @ref
 * blob_write_ops::stat -- so it cannot live in the caller.
 */
enum blob_write_result blob_write_run(const struct blob_write_ops *ops,
                                      const char *name, int want,
                                      struct blob_write_report *rep);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_WRITE_H */
