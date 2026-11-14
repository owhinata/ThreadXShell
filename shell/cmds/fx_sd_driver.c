/* shared-command: optional BOARD_HAS_SD */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * @file    fx_sd_driver.c
 * @brief   FileX media driver on a board's microSD card driver, for every
 *          board with a slot (owhinata/stm32f746g-disco#34,
 *          owhinata/wio-lite-ai#6; one implementation since issue #233).
 *
 * Maps FileX driver requests onto the card's block calls (sd_card_board_ops,
 * svc/fs/sd_card_svc.h).  The SD card has its own wear-leveling FTL, so there
 * is no LevelX layer.  FileX's fx_media_open() always reads logical sector 0
 * for the boot record and never calls _fx_partition_offset_calculate(), so an
 * MBR-partitioned card (how PC/camera tools format microSD) would otherwise
 * fail to mount.  This driver detects the layout at INIT (superfloppy VBR @
 * LBA 0, or MBR with partition 0) and adds the partition start to every
 * physical access, bounding all I/O to the partition, so FileX sees a flat
 * volume.
 *
 * [!] THIS FILE OWNS NO MUTABLE STORAGE (cmake/check_no_mutable_storage.py, per
 * board): the partition window, the format mode, the block buffer of the
 * detection and the last probe's result are the board's sd_fs_board_state
 * (shell/cmds/sd_cmd_board.h).
 *
 * Clean-room rewrite; the FileX driver request set and the MBR/BPB field layout
 * were used as a reference only (no code reused).
 */
#include "sd_cmd_board.h"
#include "sd_card_svc.h"   /* the card, through sd_card_board_ops */

#include <stdint.h>

#define LOG_TAG "fxsd"
#include "log.h"

int fx_sd_last_probe_rc(void)
{
	return sd_fs_board_state.last_probe_rc;
}

void fx_sd_set_format_mode(int on)
{
	sd_fs_board_state.format_mode = on;
}

static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* FAT partition types we accept in an MBR entry. */
static int is_fat_part_type(uint8_t t)
{
	return t == 0x01u || t == 0x04u || t == 0x06u ||
	       t == 0x0Bu || t == 0x0Cu || t == 0x0Eu;
}

/* Strong VBR sanity: a FAT boot sector starts with a jump and carries a
 * plausible BPB.  Avoids mistaking MBR boot code for a VBR. */
static int bpb_sane(const uint8_t *b)
{
	uint8_t  spc = b[13];                 /* sectors per cluster */

	if (rd_le16(b + 11) != SD_BLOCK_SIZE) /* bytes per sector */
		return 0;
	if (spc == 0u || (spc & (uint8_t)(spc - 1u)) != 0u)  /* power of two */
		return 0;
	if (rd_le16(b + 14) == 0u)            /* reserved sectors */
		return 0;
	if (b[16] != 1u && b[16] != 2u)       /* number of FATs */
		return 0;
	if (rd_le16(b + 22) == 0u && rd_le32(b + 36) == 0u)  /* FAT size 16/32 */
		return 0;
	if (rd_le16(b + 19) == 0u && rd_le32(b + 32) == 0u)  /* total sectors 16/32 */
		return 0;
	return 1;
}

/*
 * Read LBA 0 and decide the volume layout.  Returns 0 and fills the partition
 * window (part_lba/part_blocks) on success, nonzero (and zeroes it) when no
 * usable FAT layout is found.  All range checks use subtraction form so a
 * corrupt MBR cannot pass via 32-bit wrap.
 */
static int detect_partition(void)
{
	const struct sd_card_info *ci = sd_card_board_ops.info();
	uint32_t block_count = ci->block_count;
	const uint8_t *b = sd_fs_board_state.mbr_buf;

	sd_fs_board_state.part_lba = 0;
	sd_fs_board_state.part_blocks = 0;

	if (block_count == 0u)
		return -1;
	if (sd_card_board_ops.read_blocks(0, sd_fs_board_state.mbr_buf, 1) !=
	    SD_OK)
		return -1;
	if (b[510] != 0x55u || b[511] != 0xAAu)
		return -1;

	/* Superfloppy: VBR directly at LBA 0. */
	if (((b[0] == 0xEBu && b[2] == 0x90u) || b[0] == 0xE9u) && bpb_sane(b)) {
		sd_fs_board_state.part_lba = 0;
		sd_fs_board_state.part_blocks = block_count;
		LOG_INF("superfloppy (VBR@0), %lu blocks",
		        (unsigned long)block_count);
		return 0;
	}

	/* MBR: take partition entry 0 (offset 446, 16-byte entries). */
	{
		const uint8_t *e = b + 446;
		uint32_t first = rd_le32(e + 8);
		uint32_t size  = rd_le32(e + 12);

		if (is_fat_part_type(e[4]) && first != 0u && size != 0u &&
		    first < block_count && size <= block_count - first) {
			sd_fs_board_state.part_lba = first;
			sd_fs_board_state.part_blocks = size;
			LOG_INF("MBR part0 @%lu, %lu blocks",
			        (unsigned long)first, (unsigned long)size);
			return 0;
		}
	}

	LOG_WRN("no FAT VBR/MBR at LBA0");
	return -1;
}

