/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the blob header codec and the transfer's decisions (#92,
 * src/blob_state.c).
 *
 * WHY THIS EXISTS.  Every case below is one the console cannot stage.  A body
 * whose CRC is wrong, a header that names a slot it is not in, a sender that
 * closes the batch after fewer bytes than block 0 declared, a program that
 * failed in the middle of a 1.7 MB payload -- producing any of them on the
 * board means spending flash cycles of a part whose endurance is not documented
 * (#89), and most of them cannot be produced at all without a co-operating PC
 * that misbehaves on demand.
 *
 * [!] AND WHAT THEY DECIDE IS DESTRUCTIVE.  A header that decodes when it
 * should not is a `blob write` aimed at the wrong slot, or an `nn open` handed
 * a payload that is not the model it names.  The state a slot is in is the
 * whole basis for both.
 */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "blob_state.h"
#include "crc32.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* Slot 0 of the shipped table: 2 MB at 0x200000, one 4 KB unit of header. */
#define BASE   0x00200000u
#define CAP    (0x00200000u - 0x1000u)

static uint8_t hdr[BLOB_HDR_SPAN];
#define BODY  (hdr + NOR_PROGRAM_PAGE)

static void hdr_erase(void)
{
	memset(hdr, 0xFF, sizeof hdr);
}

/* Lay a valid body down, the way the writer will. */
static enum blob_hdr_reject put_body(uint32_t base, uint32_t cap,
                                     const char *name, uint32_t length,
                                     uint32_t crc)
{
	struct blob_info info;

	memset(&info, 0, sizeof info);
	memcpy(info.name, name, strlen(name));
	info.length = length;
	info.crc32  = crc;
	return blob_hdr_encode_body(BODY, NOR_PROGRAM_PAGE, base, cap, &info);
}

static void put_magic(void)
{
	CHECK(blob_hdr_encode_magic(hdr, NOR_PROGRAM_PAGE) == BLOB_REJECT_NONE,
	      "the magic would not encode");
}

static enum blob_slot_state decode(enum blob_hdr_reject *why,
                                   struct blob_info *out)
{
	return blob_hdr_decode(hdr, sizeof hdr, BASE, CAP, out, why);
}

/* ---- names --------------------------------------------------------------- */

static void t_names(void)
{
	char longest[BLOB_NAME_MAX + 2u];
	uint32_t n = 0u;

	CHECK(blob_name_check("a", &n) == BLOB_NAME_OK && n == 1u,
	      "a one-character name was refused");
	CHECK(blob_name_check("model.tflite", &n) == BLOB_NAME_OK && n == 12u,
	      "a plain name was refused");
	CHECK(blob_name_check(NULL, &n) == BLOB_NAME_EMPTY,
	      "a NULL name was accepted");
	CHECK(blob_name_check("", &n) == BLOB_NAME_EMPTY,
	      "an empty name was accepted");

	memset(longest, 'x', sizeof longest);
	longest[BLOB_NAME_MAX] = '\0';
	CHECK(blob_name_check(longest, &n) == BLOB_NAME_OK && n == BLOB_NAME_MAX,
	      "the longest legal name was refused");
	longest[BLOB_NAME_MAX] = 'x';
	longest[BLOB_NAME_MAX + 1u] = '\0';
	CHECK(blob_name_check(longest, &n) == BLOB_NAME_LONG,
	      "a name one over the limit was accepted");

	/* A space would be two arguments on the command line and two columns in
	 * `blob list`; control characters would be neither. */
	CHECK(blob_name_check("two words", &n) == BLOB_NAME_CHAR,
	      "a name with a space was accepted");
	CHECK(blob_name_check("tab\there", &n) == BLOB_NAME_CHAR,
	      "a name with a tab was accepted");
	CHECK(blob_name_check("del\x7f", &n) == BLOB_NAME_CHAR,
	      "a name with DEL was accepted");
	CHECK(blob_name_check("high\xc3\xa9", &n) == BLOB_NAME_CHAR,
	      "a name with a byte over 0x7E was accepted");

	/* len_out is not touched on a refusal: a caller that ignored the
	 * verdict would otherwise get a length for a name it may not store. */
	n = 0xDEADu;
	CHECK(blob_name_check("", &n) == BLOB_NAME_EMPTY && n == 0xDEADu,
	      "a refused name still reported a length");
	CHECK(blob_name_check("ok", NULL) == BLOB_NAME_OK,
	      "a NULL length pointer was refused");
}

/* ---- the codec ----------------------------------------------------------- */

static void t_roundtrip(void)
{
	struct blob_info got;
	enum blob_hdr_reject why;
	enum blob_slot_state st;

	hdr_erase();
	CHECK(put_body(BASE, CAP, "model.tflite", 1704672u, 0xCBF43926u) ==
	      BLOB_REJECT_NONE, "a plain body would not encode");

	/* Body first, magic second: between the two programs the slot reads
	 * INCOMPLETE, which is the state an interrupted transfer leaves. */
	st = decode(&why, &got);
	CHECK(st == BLOB_INCOMPLETE && why == BLOB_REJECT_NONE,
	      "a body without a magic is %s (%s)", blob_slot_state_name(st),
	      blob_hdr_reject_name(why));
	CHECK(got.state == BLOB_INCOMPLETE && got.length == 1704672u &&
	      got.crc32 == 0xCBF43926u && strcmp(got.name, "model.tflite") == 0,
	      "the fields did not survive the round trip");

	put_magic();
	st = decode(&why, &got);
	CHECK(st == BLOB_VALID && why == BLOB_REJECT_NONE,
	      "a complete header is %s (%s)", blob_slot_state_name(st),
	      blob_hdr_reject_name(why));
	CHECK(got.length == 1704672u && got.crc32 == 0xCBF43926u &&
	      strcmp(got.name, "model.tflite") == 0,
	      "the fields changed when the magic arrived");

	/* The longest name, and a one-byte payload: both edges of the format. */
	hdr_erase();
	CHECK(put_body(BASE, CAP,
	               "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
	               1u, 0u) == BLOB_REJECT_NONE,
	      "the longest name would not encode");
	put_magic();
	st = decode(&why, &got);
	CHECK(st == BLOB_VALID && strlen(got.name) == BLOB_NAME_MAX &&
	      got.length == 1u,
	      "the longest name did not survive: %s (%s)",
	      blob_slot_state_name(st), blob_hdr_reject_name(why));

	/* A payload exactly filling the slot is legal; one byte more is not
	 * decodable in this slot, which is how a header read from the wrong
	 * geometry gets caught. */
	hdr_erase();
	CHECK(put_body(BASE, CAP, "full", CAP, 0u) == BLOB_REJECT_NONE,
	      "a payload that exactly fills the slot would not encode");
	put_magic();
	CHECK(decode(&why, &got) == BLOB_VALID, "a full slot did not decode");
	CHECK(blob_hdr_decode(hdr, sizeof hdr, BASE, CAP - 1u, &got, &why) ==
	      BLOB_INVALID && why == BLOB_REJECT_LENGTH,
	      "a payload past the slot's end decoded: %s",
	      blob_hdr_reject_name(why));
}

static void t_empty_and_foreign(void)
{
	struct blob_info got;
	enum blob_hdr_reject why;
	enum blob_slot_state st;

	hdr_erase();
	st = decode(&why, &got);
	CHECK(st == BLOB_EMPTY && why == BLOB_REJECT_NONE,
	      "two erased pages are %s", blob_slot_state_name(st));
	CHECK(got.name[0] == '\0' && got.length == 0u && got.crc32 == 0u,
	      "an empty slot came back with fields set");

	/* [!] EMPTY is a statement about the header pages only.  This is what
	 * `blob list` has to say out loud: the payload underneath may still
	 * hold the factory data this board ships with. */

	/* The factory's own bytes.  Whatever they are, they are not a header,
	 * and the reason has to be the magic rather than a version complaint
	 * about a FlashDB sector. */
	memset(hdr, 0x55, sizeof hdr);
	st = decode(&why, &got);
	CHECK(st == BLOB_INVALID && why == BLOB_REJECT_MAGIC,
	      "foreign data is %s (%s)", blob_slot_state_name(st),
	      blob_hdr_reject_name(why));

	/* An erased magic page over foreign body bytes is INVALID and NOT
	 * incomplete: "the magic is 0xFF" is not the test.  Calling it
	 * incomplete would tell an operator a transfer of theirs was cut short
	 * when what is there is somebody else's. */
	hdr_erase();
	memset(BODY, 0x5A, NOR_PROGRAM_PAGE);
	st = decode(&why, &got);
	CHECK(st == BLOB_INVALID && why == BLOB_REJECT_VER,
	      "an erased magic over foreign bytes is %s (%s)",
	      blob_slot_state_name(st), blob_hdr_reject_name(why));

	/* A magic over an erased body: the transfer never wrote a body, so
	 * there is nothing to be valid. */
	hdr_erase();
	put_magic();
	st = decode(&why, &got);
	CHECK(st == BLOB_INVALID && why == BLOB_REJECT_VER,
	      "a magic over an erased body is %s (%s)",
	      blob_slot_state_name(st), blob_hdr_reject_name(why));
}

static void t_magic_page(void)
{
	struct blob_info got;
	enum blob_hdr_reject why;

	hdr_erase();
	(void)put_body(BASE, CAP, "m", 16u, 0u);
	put_magic();
	CHECK(decode(&why, &got) == BLOB_VALID, "the baseline did not decode");

	/* One byte of the magic wrong. */
	hdr[BLOB_MAGIC_SIZE - 1u] ^= 0x01u;
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_MAGIC,
	      "a damaged magic decoded as %s", blob_hdr_reject_name(why));

	/* [!] The magic page is the magic AND NOTHING ELSE.  This port programs
	 * eight bytes into an erased page and never touches the rest, so a page
	 * with anything further in it did not come from here -- and treating it
	 * as valid would accept a header somebody else's data happened to sit
	 * on top of. */
	put_magic();
	hdr[NOR_PROGRAM_PAGE - 1u] = 0x00u;
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_MAGIC,
	      "a magic page with a stray byte decoded as %s",
	      blob_hdr_reject_name(why));
	hdr[NOR_PROGRAM_PAGE - 1u] = 0xFFu;
	CHECK(decode(&why, &got) == BLOB_VALID,
	      "clearing the stray byte did not restore the header");
}

