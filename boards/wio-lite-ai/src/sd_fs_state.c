/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_fs_state.c
 * @brief   This board's state of the shared microSD mount glue and FileX media
 *          driver (shell/cmds/sd_fs_glue.c, shell/cmds/fx_sd_driver.c; issue
 *          #233).  Compiled only with BSP_ENABLE_SD (board.cmake).
 *
 * The one definition under the fixed name the shared files reach it by
 * (shell/cmds/sd_cmd_board.h), without an initializer: plain .bss in the
 * AXI-SRAM, where the glue's and the driver's own statics were.  Only the CPU
 * touches it -- the FileX driver hands every buffer to port/sd/sd_card.c,
 * which bounces through its own AXI-SRAM DMA buffer (sd_bounce) and does the
 * D-cache maintenance.  CPU-only memory would rather be DTCM, but the state is
 * about 14 KB and the DTCM has not that much left, so it stays in the
 * AXI-SRAM's .bss.
 */
#include "sd_cmd_board.h"

struct sd_fs_state sd_fs_board_state;
