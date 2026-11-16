/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_sdram.c
 * @brief   This board's table for the shared `sdram` (shell/cmds/cmd_ext_ram.c,
 *          issue #251): the FMC SDRAM's ranges and the steps a test takes
 *          (owhinata/stm32f746g-disco#40 until then: the whole command).
 *
 *   sdram info           the part, the window, its ranges, the state, the
 *                        controller's setup
 *   sdram test [bytes]   DESTRUCTIVE write / read-back test (default and at
 *                        most 6 MB: banks 0..2)
 *
 * THE RANGES, as src/bsp.c sets the MPU (region 0 maps the 8 MB Normal
 * non-cacheable, region 1 maps bank3 write-back on top of it, and on the
 * stedgeai_reloc backend region 2 the upper half of bank3 too) and as the
 * linker script places the banks:
 *   - banks 0..1 (0xC0000000, 4 MB), non-cacheable: the LCD's two buffers,
 *     `membench`'s 64 KB, the camera's captured frame and GUIX's camera
 *     screen (bank0), the camera stream's ring (bank1);
 *   - bank2 (0xC0400000, 2 MB), non-cacheable: the ETH DMA descriptors, the
 *     NetX pool and the MJPEG server's buffer (`.sdram.eth`);
 *   - bank3 (0xC0600000, 2 MB), write-back cacheable: nn's arena and model
 *     slots (`.sdram.ai`).  [!] A test never reaches it (the shared command
 *     stops at the first cacheable range), so since issue #251 a test no
 *     longer takes nn's claim on it: nothing of nn is in banks 0..2.
 *
 * THE STEPS, in this order (given back in the reverse one):
 *   1. the camera: camera_scratch_claim() (issue #150) -- refused while a
 *      stream runs or is held, or while another test holds it; drops the
 *      captured-frame flag and refuses every capture and stream start;
 *   2. the SDRAM owner word (sdram_owner.h): refused while `membench` runs;
 *   3. the network: while the IP instance is up -- it never goes down once
 *      created -- bank2 is skipped for this test (issue #152), after checking
 *      that the linker put `.sdram.eth` inside bank2 (refused otherwise, with
 *      where it is);
 *   4. the display: ltdc_scanout_hold() (issue #152) -- refused while GUIX
 *      owns it; stops scan-out, and until the give `lcd on`, `gui start`, the
 *      `lcd` draws and flips are refused.  The give repaints both buffers
 *      black and restores scan-out to what it was.
 *
 * NOT STOPPED BY ANY OF THIS: `devmem` can write anywhere in the SDRAM, and
 * looks at neither the test nor `membench` (README).
 *
 * Clean-room design; no third-party code reused.
 */
#include "ext_ram_cmd_board.h"
#include "sdram_owner.h"
#include "sdram.h"
#include "camera.h"        /* camera_scratch_claim / camera_scratch_release */
#include "ltdc_display.h"  /* ltdc_scanout_hold / ltdc_scanout_unhold         */
#include "nx_glue.h"       /* nx_net_is_up: bank2 holds live ETH DMA state    */

#include "stm32f7xx_hal.h" /* __get_PRIMASK / __disable_irq / __set_PRIMASK  */

#include <stddef.h>
#include <stdint.h>

/* `.sdram.eth` as the linker placed it (ldscript), checked against bank2
 * before bank2 is skipped. */
extern uint8_t _ssdram_eth[], _esdram_eth[];

#define SDRAM_ETH_OFF  (SDRAM_ETH_BANK_ADDR - SDRAM_BASE_ADDR)

/* ---- who uses the SDRAM for a test or a benchmark (sdram_owner.h) ---------- */

static volatile uint8_t sdram_owner_word;

enum sdram_owner sdram_owner_take(enum sdram_owner who)
{
	uint32_t pm = __get_PRIMASK();
	enum sdram_owner held;

	__disable_irq();
	held = (enum sdram_owner)sdram_owner_word;
	if (held == SDRAM_OWNER_NONE)
		sdram_owner_word = (uint8_t)who;
	__set_PRIMASK(pm);
	return held;
}

void sdram_owner_give(void)
{
	sdram_owner_word = (uint8_t)SDRAM_OWNER_NONE;
}

/* ---- the steps ------------------------------------------------------------ */

static int take_camera(struct ext_ram_claim *c)
{
	if (camera_scratch_claim() == 0)
		return 0;
	c->why = "the camera is streaming, or another test holds it; "
	         "`nn stream stop`, then `camera stream stop`";
	return -1;
}

static const char *give_camera(void)
{
	/* The frame stays invalid: only a new capture brings one back. */
	camera_scratch_release();
	return NULL;
}

static int take_owner(struct ext_ram_claim *c)
{
	enum sdram_owner held = sdram_owner_take(SDRAM_OWNER_TEST);

	if (held == SDRAM_OWNER_NONE)
		return 0;
	/* Say who holds it: the camera claim does not refuse a second test when
	 * the camera never came up, so it can be another test too. */
	c->why = (held == SDRAM_OWNER_TEST) ? "another `sdram test` is running"
	                                    : "`membench` is running";
	return -1;
}

static const char *give_owner(void)
{
	sdram_owner_give();
	return NULL;
}

static int take_network(struct ext_ram_claim *c)
{
	uint32_t lo = (uint32_t)(uintptr_t)_ssdram_eth;
	uint32_t hi = (uint32_t)(uintptr_t)_esdram_eth;

	if (!nx_net_is_up())
		return 0;
	if (lo < SDRAM_ETH_BANK_ADDR || hi > SDRAM_ETH_BANK_ADDR + SDRAM_BANK_BYTES) {
		c->why = ".sdram.eth is not inside bank2";
		c->span_valid = 1u;
		c->lo = lo;
		c->hi = hi;
		return -1;
	}
	c->skip_off = SDRAM_ETH_OFF;
	c->skip_len = SDRAM_BANK_BYTES;
	c->skip_why = "the network is up (ETH DMA)";
	return 0;
}

static int take_display(struct ext_ram_claim *c)
{
	int rc = ltdc_scanout_hold();

	if (rc == LTDC_OK)
		return 0;
	c->why = (rc == LTDC_ERR_STATE) ? "GUIX owns the display; `gui stop` first"
	                                : "could not hold the display";
	return -1;
}

static const char *give_display(void)
{
	return (ltdc_scanout_unhold() == LTDC_OK)
	       ? NULL : "display restore failed; `lcd on`";
}

static const struct ext_ram_step sdram_steps[] = {
	{ take_camera,  give_camera  },
	{ take_owner,   give_owner   },
	{ take_network, NULL         },
	{ take_display, give_display },
};

/* ---- the table ------------------------------------------------------------ */

static const struct ext_ram_range sdram_ranges[] = {
	{ 0u,            2u * SDRAM_BANK_BYTES, 0u, "banks 0-1: LCD, camera, membench" },
	{ SDRAM_ETH_OFF, SDRAM_BANK_BYTES,      0u,
	  "bank2: ETH, MJPEG; skipped while the network is up" },
	{ SDRAM_ETH_OFF + SDRAM_BANK_BYTES, SDRAM_BANK_BYTES, 1u, "bank3: nn" },
};

static int sdram_info_lines(ext_ram_write_fn write, void *ctx)
{
	static const char line[] =
		"config:  SDCLK 108 MHz (HCLK/2), CAS 3, refresh 0x0603\r\n";

	return (write(ctx, line, sizeof line - 1u) < 0) ? -1 : 0;
}

const struct ext_ram_board ext_ram_board = {
	.part     = "MT48LC4M32B2 (128 Mbit, 16-bit bus -> 8 MB usable)",
	.base     = SDRAM_BASE_ADDR,
	.size     = SDRAM_SIZE_BYTES,
	.ranges   = sdram_ranges,
	.n_ranges = sizeof sdram_ranges / sizeof sdram_ranges[0],
	.steps    = sdram_steps,
	.n_steps  = sizeof sdram_steps / sizeof sdram_steps[0],
	.ready    = sdram_is_up,
	.info     = sdram_info_lines,
};
