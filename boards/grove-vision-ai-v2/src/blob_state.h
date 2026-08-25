/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_state.h
 * @brief   The blob header format, and every decision a transfer makes (#92).
 *
 * Everything here is a pure function over values.  Not for tidiness: these are
 * the judgements a console cannot produce.  A slot holding a header whose body
 * CRC is wrong, a sender that closes the batch after fewer bytes than it
 * declared, a program that failed in the middle of a 1.7 MB payload -- each one
 * costs a flash cycle of a part whose endurance is not documented (#89) to
 * stage on hardware, and most cannot be staged at all.  test/test_blob_state.c
 * walks them here, and src/blob.c is left holding the I/O.
 *
 * THE HEADER IS TWO PROGRAM PAGES, AND THE MAGIC PAGE IS WRITTEN LAST AND
 * ALONE.  The first erase unit of a slot is its header sector; page 0 of that
 * sector holds the magic and nothing else, page 1 holds the body:
 *
 *     page 0   BLOB_MAGIC_SIZE bytes of magic, then erased flash
 *     page 1   ver | base | length | payload crc32 | name_len | name | body crc32
 *
 * So an interrupted transfer leaves a slot with no magic, which reads
 * INCOMPLETE and never VALID, and a body that failed to program cannot be
 * rescued by a magic that lands anyway.
 *
 * [!] THE TWO PAGES ARE WHY THIS DIFFERS FROM THE DONOR.  wio's blob puts the
 * magic and the body in ONE page and programs the page twice, resting on the
 * W25Q128JV datasheet's partial-page programming.  The die actually fitted here
 * is a Zbit ZB25LQ128C (#89) and there is no datasheet for it, so this format
 * does not depend on any second program touching a page that has already been
 * written: every page here is programmed once.
 *
 * [!] THE BODY CARRIES ITS OWN SLOT'S BASE ADDRESS.  The donor learned this the
 * expensive way (owhinata/wio-lite-ai#55): when its slot geometry changed, old
 * headers still decoded and still verified -- under a different slot number.  A
 * header that names the base it was read from cannot be re-read as another
 * slot's, and #49 Step 4 re-carves this region.
 *
 * [!] `blob list` MUST SAY WHAT EMPTY MEANS.  EMPTY is "these two pages are
 * erased", which is a statement about the HEADER only.  The payload beneath it
 * may hold anything -- and on this board it does: the factory SenseCraft
 * firmware left a FlashDB KVDB at 0x300000 and data at 0x400000 and 0x500000,
 * inside slots 0 and 1.  A slot whose header pages hold some of that reads
 * INVALID, which is the state that means "occupied by something this port
 * cannot read".
 *
 * THE PAYLOAD CRC IS OF THE STREAM THAT ARRIVED, never of a read-back.  Derived
 * from the flash it would make `blob verify` compare the flash with itself and
 * pass forever, however badly the write went.
 */
#ifndef BLOB_STATE_H
#define BLOB_STATE_H

#include <stdint.h>

#include "nor_span.h"   /* NOR_PROGRAM_PAGE -- the header's page granularity */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- format -------------------------------------------------------------- */

/** Bumped when a field changes meaning; the decoder refuses anything else. */
#define BLOB_FMT_VER         1u

/** Magic bytes at offset 0 of page 0.  ASCII, so a hexdump reads. */
#define BLOB_MAGIC_SIZE      8u

/** Longest stored name.  The name is the CLI's argument, not the sender's
 *  filename, so it is a key rather than a label -- see blob_choose_target(). */
#define BLOB_NAME_MAX        64u

/** Body field offsets, little-endian throughout.  The body CRC covers
 *  everything before it, which is why its offset is also the CRC's length. */
#define BLOB_BODY_OFF_VER      0u
#define BLOB_BODY_OFF_BASE     4u
#define BLOB_BODY_OFF_LENGTH   8u
#define BLOB_BODY_OFF_CRC     12u
#define BLOB_BODY_OFF_NAMELEN 16u
#define BLOB_BODY_OFF_NAME    20u
#define BLOB_BODY_OFF_BODYCRC (BLOB_BODY_OFF_NAME + BLOB_NAME_MAX)   /* 84 */
#define BLOB_BODY_SIZE        (BLOB_BODY_OFF_BODYCRC + 4u)           /* 88 */

/** What a stat reads from the top of a slot: the magic page and the body page. */
#define BLOB_HDR_SPAN        (2u * NOR_PROGRAM_PAGE)

/* ---- what a slot holds --------------------------------------------------- */

/**
 * The four states, and they are an enumeration of what CAN be decoded rather
 * than a "not broken" test:
 *
 *   EMPTY       both header pages are entirely erased
 *   VALID       the magic is there and the body decodes completely
 *   INCOMPLETE  the magic page is erased and the body decodes completely
 *   INVALID     anything else -- including flash this port did not write
 *
 * [!] "The magic is 0xFF" is NOT the test for INCOMPLETE.  A slot with an
 * erased magic page and an undecodable body is INVALID: calling it incomplete
 * would tell an operator that a transfer of theirs was interrupted when what is
 * actually there is somebody else's data.
 */
enum blob_slot_state {
	BLOB_EMPTY = 0,
	BLOB_VALID,
	BLOB_INCOMPLETE,
	BLOB_INVALID,
};

/** Short name for a state, for `blob list` and the host test. */
const char *blob_slot_state_name(enum blob_slot_state s);

/**
 * Why a header did not decode.  Reported so that INVALID is actionable -- "the
 * body CRC is wrong" and "this is not a header at all" call for different
 * things from an operator -- and fixed in ONE order so the answer does not
 * depend on how the decoder happens to be written:
 *
 *   1. the magic page, when it is neither erased nor the magic.  Foreign data
 *      fails every later test too, and naming the first one it fails would
 *      report a version error about a KVDB sector
 *   2. otherwise the first body field that fails, in the order they are stored
 */
enum blob_hdr_reject {
	BLOB_REJECT_NONE = 0,
	BLOB_REJECT_SHORT,     /**< caller passed fewer than BLOB_HDR_SPAN bytes */
	BLOB_REJECT_MAGIC,     /**< page 0 is neither erased nor the magic       */
	BLOB_REJECT_VER,       /**< a format this build does not know            */
	BLOB_REJECT_BASE,      /**< the body names a different slot              */
	BLOB_REJECT_LENGTH,    /**< zero, or more payload than the slot holds    */
	BLOB_REJECT_NAME,      /**< length or characters outside the rules       */
	BLOB_REJECT_BODY_CRC,  /**< the body did not survive the flash           */
};

/** Short name for a rejection. */
const char *blob_hdr_reject_name(enum blob_hdr_reject r);

/** A decoded header.  Fields other than @ref state are filled in only when the
 *  body decoded -- that is, for VALID and INCOMPLETE -- and zeroed otherwise. */
struct blob_info {
	enum blob_slot_state state;
	char     name[BLOB_NAME_MAX + 1u];   /**< NUL-terminated on the way out */
	uint32_t length;                     /**< payload bytes                 */
	uint32_t crc32;                      /**< CRC-32 of the payload as sent */
};

/* ---- names --------------------------------------------------------------- */

/** Why a name is or is not one this store will persist. */
enum blob_name_verdict {
	BLOB_NAME_OK = 0,
	BLOB_NAME_EMPTY,   /**< NULL or zero length         */
	BLOB_NAME_LONG,    /**< longer than BLOB_NAME_MAX   */
	BLOB_NAME_CHAR,    /**< a byte outside 0x21..0x7E   */
};

/** Short name for a name verdict (the console prints it back at the operator). */
const char *blob_name_verdict_name(enum blob_name_verdict v);

/**
 * @brief  May @p name be stored, and how long is it?
 *
 * Printable ASCII with no space: a space could not survive the command line
 * anyway (the parser splits on it) and `blob list` prints names in a column.
 * @p len_out receives the length on BLOB_NAME_OK; it is not touched otherwise.
 */
enum blob_name_verdict blob_name_check(const char *name, uint32_t *len_out);

/* ---- the header codec ---------------------------------------------------- */

/**
 * @brief  Decode the two header pages of the slot based at @p slot_base.
 *
 * @param hdr          BLOB_HDR_SPAN bytes read from the top of the slot
 * @param hdr_len      how many bytes @p hdr really holds
 * @param slot_base    the base the caller read them from
 * @param payload_max  bytes of payload the slot has room for
 * @param out          receives the state and, when it decoded, the fields
 * @param why          receives the rejection reason; BLOB_REJECT_NONE unless
 *                     the state is INVALID.  May be NULL
 *
 * Always answers -- there is no failure return, because "this slot holds
 * something I cannot read" is an answer a listing has to be able to print.
 */
enum blob_slot_state blob_hdr_decode(const uint8_t *hdr, uint32_t hdr_len,
                                     uint32_t slot_base, uint32_t payload_max,
                                     struct blob_info *out,
                                     enum blob_hdr_reject *why);

/**
 * @brief  Lay out the body page for @p info in @p body.
 *
 * @return BLOB_REJECT_NONE when @p body holds a body the decoder will accept,
 *         and the reason it refused otherwise -- the SAME vocabulary the
 *         decoder rejects with, deliberately: what this refuses to write and
 *         what that refuses to read are one set of rules, and the host test
 *         asserts the two agree rather than trusting that they do.
 *
 * Nothing may be programmed on a refusal.  The bytes after the name are zeroed
 * rather than left erased so that a body which lost power part-way through its
 * one program fails the body CRC instead of decoding as a shorter name.
 */
enum blob_hdr_reject blob_hdr_encode_body(uint8_t *body, uint32_t body_len,
                                          uint32_t slot_base,
                                          uint32_t payload_max,
                                          const struct blob_info *info);

/**
 * @brief  Lay out the magic page's leading bytes.
 *
 * Programmed alone, after the body has been programmed AND read back.  Written
 * with the body it would be one page program that could land the magic over a
 * half-formed body: a slot that looks valid and is not.
 *
 * @return BLOB_REJECT_NONE, or BLOB_REJECT_SHORT for a buffer too small to
 *         hold the magic -- which is a refusal to write anything, not a
 *         truncated magic that would read back as INVALID forever.
 */
enum blob_hdr_reject blob_hdr_encode_magic(uint8_t *magic, uint32_t magic_len);

/* ---- choosing where a write goes ----------------------------------------- */

/** What the caller found in one slot, as far as choosing a target goes. */
struct blob_slot_view {
	enum blob_slot_state state;
	/** Non-zero when this slot holds a VALID blob under the wanted name.
	 *  Set from a decoded header; a match on any other state is a caller
	 *  that contradicts itself and nothing is chosen. */
	uint8_t name_match;
};

/**
 * Where `blob write <name> [slot]` goes, or why it goes nowhere.
 *
 * [!] THE SLOT IS SETTLED BEFORE THE TRANSFER STARTS, and that is the whole
 * point of this function.  Choosing from the size the sender declares would put
 * the erase inside the protocol -- sink begin() runs BEFORE block 0 is ACKed,
 * and the ACK-then-erase alternative runs out the sender's 'C' retries -- so
 * the friction lives in the argument list instead, where an operator can see
 * it.
 */
enum blob_choice {
	BLOB_CHOICE_REUSE = 0,  /**< go: the slot that already holds this name  */
	BLOB_CHOICE_FRESH,      /**< go: the named slot, which holds no blob    */
	BLOB_CHOICE_NEED_SLOT,  /**< no: a new name has to say where it goes    */
	BLOB_CHOICE_DUPLICATE,  /**< no: that name lives in another slot        */
	BLOB_CHOICE_OCCUPIED,   /**< no: the named slot holds something else    */
	BLOB_CHOICE_BAD_MAP,    /**< no: there is no table to choose from       */
	BLOB_CHOICE_REFUSE,     /**< no: the request does not name a slot at all */
};

/** Short name for a choice, for the console and the host test. */
const char *blob_choice_name(enum blob_choice c);

/**
 * @brief  Pick the slot `blob write` will erase.
 *
 * @param v      one view per slot, in table order
 * @param count  how many
 * @param want   the slot the operator named, or negative for none
 * @param target receives the chosen index on REUSE / FRESH; untouched otherwise
 *
 * FRESH accepts EMPTY and INCOMPLETE and refuses VALID and INVALID.  An
 * INCOMPLETE slot has no magic, so nothing `blob list` calls a blob is lost;
 * an INVALID one holds bytes this port cannot read -- factory data, or a header
 * from a format it does not know -- and destroying those takes an explicit
 * `blob erase`.
 *
 * A name that is already in another slot is refused rather than moved, so that
 * no window exists in which two slots carry the same name: the operator erases
 * the old one first, and until they do, the blob they have is still there.
 */
enum blob_choice blob_choose_target(const struct blob_slot_view *v,
                                    unsigned count, int want,
                                    unsigned *target);

/* ---- the transfer's own state -------------------------------------------- */

/**
 * One `blob write`, from the erase to the header.
 *
 * [!] THE BOOKKEEPING IS HERE AND THE I/O IS NOT.  src/blob.c stages bytes and
 * programs them; what it may do next is decided by these functions, which is
 * what lets the host test drive a whole transfer -- including the ones that end
 * badly -- with no flash under it.
 */
enum blob_phase {
	BLOB_PHASE_IDLE = 0,  /**< nothing armed                                */
	BLOB_PHASE_ARMED,     /**< slot chosen and erased; no block 0 yet       */
	BLOB_PHASE_RECV,      /**< the sender declared a size; payload arriving */
	BLOB_PHASE_DONE,      /**< the batch closed with everything declared    */
	BLOB_PHASE_BROKEN,    /**< sticky: no more payload, and no header       */
};

struct blob_write {
	enum blob_phase phase;
	unsigned slot;      /**< index of the target, for the report            */
	uint32_t capacity;  /**< payload bytes the slot holds                   */
	uint32_t declared;  /**< what block 0 announced                         */
	uint32_t received;  /**< accepted so far                                */
	uint32_t crc;       /**< CRC-32 of what was accepted, accumulated       */
};

/** Why a step of the transfer is or is not allowed to happen. */
enum blob_wr_verdict {
	BLOB_WR_OK = 0,
	BLOB_WR_TOO_SMALL,   /**< the declared payload does not fit the slot */
	BLOB_WR_EMPTY_FILE,  /**< the sender declared nothing at all         */
	BLOB_WR_OVERRUN,     /**< more arrived than was declared             */
	BLOB_WR_SHORT,       /**< the batch closed with less than declared   */
	BLOB_WR_PHASE,       /**< this call does not belong here             */
	BLOB_WR_BROKEN,      /**< something already failed; nothing more     */
};

/** Short name for a transfer verdict. */
const char *blob_wr_verdict_name(enum blob_wr_verdict v);

/** Arm @p w for @p slot, whose payload area is @p capacity bytes.  Called after
 *  the erase reported NOR_WRITE_OK and never before it: programming into a slot
 *  whose erase was cut short reads back as the AND of the two, which the
 *  writer's read-back turns into a terminal fault (nor_write.h). */
void blob_write_arm(struct blob_write *w, unsigned slot, uint32_t capacity);

/** The sender declared @p declared bytes in block 0.  ARMED -> RECV. */
enum blob_wr_verdict blob_write_begin(struct blob_write *w, uint32_t declared);

/** @p len bytes arrived: account for them and continue the payload CRC.  The
 *  CRC is taken HERE, from the bytes as they arrive, and never from a
 *  read-back.  A zero length is the identity; a NULL buffer with a non-zero
 *  length is a caller that has lost track of its own staging, and is reported
 *  as BLOB_WR_PHASE with the transfer broken rather than skipped. */
enum blob_wr_verdict blob_write_data(struct blob_write *w, const void *buf,
                                     uint32_t len);

/** The sender closed the batch.  RECV -> DONE, or SHORT (and BROKEN) when
 *  fewer bytes arrived than were declared: a payload whose length is not the
 *  one in the header must not be given a magic. */
enum blob_wr_verdict blob_write_close(struct blob_write *w);

/** Something failed -- a program, the transfer, a cancel.  Sticky, idempotent,
 *  and the only way out is blob_write_reset(). */
void blob_write_break(struct blob_write *w);

/** May the header be written?  BLOB_WR_OK only from DONE. */
enum blob_wr_verdict blob_write_commit_check(const struct blob_write *w);

/** Back to IDLE, whatever happened.  Every exit from a `blob write` runs this. */
void blob_write_reset(struct blob_write *w);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_STATE_H */
