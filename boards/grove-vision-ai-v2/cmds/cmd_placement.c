/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_placement.c
 * @brief   `placement` command: prove the 2nd bootloader loads a LOADABLE
 *          section into SRAM and into DTCM (issue #43).
 *
 * TEMPORARY -- this is an experiment, not a feature.  It exists to answer one
 * question before #29 moves `.rodata` out of ITCM: can a section with
 * CONTENTS actually be placed in the target region by the 2nd-stage
 * bootloader?  Once the destination is decided this whole file, its two
 * linker-script sections and its board.cmake entry come back out.
 *
 * WHY THIS CANNOT BE A BUILD-ONLY CHECK
 *
 * check_image_coherence.py cross-checks the ELF against the image generator's
 * per-section intermediate payload files, and for the final shell.img it
 * asserts only that the file exists and is non-empty.  Nothing in the build
 * observes what the 2nd-stage bootloader does at boot.  Generator and gates can
 * all pass while the loader drops the payload or writes it somewhere else.
 *
 * The stake is that `.rodata` carries the shell command registry (embedded
 * there on purpose -- a standalone output section name gets dropped by the
 * generator).  A failure that empties the registry removes the console that
 * would have reported it, so the real `.rodata` stays in ITCM until this
 * answers.
 *
 * WHAT IS ALREADY KNOWN
 *
 *   DTCM: proven.  Every loadable section in this image has LMA == VMA -- the
 *   linker script places .data with `> CM55M_S_APP_DATA` and no AT> clause --
 *   so there is no ITCM staging copy.  The copy table says so outright:
 *   src == dst == 0x30000000, i.e. the CMSIS startup copy is a no-op memcpy
 *   onto itself, and what actually fills DTCM is the loader writing .data at
 *   its VMA.  The `.dtcm_probe` below therefore tests only the part DTCM has
 *   left to prove: that the tool carries an ADDITIONAL section with a name it
 *   has not seen before.
 *
 *   SRAM: unproven.  Every section in the SRAM window today (.sram_bench,
 *   .lcd_fb, .cam_raw, .cam_slots) is NOLOAD.  Nothing loadable has ever been
 *   placed there, and the 2nd-stage bootloader itself executes out of the
 *   bottom of that window while it runs.
 *
 * THE PAYLOAD is position-dependent and non-compressible-ish: word i is
 * i * 1664525 + 1013904223 (Numerical Recipes' LCG constants, used here only as
 * a cheap generator, not for randomness).  Because it is a pure function of the
 * index, the checker regenerates the expectation instead of carrying a
 * precomputed digest -- so there is no build-time constant to drift.  A partial
 * load, a shifted load and a dropped load all surface as a first-mismatch
 * index rather than a bare pass/fail.
 *
 * THE CHECKER runs from ITCM (plain .text), so it never lives in the memory it
 * is testing.  CRC32 is bitwise on purpose: a 1 KB table would be the largest
 * thing this experiment adds to the image.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Payload
 * ------------------------------------------------------------------------- */

#define PROBE_WORD(i) ((uint32_t)((uint32_t)(i) * 1664525u + 1013904223u))

/* Doubling ladder to 4096 words = 16 KB per probe.  Sized to be the same order
 * as the 29,904 B .rodata this is standing in for -- big enough that a
 * truncated or misplaced load cannot hide, small enough to keep the DTCM
 * heap..stack gap comfortable (it drops from 172,608 B to ~156 KB). */
#define P2(i)    PROBE_WORD(i), PROBE_WORD((i) + 1)
#define P4(i)    P2(i), P2((i) + 2)
#define P8(i)    P4(i), P4((i) + 4)
#define P16(i)   P8(i), P8((i) + 8)
#define P32(i)   P16(i), P16((i) + 16)
#define P64(i)   P32(i), P32((i) + 32)
#define P128(i)  P64(i), P64((i) + 64)
#define P256(i)  P128(i), P128((i) + 128)
#define P512(i)  P256(i), P256((i) + 256)
#define P1024(i) P512(i), P512((i) + 512)
#define P2048(i) P1024(i), P1024((i) + 1024)
#define P4096(i) P2048(i), P2048((i) + 2048)

#define PROBE_WORDS 4096u

/* The two probes run from DIFFERENT index bases, so their payloads differ.
 * Identical content would hide the one failure mode that matters most here:
 * a loader that writes the right bytes to the wrong section's address, or the
 * same section's bytes to both.  With distinct bases a swap shows up as a
 * mismatch at word 0 rather than as two clean passes. */
#define SRAM_PROBE_BASE 0x00000000u
#define DTCM_PROBE_BASE 0x00010000u

/* `used` stops the compiler dropping them; the linker script KEEPs the input
 * sections so --gc-sections cannot either.  const, so a stray write faults
 * rather than quietly rewriting the thing under test. */
static const uint32_t sram_probe_buf[PROBE_WORDS]
	__attribute__((used, section(".sram_probe"), aligned(32))) =
		{ P4096(SRAM_PROBE_BASE) };

static const uint32_t dtcm_probe_buf[PROBE_WORDS]
	__attribute__((used, section(".dtcm_probe"), aligned(32))) =
		{ P4096(DTCM_PROBE_BASE) };

/* ---------------------------------------------------------------------------
 * Checker
 * ------------------------------------------------------------------------- */

struct probe_result {
	uint32_t first_bad;     /**< index of first mismatch, or PROBE_WORDS */
	uint32_t got;           /**< value read there */
	uint32_t want;          /**< value expected there */
	uint32_t crc;           /**< CRC32 of the whole buffer, as a fingerprint */
};

/* Bitwise CRC32 (reflected, poly 0xEDB88320) -- the standard zlib parameters,
 * so the value can be reproduced on the host against the ELF section bytes. */
static uint32_t probe_crc32(const uint32_t *p, uint32_t words)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = p[w];

		for (unsigned b = 0; b < 4u; b++) {
			crc ^= (v >> (8u * b)) & 0xFFu;
			for (unsigned k = 0; k < 8u; k++)
				crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
		}
	}
	return ~crc;
}

