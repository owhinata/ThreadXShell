/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the `blob write` coordinator (#92, svc/blob/blob_write.c;
 * shared since #192).
 *
 * WHAT IT IS FOR.  A write holds two things at once -- the board writer's
 * reservation and the console claim -- and there are nine ways to leave: the
 * reservation refused, no slot to write into, the erase cancelled, the erase
 * incomplete, the console busy, the transfer cancelled or timed out, a program
 * refused, the header refused, the read-back disagreeing or failing.  Each has
 * to give
 * back exactly what it took.  On a board perhaps two of those can be staged, each costs
 * flash cycles (on grove-vision-ai-v2, of a part whose endurance is not
 * documented, #89), and a leaked reservation does not announce itself -- it
 * turns the next reader into "busy" for the rest of the session, at which
 * point the cause is long gone.
 *
 * So the coordinator takes its operations as a vtable and this drives it: every
 * failure injected in turn, and after each one the same three questions --
 * was the reservation given back exactly once, with the token that was handed
 * out, and was the console released exactly as often as it was claimed.
 *
 * [!] THE SINK IS DRIVEN FROM HERE TOO.  mock_receive() is handed the sink the
 * coordinator built and calls begin()/write() itself, which is how a rejected
 * block 0, a short file and an overrun are produced without a PC.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blob_write.h"

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

/* The staging buffer the coordinator is handed.  The test's own, the size the
 * board passes (blob_stage.h's BLOB_STAGE_BYTES), and NOT the board's object:
 * since issue #192 the coordinator takes the buffer as an argument, so this
 * links no blob_stage.c and a case can hand it a buffer of any size. */
#define STAGE_BYTES   (64u * 1024u)
static uint8_t stage64[STAGE_BYTES];

/* What run() hands the coordinator; reset() points it back at stage64. */
static uint8_t *g_stage;
static uint32_t g_stage_len;

/* A pretend part: one slot, a 4 KB header unit, and a payload of exactly two
 * staging buffers -- so that "the chunk filled up and was flushed" is a case
 * this test actually reaches rather than one it describes. */
#define MOCK_BASE     0x00001000u
#define MOCK_PAYLOAD  0x00002000u
#define MOCK_CAP      (2u * STAGE_BYTES)
#define MOCK_FLASH    (MOCK_PAYLOAD + MOCK_CAP)  /* modelled from address 0   */
#define MOCK_TOKEN    0xABCD0001u
#define SLOTS         4u
#define LOG_PROGRAMS  64u   /* programs whose address and length are kept */

struct mock {
	/* what happened */
	unsigned reserves, unreserves, claims, releases, programs, erases;
	unsigned receives, read_backs, stats, announces;
	unsigned announce_slot;
	unsigned magic_programs;  /* programs aimed at the magic page          */
	unsigned read_backs_at_magic; /* read_backs when the magic was programmed */
	uint32_t token_given, token_returned;
	uint32_t prog_addr[LOG_PROGRAMS], prog_len[LOG_PROGRAMS];
	/* what to break */
	int fail_reserve, fail_claim, fail_stat, fail_geometry;
	int background;           /* a bg job: no console claim can ever work */
	unsigned console_asks;
	int erase_rc, erase_cancel;
	int program_fail_at;      /* 1-based; 0 = never */
	int recv_rc;              /* what the transfer reports */
	int recv_no_file;         /* the batch closes without a file in it */
	int reject_begin;         /* declare more than the slot can hold */
	uint32_t declared, sent;  /* what block 0 says, and what arrives */
	int corrupt_read_back;    /* the flash disagrees with what was written */
	int fail_read_back;       /* the first read-back reports an error      */
	/* What a FAILED program leaves in the flash: 0 nothing, 1 the first half
	 * of what it was given, 2 all of it.  A program that reports failure says
	 * nothing certain about the page, and the magic page's three outcomes
	 * read differently (#194). */
	int fail_writes;
	unsigned count;           /* slots the table has */
	/* the pretend flash and the pretend slot contents */
	uint8_t flash[MOCK_FLASH];
	enum blob_slot_state state[SLOTS];
	const char *name[SLOTS];
};

static struct mock m;

static int mock_reserve(void *ctx, uint32_t *token)
{
	struct mock *k = ctx;

	k->reserves++;
	if (k->fail_reserve) {
		*token = 0u;
		return -1;
	}
	*token = MOCK_TOKEN;
	k->token_given = MOCK_TOKEN;
	return 0;
}

static void mock_unreserve(void *ctx, uint32_t token)
{
	struct mock *k = ctx;

	k->unreserves++;
	k->token_returned = token;
}

static int mock_erase(void *ctx, uint32_t token, uint32_t addr, uint32_t len,
                      uint32_t *done, int *cancelled)
{
	struct mock *k = ctx;

	k->erases++;
	CHECK(token == MOCK_TOKEN, "erase got token %08lX", (unsigned long)token);
	CHECK(addr == MOCK_BASE && len == (MOCK_PAYLOAD - MOCK_BASE) + MOCK_CAP,
	      "erase span is 0x%lx+%lu", (unsigned long)addr, (unsigned long)len);
	if (addr + len <= MOCK_FLASH)
		memset(&k->flash[addr], 0xFF, len);
	*done = k->erase_cancel ? len / 2u : len;
	*cancelled = k->erase_cancel;
	return k->erase_rc;
}

static int mock_program(void *ctx, uint32_t token, uint32_t addr,
                        const void *data, uint32_t len)
{
	struct mock *k = ctx;

	k->programs++;
	if (k->programs <= LOG_PROGRAMS) {
		k->prog_addr[k->programs - 1u] = addr;
		k->prog_len[k->programs - 1u] = len;
	}
	CHECK(token == MOCK_TOKEN, "program got token %08lX", (unsigned long)token);
	if (addr == MOCK_BASE) {
		k->magic_programs++;
		k->read_backs_at_magic = k->read_backs;
	}
	if (k->program_fail_at != 0 && (unsigned)k->program_fail_at == k->programs) {
		uint32_t n = (k->fail_writes == 2) ? len
		           : (k->fail_writes == 1) ? len / 2u : 0u;

		if (addr + n <= MOCK_FLASH)
			memcpy(&k->flash[addr], data, n);
		return -1;
	}
	CHECK(addr + len <= MOCK_FLASH, "program past the pretend part");
	if (addr + len <= MOCK_FLASH)
		memcpy(&k->flash[addr], data, len);
	return 0;
}

static int mock_console_ready(void *ctx)
{
	struct mock *k = ctx;

	k->console_asks++;
	CHECK(k->reserves == 0u, "the console was asked about AFTER reserving");
	return k->background ? -1 : 0;
}

static int mock_claim(void *ctx)
{
	struct mock *k = ctx;

	k->claims++;
	return k->fail_claim ? -1 : 0;
}

static void mock_release(void *ctx)
{
	((struct mock *)ctx)->releases++;
}

/* The transfer.  Drives the coordinator's own sink, which is how the protocol
 * would: block 0 first, then the payload in 1 KB pieces. */
static int mock_receive(void *ctx, const struct ym_sink *sink)
{
	struct mock *k = ctx;
	static uint8_t block[1024];
	uint32_t off;

	k->receives++;
	CHECK(k->claims == k->releases + 1u,
	      "the transfer ran without the console claimed");
	if (k->recv_no_file)
		return 0;              /* a batch that closed with nothing in it */
	if (sink->begin(sink->ctx, "sender-called-it-this.bin",
	                k->reject_begin ? MOCK_CAP + 1u : k->declared) != 0)
		return 1;                       /* the receiver cancels the batch */
	for (off = 0u; off < k->sent; off += sizeof block) {
		uint32_t take = k->sent - off;
		uint32_t i;

		if (take > sizeof block)
			take = sizeof block;
		/* [!] POSITION-DEPENDENT, WITH A PERIOD LONGER THAN A CHUNK.  This
		 * used to be `(off + i) * 7 + 1`, whose low byte repeats every 256 --
		 * so every 1 KB block was identical and so was every 64 KB staging
		 * chunk, and a coordinator that duplicated a chunk, re-sent a stale
		 * one or swapped two would have passed both the CRC and the
		 * read-back.  That is the same blindness a constant 0xA5 gave the
		 * write path for the whole of issue #88. */
		for (i = 0u; i < take; i++) {
			uint32_t v = (off + i) * 2654435761u;

			block[i] = (uint8_t)(v >> 24);
		}
		if (sink->write(sink->ctx, block, take) != 0)
			return 1;
	}
	return k->recv_rc;
}

static int mock_read_back(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	struct mock *k = ctx;

	k->read_backs++;
	if (addr + len > MOCK_FLASH)
		return -1;
	if (k->fail_read_back && k->read_backs == 1u)
		return -1;
	memcpy(buf, &k->flash[addr], len);
	/* The flash disagreeing with what went into it: one byte, on the first
	 * read-back only, which is all a CRC needs to notice. */
	if (k->corrupt_read_back && k->read_backs == 1u && len > 0u)
		((uint8_t *)buf)[0] ^= 0x01u;
	return 0;
}

/* The announcement, and WHEN it happens is the whole of what it is for: after
 * the slot is chosen, before the erase, and never on a path that refuses. */
static void mock_announce(void *ctx, unsigned slot, uint32_t base, uint32_t bytes)
{
	struct mock *k = ctx;

	k->announces++;
	k->announce_slot = slot;
	CHECK(base == MOCK_BASE, "announced base 0x%08lx", (unsigned long)base);
	CHECK(bytes == (MOCK_PAYLOAD - MOCK_BASE) + MOCK_CAP,
	      "announced %lu bytes", (unsigned long)bytes);
	CHECK(k->erases == 0u, "announced AFTER the erase had started");
	CHECK(k->reserves == 1u, "announced outside the reservation");
}

static unsigned mock_count(void *ctx)
{
	return ((struct mock *)ctx)->count;
}

static int mock_stat(void *ctx, unsigned slot, struct blob_info *info)
{
	struct mock *k = ctx;

	k->stats++;
	if (k->fail_stat)
		return -1;
	CHECK(k->reserves == 1u && k->unreserves == 0u,
	      "a slot was stat'd outside the reservation");
	memset(info, 0, sizeof *info);
	info->state = k->state[slot];
	if (k->name[slot] != NULL)
		memcpy(info->name, k->name[slot], strlen(k->name[slot]));
	info->length = 16u;
	return 0;
}

static int mock_geometry(void *ctx, unsigned slot, uint32_t *base,
                         uint32_t *paddr, uint32_t *pmax)
{
	struct mock *k = ctx;

	(void)slot;
	if (k->fail_geometry)
		return -1;
	if (base)  *base  = MOCK_BASE;
	if (paddr) *paddr = MOCK_PAYLOAD;
	if (pmax)  *pmax  = MOCK_CAP;
	return 0;
}

static const struct blob_write_ops OPS = {
	&m, mock_reserve, mock_unreserve, mock_erase, mock_program,
	mock_console_ready, mock_claim, mock_release, mock_receive,
	mock_read_back, NULL,
	mock_announce, mock_count, mock_stat, mock_geometry,
};

/* Every case starts from a part that works and a transfer that completes. */
static void reset(void)
{
	unsigned i;

	memset(&m, 0, sizeof m);
	m.count = SLOTS;
	m.declared = 4096u;
	m.sent = 4096u;
	for (i = 0u; i < SLOTS; i++)
		m.state[i] = BLOB_EMPTY;
	memset(m.flash, 0xFF, sizeof m.flash);
	g_stage = stage64;
	g_stage_len = STAGE_BYTES;
}

/* The three questions, asked after every single case. */
static void check_unwound(const char *what, int reserved, int claimed)
{
	CHECK(m.unreserves == (unsigned)(reserved ? 1 : 0),
	      "%s: unreserve called %u time(s), expected %d", what,
	      m.unreserves, reserved ? 1 : 0);
	if (reserved)
		CHECK(m.token_returned == m.token_given,
		      "%s: unreserved token %08lX, was given %08lX", what,
		      (unsigned long)m.token_returned,
		      (unsigned long)m.token_given);
	/* A claim that was REFUSED needs no release; every one that succeeded
	 * gets exactly one. */
	{
		unsigned want = m.fail_claim ? m.claims - 1u : m.claims;

		CHECK(m.releases == want,
		      "%s: %u claim(s), %u release(s), expected %u", what,
		      m.claims, m.releases, want);
	}
	CHECK(!claimed || m.claims == 1u, "%s: claimed %u times", what, m.claims);
}

/* [!] The report is filled with 0xFF before every run, so a field the
 * coordinator leaves unset shows up as garbage rather than as a plausible 0. */
static enum blob_write_result run(const char *name, int want,
                                  struct blob_write_report *rep)
{
	memset(rep, 0xFF, sizeof *rep);
	return blob_write_run(&OPS, g_stage, g_stage_len, name, want, rep);
}

/* Decode the pretend slot's header as `blob list` would. */
static enum blob_slot_state slot_state(struct blob_info *info)
{
	return blob_hdr_decode(&m.flash[MOCK_BASE], BLOB_HDR_SPAN, MOCK_BASE,
	                       MOCK_CAP, info, NULL);
}

/* [!] NOT CHOSEN MEANS slot_chosen IS 0 -- and slot 0 is a real slot, and
 * BLOB_CHOICE_REUSE is 0, so a zeroed report used to read as "slot 0, reused"
 * for writes refused before any slot was looked at (#194). */
static void check_not_chosen(const char *what, const struct blob_write_report *rep)
{
	CHECK(rep->slot_chosen == 0, "%s: the report says a slot was chosen (%d, "
	      "slot %u)", what, rep->slot_chosen, rep->slot);
}

static void check_chosen(const char *what, const struct blob_write_report *rep,
                         unsigned slot)
{
	CHECK(rep->slot_chosen != 0 && rep->slot == slot,
	      "%s: chosen %d, slot %u, expected slot %u", what, rep->slot_chosen,
	      rep->slot, slot);
}

/* [!] The magic page is written after the WHOLE payload was read back: the
 * first program aimed at it saw every read-back already done (#194). */
static void check_magic_after_verify(const char *what)
{
	CHECK(m.magic_programs == 1u, "%s: the magic was programmed %u time(s)",
	      what, m.magic_programs);
	CHECK(m.read_backs > 0u && m.read_backs_at_magic == m.read_backs,
	      "%s: the magic went down after %u of %u read-back(s)", what,
	      m.read_backs_at_magic, m.read_backs);
}

/* ---- the path that works -------------------------------------------------- */

static void t_stored(void)
{
	struct blob_write_report rep;
	enum blob_write_result res;

	reset();
	res = run("model.tflite", 1, &rep);
	CHECK(res == BLOB_WRITE_STORED, "a clean write is %s",
	      blob_write_result_name(res));
	CHECK(rep.slot == 1u && rep.choice == BLOB_CHOICE_FRESH,
	      "went to slot %u (%s)", rep.slot, blob_choice_name(rep.choice));
	CHECK(rep.received == 4096u && rep.declared == 4096u, "received %lu B",
	      (unsigned long)rep.received);
	CHECK(rep.crc == rep.verified, "the read-back CRC does not match");
	/* 1 erase + 1 payload program + body + magic. */
	CHECK(rep.transactions == 4u, "%lu transaction(s), expected 4",
	      (unsigned long)rep.transactions);
	CHECK(m.programs == 3u, "%u program(s)", m.programs);
	CHECK(m.announces == 1u && m.announce_slot == 1u,
	      "announced %u time(s) for slot %u", m.announces, m.announce_slot);
	check_chosen("stored", &rep, 1u);
	check_magic_after_verify("stored");
	check_unwound("stored", 1, 1);

	/* The header is in the flash and decodes, magic and all -- which is the
	 * one end-to-end thing this test can check: the coordinator, the codec
	 * and the CRC agreeing on the same bytes. */
	{
		struct blob_info info;
		enum blob_hdr_reject why;
		enum blob_slot_state st;

		st = blob_hdr_decode(&m.flash[MOCK_BASE], BLOB_HDR_SPAN,
		                     MOCK_BASE, MOCK_CAP, &info, &why);
		CHECK(st == BLOB_VALID, "the stored header reads %s (%s)",
		      blob_slot_state_name(st), blob_hdr_reject_name(why));
		CHECK(strcmp(info.name, "model.tflite") == 0,
		      "stored name is '%s'", info.name);
		CHECK(info.length == 4096u && info.crc32 == rep.crc,
		      "stored length/crc do not match what arrived");
	}

	/* A payload that exactly fills the slot: two full staging buffers, so the
	 * flush inside the sink runs as well as the one at the end. */
	reset();
	m.declared = m.sent = MOCK_CAP;
	res = run("full", 0, &rep);
	CHECK(res == BLOB_WRITE_STORED, "a slot-filling payload is %s",
	      blob_write_result_name(res));
	CHECK(rep.received == MOCK_CAP, "received %lu B of %lu",
	      (unsigned long)rep.received, (unsigned long)MOCK_CAP);
	CHECK(rep.crc == rep.verified, "the read-back CRC does not match");
	/* 1 erase + 2 payload chunks + body + magic. */
	CHECK(rep.transactions == 5u, "%lu transaction(s), expected 5",
	      (unsigned long)rep.transactions);
	check_magic_after_verify("full slot");
	check_unwound("full slot", 1, 1);

	/* One byte more than the slot holds is refused by the sink, before any of
	 * it reaches the flash. */
	reset();
	m.declared = m.sent = MOCK_CAP + 1u;
	CHECK(run("toobig", 0, &rep) == BLOB_WRITE_XFER_FAILED,
	      "a payload one byte too big was accepted");
	CHECK(m.programs == 0u, "a payload too big still programmed");
	check_unwound("too big", 1, 1);
}

/* ---- choosing, before anything is touched --------------------------------- */

static void t_choice(void)
{
	struct blob_write_report rep;

	reset();
	CHECK(run("newname", -1, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_NEED_SLOT,
	      "a new name without a slot was accepted");
	CHECK(m.erases == 0u && m.claims == 0u,
	      "a refused choice still erased or claimed");
	CHECK(m.announces == 0u,
	      "a refused choice still warned about destroying a slot");
	check_not_chosen("need slot", &rep);
	check_unwound("need slot", 1, 0);

	reset();
	m.state[2] = BLOB_VALID;
	m.name[2] = "model.tflite";
	CHECK(run("model.tflite", -1, &rep) == BLOB_WRITE_STORED &&
	      rep.slot == 2u && rep.choice == BLOB_CHOICE_REUSE,
	      "a known name did not go back to its slot");
	check_chosen("reuse", &rep, 2u);
	check_unwound("reuse", 1, 1);

	reset();
	m.state[2] = BLOB_VALID;
	m.name[2] = "model.tflite";
	CHECK(run("model.tflite", 1, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_DUPLICATE,
	      "a duplicate name was written to another slot");
	CHECK(m.erases == 0u, "a duplicate name still erased something");
	CHECK(m.announces == 0u, "a duplicate name still warned about a slot");
	check_not_chosen("duplicate", &rep);
	check_unwound("duplicate", 1, 0);

	reset();
	m.state[1] = BLOB_VALID;
	m.name[1] = "something.else";
	CHECK(run("mine", 1, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_OCCUPIED,
	      "another blob was overwritten");
	CHECK(m.erases == 0u, "an occupied slot was erased anyway");
	CHECK(m.announces == 0u, "an occupied slot was announced anyway");
	check_not_chosen("occupied", &rep);
	check_unwound("occupied", 1, 0);

	/* [!] No table, no reservation: nothing is taken before there is
	 * something to write into. */
	reset();
	m.count = 0u;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_BAD_MAP, "an empty table chose a slot");
	CHECK(m.reserves == 0u, "an empty table still reserved the part");
	check_not_chosen("no table", &rep);
	check_unwound("no table", 0, 0);
}

/* ---- everything that can go wrong ----------------------------------------- */

static void t_failures(void)
{
	struct blob_write_report rep;

	reset();
	m.fail_reserve = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_BUSY, "a refused reserve is not busy");
	CHECK(m.erases == 0u && m.claims == 0u, "a refused reserve went on");
	CHECK(m.announces == 0u, "a refused reserve still announced a slot");
	check_not_chosen("reserve refused", &rep);
	check_unwound("reserve refused", 0, 0);

	/* [!] A cancel during the erase must not go on to program: the flash is
	 * half erased, and programming into it reads back as the AND of the two,
	 * which the writer turns into a terminal fault. */
	reset();
	m.erase_cancel = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_CANCELLED, "a cancelled erase is not");
	CHECK(m.claims == 0u && m.programs == 0u,
	      "a cancelled erase went on to the transfer");
	CHECK(rep.erased != 0u, "the report does not say how much was erased");
	check_chosen("erase cancelled", &rep, 0u);
	check_unwound("erase cancelled", 1, 0);

	reset();
	m.erase_rc = -1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_ERASE_FAILED, "a failed erase is not");
	CHECK(m.claims == 0u && m.programs == 0u, "a failed erase went on");
	check_unwound("erase failed", 1, 0);

	reset();
	m.fail_claim = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_NO_CONSOLE, "a refused console is not");
	CHECK(m.receives == 0u, "the transfer ran without the console");
	check_unwound("console refused", 1, 0);

	/* [!] A BACKGROUND JOB COSTS NOTHING.  cli_console_claim() refuses one on
	 * `sh->fg != NULL` alone, which is knowable before anything is spent -- and
	 * the order used to be reserve, choose, ERASE, then find out.  `blob write
	 * existing 0 &` destroyed two megabytes and printed "drop the trailing
	 * '&'".  Raised by review of d98e7f5. */
	reset();
	m.background = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_NO_CONSOLE,
	      "a background job is not refused as a console failure");
	CHECK(m.console_asks == 1u, "the console was asked about %u time(s)",
	      m.console_asks);
	CHECK(m.reserves == 0u && m.stats == 0u && m.erases == 0u &&
	      m.announces == 0u && m.claims == 0u,
	      "a background job reserved, stat'd, announced or erased something");
	check_not_chosen("background", &rep);
	check_unwound("background", 0, 0);

	/* Block 0 declaring more than the slot holds: the sink rejects, the
	 * receiver cancels the batch, and nothing is programmed. */
	reset();
	m.reject_begin = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_XFER_FAILED, "an oversized file is not");
	CHECK(m.programs == 0u, "an oversized file still programmed");
	check_unwound("begin rejected", 1, 1);

	/* The batch never closed -- all the data arrived and the sender stopped. */
	reset();
	m.recv_rc = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_XFER_FAILED, "a dead transfer is not");
	CHECK(m.programs <= 1u, "a dead transfer wrote a header");
	check_unwound("transfer failed", 1, 1);

	/* Fewer bytes than block 0 declared, with a clean close.  A header whose
	 * length does not describe the payload must not be written. */
	reset();
	m.declared = 4096u;
	m.sent = 3000u;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_XFER_FAILED, "a short file is not");
	CHECK(rep.received == 3000u, "the report lost the byte count");
	check_unwound("short file", 1, 1);

	/* A batch that closed without a file in it.  Nothing was declared and
	 * nothing arrived, so the phase machine is still ARMED -- and it is that
	 * refusal, rather than any reasoning in the coordinator, which stops a
	 * header being written over an untouched slot. */
	reset();
	m.recv_no_file = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_XFER_FAILED, "an empty batch is not");
	CHECK(m.programs == 0u, "an empty batch wrote something");
	CHECK(m.flash[MOCK_BASE] == 0xFFu, "an empty batch left a magic");
	check_unwound("empty batch", 1, 1);

	/* A program that does not take, mid-transfer.  The sticky latch is what
	 * stops the header describing a payload that is only partly there. */
	reset();
	m.declared = m.sent = MOCK_CAP;      /* two chunks, so the first flushes */
	m.program_fail_at = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_PROG_FAILED, "a failed program is not");
	check_unwound("program failed", 1, 1);

	/* The body page refused: no magic follows, so the slot reads INCOMPLETE
	 * rather than valid. */
	reset();
	m.program_fail_at = 2;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_PROG_FAILED, "a failed body is not");
	CHECK(m.flash[MOCK_BASE] == 0xFFu, "the magic went down anyway");
	check_unwound("body failed", 1, 1);

	/* [!] THE MAGIC PAGE REFUSED, after the body took and the payload was
	 * read back and matched.  A program that reports failure says nothing
	 * certain about the page, so the coordinator reports PROG_FAILED and
	 * nothing about the state -- and each of the three things the flash can
	 * hold reads as itself: still erased INCOMPLETE, part-written INVALID,
	 * whole VALID (over a payload that DID match, #194). */
	{
		static const struct {
			int writes; enum blob_slot_state st; const char *what;
		} mf[] = {
			{ 0, BLOB_INCOMPLETE, "magic failed, page still erased" },
			{ 1, BLOB_INVALID,    "magic failed, page part-written" },
			{ 2, BLOB_VALID,      "magic failed, page written whole" },
		};
		unsigned j;

		for (j = 0u; j < sizeof mf / sizeof mf[0]; j++) {
			struct blob_info info;
			enum blob_slot_state st;

			reset();
			m.program_fail_at = 3;
			m.fail_writes = mf[j].writes;
			CHECK(run("x", 0, &rep) == BLOB_WRITE_PROG_FAILED,
			      "%s: not PROG_FAILED", mf[j].what);
			CHECK(rep.verified == rep.crc,
			      "%s: the payload was not verified before the magic",
			      mf[j].what);
			check_magic_after_verify(mf[j].what);
			st = slot_state(&info);
			CHECK(st == mf[j].st, "%s: reads %s, expected %s", mf[j].what,
			      blob_slot_state_name(st),
			      blob_slot_state_name(mf[j].st));
			check_chosen(mf[j].what, &rep, 0u);
			check_unwound(mf[j].what, 1, 1);
		}
	}

	reset();
	m.fail_stat = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_REFUSED, "an unreadable slot is not");
	CHECK(m.erases == 0u, "an unreadable table still erased");
	check_not_chosen("stat failed", &rep);
	check_unwound("stat failed", 1, 0);

	reset();
	m.fail_geometry = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_REFUSED, "a slot with no geometry is not");
	CHECK(m.erases == 0u, "a slot with no geometry was erased");
	/* Chosen and then refused: the number is true, nothing was touched. */
	check_chosen("geometry failed", &rep, 0u);
	check_unwound("geometry failed", 1, 0);

	/* [!] EVERYTHING WENT IN AND THE READ-BACK DISAGREES -- or cannot be done.
	 * The magic is NOT written (#194): the slot reads INCOMPLETE, never
	 * valid, and the body still says what was meant to be there.  Until #194
	 * the magic went down first and this case left a slot `blob list` called
	 * valid over bytes that were not the file. */
	{
		static const struct { int corrupt, fail; const char *what; } vf[] = {
			{ 1, 0, "verify mismatch"  },
			{ 0, 1, "read-back failed" },
		};
		unsigned j;

		for (j = 0u; j < sizeof vf / sizeof vf[0]; j++) {
			struct blob_info info;
			enum blob_slot_state st;

			reset();
			m.corrupt_read_back = vf[j].corrupt;
			m.fail_read_back = vf[j].fail;
			CHECK(run("x", 0, &rep) == BLOB_WRITE_VERIFY_FAILED,
			      "%s: not reported", vf[j].what);
			if (vf[j].corrupt)
				CHECK(rep.verified != rep.crc,
				      "%s: the report claims the CRCs match", vf[j].what);
			CHECK(m.magic_programs == 0u,
			      "%s: the magic was programmed %u time(s)", vf[j].what,
			      m.magic_programs);
			/* 1 payload chunk + the body; 1 erase + those two. */
			CHECK(m.programs == 2u && rep.transactions == 3u,
			      "%s: %u program(s), %lu transaction(s)", vf[j].what,
			      m.programs, (unsigned long)rep.transactions);
			st = slot_state(&info);
			CHECK(st == BLOB_INCOMPLETE, "%s: the slot reads %s",
			      vf[j].what, blob_slot_state_name(st));
			CHECK(st == BLOB_INCOMPLETE && strcmp(info.name, "x") == 0 &&
			      info.length == 4096u && info.crc32 == rep.crc,
			      "%s: the body does not describe what arrived", vf[j].what);
			check_chosen(vf[j].what, &rep, 0u);
			check_unwound(vf[j].what, 1, 1);
		}
	}

	/* [!] Refused for its arguments: the report is still cleared, so a
	 * caller that logs it does not read a slot out of it. */
	memset(&rep, 0xFF, sizeof rep);
	CHECK(blob_write_run(NULL, stage64, STAGE_BYTES, "x", 0, &rep) ==
	      BLOB_WRITE_REFUSED, "a NULL vtable was acted on");
	check_not_chosen("NULL vtable", &rep);
	memset(&rep, 0xFF, sizeof rep);
	CHECK(blob_write_run(&OPS, stage64, STAGE_BYTES, NULL, 0, &rep) ==
	      BLOB_WRITE_REFUSED, "a NULL name was acted on");
	check_not_chosen("NULL name", &rep);
	CHECK(blob_write_run(&OPS, stage64, STAGE_BYTES, "x", 0, NULL) ==
	      BLOB_WRITE_REFUSED, "a NULL report was acted on");
}

