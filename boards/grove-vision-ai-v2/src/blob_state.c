/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_state.c
 * @brief   The header codec and the transfer's decisions, as pure functions (#92).
 *
 * No hardware, no ThreadX, no shell -- test/test_blob_state.c compiles this
 * exact file on the host, against the same nor_span.h the firmware uses.  The
 * one thing it links is svc/crc32.c, which is equally freestanding.
 *
 * See blob_state.h for the format and for why each decision lives here rather
 * than in src/blob.c.
 */
#include "blob_state.h"

#include <string.h>

#include "crc32.h"

/* ASCII, so `nor scan` and a hexdump both read it. */
static const uint8_t magic_bytes[BLOB_MAGIC_SIZE] = {
	'B', 'L', 'O', 'B', 'S', 'T', 'O', 'R'
};

/* ---- little-endian accessors --------------------------------------------- */

/* Byte at a time in both directions: the header is a flash format, so it may
 * not inherit the compiler's idea of alignment or byte order. */
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int all_erased(const uint8_t *p, uint32_t n)
{
	uint32_t i;

	for (i = 0u; i < n; i++) {
		if (p[i] != 0xFFu)
			return 0;
	}
	return 1;
}

/* ---- names --------------------------------------------------------------- */

const char *blob_name_verdict_name(enum blob_name_verdict v)
{
	switch (v) {
	case BLOB_NAME_OK:    return "ok";
	case BLOB_NAME_EMPTY: return "name is empty";
	case BLOB_NAME_LONG:  return "name is too long";
	case BLOB_NAME_CHAR:  return "name has a character that is not printable ASCII";
	default:              break;
	}
	return "?";
}

enum blob_name_verdict blob_name_check(const char *name, uint32_t *len_out)
{
	uint32_t n = 0u;

	if (name == NULL || name[0] == '\0')
		return BLOB_NAME_EMPTY;
	while (name[n] != '\0') {
		uint8_t c = (uint8_t)name[n];

		/* 0x21..0x7E: printable and not a space.  A space could not
		 * reach here through the command line anyway, and `blob list`
		 * prints these in a column. */
		if (c < 0x21u || c > 0x7Eu)
			return BLOB_NAME_CHAR;
		n++;
		if (n > BLOB_NAME_MAX)
			return BLOB_NAME_LONG;
	}
	if (len_out != NULL)
		*len_out = n;
	return BLOB_NAME_OK;
}

/* ---- the header codec ---------------------------------------------------- */

const char *blob_slot_state_name(enum blob_slot_state s)
{
	switch (s) {
	case BLOB_EMPTY:      return "empty";
	case BLOB_VALID:      return "valid";
	case BLOB_INCOMPLETE: return "incomplete";
	case BLOB_INVALID:    return "invalid";
	default:              break;
	}
	return "?";
}

const char *blob_hdr_reject_name(enum blob_hdr_reject r)
{
	switch (r) {
	case BLOB_REJECT_NONE:      return "ok";
	case BLOB_REJECT_SHORT:     return "header buffer is too small";
	case BLOB_REJECT_MAGIC:     return "magic page is neither erased nor a header";
	case BLOB_REJECT_BODY_TAIL: return "body page holds bytes after the header";
	case BLOB_REJECT_VER:       return "format version is not this one";
	case BLOB_REJECT_BASE:      return "header names a different slot";
	case BLOB_REJECT_LENGTH:    return "payload length does not fit the slot";
	case BLOB_REJECT_NAME:      return "stored name breaks the name rules";
	case BLOB_REJECT_BODY_CRC:  return "body checksum does not match";
	default:                    break;
	}
	return "?";
}

/* Decode the body page into @p out, in FIELD ORDER, and say what stopped it.
 * Field order is the point: the reason a listing prints has to be a property of
 * the header rather than of how this function is arranged. */
