/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared FileX media driver for the microSD
 * (shell/cmds/fx_sd_driver.c, issue #233) as the boards compile it, driven
 * request by request over sd_fake.c's card (a sparse image this test writes).
 *
 * Pinned:
 *   - the layout found at INIT from block 0: a superfloppy VBR, an MBR's
 *     partition 0 (also behind boot code that looks like a jump but carries
 *     no sane BPB), and the refusals -- no signature, a partition type that is
 *     not FAT, an entry with a 0 start or size, one that starts past the card
 *     or whose start + size passes it, including where the sum wraps;
 *   - every request bounded to the partition: the last block, one past it, a
 *     count of 0, a count that passes the end, and a physical address that
 *     would wrap -- refused without a card call;
 *   - format mode: the whole card, block 0 not read, a card of 0 blocks
 *     refused;
 *   - the probe result overwritten at every INIT, before every early return;
 *   - the boot sector: its signature checked on read, its size on write.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sd_fake.h"
#include "sd_cmd_board.h"

static int failures;
static FX_MEDIA media;
static UCHAR buf[4 * SD_BLOCK_SIZE];

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       calls were: \"%s\"\n", sd_fake_calls());
		printf("       log was   : \"%s\"\n", sd_fake_log());
		printf("       window    : %lu + %lu\n",
		       (unsigned long)sd_fs_board_state.part_lba,
		       (unsigned long)sd_fs_board_state.part_blocks);
		failures++;
	}
}

static UINT req(UINT request, ULONG sector, ULONG count)
{
	media.fx_media_driver_request = request;
	media.fx_media_driver_buffer = buf;
	media.fx_media_driver_logical_sector = sector;
	media.fx_media_driver_sectors = count;
	media.fx_media_driver_status = 0xEEu;
	media.fx_media_driver_free_sector_update = FX_TRUE;
	fx_sd_driver(&media);
	return media.fx_media_driver_status;
}

/* A fresh card with nothing on it, the record and the window cleared. */
static void reset(void)
{
	sd_fake_reset();
	memset(&media, 0, sizeof media);
	media.fx_media_bytes_per_sector = SD_BLOCK_SIZE;
}

/* Clear the record only (the card and the window stay). */
static void mark(void)
{
	uint32_t lba = sd_fs_board_state.part_lba, n = sd_fs_board_state.part_blocks;
	struct sd_fake keep = sd_fake;

	sd_fake_reset();
	sd_fake = keep;
	sd_fs_board_state.part_lba = lba;
	sd_fs_board_state.part_blocks = n;
}

static int window(uint32_t lba, uint32_t n)
{
	return sd_fs_board_state.part_lba == lba && sd_fs_board_state.part_blocks == n;
}

