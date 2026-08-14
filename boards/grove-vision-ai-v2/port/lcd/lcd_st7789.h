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

/* Panel geometry in its native (portrait) orientation. */
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