/* ---- the staging buffer the caller hands over (issue #192) --------------- */

/* Nothing at all may have happened: not the console question, not the
 * reservation, not a stat, an erase, a claim or a program. */
static void check_nothing_taken(const char *what)
{
	CHECK(m.console_asks == 0u && m.reserves == 0u && m.unreserves == 0u &&
	      m.stats == 0u && m.announces == 0u && m.erases == 0u &&
	      m.claims == 0u && m.releases == 0u && m.receives == 0u &&
	      m.programs == 0u && m.read_backs == 0u,
	      "%s: something was taken (asks %u, reserves %u, stats %u, erases %u, "
	      "claims %u, programs %u)", what, m.console_asks, m.reserves,
	      m.stats, m.erases, m.claims, m.programs);
}

/* [!] A BUFFER THE COORDINATOR CANNOT USE IS REFUSED BEFORE ANYTHING.  NULL, a
 * length under one header page, and a length that is not a whole number of
 * pages -- each one BLOB_WRITE_REFUSED with every counter still at zero. */
static void t_bad_stage(void)
{
	static const struct { int null; uint32_t len; const char *what; } bad[] = {
		{ 1, STAGE_BYTES,              "NULL buffer"            },
		{ 0, 0u,                       "zero length"            },
		{ 0, 1u,                       "one byte"               },
		{ 0, BLOB_HDR_PAGE - 1u,       "one byte under a page"  },
		{ 0, BLOB_HDR_PAGE + 1u,       "a page and one byte"    },
		{ 0, BLOB_HDR_PAGE + BLOB_HDR_PAGE / 2u, "a page and a half" },
		{ 0, STAGE_BYTES - 1u,         "64 KB less one byte"    },
	};
	struct blob_write_report rep;
	unsigned i;

	for (i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
		reset();
		g_stage = bad[i].null ? NULL : stage64;
		g_stage_len = bad[i].len;
		CHECK(run("x", 0, &rep) == BLOB_WRITE_REFUSED,
		      "%s: was not refused", bad[i].what);
		check_nothing_taken(bad[i].what);
		check_not_chosen(bad[i].what, &rep);
	}

	/* Two pages is a whole number of them, and works. */
	reset();
	g_stage_len = 2u * BLOB_HDR_PAGE;
	CHECK(run("two", 0, &rep) == BLOB_WRITE_STORED,
	      "a two-page buffer was not accepted");
	check_unwound("two pages", 1, 1);
}