static void t_layouts(void)
{
	uint8_t *b;

	printf("the layout found at INIT\n");

	reset();
	sd_fake_put_vbr(0, 62333952u);
	expect("a superfloppy: the whole card",
	       req(FX_DRIVER_INIT, 0, 0) == FX_SUCCESS && window(0, 62333952u) &&
	       media.fx_media_driver_free_sector_update == FX_FALSE &&
	       strstr(sd_fake_log(), "fxsd: superfloppy (VBR@0), 62333952 blocks\n"));
	expect("  ...its boot sector read at LBA 0",
	       req(FX_DRIVER_BOOT_READ, 0, 1) == FX_SUCCESS &&
	       sd_fake.last_read_lba == 0u);

	reset();
	sd_fake_put_mbr(0x0Cu, 8192u, 62325760u);
	sd_fake_put_vbr(8192u, 62325760u);
	expect("an MBR's partition 0: its window",
	       req(FX_DRIVER_INIT, 0, 0) == FX_SUCCESS && window(8192u, 62325760u) &&
	       strstr(sd_fake_log(), "fxsd: MBR part0 @8192, 62325760 blocks\n"));
	expect("  ...the boot sector read at its start",
	       req(FX_DRIVER_BOOT_READ, 0, 1) == FX_SUCCESS &&
	       sd_fake.last_read_lba == 8192u);
	expect("  ...a read offset by its start",
	       req(FX_DRIVER_READ, 5, 2) == FX_SUCCESS &&
	       sd_fake.last_read_lba == 8197u && sd_fake.last_read_count == 2u);
	expect("  ...a write offset by its start",
	       req(FX_DRIVER_WRITE, 7, 1) == FX_SUCCESS &&
	       sd_fake.last_write_lba == 8199u && sd_fake.last_write_count == 1u);

	reset();
	sd_fake_put_mbr(0x0Cu, 2048u, 1000000u);
	b = sd_fake_block(0);
	b[0] = 0xEBu;          /* boot code that starts like a jump ...          */
	b[2] = 0x90u;          /* ... but carries no BPB (bytes/sector 0)        */
	expect("a jump with no sane BPB is not a VBR: the MBR is used",
	       req(FX_DRIVER_INIT, 0, 0) == FX_SUCCESS && window(2048u, 1000000u));

	reset();
	sd_fake_put_vbr(0, 62333952u);
	sd_fake_block(0)[511] = 0x00u;
	expect("no signature: refused, the window zeroed, nothing logged",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR && window(0, 0) &&
	       sd_fake_log()[0] == '\0');

	reset();
	sd_fake_put_mbr(0x83u, 2048u, 1000000u);
	expect("a partition type that is not FAT: refused, and logged",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR && window(0, 0) &&
	       strstr(sd_fake_log(), "fxsd: no FAT VBR/MBR at LBA0\n"));

	reset();
	sd_fake_put_mbr(0x0Cu, 0u, 1000u);
	expect("a start of 0: refused", req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR);
	reset();
	sd_fake_put_mbr(0x0Cu, 2048u, 0u);
	expect("a size of 0: refused", req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR);
	reset();
	sd_fake_put_mbr(0x0Cu, 62333952u, 1u);
	expect("a start at the card's end: refused",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR);
	reset();
	sd_fake_put_mbr(0x0Cu, 2048u, 62333952u - 2048u + 1u);
	expect("a start + size one past the card: refused",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR);
	reset();
	sd_fake_put_mbr(0x0Cu, 2048u, 62333952u - 2048u);
	expect("a start + size exactly at the card's end: taken",
	       req(FX_DRIVER_INIT, 0, 0) == FX_SUCCESS &&
	       window(2048u, 62333952u - 2048u));
	reset();
	sd_fake_put_mbr(0x0Cu, 2048u, 0xFFFFFF00u);   /* 2048 + size wraps */
	expect("a start + size that wraps 32 bits: refused",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR && window(0, 0));

	reset();
	sd_fake.block_count = 0u;
	sd_fake_put_vbr(0, 62333952u);
	expect("a card of 0 blocks: refused before block 0 is read",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR &&
	       sd_fake_at("read@") < 0);

	reset();
	sd_fake_put_vbr(0, 62333952u);
	sd_fake.read_rc = SD_ERR_HAL;
	expect("block 0 unreadable: refused",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR && window(0, 0));
}

static void t_bounds(void)
{
	printf("every request bounded to the partition\n");
	reset();
	sd_fake_put_mbr(0x0Cu, 8192u, 1000u);
	(void)req(FX_DRIVER_INIT, 0, 0);

	mark();
	expect("the last block: read",
	       req(FX_DRIVER_READ, 999, 1) == FX_SUCCESS &&
	       sd_fake.last_read_lba == 9191u);
	mark();
	expect("one past it: refused, no card call",
	       req(FX_DRIVER_READ, 1000, 1) == FX_IO_ERROR &&
	       sd_fake_at("read@") < 0);
	mark();
	expect("a count that passes the end: refused, no card call",
	       req(FX_DRIVER_READ, 998, 3) == FX_IO_ERROR &&
	       sd_fake_at("read@") < 0);
	mark();
	expect("a count of 0: refused, no card call",
	       req(FX_DRIVER_READ, 0, 0) == FX_IO_ERROR && sd_fake_at("read@") < 0);
	mark();
	expect("a write past the end: refused, no card call",
	       req(FX_DRIVER_WRITE, 999, 2) == FX_IO_ERROR &&
	       sd_fake_at("write@") < 0);

	/* A window whose physical end would wrap (a window INIT cannot make,
	 * set by hand to reach the last guard). */
	mark();
	sd_fs_board_state.part_lba = 0xFFFFFF00u;
	sd_fs_board_state.part_blocks = 0x1000u;
	expect("inside the window, but start + sector would wrap: refused",
	       req(FX_DRIVER_READ, 0x100, 1) == FX_IO_ERROR &&
	       req(FX_DRIVER_WRITE, 0x200, 1) == FX_IO_ERROR &&
	       sd_fake_at("read@") < 0 && sd_fake_at("write@") < 0);
	mark();
	expect("  ...the last sector that does not wrap: read",
	       req(FX_DRIVER_READ, 0xFF, 1) == FX_SUCCESS &&
	       sd_fake.last_read_lba == 0xFFFFFFFFu);

	reset();
	sd_fake_put_mbr(0x0Cu, 8192u, 1000u);
	(void)req(FX_DRIVER_INIT, 0, 0);
	sd_fake.read_rc = SD_ERR_TIMEOUT;
	expect("the card fails a read: I/O error", req(FX_DRIVER_READ, 0, 1) == FX_IO_ERROR);
	sd_fake.write_rc = SD_ERR_HAL;
	expect("the card fails a write: I/O error",
	       req(FX_DRIVER_WRITE, 0, 1) == FX_IO_ERROR);
}

