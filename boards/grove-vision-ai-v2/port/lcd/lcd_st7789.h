/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lcd_st7789.h
 * @brief   Waveshare 2inch LCD (ST7789VW, 240x320, 4-wire SPI) on the Grove
 *          Vision AI V2's XIAO header (issue #30, M-G3a).
 *
 * Deliberately NOT a board-independent display abstraction.  The other two
 * boards drive an LTDC-style scanout panel, whose completion and back-pressure
 * behaviour is nothing like a pushed SPI panel's; generalising across two
 * implementations, one of which does not exist yet, would fix the wrong shape.
 * When the camera arrives (M-G3b) the seam that matters is svc/frame_pipeline's
 * frame_sink, which this driver will implement -- not a "display" API.
 *
 * WIRING (2x7 XIAO footprint on the board's underside; see the board README --
 * the silkscreen is misleading, CLK/MISO/MOSI there are the microSD bus):
 *
 *   LCD        silkscreen  pad  HX6538  role
 *   DIN(MOSI)  TXD           8  PB7     SPI_M_DO   (pinmux 8)
 *   CLK        D2            3  PB8     SPI_M_CLK  (pinmux 8)
 *   CS         D1            2  PB11    GPIO2      (pinmux 2, driven by hand)
 *   DC         RXD           7  PB6     GPIO0      (pinmux 1)
 *   RST        D0            1  PA0     AON_GPIO0  (pinmux 2)
 *   BL         SCL           6  PA2     SB_GPIO0   (pinmux 2)
 *   VCC        3V3          12  --
 *   GND        GND          13  --
 *
 * CS is a plain GPIO rather than the SSPI master's own SPI_M_CS so that it can
 * be held asserted across a whole RAMWR burst.  The ST7789 treats a CS release
 * as the end of the memory write; whether the controller keeps its hardware SS
 * low across the DMA descriptor chain is not documented for this part, and
 * finding out the hard way costs a torn frame every time it is wrong.
 */
#ifndef GROVE_LCD_ST7789_H
#define GROVE_LCD_ST7789_H

#include <stdint.h>
#include <stddef.h>

/*
 * Panel geometry in its NATIVE (portrait) orientation.
 *
 * These size the framebuffer and nothing else.  The drawable geometry is
 * lcd_width()/lcd_height(), which follow MADCTL and are 320x240 once the
 * controller is transposing -- the pixel COUNT is the same either way, so one
 * buffer serves both and the placement gate's 153,600 bytes never move.
 */
#define LCD_NATIVE_W 240u
#define LCD_NATIVE_H 320u

/** Create the driver's ThreadX objects.  Call from tx_application_define(),
 *  before any interrupt that touches them can be enabled; lcd_init() refuses
 *  to run if this did not succeed. */
void lcd_create_objects(void);

/**
 * @brief  Take/release exclusive access to the panel.
 *
 * Every entry point here already guards itself, so a caller needs these only
 * to make a SEQUENCE indivisible -- a command that draws many frames in a row,
 * for instance, where another thread slipping a frame in between would be
 * visible on the glass.  The shell runs commands as background jobs
 * (`lcd loop &`), so that is a real thread, not a hypothetical one.
 *
 * Non-blocking: returns -1 immediately when another thread holds the panel.
 * Recursive within one thread, so nesting with the internal guards is fine.
 * Every acquire needs its release, on error paths too.
 */
int  lcd_acquire(void);
void lcd_release(void);

/** Bring the panel up: pinmux, SSPI master, reset pulse, init table, backlight.
 *  Thread context only.  Returns 0 on success, negative on failure (the panel
 *  is left powered down and no interrupt is left enabled). */
int lcd_init(void);

/** 1 once lcd_init() has succeeded. */
int lcd_ready(void);

/** Current SPI clock in Hz as the controller reports it (0 before init). */
uint32_t lcd_sclk_hz(void);

/** Backlight on/off (PA2).  Safe before init only after the pin is muxed. */
void lcd_backlight(int on);

/* ---- orientation (issue #31) --------------------------------------------- */

/*
 * MADCTL (0x36) bits, ST7789VW datasheet section 9.1.36.  Only the four that
 * matter here are named; ML and MH affect the panel's own refresh order, not
 * the address counter, and RGB selects the subpixel order (0 = RGB, which is
 * what the colour bars proved correct on this panel).
 */
#define LCD_MADCTL_MY  0x80u   /* page address order  (row    mirror) */
#define LCD_MADCTL_MX  0x40u   /* column address order (column mirror) */
#define LCD_MADCTL_MV  0x20u   /* row/column EXCHANGE -- the landscape bit */
#define LCD_MADCTL_RGB 0x08u   /* set = BGR subpixel order */

/** Sentinel returned by lcd_rotation() for a raw MADCTL byte that is not one
 *  of the four named rotations. */
#define LCD_ROT_CUSTOM 0xFFFFu

