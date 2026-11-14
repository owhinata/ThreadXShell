/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host fakes under the shared microSD mount glue and FileX media driver
 * (issue #233); see sd_fake.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sd_fake.h"
#include "sd_cmd_board.h"
#include "log.h"

struct sd_fake sd_fake;

/* The board's object (boards/<board>/src/sd_fs_state.c on hardware). */
struct sd_fs_state sd_fs_board_state;

static char calls[16384];
static char logbuf[4096];

static void rec(const char *fmt, ...)
{
	size_t n = strlen(calls);
	va_list ap;

	if (n + 16 >= sizeof calls)
		return;
	va_start(ap, fmt);
	vsnprintf(calls + n, sizeof calls - n - 8, fmt, ap);
	va_end(ap);
	n = strlen(calls);
	snprintf(calls + n, sizeof calls - n, "[%c%c]|",
	         sd_fake.lock_held ? 'L' : '-',
	         sd_fs_board_state.busy ? 'B' : '-');
}

const char *sd_fake_calls(void) { return calls; }
const char *sd_fake_log(void) { return logbuf; }

int sd_fake_at(const char *what)
{
	const char *p = strstr(calls, what);

	return p ? (int)(p - calls) : -1;
}

int sd_fake_count(const char *what)
{
	int n = 0;
	const char *p = calls;

	while ((p = strstr(p, what)) != NULL) {
		n++;
		p += strlen(what);
	}
	return n;
}

int sd_fake_touched_anything(void)
{
	return calls[0] != '\0' || sd_fake.lock_gets != 0 || sd_fake.dir_gets != 0;
}

void sd_fake_reset(void)
{
	memset(&sd_fs_board_state, 0, sizeof sd_fs_board_state);
	memset(&sd_fake, 0, sizeof sd_fake);
	calls[0] = '\0';
	logbuf[0] = '\0';
	sd_fake.present = 1;
	sd_fake.probe_rc = SD_OK;
	sd_fake.status_rc = SD_OK;
	sd_fake.read_rc = SD_OK;
	sd_fake.write_rc = SD_OK;
	sd_fake.block_count = 62333952u;       /* a 32 GB card */
	sd_fake.fat32 = FX_TRUE;
}

uint8_t *sd_fake_block(uint32_t lba)
{
	int i;

	for (i = 0; i < sd_fake.nblk; i++)
		if (sd_fake.blk_lba[i] == lba)
			return sd_fake.blk[i];
	if (sd_fake.nblk == SD_FAKE_BLOCKS)
		return NULL;
	sd_fake.blk_lba[sd_fake.nblk] = lba;
	memset(sd_fake.blk[sd_fake.nblk], 0, SD_BLOCK_SIZE);
	return sd_fake.blk[sd_fake.nblk++];
}

static void put_le16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	put_le16(p, v);
	put_le16(p + 2, v >> 16);
}

static void make_vbr(uint8_t *b, uint32_t total)
{
	memset(b, 0, SD_BLOCK_SIZE);
	b[0] = 0xEBu;
	b[1] = 0x58u;
	b[2] = 0x90u;
	put_le16(b + 11, SD_BLOCK_SIZE);   /* bytes per sector */
	b[13] = 64u;                       /* sectors per cluster */
	put_le16(b + 14, 32u);             /* reserved sectors */
	b[16] = 2u;                        /* FATs */
	put_le32(b + 32, total);           /* total sectors (32) */
	put_le32(b + 36, 7600u);           /* FAT size (32) */
	b[510] = 0x55u;
	b[511] = 0xAAu;
}

void sd_fake_put_vbr(uint32_t lba, uint32_t total)
{
	make_vbr(sd_fake_block(lba), total);
}

void sd_fake_put_mbr(uint8_t type, uint32_t first, uint32_t size)
{
	uint8_t *b = sd_fake_block(0);

	memset(b, 0, SD_BLOCK_SIZE);
	b[446 + 4] = type;
	put_le32(b + 446 + 8, first);
	put_le32(b + 446 + 12, size);
	b[510] = 0x55u;
	b[511] = 0xAAu;
}

/* ---- the card ------------------------------------------------------------- */

static struct sd_card_info info_snap;