static enum blob_hdr_reject body_decode(const uint8_t *body, uint32_t slot_base,
                                        uint32_t payload_max,
                                        struct blob_info *out)
{
	uint32_t nlen, length, i;

	if (get32(body + BLOB_BODY_OFF_VER) != BLOB_FMT_VER)
		return BLOB_REJECT_VER;
	if (get32(body + BLOB_BODY_OFF_BASE) != slot_base)
		return BLOB_REJECT_BASE;

	length = get32(body + BLOB_BODY_OFF_LENGTH);
	if (length == 0u || length > payload_max)
		return BLOB_REJECT_LENGTH;

	nlen = get32(body + BLOB_BODY_OFF_NAMELEN);
	if (nlen == 0u || nlen > BLOB_NAME_MAX)
		return BLOB_REJECT_NAME;
	for (i = 0u; i < nlen; i++) {
		uint8_t c = body[BLOB_BODY_OFF_NAME + i];

		if (c < 0x21u || c > 0x7Eu)
			return BLOB_REJECT_NAME;
	}

	/* Last, over everything before it.  A body that lost power part-way
	 * through its single page program fails here rather than decoding as
	 * something shorter. */
	if (crc32_update(0u, body, BLOB_BODY_OFF_BODYCRC) !=
	    get32(body + BLOB_BODY_OFF_BODYCRC))
		return BLOB_REJECT_BODY_CRC;

	if (out != NULL) {
		memcpy(out->name, body + BLOB_BODY_OFF_NAME, nlen);
		out->name[nlen] = '\0';
		out->length = length;
		out->crc32  = get32(body + BLOB_BODY_OFF_CRC);
	}
	return BLOB_REJECT_NONE;
}

enum blob_slot_state blob_hdr_decode(const uint8_t *hdr, uint32_t hdr_len,
                                     uint32_t slot_base, uint32_t payload_max,
                                     struct blob_info *out,
                                     enum blob_hdr_reject *why)
{
	const uint8_t *body;
	enum blob_hdr_reject r;
	int magic_erased, magic_present;

	if (out != NULL)
		memset(out, 0, sizeof *out);
	if (why != NULL)
		*why = BLOB_REJECT_NONE;

	if (hdr == NULL || hdr_len < BLOB_HDR_SPAN) {
		if (why != NULL)
			*why = BLOB_REJECT_SHORT;
		if (out != NULL)
			out->state = BLOB_INVALID;
		return BLOB_INVALID;
	}
	body = hdr + NOR_PROGRAM_PAGE;

	magic_erased = all_erased(hdr, NOR_PROGRAM_PAGE);
	/* Present means the magic AND nothing else: this port programs eight
	 * bytes into an erased page and never touches the rest, so a page with
	 * anything further in it did not come from here. */
	magic_present = (memcmp(hdr, magic_bytes, BLOB_MAGIC_SIZE) == 0) &&
	                all_erased(hdr + BLOB_MAGIC_SIZE,
	                           NOR_PROGRAM_PAGE - BLOB_MAGIC_SIZE);

	if (!magic_erased && !magic_present) {
		if (why != NULL)
			*why = BLOB_REJECT_MAGIC;
		if (out != NULL)
			out->state = BLOB_INVALID;
		return BLOB_INVALID;
	}

	/* [!] The SAME provenance test on page 1.  This port programs
	 * BLOB_BODY_SIZE bytes into an erased page and never touches the rest,
	 * so a byte after the body did not come from here -- and without this,
	 * a header with one stray 0x00 at offset BLOB_BODY_SIZE decoded as
	 * VALID, which is a sequence no writer here can produce.  Erased
	 * everywhere still reaches the EMPTY answer below, since an erased tail
	 * passes this. */
	if (!all_erased(body + BLOB_BODY_SIZE,
	                NOR_PROGRAM_PAGE - BLOB_BODY_SIZE)) {
		if (why != NULL)
			*why = BLOB_REJECT_BODY_TAIL;
		if (out != NULL)
			out->state = BLOB_INVALID;
		return BLOB_INVALID;
	}

