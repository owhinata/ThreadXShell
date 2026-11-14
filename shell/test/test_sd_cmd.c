/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the one shared `sd` command (shell/cmds/cmd_sd.c, issue #232)
 * through the REAL dispatcher, as the boards compile it, over stand-ins that
 * record every call in order: the board's mount glue and FileX media driver
 * (shell/cmds/sd_cmd_board.h), fx_media_format() (fx_shim/fx_api.h), the
 * filesystem bodies (fs_cmd_core.h) and the card driver table
 * (svc/fs/sd_card_svc.h, defined here).  Each recorded call carries whether
 * an ownership slot (exclusive or raw) was held when it was made.
 *
 * Built once per shape of the card driver table:
 *   FULL      every field set (what f746g-disco and wio-lite-ai define)
 *   NOPROBE / NOSTATUS / NOINFO / NOREAD / NOWRITE / NORELEASE
 *             that one field left NULL
 *
 * Pinned (nothing typed at a console shows the order of these):
 *   - `sd format`: without `yes`, or with the exclusive slot refused, nothing
 *     is touched.  On success the order is slot -> unmount -> probe -> format
 *     mode on -> format -> format mode off -> mount with the slot held -> the
 *     FAT32 answer, and the slot is given back once, after all of them.  The
 *     stand-in that gives the slot back marks the media "not FAT32" (as a
 *     concurrent umount closing it would), so an implementation that reads
 *     the answer after giving the slot back says "did not produce FAT32".
 *     Every exit (no card, too small, format failed, mount failed, not FAT32)
 *     gives the slot back exactly once and leaves format mode off.
 *   - `sd info` / `sd read`: refused while the slot is refused, touching no
 *     card; every exit gives the slot back once; `read` with an unreadable LBA
 *     takes no slot.
 *   - with a field of the table empty, `info`, `read` and `format` refuse
 *     before any slot, unmount or card call; the filesystem subcommands do not
 *     use the table and still run.
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "fs_cmd_core.h"
#include "sd_card_svc.h"
#include "sd_cmd_board.h"

#if !defined(SD_TEST_SHAPE_FULL) && !defined(SD_TEST_SHAPE_NOPROBE) && \
    !defined(SD_TEST_SHAPE_NOSTATUS) && !defined(SD_TEST_SHAPE_NOINFO) && \
    !defined(SD_TEST_SHAPE_NOREAD) && !defined(SD_TEST_SHAPE_NOWRITE) && \
    !defined(SD_TEST_SHAPE_NORELEASE)
#error "define one SD_TEST_SHAPE_*"
#endif

static int failures;

/* ---- the call record ------------------------------------------------------ */

static char t_rec[8192];
static int t_held;                /* slots (exclusive or raw) not yet given back */

/* Append one call, tagged [H] if a slot is held at that moment, else [-]. */
static void rec(const char *fmt, ...)
{
	size_t n = strlen(t_rec);
	va_list ap;

	if (n + 8 >= sizeof t_rec)
		return;
	va_start(ap, fmt);
	vsnprintf(t_rec + n, sizeof t_rec - n - 5, fmt, ap);
	va_end(ap);
	strcat(t_rec, t_held > 0 ? "[H]|" : "[-]|");
}

/* Position of @p what in the record, or -1. */
static int at(const char *what)
{
	const char *p = strstr(t_rec, what);

	return p ? (int)(p - t_rec) : -1;
}

__attribute__((unused)) static int count(const char *what)
{
	int n = 0;
	const char *p = t_rec;

	while ((p = strstr(p, what)) != NULL) {
		n++;
		p += strlen(what);
	}
	return n;
}

/* The record ends with @p what (nothing recorded after it). */
__attribute__((unused)) static int last(const char *what)
{
	size_t n = strlen(t_rec), w = strlen(what);

	return n >= w && strcmp(t_rec + n - w, what) == 0;
}

/* Every call between the first @p from and the last @p to was made with a
 * slot held: no "[-]" in between. */
__attribute__((unused)) static int held_between(const char *from, const char *to)
{
	const char *a = strstr(t_rec, from), *b = NULL, *p = t_rec;

	if (a == NULL || (a = strchr(a, '|')) == NULL)   /* past from's own tag */
		return 0;
	while ((p = strstr(p, to)) != NULL) {
		b = p;
		p += strlen(to);
	}
	if (b == NULL || b < a)
		return 0;
	for (p = a; p < b; p++)
		if (strncmp(p, "[-]", 3) == 0)
			return 0;
	return 1;
}

/* ---- the mount glue and FileX stand-ins ----------------------------------- */

static UINT t_excl_rc, t_raw_rc;  /* what the begin calls answer */
static UINT t_format_rc, t_mount_rc;
static UINT t_fat32_after;        /* the FAT32 flag a successful mount leaves */
static int t_fmt_mode;            /* fx_sd_set_format_mode()'s current value */
static FX_MEDIA t_media;
static UCHAR t_cache[64];
static ULONG t_fmt_total;         /* what fx_media_format() was handed */
static UINT t_fmt_bps, t_fmt_spc;
static int t_fmt_args_ok;

UINT sd_exclusive_begin(void)
{
	rec("excl_begin");
	if (t_excl_rc == FX_SUCCESS)
		t_held++;
	return t_excl_rc;
}

/* Giving the slot back is where another console's umount could close the
 * media: mark it "not FAT32" so a late read of the flag is seen. */
void sd_exclusive_end(void)
{
	t_held--;
	rec("excl_end");
	t_media.fx_media_32_bit_FAT = FX_FALSE;
}

UINT sd_raw_begin(void)
{
	rec("raw_begin");
	if (t_raw_rc == FX_SUCCESS)
		t_held++;
	return t_raw_rc;
}

void sd_raw_end(void)
{
	t_held--;
	rec("raw_end");
}

UINT sd_op_begin(void) { rec("op_begin"); return FX_SUCCESS; }
void sd_op_end(void) { rec("op_end"); }
void sd_dir_lock(void) { rec("dir_lock"); }
void sd_dir_unlock(void) { rec("dir_unlock"); }
int sd_is_mounted(void) { return 0; }

UINT sd_media_acquire(FX_MEDIA **out)
{
	rec("acquire");
	*out = &t_media;
	return FX_SUCCESS;
}

UINT sd_media_mount_exclusive(FX_MEDIA **out)
{
	rec("mount_excl");
	if (t_mount_rc != FX_SUCCESS)
		return t_mount_rc;
	t_media.fx_media_32_bit_FAT = t_fat32_after;
	*out = &t_media;
	return FX_SUCCESS;
}

UINT sd_media_unmount(void)
{
	rec("unmount");
	return FX_SUCCESS;
}

FX_MEDIA *sd_glue_media(void) { return &t_media; }
UCHAR *sd_glue_cache(void) { return t_cache; }
ULONG sd_glue_cache_size(void) { return sizeof t_cache; }

VOID fx_sd_driver(FX_MEDIA *media_ptr)
{
	(void)media_ptr;
	rec("driver");
}

void fx_sd_set_format_mode(int on)
{
	rec("fmode%d", on);
	t_fmt_mode = on;
}

UINT fx_media_format(FX_MEDIA *media_ptr, VOID (*driver)(FX_MEDIA *media),
                     VOID *driver_info_ptr, UCHAR *memory_ptr, UINT memory_size,
                     CHAR *volume_name, UINT number_of_fats,
                     UINT directory_entries, UINT hidden_sectors,
                     ULONG total_sectors, UINT bytes_per_sector,
                     UINT sectors_per_cluster, UINT heads,
                     UINT sectors_per_track)
{
	(void)volume_name;
	(void)heads;
	(void)sectors_per_track;
	rec("format");
	t_fmt_total = total_sectors;
	t_fmt_bps = bytes_per_sector;
	t_fmt_spc = sectors_per_cluster;
	t_fmt_args_ok = media_ptr == &t_media && driver == fx_sd_driver &&
	                driver_info_ptr == FX_NULL && memory_ptr == t_cache &&
	                memory_size == sizeof t_cache && number_of_fats == 2u &&
	                directory_entries == 0u && hidden_sectors == 0u;
	return t_format_rc;
}

/* ---- the filesystem bodies (fs_cmd_core.c is not linked) ------------------- */

const char *fs_strerror(UINT status)
{
	switch (status) {
	case FX_SUCCESS:     return "success";
	case FS_ERR_BUSY:    return "busy (format in progress)";
	case FS_ERR_NO_CARD: return "no card";
	default:             return "TESTFX error";
	}
}

static const struct fs_device *t_dev_seen;

#define FS_STUB(fn)                                                          \
	int fn(const struct fs_device *dev, struct cli_instance *sh, int argc, \
	       char **argv)                                                    \
	{                                                                      \
		(void)sh;                                                      \
		(void)argc;                                                    \
		(void)argv;                                                    \
		t_dev_seen = dev;                                              \
		rec(#fn);                                                      \
		return 0;                                                      \
	}
FS_STUB(fs_core_ls)
FS_STUB(fs_core_cat)
FS_STUB(fs_core_write)
FS_STUB(fs_core_rm)
FS_STUB(fs_core_mkdir)
FS_STUB(fs_core_info)
FS_STUB(fs_core_umount)

/* ---- the card driver table, per shape ------------------------------------- */

static int t_probe_rc, t_status_rc, t_read_rc;
static struct sd_card_info t_info;
static uint32_t t_read_lba;

__attribute__((unused)) static int t_probe(void)
{
	rec("probe");
	return t_probe_rc;
}

__attribute__((unused)) static int t_status(void)
{
	rec("status");
	return t_status_rc;
}

__attribute__((unused)) static const struct sd_card_info *t_get_info(void)
{
	rec("info");
	return &t_info;
}

__attribute__((unused)) static int t_read_blocks(uint32_t lba, void *buf, uint32_t count_)
{
	rec("read_blk");
	t_read_lba = lba;
	if (t_read_rc == SD_OK && count_ == 1u)
		memset(buf, 0xA5, SD_BLOCK_SIZE);
	return t_read_rc;
}

/* The command itself never writes nor releases the card (the FileX media
 * driver and the mount glue do, and they are stand-ins here): these record a
 * call so a test would see one. */
__attribute__((unused)) static int t_write_blocks(uint32_t lba, const void *buf,
                                                  uint32_t count_)
{
	(void)lba;
	(void)buf;
	(void)count_;
	rec("write_blk");
	return SD_OK;
}

__attribute__((unused)) static int t_release(void)
{
	rec("release");
	return SD_OK;
}

#if defined(SD_TEST_SHAPE_NOPROBE)
#define T_PROBE NULL
#else
#define T_PROBE t_probe
#endif
#if defined(SD_TEST_SHAPE_NOSTATUS)
#define T_STATUS NULL
#else
#define T_STATUS t_status
#endif
#if defined(SD_TEST_SHAPE_NOINFO)
#define T_INFO NULL
#else
#define T_INFO t_get_info
#endif
#if defined(SD_TEST_SHAPE_NOREAD)
#define T_READ NULL
#else
#define T_READ t_read_blocks
#endif
#if defined(SD_TEST_SHAPE_NOWRITE)
#define T_WRITE NULL
#else
#define T_WRITE t_write_blocks
#endif
#if defined(SD_TEST_SHAPE_NORELEASE)
#define T_RELEASE NULL
#else
#define T_RELEASE t_release
#endif

const struct sd_card_ops sd_card_board_ops = {
	.probe = T_PROBE, .status = T_STATUS, .info = T_INFO,
	.read_blocks = T_READ, .write_blocks = T_WRITE, .release = T_RELEASE,
};

/* ---- harness --------------------------------------------------------------- */

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

static void reset(void)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(NULL, NULL);

	t_rec[0] = '\0';
	t_held = 0;
	t_excl_rc = t_raw_rc = FX_SUCCESS;
	t_format_rc = t_mount_rc = FX_SUCCESS;
	t_fat32_after = FX_TRUE;
	t_fmt_mode = 0;
	memset(&t_media, 0, sizeof t_media);
	t_fmt_total = 0;
	t_fmt_bps = t_fmt_spc = 0;
	t_fmt_args_ok = 0;
	t_dev_seen = NULL;
	t_probe_rc = SD_OK;
	t_status_rc = SD_OK;
	t_read_rc = SD_OK;
	t_read_lba = 0xFFFFFFFFu;
	memset(&t_info, 0, sizeof t_info);
	t_info.type = 1u;
	t_info.version = 1u;
	t_info.block_count = 62333952u;           /* a 32 GB card */
	t_info.block_size = SD_BLOCK_SIZE;
	t_info.bus_width = 4u;
	t_info.capacity_bytes = 62333952ull * SD_BLOCK_SIZE;
	t_info.rca = 0x1234u;
	t_info.card_class = 0x5b5u;
}

static void run(const char *line)
{
	cli_dummy_clear_output(&tr0);
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		printf("       calls were: \"%s\"\n", t_rec);
		failures++;
	}
}

