/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_cmd_board.h
 * @brief   What the `sd` command needs from a board besides its card driver
 *          (issue #232): the FileX mount glue, the FileX media driver and the
 *          `sd` medium for the other commands.
 *
 * The card driver is a table under a fixed name (svc/fs/sd_card_svc.h).  The
 * glue and the FileX driver are still each board's own implementation
 * (port/filex/sd_fs_glue.c and port/filex/fx_sd_driver.c on f746g-disco and
 * wio-lite-ai), called by these names; they are declared here, once, so both
 * boards' definitions are checked against the same declarations.  A board's
 * port/filex/sd_fs_glue.h and fx_sd_driver.h include this and keep only what
 * is the board's own (its start-up call, its ownership sentinels and the
 * result of the last probe).
 *
 * [!] THE `sd` COMMAND INCLUDES THIS AND NOT A BOARD'S HEADERS
 * (shell/cmds/camera_cmd_board.h says why).  FileX types appear here, so this
 * lives in shell/cmds/ and not in svc/ (svc/README.md: FileX includes
 * tx_api.h).  Thread context only.
 */
#ifndef SD_CMD_BOARD_H
#define SD_CMD_BOARD_H

#include "fx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fs_device;     /* shell/cmds/fs_cmd_core.h; opaque here, so the port
                         headers that include this do not pull the commands'
                         bodies in */

/* ---- the mount glue (port/filex/sd_fs_glue.c) ------------------------------ */

/** Return the mounted media in *out, mounting (fx_media_open) on first use.
 *  Returns FX_SUCCESS, FS_ERR_BUSY, FS_ERR_NO_CARD, or the failing fx status. */
UINT sd_media_acquire(FX_MEDIA **out);

/**
 * Mount while the caller ALREADY holds the exclusive slot (`sd format`).
 *
 * sd_media_acquire() refuses while the exclusive slot is held, which is exactly
 * the state a format is in -- so a format that released the slot before
 * remounting would let another console or a background job take it in between
 * and make the remount fail with FS_ERR_BUSY although the format succeeded.
 * Skips the hot-plug eviction (the format has just probed the card); a board
 * that can see an empty slot still answers it with FS_ERR_NO_CARD.
 */
UINT sd_media_mount_exclusive(FX_MEDIA **out);

/** Flush + unmount (fx_media_close).  No-op when not mounted. */
UINT sd_media_unmount(void);

/** Nonzero while the SD media is mounted. */
int sd_is_mounted(void);

/** Nonzero while format/umount/raw owns the SD media. */
int sd_is_busy(void);

/*
 * Media ownership, reader/writer style: every FS subcommand holds a shared op
 * slot for its whole duration; `umount`/`format` take the exclusive slot
 * (refused while any op or another exclusive runs); the card-disruptive
 * `sd info`/`sd read` (which may re-probe, i.e. HAL_SD_DeInit) take the raw
 * slot, which is the exclusive slot plus a refusal while the media is mounted.
 * begin calls return FX_SUCCESS or FS_ERR_BUSY and never block; pair every
 * successful begin with its end on all exit paths.
 */
UINT sd_op_begin(void);
void sd_op_end(void);
UINT sd_exclusive_begin(void);
void sd_exclusive_end(void);
UINT sd_raw_begin(void);
void sd_raw_end(void);

/** Serialize multi-call directory sequences (`sd ls` sets the media-global
 *  default directory, iterates, then restores it -- FileX's mutex only covers
 *  each call). */
void sd_dir_lock(void);
void sd_dir_unlock(void);

/** The media singleton + its sector cache, for `sd format`. */
FX_MEDIA *sd_glue_media(void);
UCHAR    *sd_glue_cache(void);
ULONG     sd_glue_cache_size(void);

/* ---- the FileX media driver (port/filex/fx_sd_driver.c) -------------------- */

/** FileX media driver entry (pass to fx_media_open / fx_media_format). */
VOID fx_sd_driver(FX_MEDIA *media_ptr);

/**
 * Enable/disable "format mode" for the next driver INIT.  A blank card has no
 * valid VBR/MBR, so the normal INIT partition detection fails and
 * fx_media_format could not run.  With format mode on, INIT skips detection and
 * treats the whole card as a superfloppy (partition start 0, size = card block
 * count).  The `sd` command sets it under the exclusive ownership slot around
 * fx_media_format and clears it afterwards.
 */
void fx_sd_set_format_mode(int on);

/* ---- the medium, for other commands ---------------------------------------- */

/** The microSD media (SDMMC + FileX).  Defined by the `sd` command
 *  (shell/cmds/cmd_sd.c); `camera save` writes through it (the board's
 *  cmds/cmd_camera_board.c). */
const struct fs_device *fs_sd_device(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CMD_BOARD_H */
