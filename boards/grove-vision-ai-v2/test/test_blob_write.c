/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the `blob write` coordinator (#92, src/blob_write.c).
 *
 * WHAT IT IS FOR.  A write holds two things at once -- the NOR reservation and
 * the console claim -- and there are nine ways to leave: the reservation
 * refused, no slot to write into, the erase cancelled, the erase incomplete,
 * the console busy, the transfer cancelled or timed out, a program refused, the
 * header refused, the read-back disagreeing.  Each has to give back exactly
 * what it took.  On the board perhaps two of those can be staged, each costs
 * flash cycles of a part whose endurance is not documented (#89), and a leaked
 * reservation does not announce itself -- it turns the next `nn open` into
 * "busy" for the rest of the session, at which point the cause is long gone.
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
#include "blob_stage.h"

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

/* A pretend part: one slot, a 4 KB header unit, and a payload of exactly two
 * staging buffers -- so that "the chunk filled up and was flushed" is a case
 * this test actually reaches rather than one it describes. */
#define MOCK_BASE     0x00001000u
#define MOCK_PAYLOAD  0x00002000u
#define MOCK_CAP      (2u * BLOB_STAGE_BYTES)
#define MOCK_FLASH    (MOCK_PAYLOAD + MOCK_CAP)  /* modelled from address 0   */
#define MOCK_TOKEN    0xABCD0001u
#define SLOTS         4u

struct mock {
	/* what happened */
	unsigned reserves, unreserves, claims, releases, programs, erases;
	unsigned receives, read_backs, stats;
	uint32_t token_given, token_returned;
	/* what to break */
	int fail_reserve, fail_claim, fail_stat, fail_geometry;
	int erase_rc, erase_cancel;
	int program_fail_at;      /* 1-based; 0 = never */
	int recv_rc;              /* what the transfer reports */
	int recv_no_file;         /* the batch closes without a file in it */
	int reject_begin;         /* declare more than the slot can hold */
	uint32_t declared, sent;  /* what block 0 says, and what arrives */
	int corrupt_read_back;    /* the flash disagrees with what was written */
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
	CHECK(token == MOCK_TOKEN, "program got token %08lX", (unsigned long)token);
	if (k->program_fail_at != 0 && (unsigned)k->program_fail_at == k->programs)
		return -1;
	CHECK(addr + len <= MOCK_FLASH, "program past the pretend part");
	if (addr + len <= MOCK_FLASH)
		memcpy(&k->flash[addr], data, len);
	return 0;
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
		for (i = 0u; i < take; i++)
			block[i] = (uint8_t)((off + i) * 7u + 1u);
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
	memcpy(buf, &k->flash[addr], len);
	/* The flash disagreeing with what went into it: one byte, on the first
	 * read-back only, which is all a CRC needs to notice. */
	if (k->corrupt_read_back && k->read_backs == 1u && len > 0u)
		((uint8_t *)buf)[0] ^= 0x01u;
	return 0;
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
	mock_claim, mock_release, mock_receive, mock_read_back, NULL,
	mock_count, mock_stat, mock_geometry,
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

static enum blob_write_result run(const char *name, int want,
                                  struct blob_write_report *rep)
{
	return blob_write_run(&OPS, name, want, rep);
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
	check_unwound("need slot", 1, 0);

	reset();
	m.state[2] = BLOB_VALID;
	m.name[2] = "model.tflite";
	CHECK(run("model.tflite", -1, &rep) == BLOB_WRITE_STORED &&
	      rep.slot == 2u && rep.choice == BLOB_CHOICE_REUSE,
	      "a known name did not go back to its slot");
	check_unwound("reuse", 1, 1);

	reset();
	m.state[2] = BLOB_VALID;
	m.name[2] = "model.tflite";
	CHECK(run("model.tflite", 1, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_DUPLICATE,
	      "a duplicate name was written to another slot");
	CHECK(m.erases == 0u, "a duplicate name still erased something");
	check_unwound("duplicate", 1, 0);

	reset();
	m.state[1] = BLOB_VALID;
	m.name[1] = "something.else";
	CHECK(run("mine", 1, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_OCCUPIED,
	      "another blob was overwritten");
	CHECK(m.erases == 0u, "an occupied slot was erased anyway");
	check_unwound("occupied", 1, 0);

	/* [!] No table, no reservation: nothing is taken before there is
	 * something to write into. */
	reset();
	m.count = 0u;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_NO_SLOT &&
	      rep.choice == BLOB_CHOICE_BAD_MAP, "an empty table chose a slot");
	CHECK(m.reserves == 0u, "an empty table still reserved the part");
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

	/* The magic page refused, after the body took. */
	reset();
	m.program_fail_at = 3;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_PROG_FAILED, "a failed magic is not");
	{
		struct blob_info info;
		enum blob_slot_state st = blob_hdr_decode(&m.flash[MOCK_BASE],
		                                          BLOB_HDR_SPAN, MOCK_BASE,
		                                          MOCK_CAP, &info, NULL);
		CHECK(st == BLOB_INCOMPLETE, "a body without a magic reads %s",
		      blob_slot_state_name(st));
	}
	check_unwound("magic failed", 1, 1);

	reset();
	m.fail_stat = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_REFUSED, "an unreadable slot is not");
	CHECK(m.erases == 0u, "an unreadable table still erased");
	check_unwound("stat failed", 1, 0);

	reset();
	m.fail_geometry = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_REFUSED, "a slot with no geometry is not");
	CHECK(m.erases == 0u, "a slot with no geometry was erased");
	check_unwound("geometry failed", 1, 0);

	/* Everything went in and the flash disagrees anyway.  The blob IS stored
	 * -- the header is down -- and the operator is told the read-back does
	 * not match, which is a different thing from a failed write. */
	/* Everything went in and the read-back disagrees anyway.  The blob IS
	 * stored -- the header is down and `blob list` will show it -- and the
	 * operator is told the flash does not match what arrived, which is a
	 * different thing from a write that failed. */
	reset();
	m.corrupt_read_back = 1;
	CHECK(run("x", 0, &rep) == BLOB_WRITE_VERIFY_FAILED,
	      "a read-back that disagrees is not reported");
	CHECK(rep.verified != rep.crc, "the report claims the CRCs match");
	CHECK(m.flash[MOCK_BASE] != 0xFFu,
	      "the magic was withheld -- the blob is stored, verify is a check");
	check_unwound("verify mismatch", 1, 1);

	CHECK(blob_write_run(NULL, "x", 0, &rep) == BLOB_WRITE_REFUSED,
	      "a NULL vtable was acted on");
	CHECK(blob_write_run(&OPS, NULL, 0, &rep) == BLOB_WRITE_REFUSED,
	      "a NULL name was acted on");
	CHECK(blob_write_run(&OPS, "x", 0, NULL) == BLOB_WRITE_REFUSED,
	      "a NULL report was acted on");
}

int main(void)
{
	t_stored();
	t_choice();
	t_failures();

	if (failures != 0) {
		printf("test_blob_write: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_write: all passed\n");
	return 0;
}