	r = body_decode(body, slot_base, payload_max, out);
	if (r == BLOB_REJECT_NONE) {
		enum blob_slot_state st = magic_present ? BLOB_VALID
		                                        : BLOB_INCOMPLETE;

		if (out != NULL)
			out->state = st;
		return st;
	}

	/* Erased everywhere is the one case where a body that does not decode
	 * is not a complaint. */
	if (magic_erased && all_erased(body, NOR_PROGRAM_PAGE)) {
		if (out != NULL) {
			memset(out, 0, sizeof *out);
			out->state = BLOB_EMPTY;
		}
		return BLOB_EMPTY;
	}

	if (why != NULL)
		*why = r;
	if (out != NULL) {
		memset(out, 0, sizeof *out);
		out->state = BLOB_INVALID;
	}
	return BLOB_INVALID;
}

enum blob_hdr_reject blob_hdr_encode_body(uint8_t *page, uint32_t page_len,
                                          uint32_t slot_base,
                                          uint32_t payload_max,
                                          const struct blob_info *info)
{
	uint8_t *body = page;
	uint32_t nlen = 0u;

	if (page == NULL || page_len < NOR_PROGRAM_PAGE || info == NULL)
		return BLOB_REJECT_SHORT;
	if (blob_name_check(info->name, &nlen) != BLOB_NAME_OK)
		return BLOB_REJECT_NAME;
	if (info->length == 0u || info->length > payload_max)
		return BLOB_REJECT_LENGTH;

	/* Erased first, body over it: whatever length the caller programs, from
	 * the body to the whole page, the flash ends up the same.  0xFF over
	 * erased flash clears no bits. */
	memset(page, 0xFF, NOR_PROGRAM_PAGE);
	memset(body, 0, BLOB_BODY_SIZE);
	put32(body + BLOB_BODY_OFF_VER, BLOB_FMT_VER);
	put32(body + BLOB_BODY_OFF_BASE, slot_base);
	put32(body + BLOB_BODY_OFF_LENGTH, info->length);
	put32(body + BLOB_BODY_OFF_CRC, info->crc32);
	put32(body + BLOB_BODY_OFF_NAMELEN, nlen);
	memcpy(body + BLOB_BODY_OFF_NAME, info->name, nlen);
	put32(body + BLOB_BODY_OFF_BODYCRC,
	      crc32_update(0u, body, BLOB_BODY_OFF_BODYCRC));
	return BLOB_REJECT_NONE;
}

enum blob_hdr_reject blob_hdr_encode_magic(uint8_t *page, uint32_t page_len)
{
	if (page == NULL || page_len < NOR_PROGRAM_PAGE)
		return BLOB_REJECT_SHORT;
	memset(page, 0xFF, NOR_PROGRAM_PAGE);
	memcpy(page, magic_bytes, BLOB_MAGIC_SIZE);
	return BLOB_REJECT_NONE;
}

/* ---- choosing where a write goes ----------------------------------------- */

const char *blob_choice_name(enum blob_choice c)
{
	switch (c) {
	case BLOB_CHOICE_REUSE:     return "reuse";
	case BLOB_CHOICE_FRESH:     return "fresh";
	case BLOB_CHOICE_NEED_SLOT: return "a new name needs a slot";
	case BLOB_CHOICE_DUPLICATE: return "that name is in another slot";
	case BLOB_CHOICE_OCCUPIED:  return "that slot holds something else";
	case BLOB_CHOICE_BAD_MAP:   return "no slot table";
	case BLOB_CHOICE_REFUSE:    return "no such slot";
	default:                    break;
	}
	return "?";
}

