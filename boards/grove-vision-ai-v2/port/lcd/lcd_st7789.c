/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lcd_st7789.c
 * @brief   ST7789VW over the SSPI master (issue #30, M-G3a).
 *
 * See lcd_st7789.h for the wiring and for why this is not a board-independent
 * display abstraction.
 *
 * THE PIXEL PATH.  A full 240x320 RGB565 frame is 153,600 bytes, and the SSPI
 * master's plain DMA entry point (spi_write_dma) tops out at 4095 bytes -- 38
 * chunks per frame if the chaining were done here.  It is not: spi_write_ptl()
 * lands on the DMA controller's dmac_peritransfer_prerolling(), a CIRCULAR
 * LINKED LIST walked by the hardware and refilled by the vendor's own ISR
 * (dw_spi_s_tx_ptl_isr), with a documented ceiling of 256*4095 bytes.  One call
 * per frame, one completion, and no chunk-boundary state machine to get wrong.
 * The vendor entry point also does the D-cache clean on the buffer, so the
 * framebuffer needs no maintenance here.
 *
 * (The fallback, if that path ever proves unusable on hardware, is chunked
 * spi_write_dma behind this same function -- which is why the chunking question
 * does not leak into lcd_blit()'s callers.)
 *
 * CS IS A GPIO, not the controller's SPI_M_CS.  The ST7789 ends a memory write
 * when CS goes high, and nothing documents whether this controller holds its
 * hardware SS across a descriptor chain.  Driving PB11 by hand makes the answer
 * irrelevant: CS goes low before the command and high after the last pixel.
 *
 * INTERRUPTS.  Which lines the SSPI bring-up enables (the SSPI host's own, one
 * or more DMA controller lines) is a property of the prebuilt binary, so they
 * are measured rather than named: the bring-up runs with interrupts masked and
 * port/sdk_seam/epk_irq_wrap.c wraps and registers whatever appeared.  If any
 * of them cannot be accounted for, the whole bring-up is abandoned -- an
 * enabled line with no accounting wrapper is the fail-open issue #25 closed.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "WE2_device.h"
#include "hx_drv_scu.h"
#include "hx_drv_gpio.h"
#include "hx_drv_spi.h"

#include "tx_api.h"

#include "epk_irq_wrap.h"
#include "lcd_st7789.h"
#include "timebase.h"
#include "tx_glue.h"

#define LOG_TAG "lcd"
#include "log.h"

/* ---- configuration ------------------------------------------------------ */

/*
 * Requested SPI clock.  The SSPI master's output is always the reference
 * divided by an EVEN divider and the controller caps at 50 MHz; the SDK's
 * default low-speed source (RC96M48M) therefore lands around 24 MHz, which is
 * ~51 ms for a full frame.  That is the M-G3a target: the plan explicitly does
 * NOT chase frame rate here, and re-sourcing SSPIM from a PLL is a separate
 * issue.  The real number is read back after open() and printed by `lcd`.
 */
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 24000000u
#endif

/* ST7789 datasheet is specified to ~15 MHz; 24-60 MHz is out of spec and in
 * universal use.  Kept as a named constant so the trade is visible. */
#define LCD_SPI_HZ_MAX 50000000u

_Static_assert(LCD_SPI_HZ <= LCD_SPI_HZ_MAX,
               "LCD_SPI_HZ exceeds the SSPI master's 50 MHz ceiling");

/* A frame at ~24 MHz is ~51 ms; a whole second is a fault, not slack. */
#define LCD_DMA_TIMEOUT_TICKS ((ULONG)TX_TIMER_TICKS_PER_SECOND)
/* Poll granularity while a burst is in flight, in ticks (1 ms each here). */
#define LCD_DMA_POLL_TICKS    ((ULONG)1)

/* Pin roles.  GPIO indices follow from the pinmux choice: PB6 mux 1 is GPIO0,
 * PB11 mux 2 is GPIO2, PA0 mux 2 is AON_GPIO0, PA2 mux 2 is SB_GPIO0. */
#define LCD_PIN_DC  GPIO0
#define LCD_PIN_CS  GPIO2
#define LCD_PIN_RST AON_GPIO0
#define LCD_PIN_BL  SB_GPIO0

/* ST7789 commands used here. */
#define ST_SWRESET 0x01
#define ST_SLPOUT  0x11
#define ST_NORON   0x13
#define ST_INVOFF  0x20
#define ST_INVON   0x21
#define ST_DISPON  0x29
#define ST_CASET   0x2A
#define ST_RASET   0x2B
#define ST_RAMWR   0x2C
#define ST_MADCTL  0x36
#define ST_COLMOD  0x3A
#define ST_PORCTRL 0xB2
#define ST_GCTRL   0xB7
#define ST_VCOMS   0xBB
#define ST_LCMCTRL 0xC0
#define ST_VDVVRHEN 0xC2
#define ST_VRHS    0xC3
#define ST_VDVS    0xC4
#define ST_FRCTRL2 0xC6
#define ST_PWCTRL1 0xD0
#define ST_PVGAMCTRL 0xE0
#define ST_NVGAMCTRL 0xE1

/* ---- framebuffer -------------------------------------------------------- */

/*
 * One full-panel RGB565 frame, in SRAM because that is the memory the DMA
 * controller reaches (TCM is CPU-private on this part, exactly as on the Wio).
 * NOLOAD: 150 KB of zeros in the flash image would be 150 KB of flash writes on
 * every `--target flash`, on a part whose external NOR is the thing this
 * project is trying not to wear out.  Because it is NOLOAD it starts as
 * whatever the last boot left, so every producer writes every pixel it claims.
 *
 * check_placement_budget.py pins symbol -> size -> section -> SRAM, the same
 * binding the membench buffers get (issue #26): a framebuffer that quietly
 * landed in DTCM would not fault, it would simply never reach the panel.
 */
#define LCD_FB_PIXELS (LCD_NATIVE_W * LCD_NATIVE_H)

static uint16_t lcd_fb[LCD_FB_PIXELS]
	__attribute__((section(".lcd_fb"), aligned(32)));

_Static_assert(sizeof lcd_fb == 153600u,
               "the framebuffer is not one 240x320 RGB565 frame");

/*
 * Bounce buffer for commands and their parameters.
 *
 * [!] It has to live in SRAM, in the same section as the framebuffer and for
 * the same reason: commands go out over the SAME DMA path as pixels, and the
 * DMA controller cannot see TCM.  The obvious sources both fail that test --
 * a command byte on the stack is in DTCM, and the init table is .rodata in
 * ITCM -- so everything is copied here first.  Sized for the longest entry in
 * the init table (14 gamma bytes) plus the command byte, rounded up.
 */
static uint8_t lcd_cmdbuf[32]
	__attribute__((section(".lcd_fb"), aligned(32)));

uint16_t *lcd_framebuffer(void)      { return lcd_fb; }
size_t    lcd_framebuffer_pixels(void) { return LCD_FB_PIXELS; }

/* ---- state -------------------------------------------------------------- */

static DEV_SPI_PTR lcd_spi;
static uint32_t    lcd_ready_flag;
static uint32_t    lcd_freq_hz;

/*
 * Orientation (issue #31).
 *
 * lcd_w/lcd_h are the DRAWABLE geometry and follow MADCTL's MV bit; every
 * bounds check, window and framebuffer stride in this file reads them rather
 * than LCD_NATIVE_W/H, which now size only the buffer.  The invariant that
 * makes one buffer serve both orientations is lcd_w * lcd_h == LCD_FB_PIXELS,
 * and lcd_madctl_apply() is the ONLY place that assigns either, precisely so
 * that invariant has one owner.
 *
 * They are the native portrait pair until a successful init, because until then
 * nothing may draw anyway (every entry point checks lcd_ready_flag).
 */
static uint8_t     lcd_madctl_val;      /* 0x00 = native portrait, RGB order */
static unsigned    lcd_rot_deg;
static uint16_t    lcd_w = LCD_NATIVE_W;
static uint16_t    lcd_h = LCD_NATIVE_H;

/* Latched when the controller could not be proven stopped after an abort.
 * Re-opening a device in an unknown state risks inheriting a completion that
 * belongs to the previous life of the driver, which would make a later burst
 * report success before its data had gone out -- so the panel simply stays
 * down until the next reboot. */
static uint32_t    lcd_faulted;

/* Signalled from the vendor DMA completion callback (ISR context, so a
 * semaphore put is the ThreadX-legal way to do it) and waited on by the thread
 * that started the burst.  Created in tx_application_define() -- see
 * lcd_create_objects() -- because the port's rule is that ThreadX objects exist
 * before any interrupt that touches them can be enabled. */
static TX_SEMAPHORE lcd_dma_done;
static uint32_t     lcd_objects_ok;

/*
 * Exclusive access to everything in this file.
 *
 * [!] Not theoretical: the shell runs any command as a background job
 * (`lcd loop &`), so a second invocation genuinely can arrive while a transfer
 * is in flight -- on a different thread, sharing the framebuffer, the command
 * bounce buffer, the CS/DC pins, the vendor SPI device and the completion
 * semaphore.  The cheapest of those collisions overwrites pixels the DMA is
 * still reading; the worst has one caller consume the other's completion, or
 * halt the other's chain during its own cancellation, which wedges the vendor
 * driver's busy flag for good.
 *
 * Contenders FAIL rather than queue (TX_NO_WAIT): a background job quietly
 * blocking the console for 51 ms a frame is not better than being told the
 * panel is busy.  ThreadX mutexes are recursive, so a command that holds the
 * guard across a whole loop still nests fine through lcd_fill()/lcd_blit().
 */
static TX_MUTEX lcd_mutex;

/* What the SPI bring-up wrapped, kept so every failure path -- including ones
 * that happen long after lcd_spi_open() returned -- can roll it back. */
static struct epk_irq_wrapset lcd_wrapset;

/* ---- low level ---------------------------------------------------------- */

/* The __DSB() pairs these APB writes with the SPI register writes that follow
 * on a different bus: CS and DC must be settled before the first clock edge,
 * and settled again only after the last one. */
static inline void lcd_cs(int level)
{
	(void)hx_drv_gpio_set_out_value(LCD_PIN_CS,
	                                level ? GPIO_OUT_HIGH : GPIO_OUT_LOW);
	__DSB();
}

static inline void lcd_dc(int level)
{
	(void)hx_drv_gpio_set_out_value(LCD_PIN_DC,
	                                level ? GPIO_OUT_HIGH : GPIO_OUT_LOW);
	__DSB();
}

void lcd_backlight(int on)
{
	(void)hx_drv_gpio_set_out_value(LCD_PIN_BL,
	                                on ? GPIO_OUT_HIGH : GPIO_OUT_LOW);
}

static int lcd_dma_burst(const uint8_t *bytes, uint32_t len,
                         int (*stop)(void *), void *stop_arg);

/*
 * EVERYTHING goes out over the SAME path: spi_write_ptl() + its completion
 * interrupt, for a one-byte command exactly as for a 153,600-byte frame.
 *
 * The first version sent commands through SPI_CMD_TRANSFER_POLLING instead,
 * reasoning that a descriptor chain for five bytes was not worth it.  That is
 * true of the cost and false of the risk: it made the command path a SECOND
 * mechanism, unproven and untested, whose failure looks exactly like a dead
 * panel -- the pixel DMA reports success while no command ever reaches the
 * controller, so it stays in its power-on sleep-in/display-off state with the
 * backlight happily lit.  One mechanism that is known to work beats two, one
 * of which is only assumed to.
 *
 * (spi_write() is not an option either: it is the driver's INTERRUPT method
 * and returns once the transfer is queued, so CS and DC would move while the
 * bytes were still on the wire.)
 */
static int lcd_write_sync(const uint8_t *buf, uint32_t len)
{
	/* Commands are microseconds long and must not be left half-sent, so
	 * they are never abortable -- only the pixel burst is. */
	return lcd_dma_burst(buf, len, NULL, NULL);
}

/*
 * Send a command and its parameters, CS held low across both.
 *
 * The bytes are copied into the SRAM bounce buffer first: the caller's are in
 * ITCM (the init table is .rodata) or on the stack (DTCM), and the DMA
 * controller can see neither.
 */
static int lcd_cmd(uint8_t cmd, const uint8_t *args, uint32_t nargs)
{
	int rc;

	if (nargs + 1u > sizeof lcd_cmdbuf) {
		LOG_ERR("cmd %02x has %lu args, buffer holds %u", cmd,
		        (unsigned long)nargs, (unsigned)sizeof lcd_cmdbuf - 1u);
		return -1;
	}

	lcd_cs(0);
	lcd_dc(0);
	lcd_cmdbuf[0] = cmd;
	rc = lcd_write_sync(lcd_cmdbuf, 1u);
	if (rc == 0 && nargs != 0u) {
		memcpy(lcd_cmdbuf, args, nargs);
		lcd_dc(1);
		rc = lcd_write_sync(lcd_cmdbuf, nargs);
	}
	lcd_cs(1);
	return rc;
}

/* ---- init table --------------------------------------------------------- */

/*
 * Ported from the Wio Lite AI panel bring-up, with RAMCTRL (0xB0) and RGBCTRL
 * (0xB1) DROPPED: those configure the RGB parallel interface this panel does
 * not use, and on a 4-wire SPI part they only invite a mode nobody tests.
 *
 * MADCTL is NOT in this table (issue #31).  It is the one register here whose
 * value is a runtime choice rather than a panel constant, and duplicating it in
 * both places is how the table and lcd_madctl_val drift apart -- a re-init after
 * a failed bring-up would then quietly snap a rotated panel back to portrait
 * while lcd_w/lcd_h still said landscape, and every window after that would be
 * wrong.  lcd_init() applies it once, from the single variable that holds it.
 *
 * Encoded as {cmd, nargs, args...}, terminated by cmd == 0.
 */
static const uint8_t lcd_init_table[] = {
	ST_SLPOUT,   0,
	ST_COLMOD,   1, 0x55,              /* 16 bit/pixel, RGB565            */
	ST_PORCTRL,  5, 0x0C, 0x0C, 0x00, 0x33, 0x33,
	ST_GCTRL,    1, 0x35,
	ST_VCOMS,    1, 0x19,
	ST_LCMCTRL,  1, 0x2C,
	ST_VDVVRHEN, 2, 0x01, 0xFF,
	ST_VRHS,     1, 0x12,
	ST_VDVS,     1, 0x20,
	ST_FRCTRL2,  1, 0x0F,
	ST_PWCTRL1,  2, 0xA4, 0xA1,
	ST_PVGAMCTRL, 14, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
	              0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23,
	ST_NVGAMCTRL, 14, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
	              0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23,
	ST_INVON,    0,                    /* ST7789 panels are inverted      */
	ST_NORON,    0,
	ST_DISPON,   0,
	0
};

static int lcd_run_init_table(void)
{
	const uint8_t *p = lcd_init_table;

	while (*p != 0u) {
		uint8_t cmd   = *p++;
		uint8_t nargs = *p++;

		if (lcd_cmd(cmd, p, nargs) != 0) {
			LOG_ERR("init cmd %02x failed", cmd);
			return -1;
		}
		p += nargs;
		/* SLPOUT needs 120 ms before the next command (datasheet). */
		if (cmd == ST_SLPOUT)
			udelay(120000u);
	}
	return 0;
}

/* ---- orientation (issue #31) --------------------------------------------- */

/*
 * The four named rotations, clockwise from the native portrait scan.
 *
 * MV alone transposes; MX/MY then choose which of the two transposed views is
 * the one that is also rotated rather than mirrored.  These are the values every
 * ST7789 driver in circulation uses, and `lcd orient` confirmed 0x60 on this
 * panel: landscape, origin at the top left, a true rotation (issue #31).
 *
 * `lcd madctl` stays, for the four bit patterns that are not one of these and
 * for re-checking after a rewiring -- trying a byte from the console costs
 * nothing, while compiling one in costs a flash cycle on a NOR rated ~100k.
 */
static const struct {
	unsigned degrees;
	uint8_t  madctl;
} lcd_rot_table[] = {
	{   0u, 0x00u },                                  /* native portrait  */
	{  90u, LCD_MADCTL_MX | LCD_MADCTL_MV },          /* 0x60 landscape   */
	{ 180u, LCD_MADCTL_MY | LCD_MADCTL_MX },          /* 0xC0 portrait    */
	{ 270u, LCD_MADCTL_MY | LCD_MADCTL_MV },          /* 0xA0 landscape   */
};

/*
 * Program MADCTL and adopt the geometry it implies.  Internal: assumes the
 * caller holds the guard and that the SPI is up (lcd_init() calls it before
 * lcd_ready_flag is set, which is why the ready check lives in the public
 * wrapper rather than here).
 *
 * [!] The geometry is updated only AFTER the write succeeded.  A failed MADCTL
 * that still moved lcd_w/lcd_h would leave this driver addressing a 320-wide
 * window on a controller still in 240-wide mode: the CASET would be out of
 * range, the pixels would land wherever the controller decided to clamp, and
 * every layer would report success.
 */
static int lcd_madctl_apply(uint8_t v)
{
	uint16_t w, h;
	unsigned deg;
	size_t   i;

	if (lcd_cmd(ST_MADCTL, &v, 1u) != 0) {
		LOG_ERR("madctl %02x write failed", v);
		return -1;
	}

	/* MV is the whole geometry question: set means the address counter is
	 * transposed, so the panel is addressed 320 wide by 240 tall.  The
	 * pixel count is identical either way, which is what lets one
	 * framebuffer serve both. */
	if ((v & LCD_MADCTL_MV) != 0u) {
		w = LCD_NATIVE_H;
		h = LCD_NATIVE_W;
	} else {
		w = LCD_NATIVE_W;
		h = LCD_NATIVE_H;
	}

	deg = LCD_ROT_CUSTOM;
	for (i = 0u; i < sizeof lcd_rot_table / sizeof lcd_rot_table[0]; i++) {
		if (lcd_rot_table[i].madctl == v) {
			deg = lcd_rot_table[i].degrees;
			break;
		}
	}

	/*
	 * [!] PUBLISHED AS A GROUP.  The four fields are only meaningful
	 * together, and lcd_get_status() reads them together, so a reader that
	 * preempted this function midway would see a byte that does not match
	 * the geometry beside it -- and `lcd` would print a combination that
	 * never existed.  The panel guard does not help: readers deliberately
	 * do not take it, so that a report still answers while a 51 ms frame is
	 * on the wire.  Four scalar stores under a critical section is the
	 * whole cost.
	 */
	{
		TX_INTERRUPT_SAVE_AREA

		TX_DISABLE
		lcd_madctl_val = v;
		lcd_w          = w;
		lcd_h          = h;
		lcd_rot_deg    = deg;
		TX_RESTORE
	}
	return 0;
}

void lcd_get_status(struct lcd_status *st)
{
	TX_INTERRUPT_SAVE_AREA

	if (st == NULL)
		return;

	/* Paired with the publication above: one consistent set, never a mix of
	 * before and after. */
	TX_DISABLE
	st->ready    = lcd_ready_flag ? 1 : 0;
	st->madctl   = lcd_madctl_val;
	st->rotation = lcd_rot_deg;
	st->width    = lcd_w;
	st->height   = lcd_h;
	st->sclk_hz  = lcd_freq_hz;
	TX_RESTORE
}

static void lcd_teardown(void);

int lcd_set_madctl(uint8_t madctl)
{
	int rc;

	if (lcd_acquire() != 0)
		return -1;
	if (!lcd_ready_flag) {
		lcd_release();
		return -1;
	}

	rc = lcd_madctl_apply(madctl);
	if (rc != 0) {
		/*
		 * [!] The panel's orientation is now UNKNOWN, not merely
		 * unchanged, and the difference matters.
		 *
		 * lcd_cmd() puts the 0x36 opcode and its parameter on the wire
		 * as two SEPARATE bursts, so a failure between them leaves a
		 * controller that has already seen the command; and a burst
		 * that failed by TIMING OUT may well have shifted its byte out
		 * regardless.  Neither case is distinguishable from here.
		 *
		 * Keeping the previous value would therefore be a guess, and
		 * the cost of guessing wrong is not a failed command -- it is
		 * every window from now on being programmed against a geometry
		 * the controller does not have, clamped silently while every
		 * layer reports success.  So the device is torn down instead,
		 * exactly as every other bring-up failure in this file is: the
		 * next lcd_init() re-establishes a known state through the
		 * reset pulse and re-applies the orientation from the single
		 * variable that holds it.
		 */
		LOG_ERR("madctl failed; orientation unknown, tearing down");
		lcd_teardown();
	}
	lcd_release();
	return rc;
}

int lcd_set_rotation(unsigned degrees)
{
	size_t i;

	for (i = 0u; i < sizeof lcd_rot_table / sizeof lcd_rot_table[0]; i++) {
		if (lcd_rot_table[i].degrees == degrees)
			return lcd_set_madctl(lcd_rot_table[i].madctl);
	}
	return -1;
}

unsigned lcd_rotation(void) { return lcd_rot_deg; }
uint8_t  lcd_madctl(void)   { return lcd_madctl_val; }
uint16_t lcd_width(void)    { return lcd_w; }
uint16_t lcd_height(void)   { return lcd_h; }

/* ---- bring-up ----------------------------------------------------------- */

static int lcd_pins_init(void)
{
	/* Pinmux.  The second argument enables the pad's automatic pull
	 * configuration, as every SDK example does. */
	if (hx_drv_scu_set_PB7_pinmux(SCU_PB7_PINMUX_SPI_M_DO, 1) != SCU_NO_ERROR ||
	    hx_drv_scu_set_PB8_pinmux(SCU_PB8_PINMUX_SPI_M_CLK, 1) != SCU_NO_ERROR ||
	    hx_drv_scu_set_PB11_pinmux(SCU_PB11_PINMUX_GPIO2, 1) != SCU_NO_ERROR ||
	    hx_drv_scu_set_PB6_pinmux(SCU_PB6_PINMUX_GPIO0_1, 1) != SCU_NO_ERROR ||
	    hx_drv_scu_set_PA0_pinmux(SCU_PA0_PINMUX_AON_GPIO0_2, 1) != SCU_NO_ERROR ||
	    hx_drv_scu_set_PA2_pinmux(SCU_PA2_PINMUX_SB_GPIO0, 1) != SCU_NO_ERROR) {
		LOG_ERR("pinmux failed");
		return -1;
	}

	/* The three GPIO groups these pins live in. */
	if (hx_drv_gpio_init(GPIO_GROUP_0, HX_GPIO_GROUP_0_BASE) != GPIO_NO_ERROR ||
	    hx_drv_gpio_init(GPIO_GROUP_4, HX_GPIO_GROUP_4_BASE) != GPIO_NO_ERROR ||
	    hx_drv_gpio_init(GPIO_GROUP_5, HX_GPIO_GROUP_5_BASE) != GPIO_NO_ERROR) {
		LOG_ERR("gpio group init failed");
		return -1;
	}

	/* CS INACTIVE and the panel HELD IN RESET before anything else drives a
	 * clock edge.  PB11 has no pull-up, so between power-on and this call CS
	 * sits low (asserted) -- harmless only because RST is low too and no
	 * clock is toggling, and this is the line that ends that window. */
	if (hx_drv_gpio_set_output(LCD_PIN_CS,  GPIO_OUT_HIGH) != GPIO_NO_ERROR ||
	    hx_drv_gpio_set_output(LCD_PIN_DC,  GPIO_OUT_LOW)  != GPIO_NO_ERROR ||
	    hx_drv_gpio_set_output(LCD_PIN_RST, GPIO_OUT_LOW)  != GPIO_NO_ERROR ||
	    hx_drv_gpio_set_output(LCD_PIN_BL,  GPIO_OUT_LOW)  != GPIO_NO_ERROR) {
		LOG_ERR("gpio output setup failed");
		return -1;
	}
	return 0;
}

static void lcd_reset_pulse(void)
{
	/* RESX low >= 10 us, then 120 ms before the first command (datasheet).
	 * Generous margins: this runs once. */
	(void)hx_drv_gpio_set_out_value(LCD_PIN_RST, GPIO_OUT_LOW);
	udelay(20000u);
	(void)hx_drv_gpio_set_out_value(LCD_PIN_RST, GPIO_OUT_HIGH);
	udelay(150000u);
}

/* The vendor DMA completion callback.  ISR context: a semaphore put is the
 * only thing that happens here. */
static void lcd_dma_cb(void)
{
	(void)tx_semaphore_put(&lcd_dma_done);
}

/*
 * Undo a bring-up, whole or partial, in the reverse order it was built.
 *
 * The IRQ rollback comes FIRST and the device close second: unwrapping restores
 * the vendor's own vectors while they are still the vendor's, whereas closing
 * first would let spi_close() move a vector out from under a still-registered
 * wrapper -- the state that makes cpu% permanently untrustworthy.
 *
 * Idempotent, so every failure path can call it without tracking how far the
 * bring-up got.
 */
static void lcd_dma_abort_quiesce(void);
static void lcd_dma_drain(void);

static void lcd_teardown(void)
{
	/* Fence any transfer still in flight FIRST.  A bring-up can fail with
	 * the priming DMA unfinished, and a completion that lands after the
	 * teardown would sit in the semaphore waiting to make some later burst
	 * report success before its data had gone out. */
	if (lcd_spi != NULL)
		lcd_dma_abort_quiesce();
	grove_epk_irq_unwrap_set(&lcd_wrapset);
	if (lcd_spi != NULL) {
		(void)lcd_spi->spi_close();
		lcd_spi = NULL;
	}
	lcd_ready_flag = 0u;
}

/*
 * Open the SSPI master and account for whatever interrupts that enabled.
 *
 * [!] THE WHOLE BRING-UP, INCLUDING ONE PRIMING DMA TRANSFER, RUNS INSIDE ONE
 * PRIMASK SECTION.  The first version of this function wrapped only what
 * spi_open() enabled, and `epk` duly reported the accounting as untrustworthy
 * after the first `lcd bar` -- because the SSPI driver initialises its DMA
 * LAZILY: hx_drv_spim_DMA_init() runs inside the FIRST spi_write_ptl(), and
 * that is where the DMA controller's interrupt gets enabled, long after any
 * window this function used to hold open.
 *
 * So a 4-byte transfer is issued here, still masked, purely to force that
 * initialisation to happen where it can be observed.  spi_write_ptl() only arms
 * the descriptor chain and returns, so nothing deadlocks: the completion
 * interrupt goes PENDING while PRIMASK is set and is delivered -- through the
 * wrapper -- the moment it is released.  (grove_epk_irq_wrap_new() therefore
 * must not clear pending bits, and does not.)
 *
 * CS is high throughout, so the panel ignores the priming bytes.
 */
static int lcd_spi_open(void)
{
	struct epk_irq_snapshot snap;
	TX_INTERRUPT_SAVE_AREA
	int      wrapped_ok, primed = 0;
	int32_t  rc = -1;
	uint32_t val = 0u;

	TX_DISABLE
	grove_epk_irq_snapshot(&snap);
	lcd_wrapset.count = 0u;

	if (hx_drv_spi_mst_init(USE_DW_SPI_MST_S, DW_SPI_S_RELBASE) == 0) {
		lcd_spi = hx_drv_spi_mst_get_dev(USE_DW_SPI_MST_S);
		if (lcd_spi != NULL) {
			rc = lcd_spi->spi_open(DEV_MASTER_MODE, LCD_SPI_HZ);
			if (rc == 0) {
				/* A re-open must not inherit a count from the
				 * driver's previous life. */
				lcd_dma_drain();
				/* Priming transfer -- see the note above.  The
				 * source must be DMA-reachable, so it comes out
				 * of the framebuffer rather than the stack
				 * (DTCM, which the DMA cannot see). */
				primed = (lcd_spi->spi_write_ptl(
					          NULL, 0u, lcd_fb, 4u,
					          (void *)lcd_dma_cb) == 0);
			}
		}
	}

	wrapped_ok = grove_epk_irq_wrap_new(&snap, &lcd_wrapset);
	TX_RESTORE

	/*
	 * Interrupts are live again: collect the priming completion so the
	 * driver's internal busy flag is cleared before the first real frame.
	 *
	 * [!] The result is CHECKED, and a timeout fails the whole bring-up.
	 * Ignoring it was a real hole: the priming completion could still be on
	 * its way, and lcd_teardown()/lcd_dma_burst() draining the semaphore
	 * cannot help with a callback that has not arrived yet.  It would land
	 * later, and the first burst after that would return before a single
	 * byte had gone out -- CS and DC moving mid-transfer, which is the
	 * corrupted-frame failure this file has already paid for once.  A
	 * priming transfer that does not finish means the DMA path does not
	 * work, so there is nothing to salvage by continuing.
	 */
	if (primed &&
	    tx_semaphore_get(&lcd_dma_done, LCD_DMA_TIMEOUT_TICKS) != TX_SUCCESS) {
		LOG_ERR("sspi dma priming never completed");
		lcd_teardown();         /* fences the transfer, then unwinds */
		return -1;
	}

	if (lcd_spi == NULL) {
		LOG_ERR("sspi master init/dev failed");
		lcd_teardown();
		return -1;
	}
	if (rc != 0) {
		LOG_ERR("sspi open failed (%ld)", (long)rc);
		lcd_teardown();
		return -1;
	}
	if (!primed) {
		LOG_ERR("sspi dma priming transfer was rejected");
		lcd_teardown();
		return -1;
	}
	if (!wrapped_ok) {
		/* Some line the driver enabled could not be wrapped or
		 * registered.  grove_epk_irq_wrap_new() left the FAILED lines
		 * disabled, but the ones that succeeded are live and registered
		 * -- so the whole attempt is rolled back, not just the SPI
		 * device.  Closing the device while its wrappers stayed
		 * registered would leave the registry pointing at vectors
		 * spi_close() may have changed, and `thread` would read "--"
		 * until the next reboot. */
		LOG_ERR("sspi irq accounting failed; rolling the bring-up back");
		lcd_teardown();
		return -1;
	}

	/* Mode 0 (CPOL=0, CPHA=0) is what the ST7789 clocks on. */
	val = SPI_CLK_MODE_0;
	(void)lcd_spi->spi_control(SPI_CMD_SET_CLK_MODE, (SPI_CTRL_PARAM)&val);

	/* The divider is even and the source is whatever the bootloader left, so
	 * the achieved clock is rarely the requested one.  Report the real one
	 * rather than the wish -- `lcd` prints it and the plan's fps arithmetic
	 * only means anything against a measured SCLK. */
	if (lcd_spi->spi_control(SPI_CMD_MST_GET_CURRENT_FREQ,
	                         (SPI_CTRL_PARAM)&val) == 0)
		lcd_freq_hz = val;
	else
		lcd_freq_hz = 0u;

	return 0;
}

int lcd_init(void)
{
	uint32_t src_hz = 0u;

	if (!lcd_objects_ok) {
		LOG_ERR("ThreadX objects were not created at boot");
		return -1;
	}
	if (lcd_faulted) {
		LOG_ERR("panel is latched faulted; reboot to retry");
		return -1;
	}
	if (lcd_acquire() != 0)
		return -1;              /* another thread owns the panel */
	if (lcd_ready_flag) {
		lcd_release();
		return 0;
	}

	if (lcd_pins_init() != 0 || lcd_spi_open() != 0) {
		lcd_teardown();
		lcd_release();
		return -1;
	}

	lcd_reset_pulse();
	/* MADCTL is applied from lcd_madctl_val rather than from the table, so a
	 * re-init after a failed bring-up restores the orientation that was set
	 * rather than silently reverting to portrait behind lcd_w/lcd_h.  The
	 * reset pulse put the controller back at 0x00, so this is a real write
	 * even when the value has not changed. */
	if (lcd_run_init_table() != 0 || lcd_madctl_apply(lcd_madctl_val) != 0) {
		/* The SPI came up and its interrupts are wrapped; the PANEL is
		 * what failed.  Roll the whole thing back rather than leaving
		 * live wrappers around a device about to be closed. */
		lcd_teardown();
		lcd_release();
		return -1;
	}

	if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_LSC_SSPIM, &src_hz)
	    != SCU_NO_ERROR)
		src_hz = 0u;

	lcd_ready_flag = 1u;
	lcd_backlight(1);
	LOG_INF("st7789 up: sclk %lu Hz (source %lu Hz), %ux%u madctl %02x, "
	        "fb %u B in SRAM",
	        (unsigned long)lcd_freq_hz, (unsigned long)src_hz,
	        (unsigned)lcd_w, (unsigned)lcd_h, lcd_madctl_val,
	        (unsigned)sizeof lcd_fb);
	lcd_release();
	return 0;
}

