/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_fs_glue.h
 * @brief   Lazy-mount singleton for the microSD filesystem
 * (owhinata/stm32f746g-disco#34).
 *
 * One FX_MEDIA over fx_sd_driver, mounted on first use, with a reader/writer
 * ownership model.  The only FileX media on this board since issue #194 took the
 * QSPI NOR filesystem away, so this glue also owns fx_system_initialize().
 * Thread-context only.
 */
#ifndef SD_FS_GLUE_H
#define SD_FS_GLUE_H

#include "fx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ownership sentinels returned by the gates below and by sd_media_acquire(),
 * outside the FileX status space so a status-to-string mapper cannot confuse
 * them with one.  Defined HERE, in the port layer, and re-declared under the
 * same guards by shell/cmds/fs_cmd_core.h (the same shape as wio-lite-ai's).
 */
#ifndef FS_ERR_BUSY
#define FS_ERR_BUSY 0xF0u      /* another owner (umount / raw) holds it */
#endif
#ifndef FS_ERR_NO_CARD
#define FS_ERR_NO_CARD 0xF1u   /* the slot is empty                     */
#endif

/** Create the SD glue mutexes and run fx_system_initialize().  Call once from
 *  tx_application_define(). */
void sd_fs_glue_init(void);

/** Return the mounted media in *out, mounting (fx_media_open) on first use.
 *  Returns FX_SUCCESS or the failing fx status; a card without a FAT volume
 *  fails -- there is no SD format yet (Phase C). */
UINT sd_media_acquire(FX_MEDIA **out);

/** Flush + unmount (fx_media_close).  No-op when not mounted. */
UINT sd_media_unmount(void);

/** Nonzero while the SD media is mounted. */
int sd_is_mounted(void);

/** Nonzero while format/umount/raw owns the SD media. */
int sd_is_busy(void);

/*
 * Media ownership, reader/writer style: every FS command
 * holds a shared op slot; umount takes the exclusive slot; the card-disruptive
 * `sd info`/`sd read` (which re-probe the card) take the raw slot, additionally
 * refused while the media is mounted.  begin calls return FX_SUCCESS or
 * FS_ERR_BUSY and never block; pair every successful begin with its end.
 */
UINT sd_op_begin(void);
void sd_op_end(void);
UINT sd_exclusive_begin(void);
void sd_exclusive_end(void);
UINT sd_raw_begin(void);
void sd_raw_end(void);

/** Serialize multi-call directory sequences (`sd ls`). */
void sd_dir_lock(void);
void sd_dir_unlock(void);

/** The media singleton + its sector cache (for a future `sd format`). */
FX_MEDIA *sd_glue_media(void);
UCHAR    *sd_glue_cache(void);
ULONG     sd_glue_cache_size(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_FS_GLUE_H */