enum blob_choice blob_choose_target(const struct blob_slot_view *v,
                                    unsigned count, int want,
                                    unsigned *target)
{
	unsigned i, matches = 0u, match_at = 0u;

	if (v == NULL || target == NULL)
		return BLOB_CHOICE_REFUSE;
	if (count == 0u)
		return BLOB_CHOICE_BAD_MAP;

	for (i = 0u; i < count; i++) {
		if (!v[i].name_match)
			continue;
		/* A match is a decoded VALID header by construction.  A caller
		 * that says otherwise has contradicted itself, and choosing
		 * from a view that does not hold together is how a write ends
		 * up somewhere nobody named. */
		if (v[i].state != BLOB_VALID)
			return BLOB_CHOICE_REFUSE;
		matches++;
		match_at = i;
	}
	/* Two slots under one name is a state this port will not add to.  It
	 * cannot be produced from here -- the refusal below is what stops it --
	 * but flash outlives firmware. */
	if (matches > 1u)
		return BLOB_CHOICE_DUPLICATE;

	if (want < 0) {
		if (matches == 1u) {
			*target = match_at;
			return BLOB_CHOICE_REUSE;
		}
		return BLOB_CHOICE_NEED_SLOT;
	}

	if ((unsigned)want >= count)
		return BLOB_CHOICE_REFUSE;
	if (matches == 1u) {
		if (match_at != (unsigned)want)
			return BLOB_CHOICE_DUPLICATE;
		*target = match_at;
		return BLOB_CHOICE_REUSE;
	}
	/* Enumerate what may be overwritten rather than what may not: EMPTY has
	 * no header, INCOMPLETE has one this port wrote and never finished.
	 * VALID and INVALID both hold something an operator has to destroy on
	 * purpose. */
	if (v[want].state == BLOB_EMPTY || v[want].state == BLOB_INCOMPLETE) {
		*target = (unsigned)want;
		return BLOB_CHOICE_FRESH;
	}
	return BLOB_CHOICE_OCCUPIED;
}

/* ---- finding one to read ------------------------------------------------- */

const char *blob_lookup_name(enum blob_lookup l)
{
	switch (l) {
	case BLOB_LOOKUP_FOUND:     return "found";
	case BLOB_LOOKUP_NONE:      return "no slot holds that name";
	case BLOB_LOOKUP_DUPLICATE: return "more than one slot holds that name";
	case BLOB_LOOKUP_REFUSE:    return "the slot scan does not hold together";
	default:                    break;
	}
	return "?";
}

enum blob_lookup blob_resolve_name(const struct blob_slot_view *v,
                                   unsigned count, unsigned *found)
{
	unsigned i, matches = 0u, match_at = 0u;

	if (v == NULL || found == NULL)
		return BLOB_LOOKUP_REFUSE;
	if (count == 0u)
		return BLOB_LOOKUP_REFUSE;

	for (i = 0u; i < count; i++) {
		if (!v[i].name_match)
			continue;
		/* Same contradiction blob_choose_target() refuses on, and refused
		 * here for the reader's version of the reason: a name_match on a
		 * slot the caller also calls EMPTY is a scan that disagrees with
		 * itself, and resolving from one would point the interpreter at
		 * flash nobody looked at. */
		if (v[i].state != BLOB_VALID)
			return BLOB_LOOKUP_REFUSE;
		matches++;
		match_at = i;
	}
	if (matches > 1u)
		return BLOB_LOOKUP_DUPLICATE;
	if (matches == 0u)
		return BLOB_LOOKUP_NONE;

	*found = match_at;
	return BLOB_LOOKUP_FOUND;
}

/* ---- the transfer's own state -------------------------------------------- */

const char *blob_wr_verdict_name(enum blob_wr_verdict v)
{
	switch (v) {
	case BLOB_WR_OK:         return "ok";
	case BLOB_WR_TOO_SMALL:  return "the file does not fit the slot";
	case BLOB_WR_EMPTY_FILE: return "the sender declared an empty file";
	case BLOB_WR_OVERRUN:    return "more arrived than was declared";
	case BLOB_WR_SHORT:      return "fewer arrived than was declared";
	case BLOB_WR_PHASE:      return "not at this point in a transfer";
	case BLOB_WR_BROKEN:     return "the transfer already failed";
	default:                 break;
	}
	return "?";
}

