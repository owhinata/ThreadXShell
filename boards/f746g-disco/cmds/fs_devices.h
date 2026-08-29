/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fs_devices.h
 * @brief   The FileX media this board has, for cross-command reuse.
 *
 * The shared command bodies in shell/cmds/fs_cmd_core.c are parameterized over a
 * `struct fs_device` vtable and know nothing about which media exist.  WHICH ones
 * exist is a board fact -- this board has both the LevelX/QSPI NOR filesystem and
 * the microSD on SDMMC1, while the wio-lite-ai port has only the microSD -- so the
 * accessors are declared here, per board, rather than in the shared header.
 *
 * Each accessor is defined by the command file that owns the device instance, and
 * is used by the commands that write through another command's media:
 * `camera save` (cmds/cmd_camera.c), `send`/`recv` (cmds/cmd_xfer.c) and
 * `nn model load` (cmds/cmd_nn.c) all go through the same ownership gates as the
 * `fs` / `sd` commands themselves.
 */
#ifndef FS_DEVICES_H
#define FS_DEVICES_H

#include "fs_cmd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The QSPI NOR media (N25Q128A + LevelX + FileX).  Defined by cmds/cmd_fs.c. */
const struct fs_device *fs_qspi_device(void);

/** The microSD media (SDMMC1 + FileX).  Defined by cmds/cmd_sd.c. */
const struct fs_device *fs_sd_device(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_DEVICES_H */