static void t_body_rejections(void)
{
	struct blob_info got;
	enum blob_hdr_reject why;
	unsigned i;

	/* Each field, in the order they are stored.  A later field is not
	 * repaired first: the version test below leaves the body CRC wrong too,
	 * and the point is that the FIRST reason is reported, so the answer does
	 * not depend on how the decoder is arranged. */
	hdr_erase();
	(void)put_body(BASE, CAP, "m", 16u, 0u);
	put_magic();
	BODY[BLOB_BODY_OFF_VER] = (uint8_t)(BLOB_FMT_VER + 1u);
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_VER,
	      "a future format version decoded as %s", blob_hdr_reject_name(why));

	/* A header carrying another slot's base -- what the donor's geometry
	 * change produced, where an old header decoded under a new slot number
	 * (owhinata/wio-lite-ai#55). */
	hdr_erase();
	(void)put_body(BASE + 0x200000u, CAP, "m", 16u, 0u);
	put_magic();
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_BASE,
	      "a header from another slot decoded as %s",
	      blob_hdr_reject_name(why));

	/* Length: zero and past the end.  Both are refused by the encoder as
	 * well -- same vocabulary, deliberately -- so these are laid down by
	 * hand through a larger capacity. */
	hdr_erase();
	CHECK(put_body(BASE, CAP, "m", 0u, 0u) == BLOB_REJECT_LENGTH,
	      "the encoder wrote a zero-length body");
	CHECK(put_body(BASE, CAP, "m", CAP + 1u, 0u) == BLOB_REJECT_LENGTH,
	      "the encoder wrote a body that does not fit");

	/* Name length, damaged in place. */
	hdr_erase();
	(void)put_body(BASE, CAP, "m", 16u, 0u);
	put_magic();
	BODY[BLOB_BODY_OFF_NAMELEN] = 0u;
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_NAME,
	      "a zero-length stored name decoded as %s",
	      blob_hdr_reject_name(why));
	BODY[BLOB_BODY_OFF_NAMELEN] = (uint8_t)(BLOB_NAME_MAX + 1u);
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_NAME,
	      "an over-long stored name decoded as %s",
	      blob_hdr_reject_name(why));

	/* A stored name with a byte the name rules refuse.  The rules are one
	 * set: what blob_name_check() will not accept from an operator is what
	 * the decoder will not accept from flash. */
	hdr_erase();
	(void)put_body(BASE, CAP, "abc", 16u, 0u);
	put_magic();
	BODY[BLOB_BODY_OFF_NAME + 1u] = 0x00u;
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_NAME,
	      "a NUL inside the stored name decoded as %s",
	      blob_hdr_reject_name(why));

	/* The body CRC, one bit at a time over every field it covers.  This is
	 * the test that a body which lost power part-way through its single
	 * page program cannot decode as something shorter. */
	for (i = 0u; i < BLOB_BODY_OFF_BODYCRC; i++) {
		hdr_erase();
		(void)put_body(BASE, CAP, "abc", 16u, 0x12345678u);
		put_magic();
		BODY[i] ^= 0x01u;
		if (decode(&why, &got) == BLOB_VALID) {
			CHECK(0, "flipping body byte %u still decoded", i);
			break;
		}
	}
	hdr_erase();
	(void)put_body(BASE, CAP, "abc", 16u, 0x12345678u);
	put_magic();
	BODY[BLOB_BODY_OFF_CRC] ^= 0x01u;   /* a field no other test covers */
	CHECK(decode(&why, &got) == BLOB_INVALID && why == BLOB_REJECT_BODY_CRC,
	      "a damaged payload CRC decoded as %s", blob_hdr_reject_name(why));
	BODY[BLOB_BODY_OFF_BODYCRC] ^= 0x01u;
	CHECK(decode(&why, &got) == BLOB_INVALID,
	      "a damaged body CRC field decoded");
}