void blob_write_arm(struct blob_write *w, unsigned slot, uint32_t capacity)
{
	if (w == NULL)
		return;
	memset(w, 0, sizeof *w);
	w->phase    = BLOB_PHASE_ARMED;
	w->slot     = slot;
	w->capacity = capacity;
}

enum blob_wr_verdict blob_write_begin(struct blob_write *w, uint32_t declared)
{
	if (w == NULL)
		return BLOB_WR_PHASE;
	if (w->phase == BLOB_PHASE_BROKEN)
		return BLOB_WR_BROKEN;
	if (w->phase != BLOB_PHASE_ARMED)
		return BLOB_WR_PHASE;
	/* Both refusals happen before a byte of payload is accepted, and both
	 * break the transfer: the sender is told to give up rather than left
	 * streaming into a slot that will never get a header. */
	if (declared == 0u) {
		w->phase = BLOB_PHASE_BROKEN;
		return BLOB_WR_EMPTY_FILE;
	}
	if (declared > w->capacity) {
		w->phase = BLOB_PHASE_BROKEN;
		return BLOB_WR_TOO_SMALL;
	}
	w->declared = declared;
	w->phase    = BLOB_PHASE_RECV;
	return BLOB_WR_OK;
}

enum blob_wr_verdict blob_write_data(struct blob_write *w, const void *buf,
                                     uint32_t len)
{
	if (w == NULL)
		return BLOB_WR_PHASE;
	if (w->phase == BLOB_PHASE_BROKEN)
		return BLOB_WR_BROKEN;
	if (w->phase != BLOB_PHASE_RECV)
		return BLOB_WR_PHASE;
	if (len == 0u)
		return BLOB_WR_OK;
	if (buf == NULL) {
		w->phase = BLOB_PHASE_BROKEN;
		return BLOB_WR_PHASE;
	}
	/* Subtraction, so a length near 2^32 cannot wrap into looking like it
	 * fits.  An overrun is the sender contradicting its own block 0, which
	 * leaves nothing here worth committing. */
	if (len > w->declared - w->received) {
		w->phase = BLOB_PHASE_BROKEN;
		return BLOB_WR_OVERRUN;
	}
	w->crc = crc32_update(w->crc, buf, len);
	w->received += len;
	return BLOB_WR_OK;
}

enum blob_wr_verdict blob_write_close(struct blob_write *w)
{
	if (w == NULL)
		return BLOB_WR_PHASE;
	if (w->phase == BLOB_PHASE_BROKEN)
		return BLOB_WR_BROKEN;
	if (w->phase != BLOB_PHASE_RECV)
		return BLOB_WR_PHASE;
	if (w->received != w->declared) {
		w->phase = BLOB_PHASE_BROKEN;
		return BLOB_WR_SHORT;
	}
	w->phase = BLOB_PHASE_DONE;
	return BLOB_WR_OK;
}

void blob_write_break(struct blob_write *w)
{
	if (w == NULL)
		return;
	w->phase = BLOB_PHASE_BROKEN;
}

enum blob_wr_verdict blob_write_commit_check(const struct blob_write *w)
{
	if (w == NULL)
		return BLOB_WR_PHASE;
	if (w->phase == BLOB_PHASE_BROKEN)
		return BLOB_WR_BROKEN;
	if (w->phase != BLOB_PHASE_DONE)
		return BLOB_WR_PHASE;
	return BLOB_WR_OK;
}

void blob_write_reset(struct blob_write *w)
{
	if (w == NULL)
		return;
	memset(w, 0, sizeof *w);
	w->phase = BLOB_PHASE_IDLE;
}
