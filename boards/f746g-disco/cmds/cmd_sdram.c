/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_sdram.c
 * @brief   `sdram` shell command: FMC SDRAM status + memtest
 * (owhinata/stm32f746g-disco#40).
 *
 *   sdram info           base / size / controller state
 *   sdram test [bytes]   DESTRUCTIVE write/read-back memtest (default: all 8 MB)
 *
 * The test overwrites the tested span -- everything placed in the `.sdram`
 * linker section (the camera frame buffer, owhinata/stm32f746g-disco#41) is
 * clobbered, so the test first claims it from the camera with
 * camera_scratch_claim() (issue #150): the claim drops the captured-frame flag
 * and refuses every capture and stream start until the test releases it, so
 * `camera save`/`send` cannot ship test patterns believing them to be a frame.
 *
 * The rest of `.sdram` has three more owners the camera claim does not cover
 * (issue #152):
 *   - ltdc_fb (bank0): the LTDC scan-out surface and the shell's draw target.
 *     The test holds it from port/ltdc with ltdc_scanout_hold(): scan-out
 *     stops, and `lcd on`, `gui start`, the `lcd` draws and flips are refused
 *     until ltdc_scanout_unhold() repaints it black and restores scan-out.
 *   - `.sdram.ai` (bank3): the NN arena, model slots and decoder scratch.  The
 *     test claims it from its owner, port/nn, with nn_scratch_claim() for its
 *     whole run: no nn session can be taken (`nn model load` / `unload`, `nn
 *     bench`, `nn run`, `nn stream start` are refused as busy) and the first
 *     open after boot builds nothing.  The claim is refused while a model is
 *     loaded -- a model left "loaded" over test patterns would run on garbage
 *     -- and the test does not unload one itself.
 *   - `.sdram.eth` (bank2): the ETH DMA descriptors and the NetX pool.  The IP
 *     instance never goes down once created, so instead of refusing, the test
 *     skips the whole of bank2 while the network is up and says so.
 *   - The MJPEG server reads the camera stream, so the camera claim already
 *     excludes it; it needs no guard of its own.
 *
 * Three passes over the span, word-wise: (1) address pattern (each word holds
 * its own address -- catches stuck/shorted/aliased address lines, which a
 * single repeated value would miss on a 16-bit-bus part), (2) 0x55555555,
 * (3) 0xAAAAAAAA (alternating bits both ways -- catches stuck/coupled data
 * lines).  Each pass writes the full span, then reads it back: a full-span
 * write pass pushes every row out of the SDRAM device's open-row latches and
 * exercises refresh over the multi-ms gap before read-back.  The region is
 * MPU-mapped non-cacheable, so reads hit the device (no cache masking).
 *
 * Cancellable between 64 KB chunks (Ctrl+C); progress is printed per MB.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "sdram.h"
#include "camera.h"        /* camera_scratch_claim / camera_scratch_release   */
#include "ltdc_display.h"  /* ltdc_scanout_hold / ltdc_scanout_unhold           */
#include "nn.h"            /* nn_scratch_claim / nn_scratch_release            */
#include "nx_glue.h"       /* nx_net_is_up: bank2 holds live ETH DMA state     */

#include <stdint.h>

/* Cancellation/progress granularity: 16K words = 64 KB per chunk. */
#define SDRAM_TEST_CHUNK_WORDS  (16u * 1024u)

/* `.sdram.eth` as the linker placed it (ldscript), checked against the bank2
   window sdram.h names before that window is skipped. */
extern uint8_t _ssdram_eth[], _esdram_eth[];

/* One contiguous span of words under test.  The span [base, base+bytes) is at
   most two of these: below and above bank2 when bank2 is skipped. */
struct test_range {
	volatile uint32_t *base;
	uint32_t           words;
};

static int parse_u32(const char *s, uint32_t *out)
{
	uint32_t base = 10;
	uint32_t val = 0;
	const char *p = s;

	if (p == NULL || *p == '\0')
		return -1;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
		if (*p == '\0')
			return -1;
	}
	for (; *p != '\0'; p++) {
		uint32_t digit;
		char c = *p;

		if (c >= '0' && c <= '9')
			digit = (uint32_t)(c - '0');
		else if (base == 16 && c >= 'a' && c <= 'f')
			digit = (uint32_t)(c - 'a' + 10);
		else if (base == 16 && c >= 'A' && c <= 'F')
			digit = (uint32_t)(c - 'A' + 10);
		else
			return -1;
		if (val > (0xFFFFFFFFu - digit) / base)
			return -1;
		val = val * base + digit;
	}
	*out = val;
	return 0;
}

static int cmd_sdram_info(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_print(sh, "device:  MT48LC4M32B2 (128 Mbit, 16-bit bus -> 8 MB usable)\r\n");
	cli_print(sh, "window:  0x%08lx .. 0x%08lx (FMC bank1)\r\n",
	          (unsigned long)SDRAM_BASE_ADDR,
	          (unsigned long)(SDRAM_BASE_ADDR + SDRAM_SIZE_BYTES - 1u));
	cli_print(sh, "config:  SDCLK 108 MHz (HCLK/2), CAS 3, refresh 0x0603\r\n");
	cli_print(sh, "MPU:     Normal, non-cacheable, XN (DMA-coherent)\r\n");
	cli_print(sh, "state:   %s\r\n", sdram_is_up() ? "up" : "DOWN (init failed)");
	return 0;
}

/* One full write pass over every range, then one full read-back pass over
   every range (so the gap before read-back still spans the whole test).
   pattern==0 selects the address-in-word pattern.  Returns 0, 1 with the
   failing word's address in *fail_addr, or -1 on cancel. */
static int test_pass(struct cli_instance *sh, const struct test_range *r,
                     unsigned nr, uint32_t pattern, uint32_t *fail_addr,
                     uint32_t *fail_got)
{
	uint32_t i, chunk;

	for (unsigned k = 0; k < nr; k++) {
		volatile uint32_t *base = r[k].base;
		uint32_t words = r[k].words;

		for (i = 0; i < words; i += chunk) {
			chunk = (words - i > SDRAM_TEST_CHUNK_WORDS)
			        ? SDRAM_TEST_CHUNK_WORDS : (words - i);
			for (uint32_t j = i; j < i + chunk; j++)
				base[j] = pattern ? pattern
				                  : (uint32_t)(uintptr_t)&base[j];
			if (cli_cancel_requested(sh))
				return -1;
		}
	}

	for (unsigned k = 0; k < nr; k++) {
		volatile uint32_t *base = r[k].base;
		uint32_t words = r[k].words;

		for (i = 0; i < words; i += chunk) {
			chunk = (words - i > SDRAM_TEST_CHUNK_WORDS)
			        ? SDRAM_TEST_CHUNK_WORDS : (words - i);
			for (uint32_t j = i; j < i + chunk; j++) {
				uint32_t want = pattern ? pattern
				                        : (uint32_t)(uintptr_t)&base[j];
				uint32_t got = base[j];

				if (got != want) {
					*fail_addr = (uint32_t)(uintptr_t)&base[j];
					*fail_got = got;
					return 1;
				}
			}
			if (cli_cancel_requested(sh))
				return -1;
		}
	}
	return 0;
}

static int cmd_sdram_test(struct cli_instance *sh, int argc, char **argv)
{
	static const uint32_t patterns[] = { 0u /* address */, 0x55555555u,
	                                     0xAAAAAAAAu };
	const uint32_t hole_lo = SDRAM_ETH_BANK_ADDR;
	const uint32_t hole_hi = SDRAM_ETH_BANK_ADDR + SDRAM_BANK_BYTES;
	struct test_range ranges[2];
	unsigned nr = 0;
	uint32_t bytes = SDRAM_SIZE_BYTES;
	uint32_t end, tested, fail_addr, fail_got;
	int rc;
	int ret = 0;            /* function result (set before goto restore) */
	bool skip_eth;

	if (!sdram_is_up()) {
		cli_error(sh, "sdram: not initialized\r\n");
		return 1;
	}

	if (argc > 1) {
		if (parse_u32(argv[1], &bytes) != 0 || bytes == 0u ||
		    bytes > SDRAM_SIZE_BYTES || (bytes & 3u) != 0u) {
			cli_error(sh, "sdram: bad length (4-byte multiple, "
			              "4..%lu)\r\n",
			          (unsigned long)SDRAM_SIZE_BYTES);
			return 1;
		}
	}
	end = SDRAM_BASE_ADDR + bytes;

	/* [!] The camera's frame buffer and stream ring live in .sdram (issue
	   #150).  Claim them first, for the whole run: the claim is taken under
	   the camera's mutex, refuses while a stream runs or is held, drops the
	   captured-frame flag, and until the release below every capture and
	   stream start (from any console, and nn's) is refused with BUSY -- so
	   nothing can DMA into the ring or re-validate the frame over the test's
	   patterns.  Released on every exit from here on. */
	if (camera_scratch_claim() != 0) {
		cli_error(sh, "sdram: the camera is streaming into .sdram, or "
		              "another sdram test holds it; run 'camera stream "
		              "stop' / 'nn stream stop' first\r\n");
		return 1;
	}

	/* [!] The NN arena, model slots and decoder scratch live in .sdram.ai
	   (issue #152).  Claim it from its owner for the whole run, as the camera
	   claim above does for its buffers.  nn_scratch_claim() asks and sets in
	   one critical section (port/nn/nn.h): refused while any nn command or
	   stream holds the session or the first open is building (-1), or while a
	   model is loaded (-2).  Once held, no session can be taken and no open,
	   load region or reload puts anything into .sdram.ai until the release
	   below.  It never unloads. */
	rc = nn_scratch_claim();
	if (rc == -2) {
		cli_error(sh, "sdram: a model is loaded in .sdram.ai; run "
		              "`nn model unload` first\r\n");
		ret = 1;
		goto release_camera;
	}
	if (rc != 0) {
		cli_error(sh, "sdram: nn is busy; wait for the nn command / stream "
		              "to finish\r\n");
		ret = 1;
		goto release_camera;
	}

	/* [!] .sdram.eth (bank2) holds the ETH DMA descriptors and the NetX pool,
	   which the MAC DMA and the IP thread use for as long as the IP instance
	   exists -- and it never goes down once nx_net_init() created it.  So
	   while it is up the test skips the whole bank instead of refusing.  The
	   skipped window comes from sdram.h; fail closed if the linker placed
	   .sdram.eth anywhere outside it. */
	skip_eth = nx_net_is_up();
	if (skip_eth &&
	    ((uint32_t)(uintptr_t)_ssdram_eth < hole_lo ||
	     (uint32_t)(uintptr_t)_esdram_eth > hole_hi)) {
		cli_error(sh, "sdram: .sdram.eth (0x%08lx..0x%08lx) is not inside "
		              "bank2; refusing\r\n",
		          (unsigned long)(uintptr_t)_ssdram_eth,
		          (unsigned long)(uintptr_t)_esdram_eth);
		ret = 1;
		goto release;
	}
	if (skip_eth && end > hole_lo) {
		ranges[nr].base  = (volatile uint32_t *)SDRAM_BASE_ADDR;
		ranges[nr].words = (hole_lo - SDRAM_BASE_ADDR) / 4u;
		nr++;
		if (end > hole_hi) {
			ranges[nr].base  = (volatile uint32_t *)hole_hi;
			ranges[nr].words = (end - hole_hi) / 4u;
			nr++;
		}
	} else {
		ranges[nr].base  = (volatile uint32_t *)SDRAM_BASE_ADDR;
		ranges[nr].words = bytes / 4u;
		nr++;
		skip_eth = false;       /* nothing to skip below bank2 */
	}
	tested = 0u;
	for (unsigned k = 0; k < nr; k++)
		tested += ranges[k].words * 4u;

	/* [!] The test overwrites all of .sdram, including ltdc_fb (the LTDC
	   scan-out READ surface and the shell's draw target, pinned to bank0 --
	   owhinata/stm32f746g-disco#65).  Hold the display from its owner,
	   port/ltdc (issue #152): refused while GUIX owns it; otherwise scan-out
	   is stopped under the LTDC lock before the first write below, and until
	   the unhold `lcd on` and `gui start` are refused, the `lcd` draw commands
	   draw nothing and flips are refused -- nothing but this test writes the
	   framebuffers.  `lcd off` meanwhile only keeps the display off after. */
	rc = ltdc_scanout_hold();
	if (rc == LTDC_ERR_STATE) {
		cli_error(sh, "sdram: LTDC/GUIX owns the display; run 'gui stop' first\r\n");
		ret = 1;
		goto release;
	}
	if (rc != LTDC_OK) {
		cli_error(sh, "sdram: could not hold the display\r\n");
		ret = 1;
		goto release;
	}

	cli_warn(sh, "sdram: DESTRUCTIVE test over %lu KB "
	             "(clobbers .sdram contents, e.g. a captured frame)\r\n",
	         (unsigned long)(tested / 1024u));
	if (skip_eth)
		cli_print(sh, "sdram: bank2 (0x%08lx..0x%08lx) skipped: the network "
		              "is up (ETH DMA descriptors live there)\r\n",
		          (unsigned long)hole_lo, (unsigned long)(hole_hi - 1u));

	for (unsigned p = 0; p < sizeof patterns / sizeof patterns[0]; p++) {
		const char *name = (p == 0) ? "address" :
		                   (patterns[p] == 0x55555555u) ? "0x55555555"
		                                                : "0xAAAAAAAA";

		cli_print(sh, "pass %u/3 (%s): write+verify %lu KB ... ",
		          p + 1u, name, (unsigned long)(tested / 1024u));
		rc = test_pass(sh, ranges, nr, patterns[p], &fail_addr, &fail_got);
		if (rc < 0) {
			cli_print(sh, "cancelled\r\n");
			ret = 1;
			goto restore;
		}
		if (rc > 0) {
			cli_print(sh, "FAIL\r\n");
			cli_error(sh, "sdram: mismatch at 0x%08lx: got 0x%08lx, "
			              "want 0x%08lx\r\n",
			          (unsigned long)fail_addr,
			          (unsigned long)fail_got,
			          (unsigned long)(patterns[p] ? patterns[p]
			                                      : fail_addr));
			ret = 1;
			goto restore;
		}
		cli_print(sh, "OK\r\n");
	}

	cli_print(sh, "sdram: %lu KB tested, no errors\r\n",
	          (unsigned long)(tested / 1024u));
	ret = 0;

restore:
	/* Single postcondition (owhinata/stm32f746g-disco#65): the unhold repaints
	   both buffers black and restores scan-out to what it was before the hold
	   (off if it was off, or if `lcd off` ran during the test). */
	if (ltdc_scanout_unhold() != LTDC_OK)
		cli_warn(sh, "sdram: LTDC scanout restore failed; run 'lcd on'\r\n");
release:
	/* Released in the reverse order of the claims: .sdram.ai, then the
	   camera.  No model was loaded, so none is left over the patterns; the
	   next nn command builds (first open) or loads one normally. */
	nn_scratch_release();
release_camera:
	/* The frame stays invalid: only a new capture brings one back. */
	camera_scratch_release();
	return ret;
}

CLI_SUBCMD_SET_CREATE(sdram_subcmds,
	CLI_CMD(info, NULL, "FMC SDRAM window / config / state", cmd_sdram_info),
	CLI_CMD_ARG_USAGE(test, NULL, "DESTRUCTIVE write/read-back memtest [bytes]",
	                  "[bytes]", cmd_sdram_test, 1, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(sdram, sdram_subcmds, "external FMC SDRAM (8 MB @0xC0000000)",
                 NULL, 1, 0);
