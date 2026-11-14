/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_fs_state.c
 * @brief   This board's state of the shared microSD mount glue and FileX media
 *          driver (shell/cmds/sd_fs_glue.c, shell/cmds/fx_sd_driver.c; issue
 *          #233).
 *
 * The one definition under the fixed name the shared files reach it by
 * (shell/cmds/sd_cmd_board.h), without an initializer: plain .bss in the
 * internal SRAM, where the glue's and the driver's own statics were.  Only the
 * CPU touches it -- the FileX driver hands every buffer to port/sd/sd_card.c,
 * which bounces through its own SRAM1 DMA buffer (sd_bounce) and does the
 * D-cache maintenance -- so it needs no special placement.
 */
#include "sd_cmd_board.h"

struct sd_fs_state sd_fs_board_state;