int lcd_ready(void)          { return lcd_ready_flag ? 1 : 0; }
uint32_t lcd_sclk_hz(void)   { return lcd_freq_hz; }

/* Create the ThreadX objects the driver needs.  Called from
 * tx_application_define(), i.e. before any interrupt that touches them can be
 * enabled -- lcd_init() refuses to run if this did not succeed. */
void lcd_create_objects(void)
{
	if (tx_semaphore_create(&lcd_dma_done, "lcd_dma", 0) != TX_SUCCESS) {
		LOG_ERR("lcd dma semaphore create failed");
		return;
	}
	/* TX_NO_INHERIT: the guard is never waited on (every caller uses
	 * TX_NO_WAIT), so there is no priority to inherit. */
	if (tx_mutex_create(&lcd_mutex, "lcd", TX_NO_INHERIT) != TX_SUCCESS) {
		LOG_ERR("lcd mutex create failed");
		return;
	}
	lcd_objects_ok = 1u;
}

int lcd_acquire(void)
{
	if (!lcd_objects_ok)
		return -1;
	return (tx_mutex_get(&lcd_mutex, TX_NO_WAIT) == TX_SUCCESS) ? 0 : -1;
}

void lcd_release(void)
{
	if (lcd_objects_ok)
		(void)tx_mutex_put(&lcd_mutex);
}