/* No ownership slot, unmount or card call in the record. */
static int touched_nothing(void)
{
	return at("excl_begin") < 0 && at("raw_begin") < 0 && at("unmount") < 0 &&
	       at("probe") < 0 && at("status") < 0 && at("info") < 0 &&
	       at("read_blk") < 0 && at("format") < 0 && at("fmode") < 0 &&
	       at("mount_excl") < 0 && at("acquire") < 0 &&
	       at("write_blk") < 0 && at("release") < 0;
}

/* ---- the full table -------------------------------------------------------- */

#if defined(SD_TEST_SHAPE_FULL)
static void t_format_refusals(void)
{
	printf("format: refusals that touch nothing\n");

	reset();
	run("sd format\r");
	expect("no `yes`: asks for it, touches nothing",
	       has("sd: destructive; confirm with `sd format yes`\r\n") &&
	       touched_nothing());
	reset();
	run("sd format no\r");
	expect("`no`: asks for `yes`, touches nothing",
	       has("confirm with `sd format yes`") && touched_nothing());
	reset();
	run("sd format exfat\r");
	expect("`exfat`: asks for `sd format yes`, touches nothing",
	       has("confirm with `sd format yes`") && touched_nothing());
	reset();
	run("sd format exfat yes\r");
	expect("`exfat yes`: refused by the shell as an argument count, "
	       "touches nothing",
	       has("invalid number of arguments") && !has("exFAT") &&
	       touched_nothing());

	reset();
	t_excl_rc = FS_ERR_BUSY;
	run("sd format yes\r");
	expect("the exclusive slot refused: says busy, nothing else touched, "
	       "nothing given back",
	       has("sd: busy (format in progress)\r\n") &&
	       count("excl_begin") == 1 && count("excl_end") == 0 &&
	       at("unmount") < 0 && at("probe") < 0 && at("format") < 0 &&
	       at("fmode") < 0 && t_held == 0);
}