/**
 * @brief  Set MADCTL and take the geometry that follows from it.
 *
 * [!] MEASURED, not inherited.  The Wio port concluded that MADCTL does not
 * rotate (port/ltdc/st7789_rgb.c) -- but that was measured on the RGB PARALLEL
 * interface, where the scan comes from the controller's own timing and the
 * memory write order is a separate thing entirely.  On 4-wire SPI the RAMWR
 * write order IS the address counter, which is exactly what MADCTL steers, so
 * the two situations have nothing in common.
 *
 * Re-measured here with `lcd orient` (issue #31): rotation 90 (MADCTL 0x60)
 * comes back landscape with the origin at the top left -- a true rotation, not a
 * mirror.  The controller accepts a 320-wide window and transposes for free, so
 * this board needs no CPU-side transpose (svc/gfx_rot) at all.
 *
 * The drawable width and height follow the MV bit alone: set means the address
 * counter is transposed, so the window commands span 0..319 horizontally and
 * 0..239 vertically and the framebuffer is walked 320 pixels to the row.  MX/MY
 * mirror within that, which changes where the origin sits but not the shape.
 *
 * Panel-relative geometry is unaffected: this is a 240x320 part with no offset
 * (unlike the 240x240 and 135x240 ST7789 modules, whose windows need a fixed
 * shift per rotation), so no rotation needs a correction here.
 *
 * The panel must be up.  On failure the previous MADCTL and geometry are kept,
 * so the driver's idea of the window never disagrees with the controller's.
 *
 * @return 0 on success, -1 if the panel is down, busy, or the write failed
 */
int lcd_set_madctl(uint8_t madctl);

/**
 * @brief  Set one of the four named rotations.
 *
 * @param degrees 0, 90, 180 or 270, clockwise from the native portrait scan
 * @return 0 on success, -1 on a bad angle or any lcd_set_madctl() failure
 */
int lcd_set_rotation(unsigned degrees);

/**
 * @brief  A CONSISTENT snapshot of everything a report wants to print.
 *
 * [!] Use this, not several of the accessors below, whenever more than one
 * field is shown at once.  The individual accessors are each a single atomic
 * word, but reading four of them in a row is not atomic as a group: the shell
 * runs commands as background jobs, so `lcd rot 90 &` can land between two of
 * them and the line then reports a combination that never existed --
 * "320x240, madctl 60 (rot 0)".  For a command whose entire job is to report
 * what the orientation IS, that is the worst possible failure.
 *
 * Filled inside one critical section, paired with the equally short one in
 * which lcd_set_madctl() publishes the group.  It does NOT take the panel
 * guard, so it still answers while a frame is on the wire.
 */
struct lcd_status {
	int      ready;         /**< lcd_ready() at the instant of the snapshot */
	uint8_t  madctl;        /**< the MADCTL byte programmed               */
	unsigned rotation;      /**< degrees, or LCD_ROT_CUSTOM               */
	uint16_t width, height; /**< drawable geometry for that MADCTL        */
	uint32_t sclk_hz;       /**< SPI clock as the controller reports it   */
};

void lcd_get_status(struct lcd_status *st);

/** Current rotation in degrees, or LCD_ROT_CUSTOM after a raw lcd_set_madctl()
 *  whose byte is not one of the four.  Single values only -- see lcd_status. */
unsigned lcd_rotation(void);

/** The MADCTL byte currently programmed (0x00 before the first init). */
uint8_t lcd_madctl(void);

/** Drawable geometry for the CURRENT orientation.  240x320 natively, 320x240
 *  once MV is set.  lcd_width() * lcd_height() is always
 *  lcd_framebuffer_pixels(). */
uint16_t lcd_width(void);
uint16_t lcd_height(void);

/**
 * @brief  Orientation probe: which corner is the origin, and which way do the
 *         axes run?
 *
 * Colour bars cannot answer this.  Eight vertical stripes look the same
 * transposed as they do mirrored, and they say nothing at all about where (0,0)
 * landed -- so a MADCTL that half worked would read as success.  This pattern
 * is asymmetric in every axis instead:
 *
 *   - a WHITE square marks the origin, pixel (0,0)
 *   - a RED bar runs from it along the +X axis (the top edge, full width)
 *   - a GREEN bar runs from it along the +Y axis (the left edge, full height)
 *   - a BLUE square marks the far corner, (w-1, h-1)
 *
 * All eight MY/MX/MV combinations produce a visibly different picture, and the
 * long side of the image tells you directly whether the controller accepted a
 * landscape window.  Read the glass, then decide.
 *
 * @param stop optional abort poll (Ctrl+C); may be NULL
 * @return 0 on success, -1 on failure, 1 when @p stop asked to stop
 */
int lcd_orient(int (*stop)(void *), void *stop_arg);

/**
 * @brief  Push RGB565 pixels into a window, blocking until the DMA completes.
 *
 * @param x,y,w,h  window in panel coordinates; must lie inside the panel
 * @param pixels   w*h RGB565 samples, big-endian on the wire (ST7789 order)
 * @param stop     optional poll for an abort request (Ctrl+C); may be NULL
 * @return 0 on success, -1 on a parameter/state error, 1 when @p stop asked to
 *         stop (the transfer is halted and the panel left in a defined state)
 */
int lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
             const uint16_t *pixels, int (*stop)(void *), void *stop_arg);

/** Fill the whole panel with one RGB565 colour (no framebuffer needed). */
/**
 * @brief  As lcd_blit(), but for pixels in LITTLE-endian memory order.
 *
 * lcd_blit() hands the caller's buffer to the DMA unchanged, so it needs the
 * ST7789's wire order (big-endian).  The camera's frame pipeline carries
 * FRAME_FMT_RGB565, which is documented little-endian, so something has to
 * swap.  That something is this function, not the producer: a slot published in
 * wire order under a little-endian tag would be a lie that the next sink added
 * to the pipeline discovers the hard way.
 *
 * Copies via the driver's own framebuffer, so @p pixels may live anywhere the
 * CPU can read -- unlike lcd_blit(), whose buffer must be DMA-reachable SRAM.
 *
 * @return as lcd_blit(): 0 on success, -1 on a parameter/state error, 1 when
 *         @p stop asked to stop.
 */
int lcd_blit_le(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                const uint16_t *pixels, int (*stop)(void *), void *stop_arg);

/**
 * @brief  As lcd_blit_le(), with a chance to draw ON the staged frame before it
 *         goes out (issue #48).
 *
 * @p overlay is called once, after @p pixels have been converted into the
 * driver's framebuffer and before the DMA starts, with that framebuffer and its
 * geometry. It is how `nn preview` puts face boxes on a camera frame without a
 * second full-frame buffer and without the caller ever holding the panel guard
 * itself.
 *
 * [!] THE CALLBACK RUNS WITH THE PANEL GUARD HELD, on the caller's thread --
 * the camera producer, in the only user today. That makes it a prohibition, not
 * an invitation:
 *
 *   - it must NOT block, sleep, or wait on anything;
 *   - it must NOT run inference or any other long operation -- it sits between
 *     a staged frame and the wire, and everything else that wants the panel is
 *     failing its non-blocking acquire meanwhile;
 *   - it must NOT take the camera API or frame-pipeline lock;
 *   - it must NOT call any other LCD entry point. The guard is RECURSIVE, so
 *     re-entry would not deadlock -- it would quietly corrupt the transaction
 *     in progress, which is far worse;
 *   - it may write ONLY the framebuffer it is handed, and only within the
 *     w * h pixels it is told about, and must not retain the pointer.
 *
 * The single exception, and the reason it is enough: lcd_rect_wire() below is
 * PURE. It touches no driver state and takes no lock, so calling it from here
 * is not re-entry in any sense that matters.
 *
 * @param overlay  may be NULL, which makes this exactly lcd_blit_le()
 * @param ctx      passed back to @p overlay untouched
 */
int lcd_blit_le_overlay(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint16_t *pixels,
                        void (*overlay)(void *ctx, uint16_t *fb,
                                        uint16_t fb_w, uint16_t fb_h),
                        void *ctx,
                        int (*stop)(void *), void *stop_arg);

/**
 * @brief  Draw a hollow rectangle into a caller-supplied framebuffer.
 *
 * PURE: no driver state, no lock, no hardware -- which is what makes it the one
 * thing an lcd_blit_le_overlay() callback may call. It is here rather than in
 * the caller only because the wire byte order is this driver's knowledge and
 * should stay so.
 *
 * Coordinates are SIGNED and the rectangle is CLIPPED, because a detection box
 * routinely runs past the edge of the image it was found in. A rectangle that
 * clips away entirely draws nothing.
 *
 * @param fb        framebuffer, @p fb_w * @p fb_h pixels, in WIRE order
 * @param x0,y0     top-left, inclusive
 * @param x1,y1     bottom-right, EXCLUSIVE
 * @param rgb565    colour in normal little-endian RGB565; swapped here
 * @param stroke    edge thickness in pixels; a box thinner than twice this
 *                  comes out solid, which is the honest rendering of it
 */
void lcd_rect_wire(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint16_t rgb565, uint16_t stroke);

int lcd_fill(uint16_t rgb565, int (*stop)(void *), void *stop_arg);

/** Colour-bar test pattern: proves the wiring AND the byte order. */
int lcd_bars(int (*stop)(void *), void *stop_arg);

/** The shared framebuffer (SRAM, NOLOAD) the commands draw into. */
uint16_t *lcd_framebuffer(void);
size_t    lcd_framebuffer_pixels(void);

/**
 * @brief  Raw controller + pin state, for diagnosing a blank panel.
 *
 * There is no public TRM for this part and the panel cannot be read back over
 * a 4-wire SPI link, so "the command returned 0" says nothing about whether
 * anything reached the glass.  This prints what CAN be observed: the SSPI
 * master's own registers (is it enabled, is a slave selected, what divider did
 * it actually take, is the TX FIFO draining) and the four GPIO levels.
 * Formatted by the caller through @p out so the driver keeps no CLI dependency.
 */
void lcd_dump_regs(void (*out)(void *ctx, const char *name, uint32_t value),
                   void *ctx);

#endif /* GROVE_LCD_ST7789_H */