/* ---- controller status --------------------------------------------------- */

/*
 * The SSPI master's register block, secure alias.  Offsets are the DesignWare
 * SSI layout the SDK's own DW_SPI_REG struct declares (hx_drv_spi.h).  Read
 * directly rather than through the driver because the questions these answer
 * are "did the driver do what it said" and "is the shift register empty YET",
 * and asking the driver to grade itself is not an answer to either.
 */
#define SSPI_BASE     0x50800000UL
#define SSPI_CTRLR0   0x00u
#define SSPI_CTRLR1   0x04u
#define SSPI_SSIENR   0x08u
#define SSPI_SER      0x10u
#define SSPI_BAUDR    0x14u
#define SSPI_TXFLR    0x20u
#define SSPI_RXFLR    0x24u
#define SSPI_SR       0x28u
#define SSPI_DMACR    0x4Cu

_Static_assert(SSPI_BASE == (uint32_t)DW_SPI_S_RELBASE,
               "the SSPI base moved; lcd_dump_regs would read the wrong block");

#define SSPI_RD(off) (*(volatile uint32_t *)(SSPI_BASE + (off)))

/* SR bits (DesignWare SSI). */
#define SSPI_SR_BUSY (1u << 0)
#define SSPI_SR_TFE  (1u << 2)