static void t_format_success(void)
{
	printf("format: the order, with the slot held throughout\n");

	reset();
	run("sd format yes\r");
	expect("the calls in order",
	       at("excl_begin[-]") == 0 &&
	       at("excl_begin") < at("unmount[H]") &&
	       at("unmount[H]") < at("probe[H]") &&
	       at("probe[H]") < at("info[H]") &&
	       at("info[H]") < at("fmode1[H]") &&
	       at("fmode1[H]") < at("format[H]") &&
	       at("format[H]") < at("fmode0[H]") &&
	       at("fmode0[H]") < at("mount_excl[H]") &&
	       at("mount_excl[H]") < at("excl_end"));
	expect("the slot given back once, as the last call",
	       count("excl_end") == 1 && last("excl_end[-]|") && t_held == 0);
	expect("every call between taking and giving back the slot is held",
	       held_between("excl_begin", "excl_end"));
	expect("the remount is the exclusive entry, not the normal one",
	       count("mount_excl[H]") == 1 && at("acquire") < 0);
	expect("format mode is off afterwards", t_fmt_mode == 0);
	expect("formatted the whole card as FAT32 with 512 B sectors",
	       t_fmt_args_ok && t_fmt_total == 62333952ul && t_fmt_bps == 512u &&
	       t_fmt_spc == 32u);
	/* The release stand-in marks the media "not FAT32": this answer is the
	 * one read while the slot was still held. */
	expect("the FAT32 answer read before the slot was given back",
	       has("formatting 30436 MiB as FAT32 (16384 B clusters)...\r\n") &&
	       has("formatted and mounted (FAT32)\r\n") && !has("did not produce"));
}

