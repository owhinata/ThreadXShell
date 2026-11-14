/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_cmd_board.h
 * @brief   The microSD filesystem behind the one `sd` command: the FileX mount
 *          glue and the FileX media driver, one implementation for every board
 *          with a slot (issues #232, #233), and the state the board defines.
 *
 * shell/cmds/sd_fs_glue.c (the lazy mount, the ownership slots, the hot-plug
 * eviction) and shell/cmds/fx_sd_driver.c (FileX requests onto the card,
 * partition detection) are each compiled by every board with a microSD slot
 * (BOARD_HAS_SD).  They reach the card through the board's table under its
 * fixed name (svc/fs/sd_card_svc.h) and nothing else.
 *
 * [!] THE BOARD DEFINES sd_fs_board_state, under that fixed name and without
 * an initializer, in one file of its src/.  Like every shared translation
 * unit, the two files own no mutable storage (cmake/check_no_mutable_storage.py,
 * per board); what they remember -- the media, its sector cache, the two
 * mutexes, the mount and ownership marks, the partition window, the format
 * mode, the block buffer of the detection and the last probe's result -- is
 * that one object, reached by name and resolved at link time.  There is no
 * default definition: a board that compiles the files without defining it
 * fails to link, and a second definition fails to link too.  Where it lives
 * is the board's (CPU-only memory: the FileX driver hands every buffer to the
 * card driver, which bounces through its own DMA buffer).
 *
 * [!] THE `sd` COMMAND INCLUDES THIS AND NOT A BOARD'S HEADERS
 * (shell/cmds/camera_cmd_board.h says why), and a board's port/ does not
 * include it (the one-way layering: port <- shell).  FileX types appear here,
 * so this lives in shell/cmds/ and not in svc/ (svc/README.md: FileX includes
 * tx_api.h).  Thread context only.
 */
#ifndef SD_CMD_BOARD_H
#define SD_CMD_BOARD_H

#include "fx_api.h"
#include "sd_card_svc.h"   /* struct sd_card_ops, sd_card_board_ops, SD_BLOCK_SIZE */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fs_device;     /* shell/cmds/fs_cmd_core.h; opaque here */

/* ---- the state, which the board defines ----------------------------------- */

/** The FileX media's sector cache. */
#define SD_FS_CACHE_SIZE 4096u

/**
 * Everything the glue and the FileX driver remember.  Zero is the boot state
 * of every field (the mutexes are created by sd_fs_glue_init()).
 */
struct sd_fs_state {
	/* ---- the mount glue (sd_fs_glue.c) ---- */
	FX_MEDIA media;                    /* the one FileX media               */
	UCHAR    cache[SD_FS_CACHE_SIZE];  /* its sector cache                  */
	TX_MUTEX mount_lock;               /* the marks below, and the mount    */
	TX_MUTEX dir_mutex;                /* whole directory listings          */
	int      mounted;
	int      busy;         /* umount/format/raw owns the media              */
	int      active_ops;   /* FS commands currently inside a body           */
	/* ---- the FileX media driver (fx_sd_driver.c) ---- */
	/* Partition window for the current mount (recomputed at INIT for
	 * hot-plug): physical LBA = part_lba + logical, valid for
	 * logical < part_blocks. */
	uint32_t part_lba;
	uint32_t part_blocks;
	/* When set, the next INIT skips partition detection and uses the whole
	 * card as a superfloppy (fx_media_format of a blank card).  Set/cleared
	 * by `sd format` under the exclusive ownership slot. */
	volatile int format_mode;
	/* The SD_* result of the probe run by the most recent FX_DRIVER_INIT. */
	int      last_probe_rc;
	/* LBA 0 for partition detection (INIT only).  The card driver bounces
	 * through its own aligned DMA buffer, so any alignment is fine. */
	uint8_t  mbr_buf[SD_BLOCK_SIZE];
};

/** Defined by the board (boards/<board>/src/sd_fs_state.c). */
extern struct sd_fs_state sd_fs_board_state;

/* ---- the card driver table: every field set? ------------------------------- */

/**
 * Nonzero when every field of sd_card_board_ops is set.  The one check, made
 * first by `sd info`, `sd read`, `sd format` (shell/cmds/cmd_sd.c) and by both
 * mount entry points below, before any of them takes a slot or a mutex,
 * unmounts anything or talks to FileX or the card: the alternative is a call
 * through NULL from the shell thread.  The table is const, so a media that
 * is mounted was mounted with a complete one, and the paths that only use a
 * mounted media (unmount, the FileX driver's own requests) do not repeat it.
 */
static inline int sd_card_ops_complete(void)
{
	const struct sd_card_ops *ops = &sd_card_board_ops;

	return ops->probe != NULL && ops->status != NULL && ops->info != NULL &&
	       ops->read_blocks != NULL && ops->write_blocks != NULL &&
	       ops->release != NULL;
}

/* ---- the mount glue (shell/cmds/sd_fs_glue.c) ------------------------------ */

/** Create the glue's two mutexes and run fx_system_initialize() (the slot's
 *  media is the only FileX media on every board that has one).  Call once
 *  from tx_application_define(). */
void sd_fs_glue_init(void);

/** Return the mounted media in *out, mounting (fx_media_open) on first use.
 *  Returns FX_SUCCESS, FS_ERR_BUSY, FS_ERR_NO_CARD, FS_ERR_UNWIRED (a field of
 *  the card table is empty: nothing touched), or the failing fx status. */
UINT sd_media_acquire(FX_MEDIA **out);

/**
 * Mount while the caller ALREADY holds the exclusive slot (`sd format`).
 *
 * sd_media_acquire() refuses while the exclusive slot is held, which is exactly
 * the state a format is in -- so a format that released the slot before
 * remounting would let another console or a background job take it in between
 * and make the remount fail with FS_ERR_BUSY although the format succeeded.
 * Skips the hot-plug eviction (the format has just probed the card); an empty
 * slot is still answered with FS_ERR_NO_CARD, and an empty field of the card
 * table with FS_ERR_UNWIRED.
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
 * `sd info`/`sd read` (which may re-probe, i.e. de-initialise the card) take
 * the raw slot, which is the exclusive slot plus a refusal while the media is
 * mounted.  begin calls return FX_SUCCESS or FS_ERR_BUSY and never block;
 * pair every successful begin with its end on all exit paths.
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

/** The media + its sector cache, for `sd format`. */
FX_MEDIA *sd_glue_media(void);
UCHAR    *sd_glue_cache(void);
ULONG     sd_glue_cache_size(void);

/* ---- the FileX media driver (shell/cmds/fx_sd_driver.c) -------------------- */

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

/**
 * The SD_* result of the probe run by the most recent FX_DRIVER_INIT.
 *
 * fx_media_open() reports every driver INIT failure as a flat FX_IO_ERROR, which
 * would make "no card in the slot" indistinguishable from "card present but not
 * FAT".  The mount glue reads this afterwards to tell the two apart and report
 * FS_ERR_NO_CARD for the former.  Overwritten at the top of every INIT, so a
 * stale SD_ERR_NO_CARD can never leak into a later, unrelated failure.
 */
int fx_sd_last_probe_rc(void);

/* ---- the medium, for other commands ---------------------------------------- */

/** The microSD media (SDMMC + FileX).  Defined by the `sd` command
 *  (shell/cmds/cmd_sd.c); `camera save` writes through it (the board's
 *  cmds/cmd_camera_board.c). */
const struct fs_device *fs_sd_device(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CMD_BOARD_H */