static void t_codec_edges(void)
{
	struct blob_info info, got;
	enum blob_hdr_reject why;
	uint8_t small[4];

	hdr_erase();
	(void)put_body(BASE, CAP, "m", 16u, 0u);
	put_magic();

	/* A short read is a caller error and is reported as one rather than as
	 * a slot that holds nothing. */
	CHECK(blob_hdr_decode(hdr, BLOB_HDR_SPAN - 1u, BASE, CAP, &got, &why) ==
	      BLOB_INVALID && why == BLOB_REJECT_SHORT,
	      "a short header buffer decoded as %s", blob_hdr_reject_name(why));
	CHECK(blob_hdr_decode(NULL, BLOB_HDR_SPAN, BASE, CAP, &got, &why) ==
	      BLOB_INVALID && why == BLOB_REJECT_SHORT,
	      "a NULL header buffer decoded");
	/* Both out parameters are optional: `blob list` wants the fields, the
	 * write path only wants the state. */
	CHECK(blob_hdr_decode(hdr, sizeof hdr, BASE, CAP, NULL, NULL) ==
	      BLOB_VALID, "decoding without out parameters changed the answer");

	memset(&info, 0, sizeof info);
	memcpy(info.name, "m", 1u);
	info.length = 16u;
	CHECK(blob_hdr_encode_body(small, sizeof small, BASE, CAP, &info) ==
	      BLOB_REJECT_SHORT, "the encoder wrote into a buffer too small");
	CHECK(blob_hdr_encode_body(NULL, NOR_PROGRAM_PAGE, BASE, CAP, &info) ==
	      BLOB_REJECT_SHORT, "the encoder accepted a NULL buffer");
	CHECK(blob_hdr_encode_body(BODY, NOR_PROGRAM_PAGE, BASE, CAP, NULL) ==
	      BLOB_REJECT_SHORT, "the encoder accepted a NULL info");
	info.name[0] = '\0';
	CHECK(blob_hdr_encode_body(BODY, NOR_PROGRAM_PAGE, BASE, CAP, &info) ==
	      BLOB_REJECT_NAME, "the encoder wrote an empty name");
	memcpy(info.name, "two words", 10u);
	CHECK(blob_hdr_encode_body(BODY, NOR_PROGRAM_PAGE, BASE, CAP, &info) ==
	      BLOB_REJECT_NAME, "the encoder wrote a name with a space");
	/* [!] Not terminated at all -- the field is exactly BLOB_NAME_MAX + 1
	 * bytes, so the name rules have to stop on their own rather than on a
	 * NUL that is not there.  They read index BLOB_NAME_MAX and no
	 * further. */
	memset(info.name, 'x', sizeof info.name);
	CHECK(blob_hdr_encode_body(BODY, NOR_PROGRAM_PAGE, BASE, CAP, &info) ==
	      BLOB_REJECT_NAME, "an unterminated name was written");

	memset(small, 0xA5, sizeof small);
	CHECK(blob_hdr_encode_magic(small, sizeof small) == BLOB_REJECT_SHORT,
	      "the magic went into a buffer too small");
	CHECK(small[0] == 0xA5u, "a refused magic still wrote bytes");
	CHECK(blob_hdr_encode_magic(NULL, BLOB_MAGIC_SIZE) == BLOB_REJECT_SHORT,
	      "the magic went into a NULL buffer");
}

