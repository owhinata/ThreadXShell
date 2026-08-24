/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the NOR write request arithmetic (issue #88 Part C,
 * port/nor/nor_span.c).
 *
 * WHY THIS EXISTS.  cmake/check_nor_seam.py settles who may reach the vendor's
 * erase and program entry points, and test_nor_seam.c settles what the door
 * does when they are reached.  This settles the step BEFORE either: which bytes
 * a request names, and -- the part with teeth -- which bytes an erase actually
 * destroys, since it destroys a whole unit and not a byte.
 *
 * None of it can be produced on the board.  The writer is the only caller and
 * it is written to satisfy these rules, so an address one byte past the
 * interval, a length that rounds outside it, or a request near 2^32 exist here
 * or nowhere -- and each one that got through would cost a flash cycle of a
 * part whose endurance is not documented (issue #89), in a region holding the
 * bootloader.
 *
 * [!] THE ORDER OF THE TESTS IS THE SUBJECT, not an implementation detail.
 * Rounding before checking would round a value that has not been proved not to
 * overflow; checking before rounding, and then not re-checking, would bound the
 * wrong span.  The cases below walk both mistakes.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nor_span.h"

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

/* The real interval and unit the firmware is compiled with today.  Restated
 * here rather than taken from board.cmake on purpose: the point of the cases
 * below is the arithmetic, and a test whose numbers moved with the layout would
 * stop covering the edges it was written for.  test_flash_partitions.py and
 * check_nor_seam.py are what tie the firmware's copy to the layout. */
#define LO    0x00200000u
#define HI    0x00B7B000u
#define UNIT  0x00001000u

static void t_program_bounds(void)
{
	struct nor_span s;
	enum nor_span_verdict v;

	v = nor_span_program(LO, HI, LO, 1u, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == LO && s.len == 1u,
	      "one byte at the first writable address: %s",
	      nor_span_verdict_name(v));

	/* The last byte, and one past it.  HI is exclusive. */
	v = nor_span_program(LO, HI, HI - 1u, 1u, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == HI - 1u && s.len == 1u,
	      "last writable byte: %s", nor_span_verdict_name(v));
	v = nor_span_program(LO, HI, HI, 1u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "one past the end: %s",
	      nor_span_verdict_name(v));

	/* Exactly filling the interval, and one byte too many. */
	v = nor_span_program(LO, HI, LO, HI - LO, &s);
	CHECK(v == NOR_SPAN_OK && s.len == HI - LO, "the whole interval: %s",
	      nor_span_verdict_name(v));
	v = nor_span_program(LO, HI, LO, HI - LO + 1u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "the whole interval plus one: %s",
	      nor_span_verdict_name(v));

	/* Below the interval, and zero length. */
	v = nor_span_program(LO, HI, LO - 1u, 1u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "one below the start: %s",
	      nor_span_verdict_name(v));
	v = nor_span_program(LO, HI, LO, 0u, &s);
	CHECK(v == NOR_SPAN_EMPTY, "zero length: %s", nor_span_verdict_name(v));
}

/* [!] THE CASE THE SUBTRACTION EXISTS FOR.  `addr + len` for an address near
 * the top of the address space wraps to a small number, and a containment test
 * written as `addr + len <= hi` would call it contained.  The interval here is
 * the firmware's, so these addresses are outside it for two reasons at once --
 * which is exactly why the wrapping ones are checked against an interval that
 * ENDS at 2^32 as well, where "outside" has only one possible cause. */
static void t_program_wraparound(void)
{
	struct nor_span s;
	enum nor_span_verdict v;

	v = nor_span_program(LO, HI, 0xFFFFFF00u, 0x200u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "wrapping request: %s",
	      nor_span_verdict_name(v));

	/* An interval whose exclusive end IS 2^32, expressed as 0 -- refused as an
	 * unusable interval rather than silently treated as empty. */
	v = nor_span_program(0xFFFF0000u, 0u, 0xFFFFFF00u, 0x200u, &s);
	CHECK(v == NOR_SPAN_BAD_INTERVAL, "interval ending at 2^32: %s",
	      nor_span_verdict_name(v));

	/* And one that stops just short, where the only reason to refuse is the
	 * length: hi - addr is 0x100, the request is 0x200. */
	v = nor_span_program(0xFFFF0000u, 0xFFFFFF00u, 0xFFFFFE00u, 0x200u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "length past a high interval: %s",
	      nor_span_verdict_name(v));
	v = nor_span_program(0xFFFF0000u, 0xFFFFFF00u, 0xFFFFFE00u, 0x100u, &s);
	CHECK(v == NOR_SPAN_OK && s.len == 0x100u,
	      "length reaching a high interval's end: %s",
	      nor_span_verdict_name(v));
}

static void t_erase_rounding(void)
{
	struct nor_span s;
	enum nor_span_verdict v;

	/* Sixteen bytes in the middle of a sector destroy the sector. */
	v = nor_span_erase(LO, HI, UNIT, LO + 0x800u, 16u, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == LO && s.len == UNIT,
	      "16 B mid-sector -> one whole sector: %s %08x+%u",
	      nor_span_verdict_name(v), s.addr, s.len);

	/* One byte either side of a sector boundary destroys two. */
	v = nor_span_erase(LO, HI, UNIT, LO + UNIT - 1u, 2u, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == LO && s.len == 2u * UNIT,
	      "2 B across a boundary -> two sectors: %s %08x+%u",
	      nor_span_verdict_name(v), s.addr, s.len);

	/* An aligned request of whole units is not grown. */
	v = nor_span_erase(LO, HI, UNIT, LO + UNIT, 2u * UNIT, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == LO + UNIT && s.len == 2u * UNIT,
	      "aligned whole units are unchanged: %s %08x+%u",
	      nor_span_verdict_name(v), s.addr, s.len);

	/* The last sector, reached by a request that ends exactly at HI. */
	v = nor_span_erase(LO, HI, UNIT, HI - 1u, 1u, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == HI - UNIT && s.len == UNIT,
	      "the last byte -> the last sector: %s %08x+%u",
	      nor_span_verdict_name(v), s.addr, s.len);

	v = nor_span_erase(LO, HI, UNIT, HI, 1u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "the first byte above the interval: %s",
	      nor_span_verdict_name(v));
	v = nor_span_erase(LO, HI, UNIT, HI - 1u, 2u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "a length that reaches past the end: %s",
	      nor_span_verdict_name(v));

	/* [!] The rounded start must not escape below the interval either.  It
	 * cannot while LO is unit-aligned, and this is the case that would notice
	 * if the interval ever stopped being. */
	v = nor_span_erase(LO, HI, UNIT, LO, 1u, &s);
	CHECK(v == NOR_SPAN_OK && s.addr == LO,
	      "the first byte does not round below the interval: %s %08x",
	      nor_span_verdict_name(v), s.addr);

	v = nor_span_erase(LO, HI, UNIT, LO, 0u, &s);
	CHECK(v == NOR_SPAN_EMPTY, "zero length: %s", nor_span_verdict_name(v));
	v = nor_span_erase(LO, HI, UNIT, LO - 1u, 1u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "one below the start: %s",
	      nor_span_verdict_name(v));
}

/* An erase span is only bounded by an interval whose edges are whole units:
 * otherwise the first address in it rounds DOWN to somebody else's flash.  The
 * refusal is what makes that a property of the code rather than of the numbers
 * board.cmake happens to produce today. */
static void t_erase_interval_must_be_aligned(void)
{
	struct nor_span s;
	enum nor_span_verdict v;

	v = nor_span_erase(LO + 1u, HI, UNIT, LO + 1u, 1u, &s);
	CHECK(v == NOR_SPAN_BAD_INTERVAL, "unaligned start: %s",
	      nor_span_verdict_name(v));
	v = nor_span_erase(LO, HI - 1u, UNIT, LO, 1u, &s);
	CHECK(v == NOR_SPAN_BAD_INTERVAL, "unaligned end: %s",
	      nor_span_verdict_name(v));
	v = nor_span_erase(HI, LO, UNIT, LO, 1u, &s);
	CHECK(v == NOR_SPAN_BAD_INTERVAL, "inverted interval: %s",
	      nor_span_verdict_name(v));

	/* And a unit that is not a power of two has no rounding at all. */
	v = nor_span_erase(LO, HI, 0u, LO, 1u, &s);
	CHECK(v == NOR_SPAN_BAD_UNIT, "zero unit: %s", nor_span_verdict_name(v));
	v = nor_span_erase(LO, HI, 3000u, LO, 1u, &s);
	CHECK(v == NOR_SPAN_BAD_UNIT, "unit 3000: %s", nor_span_verdict_name(v));
}

/* [!] AND THE ERASE WRAPAROUND, which is the one that would destroy the wrong
 * flash rather than merely refuse the wrong request: rounding UP an address
 * near 2^32 is a second chance to wrap, after the addition. */
static void t_erase_wraparound(void)
{
	struct nor_span s;
	enum nor_span_verdict v;

	v = nor_span_erase(LO, HI, UNIT, 0xFFFFF800u, 0x1000u, &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "wrapping erase against the real interval: %s",
	      nor_span_verdict_name(v));

	/* An interval that runs to the top of a 32-bit space, unit aligned.  The
	 * request's end rounds up to exactly 2^32, which is 0 -- so this is where a
	 * `end + unit - 1 & ~(unit-1)` written without the containment proof in
	 * front of it would produce a span starting at 0xFFFFF000 with a length of
	 * 0x00001000 that "ends" below where it started. */
	v = nor_span_erase(0xFFF00000u, 0xFFFFF000u, UNIT, 0xFFFFE800u, 0x1000u,
	                   &s);
	CHECK(v == NOR_SPAN_OUTSIDE, "erase reaching the top of the space: %s",
	      nor_span_verdict_name(v));
	v = nor_span_erase(0xFFF00000u, 0xFFFFF000u, UNIT, 0xFFFFE800u, 0x700u,
	                   &s);
	CHECK(v == NOR_SPAN_OK && s.addr == 0xFFFFE000u && s.len == 0x1000u,
	      "erase ending exactly at a high interval's end: %s %08x+%u",
	      nor_span_verdict_name(v), s.addr, s.len);
}

static void t_page_chunks(void)
{
	/* An aligned start takes a whole page. */
	CHECK(nor_span_page_chunk(LO, 1024u, 256u) == 256u, "aligned page");
	/* An unaligned one stops at the boundary, and the next takes a page. */
	CHECK(nor_span_page_chunk(LO + 0xC0u, 1024u, 256u) == 0x40u,
	      "unaligned first chunk");
	CHECK(nor_span_page_chunk(LO + 0x100u, 1024u, 256u) == 256u,
	      "the chunk after it");
	/* Never more than what is left. */
	CHECK(nor_span_page_chunk(LO, 3u, 256u) == 3u, "shorter than a page");
	CHECK(nor_span_page_chunk(LO, 0u, 256u) == 0u, "nothing left");
	/* Never more than the caller's buffer. */
	CHECK(nor_span_page_chunk(LO, 1024u, 64u) == 64u, "capped by the buffer");
	/* [!] A zero cap returns zero rather than a page.  The writer treats that
	 * as terminal; a chunker that "helpfully" ignored the cap would hand a
	 * prebuilt archive a pointer past the end of the staging buffer. */
	CHECK(nor_span_page_chunk(LO, 1024u, 0u) == 0u, "zero cap");

	/* Walking a whole request must consume it exactly, from any offset. */
	for (uint32_t start = 0u; start < 512u; start += 37u) {
		uint32_t addr = LO + start, left = 1000u, steps = 0u;

		while (left > 0u) {
			uint32_t n = nor_span_page_chunk(addr, left, 256u);

			if (n == 0u) {
				CHECK(0, "chunk walk stalled at +%u", addr - LO);
				break;
			}
			CHECK(n <= 256u, "chunk longer than a page: %u", n);
			CHECK((addr % 256u) + n <= 256u,
			      "chunk crosses a page boundary at 0x%08x +%u", addr, n);
			addr += n;
			left -= n;
			steps++;
			CHECK(steps < 32u, "chunk walk did not terminate");
			if (steps >= 32u)
				break;
		}
	}
}

int main(void)
{
	t_program_bounds();
	t_program_wraparound();
	t_erase_rounding();
	t_erase_interval_must_be_aligned();
	t_erase_wraparound();
	t_page_chunks();

	if (failures) {
		printf("test_nor_write: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("test_nor_write: ok\n");
	return 0;
}