/*
 * Spin until the controller has finished SHIFTING, not merely finished being
 * fed.
 *
 * [!] This is load-bearing, and its absence is what made the panel stay black
 * while every layer above reported success.  The DMA completion callback fires
 * when the last byte has been handed to the SPI TX FIFO -- NOT when it has
 * left the pin.  For a one-byte command that is essentially immediate, so
 * without this wait lcd_cmd() would raise DC to "data" while the command byte
 * was still in the shift register: the panel then sees every command as data
 * and nothing is ever configured.  The same hazard ends a frame early, with CS
 * released mid-pixel.
 *
 * Bounded so a wedged controller reports rather than hangs the shell.  The
 * bound is generous: 4095 bytes at the slowest plausible SCLK is well under a
 * millisecond, and this only ever waits out the FIFO tail (a few bytes).
 */
#define SSPI_IDLE_SPINS 1000000u

static int lcd_spi_wait_idle(void)
{
	uint32_t i;

	for (i = 0u; i < SSPI_IDLE_SPINS; i++) {
		uint32_t sr = SSPI_RD(SSPI_SR);

		if ((sr & SSPI_SR_BUSY) == 0u && (sr & SSPI_SR_TFE) != 0u)
			return 0;
	}
	LOG_ERR("sspi never went idle (SR %08lx)",
	        (unsigned long)SSPI_RD(SSPI_SR));
	return -1;
}