__attribute__((unused)) static int f_probe(void)
{
	rec("probe");
	if (!sd_fake.present) {
		sd_fake.probed = 0;
		return SD_ERR_NO_CARD;
	}
	sd_fake.probed = (sd_fake.probe_rc == SD_OK);
	if (sd_fake.probed) {
		memset(&info_snap, 0, sizeof info_snap);
		info_snap.type = 1u;
		info_snap.version = 1u;
		info_snap.block_count = sd_fake.block_count;
		info_snap.block_size = SD_BLOCK_SIZE;
		info_snap.bus_width = 4u;
		info_snap.capacity_bytes = (uint64_t)sd_fake.block_count * SD_BLOCK_SIZE;
	}
	return sd_fake.probe_rc;
}

__attribute__((unused)) static int f_status(void)
{
	rec("status");
	if (!sd_fake.probed)
		return SD_ERR_STATE;
	if (!sd_fake.present)
		return SD_ERR_NO_CARD;
	return sd_fake.status_rc;
}

__attribute__((unused)) static const struct sd_card_info *f_info(void)
{
	rec("info");
	return &info_snap;
}

__attribute__((unused)) static int f_read(uint32_t lba, void *buf, uint32_t count)
{
	uint32_t i;
	int j;

	rec("read@%lu", (unsigned long)lba);
	sd_fake.last_read_lba = lba;
	sd_fake.last_read_count = count;
	if (sd_fake.read_rc != SD_OK)
		return sd_fake.read_rc;
	for (i = 0; i < count; i++) {
		uint8_t *dst = (uint8_t *)buf + (size_t)i * SD_BLOCK_SIZE;

		memset(dst, 0, SD_BLOCK_SIZE);
		for (j = 0; j < sd_fake.nblk; j++)
			if (sd_fake.blk_lba[j] == lba + i)
				memcpy(dst, sd_fake.blk[j], SD_BLOCK_SIZE);
	}
	return SD_OK;
}

__attribute__((unused)) static int f_write(uint32_t lba, const void *buf,
                                           uint32_t count)
{
	uint8_t *dst;

	rec("write@%lu", (unsigned long)lba);
	sd_fake.last_write_lba = lba;
	sd_fake.last_write_count = count;
	if (sd_fake.write_rc != SD_OK)
		return sd_fake.write_rc;
	if (count == 1u && (dst = sd_fake_block(lba)) != NULL)
		memcpy(dst, buf, SD_BLOCK_SIZE);
	return SD_OK;
}

__attribute__((unused)) static int f_release(void)
{
	rec("release");
	sd_fake.probed = 0;
	return SD_OK;
}

#if defined(SD_FAKE_SHAPE_NOPROBE)
#define F_PROBE NULL
#else
#define F_PROBE f_probe
#endif
#if defined(SD_FAKE_SHAPE_NOSTATUS)
#define F_STATUS NULL
#else
#define F_STATUS f_status
#endif
#if defined(SD_FAKE_SHAPE_NOINFO)
#define F_INFO NULL
#else
#define F_INFO f_info
#endif
#if defined(SD_FAKE_SHAPE_NOREAD)
#define F_READ NULL
#else
#define F_READ f_read
#endif
#if defined(SD_FAKE_SHAPE_NOWRITE)
#define F_WRITE NULL
#else
#define F_WRITE f_write
#endif
#if defined(SD_FAKE_SHAPE_NORELEASE)
#define F_RELEASE NULL
#else
#define F_RELEASE f_release
#endif

const struct sd_card_ops sd_card_board_ops = {
	.probe = F_PROBE, .status = F_STATUS, .info = F_INFO,
	.read_blocks = F_READ, .write_blocks = F_WRITE, .release = F_RELEASE,
};

/* ---- ThreadX -------------------------------------------------------------- */

static int busy_at_get;

UINT tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit)
{
	(void)mutex_ptr;
	(void)inherit;
	rec("mutex_create:%s", name_ptr);
	sd_fake.mutexes_created++;
	return TX_SUCCESS;
}

UINT tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
	(void)wait_option;
	if (mutex_ptr == &sd_fs_board_state.mount_lock) {
		if (sd_fake.lock_held)
			sd_fake.lock_nested++;
		sd_fake.lock_held = 1;
		sd_fake.lock_gets++;
		busy_at_get = sd_fs_board_state.busy;
	} else if (mutex_ptr == &sd_fs_board_state.dir_mutex) {
		sd_fake.dir_gets++;
	}
	return TX_SUCCESS;
}

