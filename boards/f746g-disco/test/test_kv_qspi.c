/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_kv_qspi.c
 * @brief   Host test of this board's side of the key-value store (issue #214):
 *          port/flashdb/fal_qspi.c compiled unchanged, under the real shared
 *          svc/kv/kv.c, the real FlashDB and FAL, and the real blob busy flag
 *          (src/blob.c), on a 16 MB array for the QSPI NOR.
 *
 * What it holds:
 *
 *   - every QSPI access the store makes happens with the blob busy flag held
 *     and kv's record naming the calling thread -- reads included (FlashDB
 *     writes while it reads, so on this board the whole store is under the
 *     flag);
 *   - after every entry point, on every way out (success, each failure, busy),
 *     the flag, kv's record and kv's lock are all free again, and a busy
 *     answer touched nothing;
 *   - an entry point waits up to its deadline for a flag somebody else holds,
 *     then answers busy; one that gets it within the deadline proceeds;
 *   - a re-entry from inside the store (a walk's callback calling back in) is
 *     refused before anything is taken, and the outer operation's hold is
 *     intact afterwards;
 *   - FAL's write and erase refuse unless kv's record names the calling
 *     thread: no hold at all, the flag held by somebody else, or kv's hold on
 *     another thread;
 *   - a release by a thread that does not hold, or with nothing held, gives
 *     back nothing (in particular not somebody else's flag);
 *   - the FAL device is the first megabyte: nothing at or past 0x100000 is
 *     read, programmed or erased, a range that reaches past it or is not whole
 *     4 KB subsectors is refused, and a 64 KB erase is issued only on a 64 KB
 *     boundary with a whole sector left;
 *   - no program ever targets a byte that is not 0xFF.
 *
 * The thread is a pointer the test switches; the clock moves only when
 * something sleeps or waits (kv_shim/tx_api.h).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fal.h>
#include <flashdb.h>

#include "blob.h"
#include "fal_cfg.h"
#include "fal_qspi.h"
#include "kv.h"
#include "kv_board.h"
#include "qspi_flash.h"
#include "tx_api.h"
#include "log.h"

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                      \
			fprintf(stderr, "test_kv_qspi: %s:%d: %s\n", __FILE__,          \
			        __LINE__, #cond);                                       \
			failures++;                                                     \
		}                                                                   \
	} while (0)

/* ------------------------------------------------------------------ *
 *  ThreadX stand-in (kv_shim/tx_api.h)
 * ------------------------------------------------------------------ */

static TX_THREAD thr_a = { "A" }, thr_b = { "B" };
static TX_THREAD *cur = &thr_a;
static ULONG now;
static unsigned long sleeps, deadlocks;
static void (*on_sleep)(void);
static int mutex_create_fails;
static TX_MUTEX *kv_mutex;          /* the one fal_qspi.c created */

UINT tx_mutex_create(TX_MUTEX *m, CHAR *name, UINT inherit)
{
	(void)inherit;
	if (mutex_create_fails)
		return TX_MUTEX_ERROR;
	memset(m, 0, sizeof *m);
	m->name = name;
	m->created = 1;
	kv_mutex = m;
	return TX_SUCCESS;
}

UINT tx_mutex_get(TX_MUTEX *m, ULONG wait)
{
	if (!m->created)
		return TX_MUTEX_ERROR;
	if (m->owner == NULL || m->owner == cur) {
		m->owner = cur;
		m->count++;
		return TX_SUCCESS;
	}
	if (wait == TX_NO_WAIT)
		return TX_NOT_AVAILABLE;
	if (wait == TX_WAIT_FOREVER) {
		deadlocks++;              /* one thread cannot wait for another here */
		return TX_NOT_AVAILABLE;
	}
	now += wait;                  /* the budget is spent, nobody gave it back */
	return TX_NOT_AVAILABLE;
}

UINT tx_mutex_put(TX_MUTEX *m)
{
	if (m->owner != cur)
		return TX_NOT_OWNED;
	if (--m->count == 0u)
		m->owner = NULL;
	return TX_SUCCESS;
}

TX_THREAD *tx_thread_identify(void)
{
	return cur;
}

UINT tx_thread_sleep(ULONG ticks)
{
	now += ticks;
	sleeps++;
	if (on_sleep != NULL)
		on_sleep();
	return TX_SUCCESS;
}

ULONG tx_time_get(void)
{
	return now;
}

/* ------------------------------------------------------------------ *
 *  Log stand-in (kv_shim/log.h)
 * ------------------------------------------------------------------ */

#define LINES 64
static char lines[LINES][160];
static unsigned nlines;

static void keep(const char *s)
{
	if (nlines < LINES)
		snprintf(lines[nlines++], sizeof lines[0], "%s", s);
}

void log_vwrite(unsigned level, const char *tag, const char *fmt, va_list ap)
{
	char b[160];

	(void)level; (void)tag;
	vsnprintf(b, sizeof b, fmt, ap);
	keep(b);
}

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vwrite(level, tag, fmt, ap);
	va_end(ap);
}