/* Overflow-safe bound: logical..logical+count within the partition and the
 * physical addition does not wrap. */
static int bounds_ok(uint32_t logical, uint32_t count)
{
	if (count == 0u)
		return 0;
	if (logical >= sd_fs_board_state.part_blocks)
		return 0;
	if (count > sd_fs_board_state.part_blocks - logical)
		return 0;
	if (logical > 0xFFFFFFFFu - sd_fs_board_state.part_lba)
		return 0;
	return 1;
}

VOID fx_sd_driver(FX_MEDIA *media_ptr)
{
	UCHAR *buffer = (UCHAR *)media_ptr->fx_media_driver_buffer;
	uint32_t sector = (uint32_t)media_ptr->fx_media_driver_logical_sector;
	uint32_t count  = (uint32_t)media_ptr->fx_media_driver_sectors;

	media_ptr->fx_media_driver_status = FX_IO_ERROR;

	switch (media_ptr->fx_media_driver_request) {
	case FX_DRIVER_INIT:
		/* SD has an internal FTL: no RELEASE_SECTORS forwarding needed. */
		media_ptr->fx_media_driver_free_sector_update = FX_FALSE;
		/* Overwritten unconditionally, before any early return: a stale
		 * SD_ERR_NO_CARD from an earlier mount attempt must never survive to
		 * make the glue misreport a later non-FAT card as an empty slot. */
		sd_fs_board_state.last_probe_rc = sd_card_board_ops.probe();
		if (sd_fs_board_state.last_probe_rc != SD_OK)
			return;
		if (sd_fs_board_state.format_mode) {
			/* Formatting: write the whole card as a superfloppy (a blank
			 * card has no VBR/MBR yet, so detection would fail). */
			sd_fs_board_state.part_lba = 0;
			sd_fs_board_state.part_blocks =
				sd_card_board_ops.info()->block_count;
			if (sd_fs_board_state.part_blocks == 0u)
				return;
		} else if (detect_partition() != 0) {
			return;     /* status stays FX_IO_ERROR -> mount fails cleanly */
		}
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_UNINIT:
		/* Leave the card probed so the next mount is cheap; the card's
		 * release (hot-plug) is the mount glue's eviction, not here. */
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_BOOT_READ:
		if (!bounds_ok(0, 1))
			return;
		if (sd_card_board_ops.read_blocks(sd_fs_board_state.part_lba,
		                                  buffer, 1) != SD_OK)
			return;
		/* FileX leaves the boot-signature check to the driver. */
		if (buffer[510] != 0x55u || buffer[511] != 0xAAu) {
			media_ptr->fx_media_driver_status = FX_MEDIA_INVALID;
			return;
		}
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_READ:
		if (!bounds_ok(sector, count))
			return;
		if (sd_card_board_ops.read_blocks(sd_fs_board_state.part_lba +
		                                  sector, buffer, count) != SD_OK)
			return;
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_WRITE:
		if (!bounds_ok(sector, count))
			return;
		if (sd_card_board_ops.write_blocks(sd_fs_board_state.part_lba +
		                                   sector, buffer, count) != SD_OK)
			return;
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_BOOT_WRITE:
		if (media_ptr->fx_media_bytes_per_sector != SD_BLOCK_SIZE)
			return;
		if (!bounds_ok(0, 1))
			return;
		if (sd_card_board_ops.write_blocks(sd_fs_board_state.part_lba,
		                                   buffer, 1) != SD_OK)
			return;
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	case FX_DRIVER_RELEASE_SECTORS:
	case FX_DRIVER_FLUSH:
	case FX_DRIVER_ABORT:
		/* Block writes are synchronous to the card; nothing to flush. */
		media_ptr->fx_media_driver_status = FX_SUCCESS;
		break;

	default:
		LOG_WRN("unhandled driver request %u",
		        media_ptr->fx_media_driver_request);
		break;
	}
}
