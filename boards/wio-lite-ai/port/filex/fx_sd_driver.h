/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    fx_sd_driver.h
 * @brief   FileX media driver on the SDMMC1 block API (owhinata/wio-lite-ai#6).
 *
 * Maps FileX driver requests onto sd_card_read/write_blocks().  The SD card has
 * its own wear-levelling FTL, so there is no LevelX layer.  Because PC and camera
 * tools format microSD with an MBR + a FAT partition (the VBR is then not at LBA 0)
 * while FileX itself only ever reads logical sector 0 and never computes a
 * partition offset, this driver presents partition 0 as a superfloppy: INIT reads
 * LBA 0, works out the partition start and size, and every later request adds that
 * start so FileX sees a flat volume.
 */
#ifndef FX_SD_DRIVER_H
#define FX_SD_DRIVER_H

#include "fx_api.h"

/* The driver's entry point fx_sd_driver() and its format mode are declared once
 * for every board in shell/cmds/sd_cmd_board.h (issue #232). */
#include "sd_cmd_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The SD_* result of the sd_card_probe() run by the most recent FX_DRIVER_INIT.
 *
 * fx_media_open() reports every driver INIT failure as a flat FX_IO_ERROR, which
 * would make "no card in the slot" indistinguishable from "card present but not
 * FAT".  The mount glue reads this afterwards to tell the two apart and report
 * FS_ERR_NO_CARD for the former.  Reset at the top of every INIT, so a stale
 * SD_ERR_NO_CARD can never leak into a later, unrelated failure.
 */
int fx_sd_last_probe_rc(void);

#ifdef __cplusplus
}
#endif

#endif /* FX_SD_DRIVER_H */
