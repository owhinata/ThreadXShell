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

#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_container.h"  /* nn_container_crc() (issue #159) */
#endif

/*
 * The whole-file read behind `nn model load --path`.
 *
 * [!] AND, FOR A PLUGIN CONTAINER, THE CLOSING CHECK (issue #159).  Nothing on
 * the PC checks that the file copied to the card is the file the build
 * published, and this board has no `blob list`: the CRC-32 printed here, over
 * every byte just read, is what the operator compares with the asset receipt.
 * Printed only for a WHOLE read (fs_core_read_file() refuses a short or
 * oversize one) and only for a container -- a bare `.tflite` load prints what
 * it always printed.  Printed before the firmware validates the container, on
 * purpose: a refused container's CRC is still the number that says whether
 * the right file reached the card.  The port computes the CRC `nn info` shows
 * beside the claims with the same function over the same (buf, len)
 * (port/nn/nn_container.h), so the two are one value.
 */
int nn_board_read_file(void *ctx, const char *path, void *buf, uint32_t cap,
                       uint32_t *len)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;
	int rc = fs_core_read_file(fs_sd_device(), sh, path, buf, cap, len);
#if defined(CONFIG_NN_BACKEND_TFLM)
	uint32_t crc;

	if (rc == 0 && len != NULL && nn_container_crc(buf, *len, &crc))
		cli_print(sh, "nn: read %s: plugin container, %lu B, crc32 %08lX\r\n",
		          path, (unsigned long)*len, (unsigned long)crc);
#endif
	return rc;
}