static void t_boot_sector(void)
{
	printf("the boot sector\n");
	reset();
	sd_fake_put_mbr(0x0Cu, 8192u, 1000u);
	(void)req(FX_DRIVER_INIT, 0, 0);
	expect("no signature at the partition's start: media invalid",
	       req(FX_DRIVER_BOOT_READ, 0, 1) == FX_MEDIA_INVALID);
	mark();
	media.fx_media_bytes_per_sector = 4096u;
	expect("a write of another sector size: refused, nothing written",
	       req(FX_DRIVER_BOOT_WRITE, 0, 1) == FX_IO_ERROR &&
	       sd_fake_at("write@") < 0);
	media.fx_media_bytes_per_sector = SD_BLOCK_SIZE;
	expect("a 512 B write: at the partition's start",
	       req(FX_DRIVER_BOOT_WRITE, 0, 1) == FX_SUCCESS &&
	       sd_fake.last_write_lba == 8192u);
	mark();
	expect("UNINIT / FLUSH / ABORT / RELEASE_SECTORS: success, no card call",
	       req(FX_DRIVER_UNINIT, 0, 0) == FX_SUCCESS &&
	       req(FX_DRIVER_FLUSH, 0, 0) == FX_SUCCESS &&
	       req(FX_DRIVER_ABORT, 0, 0) == FX_SUCCESS &&
	       req(FX_DRIVER_RELEASE_SECTORS, 0, 0) == FX_SUCCESS &&
	       sd_fake_calls()[0] == '\0');
	expect("an unknown request: I/O error, logged",
	       req(99u, 0, 0) == FX_IO_ERROR &&
	       strstr(sd_fake_log(), "fxsd: unhandled driver request 99\n"));
}

static void t_format_mode(void)
{
	printf("format mode\n");
	reset();
	fx_sd_set_format_mode(1);
	expect("the whole card, block 0 not read",
	       req(FX_DRIVER_INIT, 0, 0) == FX_SUCCESS && window(0, 62333952u) &&
	       sd_fake_at("read@") < 0);
	reset();
	fx_sd_set_format_mode(1);
	sd_fake.block_count = 0u;
	expect("a card of 0 blocks: refused",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR);
	reset();
	fx_sd_set_format_mode(1);
	fx_sd_set_format_mode(0);
	expect("off again: a blank card is refused",
	       sd_fs_board_state.format_mode == 0 &&
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR);
}

static void t_probe_result(void)
{
	printf("the probe result, at every INIT\n");
	reset();
	sd_fake.present = 0;
	expect("an empty slot: recorded, INIT fails, the card not read",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR &&
	       fx_sd_last_probe_rc() == SD_ERR_NO_CARD && sd_fake_at("read@") < 0);
	sd_fake.present = 1;             /* a card that is not FAT */
	expect("then a card that is not FAT: overwritten with success, though "
	       "INIT fails",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR &&
	       fx_sd_last_probe_rc() == SD_OK);
	sd_fake.probe_rc = SD_ERR_TIMEOUT;
	expect("then a probe that times out: that",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR &&
	       fx_sd_last_probe_rc() == SD_ERR_TIMEOUT);
	sd_fake.probe_rc = SD_OK;
	sd_fake.present = 0;
	fx_sd_set_format_mode(1);
	expect("in format mode too",
	       req(FX_DRIVER_INIT, 0, 0) == FX_IO_ERROR &&
	       fx_sd_last_probe_rc() == SD_ERR_NO_CARD);
	fx_sd_set_format_mode(0);
	sd_fake.present = 1;
	sd_fake_put_vbr(0, 62333952u);
	expect("then a FAT card: success",
	       req(FX_DRIVER_INIT, 0, 0) == FX_SUCCESS &&
	       fx_sd_last_probe_rc() == SD_OK);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("test_fx_sd_driver\n");
	t_layouts();
	t_bounds();
	t_boot_sector();
	t_format_mode();
	t_probe_result();
	if (failures) {
		printf("test_fx_sd_driver: %d FAILED\n", failures);
		return 1;
	}
	printf("test_fx_sd_driver: all passed\n");
	return 0;
}