/* ---- choosing a slot ------------------------------------------------------ */

#define VIEWS 4u

static void views_init(struct blob_slot_view *v)
{
	unsigned i;

	for (i = 0u; i < VIEWS; i++) {
		v[i].state = BLOB_EMPTY;
		v[i].name_match = 0u;
	}
}

static void t_choose(void)
{
	struct blob_slot_view v[VIEWS];
	unsigned target;

	/* The name is already somewhere: no slot argument needed, and it goes
	 * back where it was. */
	views_init(v);
	v[2].state = BLOB_VALID;
	v[2].name_match = 1u;
	target = 0xEEu;
	CHECK(blob_choose_target(v, VIEWS, -1, &target) == BLOB_CHOICE_REUSE &&
	      target == 2u, "a known name did not go back to its slot");
	target = 0xEEu;
	CHECK(blob_choose_target(v, VIEWS, 2, &target) == BLOB_CHOICE_REUSE &&
	      target == 2u, "naming the slot a blob is already in was refused");

	/* Naming a DIFFERENT slot is refused rather than moved: two slots must
	 * never carry one name, and the blob the operator has stays where it
	 * is until they erase it. */
	target = 0xEEu;
	CHECK(blob_choose_target(v, VIEWS, 1, &target) ==
	      BLOB_CHOICE_DUPLICATE && target == 0xEEu,
	      "a duplicate name was accepted into another slot");

	/* A new name has to say where it goes -- which is what puts the erase
	 * outside the protocol. */
	views_init(v);
	CHECK(blob_choose_target(v, VIEWS, -1, &target) ==
	      BLOB_CHOICE_NEED_SLOT, "a new name chose a slot on its own");

	/* What may be overwritten: nothing there, or a header this port wrote
	 * and never finished. */
	views_init(v);
	target = 0xEEu;
	CHECK(blob_choose_target(v, VIEWS, 1, &target) == BLOB_CHOICE_FRESH &&
	      target == 1u, "an empty slot was refused");
	v[1].state = BLOB_INCOMPLETE;
	CHECK(blob_choose_target(v, VIEWS, 1, &target) == BLOB_CHOICE_FRESH,
	      "an interrupted transfer's slot was refused");

	/* What may not: a blob under another name, and bytes this port cannot
	 * read at all -- the factory data lands here. */
	v[1].state = BLOB_VALID;
	target = 0xEEu;
	CHECK(blob_choose_target(v, VIEWS, 1, &target) == BLOB_CHOICE_OCCUPIED &&
	      target == 0xEEu, "another blob was overwritten");
	v[1].state = BLOB_INVALID;
	CHECK(blob_choose_target(v, VIEWS, 1, &target) == BLOB_CHOICE_OCCUPIED,
	      "unreadable data was overwritten");

	/* Slots that do not exist, and a table that is not there. */
	views_init(v);
	CHECK(blob_choose_target(v, VIEWS, (int)VIEWS, &target) ==
	      BLOB_CHOICE_REFUSE, "a slot past the end of the table was chosen");
	CHECK(blob_choose_target(v, 0u, 0, &target) == BLOB_CHOICE_BAD_MAP,
	      "an empty table chose a slot");
	CHECK(blob_choose_target(NULL, VIEWS, 0, &target) ==
	      BLOB_CHOICE_REFUSE, "a NULL view chose a slot");
	CHECK(blob_choose_target(v, VIEWS, 0, NULL) == BLOB_CHOICE_REFUSE,
	      "a NULL target was accepted");

	/* One name in two slots cannot be produced from here -- the refusal
	 * above is what stops it -- but flash outlives firmware, so it is
	 * refused rather than resolved by picking one. */
	views_init(v);
	v[0].state = BLOB_VALID; v[0].name_match = 1u;
	v[3].state = BLOB_VALID; v[3].name_match = 1u;
	CHECK(blob_choose_target(v, VIEWS, -1, &target) ==
	      BLOB_CHOICE_DUPLICATE, "a name in two slots chose one of them");
	CHECK(blob_choose_target(v, VIEWS, 0, &target) ==
	      BLOB_CHOICE_DUPLICATE,
	      "naming one of two slots under the same name was accepted");

	/* A view that contradicts itself: a match on a slot that does not hold
	 * a valid header.  Choosing from it is how a write ends up somewhere
	 * nobody named. */
	views_init(v);
	v[1].state = BLOB_EMPTY;
	v[1].name_match = 1u;
	CHECK(blob_choose_target(v, VIEWS, -1, &target) == BLOB_CHOICE_REFUSE,
	      "a match on an empty slot was believed");
}