UINT tx_mutex_put(TX_MUTEX *mutex_ptr)
{
	if (mutex_ptr == &sd_fs_board_state.mount_lock) {
		sd_fake.lock_held = 0;
		sd_fake.lock_puts++;
		if (busy_at_get && !sd_fs_board_state.busy) {
			rec("slot_released");
			sd_fs_board_state.media.fx_media_32_bit_FAT = FX_FALSE;
		}
	} else if (mutex_ptr == &sd_fs_board_state.dir_mutex) {
		sd_fake.dir_puts++;
	}
	return TX_SUCCESS;
}

/* ---- the log -------------------------------------------------------------- */

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)level;
	if (n + 16 >= sizeof logbuf)
		return;
	snprintf(logbuf + n, sizeof logbuf - n, "%s: ", tag);
	n = strlen(logbuf);
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n - 2, fmt, ap);
	va_end(ap);
	strcat(logbuf, "\n");
}

/* ---- FileX ---------------------------------------------------------------- */

VOID fx_system_initialize(VOID)
{
	rec("fx_init");
	sd_fake.fx_inits++;
}

static UCHAR boot_buf[SD_BLOCK_SIZE];

static UINT drive(FX_MEDIA *m, VOID (*driver)(FX_MEDIA *), UINT request,
                  UCHAR *buf, ULONG sector, ULONG count)
{
	m->fx_media_driver_request = request;
	m->fx_media_driver_buffer = buf;
	m->fx_media_driver_logical_sector = sector;
	m->fx_media_driver_sectors = count;
	m->fx_media_driver_status = 0xEEu;   /* the driver must set it */
	driver(m);
	return m->fx_media_driver_status;
}

UINT fx_media_open(FX_MEDIA *media_ptr, CHAR *media_name,
                   VOID (*media_driver)(FX_MEDIA *), VOID *driver_info_ptr,
                   VOID *memory_ptr, ULONG memory_size)
{
	int ok_args = media_ptr == &sd_fs_board_state.media &&
	              strcmp(media_name, "sd") == 0 && driver_info_ptr == FX_NULL &&
	              memory_ptr == sd_fs_board_state.cache &&
	              memory_size == sizeof sd_fs_board_state.cache;

	rec("media_open%s", ok_args ? "" : "(BAD ARGS)");
	if (sd_fake.opened)
		sd_fake.double_open++;
	if (drive(media_ptr, media_driver, FX_DRIVER_INIT, FX_NULL, 0, 0) !=
	    FX_SUCCESS)
		return FX_IO_ERROR;     /* FileX: a failed INIT, nothing kept open */
	if (drive(media_ptr, media_driver, FX_DRIVER_BOOT_READ, boot_buf, 0, 1) !=
	    FX_SUCCESS)
		return FX_BOOT_ERROR;
	if (sd_fake.open_rc != FX_SUCCESS)
		return sd_fake.open_rc;
	media_ptr->fx_media_32_bit_FAT = sd_fake.fat32;
	media_ptr->fx_media_bytes_per_sector = SD_BLOCK_SIZE;
	sd_fake.opened = 1;
	return FX_SUCCESS;
}

UINT fx_media_close(FX_MEDIA *media_ptr)
{
	(void)media_ptr;
	rec("media_close");
	sd_fake.opened = 0;
	return FX_SUCCESS;
}

UINT fx_media_abort(FX_MEDIA *media_ptr)
{
	(void)media_ptr;
	rec("media_abort");
	sd_fake.opened = 0;
	return FX_SUCCESS;
}

