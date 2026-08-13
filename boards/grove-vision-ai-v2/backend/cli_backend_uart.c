/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_backend_uart.c
 * @brief   UART0 interrupt-driven transport implementation (Grove Vision AI V2).
 *
 * Implements struct cli_transport_api over the Himax DW UART driver (prebuilt
 * libdriver.a) plus the printf (_write) retarget that shares the single TX
 * ring.  See cli_backend_uart.h for the threading model and the primary /
 * fallback RX-path decision; shell/backend/cli_uart_ring.h for the
 * (host-tested) ring helpers.
 *
 * Single console: UART0 is the only console on this board (the USB-C is a
 * CH343P bridge into it), so the file-global @ref g_uart_console resolves the
 * driver callbacks (which carry no context) back to the owning context, and
 * _write routes through it.
 */
#include "cli_backend_uart.h"
#include "cli_internal.h"    /* cli_out_begin/cli_out_end + cli_xfer_active */

#include "WE2_device.h"
#include "hx_drv_uart.h"

#define LOG_TAG "uart"
#include "log.h"

/* The one active UART console (set in init).  The driver callbacks and _write
 * reach the context through this; NULL until the first cli_init(). */
static struct cli_grove_uart *g_uart_console;

/* UART0 IRQ priority: above SysTick (6) for echo latency / FIFO headroom; any
 * value is ThreadX-safe under this port's PRIMASK critical sections.
 * __NVIC_PRIO_BITS = 3 on this device. */
#define CLI_GROVE_UART_IRQ_PRIORITY 2u

/* Console baud: the board-wide convention (bootloader menu, xmodem flashing
 * and this console all run the same rate on the same wire). */
#define CLI_GROVE_UART_BAUD UART_BAUDRATE_921600

/* Bounded spin for the printf path when the TX ring is momentarily full: the
 * TX-done callback drains in the background between iterations; the cap only
 * stops a wedged TX from hanging the caller (then drops). */
#define CLI_GROVE_WRITE_SPIN_MAX 1000000u

/* PRIMASK critical section.  Nests safely inside ThreadX's own. */
#define CLI_UART_CRIT_ENTER()  do { uint32_t _pm = __get_PRIMASK(); __disable_irq()
#define CLI_UART_CRIT_EXIT()   __set_PRIMASK(_pm); } while (0)

/* The driver takes the chunk as a DEV_BUFFER it holds on to until the TX-done
 * callback; must stay valid for the whole transfer, hence file-static. */
static DEV_BUFFER g_tx_dbuf;

/*
 * Hand the driver the next contiguous run if none is in flight.  MUST be
 * called with the critical section held.  On any driver refusal the bytes
 * stay queued and the next write()/TX-done retries.
 */
static void tx_start_locked(struct cli_grove_uart *u)
{
	DEV_UART_PTR dev = (DEV_UART_PTR)u->dev;
	const uint8_t *p;
	size_t run;

	if (u->tx_in_flight || dev == NULL)
		return;

	run = cli_uart_ring_contig(&u->tx_ring, &p);
	if (run == 0u)
		return;                 /* nothing to send */

	g_tx_dbuf.buf = (void *)(uintptr_t)p;
	g_tx_dbuf.len = (uint32_t)run;
	g_tx_dbuf.ofs = 0u;
	if (dev->uart_control(UART_CMD_SET_TXINT_BUF,
	                      (UART_CTRL_PARAM)&g_tx_dbuf) == 0 &&
	    dev->uart_control(UART_CMD_SET_TXINT, (UART_CTRL_PARAM)1u) == 0) {
		u->tx_in_flight = 1u;
		u->tx_chunk     = (uint32_t)run;
	}
}

/* ---- driver callbacks (ISR context; resolved via g_uart_console) -------- */