/* ---- one transfer -------------------------------------------------------- */

static void t_transfer_happy(void)
{
	static uint8_t payload[4096];
	struct blob_write w;
	uint32_t i, off;

	for (i = 0u; i < sizeof payload; i++)
		payload[i] = (uint8_t)(i * 7u + 1u);

	blob_write_arm(&w, 3u, CAP);
	CHECK(w.phase == BLOB_PHASE_ARMED && w.slot == 3u && w.received == 0u &&
	      w.crc == 0u, "arming did not start from nothing");

	CHECK(blob_write_begin(&w, sizeof payload) == BLOB_WR_OK &&
	      w.phase == BLOB_PHASE_RECV, "a legal declaration was refused");

	/* YMODEM-shaped: 1024 at a time with a short last block. */
	for (off = 0u; off < sizeof payload; off += 1000u) {
		uint32_t take = sizeof payload - off;

		if (take > 1000u)
			take = 1000u;
		CHECK(blob_write_data(&w, payload + off, take) == BLOB_WR_OK,
		      "a block was refused at offset %u", off);
	}
	CHECK(w.received == sizeof payload, "the byte count is wrong");

	/* The CRC is of the stream as it arrived, and it is the same number one
	 * call over the whole buffer gives -- which is what makes the value in
	 * the header comparable with the file on the PC. */
	CHECK(w.crc == crc32_update(0u, payload, sizeof payload),
	      "the accumulated CRC is not the one-shot value");

	CHECK(blob_write_close(&w) == BLOB_WR_OK && w.phase == BLOB_PHASE_DONE,
	      "a complete transfer did not close");
	CHECK(blob_write_commit_check(&w) == BLOB_WR_OK,
	      "a complete transfer may not write its header");

	/* A zero-length write is the identity, which a receiver handing the
	 * sink an empty block relies on. */
	blob_write_arm(&w, 0u, CAP);
	(void)blob_write_begin(&w, 8u);
	CHECK(blob_write_data(&w, NULL, 0u) == BLOB_WR_OK && w.received == 0u,
	      "an empty write moved the transfer");
}

