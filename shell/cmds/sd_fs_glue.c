/* shared-command: optional BOARD_HAS_SD */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sd_fs_glue.c
 * @brief   Lazy-mount singleton for the microSD filesystem, for every board
 *          with a slot (owhinata/stm32f746g-disco#34, owhinata/wio-lite-ai#6;
 *          one implementation since issue #233).
 *
 * One FX_MEDIA over fx_sd_driver (shell/cmds/fx_sd_driver.c), mounted on first
 * use, with a reader/writer ownership model (shell/cmds/sd_cmd_board.h).  The
 * slot's media is the only FileX media on every board that has one, so this
 * glue owns fx_system_initialize().  The SD card has its own FTL.  The raw slot
 * gates `sd info`/`sd read`, which re-probe the card and must not run while the
 * media is mounted.
 *
 * The card is reached through the board's table (sd_card_board_ops), and an
 * empty slot is learnt the same way on every board: nothing is checked before
 * the mount, and a failed mount is read back through the probe the FileX
 * driver recorded.  How the card driver itself sees an empty slot (a
 * card-detect pin, or no card answering) stays inside it.
 *
 * [!] THIS FILE OWNS NO MUTABLE STORAGE (cmake/check_no_mutable_storage.py, per
 * board): the media, its cache, the mutexes and the marks are the board's
 * sd_fs_board_state.  The media cache is plain CPU memory: fx_sd_driver hands
 * every buffer to the card driver, which bounces through its own DMA buffer
 * and does the cache maintenance, so where the board puts the state is not
 * this file's concern.
 *
 * Lock order: the mount lock -> FileX's media mutex (inside fx_media_*) -> the
 * card driver's own lock (inside each sd_card_board_ops call).  Nothing here
 * takes them the other way round.
 */
#include "fs_cmd_core.h"    /* the FS_ERR_* sentinels */
#include "sd_cmd_board.h"
#include "sd_card_svc.h"    /* the card, through sd_card_board_ops */

#include "tx_api.h"

#define LOG_TAG "sdfs"
#include "log.h"

void sd_fs_glue_init(void)
{
	tx_mutex_create(&sd_fs_board_state.mount_lock, "sd_mount", TX_INHERIT);
	tx_mutex_create(&sd_fs_board_state.dir_mutex, "sd_dir", TX_INHERIT);
	/* The only FileX media on a board with a slot, so this glue owns the
	 * one-time system init (FileX's date/time and internal structures). */
	fx_system_initialize();
}

FX_MEDIA *sd_glue_media(void)
{
	return &sd_fs_board_state.media;
}

UCHAR *sd_glue_cache(void)
{
	return sd_fs_board_state.cache;
}

ULONG sd_glue_cache_size(void)
{
	return sizeof sd_fs_board_state.cache;
}

int sd_is_mounted(void)
{
	return sd_fs_board_state.mounted;
}

int sd_is_busy(void)
{
	return sd_fs_board_state.busy;
}

UINT sd_op_begin(void)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);
	if (sd_fs_board_state.busy)
		status = FS_ERR_BUSY;
	else
		sd_fs_board_state.active_ops++;
	tx_mutex_put(&sd_fs_board_state.mount_lock);
	return status;
}

void sd_op_end(void)
{
	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);
	sd_fs_board_state.active_ops--;
	tx_mutex_put(&sd_fs_board_state.mount_lock);
}

/* Exclusive owner; @p require_unmounted additionally refuses while mounted
 * (card re-probe / format must never run under a live filesystem). */
static UINT sd_owner_begin(int require_unmounted)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);
	if (sd_fs_board_state.busy || sd_fs_board_state.active_ops > 0 ||
	    (require_unmounted && sd_fs_board_state.mounted))
		status = FS_ERR_BUSY;
	else
		sd_fs_board_state.busy = 1;
	tx_mutex_put(&sd_fs_board_state.mount_lock);
	return status;
}

UINT sd_exclusive_begin(void)
{
	return sd_owner_begin(0);
}

UINT sd_raw_begin(void)
{
	return sd_owner_begin(1);
}

void sd_exclusive_end(void)
{
	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);
	sd_fs_board_state.busy = 0;
	tx_mutex_put(&sd_fs_board_state.mount_lock);
}

void sd_raw_end(void)
{
	sd_exclusive_end();
}

void sd_dir_lock(void)
{
	tx_mutex_get(&sd_fs_board_state.dir_mutex, TX_WAIT_FOREVER);
}

void sd_dir_unlock(void)
{
	tx_mutex_put(&sd_fs_board_state.dir_mutex);
}

