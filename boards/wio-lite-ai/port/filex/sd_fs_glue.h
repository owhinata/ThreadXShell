/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    sd_fs_glue.h
 * @brief   Lazy-mount singleton for the microSD filesystem (owhinata/wio-lite-ai#6).
 *
 * One FX_MEDIA over fx_sd_driver, mounted on first use, plus the ownership model
 * the `sd` subcommands share.  After sd_media_acquire() succeeds, fx_* calls on
 * the returned media are serialized by FileX's own per-media mutex; this layer's
 * mutex only guards the mount/unmount transitions and the ownership counters.
 * Thread-context only.
 */
#ifndef SD_FS_GLUE_H
#define SD_FS_GLUE_H

#include "fx_api.h"

/*
 * Ownership sentinels returned by the gates below and by sd_media_acquire(),
 * deliberately outside the FileX status space so a status-to-string mapper cannot
 * confuse them with one.  Defined HERE, in the port layer, and re-declared under
 * the same guards by shell/cmds/fs_cmd_core.h -- the dependency runs
 * port/ <- shell/, never the other way (CLAUDE.md layering).
 */
#ifndef FS_ERR_BUSY
#define FS_ERR_BUSY 0xF0u      /* another owner (umount / format / raw) holds it */
#endif
#ifndef FS_ERR_NO_CARD
#define FS_ERR_NO_CARD 0xF1u   /* the slot is empty                              */
#endif

/* Everything else this glue offers -- the mount entry points, the ownership
 * slots, the directory lock and the format's media + cache -- is declared once
 * for every board in shell/cmds/sd_cmd_board.h (issue #232), after the
 * sentinels above so they stay this layer's definitions. */
#include "sd_cmd_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the glue mutexes and run fx_system_initialize().  Call once from
 *  tx_application_define(). */
void sd_fs_glue_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_FS_GLUE_H */