__attribute__((weak))
UINT fx_media_format(FX_MEDIA *media_ptr, VOID (*driver)(FX_MEDIA *media),
                     VOID *driver_info_ptr, UCHAR *memory_ptr, UINT memory_size,
                     CHAR *volume_name, UINT number_of_fats,
                     UINT directory_entries, UINT hidden_sectors,
                     ULONG total_sectors, UINT bytes_per_sector,
                     UINT sectors_per_cluster, UINT heads,
                     UINT sectors_per_track)
{
	static UCHAR vbr[SD_BLOCK_SIZE];

	(void)driver_info_ptr; (void)memory_ptr; (void)memory_size;
	(void)volume_name; (void)number_of_fats; (void)directory_entries;
	(void)hidden_sectors; (void)bytes_per_sector; (void)sectors_per_cluster;
	(void)heads; (void)sectors_per_track;
	rec("media_format");
	if (drive(media_ptr, driver, FX_DRIVER_INIT, FX_NULL, 0, 0) != FX_SUCCESS)
		return FX_IO_ERROR;
	make_vbr(vbr, (uint32_t)total_sectors);
	media_ptr->fx_media_bytes_per_sector = SD_BLOCK_SIZE;
	if (drive(media_ptr, driver, FX_DRIVER_BOOT_WRITE, vbr, 0, 1) != FX_SUCCESS)
		return FX_IO_ERROR;
	(void)drive(media_ptr, driver, FX_DRIVER_UNINIT, FX_NULL, 0, 0);
	return sd_fake.format_rc;
}

/* What the filesystem bodies call once they hold a media: recorded, and a
 * plain success (an empty directory). */
#define FX_WEAK __attribute__((weak))

FX_WEAK UINT fx_media_flush(FX_MEDIA *m) { (void)m; rec("fx_flush"); return FX_SUCCESS; }
FX_WEAK UINT fx_media_extended_space_available(FX_MEDIA *m, ULONG64 *a)
{
	(void)m;
	rec("fx_space");
	*a = 0;
	return FX_SUCCESS;
}
FX_WEAK UINT fx_directory_default_set(FX_MEDIA *m, CHAR *p)
{
	(void)m; (void)p;
	rec("fx_dir_set");
	return FX_SUCCESS;
}
FX_WEAK UINT fx_directory_first_full_entry_find(FX_MEDIA *m, CHAR *n, UINT *a,
                                                ULONG *s, UINT *y, UINT *mo,
                                                UINT *d, UINT *h, UINT *mi,
                                                UINT *se)
{
	(void)m; (void)n; (void)a; (void)s; (void)y; (void)mo; (void)d; (void)h;
	(void)mi; (void)se;
	rec("fx_dir_first");
	return FX_NO_MORE_ENTRIES;
}
FX_WEAK UINT fx_directory_next_full_entry_find(FX_MEDIA *m, CHAR *n, UINT *a,
                                               ULONG *s, UINT *y, UINT *mo,
                                               UINT *d, UINT *h, UINT *mi,
                                               UINT *se)
{
	(void)m; (void)n; (void)a; (void)s; (void)y; (void)mo; (void)d; (void)h;
	(void)mi; (void)se;
	rec("fx_dir_next");
	return FX_NO_MORE_ENTRIES;
}
FX_WEAK UINT fx_directory_create(FX_MEDIA *m, CHAR *n) { (void)m; (void)n; rec("fx_mkdir"); return FX_SUCCESS; }
FX_WEAK UINT fx_directory_delete(FX_MEDIA *m, CHAR *n) { (void)m; (void)n; rec("fx_rmdir"); return FX_SUCCESS; }
FX_WEAK UINT fx_file_create(FX_MEDIA *m, CHAR *n) { (void)m; (void)n; rec("fx_create"); return FX_SUCCESS; }
FX_WEAK UINT fx_file_delete(FX_MEDIA *m, CHAR *n) { (void)m; (void)n; rec("fx_delete"); return FX_SUCCESS; }
FX_WEAK UINT fx_file_open(FX_MEDIA *m, FX_FILE *f, CHAR *n, UINT t)
{
	(void)m; (void)f; (void)n; (void)t;
	rec("fx_fopen");
	return FX_SUCCESS;
}
FX_WEAK UINT fx_file_read(FX_FILE *f, VOID *b, ULONG r, ULONG *got)
{
	(void)f; (void)b; (void)r;
	rec("fx_fread");
	*got = 0;
	return FX_END_OF_FILE;
}
FX_WEAK UINT fx_file_truncate(FX_FILE *f, ULONG s) { (void)f; (void)s; rec("fx_trunc"); return FX_SUCCESS; }
FX_WEAK UINT fx_file_write(FX_FILE *f, VOID *b, ULONG s) { (void)f; (void)b; (void)s; rec("fx_fwrite"); return FX_SUCCESS; }
FX_WEAK UINT fx_file_close(FX_FILE *f) { (void)f; rec("fx_fclose"); return FX_SUCCESS; }