/*
 * Shared mount body; the caller holds the mount lock.  Nothing is checked
 * before the mount: the card driver's probe, run by FX_DRIVER_INIT inside
 * fx_media_open(), answers an empty slot itself.  fx_media_open() reports any
 * INIT failure as a flat FX_IO_ERROR, so the empty slot is recovered
 * afterwards from the FileX driver's recorded probe result -- for both entry
 * points below, including a card pulled between `sd format` and its remount.
 *
 * Why there is no "is a card present?" check before the mount: a predicate the
 * card driver could answer without talking to the card would be a cached one
 * (on a board without a card-detect pin, "probed") that is false until
 * something has already talked to the card.  Gating on it here would refuse
 * the very fx_media_open() whose INIT does the probe, and a cold boot with a
 * good card would report "no card" forever.
 */
static UINT mount_locked(void)
{
	UINT status;

	if (sd_fs_board_state.mounted)
		return FX_SUCCESS;

	status = fx_media_open(&sd_fs_board_state.media, "sd", fx_sd_driver,
	                       FX_NULL, sd_fs_board_state.cache,
	                       sizeof sd_fs_board_state.cache);
	if (status == FX_SUCCESS) {
		sd_fs_board_state.mounted = 1;
		LOG_INF("media mounted");
	} else if (fx_sd_last_probe_rc() == SD_ERR_NO_CARD) {
		/* Empty slot, not a broken filesystem: report it as such so the shell
		 * prints "no card" instead of a mount hint about FAT. */
		status = FS_ERR_NO_CARD;
	}
	return status;
}

UINT sd_media_acquire(FX_MEDIA **out)
{
	UINT status = FX_SUCCESS;

	/* An empty field of the card table: refused before the mount lock, FileX
	 * or the card is touched (sd_card_ops_complete()). */
	if (!sd_card_ops_complete())
		return FS_ERR_UNWIRED;

	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);

	if (sd_fs_board_state.busy) {
		/* umount/format/raw owns the media; do not mount behind its back. */
		tx_mutex_put(&sd_fs_board_state.mount_lock);
		return FS_ERR_BUSY;
	}

	/* Hot-plug: if the mounted card was removed or swapped, the open FX_MEDIA is
	 * now stale.  The status query alone tells: the card driver answers "not
	 * probed" or "no card" for a card it has lost (how it sees an empty slot is
	 * its own), and a swapped card no longer answers CMD13 at the old RCA.
	 * Evict it WITHOUT flushing -- the card may be gone, so abort (not close).
	 * Eviction is only safe when no other op is using the media (active_ops
	 * counts this caller's op_begin); if another op is in flight, refuse rather
	 * than hand back the stale media (that op will fail with an I/O error and the
	 * next solo command evicts).  The mount lock is held throughout, and every
	 * op takes it to change active_ops, so the count cannot move while the
	 * status query waits for the card driver's lock. */
	if (sd_fs_board_state.mounted && sd_card_board_ops.status() != SD_OK) {
		if (sd_fs_board_state.active_ops > 1) {
			tx_mutex_put(&sd_fs_board_state.mount_lock);
			return FS_ERR_BUSY;
		}
		(void)fx_media_abort(&sd_fs_board_state.media);
		(void)sd_card_board_ops.release(); /* next mount re-probes */
		sd_fs_board_state.mounted = 0;
		LOG_INF("card removed/changed; media evicted");
	}

	status = mount_locked();

	tx_mutex_put(&sd_fs_board_state.mount_lock);

	if (status == FX_SUCCESS && out != FX_NULL)
		*out = &sd_fs_board_state.media;
	return status;
}

UINT sd_media_mount_exclusive(FX_MEDIA **out)
{
	UINT status;

	/* The same refusal as sd_media_acquire()'s, made first. */
	if (!sd_card_ops_complete())
		return FS_ERR_UNWIRED;

	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);
	status = mount_locked();
	tx_mutex_put(&sd_fs_board_state.mount_lock);

	if (status == FX_SUCCESS && out != FX_NULL)
		*out = &sd_fs_board_state.media;
	return status;
}

UINT sd_media_unmount(void)
{
	UINT status = FX_SUCCESS;

	tx_mutex_get(&sd_fs_board_state.mount_lock, TX_WAIT_FOREVER);

	if (sd_fs_board_state.mounted) {
		/* fx_media_close flushes and sends FX_DRIVER_UNINIT. */
		status = fx_media_close(&sd_fs_board_state.media);
		sd_fs_board_state.mounted = 0;
		if (status == FX_SUCCESS)
			LOG_INF("media unmounted");
		else
			LOG_ERR("fx_media_close failed (%u)", status);
	}

	tx_mutex_put(&sd_fs_board_state.mount_lock);
	return status;
}