static void t_transfer_refusals(void)
{
	struct blob_write w;
	static const uint8_t buf[16] = { 0 };

	/* Block 0 declaring nothing, or more than the slot holds.  Both are
	 * settled before a byte of payload is accepted, and both end the
	 * transfer: the sender is told to give up rather than left streaming
	 * into a slot that will never get a header. */
	blob_write_arm(&w, 0u, 100u);
	CHECK(blob_write_begin(&w, 0u) == BLOB_WR_EMPTY_FILE &&
	      w.phase == BLOB_PHASE_BROKEN, "an empty file was accepted");

	blob_write_arm(&w, 0u, 100u);
	CHECK(blob_write_begin(&w, 101u) == BLOB_WR_TOO_SMALL &&
	      w.phase == BLOB_PHASE_BROKEN, "an oversized file was accepted");
	blob_write_arm(&w, 0u, 100u);
	CHECK(blob_write_begin(&w, 100u) == BLOB_WR_OK,
	      "a file exactly filling the slot was refused");

	/* More than was declared: the sender contradicting its own block 0. */
	blob_write_arm(&w, 0u, 100u);
	(void)blob_write_begin(&w, 16u);
	CHECK(blob_write_data(&w, buf, 8u) == BLOB_WR_OK, "8 bytes were refused");
	CHECK(blob_write_data(&w, buf, 9u) == BLOB_WR_OVERRUN &&
	      w.phase == BLOB_PHASE_BROKEN, "an overrun was accepted");
	/* And it stays broken: every later step refuses, so no header can be
	 * written over a payload nobody can describe. */
	CHECK(blob_write_data(&w, buf, 1u) == BLOB_WR_BROKEN,
	      "data was accepted after an overrun");
	CHECK(blob_write_close(&w) == BLOB_WR_BROKEN,
	      "a broken transfer closed");
	CHECK(blob_write_commit_check(&w) == BLOB_WR_BROKEN,
	      "a broken transfer may write its header");

	/* A length near 2^32 must not wrap into looking like it fits. */
	blob_write_arm(&w, 0u, 100u);
	(void)blob_write_begin(&w, 16u);
	(void)blob_write_data(&w, buf, 8u);
	CHECK(blob_write_data(&w, buf, 0xFFFFFFFFu) == BLOB_WR_OVERRUN,
	      "a length near 2^32 wrapped into fitting");

	/* Fewer than declared: the batch closed cleanly but the file was
	 * truncated on the way, and a payload whose length is not the one in
	 * the header must not be given a magic. */
	blob_write_arm(&w, 0u, 100u);
	(void)blob_write_begin(&w, 16u);
	(void)blob_write_data(&w, buf, 15u);
	CHECK(blob_write_close(&w) == BLOB_WR_SHORT &&
	      w.phase == BLOB_PHASE_BROKEN, "a truncated transfer closed");
	CHECK(blob_write_commit_check(&w) == BLOB_WR_BROKEN,
	      "a truncated transfer may write its header");

	/* A NULL buffer with bytes to copy is a caller that has lost its own
	 * staging; it breaks the transfer rather than being skipped. */
	blob_write_arm(&w, 0u, 100u);
	(void)blob_write_begin(&w, 16u);
	CHECK(blob_write_data(&w, NULL, 4u) == BLOB_WR_PHASE &&
	      w.phase == BLOB_PHASE_BROKEN, "a NULL payload was skipped");
}