void log_write_bytes(unsigned level, const char *tag, const char *prefix,
                     const char *s, size_t len, const char *more)
{
	char b[160];

	(void)level; (void)tag; (void)more;
	snprintf(b, sizeof b, "%s%.*s", prefix ? prefix : "", (int)len, s);
	keep(b);
}

void log_line_init(struct log_line *l)
{
	l->used = 0u;
	l->esc = 0u;
}

void log_line_flush(struct log_line *l, const char *tag)
{
	(void)tag;
	if (l->used == 0u)
		return;
	l->buf[l->used] = '\0';
	l->used = 0u;
	keep(l->buf);
}

void log_line_feed(struct log_line *l, const char *tag, const void *data,
                   size_t len)
{
	const char *p = data;
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] == '\n' || p[i] == '\r') {
			log_line_flush(l, tag);
			continue;
		}
		if (l->used >= LOG_LINE_ASM_MAX - 1u)
			log_line_flush(l, tag);
		l->buf[l->used++] = p[i];
	}
}

static int logged(const char *needle)
{
	unsigned i;

	for (i = 0; i < nlines; i++)
		if (strstr(lines[i], needle) != NULL)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ *
 *  The QSPI NOR (blob.c's PRIMASK shim and the driver)
 * ------------------------------------------------------------------ */

uint32_t blob_test_primask;
unsigned blob_test_masked_sections;

static uint8_t nor[QSPI_FLASH_SIZE];
static int nor_up = 1;

#define MAX_ERASES 512
static struct { uint32_t addr, len; } erases[MAX_ERASES];
static unsigned n_erases;

static struct {
	unsigned long reads, writes;
	unsigned long unheld;       /* an access without kv's record on this thread */
	unsigned long unflagged;    /* ... or without the blob flag held            */
	unsigned long outside;      /* an access at or past the first megabyte      */
	unsigned long not_blank;    /* a programmed byte that was not 0xFF          */
} fm;
static int write_fail_after = -1;   /* writes that succeed before they fail */
static int erase_fail_after = -1;   /* erases that succeed before they fail */

/* Is the blob busy flag held?  Asked by trying it and giving it straight back
 * when that worked (a public blob read would answer too, but reads the flash
 * when the flag is free). */
static int flag_held(void)
{
	if (blob_busy_acquire() == BLOB_OK) {
		blob_busy_release();
		return 0;
	}
	return 1;
}

static void fm_touch(uint32_t addr, uint32_t len)
{
	if (kv_qspi_holder() != (const void *)cur)
		fm.unheld++;
	if (!flag_held())
		fm.unflagged++;
	if (addr >= KV_FAL_DEV_LEN || len > KV_FAL_DEV_LEN - addr)
		fm.outside++;
}

int qspi_flash_ready(void)
{
	return nor_up;
}

int qspi_flash_read(uint32_t addr, void *buf, uint32_t len)
{
	fm.reads++;
	fm_touch(addr, len);
	if (addr >= QSPI_FLASH_SIZE || len > QSPI_FLASH_SIZE - addr)
		return QSPI_FLASH_ERR_PARAM;
	memcpy(buf, &nor[addr], len);
	return QSPI_FLASH_OK;
}

int qspi_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	uint32_t i;

	fm.writes++;
	fm_touch(addr, len);
	if (write_fail_after == 0)
		return QSPI_FLASH_ERR_FLASH;
	if (write_fail_after > 0)
		write_fail_after--;
	if (addr >= QSPI_FLASH_SIZE || len > QSPI_FLASH_SIZE - addr)
		return QSPI_FLASH_ERR_PARAM;
	for (i = 0; i < len; i++) {
		if (nor[addr + i] != 0xFFu)
			fm.not_blank++;
		nor[addr + i] &= p[i];
	}
	return QSPI_FLASH_OK;
}

static int erase_unit(uint32_t addr, uint32_t unit)
{
	uint32_t at = addr - (addr % unit);

	fm_touch(at, unit);
	if (n_erases < MAX_ERASES) {
		erases[n_erases].addr = at;
		erases[n_erases].len = unit;
	}
	n_erases++;
	if (erase_fail_after == 0)
		return QSPI_FLASH_ERR_FLASH;
	if (erase_fail_after > 0)
		erase_fail_after--;
	memset(&nor[at], 0xFF, unit);
	return QSPI_FLASH_OK;
}

int qspi_flash_erase_sector(uint32_t addr)
{
	return erase_unit(addr, QSPI_FLASH_SECTOR_SIZE);
}

int qspi_flash_erase_subsector(uint32_t addr)
{
	return erase_unit(addr, QSPI_FLASH_SUBSECTOR_SIZE);
}

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */

/* The blob region and the top: a pattern the store must never change. */
#define OUTSIDE_FILL 0xA5u

static void nor_fresh(void)
{
	memset(nor, 0xFF, KV_FAL_DEV_LEN);
	memset(nor + KV_FAL_DEV_LEN, OUTSIDE_FILL, sizeof nor - KV_FAL_DEV_LEN);
}

static int outside_intact(void)
{
	size_t i;

	for (i = KV_FAL_DEV_LEN; i < sizeof nor; i++)
		if (nor[i] != OUTSIDE_FILL)
			return 0;
	return 1;
}

static void reset_counts(void)
{
	memset(&fm, 0, sizeof fm);
	n_erases = 0u;
	nlines = 0u;
	write_fail_after = -1;
	erase_fail_after = -1;
	on_sleep = NULL;
	sleeps = 0u;
	deadlocks = 0u;
	cur = &thr_a;
}

/* Everything the store takes is free again. */
static int all_free(void)
{
	int flag_free;

	if (kv_qspi_holder() != NULL)
		return 0;
	if (kv_mutex != NULL && (kv_mutex->owner != NULL || kv_mutex->count != 0u))
		return 0;
	flag_free = (blob_busy_acquire() == BLOB_OK);
	if (flag_free)
		blob_busy_release();
	return flag_free;
}

static int clean_accesses(void)
{
	return fm.unheld == 0u && fm.unflagged == 0u && fm.outside == 0u &&
	       fm.not_blank == 0u && deadlocks == 0u;
}

static const struct fal_flash_dev *dev(void)
{
	return fal_flash_device_find(QSPI_FAL_DEV_NAME);
}

static const struct fal_partition *part(void)
{
	return fal_partition_find(KV_PART_NAME);
}

static void restart_store(void)
{
	memset(&kv_board_state, 0, sizeof kv_board_state);
}

/* ------------------------------------------------------------------ *
 *  Tests
 * ------------------------------------------------------------------ */

/* Before kv_qspi_init(): no lock, so nothing can be had, and nothing is touched. */
static void t_no_lock(void)
{
	struct kv_value v;

	reset_counts();
	nor_fresh();
	restart_store();
	CHECK(kv_board_facts.hold_try() == KV_ERR_STATE);
	CHECK(kv_board_facts.hold_wait() != 0);
	CHECK(kv_init() == KV_ERR_STATE && kv_state() == KV_STATE_FAILED);
	CHECK(kv_get("a.b", &v) == KV_ERR_STATE);
	CHECK(kv_format() == KV_ERR_STATE);
	CHECK(fm.reads == 0u && fm.writes == 0u && n_erases == 0u);
	CHECK(all_free());

	/* ...and a lock that cannot be created leaves it that way. */
	mutex_create_fails = 1;
	CHECK(kv_qspi_init() != 0);
	CHECK(kv_board_facts.hold_try() == KV_ERR_STATE);
	mutex_create_fails = 0;
	CHECK(kv_qspi_init() == 0 && kv_mutex != NULL);
	CHECK(kv_qspi_init() == 0);          /* once */
}

/* A blank partition: created, under the flag throughout, inside the megabyte. */
static void t_init_blank(void)
{
	unsigned i;
	int ok = 1;

	reset_counts();
	nor_fresh();
	restart_store();
	CHECK(kv_init() == KV_OK && kv_state() == KV_STATE_READY);
	CHECK(logged("blank partition -- creating the store"));
	CHECK(logged("store created: 1024 KB partition, 4096 B sectors"));
	CHECK(clean_accesses() && fm.reads > 0u && fm.writes > 0u && n_erases > 0u);
	for (i = 0; i < n_erases && i < MAX_ERASES; i++)
		if (erases[i].len != QSPI_FLASH_SUBSECTOR_SIZE)
			ok = 0;
	CHECK(ok);                           /* FlashDB erases 4 KB sectors */
	CHECK(outside_intact());
	CHECK(all_free());
}

static void t_data_path(void)
{
	struct kv_value v;
	uint32_t u = 0u;
	char s[16];

	reset_counts();
	CHECK(kv_set("test.a", KV_TYPE_STR, "hello", 5u, NULL) == KV_OK);
	CHECK(all_free());
	CHECK(kv_get("test.a", &v) == KV_OK && v.type == KV_TYPE_STR &&
	      strcmp((const char *)v.data, "hello") == 0);
	CHECK(all_free());
	u = 2323u;
	CHECK(kv_set("net.shell.port", KV_TYPE_U32, &u, 4u, NULL) == KV_OK);
	u = 0u;
	CHECK(kv_get_u32("net.shell.port", &u) == KV_OK && u == 2323u);
	CHECK(kv_get_str("test.a", s, sizeof s) == KV_OK && strcmp(s, "hello") == 0);
	CHECK(kv_get("no.such", &v) == KV_ERR_NOTFOUND);
	CHECK(all_free());
	CHECK(kv_del("test.a") == KV_OK && kv_get("test.a", &v) == KV_ERR_NOTFOUND);
	CHECK(kv_del("test.a") == KV_ERR_NOTFOUND);
	CHECK(all_free());
	CHECK(clean_accesses() && outside_intact());
}

/* Somebody else holds the flag (a `blob write`, say). */
static unsigned release_after;
static void release_flag_later(void)
{
	if (release_after > 0u && --release_after == 0u)
		blob_busy_release();
}

static void t_flag_busy(void)
{
	struct kv_value v;
	unsigned long w0;
	ULONG t0;

	reset_counts();
	CHECK(blob_busy_acquire() == BLOB_OK);     /* the other holder */

	t0 = now;
	w0 = fm.writes;
	CHECK(kv_get("net.shell.port", &v) == KV_ERR_BUSY);
	CHECK(now - t0 >= 5000u && now - t0 < 5000u + 20u);   /* the deadline */
	CHECK(sleeps > 0u);
	CHECK(kv_set("x.y", KV_TYPE_STR, "z", 1u, NULL) == KV_ERR_BUSY);
	CHECK(kv_del("net.shell.port") == KV_ERR_BUSY);
	CHECK(kv_format() == KV_ERR_BUSY);
	{
		struct kv_info info;

		CHECK(kv_get_info(&info) == KV_ERR_BUSY);
	}
	/* Nothing was touched, kv holds nothing, and the other holder still has
	 * its flag -- which also refuses the public blob reads. */
	CHECK(fm.reads == 0u && fm.writes == w0 && n_erases == 0u);
	CHECK(kv_qspi_holder() == NULL && kv_mutex->owner == NULL);
	CHECK(blob_busy_acquire() == BLOB_ERR_BUSY);
	{
		struct blob_info bi;

		CHECK(blob_stat(0u, &bi, NULL) == BLOB_ERR_BUSY && fm.reads == 0u);
	}

	/* [!] A release with nothing held must not give back the other's flag. */
	kv_board_facts.hold_release();
	CHECK(blob_busy_acquire() == BLOB_ERR_BUSY);
	CHECK(logged("hold given back by a thread that does not hold it"));

	/* FAL's write and erase refuse while the flag is somebody else's. */
	{
		uint8_t b = 0x00u;

		CHECK(fal_partition_write(part(), 0x1000u, &b, 1u) < 0);
		CHECK(fal_partition_erase(part(), 0x1000u, 0x1000u) < 0);
		CHECK(fm.writes == w0 && n_erases == 0u);
		CHECK(logged("FAL write at 0x001000 refused"));
		CHECK(logged("FAL erase at 0x001000 refused"));
	}

	/* The other holder finishes part way through the wait: the entry point
	 * proceeds. */
	release_after = 3u;
	on_sleep = release_flag_later;
	CHECK(kv_get_u32("net.shell.port", &(uint32_t){0}) == KV_OK);
	CHECK(all_free() && clean_accesses());

	/* kv_init()'s hold has no deadline: it waits until the flag comes back. */
	restart_store();
	CHECK(blob_busy_acquire() == BLOB_OK);
	release_after = 1000u;                       /* 10 s at 10 ms a try */
	on_sleep = release_flag_later;
	t0 = now;
	CHECK(kv_init() == KV_OK && now - t0 >= 10000u);
	CHECK(all_free() && clean_accesses());
}

/* kv's hold on another thread: the mutex waits out its budget. */
static void t_other_thread(void)
{
	struct kv_value v;
	uint8_t b = 0x00u;
	unsigned long w0;

	reset_counts();
	cur = &thr_b;
	CHECK(kv_board_facts.hold_try() == KV_OK);
	CHECK(kv_qspi_holder() == (const void *)&thr_b && flag_held());

	cur = &thr_a;
	w0 = fm.writes;
	CHECK(kv_get("net.shell.port", &v) == KV_ERR_BUSY);
	CHECK(kv_qspi_holder() == (const void *)&thr_b && flag_held());
	/* FAL from a thread the record does not name, while kv holds on B. */
	CHECK(fal_partition_write(part(), 0x2000u, &b, 1u) < 0);
	CHECK(fal_partition_erase(part(), 0x2000u, 0x1000u) < 0);
	CHECK(fm.writes == w0 && n_erases == 0u);
	CHECK(kv_board_facts.db_lock() != 0);       /* FlashDB's lock: not A's */
	/* A release from A gives back nothing of B's. */
	kv_board_facts.hold_release();
	CHECK(kv_qspi_holder() == (const void *)&thr_b && flag_held());

	cur = &thr_b;
	CHECK(kv_board_facts.db_lock() == 0);
	kv_board_facts.hold_release();
	cur = &thr_a;
	CHECK(all_free() && clean_accesses() && deadlocks == 0u);
}

/* No hold at all: FAL refuses before touching the flash. */
static void t_unheld_fal(void)
{
	uint8_t b = 0x00u;

	reset_counts();
	CHECK(fal_partition_write(part(), 0x3000u, &b, 1u) < 0);
	CHECK(fal_partition_erase(part(), 0x3000u, 0x1000u) < 0);
	CHECK(fal_partition_erase_all(part()) < 0);
	CHECK(fm.writes == 0u && n_erases == 0u);
	CHECK(kv_board_facts.db_lock() != 0);
	CHECK(all_free());
}

/* A walk's callback that calls back into the store. */
static int reentries, reentry_rc_get, reentry_rc_set;
static int reenter_cb(const char *key, const struct kv_value *v, void *arg)
{
	struct kv_value w;

	(void)key; (void)v; (void)arg;
	reentries++;
	reentry_rc_get = kv_get("net.shell.port", &w);
	reentry_rc_set = kv_set("r.e", KV_TYPE_STR, "x", 1u, NULL);
	/* The outer walk's hold is still intact. */
	CHECK(kv_qspi_holder() == (const void *)&thr_a && flag_held());
	CHECK(kv_mutex->owner == &thr_a && kv_mutex->count == 1u);
	return 0;
}

static void t_reentry(void)
{
	struct kv_value v;
	unsigned long w0;
	ULONG t0;

	reset_counts();
	reentries = 0;
	w0 = fm.writes;
	t0 = now;
	CHECK(kv_foreach(NULL, reenter_cb, NULL) == KV_OK);
	CHECK(reentries > 0);
	CHECK(reentry_rc_get == KV_ERR_STATE && reentry_rc_set == KV_ERR_STATE);
	CHECK(now == t0);                        /* refused at once, no waiting */
	CHECK(fm.writes == w0);                  /* the inner set wrote nothing */
	CHECK(kv_get("r.e", &v) == KV_ERR_NOTFOUND);
	CHECK(all_free() && clean_accesses());
}

/* The ways out of kv_format(), kv_init() and a failed write. */
static void t_failure_exits(void)
{
	uint32_t u = 7u;

	/* A FlashDB write that fails: the error, and everything given back. */
	reset_counts();
	write_fail_after = 0;
	CHECK(kv_set("f.w", KV_TYPE_U32, &u, 4u, NULL) == KV_ERR_IO);
	CHECK(all_free());

	/* kv_format(): its own wipe fails. */
	reset_counts();
	erase_fail_after = 0;
	CHECK(kv_format() == KV_ERR_IO && kv_state() == KV_STATE_FAILED);
	CHECK(logged("format: erase failed"));
	CHECK(all_free());

	/*
	 * kv_format(): the wipe works and FlashDB's own sector erases then fail.
	 * [!] FlashDB does not report it: _fdb_kv_load() discards what formatting
	 * its sectors returned, so fdb_kvdb_init() answers success (and the store
	 * reads READY over sectors that were never erased).  Which of
	 * kv_format()'s two answers comes back is FlashDB's; what is this board's
	 * is that the hold is given back either way.
	 */
	reset_counts();
	erase_fail_after = (int)(KV_FAL_DEV_LEN / QSPI_FLASH_SECTOR_SIZE);
	{
		int rc = kv_format();

		CHECK(rc == KV_OK || rc == KV_ERR_IO);
	}
	CHECK(n_erases > KV_FAL_DEV_LEN / QSPI_FLASH_SECTOR_SIZE);   /* it got there */
	CHECK(all_free());

	/* kv_format(): the way back. */
	reset_counts();
	CHECK(kv_format() == KV_OK && kv_state() == KV_STATE_READY);
	CHECK(logged("store formatted"));
	CHECK(all_free() && clean_accesses() && outside_intact());

	/* kv_init() on a partition that is neither a database nor blank (the
	 * remains of the old LevelX volume, say): reported, left alone. */
	reset_counts();
	restart_store();
	memset(nor, 0x00, 64u);
	CHECK(kv_init() == KV_ERR_STATE && kv_state() == KV_STATE_CORRUPT);
	CHECK(logged("partition is neither a valid database nor blank"));
	CHECK(fm.writes == 0u && n_erases == 0u);
	CHECK(all_free());

	/* kv_init() on a blank partition whose format's erases fail: the same
	 * FlashDB answer as above; the hold is given back either way. */
	reset_counts();
	restart_store();
	nor_fresh();
	erase_fail_after = 0;
	(void)kv_init();
	CHECK(logged("blank partition -- creating the store") && n_erases > 0u);
	CHECK(all_free());

	/* kv_init() with the QSPI down: refused before any hold. */
	reset_counts();
	restart_store();
	nor_up = 0;
	CHECK(kv_init() == KV_ERR_STATE && kv_state() == KV_STATE_DOWN);
	CHECK(kv_format() == KV_ERR_STATE);
	nor_up = 1;
	CHECK(fm.reads == 0u && all_free());

	/* Back to a working store for what follows. */
	reset_counts();
	restart_store();
	nor_fresh();
	CHECK(kv_init() == KV_OK);
	CHECK(all_free() && clean_accesses());
}

/* The FAL device's own bounds, under a hold, called directly. */
static void t_bounds(void)
{
	const struct fal_flash_dev *d = dev();
	uint8_t b[4] = { 0x00u, 0x00u, 0x00u, 0x00u };
	unsigned i;
	int ok;

	reset_counts();
	CHECK(d != NULL && d->len == KV_FAL_DEV_LEN && d->blk_size == 4096u);
	CHECK(kv_board_facts.hold_try() == KV_OK);

	/* 64 KB only on a 64 KB boundary with a whole sector left. */
	n_erases = 0u;
	CHECK(d->ops.erase(0x10000, 0x10000) == 0x10000);
	CHECK(n_erases == 1u && erases[0].addr == 0x10000u &&
	      erases[0].len == QSPI_FLASH_SECTOR_SIZE);
	n_erases = 0u;
	CHECK(d->ops.erase(0x1000, 0x10000) == 0x10000);
	ok = (n_erases == 16u);
	for (i = 0; i < n_erases && i < MAX_ERASES; i++)
		if (erases[i].len != QSPI_FLASH_SUBSECTOR_SIZE ||
		    erases[i].addr != 0x1000u + i * 0x1000u)
			ok = 0;
	CHECK(ok);
	n_erases = 0u;
	CHECK(d->ops.erase(0x20000, 0x11000) == 0x11000);   /* one 64 KB + one 4 KB */
	CHECK(n_erases == 2u && erases[0].len == QSPI_FLASH_SECTOR_SIZE &&
	      erases[1].addr == 0x30000u && erases[1].len == QSPI_FLASH_SUBSECTOR_SIZE);
	n_erases = 0u;
	CHECK(d->ops.erase(0, KV_FAL_DEV_LEN) == (int)KV_FAL_DEV_LEN);
	ok = (n_erases == 16u);
	for (i = 0; i < n_erases && i < MAX_ERASES; i++)
		if (erases[i].len != QSPI_FLASH_SECTOR_SIZE)
			ok = 0;
	CHECK(ok);

	/* Refused, and nothing erased. */
	n_erases = 0u;
	CHECK(d->ops.erase(0xF0000, 0x20000) < 0);          /* past the megabyte */
	CHECK(d->ops.erase(0x100000, 0x1000) < 0);
	CHECK(d->ops.erase(0x800, 0x1000) < 0);             /* not on 4 KB */
	CHECK(d->ops.erase(0x1000, 0x800) < 0);
	CHECK(d->ops.erase(0x1000, 0) < 0);
	CHECK(d->ops.erase(-0x1000, 0x1000) < 0);
	CHECK(n_erases == 0u);

	fm.writes = 0u;
	CHECK(d->ops.write(0xFFFFF, b, 2) < 0);             /* one byte past */
	CHECK(d->ops.write(0x100000, b, 1) < 0);
	CHECK(d->ops.write(-1, b, 1) < 0);
	CHECK(fm.writes == 0u);
	CHECK(d->ops.write(0xFFFFF, b, 1) == 1 && fm.writes == 1u);
	CHECK(d->ops.read(0x100000, b, 1) < 0);
	CHECK(d->ops.read(0xFFFFC, b, 8) < 0);
	CHECK(d->ops.read(0xFFFFC, b, 4) == 4);

	kv_board_facts.hold_release();
	CHECK(all_free() && clean_accesses() && outside_intact());

	/* The partition and the device agree. */
	CHECK(part() != NULL && part()->offset == 0 && part()->len == KV_FAL_DEV_LEN);
}

int main(void)
{
	t_no_lock();
	t_init_blank();
	t_data_path();
	t_flag_busy();
	t_other_thread();
	t_unheld_fal();
	t_reentry();
	t_failure_exits();
	t_bounds();
	if (failures != 0) {
		fprintf(stderr, "test_kv_qspi: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_kv_qspi: OK\n");
	return 0;
}