static void exit_once(const char *what, const char *say)
{
	char line[160];

	snprintf(line, sizeof line, "%s: says so, the slot given back once, last, "
	         "format mode off", what);
	expect(line, has(say) && count("excl_end") == 1 && last("excl_end[-]|") &&
	             t_held == 0 && t_fmt_mode == 0 &&
	             held_between("excl_begin", "excl_end") &&
	             !has("formatted and mounted"));
}

static void t_format_exits(void)
{
	printf("format: every failing exit gives the slot back once\n");

	reset();
	t_probe_rc = SD_ERR_NO_CARD;
	run("sd format yes\r");
	exit_once("no card", "sd: no card in slot\r\n");
	expect("  ...and no format was tried", at("format") < 0 && at("fmode") < 0);

	reset();
	t_info.block_count = 1000u;
	run("sd format yes\r");
	exit_once("a card too small", "sd: card too small for FAT32\r\n");
	expect("  ...and no format was tried", at("format") < 0 && at("fmode") < 0);

	reset();
	t_info.block_count = 0u;
	run("sd format yes\r");
	exit_once("a card of 0 blocks", "sd: card too small for FAT32\r\n");

	reset();
	t_format_rc = 0x90u;
	run("sd format yes\r");
	exit_once("the format failed", "sd: format failed: TESTFX error (0x90)\r\n");
	expect("  ...format mode on and off around it, no mount",
	       at("fmode1[H]") < at("format[H]") &&
	       at("format[H]") < at("fmode0[H]") && at("mount_excl") < 0);

	reset();
	t_mount_rc = 0x02u;
	run("sd format yes\r");
	exit_once("the mount after the format failed",
	          "sd: mount after format failed: TESTFX error (0x02)\r\n");
	expect("  ...the mount was made with the slot held",
	       at("mount_excl[H]") >= 0);

	reset();
	t_mount_rc = FS_ERR_NO_CARD;
	run("sd format yes\r");
	exit_once("the card gone before the remount",
	          "sd: mount after format failed: no card (0xf1)\r\n");

	reset();
	t_fat32_after = FX_FALSE;
	run("sd format yes\r");
	exit_once("not FAT32", "sd: format did not produce FAT32 (card too small)\r\n");
}

