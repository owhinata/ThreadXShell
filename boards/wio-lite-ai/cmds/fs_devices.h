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
 * exist is a board fact -- this board has the microSD on SDMMC1 and nothing else,
 * while the F746 port has a LevelX/QSPI NOR device alongside it -- so the
 * accessors are declared here, per board, rather than in the shared header.
 *
 * Each accessor is defined by the command file that owns the device instance.
 * The declaration is guarded by the same BSP_ENABLE_* switch that compiles that
 * file, so a build with the media turned off cannot reach for it by accident.
 */
#ifndef FS_DEVICES_H
#define FS_DEVICES_H

#include "fs_cmd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#if BSP_ENABLE_SD
/** The microSD media (SDMMC1 + FileX).  Defined by cmds/cmd_sd.c. */
const struct fs_device *fs_sd_device(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FS_DEVICES_H */
