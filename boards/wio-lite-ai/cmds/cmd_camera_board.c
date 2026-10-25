/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    cmd_camera_board.c
 * @brief   This board's half of the `camera` command: the raw SCCB tools, and
 *          the shell-layer services the shared command needs
 *          (owhinata/wio-lite-ai#8, issue #143).
 *
 * The command itself -- probe / on / off / info / capture / save / send /
 * stream -- is shell/cmds/cmd_camera.c, shared with the other boards, over this
 * board's adapter port/camera/camera_svc_wio.c.  What stays here is appended to
 * that table by CAMERA_SVC_BOARD_SUBCMDS (svc/camera_svc_config.h):
 *
 *   camera scan           list every 7-bit SCCB address that ACKs
 *   camera reg <a> <r> .. raw SCCB register read / write
 *
 * plus camera_board_send() (YMODEM, with this board's `rz -y -b` reminder) and,
 * with the microSD built in, camera_board_medium() (shell/cmds/
 * camera_cmd_board.h).  The notes below are about reading the capture report,
 * which camera_svc_wio.c now writes.
 *
 * `camera probe` proves the sensor-control side: a chip ID means XCLK is at a
 * frequency the sensor accepts, PWDN and RESETB have the polarity assumed, and
 * I2C4 is talking at the right bit rate -- four things at once.  `camera capture`
 * proves the pixel side; read its channel statistics, not just its exit status.
 * Its `seam:` line reports the largest step between adjacent rows' channel means:
 * a settled frame is well under 1, and anything in the tens is a gain landing
 * mid-readout.
 *
 * READ THE RATIO BETWEEN THE THREE CHANNELS, NOT JUST dR (owhinata/wio-lite-ai#44).  A
 * gain landing mid-readout lands once, at a varying size.  A DEAD DVP DATA LINE gives
 * the same step size in EVERY frame at a random row, and the ratio between the
 * channels names the line, because a DVP bit sits at a fixed weight in each
 * channel.  With the byte order this driver uses (IMAGE_MODE bit 0 set, so the
 * bytes land in arrival order and a pixel reads little-endian):
 *
 *     high byte  R4 R3 R2 R1 R0 G5 G4 G3      low byte  G2 G1 G0 B4 B3 B2 B1 B0
 *
 *     line   dR    dG    dB          line   dR    dG    dB
 *     D7     16     4     0          D3      1     0     8
 *     D6      8     2     0          D2      0    32     4
 *     D5      4     1     0          D1      0    16     2
 *     D4      2     0    16          D0      0     8     1
 *
 * owhinata/wio-lite-ai#44 was `dR 8.00  dG 2.05  dB 0.02` in every single capture:
 * DCMI_D6 (PE5, ball C3 -> FPC-24 pin 11) was open inside the camera module, so that
 * one bit was sampled off a floating pin.  It read as a constant per row, drifting
 * between the rails every few rows, which is a horizontal stripe -- and since the
 * bit is only 8/31 of R and 2/63 of G, the picture still looked broadly right.
 *
 * NOTHING ELSE CAN SEE THIS.  `ovr dcmi`, `dma fe` and the then frame stream's
 * ring counters all stayed at zero throughout: neither the DCMI nor the DMA has any way to know
 * that one data bit is lying.  Every counter reading healthy is not evidence that
 * the pixels arrived intact -- this line is.  Swapping the camera module took
 * `dR` from 8.00 to 0.05.
 *
 * The snapshot lives in the PSRAM, and the preview of a stream writes the LTDC
 * frame buffer there, so `capture`, `save`, `send` and `stream start` all take
 * the SHARED OCTOSPI1 guard -- which keeps out a command that is
 * reconfiguring the bus, but not the LTDC scanning out of it.  That distinction
 * is what lets the preview put a live image on the display; the adapter
 * (port/camera/camera_svc_wio.c) holds it, and src/psram.h says why.
 *
 * `xclk` and `tune` used to sit here: seven run-time knobs for board facts the
 * schematic could not settle (XCLK rate, PWDN/RESETB polarity, SCCB pull-ups and
 * bit rate, the read style, the DVP byte swap, the warm-up count).  They existed
 * because the internal flash is rated for ~10k erase cycles and sweeping an
 * unknown by reflashing burns them -- the lesson from owhinata/wio-lite-ai#7's LCD
 * bring-up.  The board answered every one of them and has not changed its mind since,
 * so phase 3c-2 folded the answers into constants and deleted the knobs.  `reg`
 * stays: poking the sensor's registers is still how you learn anything about it.
 *
 * Note `camera off` does NOT remove power: the camera's 2V8 rail comes off a
 * fixed LDO with no enable pin (schematic U8), so the deepest state reachable
 * is PWDN asserted + RESETB low + XCLK stopped.
 *
 * Clean-room design; no third-party code reused.
 */
#include "camera.h"
#include "camera_cmd_board.h"
#include "camera_svc_config.h" /* CAMERA_SVC_BOARD_DECLS: the names declared */
#include "cli.h"
#if BSP_ENABLE_SD
#include "fs_cmd_core.h"  /* struct fs_device */
#include "fs_devices.h"   /* fs_sd_device(): the microSD media + its ownership gates */
#endif
#include "cmd_xfer.h"     /* xfer_send_source(): YMODEM over the console */

#include <stdint.h>
#include <string.h>

/* What the shared table expands; declared once, there and here. */
CAMERA_SVC_BOARD_DECLS

#define CAM_SCAN_MAX 16u

static void report(struct cli_instance *sh, int rc)
{
	if (rc != CAM_OK)
		cli_error(sh, "camera: %s\r\n", cam_strerror(rc));
}

/* ---- the shell-layer services (shell/cmds/camera_cmd_board.h) ------------ */

/*
 * On the PC side use `rz -y -b`, and `ffmpeg -y` to render.  Without those flags
 * neither tool overwrites an existing file -- YMODEM block 0 carries a name and a
 * size but no mtime, so rz has nothing to compare -- and you quietly keep looking
 * at the PREVIOUS capture, which is indistinguishable from a camera that has
 * stopped producing new frames.  That is exactly what happened during bring-up.
 */
int camera_board_send(struct cli_instance *sh, const struct ym_source *src)
{
	cli_print(sh, "camera: receive with `rz -y -b` -- without -y an existing "
	              "file is kept and you get the previous frame\r\n");
	return xfer_send_source(sh, src);
}

#if BSP_ENABLE_SD
const struct fs_device *camera_board_medium(const char *name)
{
	return (strcmp(name, "sd") == 0) ? fs_sd_device() : NULL;
}
#endif

/* ---- the SCCB tools -------------------------------------------------------- */

int camera_board_cmd_scan(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t addrs[CAM_SCAN_MAX];
	unsigned found = 0u;
	int rc;

	(void)argc;
	(void)argv;
	rc = camera_scan(addrs, CAM_SCAN_MAX, &found);
	if (rc != CAM_OK) {
		if (rc == CAM_ERR_STATE)
			cli_error(sh, "camera: not powered -- run `camera on` first "
			              "(SCCB needs XCLK)\r\n");
		else
			report(sh, rc);
		return 1;
	}
	if (found == 0u) {
		cli_print(sh, "camera: no device ACKed on SCCB (0x08..0x77)\r\n");
		return 0;
	}
	cli_print(sh, "camera: %u device%s:", found, (found == 1u) ? "" : "s");
	for (unsigned i = 0u; i < found && i < CAM_SCAN_MAX; i++)
		cli_print(sh, " 0x%02x", (unsigned)addrs[i]);
	cli_print(sh, "\r\n");
	return 0;
}

/*
 * camera reg <i2c-addr> <reg> [value] [-16]
 *
 * The address and the width are explicit rather than taken from the last probe:
 * this command's whole reason to exist is poking at a sensor probe could NOT
 * identify, where neither is known.
 */
int camera_board_cmd_reg(struct cli_instance *sh, int argc, char **argv)
{
	unsigned width = CAM_REG_WIDTH_8;
	uint32_t addr, reg, val = 0u;
	int have_val = 0;
	uint8_t v;
	int rc;

	if (cli_parse_u32(argv[1], &addr) != 0 || addr > 0x7Fu) {
		cli_error(sh, "camera: bad 7-bit SCCB address '%s'\r\n", argv[1]);
		return 1;
	}
	if (cli_parse_u32(argv[2], &reg) != 0 || reg > 0xFFFFu) {
		cli_error(sh, "camera: bad register '%s'\r\n", argv[2]);
		return 1;
	}
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "-16") == 0) {
			width = CAM_REG_WIDTH_16;
		} else if (!have_val && cli_parse_u32(argv[i], &val) == 0 &&
		           val <= 0xFFu) {
			have_val = 1;
		} else {
			cli_error(sh, "camera: bad argument '%s' (value 0..255 | -16)\r\n",
			          argv[i]);
			return 1;
		}
	}
	if (width == CAM_REG_WIDTH_8 && reg > 0xFFu) {
		cli_error(sh, "camera: register 0x%lx needs -16\r\n",
		          (unsigned long)reg);
		return 1;
	}

	/* svc/fmt.c implements no '*' field width, so the two register widths need
	   two literal formats rather than one parameterised one. */
	if (have_val) {
		rc = camera_reg_write((uint8_t)addr, (uint16_t)reg, width, (uint8_t)val);
		if (rc != CAM_OK) {
			report(sh, rc);
			return 1;
		}
		if (width == CAM_REG_WIDTH_16)
			cli_print(sh, "0x%02x[0x%04x] <- 0x%02x\r\n", (unsigned)addr,
			          (unsigned)reg, (unsigned)val);
		else
			cli_print(sh, "0x%02x[0x%02x] <- 0x%02x\r\n", (unsigned)addr,
			          (unsigned)reg, (unsigned)val);
		return 0;
	}

	rc = camera_reg_read((uint8_t)addr, (uint16_t)reg, width, &v);
	if (rc != CAM_OK) {
		report(sh, rc);
		return 1;
	}
	if (width == CAM_REG_WIDTH_16)
		cli_print(sh, "0x%02x[0x%04x] = 0x%02x\r\n", (unsigned)addr,
		          (unsigned)reg, (unsigned)v);
	else
		cli_print(sh, "0x%02x[0x%02x] = 0x%02x\r\n", (unsigned)addr,
		          (unsigned)reg, (unsigned)v);
	return 0;
}
