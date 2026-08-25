/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for svc/crc32.c AS the blob asset store USES IT (#92, #49 Step 2).
 *
 * The blob region stamps every stored asset with a CRC-32 computed from the YMODEM
 * stream as it arrives, chunk by chunk, and `blob verify` re-reads the flash and
 * compares.  That rests on two properties that are invisible in the signature:
 *
 *   A. starting from 0 it produces standard CRC-32/ISO-HDLC -- the same number as
 *      `crc32` / Python's zlib.crc32 on the PC, so a value printed by `blob list`
 *      can be checked against the file that was sent;
 *   B. feeding the previous result back in continues the same CRC, so a 1.7 MB
 *      model delivered as ~1665 separate 1024-byte blocks ends up with the value
 *      it would have had in one call.
 *
 * Case D is the trap that bit the donor (owhinata/wio-lite-ai#10): crc32_update()
 * inverts at BOTH ends, so the usual "init 0xFFFFFFFF, complement the result"
 * wrapper inverts twice and yields a different number.  It is asserted to be
 * wrong here so nobody re-adds it.
 *
 * Case E is what keeps the 64-byte table honest, and it is not redundant with A:
 * A's five expected values are constants in THIS file, so a table regenerated from
 * the wrong polynomial goes green again the moment someone "fixes" them to match.
 * Tried: with a CRC-32C table and A/B/D's constants updated to suit, A, B, C and D
 * all passed and E was the only failure.  E recomputes the CRC bit by bit straight
 * from 0xEDB88320 -- sharing no table with the implementation -- over every
 * single-byte input and over the whole stream.
 *
 * This is the only part of the blob work that can be verified without the board,
 * which is why it exists at all.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"

/* ---- A: canonical CRC-32/ISO-HDLC vectors -------------------------------- */

static void test_vectors(void)
{
	static const struct {
		const char *s;
		uint32_t    crc;
	} v[] = {
		{ "",                                            0x00000000u },
		{ "a",                                           0xE8B7BE43u },
		{ "abc",                                         0x352441C2u },
		{ "123456789",                                   0xCBF43926u },
		{ "The quick brown fox jumps over the lazy dog", 0x414FA339u },
	};
	size_t i;

	for (i = 0u; i < sizeof v / sizeof v[0]; i++) {
		uint32_t got = crc32_update(0u, v[i].s, strlen(v[i].s));

		if (got != v[i].crc) {
			printf("crc32(\"%s\") = %08X, expected %08X\n",
			       v[i].s, (unsigned)got, (unsigned)v[i].crc);
			assert(0);
		}
	}
	/* A zero-length chunk must not touch the buffer at all: a receiver that
	 * hands the sink an empty write passes whatever pointer it has. */
	assert(crc32_update(0xDEADBEEFu, NULL, 0u) == 0xDEADBEEFu);
	printf("  A. canonical vectors OK (and len 0 never dereferences buf)\n");
}

/* ---- B: chaining equals one call ----------------------------------------- */

/* Split "123456789" at every possible point; each split must reproduce the
 * one-shot value.  Includes the two degenerate splits (0 and 9 bytes), which is
 * also the zero-length-chunk case: an empty call must be the identity, or a
 * receiver handing the sink a 0-byte write would corrupt the running value. */
static void test_chain_all_splits(void)
{
	static const char msg[] = "123456789";
	size_t n = strlen(msg), cut;

	for (cut = 0u; cut <= n; cut++) {
		uint32_t crc = crc32_update(0u, msg, cut);

		crc = crc32_update(crc, msg + cut, n - cut);
		if (crc != 0xCBF43926u) {
			printf("split at %zu gave %08X\n", cut, (unsigned)crc);
			assert(0);
		}
	}
	printf("  B. chaining at every split point OK\n");
}

/* ---- the reference: CRC-32 straight from the polynomial, no table --------- */

/* Deliberately the slow, obvious formulation.  It shares nothing with crc32.c
 * except the polynomial constant, so agreement between the two is evidence about
 * the table rather than a restatement of it. */
#define CRC32_POLY_REFLECTED  0xEDB88320u

static uint32_t crc32_bitwise(uint32_t crc, const uint8_t *p, size_t len)
{
	crc = ~crc;
	while (len--) {
		unsigned bit;

		crc ^= *p++;
		for (bit = 0u; bit < 8u; bit++)
			crc = (crc & 1u) ? (crc >> 1) ^ CRC32_POLY_REFLECTED
			                 : (crc >> 1);
	}
	return ~crc;
}

/* ---- C/E: a blob-sized stream in YMODEM-sized blocks ---------------------- */

/* The real shape: the cls model (1,704,672 B -- the largest asset that will land
 * in a blob slot) delivered as 1024-byte blocks with a short final one,
 * accumulated the way the blob sink will, versus one call over the whole buffer. */
#define STREAM_LEN   1704672u         /* 1664 x 1024 + 736: a short final block */
#define BLOCK_LEN    1024u

static uint8_t stream[STREAM_LEN];

static void fill_stream(void)
{
	uint32_t i, x = 0x12345678u;

	/* Deterministic pseudo-random content: a constant or a counter would let a
	 * byte-order or table mistake cancel out. */
	for (i = 0u; i < STREAM_LEN; i++) {
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		stream[i] = (uint8_t)(x & 0xFFu);
	}
}

static void test_chain_stream(void)
{
	uint32_t whole, chained = 0u, off = 0u, blocks = 0u;

	whole = crc32_update(0u, stream, STREAM_LEN);
	while (off < STREAM_LEN) {
		uint32_t take = STREAM_LEN - off;

		if (take > BLOCK_LEN)
			take = BLOCK_LEN;
		chained = crc32_update(chained, stream + off, take);
		off += take;
		blocks++;
	}
	assert(blocks == 1665u);
	if (chained != whole) {
		printf("chained %08X != whole %08X\n",
		       (unsigned)chained, (unsigned)whole);
		assert(0);
	}
	/* A single flipped bit must change the answer -- otherwise `blob verify`
	 * would be checking nothing. */
	stream[STREAM_LEN / 2u] ^= 0x01u;
	assert(crc32_update(0u, stream, STREAM_LEN) != whole);
	stream[STREAM_LEN / 2u] ^= 0x01u;
	printf("  C. %lu-byte stream in %lu blocks OK (and bit-sensitive)\n",
	       (unsigned long)STREAM_LEN, (unsigned long)blocks);
}

/* ---- D: the wrapper that must NOT be added ------------------------------- */

/* Documents the double-inversion trap by asserting that the "standard" idiom is
 * wrong HERE.  If this ever starts matching, crc32_update() has changed its own
 * inversion and crc32.h's note (and this test) have to be revisited. */
static void test_no_double_inversion(void)
{
	uint32_t wrapped = ~crc32_update(0xFFFFFFFFu, "123456789", 9u);

	assert(wrapped != 0xCBF43926u);
	printf("  D. init-FFFFFFFF + final-complement is NOT the canonical value "
	       "(%08X) -- do not wrap\n", (unsigned)wrapped);
}

/* ---- E: agreement with the table-free reference --------------------------- */

static void test_against_bitwise(void)
{
	uint32_t i;

	/* Every table entry is selected by some low nibble, and every byte value
	 * exercises two of them, so the 256 single-byte inputs cover all 16 entries
	 * in both lookups. */
	for (i = 0u; i < 256u; i++) {
		uint8_t b = (uint8_t)i;
		uint32_t got = crc32_update(0u, &b, 1u);
		uint32_t ref = crc32_bitwise(0u, &b, 1u);

		if (got != ref) {
			printf("byte %02X: table %08X != bitwise %08X\n",
			       (unsigned)i, (unsigned)got, (unsigned)ref);
			assert(0);
		}
	}
	/* And over the whole stream, where a wrong entry that cancels out on short
	 * inputs cannot stay hidden. */
	assert(crc32_update(0u, stream, STREAM_LEN) ==
	       crc32_bitwise(0u, stream, STREAM_LEN));
	printf("  E. agrees with the table-free reference on all 256 bytes "
	       "and on the stream\n");
}

int main(void)
{
	printf("test_crc32 (svc/crc32.c):\n");
	fill_stream();
	test_vectors();
	test_chain_all_splits();
	test_chain_stream();
	test_no_double_inversion();
	test_against_bitwise();
	printf("test_crc32: all passed\n");
	return 0;
}