/* A buffer of exactly one page, with bytes after it that must stay untouched:
 * an overrun of the buffer the caller handed over lands in @ref guard. */
#define GUARD_BYTES 64u
static struct {
	uint8_t page[BLOB_HDR_PAGE];
	uint8_t guard[GUARD_BYTES];
} one;

/* Ten whole pages and a 100-byte tail. */
#define ONE_SENT  (10u * BLOB_HDR_PAGE + 100u)
#define ONE_CHUNKS 11u

static void reset_one(void)
{
	reset();
	memset(one.guard, 0x5A, sizeof one.guard);
	g_stage = one.page;
	g_stage_len = BLOB_HDR_PAGE;
	m.declared = m.sent = ONE_SENT;
}

static void check_guard(const char *what)
{
	unsigned i;

	for (i = 0u; i < GUARD_BYTES; i++) {
		if (one.guard[i] != 0x5Au) {
			CHECK(0, "%s: the byte %u past the buffer was written", what, i);
			return;
		}
	}
}

static void t_one_page(void)
{
	struct blob_write_report rep;
	enum blob_write_result res;
	unsigned i;

	/* The path that works: the buffer is written out again and again, the
	 * last chunk is short, the body and the magic are separate programs, and
	 * the verify reads the payload back through the same page. */
	reset_one();
	res = run("onepage", 0, &rep);
	CHECK(res == BLOB_WRITE_STORED, "one page: %s", blob_write_result_name(res));
	CHECK(rep.received == ONE_SENT && rep.crc == rep.verified,
	      "one page: received %lu, crc %08lX verified %08lX",
	      (unsigned long)rep.received, (unsigned long)rep.crc,
	      (unsigned long)rep.verified);
	CHECK(m.programs == ONE_CHUNKS + 2u, "one page: %u program(s), expected %u",
	      m.programs, ONE_CHUNKS + 2u);
	CHECK(rep.transactions == 1u + ONE_CHUNKS + 2u,
	      "one page: %lu transaction(s)", (unsigned long)rep.transactions);
	for (i = 0u; i < ONE_CHUNKS; i++) {
		uint32_t want = (i + 1u < ONE_CHUNKS) ? BLOB_HDR_PAGE : 100u;

		CHECK(m.prog_addr[i] == MOCK_PAYLOAD + i * BLOB_HDR_PAGE &&
		      m.prog_len[i] == want,
		      "one page: chunk %u is %lu B at 0x%lx", i,
		      (unsigned long)m.prog_len[i], (unsigned long)m.prog_addr[i]);
	}
	CHECK(m.prog_addr[ONE_CHUNKS] == MOCK_BASE + BLOB_HDR_PAGE &&
	      m.prog_len[ONE_CHUNKS] == BLOB_BODY_SIZE,
	      "one page: the body went %lu B at 0x%lx",
	      (unsigned long)m.prog_len[ONE_CHUNKS],
	      (unsigned long)m.prog_addr[ONE_CHUNKS]);
	CHECK(m.prog_addr[ONE_CHUNKS + 1u] == MOCK_BASE &&
	      m.prog_len[ONE_CHUNKS + 1u] == BLOB_MAGIC_SIZE,
	      "one page: the magic went %lu B at 0x%lx",
	      (unsigned long)m.prog_len[ONE_CHUNKS + 1u],
	      (unsigned long)m.prog_addr[ONE_CHUNKS + 1u]);
	CHECK(m.read_backs == ONE_CHUNKS, "one page: %u read-back(s), expected %u",
	      m.read_backs, ONE_CHUNKS);
	check_magic_after_verify("one page");
	{
		struct blob_info info;
		enum blob_slot_state st = blob_hdr_decode(&m.flash[MOCK_BASE],
		                                          BLOB_HDR_SPAN, MOCK_BASE,
		                                          MOCK_CAP, &info, NULL);
		CHECK(st == BLOB_VALID && info.length == ONE_SENT &&
		      info.crc32 == rep.crc, "one page: the header reads %s",
		      blob_slot_state_name(st));
	}
	check_guard("one page");
	check_unwound("one page", 1, 1);

	/* Each program failing in turn: the first chunk, one in the middle, the
	 * short last one, the body and the magic.  Nothing is programmed after
	 * the one that failed, no magic follows a failed payload or body, and
	 * everything taken is given back once. */
	{
		static const unsigned at[] = { 1u, 6u, ONE_CHUNKS, ONE_CHUNKS + 1u,
		                               ONE_CHUNKS + 2u };

		for (i = 0u; i < sizeof at / sizeof at[0]; i++) {
			char what[48];

			snprintf(what, sizeof what, "one page, program %u fails", at[i]);
			reset_one();
			m.program_fail_at = (int)at[i];
			res = run("onepage", 0, &rep);
			CHECK(res == BLOB_WRITE_PROG_FAILED, "%s: %s", what,
			      blob_write_result_name(res));
			CHECK(m.programs == at[i], "%s: %u program(s) ran", what,
			      m.programs);
			/* The verify runs between the body and the magic, so only
			 * a failed magic has it behind it (#194). */
			if (at[i] <= ONE_CHUNKS + 1u)
				CHECK(m.read_backs == 0u, "%s: a verify ran", what);
			else
				check_magic_after_verify(what);
			if (at[i] <= ONE_CHUNKS + 1u)
				CHECK(m.flash[MOCK_BASE] == 0xFFu,
				      "%s: the magic went down", what);
			check_guard(what);
			check_unwound(what, 1, 1);
		}
	}

	/* The read-back disagreeing, through the one page. */
	reset_one();
	m.corrupt_read_back = 1;
	CHECK(run("onepage", 0, &rep) == BLOB_WRITE_VERIFY_FAILED,
	      "one page: a read-back that disagrees is not reported");
	CHECK(rep.verified != rep.crc, "one page: the report claims a match");
	CHECK(m.magic_programs == 0u && m.flash[MOCK_BASE] == 0xFFu,
	      "one page: the magic went down over a payload that did not match");
	check_guard("one page, verify mismatch");
	check_unwound("one page, verify mismatch", 1, 1);

	/* The transfer and the console failing, once each. */
	reset_one();
	m.recv_rc = 1;
	CHECK(run("onepage", 0, &rep) == BLOB_WRITE_XFER_FAILED,
	      "one page: a dead transfer is not");
	check_guard("one page, transfer failed");
	check_unwound("one page, transfer failed", 1, 1);

	reset_one();
	m.fail_claim = 1;
	CHECK(run("onepage", 0, &rep) == BLOB_WRITE_NO_CONSOLE,
	      "one page: a refused console is not");
	check_unwound("one page, console refused", 1, 0);

	reset_one();
	m.erase_rc = -1;
	CHECK(run("onepage", 0, &rep) == BLOB_WRITE_ERASE_FAILED,
	      "one page: a failed erase is not");
	check_unwound("one page, erase failed", 1, 0);
}

int main(void)
{
	t_stored();
	t_choice();
	t_failures();
	t_bad_stage();
	t_one_page();

	if (failures != 0) {
		printf("test_blob_write: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_write: all passed\n");
	return 0;
}
