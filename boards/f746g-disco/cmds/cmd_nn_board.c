/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn_board.c
 * @brief   The one thing `nn` needs from this board that a port cannot supply.
 *
 * `nn` is one shared command (shell/cmds/cmd_nn.c) above a neutral contract, and
 * since issue #99 that includes `nn stream` -- this file used to hold this
 * board's own version of it and no longer does.
 *
 * What is left is here for a reason that has not changed: fs_core_read_file()
 * takes a shell instance and prints its own errors, and both the filesystem
 * helper and this board's device table are shell-layer things a port must not
 * reach up for.  The shared command passes this function DOWN, so the port sees
 * a pointer and names nothing above itself.
 */
#include "cli.h"

#include <stdint.h>

#include "fs_cmd_core.h"   /* fs_core_read_file() */
#include "fs_devices.h"    /* fs_sd_device() */

/* The whole-file read behind `nn model load --path`. */
int nn_board_read_file(void *ctx, const char *path, void *buf, uint32_t cap,
                       uint32_t *len)
{
	return fs_core_read_file(fs_sd_device(), (struct cli_instance *)ctx, path,
	                         buf, cap, len);
}
