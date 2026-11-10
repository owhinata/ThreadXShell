/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_kv_store.c
 * @brief   Host test of the shared key-value store (svc/kv/kv.c, issue #214)
 *          running the real FlashDB and FAL under the real svc/kv/fdb_cfg.h,
 *          on a flash model in RAM.
 *
 * What it holds, over every operation it runs (init on a blank partition, the
 * data path, a few thousand rewrites that make FlashDB recycle its sectors,
 * reboots, a corrupt partition, `kv format`):
 *
 *   - no program ever targets a byte that is not 0xFF (the reason fdb_cfg.h sets
 *     the write granularity to 8 bits; the model counts every such byte);
 *   - no read, program or erase leaves the store's partition, whose neighbours
 *     must keep their bytes;
 *   - every device access happens with the board's hold or FlashDB's lock held,
 *     and every outer hold taken is given back exactly once;
 *   - the board's hold answers (busy, cannot be had, a value it should not
 *     return) become the store's, with the device untouched;
 *   - a failed lock inside FlashDB latches the store offline, and `kv format`
 *     is the way back;
 *   - a partition that is neither a database nor blank is left alone, and the
 *     lines reported through the board's report function are the ones the
 *     store wrote to wio-lite-ai's log before it was shared.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fal.h>
#include <flashdb.h>

#include "kv.h"
#include "kv_board.h"

_Static_assert(FDB_WRITE_GRAN == 8, "the shared fdb_cfg.h programs bytes");

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                      \
			fprintf(stderr, "test_kv_store: %s:%d: %s\n", __FILE__,         \
			        __LINE__, #cond);                                       \
			failures++;                                                     \
		}                                                                   \
	} while (0)

/* ------------------------------------------------------------------ *
 *  The flash model
 * ------------------------------------------------------------------ */

static uint8_t flash[KV_TEST_DEV_SIZE];

static struct {
	unsigned long reads, writes, erases;  /* calls                              */
	unsigned long bytes_programmed;
	unsigned long not_blank;              /* bytes programmed that were not 0xFF */
	unsigned long outside;                /* accesses not inside the partition   */
	unsigned long unheld;                 /* accesses with nothing held          */
	unsigned long misaligned;             /* erases not on sector boundaries     */
	long first_not_blank;                 /* device offset, -1 = none            */
} fm;

static int outer_held;     /* the board's hold, as the store took it          */
static int inner_held;     /* FlashDB's lock                                  */
static unsigned long outer_unbalanced, inner_unbalanced;

static void fm_reset_counts(void)
{
	memset(&fm, 0, sizeof fm);
	fm.first_not_blank = -1;
}

static unsigned long fm_accesses(void)
{
	return fm.reads + fm.writes + fm.erases;
}

static void fm_touch(long off, size_t n)
{
	if (off < (long)KV_TEST_PART_OFF ||
	    (unsigned long)off + n > (unsigned long)KV_TEST_PART_OFF + KV_TEST_PART_LEN)
		fm.outside++;
	if (outer_held == 0 && inner_held == 0)
		fm.unheld++;
}

static int fm_init(void)
{
	return 0;
}

static int fm_read(long off, uint8_t *buf, size_t n)
{
	fm.reads++;
	fm_touch(off, n);
	if (off < 0 || (unsigned long)off + n > sizeof flash)
		return -1;
	memcpy(buf, flash + off, n);
	return (int)n;
}

/* NOR physics: a program only clears bits.  Any target byte that does not read
 * 0xFF is counted, whatever the data. */
static int fm_write(long off, const uint8_t *buf, size_t n)
{
	size_t i;

	fm.writes++;
	fm_touch(off, n);
	if (off < 0 || (unsigned long)off + n > sizeof flash)
		return -1;
	for (i = 0; i < n; i++) {
		if (flash[off + (long)i] != 0xFFu) {
			if (fm.first_not_blank < 0)
				fm.first_not_blank = off + (long)i;
			fm.not_blank++;
		}
		flash[off + (long)i] &= buf[i];
	}
	fm.bytes_programmed += n;
	return (int)n;
}

