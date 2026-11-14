/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host fakes under the shared microSD mount glue and FileX media driver
 * (shell/cmds/sd_fs_glue.c, shell/cmds/fx_sd_driver.c, issue #233): a card
 * behind the board's table (sd_card_board_ops, once per shape), FileX's
 * open / close / abort / format and the calls the filesystem bodies make, the
 * ThreadX mutexes, the board's log, and the board's state object.  Every call
 * is recorded in order as "name[LB]|": L when the mount lock is held at that
 * moment, B when the media's exclusive slot is (the state's busy mark); "-"
 * for either when not.
 *
 * fx_media_open() runs the driver the way FileX does: INIT, and only when INIT
 * succeeded the boot-sector read; a failed INIT is FX_IO_ERROR and leaves the
 * media closed.  fx_media_format() runs INIT and writes a FAT boot sector
 * through the driver.  The filesystem bodies' FileX calls (and fx_media_format)
 * are weak, so a test that has its own (camera_svc_stub.c, test_sd_cmd.c)
 * keeps them.
 *
 * Giving the exclusive slot back (the mount lock released with the busy mark
 * dropped) is recorded as "slot_released" and marks the media "not FAT32", as
 * another console's umount closing it would: an answer read after the release
 * is seen.
 *
 * Shapes of the card table: SD_FAKE_SHAPE_NOPROBE / NOSTATUS / NOINFO /
 * NOREAD / NOWRITE / NORELEASE leave that field NULL; none, every field set.
 */
#ifndef SD_FAKE_H
#define SD_FAKE_H

#include <stdint.h>

#include "fx_api.h"
#include "sd_card_svc.h"

#define SD_FAKE_BLOCKS 6

struct sd_fake {
	/* ---- the card ---- */
	int      present;       /* 0: probe and status answer SD_ERR_NO_CARD  */
	int      probe_rc;      /* with a card: what probe answers            */
	int      status_rc;     /* with a card: what status answers once probed */
	int      probed;        /* set by a successful probe, cleared by release */
	int      read_rc, write_rc;
	uint32_t block_count;
	/* A sparse image: these LBAs hold these blocks, every other reads 0. */
	int      nblk;
	uint32_t blk_lba[SD_FAKE_BLOCKS];
	uint8_t  blk[SD_FAKE_BLOCKS][SD_BLOCK_SIZE];
	uint32_t last_read_lba, last_read_count;
	uint32_t last_write_lba, last_write_count;
	/* ---- FileX ---- */
	UINT     open_rc;       /* after INIT + boot read succeeded: forced     */
	UINT     fat32;         /* the FAT32 flag a successful open leaves      */
	UINT     format_rc;
	int      opened;        /* the fake's own view: open and not closed     */
	int      double_open;   /* an open while already open (a glue bug)      */
	/* ---- ThreadX ---- */
	int      mutexes_created;
	int      lock_held;     /* the mount lock                               */
	int      lock_nested;   /* a get while held (never expected)            */
	int      lock_gets, lock_puts, dir_gets, dir_puts;
	int      fx_inits;
};

extern struct sd_fake sd_fake;

/* Zero the board's state, the record and the log; a present, probed-able
 * 32 GB card with nothing on it. */
void sd_fake_reset(void);

/* Put a FAT boot sector (a superfloppy VBR covering @p total) at @p lba. */
void sd_fake_put_vbr(uint32_t lba, uint32_t total);
/* Put an MBR at LBA 0 whose entry 0 is (@p type, @p first, @p size). */
void sd_fake_put_mbr(uint8_t type, uint32_t first, uint32_t size);
/* Put raw bytes at LBA @p lba (the rest of the block zero). */
uint8_t *sd_fake_block(uint32_t lba);

const char *sd_fake_calls(void);
const char *sd_fake_log(void);
/* Position of @p what in the record, or -1; and how many times it is in. */
int sd_fake_at(const char *what);
int sd_fake_count(const char *what);
/* Anything on the card, in FileX or on a lock at all. */
int sd_fake_touched_anything(void);

#endif /* SD_FAKE_H */
