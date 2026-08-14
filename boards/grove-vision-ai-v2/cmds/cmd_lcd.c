/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_lcd.c
 * @brief   `lcd` shell command: ST7789VW panel bring-up and test patterns
 *          (Grove Vision AI V2, issue #30 / M-G3a).
 *
 * Registered LOCALLY on this board rather than in shell/cmds: the other two
 * boards have no SPI panel, so a shared command would mean an implementation
 * and two stubs plus the verification that the stubs stay stubs -- for a
 * command whose entire body is board-specific.
 *
 * The subcommands exist to answer specific questions, in order:
 *
 *   lcd            -- did the panel come up, and at what SCLK?  (The requested
 *                     clock and the achieved one differ: the SSPI master's
 *                     divider is even, so the answer has to be read back.)
 *   lcd bar        -- is the wiring right AND is the byte order right?  Eight
 *                     primary bars fail differently for each: wrong wiring
 *                     gives noise or nothing, a swapped byte pair turns the
 *                     pure colours into muddy ones while the LAYOUT survives.
 *   lcd fill <c>   -- one flat colour, for a look at uniformity and for timing
 *                     a full-frame write with nothing else in the way.
 *   lcd loop [n]   -- REPEATED DMA transfers, interruptible with Ctrl+C.  This
 *                     is the M-G3a completion condition that a static pattern
 *                     cannot reach: M-G3b's camera path depends on starting a
 *                     burst, finishing it, and being able to stop MID-CHAIN,
 *                     and none of that is exercised by drawing one frame.
 *   lcd orient     -- WHERE IS (0,0) and which way do the axes run?  (issue
 *                     #31.  The colour bars cannot answer this: transposed and
 *                     mirrored look identical in eight vertical stripes.)
 *   lcd rot <deg>  -- does MADCTL rotate this panel at all?
 *   lcd madctl <b> -- the same question with the raw byte, for the four bit
 *                     patterns that are not one of the named rotations.
 *   lcd backlight  -- PA2 on/off (the pin has a 2.2k pull-up to 3V3, so the
 *                     backlight is on from power-up until something drives it).
 *
 * WHY `madctl` TAKES A RAW BYTE.  Issue #31 is a measurement, and its whole
 * cost is in the retries: if 0x60 comes back mirrored the next thing to try is
 * 0xA0, then 0x20, then 0xE0.  Compiling the value in would spend a flash cycle
 * per trial on an external NOR this project is explicitly rationing, so the
 * eight MY/MX/MV combinations are reachable from the console instead.
 */
#include "cli.h"
#include "lcd_st7789.h"

#include "tx_api.h"

#include <stdlib.h>   /* strtoul */
#include <string.h>   /* strcmp  */

#define LCD_LOOP_DEFAULT 60u
#define LCD_LOOP_MAX     100000u

/* Ctrl+C bridge for the driver's abort hook. */
static int lcd_stop_cb(void *arg)
{
	return cli_cancel_requested((struct cli_instance *)arg) ? 1 : 0;
}

/* Sink for lcd_dump_regs(), which formats nothing itself so that the driver
 * carries no dependency on the shell. */
static void lcd_reg_out(void *ctx, const char *name, uint32_t value)
{
	cli_print((struct cli_instance *)ctx, "  %s : %08lx\r\n", name,
	          (unsigned long)value);
}

/*
 * The current orientation, in the form that says the most in one line: the
 * geometry the driver will accept, the raw byte behind it, and -- only when the
 * byte is one of the four named rotations -- the angle.
 *
 * [!] Takes a SNAPSHOT, never the individual accessors.  Reading rotation,
 * width, height and MADCTL one at a time is not atomic as a group, and the
 * shell runs commands as background jobs, so a concurrent `lcd rot 90 &` can
 * land between two of the reads.  The line would then report a combination that
 * never existed -- "320x240, madctl 60 (rot 0)".  For the command whose whole
 * purpose in issue #31 is to say what the orientation IS, a plausible-looking
 * lie is the worst available failure.
 */
static void lcd_print_orientation(struct cli_instance *sh,
                                  const struct lcd_status *st)
{
	cli_print(sh, "orientation: %ux%u, madctl %02x", st->width, st->height,
	          st->madctl);
	if (st->rotation == LCD_ROT_CUSTOM)
		cli_print(sh, " (raw, not a named rotation)\r\n");
	else
		cli_print(sh, " (rot %u)\r\n", st->rotation);
}

static void lcd_report(struct cli_instance *sh)
{
	struct lcd_status st;

	/* One snapshot for the whole report: asking twice could say "up" and
	 * then print nothing, or print a geometry from a different instant. */
	lcd_get_status(&st);

	cli_print(sh, "panel      : ST7789VW 240x320 RGB565, 4-wire SPI\r\n");
	cli_print(sh, "state      : %s\r\n",
	          st.ready ? "up" : "not initialised");
	if (st.ready) {
		uint32_t hz = st.sclk_hz;
		uint32_t bytes = (uint32_t)st.width * st.height * 2u;

		lcd_print_orientation(sh, &st);

		cli_print(sh, "sclk       : %lu Hz (read back from the "
		              "controller)\r\n", (unsigned long)hz);
		if (hz != 0u) {
			/* Ideal wire time only: no DMA turnaround, no RAMWR,
			 * no inter-frame gap.  An upper bound, printed as one
			 * so nobody mistakes it for a frame rate. */
			uint32_t us = (uint32_t)(((uint64_t)bytes * 8u *
			                          1000000ull) / hz);

			cli_print(sh, "frame wire : %lu us for %lu B "
			              "(ideal ceiling %lu fps)\r\n",
			          (unsigned long)us, (unsigned long)bytes,
			          (unsigned long)(us ? 1000000u / us : 0u));
		}
	}
	cli_print(sh, "framebuffer: %u B in SRAM (NOLOAD)\r\n",
	          (unsigned)(lcd_framebuffer_pixels() * 2u));
}

/*
 * Take the panel for the whole subcommand and make sure it is up.
 *
 * Acquiring HERE rather than leaving it to the driver keeps the two failures
 * distinguishable: "busy" means another thread owns the panel (the shell runs
 * commands as background jobs, so `lcd loop &` is a real contender), while
 * "bring-up failed" means the hardware did not come up.  The driver's own
 * guards then nest recursively inside this one.
 *
 * Every caller that gets 0 must lcd_release().
 */
static int lcd_hold_ready(struct cli_instance *sh)
{
	if (lcd_acquire() != 0) {
		cli_error(sh, "lcd: busy (another lcd command holds the "
		              "panel)\r\n");
		return -1;
	}
	if (lcd_ready() || lcd_init() == 0)
		return 0;
	lcd_release();
	cli_error(sh, "lcd: panel bring-up failed (see `dmesg`)\r\n");
	return -1;
}

/* Time a run of full-frame writes and report the achieved rate.  The loop is
 * the point: it is the only thing in M-G3a that starts a DMA burst while the
 * previous one has just finished, which is the pattern the camera preview will
 * run in, and Ctrl+C has to land in the middle of one. */
static int lcd_loop(struct cli_instance *sh, uint32_t frames)
{
	static const uint16_t cycle[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
	ULONG t0, t1;
	uint32_t done = 0u;
	int rc = 0;

	/* Held across every frame, not just each one: a second `lcd` command --
	 * the shell will happily run one as a background job -- slipping a
	 * frame in between would be visible on the glass and would make the fps
	 * number a measurement of two commands. */
	if (lcd_acquire() != 0) {
		cli_error(sh, "lcd: busy (another lcd command holds the "
		              "panel)\r\n");
		return 1;
	}

	t0 = tx_time_get();
	while (done < frames) {
		rc = lcd_fill(cycle[done & 3u], lcd_stop_cb, sh);
		if (rc != 0)
			break;
		done++;
	}
	t1 = tx_time_get();
	lcd_release();

	if (rc < 0) {
		cli_error(sh, "lcd: transfer failed after %lu frame(s)\r\n",
		          (unsigned long)done);
		return 1;
	}
	if (rc == 1) {
		/* Cancelled.  Deliberately silent: the shared core drops any
		 * output produced while cancel_req is still set (cli_session.c
		 * discards the staging buffer and the TX path fast-fails), so a
		 * summary printed here would never reach the terminal.  The
		 * dispatcher's "^C" is the feedback, and the abort itself is
		 * what this path exists to exercise. */
		return 0;
	}

	{
		ULONG ticks = t1 - t0;
		uint32_t ms = (uint32_t)((ticks * 1000u) /
		                         TX_TIMER_TICKS_PER_SECOND);

		cli_print(sh, "%lu frame(s) in %lu ms", (unsigned long)done,
		          (unsigned long)ms);
		if (ms != 0u)
			cli_print(sh, " = %lu.%lu fps",
			          (unsigned long)(done * 1000u / ms),
			          (unsigned long)((done * 10000u / ms) % 10u));
		cli_print(sh, "\r\n");
	}
	return 0;
}

static int cmd_lcd(struct cli_instance *sh, int argc, char **argv)
{
	if (argc == 1) {
		lcd_report(sh);
		return 0;
	}

	if (strcmp(argv[1], "init") == 0) {
		if (lcd_hold_ready(sh) != 0)
			return 1;
		lcd_release();
		return 0;
	}

	if (strcmp(argv[1], "bar") == 0) {
		int rc;

		if (lcd_hold_ready(sh) != 0)
			return 1;
		rc = lcd_bars(lcd_stop_cb, sh);
		lcd_release();
		if (rc < 0) {
			cli_error(sh, "lcd: bar transfer failed\r\n");
			return 1;
		}
		cli_print(sh, "colour bars: white yellow cyan green magenta "
		              "red blue black (left to right)\r\n");
		cli_print(sh, "  wrong order or muddy colours = RGB565 byte "
		              "order is wrong, not the wiring\r\n");
		return 0;
	}

	if (strcmp(argv[1], "orient") == 0) {
		struct lcd_status st;
		int rc;

		if (lcd_hold_ready(sh) != 0)
			return 1;
		rc = lcd_orient(lcd_stop_cb, sh);
		/* Snapshot while the panel is still HELD, so the line describes
		 * the orientation this pattern was actually drawn in rather
		 * than whatever a background job may have set since. */
		lcd_get_status(&st);
		lcd_release();
		if (rc < 0) {
			cli_error(sh, "lcd: orient transfer failed\r\n");
			return 1;
		}
		if (rc == 1)
			return 0;               /* cancelled; output is dropped */
		lcd_print_orientation(sh, &st);
		cli_print(sh, "  white 32x32 square = pixel (0,0)\r\n");
		cli_print(sh, "  red bar    = the +X axis (row 0, full width)\r\n");
		cli_print(sh, "  green bar  = the +Y axis (column 0, full "
		              "height)\r\n");
		cli_print(sh, "  blue 24x24 square = the far corner "
		              "(w-1,h-1)\r\n");
		return 0;
	}

	if (strcmp(argv[1], "rot") == 0) {
		struct lcd_status st;
		unsigned long v;
		char *end = NULL;

		if (argc < 3) {
			if (lcd_hold_ready(sh) != 0)
				return 1;
			lcd_get_status(&st);
			lcd_release();
			lcd_print_orientation(sh, &st);
			return 0;
		}
		v = strtoul(argv[2], &end, 0);
		if (end == argv[2] || *end != '\0') {
			cli_error(sh, "lcd: rot takes 0, 90, 180 or 270\r\n");
			return 1;
		}
		if (lcd_hold_ready(sh) != 0)
			return 1;
		if (lcd_set_rotation((unsigned)v) != 0) {
			lcd_release();
			cli_error(sh, "lcd: rot takes 0, 90, 180 or 270 (and "
			              "the panel must accept the write)\r\n");
			return 1;
		}
		lcd_get_status(&st);
		lcd_release();
		lcd_print_orientation(sh, &st);
		cli_print(sh, "  now run `lcd orient` -- the picture is the "
		              "measurement, not this line\r\n");
		return 0;
	}

	if (strcmp(argv[1], "madctl") == 0) {
		struct lcd_status st;
		unsigned long v;
		char *end = NULL;

		if (argc < 3) {
			if (lcd_hold_ready(sh) != 0)
				return 1;
			lcd_get_status(&st);
			lcd_release();
			lcd_print_orientation(sh, &st);
			return 0;
		}
		v = strtoul(argv[2], &end, 0);
		if (end == argv[2] || *end != '\0' || v > 0xFFu) {
			cli_error(sh, "lcd: madctl takes one byte, e.g. "
			              "0x60\r\n");
			return 1;
		}
		if (lcd_hold_ready(sh) != 0)
			return 1;
		if (lcd_set_madctl((uint8_t)v) != 0) {
			lcd_release();
			cli_error(sh, "lcd: madctl write failed\r\n");
			return 1;
		}
		lcd_get_status(&st);
		lcd_release();
		lcd_print_orientation(sh, &st);
		/* MV is the only bit that changes the geometry, so it is the
		 * only one worth restating: MX/MY move the origin around within
		 * whichever shape MV picked. */
		cli_print(sh, "  MY %u MX %u MV %u RGB-order %s\r\n",
		          (unsigned)((v & LCD_MADCTL_MY) != 0u),
		          (unsigned)((v & LCD_MADCTL_MX) != 0u),
		          (unsigned)((v & LCD_MADCTL_MV) != 0u),
		          (v & LCD_MADCTL_RGB) ? "BGR" : "RGB");
		return 0;
	}

	if (strcmp(argv[1], "fill") == 0) {
		unsigned long v;
		char *end = NULL;
		int rc;

		if (argc < 3) {
			cli_error(sh, "lcd: fill needs an RGB565 value "
			              "(e.g. 0xF800)\r\n");
			return 1;
		}
		v = strtoul(argv[2], &end, 0);
		if (end == argv[2] || *end != '\0' || v > 0xFFFFu) {
			cli_error(sh, "lcd: fill takes a 16-bit RGB565 "
			              "value\r\n");
			return 1;
		}
		if (lcd_hold_ready(sh) != 0)
			return 1;
		rc = lcd_fill((uint16_t)v, lcd_stop_cb, sh);
		lcd_release();
		if (rc < 0) {
			cli_error(sh, "lcd: fill transfer failed\r\n");
			return 1;
		}
		return 0;
	}

	if (strcmp(argv[1], "loop") == 0) {
		uint32_t frames = LCD_LOOP_DEFAULT;

		if (argc >= 3) {
			char *end = NULL;
			unsigned long v = strtoul(argv[2], &end, 0);

			if (end == argv[2] || *end != '\0' || v == 0u ||
			    v > LCD_LOOP_MAX) {
				cli_error(sh, "lcd: loop takes 1..%lu "
				              "frames\r\n",
				          (unsigned long)LCD_LOOP_MAX);
				return 1;
			}
			frames = (uint32_t)v;
		}
		if (lcd_hold_ready(sh) != 0)
			return 1;
		lcd_release();
		return lcd_loop(sh, frames);
	}

	if (strcmp(argv[1], "regs") == 0) {
		if (lcd_hold_ready(sh) != 0)
			return 1;
		cli_print(sh, "SSIENR 1 = controller enabled; SER != 0 = a "
		              "slave is selected (DesignWare SSI does not\r\n"
		              "clock at all with SER == 0); BAUDR = the even "
		              "divider it really took;\r\n"
		              "TXFLR 0 = the TX FIFO drained.\r\n\r\n");
		lcd_dump_regs(lcd_reg_out, sh);
		lcd_release();
		return 0;
	}

	if (strcmp(argv[1], "backlight") == 0) {
		if (argc < 3 ||
		    (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
			cli_error(sh, "lcd: backlight on|off\r\n");
			return 1;
		}
		if (lcd_hold_ready(sh) != 0)
			return 1;
		lcd_backlight(strcmp(argv[2], "on") == 0);
		/* PA2 also drives the Grove connector's 5 V I2C SCL through a
		 * level shifter; holding it low jams that bus.  Say so rather
		 * than let it be discovered with a sensor attached. */
		lcd_release();
		cli_print(sh, "backlight %s%s\r\n", argv[2],
		          (strcmp(argv[2], "off") == 0)
		              ? " (PA2 low also holds the Grove I2C SCL low)"
		              : "");
		return 0;
	}

	cli_error(sh, "lcd: usage: lcd [init|bar|orient|rot [deg]|"
	              "madctl [byte]|fill <rgb565>|loop [n]|regs|"
	              "backlight on|off]\r\n");
	return 1;
}

CLI_CMD_REGISTER_USAGE(lcd, NULL,
                       "ST7789 SPI panel: bring-up, test patterns, rotation",
                       "[init|bar|orient|rot [deg]|madctl [byte]|"
                       "fill <rgb565>|loop [n]|regs|backlight on|off]",
                       cmd_lcd, 1, 3);