/* ---- diagnostics -------------------------------------------------------- */

void lcd_dump_regs(void (*out)(void *ctx, const char *name, uint32_t value),
                   void *ctx)
{
	uint8_t v = 0u;

	if (out == NULL)
		return;

	out(ctx, "SSPI CTRLR0", SSPI_RD(SSPI_CTRLR0));
	out(ctx, "SSPI CTRLR1", SSPI_RD(SSPI_CTRLR1));
	out(ctx, "SSPI SSIENR", SSPI_RD(SSPI_SSIENR));
	out(ctx, "SSPI SER   ", SSPI_RD(SSPI_SER));
	out(ctx, "SSPI BAUDR ", SSPI_RD(SSPI_BAUDR));
	out(ctx, "SSPI TXFLR ", SSPI_RD(SSPI_TXFLR));
	out(ctx, "SSPI RXFLR ", SSPI_RD(SSPI_RXFLR));
	out(ctx, "SSPI SR    ", SSPI_RD(SSPI_SR));
	out(ctx, "SSPI DMACR ", SSPI_RD(SSPI_DMACR));

	/* Pin levels, read back through the driver so they reflect what the
	 * GPIO block actually holds rather than what this file last wrote. */
	(void)hx_drv_gpio_get_in_value(LCD_PIN_CS, &v);
	out(ctx, "CS  (PB11) ", v);
	v = 0u;
	(void)hx_drv_gpio_get_in_value(LCD_PIN_DC, &v);
	out(ctx, "DC  (PB6)  ", v);
	v = 0u;
	(void)hx_drv_gpio_get_in_value(LCD_PIN_RST, &v);
	out(ctx, "RST (PA0)  ", v);
	v = 0u;
	(void)hx_drv_gpio_get_in_value(LCD_PIN_BL, &v);
	out(ctx, "BL  (PA2)  ", v);

	/*
	 * The pinmux, read back rather than assumed.  A mux write that did not
	 * take is invisible everywhere else: the SPI controller still clocks
	 * happily, the DMA still completes, every call still returns 0 -- the
	 * signals just never leave the die.  Expected: PB7=8 (SPI_M_DO),
	 * PB8=8 (SPI_M_CLK), PB11=2 (GPIO2), PB6=1 (GPIO0), PA0=2 (AON_GPIO0),
	 * PA2=2 (SB_GPIO0).
	 */
	{
		SCU_PINMUX_CFG_T pinmux;

		if (hx_drv_scu_get_all_pinmux_cfg(&pinmux) == SCU_NO_ERROR) {
			out(ctx, "mux PB7    ", (uint32_t)pinmux.pin_pb7);
			out(ctx, "mux PB8    ", (uint32_t)pinmux.pin_pb8);
			out(ctx, "mux PB11   ", (uint32_t)pinmux.pin_pb11);
			out(ctx, "mux PB6    ", (uint32_t)pinmux.pin_pb6);
			out(ctx, "mux PA0    ", (uint32_t)pinmux.pin_pa0);
			out(ctx, "mux PA2    ", (uint32_t)pinmux.pin_pa2);
		}
	}
}

