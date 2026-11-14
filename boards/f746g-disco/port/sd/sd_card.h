/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_card.h
 * @brief   microSD low-level driver over SDMMC1 + DMA
 * (owhinata/stm32f746g-disco#33, Epic owhinata/stm32f746g-disco#32).
 *
 * Block driver for the on-board microSD slot behind the STM32F746 SDMMC1
 * controller, using HAL_SD in DMA mode.  It is the SDMMC analogue of the QSPI
 * NOR driver (port/qspi/qspi_flash.c): a singleton HAL handle, a per-operation
 * ThreadX mutex, an idempotent init that touches no card, and negative error
 * codes.  Unlike QSPI (indirect/polled), SD transfers run on DMA2, so two extra
 * mechanisms appear here:
 *
 *   - DMA completion is signalled from an ISR via a count-0 TX_SEMAPHORE; the
 *     calling thread waits on it with a finite timeout (no busy-wait).
 *   - The DMA always targets a single 32 B-aligned bounce buffer in SRAM1
 *     (.sram1_dma section); the D-cache is cleaned/invalidated around that
 *     buffer only, and the caller's buffer is touched solely by memcpy.  This
 *     keeps cache coherency correct for any caller alignment without an MPU.
 *
 * Concurrency: every public call serializes on the internal mutex for the whole
 * operation (state wait -> DMA -> completion -> cache).  The API is therefore
 * **thread-context only**: never call it from an ISR, and never before
 * sd_card_init() ran (tx_application_define).
 *
 * Clean-room implementation; the ST BSP (stm32746g_discovery_sd.c) and RM0385
 * were used as a register/pin/DMA-mapping reference only.
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>

/* The block size, the SD_* result codes and struct sd_card_info are the
 * shared contract's (svc/fs/sd_card_svc.h, issue #232); this driver defines
 * its sd_card_board_ops table in sd_card.c. */
#include "sd_card_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One-time bring-up: GPIO (AF12), DMA2 streams, NVIC, SDMMC1 clock source mux,
 * operation mutex and DMA-completion semaphore.  Performs **no card I/O** (no
 * HAL_SD_Init), so it is safe to call from tx_application_define() before the
 * scheduler runs.  Idempotent: a second call returns 0 without re-doing setup.
 */
int sd_card_init(void);

/**
 * Power up / identify the inserted card (CMD0..ACMD41..CMD2/3, 4-bit bus, 24 MHz
 * transfer clock) and fill the info snapshot.  Returns SD_ERR_NO_CARD if the
 * slot is empty, SD_ERR_HAL on an identification failure.  Re-probes from
 * RESET each call, so it doubles as a remount entry point (hot-plug, Phase C).
 */
int sd_card_probe(void);

/** Tear the card down to HAL_SD_STATE_RESET (HAL_SD_DeInit).  Idempotent. */
int sd_card_deinit(void);

/** Nonzero when a card is physically present (card-detect PC13 = low). */
int sd_card_is_present(void);

/**
 * Read @p count 512 B blocks starting at LBA @p lba into @p buf (any alignment).
 * Internally chunked through the SRAM1 bounce buffer with DMA + cache
 * invalidation.  @p buf may be unaligned; @p count may exceed the bounce size.
 */
int sd_card_read_blocks(uint32_t lba, void *buf, uint32_t count);

/**
 * Write @p count 512 B blocks from @p buf (any alignment) to LBA @p lba.
 * Destructive; reached only through the FileX media driver (`sd write`, `sd
 * format`, `camera save`), never by a raw `sd` subcommand.  Waits for
 * the card to leave PROGRAMMING before returning so the data is committed.
 */
int sd_card_write_blocks(uint32_t lba, const void *buf, uint32_t count);

/** Probed card identity/geometry; valid only after a successful sd_card_probe(). */
const struct sd_card_info *sd_card_get_info(void);

/** Normalized current state: SD_OK if the card is in TRANSFER, else an SD_ERR_*. */
int sd_card_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