static void probe_check(const uint32_t *p, uint32_t words, uint32_t base,
                        struct probe_result *r)
{
	r->first_bad = words;
	r->got       = 0;
	r->want      = 0;

	for (uint32_t i = 0; i < words; i++) {
		uint32_t want = PROBE_WORD(base + i);

		if (p[i] != want) {
			r->first_bad = i;
			r->got       = p[i];
			r->want      = want;
			break;
		}
	}
	r->crc = probe_crc32(p, words);
}

static void probe_report(struct cli_instance *sh, const char *what,
                         const uint32_t *p, uint32_t words, uint32_t base)
{
	struct probe_result r;

	probe_check(p, words, base, &r);

	cli_print(sh, "%-6s %p..%p  %u B\r\n", what, (const void *)p,
	          (const void *)(p + words), (unsigned)(words * 4u));
	cli_print(sh, "       first=0x%08lx last=0x%08lx crc32=0x%08lx\r\n",
	          (unsigned long)p[0], (unsigned long)p[words - 1u],
	          (unsigned long)r.crc);

	if (r.first_bad == words) {
		cli_print(sh, "       OK: all %u words match\r\n", (unsigned)words);
	} else {
		cli_error(sh,
		          "       MISMATCH at word %u: got 0x%08lx want 0x%08lx\r\n",
		          (unsigned)r.first_bad, (unsigned long)r.got,
		          (unsigned long)r.want);
	}
}

static int cmd_placement(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* The expectation is regenerated from the index, so a matching report means
	 * the loader placed these bytes -- not merely that something is there. */
	cli_print(sh, "loadable-section placement probe (issue #43)\r\n");
	probe_report(sh, "SRAM", sram_probe_buf, PROBE_WORDS, SRAM_PROBE_BASE);
	probe_report(sh, "DTCM", dtcm_probe_buf, PROBE_WORDS, DTCM_PROBE_BASE);

	return 0;
}

CLI_CMD_REGISTER(placement, NULL,
                 "verify the loader placed the SRAM/DTCM probe sections (#43)",
                 cmd_placement, 1, 0);
