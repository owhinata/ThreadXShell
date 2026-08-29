/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_lcd.c
 * @brief   `lcd` shell command: ST7789VW panel bring-up and test patterns
 *          (Grove Vision AI V2, issue #30 / M-G3a; subcommand set in #33).
 *
 * Registered LOCALLY on this board rather than in shell/cmds: the other two
 * boards have no SPI panel, so a shared command would mean an implementation
 * and two stubs plus the verification that the stubs stay stubs -- for a
 * command whose entire body is board-specific.
 *
 * The subcommands exist to answer specific questions, in order:
 *
 *   lcd info       -- did the panel come up, and at what SCLK?  (The requested
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
 *   lcd on / off   -- the backlight, PA2 (the pin has a 2.2k pull-up to 3V3, so
 *                     it is on from power-up until something drives it).
 *
 * WHY `madctl` TAKES A RAW BYTE.  Issue #31 is a measurement, and its whole
 * cost is in the retries: if 0x60 comes back mirrored the next thing to try is
 * 0xA0, then 0x20, then 0xE0.  Compiling the value in would spend a flash cycle
 * per trial on an external NOR this project is explicitly rationing, so the
 * eight MY/MX/MV combinations are reachable from the console instead.
 *
 * FORM (issue #33).  A CLI_SUBCMD_SET_CREATE set, like `lcd` on the other two
 * boards and like `devmem` here, rather than the strcmp chain #30 shipped: the
 * static tree is what tab completion walks and what `help lcd` enumerates, and
 * neither can see into a handler's strcmps.
 *
 * `lcd` alone is NOT the report: the root is a pure parent (no handler), so a
 * mistyped subcommand reports itself as one instead of arriving at a handler
 * as a stray argument.  The report is `lcd info`, spelled as on the other two
 * boards.
 *
 * WHY `on`/`off` AND NOT `backlight on|off`.  Neither of the other boards has a
 * `backlight` subcommand -- there, `lcd on`/`lcd off` is the spelling, so that
 * is the spelling here too; one vocabulary across the three consoles is worth
 * more than a name that documents this board's internals.  What differs is what
 * is behind it: on wio/f746 `off` also stops the LTDC scanout, while this panel
 * is push-driven and has no scanout, so here it is the backlight GPIO alone.
 * The help lines and the command's own output therefore say PA2 outright --
 * which the pad-6 identification procedure in the board README depends on,
 * since PA2 is the one pin there that can be confirmed without a meter.
 */
#include "cli.h"
#include "cam_lcd_sink.h"
#include "lcd_st7789.h"

#include "tx_api.h"

#include <string.h>   /* strcmp (colour names) */

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
 * Colour name or numeric RGB565 (0x.... or decimal), the same spelling `lcd
 * fill` takes on the other two boards.  The eight names are the primaries the
 * colour bars draw, which is the point: after `lcd bar` shows a suspicious
 * hue, `lcd fill red` puts that one candidate on the whole glass without
 * anyone having to recall whether red is 0xF800 or 0x001F -- and getting that
 * backwards is exactly the byte-order failure being chased.
 */
static int lcd_parse_colour(const char *s, uint16_t *out)
{
	static const struct {
		const char *name;
		uint16_t    rgb;
	} names[] = {
		{ "black",   0x0000u },
		{ "blue",    0x001Fu },
		{ "green",   0x07E0u },
		{ "cyan",    0x07FFu },
		{ "red",     0xF800u },
		{ "magenta", 0xF81Fu },
		{ "yellow",  0xFFE0u },
		{ "white",   0xFFFFu },
	};
	uint32_t v;

	for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (strcmp(s, names[i].name) == 0) {
			*out = names[i].rgb;
			return 0;
		}
	}
	if (cli_parse_u32(s, &v) == 0 && v <= 0xFFFFu) {
		*out = (uint16_t)v;
		return 0;
	}
	return -1;
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

/*
 * Hold the panel for a change to its PERSISTENT state, refusing while a camera
 * sink owns it (issue #99).
 *
 * [!] THE CHECK IS MADE UNDER THE GUARD, and that is the whole point rather than
 * tidiness.  Rotation and MADCTL move the driver's geometry and the backlight
 * stays where it is put; the panel sink blits a fixed size that the driver
 * validates inside the guard, so a rotation that lands between an unguarded
 * check and the write leaves every later blit refused or clamped while each
 * layer still reports success.  The sink's attach path holds this same guard
 * from bring-up until it is linked, so a caller that holds it here either got in
 * before the sink existed or can see it.
 *
 * Only rotation and MADCTL come through here.  A transfer-scoped command (fill,
 * bar, orient, the loop) does not need to: the worst it can do is lose a frame.
 * Neither does the backlight, which is a GPIO write with an obvious and
 * instantly reversible effect -- see lcd_set_backlight().
 */
static int lcd_hold_exclusive(struct cli_instance *sh, const char *what)
{
	if (lcd_hold_ready(sh) != 0)
		return -1;
	if (cam_lcd_sink_linked()) {
		lcd_release();
		cli_error(sh, "lcd: a camera sink owns the panel -- %s would leave "
		              "its frames refused or clamped.\r\n"
		              "     Stop it first (`nn stream stop`, or `camera "
		              "preview` with Ctrl+C)\r\n", what);
		return -1;
	}
	return 0;
}

/* Shared by `fill` and `clear`: hold the panel, flood it, report a transfer
 * failure.  A cancel (rc == 1) is silent -- see lcd_run_loop(). */
static int lcd_flood(struct cli_instance *sh, uint16_t rgb565)
{
	int rc;

	if (lcd_hold_ready(sh) != 0)
		return 1;
	rc = lcd_fill(rgb565, lcd_stop_cb, sh);
	lcd_release();
	if (rc < 0) {
		cli_error(sh, "lcd: fill transfer failed\r\n");
		return 1;
	}
	return 0;
}

/* Time a run of full-frame writes and report the achieved rate.  The loop is
 * the point: it is the only thing in M-G3a that starts a DMA burst while the
 * previous one has just finished, which is the pattern the camera preview will
 * run in, and Ctrl+C has to land in the middle of one. */
static int lcd_run_loop(struct cli_instance *sh, uint32_t frames)
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

/* ---- subcommands --------------------------------------------------------- */

static int cmd_lcd_info(struct cli_instance *sh, int argc, char **argv)
{
	struct lcd_status st;

	(void)argc; (void)argv;

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
	return 0;
}

static int cmd_lcd_init(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;

	if (lcd_hold_ready(sh) != 0)
		return 1;
	lcd_release();
	return 0;
}

static int cmd_lcd_bar(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc; (void)argv;

	if (lcd_hold_ready(sh) != 0)
		return 1;
	rc = lcd_bars(lcd_stop_cb, sh);
	lcd_release();
	if (rc < 0) {
		cli_error(sh, "lcd: bar transfer failed\r\n");
		return 1;
	}
	cli_print(sh, "colour bars: white yellow cyan green magenta red blue "
	              "black (left to right)\r\n");
	cli_print(sh, "  wrong order or muddy colours = RGB565 byte order is "
	              "wrong, not the wiring\r\n");
	return 0;
}

static int cmd_lcd_orient(struct cli_instance *sh, int argc, char **argv)
{
	struct lcd_status st;
	int rc;

	(void)argc; (void)argv;

	if (lcd_hold_ready(sh) != 0)
		return 1;
	rc = lcd_orient(lcd_stop_cb, sh);
	/* Snapshot while the panel is still HELD, so the line describes the
	 * orientation this pattern was actually drawn in rather than whatever a
	 * background job may have set since. */
	lcd_get_status(&st);
	lcd_release();
	if (rc < 0) {
		cli_error(sh, "lcd: orient transfer failed\r\n");
		return 1;
	}
	if (rc == 1)
		return 0;                       /* cancelled; output is dropped */
	lcd_print_orientation(sh, &st);
	cli_print(sh, "  white 32x32 square = pixel (0,0)\r\n");
	cli_print(sh, "  red bar    = the +X axis (row 0, full width)\r\n");
	cli_print(sh, "  green bar  = the +Y axis (column 0, full height)\r\n");
	cli_print(sh, "  blue 24x24 square = the far corner (w-1,h-1)\r\n");
	return 0;
}

static int cmd_lcd_rot(struct cli_instance *sh, int argc, char **argv)
{
	struct lcd_status st;
	uint32_t v;

	if (argc < 2) {
		if (lcd_hold_ready(sh) != 0)
			return 1;
		lcd_get_status(&st);
		lcd_release();
		lcd_print_orientation(sh, &st);
		return 0;
	}
	if (cli_parse_u32(argv[1], &v) != 0) {
		cli_error(sh, "lcd: rot takes 0, 90, 180 or 270\r\n");
		return 1;
	}
	/* The SETTER only: reading the rotation back is always safe. */
	if (lcd_hold_exclusive(sh, "rotating the panel") != 0)
		return 1;
	if (lcd_set_rotation((unsigned)v) != 0) {
		lcd_release();
		cli_error(sh, "lcd: rot takes 0, 90, 180 or 270 (and the panel "
		              "must accept the write)\r\n");
		return 1;
	}
	lcd_get_status(&st);
	lcd_release();
	lcd_print_orientation(sh, &st);
	cli_print(sh, "  now run `lcd orient` -- the picture is the "
	              "measurement, not this line\r\n");
	return 0;
}

static int cmd_lcd_madctl(struct cli_instance *sh, int argc, char **argv)
{
	struct lcd_status st;
	uint32_t v;

	if (argc < 2) {
		if (lcd_hold_ready(sh) != 0)
			return 1;
		lcd_get_status(&st);
		lcd_release();
		lcd_print_orientation(sh, &st);
		return 0;
	}
	if (cli_parse_u32(argv[1], &v) != 0 || v > 0xFFu) {
		cli_error(sh, "lcd: madctl takes one byte, e.g. 0x60\r\n");
		return 1;
	}
	/* The SETTER only: reading MADCTL back is always safe. */
	if (lcd_hold_exclusive(sh, "writing MADCTL") != 0)
		return 1;
	if (lcd_set_madctl((uint8_t)v) != 0) {
		lcd_release();
		cli_error(sh, "lcd: madctl write failed\r\n");
		return 1;
	}
	lcd_get_status(&st);
	lcd_release();
	lcd_print_orientation(sh, &st);
	/* MV is the only bit that changes the geometry, so it is the only one
	 * worth restating: MX/MY move the origin around within whichever shape
	 * MV picked. */
	cli_print(sh, "  MY %u MX %u MV %u RGB-order %s\r\n",
	          (unsigned)((v & LCD_MADCTL_MY) != 0u),
	          (unsigned)((v & LCD_MADCTL_MX) != 0u),
	          (unsigned)((v & LCD_MADCTL_MV) != 0u),
	          (v & LCD_MADCTL_RGB) ? "BGR" : "RGB");
	return 0;
}

static int cmd_lcd_fill(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t rgb565;

	(void)argc;                             /* mandatory = 2 */

	if (lcd_parse_colour(argv[1], &rgb565) != 0) {
		cli_error(sh, "lcd: bad colour '%s' (name or 0xRGB565)\r\n",
		          argv[1]);
		return 1;
	}
	return lcd_flood(sh, rgb565);
}

static int cmd_lcd_clear(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;

	return lcd_flood(sh, 0x0000u);
}

static int cmd_lcd_loop(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t frames = LCD_LOOP_DEFAULT;

	if (argc >= 2) {
		uint32_t v;

		if (cli_parse_u32(argv[1], &v) != 0 || v == 0u ||
		    v > LCD_LOOP_MAX) {
			cli_error(sh, "lcd: loop takes 1..%lu frames\r\n",
			          (unsigned long)LCD_LOOP_MAX);
			return 1;
		}
		frames = v;
	}
	/* Bring-up (and its error message) before the timed run, so a panel
	 * that never came up does not count as frame 0 of a measurement. */
	if (lcd_hold_ready(sh) != 0)
		return 1;
	lcd_release();
	return lcd_run_loop(sh, frames);
}

static int cmd_lcd_regs(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;

	if (lcd_hold_ready(sh) != 0)
		return 1;
	cli_print(sh, "SSIENR 1 = controller enabled; SER != 0 = a slave is "
	              "selected (DesignWare SSI does not\r\n"
	              "clock at all with SER == 0); BAUDR = the even divider it "
	              "really took;\r\n"
	              "TXFLR 0 = the TX FIFO drained.\r\n\r\n");
	lcd_dump_regs(lcd_reg_out, sh);
	lcd_release();
	return 0;
}

/* PA2 also drives the Grove connector's 5 V I2C SCL through a level shifter;
 * holding it low jams that bus.  Say so rather than let it be discovered with a
 * sensor attached. */
static int lcd_set_backlight(struct cli_instance *sh, int on)
{
	/*
	 * [!] NEITHER DIRECTION TAKES THE TRANSACTION GUARD, AND NEITHER IS
	 * REFUSED.  Two separate things, both learned at the bench.
	 *
	 * The guard first: lcd_backlight() is ONE GPIO WRITE.  It touches no SPI
	 * state, no framebuffer, no CS/DC and no DMA, so the panel guard protects
	 * nothing it does -- while under a live stream the panel thread holds that
	 * guard for about 96% of every frame (compare `blit` against `profile` in
	 * `camera stats`).  Taking it made `lcd on` fail with "busy" almost every
	 * time, in exactly the case it exists for.
	 *
	 * Then the refusal, which this file briefly had on `off` alone.  That was
	 * justified by "a live preview goes dark and no frame turns it back on" --
	 * a premise the line above removes, because `on` now works during a stream.
	 * What was left was an asymmetry with no hazard under it, and turning the
	 * panel off while inference runs is an ordinary thing to want.
	 *
	 * Rotation and MADCTL stay refused, and the difference is the KIND of
	 * failure rather than the severity: those leave the sink's fixed-size blits
	 * refused or clamped while every layer still reports success, which is
	 * silent and lasts until the stream is stopped.  A dark panel announces
	 * itself and is one command away from undone.
	 */
	int streaming = cam_lcd_sink_linked();

	if (streaming) {
		/* A sink owns the panel, so it is already up: straight to the pin. */
		lcd_backlight(on);
	} else {
		if (lcd_hold_ready(sh) != 0)
			return 1;
		lcd_backlight(on);
		lcd_release();
	}
	cli_print(sh, "backlight %s%s\r\n", on ? "on" : "off",
	          on ? "" : " (PA2 low also holds the Grove I2C SCL low)");
	if (streaming && !on)
		cli_print(sh, "  the stream is still running -- `lcd on` brings the "
		              "picture back\r\n");
	return 0;
}

static int cmd_lcd_on(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;

	return lcd_set_backlight(sh, 1);
}

static int cmd_lcd_off(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;

	return lcd_set_backlight(sh, 0);
}

/* ---- registration -------------------------------------------------------- */

CLI_SUBCMD_SET_CREATE(lcd_subcmds,
	CLI_CMD(info, NULL, "panel / orientation / read-back SCLK / frame "
	        "buffer", cmd_lcd_info),
	CLI_CMD(init, NULL, "bring the panel up (the others do it for you)",
	        cmd_lcd_init),
	CLI_CMD(bar, NULL, "8 colour bars (wiring AND byte-order check)",
	        cmd_lcd_bar),
	CLI_CMD(orient, NULL, "origin / axis probe (where is (0,0)?)",
	        cmd_lcd_orient),
	CLI_CMD_ARG_USAGE(rot, NULL, "read or set the rotation (0|90|180|270)",
	                  "[deg]", cmd_lcd_rot, 1, 1),
	CLI_CMD_ARG_USAGE(madctl, NULL, "read or set MADCTL raw, e.g. 0x60",
	                  "[byte]", cmd_lcd_madctl, 1, 1),
	CLI_CMD_ARG_USAGE(fill, NULL, "flood with a colour (name or 0xRGB565)",
	                  "<colour>", cmd_lcd_fill, 2, 0),
	CLI_CMD(clear, NULL, "fill black", cmd_lcd_clear),
	CLI_CMD_ARG_USAGE(loop, NULL, "repeated full-frame writes, Ctrl+C to "
	                  "stop", "[frames]", cmd_lcd_loop, 1, 1),
	CLI_CMD(regs, NULL, "raw SSPI controller registers", cmd_lcd_regs),
	CLI_CMD(on, NULL, "backlight on (PA2 high)", cmd_lcd_on),
	CLI_CMD(off, NULL, "backlight off (PA2 low -- also holds the Grove I2C "
	        "SCL low)", cmd_lcd_off),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(lcd, lcd_subcmds,
                 "ST7789 SPI panel: bring-up, test patterns, rotation",
                 NULL, 1, 0);