/* ---- pixel path --------------------------------------------------------- */

static int lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint8_t a[4];
	uint16_t x1 = (uint16_t)(x + w - 1u);
	uint16_t y1 = (uint16_t)(y + h - 1u);

	a[0] = (uint8_t)(x >> 8);  a[1] = (uint8_t)x;
	a[2] = (uint8_t)(x1 >> 8); a[3] = (uint8_t)x1;
	if (lcd_cmd(ST_CASET, a, 4u) != 0)
		return -1;

	a[0] = (uint8_t)(y >> 8);  a[1] = (uint8_t)y;
	a[2] = (uint8_t)(y1 >> 8); a[3] = (uint8_t)y1;
	if (lcd_cmd(ST_RASET, a, 4u) != 0)
		return -1;
	return 0;
}

/* Consume any completion left over from an earlier transfer.  Cheap, and the
 * last line of defence for the invariant every burst depends on: the semaphore
 * is zero when a transfer is armed. */
static void lcd_dma_drain(void)
{
	while (tx_semaphore_get(&lcd_dma_done, TX_NO_WAIT) == TX_SUCCESS)
		;
}

/*
 * Bring an aborted transfer to a stop that can be REASONED about, not just
 * waited out.
 *
 * spi_write_ptl_halt() races the transfer it is stopping: its completion may
 * have fired already, may be pending at the NVIC, or may be moments away.
 * Simply sleeping and draining leaves the interesting case -- a callback
 * arriving after the drain -- entirely to timing, and its consequence is
 * severe: the next burst's tx_semaphore_get() returns instantly, so CS and DC
 * move while that transfer's data is still going out.  That is the same class
 * of bug as the FIFO-versus-wire one above, and it would surface as an
 * occasional corrupt frame after a Ctrl+C.
 *
 * So the completion is fenced instead of timed.  The interrupts the SPI
 * bring-up enabled are known exactly -- grove_epk_irq_wrap_new() recorded them
 * in lcd_wrapset -- so they can be masked while the controller is confirmed
 * idle and any pending completion is discarded at its source.  After this
 * returns, no callback from the aborted transfer can still be in flight.
 *
 * Masking is safe for the accounting: the registry permits a registered line to
 * be disabled (an idle peripheral bills nothing), and the lines go straight
 * back on.
 */