static void t_transfer_phases(void)
{
	struct blob_write w;
	static const uint8_t buf[16] = { 0 };

	/* Nothing may happen before the slot is armed -- which happens only
	 * after the erase reported OK. */
	memset(&w, 0, sizeof w);
	CHECK(w.phase == BLOB_PHASE_IDLE, "a zeroed transfer is not idle");
	CHECK(blob_write_begin(&w, 16u) == BLOB_WR_PHASE, "IDLE accepted block 0");
	CHECK(blob_write_data(&w, buf, 4u) == BLOB_WR_PHASE, "IDLE accepted data");
	CHECK(blob_write_close(&w) == BLOB_WR_PHASE, "IDLE closed");
	CHECK(blob_write_commit_check(&w) == BLOB_WR_PHASE,
	      "IDLE may write a header");

	blob_write_arm(&w, 0u, 100u);
	CHECK(blob_write_data(&w, buf, 4u) == BLOB_WR_PHASE,
	      "payload arrived before block 0");
	CHECK(blob_write_close(&w) == BLOB_WR_PHASE, "ARMED closed");
	CHECK(blob_write_commit_check(&w) == BLOB_WR_PHASE,
	      "ARMED may write a header");

	(void)blob_write_begin(&w, 16u);
	CHECK(blob_write_begin(&w, 16u) == BLOB_WR_PHASE,
	      "a second block 0 was accepted");
	CHECK(blob_write_commit_check(&w) == BLOB_WR_PHASE,
	      "a transfer still running may write its header");

	(void)blob_write_data(&w, buf, 16u);
	(void)blob_write_close(&w);
	CHECK(blob_write_data(&w, buf, 1u) == BLOB_WR_PHASE,
	      "data arrived after the batch closed");
	CHECK(blob_write_close(&w) == BLOB_WR_PHASE, "a closed batch closed again");

	/* [!] The sticky latch.  A program that did not report OK -- or a
	 * cancel, or a transport that died -- breaks the transfer, and there is
	 * no path back except a reset.  This is what stops a header describing
	 * a payload that was only partly programmed. */
	blob_write_arm(&w, 0u, 100u);
	(void)blob_write_begin(&w, 16u);
	(void)blob_write_data(&w, buf, 8u);
	blob_write_break(&w);
	CHECK(w.phase == BLOB_PHASE_BROKEN, "break did not break");
	blob_write_break(&w);
	CHECK(blob_write_data(&w, buf, 1u) == BLOB_WR_BROKEN &&
	      blob_write_close(&w) == BLOB_WR_BROKEN &&
	      blob_write_commit_check(&w) == BLOB_WR_BROKEN &&
	      blob_write_begin(&w, 16u) == BLOB_WR_BROKEN,
	      "a broken transfer took another step");

	/* Breaking a transfer that has already succeeded still stops the
	 * header: the caller found out afterwards. */
	blob_write_arm(&w, 0u, 100u);
	(void)blob_write_begin(&w, 8u);
	(void)blob_write_data(&w, buf, 8u);
	(void)blob_write_close(&w);
	blob_write_break(&w);
	CHECK(blob_write_commit_check(&w) == BLOB_WR_BROKEN,
	      "a header was allowed after the transfer broke");

	/* Reset is the one way back, and it leaves nothing of the last one. */
	blob_write_reset(&w);
	CHECK(w.phase == BLOB_PHASE_IDLE && w.received == 0u &&
	      w.declared == 0u && w.crc == 0u,
	      "reset left something behind");
	blob_write_arm(&w, 1u, 100u);
	CHECK(blob_write_begin(&w, 8u) == BLOB_WR_OK,
	      "the port could not be armed again after a reset");

	/* NULL is refused rather than crashed on, and the void helpers ignore
	 * it: every exit path from `blob write` runs the reset. */
	CHECK(blob_write_begin(NULL, 8u) == BLOB_WR_PHASE &&
	      blob_write_data(NULL, buf, 1u) == BLOB_WR_PHASE &&
	      blob_write_close(NULL) == BLOB_WR_PHASE &&
	      blob_write_commit_check(NULL) == BLOB_WR_PHASE,
	      "a NULL transfer was acted on");
	blob_write_arm(NULL, 0u, 0u);
	blob_write_break(NULL);
	blob_write_reset(NULL);
}

int main(void)
{
	t_names();
	t_roundtrip();
	t_empty_and_foreign();
	t_magic_page();
	t_body_rejections();
	t_codec_edges();
	t_choose();
	t_transfer_happy();
	t_transfer_refusals();
	t_transfer_phases();

	if (failures != 0) {
		printf("test_blob_state: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_state: all passed\n");
	return 0;
}