static void uart_rx_cb(void *arg)
{
	struct cli_grove_uart *u = g_uart_console;
	DEV_UART_PTR dev;
	uint8_t b;
	int notify = 0;
	(void)arg;

	if (u == NULL || u->dev == NULL)
		return;
	dev = (DEV_UART_PTR)u->dev;

	/* Drain whatever the FIFO holds; DATA_AVAIL fires again for more.  On
	 * ring overflow drop the byte and count (the shell surfaces rx_dropped). */
	while (dev->uart_read_nonblock(&b, 1u) == 1) {
		if (!cli_uart_ring_put(&u->rx_ring, b)) {
			u->rx_dropped_ring++;
			if (u->sh != NULL)
				u->sh->rx_dropped++;
		}
		notify = 1;
	}

	if (notify && u->sh != NULL)
		cli_transport_notify_rx(u->sh);     /* ISR-safe: sets an event flag */
}

static void uart_tx_cb(void *arg)
{
	struct cli_grove_uart *u = g_uart_console;
	DEV_UART_PTR dev;
	(void)arg;

	if (u == NULL || u->dev == NULL)
		return;
	dev = (DEV_UART_PTR)u->dev;

	CLI_UART_CRIT_ENTER();
	cli_uart_ring_advance_tail(&u->tx_ring, u->tx_chunk);
	u->tx_chunk     = 0u;
	u->tx_in_flight = 0u;
	tx_start_locked(u);             /* next chunk, if any (one code path) */
	if (!u->tx_in_flight) {
		/* Ring empty: park TX so an empty-FIFO interrupt cannot storm. */
		(void)dev->uart_control(UART_CMD_SET_TXINT, (UART_CTRL_PARAM)0u);
		(void)dev->uart_control(UART_CMD_SET_TXINT_BUF, (UART_CTRL_PARAM)0u);
	}
	CLI_UART_CRIT_EXIT();

	/* Space just freed: wake the core if it was blocked on TX. */
	if (u->sh != NULL)
		cli_transport_notify_tx(u->sh);
}

static void uart_err_cb(void *arg)
{
	struct cli_grove_uart *u = g_uart_console;
	(void)arg;

	if (u == NULL)
		return;
	u->err_events++;                /* overrun/framing/parity: counted only */
}

/* ---- transport vtable -------------------------------------------------- */

static int uart_init(struct cli_transport *tr)
{
	struct cli_grove_uart *u = (struct cli_grove_uart *)tr->ctx;

	if (u == NULL)
		return -1;

	cli_uart_ring_init(&u->rx_ring, u->rx_buf, sizeof u->rx_buf);
	cli_uart_ring_init(&u->tx_ring, u->tx_buf, sizeof u->tx_buf);
	u->dev             = NULL;
	u->tx_in_flight    = 0u;
	u->tx_chunk        = 0u;
	u->enabled         = 0u;
	u->rx_dropped_ring = 0u;
	u->err_events      = 0u;
	u->sh              = tr->sh;    /* cli_init() set tr->sh before init */

	/* Become the console now so _write routes here; `enabled` stays 0 until
	 * enable() arms the hardware, so pre-enable _write only enqueues. */
	g_uart_console = u;
	return 0;
}

