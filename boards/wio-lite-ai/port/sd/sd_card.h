/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    sd_card.h
 * @brief   microSD low-level driver over SDMMC1 + IDMA (owhinata/wio-lite-ai#6).
 *
 * Block driver for the on-board microSD slot (J4) behind the STM32H725's SDMMC1
 * controller, using HAL_SD in DMA mode.  Ported from the STM32F746 Discovery
 * firmware (../stm32f746g-disco port/sd), which shares this project's layering
 * and idioms; see the .c for the three places where the H7 forced a real
 * redesign rather than a copy.
 *
 * Structurally it is the SDMMC analogue of app/psram.c: a singleton HAL handle,
 * a per-operation ThreadX mutex, an idempotent init that touches no card, and
 * negative error codes.  Two mechanisms exist here that a polled driver has no
 * need for:
 *
 *   - Transfer completion is signalled from the SDMMC1 ISR via a count-0
 *     TX_SEMAPHORE; the calling thread waits on it with a finite timeout (no
 *     busy-wait).
 *   - The DMA always targets a single 32 B-aligned bounce buffer in AXI-SRAM
 *     (.axi_dma section); the D-cache is cleaned/invalidated around that buffer
 *     only, and the caller's buffer is touched solely by memcpy.  This keeps
 *     cache coherency correct for any caller alignment without an MPU region.
 *
 * SDMMC1 is the FIRST bus master this firmware has ever enabled -- USB dwc2 runs
 * slave/FIFO and everything else is CPU-driven -- so the "one CPU behind one
 * D-cache is self-coherent" premise recorded in app/main.c and app/mpu.c stops
 * holding the moment a card is probed.  The bounce buffer is what re-establishes
 * it; do not hand a caller pointer to the DMA.
 *
 * Concurrency: every public call serializes on the internal mutex for the whole
 * operation (state wait -> DMA -> completion -> cache).  The API is therefore
 * **thread-context only**: never call it from an ISR, and never before
 * sd_card_init() ran (tx_application_define).
 *
 * Clean-room implementation; RM0468 and the ST BSP were used as a register /
 * pin / clock-mux reference only.
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
 * One-time bring-up: GPIO (AF12), NVIC, the SDMMC1 kernel-clock mux, the
 * operation mutex and the completion semaphore.  Performs **no card I/O** (no
 * HAL_SD_Init), which is what makes it safe to call from tx_application_define()
 * before the scheduler runs: nothing here waits on HAL_GetTick
 * (owhinata/wio-lite-ai#12).  Idempotent: a second call returns 0 without re-doing
 * setup.
 */
int sd_card_init(void);

/**
 * Power up / identify the inserted card (CMD0..ACMD41..CMD2/3, 4-bit bus) and
 * fill the info snapshot.  Returns SD_ERR_NO_CARD when nothing answers (the only
 * empty-slot signal this board has -- see sd_card_is_probed), SD_ERR_HAL on an
 * identification failure.  Re-identifies from RESET on every call, so it doubles
 * as the remount entry point.
 */
int sd_card_probe(void);

/** Tear the card down to HAL_SD_STATE_RESET (HAL_SD_DeInit).  Idempotent. */
int sd_card_deinit(void);

/**
 * Nonzero while a card is known to be identified and answering.
 *
 * NOT a card-detect read: the Wio Lite AI microSD socket (J4, Molex 472192001)
 * has no card-detect line routed to the MCU -- the f746 board's CD pin (PC13) is
 * the red LED here.  This is therefore a *cached* predicate: it becomes true on
 * a successful sd_card_probe() and false again when a command times out (which
 * is how a removal surfaces).  It is meaningful only once something has probed;
 * before that it is false even with a card in the slot, so it must never gate
 * the path that would do the probing.
 */
int sd_card_is_probed(void);

/**
 * Read @p count 512 B blocks starting at LBA @p lba into @p buf (any alignment).
 * Internally chunked through the AXI-SRAM bounce buffer with DMA + cache
 * invalidation.  @p buf may be unaligned; @p count may exceed the bounce size.
 */
int sd_card_read_blocks(uint32_t lba, void *buf, uint32_t count);

/**
 * Write @p count 512 B blocks from @p buf (any alignment) to LBA @p lba.
 * Destructive.  Waits for the card to leave PROGRAMMING before returning so the
 * data is committed once this call succeeds.
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