static int fm_erase(long off, size_t n)
{
	fm.erases++;
	fm_touch(off, n);
	if (off < 0 || (unsigned long)off + n > sizeof flash)
		return -1;
	if ((unsigned long)off % KV_TEST_SECTOR != 0u || n % KV_TEST_SECTOR != 0u)
		fm.misaligned++;
	memset(flash + off, 0xFF, n);
	return (int)n;
}

const struct fal_flash_dev kv_test_flash = {
	.name = KV_TEST_DEV_NAME,
	.addr = 0,
	.len = KV_TEST_DEV_SIZE,
	.blk_size = KV_TEST_SECTOR,
	.ops = { fm_init, fm_read, fm_write, fm_erase },
	.write_gran = 8,
};

/* The two neighbours carry a pattern no store write could make. */
static void fill_neighbours(void)
{
	size_t i;

	for (i = 0; i < KV_TEST_PART_OFF; i++)
		flash[i] = (uint8_t)(0x5Au ^ i);
	for (i = KV_TEST_ABOVE_OFF; i < sizeof flash; i++)
		flash[i] = (uint8_t)(0xA5u ^ i);
}

static int neighbours_intact(void)
{
	size_t i;

	for (i = 0; i < KV_TEST_PART_OFF; i++)
		if (flash[i] != (uint8_t)(0x5Au ^ i))
			return 0;
	for (i = KV_TEST_ABOVE_OFF; i < sizeof flash; i++)
		if (flash[i] != (uint8_t)(0xA5u ^ i))
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ *
 *  The board's side: state, hold, report, FlashDB's hooks
 * ------------------------------------------------------------------ */

struct kv_state kv_board_state;

static int dev_up = 1;
static int hold_wait_fails;
static int hold_try_answer = KV_OK;  /* what hold_try returns instead of taking */
static int db_lock_fails;            /* the next N db_lock calls fail           */
static unsigned long hold_try_calls;

static int t_ready(void)
{
	return dev_up;
}

static int t_hold_wait(void)
{
	if (hold_wait_fails)
		return -1;
	outer_held++;
	return 0;
}

static int t_hold_try(void)
{
	hold_try_calls++;
	if (hold_try_answer != KV_OK)
		return hold_try_answer;
	outer_held++;
	return KV_OK;
}

static void t_hold_release(void)
{
	if (outer_held == 0)
		outer_unbalanced++;
	else
		outer_held--;
}

static int t_db_lock(void)
{
	if (db_lock_fails > 0) {
		db_lock_fails--;
		inner_held++;   /* the store goes on regardless; keep the pair even */
		return -1;
	}
	inner_held++;
	return 0;
}

static void t_db_unlock(void)
{
	if (inner_held == 0)
		inner_unbalanced++;
	else
		inner_held--;
}

#define REPORT_MAX 64
static struct { unsigned level; char text[160]; } reports[REPORT_MAX];
static int nreports;

static void t_report(unsigned level, const char *fmt, ...)
{
	va_list ap;

	if (nreports >= REPORT_MAX)
		return;
	reports[nreports].level = level;
	va_start(ap, fmt);
	vsnprintf(reports[nreports].text, sizeof reports[nreports].text, fmt, ap);
	va_end(ap);
	nreports++;
}

const struct kv_facts kv_board_facts = {
	.part_name    = KV_TEST_PART_NAME,
	.device_ready = t_ready,
	.hold_wait    = t_hold_wait,
	.hold_try     = t_hold_try,
	.hold_release = t_hold_release,
	.db_lock      = t_db_lock,
	.db_unlock    = t_db_unlock,
	.report       = t_report,
};

static unsigned long fdb_prints;
static int asserts;

void kv_fdb_print(const char *fmt, ...)
{
	(void)fmt;
	fdb_prints++;
}

void kv_fdb_assert_failed(const char *expr, const char *func)
{
	fprintf(stderr, "test_kv_store: FlashDB assert %s in %s()\n", expr, func);
	asserts++;
}

int kv_fdb_assert_tripped(void)
{
	return asserts != 0;
}

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */

/* Was exactly this line reported, at this weight, since the last clear? */
static int reported(unsigned level, const char *text)
{
	int i;

	for (i = 0; i < nreports; i++)
		if (reports[i].level == level && strcmp(reports[i].text, text) == 0)
			return 1;
	return 0;
}

static int reported_prefix(unsigned level, const char *prefix)
{
	int i;

	for (i = 0; i < nreports; i++)
		if (reports[i].level == level &&
		    strncmp(reports[i].text, prefix, strlen(prefix)) == 0)
			return 1;
	return 0;
}

/* A reset: the board's state is zero again (FAL's and the flash are not). */
static void reboot(void)
{
	memset(&kv_board_state, 0, sizeof kv_board_state);
	nreports = 0;
}

/* After every public call: nothing left held. */
static void quiet(int line)
{
	if (outer_held != 0 || inner_held != 0) {
		fprintf(stderr, "test_kv_store: line %d: held outer %d inner %d\n",
		        line, outer_held, inner_held);
		failures++;
		outer_held = inner_held = 0;
	}
}
#define QUIET() quiet(__LINE__)

static int get_str_is(const char *key, const char *want)
{
	char buf[KV_VALUE_MAX + 1];

	return kv_get_str(key, buf, sizeof buf) == KV_OK && strcmp(buf, want) == 0;
}

struct count_ctx { unsigned n, invalid; };

static int count_one(const char *key, const struct kv_value *v, void *arg)
{
	struct count_ctx *c = arg;

	(void)key;
	c->n++;
	if (v->type == KV_TYPE_INVALID)
		c->invalid++;
	return 0;
}

/* ------------------------------------------------------------------ *
 *  Tests
 * ------------------------------------------------------------------ */

/* The model's own detectors fire: otherwise a zero below proves nothing. */
static void test_model_detects(void)
{
	uint8_t z = 0u;

	memset(flash, 0xFF, sizeof flash);
	fm_reset_counts();
	outer_held = 1;
	CHECK(fm_write(KV_TEST_PART_OFF, &z, 1u) == 1);
	CHECK(fm.not_blank == 0u);
	CHECK(fm_write(KV_TEST_PART_OFF, &z, 1u) == 1);      /* again: not blank */
	CHECK(fm.not_blank == 1u && fm.first_not_blank == (long)KV_TEST_PART_OFF);
	CHECK(fm.outside == 0u);
	CHECK(fm_read(KV_TEST_PART_OFF - 1, &z, 1u) == 1 && fm.outside == 1u);
	CHECK(fm_read(KV_TEST_ABOVE_OFF, &z, 1u) == 1 && fm.outside == 2u);
	outer_held = 0;
	CHECK(fm_read(KV_TEST_PART_OFF, &z, 1u) == 1 && fm.unheld == 1u);
	CHECK(fm_erase(KV_TEST_PART_OFF + 1, KV_TEST_SECTOR) == (int)KV_TEST_SECTOR);
	CHECK(fm.misaligned == 1u);
}

static void test_down_and_unheld(void)
{
	memset(flash, 0xFF, sizeof flash);
	fill_neighbours();
	fm_reset_counts();

	reboot();
	dev_up = 0;
	CHECK(kv_init() == KV_ERR_STATE);
	CHECK(kv_state() == KV_STATE_DOWN);
	CHECK(strcmp(kv_state_str(), "NOR flash device is down") == 0);
	CHECK(reported(KV_REPORT_ERR, "NOR device down -- configuration store unavailable"));
	CHECK(kv_format() == KV_ERR_STATE);
	dev_up = 1;
	QUIET();

	reboot();
	hold_wait_fails = 1;
	CHECK(kv_init() == KV_ERR_STATE);
	CHECK(kv_state() == KV_STATE_FAILED);
	CHECK(nreports == 0);
	hold_wait_fails = 0;
	QUIET();

	/* Not open: every entry refuses without the device. */
	reboot();
	CHECK(kv_state() == KV_STATE_INIT);
	CHECK(strcmp(kv_state_str(), "not initialized yet") == 0);
	{
		struct kv_value v;
		struct kv_info info;
		struct count_ctx c = { 0u, 0u };

		CHECK(kv_get("a", &v) == KV_ERR_STATE);
		CHECK(kv_set("a", KV_TYPE_STR, "x", 1u, NULL) == KV_ERR_STATE);
		CHECK(kv_del("a") == KV_ERR_STATE);
		CHECK(kv_set_desc("a", "d") == KV_ERR_STATE);
		CHECK(kv_foreach(NULL, count_one, &c) == KV_ERR_STATE);
		CHECK(kv_get_info(&info) == KV_ERR_STATE);
	}
	CHECK(fm_accesses() == 0u);
	CHECK(hold_try_calls == 0u);
	QUIET();
}

static void test_blank_then_data(void)
{
	struct kv_value v;
	struct kv_info info;
	uint32_t u, port = 2323u;
	int b, on = 1;
	uint8_t raw[3] = { 0u, 1u, 0u };
	struct count_ctx c = { 0u, 0u };

	reboot();
	CHECK(kv_init() == KV_OK);
	CHECK(kv_state() == KV_STATE_READY);
	CHECK(strcmp(kv_state_str(), "ready") == 0);
	CHECK(reported(KV_REPORT_INF, "blank partition -- creating the store"));
	CHECK(reported(KV_REPORT_INF, "store created: 64 KB partition, 4096 B sectors"));
	CHECK(kv_init() == KV_OK);                 /* idempotent */
	QUIET();

	CHECK(kv_set("net.mode", KV_TYPE_STR, "dhcp", 4u, "how the address is set") == KV_OK);
	CHECK(kv_set("net.shell.port", KV_TYPE_U32, &port, 4u, NULL) == KV_OK);
	CHECK(kv_set("wifi.enable", KV_TYPE_BOOL, &on, 1u, NULL) == KV_OK);
	CHECK(kv_set("blob.x", KV_TYPE_BYTES, raw, 3u, NULL) == KV_OK);
	QUIET();

	CHECK(get_str_is("net.mode", "dhcp"));
	CHECK(kv_get_u32("net.shell.port", &u) == KV_OK && u == 2323u);
	CHECK(kv_get_bool("wifi.enable", &b) == KV_OK && b == 1);
	CHECK(kv_get("blob.x", &v) == KV_OK && v.type == KV_TYPE_BYTES && v.len == 3u &&
	      memcmp(v.data, raw, 3u) == 0);
	CHECK(kv_get_u32("net.mode", &u) == KV_ERR_TYPE);
	CHECK(kv_get_bool("net.mode", &b) == KV_ERR_TYPE);
	{
		char small[4];

		CHECK(kv_get_str("net.mode", small, sizeof small) == KV_ERR_TRUNC);
		CHECK(kv_get_str("net.mode", small, 0u) == KV_ERR_PARAM);
	}
	CHECK(kv_get("no.such", &v) == KV_ERR_NOTFOUND && v.type == KV_TYPE_INVALID);
	QUIET();

	/* The description travels with the value: a set with NULL keeps it. */
	CHECK(kv_set("net.mode", KV_TYPE_STR, "static", 6u, NULL) == KV_OK);
	CHECK(kv_get("net.mode", &v) == KV_OK &&
	      strcmp(v.desc, "how the address is set") == 0 &&
	      strcmp((const char *)v.data, "static") == 0);
	CHECK(kv_set_desc("net.mode", "dhcp / static / manual") == KV_OK);
	CHECK(kv_get("net.mode", &v) == KV_OK && strcmp(v.desc, "dhcp / static / manual") == 0 &&
	      strcmp((const char *)v.data, "static") == 0);
	CHECK(kv_set_desc("no.such", "x") == KV_ERR_NOTFOUND);
	CHECK(kv_set("net.mode", KV_TYPE_STR, "dhcp", 4u, "") == KV_OK);
	CHECK(kv_get("net.mode", &v) == KV_OK && v.desc[0] == '\0');
	QUIET();

	/* Refusals before the device is touched. */
	{
		unsigned long before = fm_accesses(), tries = hold_try_calls;
		uint8_t two = 2u;
		char longdesc[KV_DESC_MAX + 2];

		memset(longdesc, 'd', sizeof longdesc - 1u);
		longdesc[sizeof longdesc - 1u] = '\0';
		CHECK(kv_set("bad key", KV_TYPE_STR, "x", 1u, NULL) == KV_ERR_PARAM);
		CHECK(kv_set("", KV_TYPE_STR, "x", 1u, NULL) == KV_ERR_PARAM);
		CHECK(kv_set("k", KV_TYPE_STR, NULL, 1u, NULL) == KV_ERR_PARAM);
		CHECK(kv_set("k", 9u, "x", 1u, NULL) == KV_ERR_PARAM);
		CHECK(kv_set("k", KV_TYPE_BOOL, &two, 1u, NULL) == KV_ERR_PARAM);
		CHECK(kv_set("k", KV_TYPE_U32, "abc", 3u, NULL) == KV_ERR_PARAM);
		CHECK(kv_set("k", KV_TYPE_STR, "x", 1u, longdesc) == KV_ERR_PARAM);
		CHECK(kv_set_desc("k", longdesc) == KV_ERR_PARAM);
		CHECK(kv_set_desc("k", NULL) == KV_ERR_PARAM);
		CHECK(kv_get("bad/key", &v) == KV_ERR_PARAM);
		CHECK(kv_get("k", NULL) == KV_ERR_PARAM);
		CHECK(kv_del("") == KV_ERR_PARAM);
		CHECK(kv_foreach(NULL, NULL, NULL) == KV_ERR_PARAM);
		CHECK(kv_get_info(NULL) == KV_ERR_PARAM);
		CHECK(fm_accesses() == before && hold_try_calls == tries);
	}

	CHECK(kv_foreach(NULL, count_one, &c) == KV_OK && c.n == 4u && c.invalid == 0u);
	c.n = 0u;
	CHECK(kv_foreach("net.", count_one, &c) == KV_OK && c.n == 2u);
	CHECK(kv_get_info(&info) == KV_OK);
	CHECK(info.kv_count == 4u && info.part_offset == KV_TEST_PART_OFF &&
	      info.part_size == KV_TEST_PART_LEN && info.sec_size == KV_TEST_SECTOR);
	CHECK(info.value_bytes > 0u && info.obj_bytes > info.value_bytes);

	CHECK(kv_del("blob.x") == KV_OK);
	CHECK(kv_del("blob.x") == KV_ERR_NOTFOUND);
	CHECK(kv_get("blob.x", &v) == KV_ERR_NOTFOUND);
	QUIET();

	/* A reset keeps everything. */
	reboot();
	CHECK(kv_init() == KV_OK);
	CHECK(reported(KV_REPORT_INF, "store open: 64 KB partition, 4096 B sectors"));
	CHECK(!reported_prefix(KV_REPORT_INF, "blank partition"));
	CHECK(get_str_is("net.mode", "dhcp"));
	CHECK(kv_get_u32("net.shell.port", &u) == KV_OK && u == 2323u);
	CHECK(kv_get("blob.x", &v) == KV_ERR_NOTFOUND);
	QUIET();
}

/* Many rewrites: FlashDB fills its sectors and recycles them. */
static void test_churn(void)
{
	char key[16], val[KV_VALUE_MAX + 1], got[KV_VALUE_MAX + 1];
	static unsigned last[24];
	unsigned long erases_before = fm.erases;
	unsigned i, k;

	for (i = 1u; i <= 3000u; i++) {
		unsigned len;

		k = (i * 7u) % 24u;
		len = 1u + (i * 37u) % 200u;
		snprintf(key, sizeof key, "churn.%u", k);
		memset(val, 'a' + (int)(i % 26u), len);
		val[len] = '\0';
		if (kv_set(key, KV_TYPE_STR, val, len, (i % 5u) ? NULL : "rewritten") != KV_OK) {
			fprintf(stderr, "test_kv_store: churn set %u failed\n", i);
			failures++;
			break;
		}
		last[k] = i;
		if (i % 500u == 0u) {
			reboot();
			CHECK(kv_init() == KV_OK);
		}
		QUIET();
	}
	for (k = 0u; k < 24u; k++) {
		unsigned j = last[k], len = 1u + (j * 37u) % 200u;

		snprintf(key, sizeof key, "churn.%u", k);
		memset(val, 'a' + (int)(j % 26u), len);
		val[len] = '\0';
		CHECK(kv_get_str(key, got, sizeof got) == KV_OK && strcmp(got, val) == 0);
	}
	/* The rewrites did recycle sectors: far more erases than the 16 a format
	 * takes. */
	CHECK(fm.erases - erases_before > 64u);
	QUIET();
}

/* The board's hold says no: the store says so, and touches nothing. */
static void test_hold_answers(void)
{
	struct kv_value v;
	struct kv_info info;
	struct count_ctx c = { 0u, 0u };
	unsigned long before = fm_accesses();

	hold_try_answer = KV_ERR_BUSY;
	CHECK(kv_get("net.mode", &v) == KV_ERR_BUSY);
	CHECK(kv_set("net.mode", KV_TYPE_STR, "x", 1u, NULL) == KV_ERR_BUSY);
	CHECK(kv_del("net.mode") == KV_ERR_BUSY);
	CHECK(kv_set_desc("net.mode", "x") == KV_ERR_BUSY);
	CHECK(kv_foreach(NULL, count_one, &c) == KV_ERR_BUSY && c.n == 0u);
	CHECK(kv_get_info(&info) == KV_ERR_BUSY);
	CHECK(kv_format() == KV_ERR_BUSY);
	hold_try_answer = KV_ERR_STATE;
	CHECK(kv_get("net.mode", &v) == KV_ERR_STATE);
	CHECK(kv_format() == KV_ERR_STATE);
	hold_try_answer = 7;                       /* not one of its three answers */
	CHECK(kv_get("net.mode", &v) == KV_ERR_STATE);
	CHECK(kv_format() == KV_ERR_STATE);
	hold_try_answer = KV_ERR_IO;
	CHECK(kv_set("net.mode", KV_TYPE_STR, "x", 1u, NULL) == KV_ERR_STATE);
	hold_try_answer = KV_OK;
	CHECK(fm_accesses() == before);
	CHECK(kv_state() == KV_STATE_READY);       /* a refused hold is not a fault */
	CHECK(get_str_is("net.mode", "dhcp"));
	QUIET();
}

/* FlashDB's lock fails once: the store latches itself offline. */
static void test_lock_broken(void)
{
	struct kv_value v;
	uint32_t port = 23u;

	nreports = 0;
	db_lock_fails = 1;
	(void)kv_set("net.shell.port", KV_TYPE_U32, &port, 4u, NULL);
	db_lock_fails = 0;
	CHECK(reported(KV_REPORT_ERR,
	               "device lock failed inside FlashDB -- store latched offline"));
	CHECK(kv_state() == KV_STATE_FAILED);
	CHECK(strcmp(kv_state_str(), "initialization failed") == 0);
	{
		unsigned long before = fm_accesses();

		CHECK(kv_get("net.mode", &v) == KV_ERR_STATE);
		CHECK(fm_accesses() == before);
	}
	QUIET();

	/* `kv format` is the way out, and it really empties the store. */
	nreports = 0;
	CHECK(kv_format() == KV_OK);
	CHECK(reported(KV_REPORT_INF, "store formatted"));
	CHECK(kv_state() == KV_STATE_READY);
	CHECK(kv_get("net.mode", &v) == KV_ERR_NOTFOUND);
	QUIET();
}

/* A healthy store formatted on request: every key goes. */
static void test_format_healthy(void)
{
	struct kv_info info;

	CHECK(kv_set("a.b", KV_TYPE_STR, "1", 1u, NULL) == KV_OK);
	CHECK(kv_get_info(&info) == KV_OK && info.kv_count == 1u);
	CHECK(kv_format() == KV_OK);
	CHECK(kv_get_info(&info) == KV_OK && info.kv_count == 0u);
	reboot();
	CHECK(kv_init() == KV_OK);
	CHECK(reported(KV_REPORT_INF, "store open: 64 KB partition, 4096 B sectors"));
	CHECK(kv_get_info(&info) == KV_OK && info.kv_count == 0u);
	QUIET();
}

/* Neither a database nor blank: reported, left alone, and only `kv format` acts. */
static void test_corrupt(void)
{
	static uint8_t copy[KV_TEST_PART_LEN];
	struct kv_info info;
	unsigned long writes, erases;
	size_t i;

	/* Leftovers from something else: no FlashDB sector header anywhere. */
	for (i = 0; i < KV_TEST_PART_LEN; i++)
		flash[KV_TEST_PART_OFF + i] = (uint8_t)(i * 13u + 1u);
	memcpy(copy, flash + KV_TEST_PART_OFF, sizeof copy);
	writes = fm.writes;
	erases = fm.erases;

	reboot();
	CHECK(kv_init() == KV_ERR_STATE);
	CHECK(kv_state() == KV_STATE_CORRUPT);
	CHECK(strcmp(kv_state_str(), "partition is not a valid database "
	                             "(run `kv format yes` to discard it)") == 0);
	CHECK(reported_prefix(KV_REPORT_ERR, "partition is neither a valid database nor blank ("));
	CHECK(fm.writes == writes && fm.erases == erases);
	CHECK(memcmp(copy, flash + KV_TEST_PART_OFF, sizeof copy) == 0);
	CHECK(kv_get_info(&info) == KV_ERR_STATE && info.part_size == KV_TEST_PART_LEN &&
	      info.sec_size == KV_TEST_SECTOR);
	QUIET();

	CHECK(kv_format() == KV_OK);
	CHECK(kv_state() == KV_STATE_READY);
	CHECK(kv_get_info(&info) == KV_OK && info.kv_count == 0u);
	CHECK(kv_set("after.format", KV_TYPE_STR, "ok", 2u, NULL) == KV_OK);
	CHECK(get_str_is("after.format", "ok"));
	QUIET();
}

int main(void)
{
	test_model_detects();
	test_down_and_unheld();

	fm_reset_counts();
	test_blank_then_data();
	test_churn();
	test_hold_answers();
	test_lock_broken();
	test_format_healthy();
	test_corrupt();

	/* Over everything since the blank partition. */
	CHECK(fm.bytes_programmed > 100000u);      /* the run did program */
	if (fm.not_blank != 0u)
		fprintf(stderr, "test_kv_store: %lu byte(s) programmed that were not 0xFF, "
		        "first at 0x%lx\n", fm.not_blank, (unsigned long)fm.first_not_blank);
	CHECK(fm.not_blank == 0u);
	CHECK(fm.outside == 0u);
	CHECK(fm.unheld == 0u);
	CHECK(fm.misaligned == 0u);
	CHECK(neighbours_intact());
	CHECK(outer_unbalanced == 0u && inner_unbalanced == 0u);
	CHECK(asserts == 0);

	if (failures != 0) {
		fprintf(stderr, "test_kv_store: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_kv_store: OK (%lu programs, %lu B, %lu erases, %lu reads, "
	       "0 onto a byte not 0xFF, 0 outside the partition)\n",
	       fm.writes, fm.bytes_programmed, fm.erases, fm.reads);
	return 0;
}