static int uart_enable(struct cli_transport *tr)
{
	struct cli_grove_uart *u = (struct cli_grove_uart *)tr->ctx;
	DEV_UART_PTR dev;

	dev = hx_drv_uart_get_dev(USE_DW_UART_0);
	if (dev == NULL) {
		LOG_ERR("uart0 dev unavailable");
		return -1;
	}
	if (dev->uart_open(CLI_GROVE_UART_BAUD) != 0) {
		LOG_ERR("uart0 open failed");
		return -1;
	}
	u->dev = dev;

	/* RX mode: NULL RX buffer + RXCB => callback on DATA_AVAIL
	 * (hx_drv_uart.h UART_CMD_SET_RXCB).  Verified interactively on hardware.
	 * If a future SDK bump breaks it, the designed fallback is a 1-byte
	 * uart_read_udma() re-arm -- which ALSO needs NVIC_EnableIRQ(DMA3
	 * combined IRQ 69, and only 69): uart_read_udma uses UART0's fixed DMA3
	 * channels, and without that IRQ the completion callback never runs. */
	(void)dev->uart_control(UART_CMD_SET_RXINT_BUF, (UART_CTRL_PARAM)0u);
	(void)dev->uart_control(UART_CMD_SET_RXCB, (UART_CTRL_PARAM)uart_rx_cb);
	(void)dev->uart_control(UART_CMD_SET_TXCB, (UART_CTRL_PARAM)uart_tx_cb);
	(void)dev->uart_control(UART_CMD_SET_ERRCB, (UART_CTRL_PARAM)uart_err_cb);
	if (dev->uart_control(UART_CMD_SET_RXINT, (UART_CTRL_PARAM)1u) != 0) {
		LOG_ERR("uart0 rx int enable failed");
		return -1;
	}

	/* The pre-kernel hygiene sweep in main() disabled every external IRQ;
	 * this is the one place the console IRQ comes back.  uart_open()
	 * installed the vector (EPII_NVIC_SetVector inside libdriver). */
	NVIC_ClearPendingIRQ(UART0_intr_IRQn);
	NVIC_SetPriority(UART0_intr_IRQn, CLI_GROVE_UART_IRQ_PRIORITY);
	NVIC_EnableIRQ(UART0_intr_IRQn);

	u->enabled = 1u;

	/* Flush anything printf enqueued before enable (the boot banner). */
	CLI_UART_CRIT_ENTER();
	tx_start_locked(u);
	CLI_UART_CRIT_EXIT();

	LOG_INF("uart0 console up, 921600");
	return 0;
}

static int uart_write(struct cli_transport *tr, const uint8_t *data, size_t len)
{
	struct cli_grove_uart *u = (struct cli_grove_uart *)tr->ctx;
	size_t acc;

	/* Non-blocking: enqueue what fits, kick TX, return the count.  A short/
	 * zero return makes the core block on CLI_EVT_TX; the TX-done callback
	 * frees space and fires cli_transport_notify_tx() to wake it. */
	CLI_UART_CRIT_ENTER();
	acc = cli_uart_ring_put_buf(&u->tx_ring, data, len);
	if (u->enabled)
		tx_start_locked(u);
	CLI_UART_CRIT_EXIT();

	return (int)acc;
}

static int uart_read(struct cli_transport *tr, uint8_t *data, size_t cap)
{
	struct cli_grove_uart *u = (struct cli_grove_uart *)tr->ctx;

	/* SPSC: the UART0 ISR is the only producer, this (the shell thread) the
	 * only consumer, so draining the ring needs no lock. */
	return (int)cli_uart_ring_get_buf(&u->rx_ring, data, cap);
}

static void uart_uninit(struct cli_transport *tr)
{
	struct cli_grove_uart *u = (struct cli_grove_uart *)tr->ctx;
	DEV_UART_PTR dev = (DEV_UART_PTR)u->dev;

	if (dev != NULL) {
		(void)dev->uart_control(UART_CMD_SET_RXINT, (UART_CTRL_PARAM)0u);
		(void)dev->uart_control(UART_CMD_SET_TXINT, (UART_CTRL_PARAM)0u);
	}
	NVIC_DisableIRQ(UART0_intr_IRQn);
	u->enabled      = 0u;
	u->tx_in_flight = 0u;
	u->tx_chunk     = 0u;
}

const struct cli_transport_api cli_grove_uart_api = {
	uart_init, uart_enable, uart_write, uart_read, uart_uninit, NULL, NULL, NULL,
};

/* ---- printf / _write (single TX owner; 3-phase policy) ------------------ */

/*
 * Push one byte into the TX ring, spinning (bounded) until it fits while the
 * TX-done callback drains in the background.  Returns 1 on success, 0 if TX
 * stayed wedged past the spin cap.  Only used once the console is enabled.
 */
