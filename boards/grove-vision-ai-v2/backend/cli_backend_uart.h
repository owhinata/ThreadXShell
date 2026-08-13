/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_backend_uart.h
 * @brief   UART0 (CH343P bridge) interrupt-driven transport for the shell.
 *
 * A `struct cli_transport_api` implementation over the Himax SDK's DesignWare
 * UART driver (hx_drv_uart, prebuilt in libdriver.a).  It does NOT initialise
 * the pinmux (PB0/PB1 are muxed by the SDK's pinmux_init() from main), but it
 * DOES own uart_open(): baud, callbacks and the NVIC enable all happen in the
 * backend's enable(), which the shell core calls on the shell thread after the
 * scheduler starts -- keeping the "interrupts only after TX objects" rule.
 *
 *   - RX (primary mode): UART_CMD_SET_RXCB with a NULL RX buffer -- the SDK
 *     header specifies the callback then fires on the DATA_AVAIL interrupt.
 *     The callback drains the FIFO with uart_read_nonblock() into
 *     @ref cli_grove_uart::rx_ring (producer = UART0 ISR, consumer = shell
 *     thread -- SPSC).
 *   - TX: write() enqueues into @ref cli_grove_uart::tx_ring and hands the
 *     driver the contiguous run via UART_CMD_SET_TXINT_BUF + SET_TXINT(1);
 *     the TX-done callback advances the tail and starts the next chunk, or
 *     parks TX (SET_TXINT(0)).  Two producers (shell thread + printf _write),
 *     so head/in-flight state is guarded by a short PRIMASK critical section.
 *
 * The interrupt path is specified by the SDK header and has no usage
 * precedent in the vendor tree, so it was brought up as the primary with a
 * planned retreat.  It WORKS: verified on hardware 2026-08-13 (interactive
 * echo, line editing and Ctrl-C at 921600).  The retreat -- a 1-byte
 * uart_read_udma() re-arm -- stays documented because it needs a detail that
 * is easy to miss: the DMA3 combined IRQ (69) must be enabled too, not just
 * the UART0 IRQ (90).  See the enable() comment.
 *
 * Concurrency rests on this port's ThreadX critical sections being
 * PRIMASK-based (TX_PORT_USE_BASEPRI undefined; see port/threadx), so a UART
 * ISR calling cli_transport_notify_rx/tx (tx_event_flags_set) is safe at any
 * NVIC priority.  The byte ring (cli_uart_ring.h) is shared, host-tested code.
 */
#ifndef CLI_BACKEND_UART_H
#define CLI_BACKEND_UART_H

#include <stddef.h>
#include <stdint.h>

#include "cli_instance.h"    /* struct cli_transport[_api], cli_transport_notify_* */
#include "cli_uart_ring.h"   /* struct cli_uart_ring + lock-free helpers */

#ifdef __cplusplus
extern "C" {
#endif

/* RX ring depth (bytes).  Holds CLI_GROVE_RX_BUFFER_SIZE-1; a burst that
 * outruns the shell thread is dropped + counted. */
#ifndef CLI_GROVE_RX_BUFFER_SIZE
#define CLI_GROVE_RX_BUFFER_SIZE 1024
#endif

/* TX ring depth (bytes).  Sized to hold the whole boot banner enqueued before
 * enable() arms TX, plus normal command output. */
#ifndef CLI_GROVE_TX_BUFFER_SIZE
#define CLI_GROVE_TX_BUFFER_SIZE 4096
#endif

/** Backend-private context (the `ctx` of a UART transport). */
/**
 * Console RX-drop and driver-error counts, for the `version` command.
 * @return 1 with both outputs written; 0 if no console is bound yet.
 */
int cli_grove_uart_stats(uint32_t *rx_dropped, uint32_t *err_events);

struct cli_grove_uart {
	void                *dev;     /**< DEV_UART_PTR, bound in enable() */
	struct cli_instance *sh;      /**< owning instance (cached from tr->sh) */

	/* RX: producer = UART0 ISR (driver callback), consumer = shell thread. */
	struct cli_uart_ring rx_ring;
	uint8_t  rx_buf[CLI_GROVE_RX_BUFFER_SIZE];

	/* TX: producers = shell thread + _write, consumer = TX-done callback. */
	struct cli_uart_ring tx_ring;
	uint8_t  tx_buf[CLI_GROVE_TX_BUFFER_SIZE];
	volatile uint8_t tx_in_flight; /**< a TXINT_BUF chunk is in progress */
	uint32_t tx_chunk;             /**< length of the in-flight chunk */

	volatile uint8_t enabled;      /**< enable() succeeded; IRQ armed */
	uint32_t rx_dropped_ring;      /**< ring-full drops (also sh->rx_dropped) */
	uint32_t err_events;           /**< driver err_cb invocations */
};

/** The UART transport vtable (init/enable/write/read/uninit). */
extern const struct cli_transport_api cli_grove_uart_api;

/**
 * Statically define a UART0 transport @p _name.  Bind it to an instance with
 * CLI_INSTANCE_DEFINE(inst, &_name, "prompt> ").
 */
#define CLI_BACKEND_UART_DEFINE(_name)                                        \
	static struct cli_grove_uart _name##_ctx;                             \
	static struct cli_transport  _name = { &cli_grove_uart_api, NULL,     \
	                                       &_name##_ctx, 0 }

/* A ring needs >= 2 bytes (one slot is the full/empty sentinel). */
_Static_assert(CLI_GROVE_RX_BUFFER_SIZE >= 2, "RX ring must be >= 2");
_Static_assert(CLI_GROVE_TX_BUFFER_SIZE >= 2, "TX ring must be >= 2");

#ifdef __cplusplus
}
#endif

#endif /* CLI_BACKEND_UART_H */
