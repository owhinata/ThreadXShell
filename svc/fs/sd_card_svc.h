/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_card_svc.h
 * @brief   The contract of a board's microSD card driver, as the `sd` command
 *          sees it (issue #232).
 *
 * Each board with a microSD slot has its own block driver over its own SDMMC
 * controller (f746g-disco and wio-lite-ai port/sd/sd_card.c).  What the `sd`
 * command, the FileX media driver and the mount glue need from that driver is
 * the six calls in struct sd_card_ops, and the values below that both drivers
 * already answered in: the result codes, the block size and the identity of
 * the probed card.
 *
 * [!] THE BOARD DEFINES sd_card_board_ops UNDER THAT FIXED NAME, in its
 * port/sd/sd_card.c, as a const pointing at its driver's functions.  There is
 * no default definition: a board that compiles the command without defining it
 * fails to link.  The FileX media driver and the mount glue (shared,
 * shell/cmds/fx_sd_driver.c and sd_fs_glue.c, issue #233) reach the card
 * through this table too, and no field may be left NULL (they refuse first,
 * shell/cmds/sd_cmd_board.h); only the start-up code calls the driver by
 * name (its init).
 *
 * Thread context only, like the drivers behind it.  Freestanding: no HAL, no
 * board header, no tx_api.h, no FileX.
 */
#ifndef SD_CARD_SVC_H
#define SD_CARD_SVC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical/physical block size.  SDHC/SDXC are fixed 512 B; HAL also assumes it. */
#define SD_BLOCK_SIZE   512u

/* Error returns (negative); 0 is success. */
#define SD_OK            0
#define SD_ERR_PARAM    -1   /* bad argument (null buffer, zero count)          */
#define SD_ERR_HAL      -2   /* HAL/DMA reported an error or refused the request*/
#define SD_ERR_TIMEOUT  -3   /* card never reached TRANSFER / DMA never completed*/
#define SD_ERR_STATE    -4   /* driver not initialized / card not probed        */
#define SD_ERR_NO_CARD  -5   /* the slot is empty (how a board learns that is its
                                own: a card-detect pin, or no card answering)   */

/** Snapshot of the probed card's identity/geometry (filled by the probe). */
struct sd_card_info {
	uint32_t type;            /* 0 = SDSC, 1 = SDHC/SDXC (HAL CardType)        */
	uint32_t version;         /* 0 = v1.x, 1 = v2.x (HAL CardVersion)          */
	uint32_t card_class;      /* card command classes (CCC)                   */
	uint32_t rca;             /* relative card address                        */
	uint32_t block_count;     /* logical 512 B block count (LogBlockNbr)      */
	uint32_t block_size;      /* logical block size, always 512               */
	uint32_t bus_width;       /* negotiated data lines: 1 or 4                */
	uint64_t capacity_bytes;  /* block_count * 512                            */
	uint32_t cid[4];          /* raw CID                                       */
	uint32_t csd[4];          /* raw CSD                                       */
};

/** What a board's card driver hands the `sd` command. */
struct sd_card_ops {
	/** Power up / identify the inserted card from RESET and fill the info
	 *  snapshot.  SD_OK, or SD_ERR_NO_CARD for an empty slot, or another
	 *  SD_ERR_*. */
	int (*probe)(void);
	/** SD_OK if the card is identified and in TRANSFER, else an SD_ERR_*. */
	int (*status)(void);
	/** The probed card; valid only after a successful probe. */
	const struct sd_card_info *(*info)(void);
	/** Read @p count 512 B blocks from LBA @p lba into @p buf (any
	 *  alignment).  The command reads one block at a time. */
	int (*read_blocks)(uint32_t lba, void *buf, uint32_t count);
	/** Write @p count 512 B blocks from @p buf (any alignment) to LBA
	 *  @p lba; returns once the card has committed them.  Destructive:
	 *  reached only through the FileX media driver. */
	int (*write_blocks)(uint32_t lba, const void *buf, uint32_t count);
	/** Drop the probed card back to RESET so the next probe identifies
	 *  it from scratch (the mount glue's eviction).  Idempotent. */
	int (*release)(void);
};

/** Defined by the board (port/sd/sd_card.c), read by the `sd` command, the
 *  FileX media driver and the mount glue. */
extern const struct sd_card_ops sd_card_board_ops;

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_SVC_H */
