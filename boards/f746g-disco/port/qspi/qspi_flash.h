/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    qspi_flash.h
 * @brief   QSPI NOR flash driver (Micron N25Q128A / MT25QL128, 16 MB) --
 * owhinata/stm32f746g-disco#29.
 *
 * Low-level driver for the on-board 128 Mbit QSPI NOR behind the STM32F746
 * QUADSPI controller, in *indirect* mode only (polled HAL transfers through the
 * controller FIFO; no DMA, no memory-mapped window at 0x90000000).  Because no
 * CPU access ever touches the memory-mapped region, the D-cache needs no
 * maintenance here.  Reads run 1-1-4 (FAST READ QUAD OUTPUT 0x6B,
 * owhinata/stm32f746g-disco#31) once init verifies the VCR dummy-cycle setup, with a 1-1-1
 * (0x0B) fallback;  all other commands (id/status/program/erase) stay 1-1-1.
 *
 * Concurrency: every public call serializes on an internal ThreadX mutex for the
 * whole flash operation (write-enable -> command -> busy-wait -> error check), so
 * callers from different threads (shell foreground, background jobs) can
 * interleave safely.  Consequently the API is **thread-context only**: never call
 * it from an ISR, and never before qspi_flash_init() ran (tx_application_define).
 *
 * Clean-room implementation; the ST BSP (stm32746g_discovery_qspi.c) and the
 * Micron datasheet were used as a register/command-value reference only.
 */
#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Device geometry (N25Q128A13EF840F / MT25QL128ABA1EW9). */
#define QSPI_FLASH_SIZE            0x1000000u  /* 16 MB                       */
#define QSPI_FLASH_SECTOR_SIZE     0x10000u    /* 64 KB erase unit (0xD8)     */
#define QSPI_FLASH_SUBSECTOR_SIZE  0x1000u     /*  4 KB erase unit (0x20)     */
#define QSPI_FLASH_PAGE_SIZE       0x100u      /* 256 B program unit (0x02)   */

/* Error returns (negative); 0 is success. */
#define QSPI_FLASH_OK            0
#define QSPI_FLASH_ERR_PARAM    -1   /* address/length out of range            */
#define QSPI_FLASH_ERR_HAL      -2   /* HAL command/transfer failed            */
#define QSPI_FLASH_ERR_TIMEOUT  -3   /* WIP busy-wait timed out                */
#define QSPI_FLASH_ERR_FLASH    -4   /* flag status reported P_ERR/E_ERR       */
#define QSPI_FLASH_ERR_STATE    -5   /* not initialized / mutex unavailable    */

struct qspi_flash_info {
	uint32_t size;            /* total bytes (16 MB)            */
	uint32_t sector_size;     /* 64 KB                          */
	uint32_t subsector_size;  /*  4 KB                          */
	uint32_t page_size;       /* 256 B                          */
	uint32_t sclk_hz;         /* QUADSPI serial clock           */
};

/**
 * One-time bring-up: GPIO/RCC, QUADSPI peripheral init, operation mutex.
 * Idempotent (later calls return 0 without touching the hardware).  Call it
 * once from tx_application_define(); it performs no flash transaction itself.
 */
int qspi_flash_init(void);

/** Read the 3-byte JEDEC ID (0x9F); expect 20 BA 18. */
int qspi_flash_read_id(uint8_t id[3]);

/**
 * Read @p len bytes from @p addr.  Uses FAST READ QUAD OUTPUT (0x6B, 1-1-4,
 * 10 dummy cycles via the VCR -- configured and verified by init) when quad
 * mode came up, falling back to FAST_READ (0x0B, 1-1-1, 8 dummy) otherwise.
 */
int qspi_flash_read(uint32_t addr, void *buf, uint32_t len);

/** Nonzero when reads run on 4 data lines (0x6B); 0 = 1-line fallback. */
int qspi_flash_quad_enabled(void);

/** Nonzero once qspi_flash_init() brought the part up; every other call
 *  returns QSPI_FLASH_ERR_STATE until then.  No flash transaction (issue #194:
 *  the blob store asks before it acts, as wio-lite-ai's asks nor_flash_ready()). */
int qspi_flash_ready(void);

/**
 * Program @p len bytes at @p addr.  Split internally at 256 B page boundaries;
 * each page runs WREN -> PAGE PROGRAM (0x02) -> WIP wait -> flag-status check.
 * NOR semantics: programming only clears bits (1 -> 0); erase first for fresh
 * 0xFF.  Blocks for ~0.5 ms typ per page.
 */
int qspi_flash_write(uint32_t addr, const void *buf, uint32_t len);

/** Erase the 64 KB sector containing @p addr (0xD8).  Blocks ~0.7 s typ / 3 s max. */
int qspi_flash_erase_sector(uint32_t addr);

/** Erase the 4 KB subsector containing @p addr (0x20).  Blocks ~0.25 s typ / 0.8 s max. */
int qspi_flash_erase_subsector(uint32_t addr);

/** Erase the whole chip (0xC7).  Blocks for minutes (250 s max); danger-cmd use only. */
int qspi_flash_erase_chip(void);

/** Read the status register (0x05); bit0 = WIP. */
int qspi_flash_read_status(uint8_t *sr);

/** Geometry + configured clock; valid (static) even before init. */
const struct qspi_flash_info *qspi_flash_get_info(void);

/* ---- failure record and live registers (issue #194) ----------------------
 * After every receive the driver puts the controller's functional mode back
 * to indirect write and reads it back; a receive that does not then read idle
 * fails.  Every failed HAL call, and every such receive, is recorded (before
 * any recovery) and the controller is then brought back to idle if it can be;
 * the failed call still fails, and nothing is retried.  The rules are in
 * qspi_idle.h.  Both functions below only read. */

/** QUADSPI registers without side effects (DR is never read: it pops the FIFO). */
struct qspi_flash_regs {
	uint32_t cr, sr, ccr, dlr, ar;
};

/** One failure. */
struct qspi_flash_fail {
	const char *op;          /* driver step, e.g. "rdsr", "qread", "wren"   */
	const char *phase;       /* "cmd", "rx", "tx", or "rxend" (a receive the
	                          * HAL called successful that did not end idle) */
	uint32_t tick;           /* HAL_GetTick() at the capture                 */
	int hal_rc;              /* HAL_StatusTypeDef (0 ok, 1 error, 2 busy,
	                          * 3 timeout)                                   */
	uint32_t state, error;   /* handle State / ErrorCode before recovery     */
	struct qspi_flash_regs regs;    /* before recovery                       */
	int recovered;           /* 1 = idle read back, handle READY             */
	struct qspi_flash_regs after;   /* after the recovery attempt            */
	uint32_t state_after;    /* handle State after it (READY or ERROR)       */
};

struct qspi_flash_record {
	uint32_t fail_count;
	struct qspi_flash_fail first, last;   /* valid when fail_count > 0       */
};

/** Live registers and handle state; takes no lock and issues no command. */
void qspi_flash_regs_now(struct qspi_flash_regs *r, uint32_t *state,
                         uint32_t *error);

/** Copy of the failure record (bounded wait for the operation mutex). */
void qspi_flash_record_get(struct qspi_flash_record *out);

#ifdef __cplusplus
}
#endif

#endif /* QSPI_FLASH_H */
