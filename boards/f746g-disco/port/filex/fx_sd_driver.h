/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fx_sd_driver.h
 * @brief   FileX media driver on the SDMMC1 block API
 * (owhinata/stm32f746g-disco#34, Epic owhinata/stm32f746g-disco#32).
 *
 * Maps FileX driver requests onto sd_card_read/write_blocks().  The SD card has
 * its own wear-leveling FTL, so there is no LevelX layer.  Because PC/camera
 * tools format microSD with an MBR + a FAT partition (the VBR is not at LBA 0),
 * the driver presents partition 0 as a superfloppy: INIT reads LBA 0, finds the
 * partition start (sd_part_lba) and size, and every request adds sd_part_lba so
 * FileX -- which never computes a partition offset itself -- sees a flat volume.
 */
#ifndef FX_SD_DRIVER_H
#define FX_SD_DRIVER_H

#include "fx_api.h"

/* The driver's entry point fx_sd_driver() and its format mode are declared once
 * for every board in shell/cmds/sd_cmd_board.h (issue #232). */
#include "sd_cmd_board.h"

#endif /* FX_SD_DRIVER_H */