static int tx_putc_spin(struct cli_grove_uart *u, uint8_t b, uint32_t *stall)
{
	for (;;) {
		int ok;
		CLI_UART_CRIT_ENTER();
		ok = cli_uart_ring_put(&u->tx_ring, b);
		tx_start_locked(u);
		CLI_UART_CRIT_EXIT();

		if (ok) {
			*stall = 0;
			return 1;
		}
		if (++*stall > CLI_GROVE_WRITE_SPIN_MAX)
			return 0;               /* TX wedged: give up (best-effort) */
	}
}

/*
 * Resolve a shell instance to this backend's context, but only when this
 * backend owns it (api identity) and the console is live.  A non-UART
 * transport (the dummy backend in host tests / a bg job aliased elsewhere),
 * an un-enabled instance or NULL returns NULL -> console fallback.
 */
static struct cli_grove_uart *uart_ctx_from_instance(struct cli_instance *sh)
{
	struct cli_grove_uart *u;

	if (sh == NULL || sh->tr == NULL || sh->tr->api != &cli_grove_uart_api)
		return NULL;
	u = (struct cli_grove_uart *)sh->tr->ctx;
	return (u != NULL && u->enabled) ? u : NULL;
}

/*
 * printf retarget.  Three phases (reviewed M-G1 plan):
 *   1. before cli_init (no console bound):     drop -- there is nowhere to go
 *      (the UART is not open; no polling path exists pre-open on this driver).
 *   2. after cli_init, before enable():        enqueue-only into the TX ring,
 *      NEVER wait -- there is no scheduler and nothing drains the ring yet.
 *      The boot banner takes this path; enable() flushes it.
 *   3. enabled:                                normal non-blocking ring path
 *      with the bounded spin; the TX callback drains concurrently.
 *
 * Bare LF is translated to CR+LF so a raw terminal shows printf output
 * without staircasing.  Line-atomicity: when the calling thread owns a shell
 * instance on this backend, the drain is bracketed by cli_out_begin/end (the
 * same lock cli_print takes).  During a raw transfer (cli_xfer_active) printf
 * output is dropped so it cannot corrupt the byte stream.
 */
int _write(int file, char *ptr, int len)
{
	struct cli_instance   *sh = cli_current_instance();
	struct cli_grove_uart *u  = uart_ctx_from_instance(sh);
	int locked = 0;
	(void)file;

	if (len <= 0)
		return len;
	if (cli_xfer_active)
		return len;

	if (u == NULL)
		u = g_uart_console;     /* ISR / pre-kernel / non-shell thread */
	else
		locked = (cli_out_begin(sh) == 0);

	if (u == NULL) {                /* phase 1: no console bound yet */
		return len;
	}

	{
		const uint8_t *d = (const uint8_t *)ptr;
		uint32_t stall = 0;
		uint8_t  prev = 0;
		int i;

		for (i = 0; i < len; i++) {
			uint8_t b = d[i];

			if (b == (uint8_t)'\n' && prev != (uint8_t)'\r') {
				if (u->enabled) {
					if (!tx_putc_spin(u, (uint8_t)'\r', &stall))
						break;
				} else {
					CLI_UART_CRIT_ENTER();
					(void)cli_uart_ring_put(&u->tx_ring, (uint8_t)'\r');
					CLI_UART_CRIT_EXIT();
				}
			}
			if (u->enabled) {
				if (!tx_putc_spin(u, b, &stall))
					break;          /* wedged: drop the rest */
			} else {
				/* phase 2: enqueue what fits, never wait. */
				CLI_UART_CRIT_ENTER();
				(void)cli_uart_ring_put(&u->tx_ring, b);
				CLI_UART_CRIT_EXIT();
			}
			prev = b;
		}
	}

	if (locked)
		cli_out_end(sh);
	return len;
}