static void t_info_cmd(void)
{
	printf("info: the raw slot, the card only under it\n");

	reset();
	t_raw_rc = FS_ERR_BUSY;
	run("sd info\r");
	expect("mounted or busy: refused, no card call, nothing given back",
	       has("sd: filesystem mounted or busy; run `sd umount` first\r\n") &&
	       count("raw_begin") == 1 && at("raw_end") < 0 && at("status") < 0 &&
	       at("probe") < 0 && at("info") < 0 && t_held == 0);

	reset();
	run("sd info\r");
	expect("a ready card: status, the info, then the slot given back once",
	       at("raw_begin") == 0 && at("raw_begin") < at("status[H]") &&
	       at("status[H]") < at("info[H]") && at("probe") < 0 &&
	       count("raw_end") == 1 && last("raw_end[-]|") && t_held == 0);
	expect("the lines, and no clock line",
	       has("type      : SDHC/SDXC (v2.x)\r\n") &&
	       has("capacity  : 30436 MiB (62333952 blocks x 512 B)\r\n") &&
	       has("bus width : 4-bit\r\n") && has("rca       : 0x1234\r\n") &&
	       has("ccc       : 0x5b5\r\n") && has("cid       : ") &&
	       has("csd       : ") && !has("clock"));

	reset();
	t_status_rc = SD_ERR_STATE;
	run("sd info\r");
	expect("a card not yet identified: probed under the slot",
	       at("status[H]") < at("probe[H]") && at("probe[H]") < at("info[H]") &&
	       count("raw_end") == 1 && t_held == 0);

	reset();
	t_status_rc = SD_ERR_STATE;
	t_probe_rc = SD_ERR_NO_CARD;
	run("sd info\r");
	expect("the probe fails: says so, no info, the slot given back once",
	       has("sd: no card in slot\r\n") && at("info") < 0 &&
	       count("raw_end") == 1 && last("raw_end[-]|") && t_held == 0 &&
	       !has("type      :"));
}

static void t_read_cmd(void)
{
	printf("read: the raw slot, an unreadable LBA takes none\n");

	reset();
	run("sd read 12x\r");
	expect("an unreadable LBA: refused before any slot",
	       has("sd: bad LBA '12x'\r\n") && touched_nothing());

	reset();
	t_raw_rc = FS_ERR_BUSY;
	run("sd read 0\r");
	expect("mounted or busy: refused, no card call, nothing given back",
	       has("filesystem mounted or busy; run `sd umount` first") &&
	       at("read_blk") < 0 && at("status") < 0 && at("raw_end") < 0 &&
	       t_held == 0);

	reset();
	run("sd read 2\r");
	expect("a ready card: the block read under the slot, given back once",
	       at("raw_begin") < at("status[H]") &&
	       at("status[H]") < at("read_blk[H]") && count("raw_end") == 1 &&
	       last("raw_end[-]|") && t_held == 0 && t_read_lba == 2u);
	expect("  ...the hexdump at the block's byte offset",
	       has("00000400") && has("a5 a5"));

	reset();
	t_read_rc = SD_ERR_HAL;
	run("sd read 0\r");
	expect("the read fails: the board-independent sentence, given back once",
	       has("sd: read failed: SDMMC transfer failed\r\n") &&
	       count("raw_end") == 1 && t_held == 0);

	reset();
	t_status_rc = SD_ERR_STATE;
	t_probe_rc = SD_ERR_TIMEOUT;
	run("sd read 0\r");
	expect("the probe fails: says so, no read, given back once",
	       has("sd: card not ready / transfer timed out\r\n") &&
	       at("read_blk") < 0 && count("raw_end") == 1 && t_held == 0);
}

