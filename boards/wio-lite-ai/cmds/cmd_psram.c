/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_psram.c
 * @brief   This board's table for the shared `psram` (shell/cmds/cmd_ext_ram.c,
 *          issue #251), and its six tuning subcommands (owhinata/wio-lite-ai#3
 *          until then: the whole command).
 *
 *   psram info          the part, the window, its ranges, the state, then the
 *                       bring-up lines (clock, id, latency, dlyb, mr0, ...)
 *   psram test [bytes]  write/read-back test (default and at most the 6 MB
 *                       non-cacheable pool)
 *   psram set | mr0 | phase | mmapscan | wtune | clk   tuning (svc/ext_ram_config.h;
 *                       their words are issue #252's)
 *
 * The test is the primary functional check for the OCTOSPI1 octal wiring and
 * latency: a wrong data-line AF, latency code or delay-block setting shows up as
 * a mismatch (or, if the mmap latency is badly off, an AXI/bus fault that the
 * fault handler records to dmesg).  PSRAM is scratch RAM, so no dangerous-command
 * gate.
 *
 * THE RANGES, as src/mpu.c maps the window: the low 6 MB Normal non-cacheable
 * (the LCD's two buffers and the camera's frame), the top 2 MB write-back
 * cacheable (owhinata/wio-lite-ai#9: nn's arena and model slots, membench's
 * cached buffer).  [!] A test never reaches the cacheable 2 MB (the shared
 * command stops at the first cacheable range): a store and its load can be
 * answered by the cache and never reach the APS6408 -- the reading
 * owhinata/wio-lite-ai#3 and #16 tuned the timing against has to stay a device
 * test.
 *
 * THE STEPS, in this order (given back in the reverse one):
 *   1. the OCTOSPI1 reconfigurer mark, psram_acquire(): refused while anyone
 *      uses the memory, while the LTDC scans out of it, while a DMA2D transfer
 *      was never confirmed stopped, or while a camera stream runs.  Held for
 *      the whole test, so the LCD, the camera, nn, membench's PSRAM rows,
 *      devmem's PSRAM and `wifi flash` are refused meanwhile;
 *   2. the camera's single-capture frame is dropped (issue #150), under the
 *      mark and before the first write: a frame captured before would
 *      otherwise stay valid over the test's patterns;
 *   3. (with the LCD built) the give repaints both LCD buffers black, with the
 *      CPU, while the mark is still held -- so no `lcd on` can show the
 *      patterns -- and only then is the mark given back.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "ext_ram_cmd_board.h"
#include "fmt.h"         /* fmt_vsnformat: the board's `info` lines */
#include "psram.h"
#include "camera.h"      /* camera_frame_invalidate(): the test clobbers cam_frame */
#if BSP_ENABLE_LCD
#include "ltdc_display.h" /* ltdc_fb_repaint_black(): after a test */
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Take the OCTOSPI1 concurrency guard for a hardware subcommand; on contention
 * (a backgrounded `psram`/`membench` command still holds it) print and bail so
 * two commands never drive OCTOSPI1 at once.  Pair every success with
 * psram_release().  Returns 0 when acquired, -1 when busy. */
static int psram_guard(struct cli_instance *sh)
{
	if (!psram_acquire()) {
		cli_error(sh, "psram: OCTOSPI1 busy (another psram/membench command or "
		          "`nn stream` holds it, or the LCD is scanning out of it -- "
		          "run `lcd off`)\r\n");
		return -1;
	}
	return 0;
}

/* Bring-up stage names (PSRAM_STAGE_*). */
static const char *psram_stage_name(uint32_t s)
{
	switch (s) {
	case PSRAM_STAGE_GRST: return "global-reset (TCF timeout)";
	case PSRAM_STAGE_MR0:  return "MA0 pair read";
	case PSRAM_STAGE_MR2:  return "MA2 pair read";
	default:               return "OK";
	}
}

/* One of the board's `info` lines, formatted on the stack and written whole. */
static int psram_line(ext_ram_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	return (write(ctx, line, (size_t)n) < 0) ? -1 : 0;
}

/* `psram info`, after the shared lines: the bring-up values, sampled first. */
static int psram_info_lines(ext_ram_write_fn write, void *ctx)
{
	uint8_t id[2];
	uint32_t rd, wr, sel, unit;

	psram_read_id(id);
	psram_get_latency(&rd, &wr);
	psram_get_phase(&sel, &unit);

	if (!psram_ready() &&
	    psram_line(write, ctx, "  init    : failed at %s\r\n",
	               psram_stage_name(psram_init_stage())) != 0)
		return -1;
	if (psram_line(write, ctx, "  clock   : %lu Hz (device)\r\n",
	               (unsigned long)psram_clock_hz()) != 0 ||
	    psram_line(write, ctx, "  id      : MR1=0x%02X MR2=0x%02X\r\n", id[0],
	               id[1]) != 0 ||
	    psram_line(write, ctx, "  latency : read %lu / write %lu dummy cycles\r\n",
	               (unsigned long)rd, (unsigned long)wr) != 0 ||
	    psram_line(write, ctx, "  dlyb    : phase %lu / units %lu\r\n",
	               (unsigned long)sel, (unsigned long)unit) != 0 ||
	    psram_line(write, ctx, "  mr0     : 0x%02lX   read-flags 0x%lX "
	               "(b0=SSHIFT b1=DHQC)\r\n", (unsigned long)psram_get_mr0(),
	               (unsigned long)psram_get_read_flags()) != 0 ||
	    psram_line(write, ctx, "  ifmt    : %s instruction\r\n",
	               psram_get_instruction_dtr() ? "16-bit DTR doubled"
	                                           : "8-bit SDR official") != 0 ||
	    psram_line(write, ctx, "  probe   : dqse %lu / refresh %lu (DCR4)\r\n",
	               (unsigned long)psram_get_dqse(),
	               (unsigned long)psram_get_refresh()) != 0)
		return -1;
	return 0;
}

/* psram clk <presc>: device clock = 266MHz/(presc+1).  2=88.7M, 7=33.3M. */
int cmd_psram_clk(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t p;

	(void)argc;
	if (cli_parse_u32(argv[1], &p) != 0 || p > 255u) {
		cli_error(sh, "psram: bad prescaler (0..255)\r\n");
		return 1;
	}
	if (psram_guard(sh))
		return 1;
	psram_set_prescaler(p);
	psram_release();
	cli_print(sh, "psram: clock %lu Hz; run `psram test 4096`\r\n",
	          (unsigned long)psram_clock_hz());
	/* DCR4.REFRESH is not re-derived here (RM0468 sec 25.7.5: it counts kernel
	 * cycles, so its wall-clock window shifts with the prescaler).  Harmless at
	 * the shipped point -- CSBOUND=32B releases NCS long before the refresh
	 * limiter or tCEM -- but a diagnostic caveat for the `psram clk` sweep. */
	cli_print(sh, "  note: DCR4 refresh not re-tuned for this clock (diagnostic)\r\n");
	return 0;
}

/* psram phase <sel> [units]: override the DLYB read-sampling phase to sweep for
 * the value that aligns the read data eye (no reflash). */
int cmd_psram_phase(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t sel, unit, cur_sel, cur_unit;

	psram_get_phase(&cur_sel, &cur_unit);
	unit = cur_unit;
	if (cli_parse_u32(argv[1], &sel) != 0 || sel > 12u ||
	    (argc >= 3 && (cli_parse_u32(argv[2], &unit) != 0 || unit > 127u))) {
		cli_error(sh, "psram: bad phase (sel 0..12, units 0..127)\r\n");
		return 1;
	}
	if (psram_guard(sh))
		return 1;
	psram_set_phase(sel, unit);
	psram_release();
	cli_print(sh, "psram: dlyb phase %lu / units %lu; run `psram test 4096`\r\n",
	          (unsigned long)sel, (unsigned long)unit);
	return 0;
}

/* psram mr0 <hex>: write the APS6408 MR0 read-latency register
   (owhinata/wio-lite-ai#16 diagnostic).
 * 0x09 = power-up default (Variable LC5); 0x24 = Fixed Latency LC8 (ST U585
 * reference -- pair with `psram set 8 <wr>`).  `psram info` shows the value
 * written, not one read back from the device. */
int cmd_psram_mr0(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t v;

	(void)argc;
	if (!psram_ready()) {
		cli_error(sh, "psram: not ready\r\n");
		return 1;
	}
	if (cli_parse_u32(argv[1], &v) != 0 || v > 0xFFu) {
		cli_error(sh, "psram: bad MR0 value (0..0xFF)\r\n");
		return 1;
	}
	if (psram_guard(sh))
		return 1;
	psram_set_mr0(v);
	psram_release();
	cli_print(sh, "psram: MR0<=0x%02lX (not read back); set the read dummy "
	          "(`psram set`), then `psram test 4096`\r\n", (unsigned long)v);
	return 0;
}

/* psram set <read_dcyc> <write_dcyc>: re-enter mmap with new latencies to sweep
 * for values that pass `psram test` without a reflash. */
int cmd_psram_set(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t rd, wr;

	(void)argc;
	if (cli_parse_u32(argv[1], &rd) != 0 || cli_parse_u32(argv[2], &wr) != 0 ||
	    rd > 31u || wr > 31u) {
		cli_error(sh, "psram: bad dummy-cycle count (0..31)\r\n");
		return 1;
	}
	if (psram_guard(sh))
		return 1;
	psram_set_latency(rd, wr);
	psram_release();
	cli_print(sh, "psram: latency read %lu / write %lu; run `psram test 4096`\r\n",
	          (unsigned long)rd, (unsigned long)wr);
	return 0;
}

/* psram mmapscan <start [phase]|show|stop>: reset-persistent DLYB sweep
 * validated against real memory-mapped access (owhinata/wio-lite-ai#16).  `start`
 * captures the current clock/latency/MR0 and sweeps DLYB units 0..124 (step 4) on
 * `phase` (default 3), auto-rebooting per unit -- the console drops during the sweep
 * and returns when it completes; then `show` prints which units passed mmap. */
int cmd_psram_mmapscan(struct cli_instance *sh, int argc, char **argv)
{
	struct psram_scan_state st;
	uint32_t phase = 3u, ulo, ustep, ncand, idx, ci;
	int state;

	if (argc < 2) {
		cli_error(sh, "psram: usage mmapscan <start [phase]|show|stop>\r\n");
		return 1;
	}
	if (strcmp(argv[1], "stop") == 0) {
		psram_mmapscan_stop();
		cli_print(sh, "psram mmapscan: stopped\r\n");
		return 0;
	}
	if (strcmp(argv[1], "show") == 0) {
		state = psram_mmapscan_get(&st);
		if (state == 0) {
			cli_print(sh, "psram mmapscan: no sweep recorded\r\n");
			return 0;
		}
		ulo   = st.rng & 0xFFu;
		ustep = (st.rng >> 8) & 0xFFu;
		ncand = (st.rng >> 16) & 0xFFu;
		idx   = (st.rng >> 24) & 0xFFu;
		cli_print(sh, "psram mmapscan: %s  phase %lu  presc %lu (%lu Hz)  "
		          "rd %lu wr %lu  mr0 0x%02lX\r\n",
		          (state == 2) ? "COMPLETE" : "running",
		          (unsigned long)((st.cfg >> 8) & 0xFFu),
		          (unsigned long)(st.cfg & 0xFFu),
		          (unsigned long)(266000000u / ((st.cfg & 0xFFu) + 1u)),
		          (unsigned long)((st.cfg >> 16) & 0xFFu),
		          (unsigned long)((st.cfg >> 24) & 0xFFu),
		          (unsigned long)st.mr0);
		cli_print(sh, "  mmap eye (P pass / x fail / . untested), "
		          "unit %lu..%lu step %lu:\r\n", (unsigned long)ulo,
		          (unsigned long)(ulo + (ncand - 1u) * ustep), (unsigned long)ustep);
		cli_print(sh, "  |");
		for (ci = 0u; ci < ncand; ci++)
			cli_print(sh, "%c", (st.passed & (1u << ci)) ? 'P'
			          : (st.tested & (1u << ci)) ? 'x' : '.');
		cli_print(sh, "|  (%lu/%lu tested)\r\n",
		          (unsigned long)(idx < ncand ? idx : ncand), (unsigned long)ncand);
		for (ci = 0u; ci < ncand; ci++)
			if (st.passed & (1u << ci))
				cli_print(sh, "  PASS unit %lu\r\n",
				          (unsigned long)(ulo + ci * ustep));
		return 0;
	}
	if (strcmp(argv[1], "start") == 0) {
#if !BSP_ENABLE_IWDG
		/* The sweep relies on the IWDG to recover a hanging mmap read at a bad
		 * DLYB point; without it a hang is unrecoverable (a permanent lock). */
		cli_error(sh, "psram: mmapscan needs the IWDG (built -DBSP_ENABLE_IWDG=OFF)\r\n");
		return 1;
#else
		if (!psram_ready()) {
			cli_error(sh, "psram: not ready\r\n");
			return 1;
		}
		if (argc >= 3 && (cli_parse_u32(argv[2], &phase) != 0 ||
		    phase < 1u || phase > 12u)) {
			cli_error(sh, "psram: bad phase (1..12)\r\n");
			return 1;
		}
		/* Serialize the config snapshot against a backgrounded psram/membench
		 * command so we capture a coherent clock/latency/MR0 (start does not
		 * return on success, so no release needed then). */
		if (psram_guard(sh))
			return 1;
		cli_print(sh, "psram mmapscan: sweeping phase %lu, unit 0..124 step 4 at "
		          "the current clock/latency; the board will auto-reboot per unit "
		          "(console drops). Reconnect when it stops, then `psram mmapscan "
		          "show`.\r\n", (unsigned long)phase);
		/* Does not return on success (system reset into the boot loop). */
		if (!psram_mmapscan_start(phase, 0u, PSRAM_SCAN_STEP, PSRAM_SCAN_COLS)) {
			psram_release();
			cli_error(sh, "psram: mmapscan start rejected\r\n");
		}
		return 1;
#endif
	}
	cli_error(sh, "psram: usage mmapscan <start [phase]|show|stop>\r\n");
	return 1;
}

/*
 * psram wtune [addr]: sweep the mmap write dummy 0..15 with an 8-byte marker
 * written at addr and read back indirectly (issue #252).  Use it with a read
 * path that is trusted: the shipped setting, or one `psram test` passed.
 *
 * [!] The order: the address (a multiple of 4, all 8 bytes in the 6 MB
 * non-cacheable pool; refused before anything else), the mark, the captured
 * frame dropped (the marker may land in it), the sweep.  Then
 * psram_wtune_end(): with a write path shown, repaint the LCD buffers (the
 * marker may land in them) and give the mark back; otherwise keep the mark
 * until `reboot` (psram.h).
 */
int cmd_psram_wtune(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr = PSRAM_BASE_ADDR + 0x200u;
	uint32_t mask, d;
	int rc, end;

	if (argc >= 2 && cli_parse_u32(argv[1], &addr) != 0) {
		cli_error(sh, "psram: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (!psram_ready()) {
		cli_error(sh, "psram: not ready\r\n");
		return 1;
	}
	if (addr < PSRAM_BASE_ADDR || !psram_wtune_off_ok(addr - PSRAM_BASE_ADDR)) {
		cli_error(sh, "psram: wtune address: a multiple of 4 in "
		              "0x90000000..0x905FFFF8\r\n");
		return 1;
	}
	if (psram_guard(sh))
		return 1;
	camera_frame_invalidate();
	rc = psram_wtune(addr - PSRAM_BASE_ADDR, 4u, &mask);
	end = psram_wtune_end(rc, mask);
	if (end == PSRAM_WTUNE_END_REPAINT_GIVE) {
#if BSP_ENABLE_LCD
		ltdc_fb_repaint_black();
#endif
	}
	if (end != PSRAM_WTUNE_END_KEEP)
		psram_release();
	if (rc == PSRAM_WTUNE_REFUSED) {
		cli_error(sh, "psram: wtune refused\r\n");
		return 1;
	}
	for (d = 0u; d <= 15u; d++)
		cli_print(sh, "  wr dummy %2lu: %s\r\n", (unsigned long)d,
		          (mask & (1u << d)) ? "PASS" : "fail");
	if (end == PSRAM_WTUNE_END_KEEP) {
		cli_error(sh, "psram wtune: %s; OCTOSPI1 held until `reboot`\r\n",
		          rc == PSRAM_WTUNE_DEAD ? "device stopped answering"
		                                 : "nothing round-tripped");
		return 1;
	}
	cli_print(sh, "psram wtune: lowest passing dummy applied; "
	          "run `psram test 4096`\r\n");
	return 0;
}

/* ---- the table (shell/cmds/ext_ram_cmd_board.h) --------------------------- */

static int take_octospi(struct ext_ram_claim *c)
{
	if (psram_acquire())
		return 0;
	c->why = "OCTOSPI1 busy (another psram/membench command or `nn stream` "
	         "holds it, or the LCD is scanning out of it -- run `lcd off`)";
	return -1;
}

static const char *give_octospi(void)
{
	psram_release();
	return NULL;
}

/* [!] Under the mark and before the first write (issue #150). */
static int take_frame(struct ext_ram_claim *c)
{
	(void)c;
	camera_frame_invalidate();
	return 0;
}

#if BSP_ENABLE_LCD
/* [!] Before the mark is given back: the step after the mark's in the table. */
static const char *give_repaint(void)
{
	ltdc_fb_repaint_black();
	return NULL;
}
#endif

static const struct ext_ram_step psram_steps[] = {
	{ take_octospi, give_octospi },
	{ take_frame,   NULL         },
#if BSP_ENABLE_LCD
	{ NULL,         give_repaint },
#endif
};

static const struct ext_ram_range psram_ranges[] = {
	{ 0u,             PSRAM_NC_BYTES, 0u, "LCD, camera" },
	{ PSRAM_NC_BYTES, PSRAM_AI_BYTES, 1u, "nn, membench" },
};

_Static_assert(PSRAM_NC_BYTES + PSRAM_AI_BYTES == PSRAM_SIZE_BYTES,
               "the two ranges are the whole window");

const struct ext_ram_board ext_ram_board = {
	.part     = "APS6408 on OCTOSPI1",
	.base     = PSRAM_BASE_ADDR,
	.size     = PSRAM_SIZE_BYTES,
	.ranges   = psram_ranges,
	.n_ranges = sizeof psram_ranges / sizeof psram_ranges[0],
	.steps    = psram_steps,
	.n_steps  = sizeof psram_steps / sizeof psram_steps[0],
	.ready    = psram_ready,
	.info     = psram_info_lines,
};
