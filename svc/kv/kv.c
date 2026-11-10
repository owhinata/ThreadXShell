/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv.c
 * @brief   Persistent configuration key-value store (owhinata/wio-lite-ai#37;
 *          the shared implementation since issue #214).
 *
 * See kv.h for the contract.  This file is the only caller of FlashDB.  It owns
 * no storage: the database, the state and the walk's scratch are the board's
 * kv_board_state, and the device hold and the log are the board's kv_board_facts
 * (kv_board.h).
 */
#include "kv.h"
#include "kv_board.h"
#include "kv_record.h"

#include <flashdb.h>
#include <fal.h>

#include <stddef.h>
#include <string.h>

/* The board defines the state without an initializer; zero has to mean INIT. */
_Static_assert(KV_STATE_INIT == 0, "a zeroed kv_state must read as not initialised");

/* Buffer for the blank-partition scan.  Only used on the failure path. */
#define KV_SCAN_CHUNK  256u

#define KV_ST  kv_board_state
#define KV_FX  kv_board_facts

/* ------------------------------------------------------------------ *
 *  Locking
 * ------------------------------------------------------------------ */

/*
 * FlashDB's lock callback.  Its signature returns nothing and its caller does not
 * look at the result, so a failure here cannot be reported downward: FlashDB would
 * simply carry on and touch the device unlocked.  The only two defences are (a)
 * the board's lock waiting forever rather than giving up, so the ordinary
 * "someone else is erasing" case can never turn into a failure at all, and (b)
 * latching lock_broken so the next call through the public API refuses.
 */
static void kv_db_lock(fdb_db_t db)
{
	(void)db;
	if (KV_FX.db_lock() != 0) {
		KV_ST.lock_broken = 1;
		KV_FX.report(KV_REPORT_ERR,
		             "device lock failed inside FlashDB -- store latched offline");
	}
}

static void kv_db_unlock(fdb_db_t db)
{
	(void)db;
	KV_FX.db_unlock();
}

/* The board's bounded hold, held to its three answers. */
static int kv_hold_try(void)
{
	int rc = KV_FX.hold_try();

	if (rc != KV_OK && rc != KV_ERR_BUSY)
		rc = KV_ERR_STATE;
	return rc;
}

/* Outer gate for every public entry point: refuse unless the store is usable, then
 * take the device with a finite wait.  Pairs with kv_op_end(). */
static int kv_op_begin(void)
{
	if (KV_ST.status != KV_STATE_READY)
		return KV_ERR_STATE;
	if (KV_ST.lock_broken || kv_fdb_assert_tripped())
		return KV_ERR_STATE;
	return kv_hold_try();
}

static void kv_op_end(void)
{
	KV_FX.hold_release();
}

/* Map FlashDB's error enum onto this layer's codes. */
static int kv_map_err(fdb_err_t err)
{
	switch (err) {
	case FDB_NO_ERR:        return KV_OK;
	case FDB_KV_NAME_ERR:   return KV_ERR_PARAM;
	case FDB_KV_NAME_EXIST: return KV_ERR_PARAM;
	case FDB_SAVED_FULL:    return KV_ERR_FULL;
	case FDB_READ_ERR:
	case FDB_WRITE_ERR:
	case FDB_ERASE_ERR:     return KV_ERR_IO;
	default:                return KV_ERR_IO;
	}
}

/* ------------------------------------------------------------------ *
 *  Initialisation
 * ------------------------------------------------------------------ */

/* Is the whole partition erased?  Only asked when an init attempt has already
 * failed, so the cost (a megabyte read, tens of milliseconds) is paid on the rare
 * path and a normal boot never scans. */
static int kv_partition_blank(const struct fal_partition *part)
{
	uint8_t buf[KV_SCAN_CHUNK];
	uint32_t off = 0u;

	while (off < part->len) {
		uint32_t chunk = part->len - off;
		uint32_t i;

		if (chunk > sizeof buf)
			chunk = sizeof buf;
		if (fal_partition_read(part, off, buf, chunk) < 0)
			return 0;
		for (i = 0u; i < chunk; i++) {
			if (buf[i] != 0xFFu)
				return 0;
		}
		off += chunk;
	}
	return 1;
}

/*
 * One initialisation attempt from a clean slate.
 *
 * The database object is re-zeroed and its three control settings re-applied every
 * time, so the second attempt cannot inherit anything the first one left behind.
 * That sidesteps the question of whether a half-initialised fdb_kvdb is reusable
 * entirely -- and it is safe to do because FlashDB keeps no registry of open
 * databases; the whole of its state lives in this struct.
 */
static fdb_err_t kv_try_init(int allow_format)
{
	bool not_format = allow_format ? false : true;

	memset(&KV_ST.db, 0, sizeof KV_ST.db);
	fdb_kvdb_control(&KV_ST.db, FDB_KVDB_CTRL_SET_LOCK, (void *)(uintptr_t)kv_db_lock);
	fdb_kvdb_control(&KV_ST.db, FDB_KVDB_CTRL_SET_UNLOCK, (void *)(uintptr_t)kv_db_unlock);
	fdb_kvdb_control(&KV_ST.db, FDB_KVDB_CTRL_SET_NOT_FORMAT, &not_format);
	/* The name and partition strings are stored by pointer, so they must be
	 * literals with static lifetime -- not stack buffers. */
	return fdb_kvdb_init(&KV_ST.db, "cfg", KV_FX.part_name, NULL, NULL);
}

int kv_init(void)
{
	const struct fal_partition *part;
	fdb_err_t err;

	if (KV_ST.status == KV_STATE_READY)
		return KV_OK;

	if (!KV_FX.device_ready()) {
		KV_ST.status = KV_STATE_DOWN;
		KV_FX.report(KV_REPORT_ERR,
		             "NOR device down -- configuration store unavailable");
		return KV_ERR_STATE;
	}

	/* Hold the device across the whole of initialisation.  FlashDB drops its own
	 * lock between internal steps, and a first-time format erases a megabyte; an
	 * outer hold keeps a shell command from landing in the middle of that. */
	if (KV_FX.hold_wait() != 0) {
		KV_ST.status = KV_STATE_FAILED;
		return KV_ERR_STATE;
	}

	if (fal_init() < 0) {
		KV_ST.status = KV_STATE_FAILED;
		KV_FX.report(KV_REPORT_ERR, "FAL init failed");
		goto out;
	}
	part = fal_partition_find(KV_FX.part_name);
	if (part == NULL) {
		KV_ST.status = KV_STATE_FAILED;
		KV_FX.report(KV_REPORT_ERR, "partition '%s' not found", KV_FX.part_name);
		goto out;
	}
	KV_ST.geom.part_offset = (uint32_t)part->offset;
	KV_ST.geom.part_size = (uint32_t)part->len;
	/* Take the sector size from the flash device rather than waiting to read it
	 * back out of the database.  `kv info` has to be useful precisely when the
	 * database did NOT open, and reporting "0 B sectors" there would look like a
	 * second fault on top of the first. */
	{
		const struct fal_flash_dev *dev = fal_flash_device_find(part->flash_name);

		if (dev != NULL)
			KV_ST.geom.sec_size = (uint32_t)dev->blk_size;
	}

	/* First attempt: refuse to format.  A database that is merely unrecognised
	 * must not be erased on our way past it. */
	err = kv_try_init(0);
	if (err == FDB_NO_ERR) {
		KV_ST.status = KV_STATE_READY;
		KV_ST.geom.sec_size = KV_ST.db.parent.sec_size;
		KV_FX.report(KV_REPORT_INF, "store open: %lu KB partition, %lu B sectors",
		             (unsigned long)(KV_ST.geom.part_size >> 10),
		             (unsigned long)KV_ST.geom.sec_size);
		goto out;
	}

	/* It failed.  Only a partition that is entirely erased is a first boot;
	 * anything else is damage, and gets reported rather than overwritten. */
	if (!kv_partition_blank(part)) {
		KV_ST.status = KV_STATE_CORRUPT;
		KV_FX.report(KV_REPORT_ERR,
		             "partition is neither a valid database nor blank (%d) -- "
		             "run `kv format yes` to discard it", (int)err);
		goto out;
	}

	KV_FX.report(KV_REPORT_INF, "blank partition -- creating the store");
	err = kv_try_init(1);
	if (err == FDB_NO_ERR) {
		KV_ST.status = KV_STATE_READY;
		KV_ST.geom.sec_size = KV_ST.db.parent.sec_size;
		KV_FX.report(KV_REPORT_INF, "store created: %lu KB partition, %lu B sectors",
		             (unsigned long)(KV_ST.geom.part_size >> 10),
		             (unsigned long)KV_ST.geom.sec_size);
	} else {
		KV_ST.status = KV_STATE_FAILED;
		KV_FX.report(KV_REPORT_ERR, "format failed (%d)", (int)err);
	}

out:
	KV_FX.hold_release();
	return (KV_ST.status == KV_STATE_READY) ? KV_OK : KV_ERR_STATE;
}

int kv_state(void)
{
	if (KV_ST.status == KV_STATE_READY &&
	    (KV_ST.lock_broken || kv_fdb_assert_tripped()))
		return KV_STATE_FAILED;
	return KV_ST.status;
}

const char *kv_state_str(void)
{
	switch (kv_state()) {
	case KV_STATE_READY:   return "ready";
	case KV_STATE_INIT:    return "not initialized yet";
	case KV_STATE_DOWN:    return "NOR flash device is down";
	case KV_STATE_CORRUPT: return "partition is not a valid database "
	                              "(run `kv format yes` to discard it)";
	default:               return "initialization failed";
	}
}

/* ------------------------------------------------------------------ *
 *  Data path
 * ------------------------------------------------------------------ */

/*
 * Raw record I/O.  Both assume the device lock is already held.
 *
 * Reading deliberately avoids fdb_kv_get_blob(): that call funnels into get_kv(),
 * which ignores what _fdb_flash_read() returned (fdb_kvdb.c:636-638) and reports
 * the requested length either way -- so a value the flash failed to hand over would
 * come back as a successful read of a stale buffer.  Looking the key up and reading
 * the blob explicitly costs one extra call and gets an answer that is either the
 * stored bytes or an error, because fdb_blob_read() returns 0 on a failed read
 * (fdb_utils.c:243-246).
 */
static int kv_raw_get(const char *key, uint8_t *raw, uint32_t rawsize,
                      uint32_t *rawlen)
{
	struct fdb_blob blob;
	struct fdb_kv kv;
	size_t got;

	*rawlen = 0u;
	if (fdb_kv_get_obj(&KV_ST.db, key, &kv) == NULL || kv.value_len == 0u)
		return KV_ERR_NOTFOUND;
	if (kv.value_len > rawsize)
		return KV_ERR_FORMAT;   /* longer than any record we could have written */

	fdb_kv_to_blob(&kv, fdb_blob_make(&blob, raw, rawsize));
	got = fdb_blob_read(&KV_ST.db.parent, &blob);
	if (got != blob.saved.len)
		return KV_ERR_IO;
	*rawlen = (uint32_t)got;
	return KV_OK;
}

static int kv_raw_set(const char *key, const uint8_t *raw, uint32_t rawlen)
{
	struct fdb_blob blob;

	return kv_map_err(fdb_kv_set_blob(&KV_ST.db, key,
	                                  fdb_blob_make(&blob, raw, rawlen)));
}

/*
 * Write a value, optionally carrying the existing description forward.
 *
 * Read-modify-write, and the read half is why this holds one lock across both:
 * the description belongs to the key, not to the caller, so `kv set` on a
 * documented setting must not quietly drop the documentation, and a concurrent
 * `kv desc` must not be able to slip between the read and the write.
 */
int kv_set(const char *key, uint8_t type, const void *val, uint32_t len,
           const char *desc)
{
	uint8_t raw[KV_REC_MAX];
	char kept[KV_DESC_MAX + 1u];
	uint32_t rawlen;
	int rc;

	if (!kv_key_ok(key) || val == NULL || !kv_type_ok(type))
		return KV_ERR_PARAM;
	/* Reject before taking the device: kv_rec_pack() would refuse it anyway, but
	 * only after a lock and a read-modify-write's worth of work. */
	if (!kv_value_bytes_ok(type, (const uint8_t *)val, len))
		return KV_ERR_PARAM;
	if (desc != NULL && strlen(desc) > KV_DESC_MAX)
		return KV_ERR_PARAM;

	rc = kv_op_begin();
	if (rc != KV_OK)
		return rc;

	if (desc == NULL) {
		/* Inherit whatever description is already there.  A record we cannot
		 * decode simply contributes none -- overwriting it is what the caller
		 * asked for, and refusing would leave a bad record unfixable. */
		struct kv_value old;

		kept[0] = '\0';
		if (kv_raw_get(key, raw, sizeof raw, &rawlen) == KV_OK &&
		    kv_rec_unpack(raw, rawlen, &old) == KV_OK)
			memcpy(kept, old.desc, strlen(old.desc) + 1u);
		desc = kept;
	}

	rawlen = kv_rec_pack(raw, type, val, len, desc);
	rc = (rawlen == 0u) ? KV_ERR_PARAM : kv_raw_set(key, raw, rawlen);
	kv_op_end();
	return rc;
}

int kv_get(const char *key, struct kv_value *out)
{
	uint8_t raw[KV_REC_MAX];
	uint32_t rawlen;
	int rc;

	if (!kv_key_ok(key) || out == NULL)
		return KV_ERR_PARAM;

	rc = kv_op_begin();
	if (rc != KV_OK)
		return rc;
	rc = kv_raw_get(key, raw, sizeof raw, &rawlen);
	if (rc == KV_OK)
		rc = kv_rec_unpack(raw, rawlen, out);
	else
		out->type = KV_TYPE_INVALID;
	kv_op_end();
	return rc;
}

int kv_set_desc(const char *key, const char *desc)
{
	uint8_t raw[KV_REC_MAX];
	struct kv_value cur;
	uint32_t rawlen;
	int rc;

	if (!kv_key_ok(key) || desc == NULL || strlen(desc) > KV_DESC_MAX)
		return KV_ERR_PARAM;

	rc = kv_op_begin();
	if (rc != KV_OK)
		return rc;
	rc = kv_raw_get(key, raw, sizeof raw, &rawlen);
	if (rc == KV_OK)
		rc = kv_rec_unpack(raw, rawlen, &cur);
	if (rc == KV_OK) {
		/* Rebuild from the decoded value so the new record is as well-formed as
		 * a freshly written one -- patching the description in place would have
		 * to move the value and rewrite the lengths anyway. */
		rawlen = kv_rec_pack(raw, cur.type, cur.data, cur.len, desc);
		rc = (rawlen == 0u) ? KV_ERR_FORMAT : kv_raw_set(key, raw, rawlen);
	}
	kv_op_end();
	return rc;
}

/* ---- typed readers ------------------------------------------------------- */

int kv_get_str(const char *key, char *buf, uint32_t buflen)
{
	struct kv_value v;
	int rc;

	if (buf == NULL || buflen == 0u)
		return KV_ERR_PARAM;
	rc = kv_get(key, &v);
	if (rc != KV_OK)
		return rc;
	if (v.type != KV_TYPE_STR)
		return KV_ERR_TYPE;
	if (v.len + 1u > buflen)
		return KV_ERR_TRUNC;
	memcpy(buf, v.data, v.len + 1u);
	return KV_OK;
}

int kv_get_u32(const char *key, uint32_t *out)
{
	struct kv_value v;
	int rc;

	if (out == NULL)
		return KV_ERR_PARAM;
	rc = kv_get(key, &v);
	if (rc != KV_OK)
		return rc;
	if (v.type != KV_TYPE_U32)
		return KV_ERR_TYPE;
	*out = kv_rec_u32(v.data);
	return KV_OK;
}

int kv_get_bool(const char *key, int *out)
{
	struct kv_value v;
	int rc;

	if (out == NULL)
		return KV_ERR_PARAM;
	rc = kv_get(key, &v);
	if (rc != KV_OK)
		return rc;
	if (v.type != KV_TYPE_BOOL)
		return KV_ERR_TYPE;
	*out = (v.data[0] != 0u);
	return KV_OK;
}

int kv_del(const char *key)
{
	fdb_err_t err;
	int rc;

	if (!kv_key_ok(key))
		return KV_ERR_PARAM;

	rc = kv_op_begin();
	if (rc != KV_OK)
		return rc;
	err = fdb_kv_del(&KV_ST.db, key);
	kv_op_end();
	/* FlashDB reports a missing key as a generic failure; distinguish it here so
	 * `kv del` can say "no such key" instead of "I/O error". */
	if (err == FDB_KV_NAME_ERR)
		return KV_ERR_NOTFOUND;
	return kv_map_err(err);
}

int kv_format(void)
{
	const struct fal_partition *part;
	fdb_err_t err;
	int rc = KV_OK;

	/* Formatting is the way OUT of a corrupt or failed store, so unlike every
	 * other entry point it must work when the store is not ready.  It still needs
	 * the device itself to be alive. */
	if (!KV_FX.device_ready())
		return KV_ERR_STATE;

	rc = kv_hold_try();
	if (rc != KV_OK)
		return rc;

	part = fal_partition_find(KV_FX.part_name);
	if (part == NULL) {
		KV_FX.hold_release();
		return KV_ERR_STATE;
	}

	/*
	 * Erase the partition ourselves before building, and do NOT mistake this for
	 * redundant work just because FlashDB's format_sector() erases every sector
	 * again on its way through (fdb_kvdb.c:776).  Without this step, a format
	 * asked of a *healthy* store would do nothing at all: kv_try_init() with
	 * formatting permitted still only formats a database it cannot open, so it
	 * would simply re-open the existing one and report success while every key
	 * survived.  Wiping first is what makes "kv format" mean what it says, and it
	 * is also what makes it work from a partition FlashDB refuses to open.
	 *
	 * It is slow, and unavoidably so: one sector erase per database sector inside
	 * FlashDB dominates (each board README has its part's time; wio-lite-ai
	 * measured ~14 s on board #2).  Our own pass is the cheap part when the FAL
	 * device erases in larger blocks, and shortening it would not be noticed.
	 */
	if (fal_partition_erase_all(part) < 0) {
		KV_FX.hold_release();
		KV_FX.report(KV_REPORT_ERR, "format: erase failed");
		KV_ST.status = KV_STATE_FAILED;
		return KV_ERR_IO;
	}

	KV_ST.lock_broken = 0;
	err = kv_try_init(1);
	if (err == FDB_NO_ERR) {
		KV_ST.status = KV_STATE_READY;
		KV_ST.geom.sec_size = KV_ST.db.parent.sec_size;
		KV_FX.report(KV_REPORT_INF, "store formatted");
	} else {
		KV_ST.status = KV_STATE_FAILED;
		KV_FX.report(KV_REPORT_ERR, "format: init failed (%d)", (int)err);
		rc = KV_ERR_IO;
	}
	KV_FX.hold_release();
	return rc;
}

/* ------------------------------------------------------------------ *
 *  Iteration + usage
 * ------------------------------------------------------------------ */

/*
 * Walk the database once, optionally invoking @p cb, and always accumulating the
 * usage totals.  Both `kv list` and `kv info` need exactly this walk, and doing it
 * in one place means the two can never disagree about what is in the store.
 * Called with the device already held; its scratch is the board's (kv_board.h).
 */
static int kv_walk(const char *prefix, kv_iter_cb cb, void *arg,
                   struct kv_info *info)
{
	struct fdb_kv_iterator itr;
	size_t prefix_len = (prefix != NULL) ? strlen(prefix) : 0u;

	if (info != NULL) {
		info->kv_count = 0u;
		info->value_bytes = 0u;
		info->obj_bytes = 0u;
	}

	fdb_kv_iterator_init(&KV_ST.db, &itr);
	while (fdb_kv_iterate(&KV_ST.db, &itr)) {
		struct fdb_kv *kv = &itr.curr_kv;
		struct fdb_blob blob;
		size_t got;

		if (info != NULL) {
			info->kv_count++;
			info->value_bytes += (uint32_t)kv->value_len;
			info->obj_bytes += (uint32_t)kv->len;
		}
		if (cb == NULL)
			continue;
		if (prefix_len != 0u && strncmp(kv->name, prefix, prefix_len) != 0)
			continue;

		/* fdb_blob_read() takes no lock of its own -- it is safe here only
		 * because this whole walk runs under the caller's device lock. */
		fdb_kv_to_blob(kv, fdb_blob_make(&blob, KV_ST.walk_raw,
		                                 sizeof KV_ST.walk_raw));
		got = fdb_blob_read(&KV_ST.db.parent, &blob);
		/* A short or failed read, an oversized record and a record we cannot
		 * decode all end up as KV_TYPE_INVALID.  The callback still gets called:
		 * a listing that skipped what it could not read would make a store that
		 * is half-corrupt look healthy, which is the one impression it must
		 * never give. */
		if (got != blob.saved.len ||
		    kv_rec_unpack(KV_ST.walk_raw, (uint32_t)got, &KV_ST.walk_val) != KV_OK) {
			KV_ST.walk_val.type = KV_TYPE_INVALID;
			KV_ST.walk_val.len = 0u;
			KV_ST.walk_val.data[0] = '\0';
			KV_ST.walk_val.desc[0] = '\0';
		}
		if (cb(kv->name, &KV_ST.walk_val, arg) != 0)
			break;
	}
	return KV_OK;
}

int kv_foreach(const char *prefix, kv_iter_cb cb, void *arg)
{
	int rc;

	if (cb == NULL)
		return KV_ERR_PARAM;

	rc = kv_op_begin();
	if (rc != KV_OK)
		return rc;
	rc = kv_walk(prefix, cb, arg, NULL);
	kv_op_end();
	return rc;
}

int kv_get_info(struct kv_info *info)
{
	int rc;

	if (info == NULL)
		return KV_ERR_PARAM;

	*info = KV_ST.geom;
	if (kv_state() != KV_STATE_READY)
		return KV_ERR_STATE;

	rc = kv_op_begin();
	if (rc != KV_OK)
		return rc;
	rc = kv_walk(NULL, NULL, NULL, info);
	kv_op_end();
	info->part_offset = KV_ST.geom.part_offset;
	info->part_size = KV_ST.geom.part_size;
	info->sec_size = KV_ST.geom.sec_size;
	return rc;
}