static void t_fs_binders(void)
{
	const struct fs_device *d;

	printf("the filesystem subcommands and the medium\n");

	reset();
	run("sd ls\r");
	run("sd cat /a\r");
	run("sd write /a x\r");
	run("sd rm /a\r");
	run("sd mkdir /d\r");
	run("sd df\r");
	run("sd umount\r");
	expect("each reaches its shared body with the sd medium",
	       at("fs_core_ls") >= 0 && at("fs_core_cat") >= 0 &&
	       at("fs_core_write") >= 0 && at("fs_core_rm") >= 0 &&
	       at("fs_core_mkdir") >= 0 && at("fs_core_info") >= 0 &&
	       at("fs_core_umount") >= 0 && t_dev_seen == fs_sd_device() &&
	       at("probe") < 0 && at("status") < 0);

	d = fs_sd_device();
	expect("the medium: its name, its hint and the board's glue",
	       strcmp(d->name, "sd") == 0 &&
	       strcmp(d->mount_hint, "no FAT filesystem? insert a FAT-formatted "
	                             "card or run `sd format yes`") == 0 &&
	       d->acquire == sd_media_acquire && d->unmount == sd_media_unmount &&
	       d->is_mounted == sd_is_mounted && d->op_begin == sd_op_begin &&
	       d->op_end == sd_op_end && d->excl_begin == sd_exclusive_begin &&
	       d->excl_end == sd_exclusive_end && d->dir_lock == sd_dir_lock &&
	       d->dir_unlock == sd_dir_unlock && d->info_extra == NULL);
}
#endif

/* ---- a field of the table left empty --------------------------------------- */

#if !defined(SD_TEST_SHAPE_FULL)
static void t_empty_field(void)
{
	static const char *const lines[] = {
		"sd info\r", "sd read 0\r", "sd read 12x\r", "sd format yes\r",
		"sd format\r",
	};
	const char *say = "sd: refused -- this firmware's card driver table "
	                  "(sd_card_board_ops) has an empty field\r\n";
	char what[96];
	unsigned i;

	printf("a field of the card table left empty: refused, touching nothing\n");
	for (i = 0; i < sizeof lines / sizeof lines[0]; i++) {
		reset();
		run(lines[i]);
		snprintf(what, sizeof what, "`%.*s`: refused before any slot, "
		         "unmount or card call", (int)strlen(lines[i]) - 1, lines[i]);
		expect(what, has(say) && touched_nothing() && t_held == 0 &&
		             t_rec[0] == '\0');
	}

	reset();
	run("sd ls\r");
	expect("`sd ls` does not use the table and still runs",
	       at("fs_core_ls") >= 0 && !has("refused"));
}
#endif

int main(void)
{
	/* A call through an empty field crashes: keep what was printed before. */
	setvbuf(stdout, NULL, _IONBF, 0);
#if defined(SD_TEST_SHAPE_FULL)
	printf("test_sd_cmd (FULL)\n");
	t_format_refusals();
	t_format_success();
	t_format_exits();
	t_info_cmd();
	t_read_cmd();
	t_fs_binders();
#else
	printf("test_sd_cmd (%s)\n",
#if defined(SD_TEST_SHAPE_NOPROBE)
	       "NOPROBE"
#elif defined(SD_TEST_SHAPE_NOSTATUS)
	       "NOSTATUS"
#elif defined(SD_TEST_SHAPE_NOINFO)
	       "NOINFO"
#elif defined(SD_TEST_SHAPE_NOREAD)
	       "NOREAD"
#elif defined(SD_TEST_SHAPE_NOWRITE)
	       "NOWRITE"
#else
	       "NORELEASE"
#endif
	);
	t_empty_field();
#endif
	if (failures) {
		printf("test_sd_cmd: %d FAILED\n", failures);
		return 1;
	}
	printf("test_sd_cmd: all passed\n");
	return 0;
}