static void lcd_dma_abort_quiesce(void)
{
	uint32_t i;
	int idle;

	(void)lcd_spi->spi_write_ptl_halt();

	for (i = 0u; i < lcd_wrapset.count; i++)
		NVIC_DisableIRQ((IRQn_Type)lcd_wrapset.irqn[i]);
	__DSB();
	__ISB();

	/* Has it actually stopped?  Everything below is only valid if it has:
	 * clearing pending and draining while the controller is STILL SHIFTING
	 * just means the completion arrives after the clear, and re-enabling
	 * the line then lets it land -- reopening the very hole this function
	 * exists to close. */
	idle = lcd_spi_wait_idle();

	for (i = 0u; i < lcd_wrapset.count; i++)
		NVIC_ClearPendingIRQ((IRQn_Type)lcd_wrapset.irqn[i]);
	lcd_dma_drain();

	if (idle != 0) {
		/* [!] Not proven stopped: leave the lines MASKED and latch the
		 * panel as faulted.  The lines stay registered (the registry
		 * permits registered-but-disabled), so the accounting stays
		 * consistent; what is refused is any further use of a device
		 * that cannot be brought to a known state.  lcd_teardown()
		 * unwraps them right after this, and lcd_init() will not
		 * restart until the fault is cleared by a reboot. */
		lcd_faulted = 1u;
		LOG_ERR("sspi would not go idle after halt; panel disabled");
		return;
	}

	for (i = 0u; i < lcd_wrapset.count; i++)
		NVIC_EnableIRQ((IRQn_Type)lcd_wrapset.irqn[i]);
	__DSB();
	__ISB();
}

/*
 * One DMA burst, CS held low for its whole length.
 *
 * The completion path is exactly-once by construction: either spi_write_ptl()
 * returns non-zero (nothing was started, no callback will come) or it returns 0
 * and the callback fires precisely once.  A timeout is treated as a hardware
 * fault rather than retried -- a second burst on top of a stuck one would leave
 * the panel mid-RAMWR with no way back.
 *
 * [!] The wait is SLICED rather than one long block, so @p stop can abort a
 * transfer that is still on the wire.  A whole frame is a single 51 ms
 * descriptor chain here, so waiting it out and only then noticing Ctrl+C would
 * mean the abort never actually exercises the in-flight path -- which is
 * precisely the path M-G3b's Ctrl+C handling will depend on.
 *
 * Returns 0 on completion, 1 when @p stop asked to stop (the chain is halted
 * and the semaphore drained), -1 on failure.
 */
static int lcd_dma_burst(const uint8_t *bytes, uint32_t len,
                         int (*stop)(void *), void *stop_arg)
{
	ULONG waited;

	/* Arm only with the semaphore at zero: a stale count would make the
	 * wait below return before a single byte had gone out. */
	lcd_dma_drain();

	if (lcd_spi->spi_write_ptl(NULL, 0u, bytes, len,
	                           (void *)lcd_dma_cb) != 0) {
		LOG_ERR("spi_write_ptl rejected %lu bytes", (unsigned long)len);
		return -1;
	}

	for (waited = 0u; waited < LCD_DMA_TIMEOUT_TICKS;
	     waited += LCD_DMA_POLL_TICKS) {
		if (tx_semaphore_get(&lcd_dma_done, LCD_DMA_POLL_TICKS)
		    == TX_SUCCESS) {
			/* [!] The callback means "the FIFO has it", not "the pin
			 * has sent it".  Every caller changes DC or CS as soon
			 * as this returns, so the shift register has to be
			 * empty first -- see lcd_spi_wait_idle(). */
			return lcd_spi_wait_idle();
		}
		if (stop != NULL && stop(stop_arg)) {
			lcd_dma_abort_quiesce();
			return 1;
		}
	}

	/* Stop the descriptor chain before returning, so the next call does not
	 * inherit a half-finished transfer. */
	lcd_dma_abort_quiesce();
	LOG_ERR("spi dma did not complete within %lu ticks",
	        (unsigned long)LCD_DMA_TIMEOUT_TICKS);
	return -1;
}

int lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
             const uint16_t *pixels, int (*stop)(void *), void *stop_arg)
{
	uint32_t total;
	int rc;

	if (pixels == NULL)
		return -1;

	/* Exclusive for the WHOLE transaction: window, RAMWR and pixels are one
	 * indivisible conversation with the controller, and the bounce buffer
	 * and CS/DC belong to it throughout.  Recursive, so a caller already
	 * holding the guard (lcd_fill, or a command spanning a loop) nests. */
	if (lcd_acquire() != 0)
		return -1;

	/*
	 * [!] VALIDATED INSIDE THE GUARD, and it has to be.  Since issue #31
	 * both lcd_ready_flag and the geometry are things another thread can
	 * change -- `lcd rot 90` moves lcd_w/lcd_h, a teardown clears ready --
	 * so anything checked before the acquire describes a panel state that
	 * need not still hold when the window is programmed a moment later.  A
	 * blit accepted against 320x240 and then issued to a controller since
	 * put back to 240 wide runs CASET past its range: the controller
	 * clamps, the pixels land somewhere else, and every layer still reports
	 * success.  That is the silent-corruption shape this file keeps paying
	 * for, so the check moved inside.
	 *
	 * The bounds are the CURRENT orientation's, not the native one --
	 * checking against 240 would reject every legitimate landscape blit.
	 */
	if (!lcd_ready_flag || w == 0u || h == 0u ||
	    (uint32_t)x + w > lcd_w || (uint32_t)y + h > lcd_h) {
		lcd_release();
		return -1;
	}

	if (stop != NULL && stop(stop_arg)) {
		lcd_release();
		return 1;
	}

	if (lcd_set_window(x, y, w, h) != 0) {
		lcd_release();
		return -1;
	}

	total = (uint32_t)w * (uint32_t)h * 2u;

	lcd_cs(0);
	lcd_dc(0);
	lcd_cmdbuf[0] = ST_RAMWR;
	if (lcd_write_sync(lcd_cmdbuf, 1u) != 0) {
		lcd_cs(1);
		lcd_release();
		return -1;
	}
	lcd_dc(1);

	/* An aborted RAMWR is still ended properly: CS goes high, which is how
	 * the ST7789 is told the memory write is over.  The controller simply
	 * has fewer pixels than the window asked for, and the next blit sets
	 * the window again. */
	rc = lcd_dma_burst((const uint8_t *)pixels, total, stop, stop_arg);
	lcd_cs(1);
	lcd_release();
	return (rc == 0) ? 0 : ((rc == 1) ? 1 : -1);
}

/*
 * RGB565 on the wire is big-endian: the ST7789 takes the high byte first.  The
 * framebuffer is uint16_t in little-endian memory, so every value stored into
 * it is byte-swapped here, once, at the point it is written.  Doing it in the
 * producer rather than in a pass over the buffer keeps the frame path to a
 * single write of each pixel.
 */
static inline uint16_t lcd_wire(uint16_t rgb565)
{
	return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

int lcd_fill(uint16_t rgb565, int (*stop)(void *), void *stop_arg)
{
	uint16_t wire = lcd_wire(rgb565);
	size_t i;
	int rc;

	/* Held across the FILL as well as the blit: writing the framebuffer
	 * while another caller's DMA is reading it is the cheapest way to get
	 * a torn frame, and the guard is what makes that impossible.  The ready
	 * test is inside it for the reason given in lcd_blit(). */
	if (lcd_acquire() != 0)
		return -1;
	if (!lcd_ready_flag) {
		lcd_release();
		return -1;
	}
	for (i = 0u; i < LCD_FB_PIXELS; i++)
		lcd_fb[i] = wire;
	rc = lcd_blit(0, 0, lcd_w, lcd_h, lcd_fb, stop, stop_arg);
	lcd_release();
	return rc;
}

/*
 * Fill a rectangle in the framebuffer, in the current orientation's
 * coordinates.  Clipped rather than trusted: lcd_w/lcd_h are runtime values
 * now, and a producer that assumed the other orientation would otherwise walk
 * off the end of a buffer that is exactly one frame long.
 *
 * Caller holds the guard.
 */
static void lcd_fb_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        uint16_t rgb565)
{
	uint16_t wire = lcd_wire(rgb565);
	uint32_t xe = (uint32_t)x + w;
	uint32_t ye = (uint32_t)y + h;
	uint32_t xx, yy;

	if (xe > lcd_w)
		xe = lcd_w;
	if (ye > lcd_h)
		ye = lcd_h;

	for (yy = y; yy < ye; yy++)
		for (xx = x; xx < xe; xx++)
			lcd_fb[(size_t)yy * lcd_w + xx] = wire;
}

/*
 * The orientation probe -- see lcd_st7789.h for what each mark means and why
 * the colour bars cannot answer this question.
 */
#define LCD_ORIENT_BAR  16u     /* thickness of the two axis bars      */
#define LCD_ORIENT_MARK 32u     /* the origin square                   */
#define LCD_ORIENT_FAR  24u     /* the far-corner square               */

int lcd_orient(int (*stop)(void *), void *stop_arg)
{
	int rc;

	if (lcd_acquire() != 0)
		return -1;
	if (!lcd_ready_flag) {
		lcd_release();
		return -1;
	}

	lcd_fb_rect(0u, 0u, lcd_w, lcd_h, 0x0000u);              /* black   */
	lcd_fb_rect(0u, 0u, lcd_w, LCD_ORIENT_BAR, 0xF800u);     /* +X red  */
	lcd_fb_rect(0u, 0u, LCD_ORIENT_BAR, lcd_h, 0x07E0u);     /* +Y green*/
	/* Drawn last so it sits on top of both bars: the origin has to be
	 * unmistakable even where the two overlap. */
	lcd_fb_rect(0u, 0u, LCD_ORIENT_MARK, LCD_ORIENT_MARK, 0xFFFFu);
	lcd_fb_rect((uint16_t)(lcd_w - LCD_ORIENT_FAR),
	            (uint16_t)(lcd_h - LCD_ORIENT_FAR),
	            LCD_ORIENT_FAR, LCD_ORIENT_FAR, 0x001Fu);    /* far blue*/

	rc = lcd_blit(0, 0, lcd_w, lcd_h, lcd_fb, stop, stop_arg);
	lcd_release();
	return rc;
}

/*
 * Colour bars.  This is the wiring test, not decoration: eight vertical bars in
 * a known order prove that DIN/CLK reach the panel at all, that CS and DC are
 * on the pins this driver thinks they are, and -- because the bars are pure
 * primaries -- that the RGB565 byte order is right.  A swapped pair of bytes
 * turns red into a dark green-blue rather than into noise, which is exactly the
 * kind of failure that survives a "looks like it works" glance.
 */
int lcd_bars(int (*stop)(void *), void *stop_arg)
{
	static const uint16_t bar[8] = {
		0xFFFF, /* white   */
		0xFFE0, /* yellow  */
		0x07FF, /* cyan    */
		0x07E0, /* green   */
		0xF81F, /* magenta */
		0xF800, /* red     */
		0x001F, /* blue    */
		0x0000, /* black   */
	};
	uint16_t xpix, ypix;
	int rc;

	if (lcd_acquire() != 0)
		return -1;
	if (!lcd_ready_flag) {
		lcd_release();
		return -1;
	}

	for (ypix = 0u; ypix < lcd_h; ypix++) {
		for (xpix = 0u; xpix < lcd_w; xpix++)
			lcd_fb[(size_t)ypix * lcd_w + xpix] =
				lcd_wire(bar[(xpix * 8u) / lcd_w]);
	}
	rc = lcd_blit(0, 0, lcd_w, lcd_h, lcd_fb, stop, stop_arg);
	lcd_release();
	return rc;
}
