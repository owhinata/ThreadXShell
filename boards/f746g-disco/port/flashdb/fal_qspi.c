/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fal_qspi.c
 * @brief   FAL flash device over the first megabyte of the QSPI NOR, FlashDB's
 *          log / assert hooks, and the shared key-value store's state and hold
 *          (issue #214; the store itself is svc/kv/kv.c).
 *
 * This is the whole of FlashDB's contact with the hardware: four operations
 * forwarded to port/qspi, which owns the controller, a mutex per call and the
 * bounded waiting.  Nothing about QUADSPI appears here.
 *
 * [!] THE HOLD IS KV'S LOCK, THEN THE BLOB STORE'S BUSY FLAG (decision D1).
 * On this board the QSPI NOR is erased and programmed only under the busy
 * flag (src/blob.h), and FlashDB writes while it reads -- it rewrites the state
 * of a record whose length it finds broken (fdb_kvdb.c read_kv()), and its init
 * resumes an interrupted clean-up -- so every entry point of the store takes
 * the flag for its whole operation, reads and init included.  The consequence
 * is deliberate: while a `blob write` or an `nn model load <name>` runs, `kv
 * get` and `kv list` are busy too, and a blob read refuses while a kv operation
 * runs.
 *
 *   order        kv's mutex (waited for, bounded) -> the busy flag (tried until
 *                the same deadline).  Nothing that holds the flag -- blob,
 *                `qspi erase` / `qspi test`, `nn model load` -- takes kv's
 *                mutex, so the two never wait on each other in a circle.
 *   the record   the flag records no holder, so once kv has it, kv writes which
 *                thread took it to kv_flag_owner; FAL's write and erase compare
 *                the calling thread with it and refuse on a mismatch, so a
 *                FlashDB call that some path made without the hold cannot ride
 *                another holder's flag.  Written only after the flag is kv's,
 *                cleared before it is given back.
 *   re-entry     an entry point called from inside kv on the thread the record
 *                names is refused before anything is taken.
 *   one way out  kv_hold_give() is the only release: record, flag, mutex, in
 *                that order.  svc/kv/kv.c calls it on every exit after a
 *                successful take.  FlashDB's own lock callback takes and gives
 *                nothing: it only checks that the record names this thread.
 *
 * The flag is a PRIMASK test-and-set (src/blob.c); taking it here never blocks.
 * QSPI driver calls are each atomic on the bus under the driver's own mutex,
 * which is given back between calls, so a blob read that passed the flag check
 * before kv took it may run alongside a kv operation: kv never writes the blob
 * interval, so what it reads does not change.
 *
 * THREAD CONTEXT ONLY, after the scheduler is running: the driver sleeps while
 * an erase or a program completes.
 */
#include <fal.h>
#include <fdb_cfg.h>

#include "fal_qspi.h"
#include "kv_board.h"    /* the shared store's state and facts */
#include "blob.h"        /* the busy flag, BLOB_REGION_BASE */
#include "qspi_flash.h"

#include "tx_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "kv"
#include "log.h"

/*
 * FAL's write_gran field exists but fal_partition_write() passes it straight
 * through to ops.write() without checking anything against it, so a device
 * that disagreed with FDB_WRITE_GRAN would not be caught at run time.  It is
 * caught here, at compile time.
 */
#define QSPI_FAL_WRITE_GRAN  8
_Static_assert(FDB_WRITE_GRAN == QSPI_FAL_WRITE_GRAN,
               "FlashDB write granularity must match the FAL device's write_gran");
/* The device FlashDB sees ends where the blob store begins, on a 64 KB
 * boundary, so no erase this file issues can reach a slot. */
_Static_assert(KV_PART_OFFSET + KV_PART_LEN == KV_FAL_DEV_LEN,
               "the kv partition is the whole FAL device");
_Static_assert(KV_FAL_DEV_LEN == BLOB_REGION_BASE,
               "the FAL device must end where the blob store begins");
_Static_assert((KV_FAL_DEV_LEN % QSPI_FLASH_SECTOR_SIZE) == 0u,
               "the FAL device must be whole 64 KB sectors");

/* ------------------------------------------------------------------ *
 *  The hold
 * ------------------------------------------------------------------ */

/* kv's own lock: serialises the store's entry points against each other.
 * Priority inheritance, since a shell command and the configuration thread at
 * a higher priority can both want it. */
static TX_MUTEX kv_lock;
static volatile int kv_lock_ready;

/* Which thread holds the busy flag for kv, or NULL.  Written only by the
 * thread that holds kv_lock AND the flag (set after taking the flag, cleared
 * before giving it back); read by FAL's write and erase on the calling thread.
 * A thread comparing it with itself cannot be misled by a concurrent write:
 * only that same thread could store its own identity here. */
static TX_THREAD *volatile kv_flag_owner;

/*
 * How long an entry point waits before answering "store busy".  It has to
 * outlast one kv operation on the other console (a sector erase is 0.8 s at
 * most) without making a stuck holder look like a hung console.  It does NOT
 * try to outlast a `blob write` (minutes) or a `kv format` (about 75 s): a
 * command that arrives during one is told the store is busy, which is both true
 * and more useful than a console that stops answering.  1 tick = 1 ms.
 */
#define KV_HOLD_WAIT_TICKS  5000u
/* How often a waiting entry point tries the busy flag again. */
#define KV_FLAG_POLL_TICKS  10u

int kv_qspi_init(void)
{
	if (kv_lock_ready)
		return 0;
	if (tx_mutex_create(&kv_lock, "kv", TX_INHERIT) != TX_SUCCESS)
		return -1;
	kv_lock_ready = 1;
	return 0;
}

const void *kv_qspi_holder(void)
{
	return (const void *)kv_flag_owner;
}

/* Nonzero when kv holds the busy flag, on this thread. */
static int kv_owns_flag(void)
{
	TX_THREAD *self = tx_thread_identify();

	return self != TX_NULL && kv_flag_owner == self;
}

/*
 * Take the hold: kv's mutex, then the busy flag, within @p budget ticks
 * (TX_WAIT_FOREVER: no limit).  KV_OK, KV_ERR_BUSY when the budget ran out,
 * KV_ERR_STATE when it cannot be had at all (no lock, not a thread, or a
 * re-entry).  On anything but KV_OK nothing is held and the record is
 * untouched.
 */
static int kv_hold_take(ULONG budget)
{
	TX_THREAD *self = tx_thread_identify();
	ULONG t0, left;
	UINT rc;

	if (!kv_lock_ready || self == TX_NULL)
		return KV_ERR_STATE;
	/* [!] Re-entry (an entry point called from inside one, on its thread):
	 * refused before the mutex, which is recursive and would let it in, and
	 * before the flag. */
	if (kv_flag_owner == self)
		return KV_ERR_STATE;

	t0 = tx_time_get();
	rc = tx_mutex_get(&kv_lock, budget);
	if (rc == TX_NOT_AVAILABLE)
		return KV_ERR_BUSY;
	if (rc != TX_SUCCESS)
		return KV_ERR_STATE;

	while (blob_busy_acquire() != BLOB_OK) {
		if (budget != TX_WAIT_FOREVER) {
			ULONG spent = tx_time_get() - t0;

			if (spent >= budget) {
				(void)tx_mutex_put(&kv_lock);
				return KV_ERR_BUSY;
			}
			left = budget - spent;
			(void)tx_thread_sleep(left < KV_FLAG_POLL_TICKS ? left
			                                                : KV_FLAG_POLL_TICKS);
		} else {
			(void)tx_thread_sleep(KV_FLAG_POLL_TICKS);
		}
	}
	kv_flag_owner = self;      /* only now that the flag is kv's */
	return KV_OK;
}

/* The one way out of a hold: the record, then the flag, then the mutex. */
static void kv_hold_give(void)
{
	if (!kv_owns_flag()) {
		/* Not reachable through svc/kv/kv.c, which gives back only what it
		 * took.  Giving the flag back here would release somebody else's. */
		LOG_ERR("hold given back by a thread that does not hold it -- ignored");
		return;
	}
	kv_flag_owner = TX_NULL;
	blob_busy_release();
	(void)tx_mutex_put(&kv_lock);
}

/* ------------------------------------------------------------------ *
 *  The FAL device: the first megabyte of the QSPI NOR
 * ------------------------------------------------------------------ */

/* [off, off + n) inside the device FlashDB is given. */
static int qspi_fal_inside(long off, size_t n)
{
	return off >= 0 && (unsigned long)off <= KV_FAL_DEV_LEN &&
	       n <= KV_FAL_DEV_LEN - (unsigned long)off;
}

static int qspi_fal_init(void)
{
	/* The driver was brought up in tx_application_define(); this only reports
	 * whether it came up.  No flash transaction. */
	return qspi_flash_ready() ? 0 : -1;
}

static int qspi_fal_read(long offset, uint8_t *buf, size_t size)
{
	if (!qspi_fal_inside(offset, size))
		return -1;
	if (qspi_flash_read((uint32_t)offset, buf, (uint32_t)size) != QSPI_FLASH_OK)
		return -1;
	return (int)size;
}

static int qspi_fal_write(long offset, const uint8_t *buf, size_t size)
{
	if (!qspi_fal_inside(offset, size))
		return -1;
	/* [!] Programmed only under the busy flag, held by kv on this thread. */
	if (!kv_owns_flag()) {
		LOG_ERR("FAL write at 0x%06lx refused: kv does not hold the QSPI "
		        "busy flag on this thread", (unsigned long)offset);
		return -1;
	}
	if (qspi_flash_write((uint32_t)offset, buf, (uint32_t)size) != QSPI_FLASH_OK)
		return -1;
	return (int)size;
}

static int qspi_fal_erase(long offset, size_t size)
{
	uint32_t at, end;

	/*
	 * Whole 4 KB subsectors inside the device, or nothing.  FlashDB asks for
	 * whole sectors (and fal_partition_erase_all() for the whole device), so a
	 * range that is not is a fault upstream: refused rather than widened, so an
	 * erase never does more than it was asked.
	 */
	if (size == 0u || !qspi_fal_inside(offset, size) ||
	    ((unsigned long)offset % QSPI_FLASH_SUBSECTOR_SIZE) != 0u ||
	    (size % QSPI_FLASH_SUBSECTOR_SIZE) != 0u)
		return -1;
	if (!kv_owns_flag()) {
		LOG_ERR("FAL erase at 0x%06lx refused: kv does not hold the QSPI "
		        "busy flag on this thread", (unsigned long)offset);
		return -1;
	}

	at = (uint32_t)offset;
	end = at + (uint32_t)size;
	while (at < end) {
		/* [!] ONE ERASE PER CALL, and a 64 KB one only on a 64 KB boundary
		 * with a whole sector left: qspi_flash_erase_sector() erases the
		 * sector CONTAINING its address, so anywhere else it would reach
		 * outside the range.  The driver's mutex is given back between. */
		int whole = (at % QSPI_FLASH_SECTOR_SIZE) == 0u &&
		            end - at >= QSPI_FLASH_SECTOR_SIZE;
		int rc = whole ? qspi_flash_erase_sector(at)
		               : qspi_flash_erase_subsector(at);

		if (rc != QSPI_FLASH_OK)
			return -1;
		at += whole ? QSPI_FLASH_SECTOR_SIZE : QSPI_FLASH_SUBSECTOR_SIZE;
	}
	return (int)size;
}

const struct fal_flash_dev qspi_fal_dev = {
	.name = QSPI_FAL_DEV_NAME,
	.addr = 0,                      /* offsets, not addresses: nothing is mapped */
	.len = KV_FAL_DEV_LEN,          /* the first megabyte only (fal_cfg.h) */
	.blk_size = QSPI_FLASH_SUBSECTOR_SIZE,
	.ops = {
		.init  = qspi_fal_init,
		.read  = qspi_fal_read,
		.write = qspi_fal_write,
		.erase = qspi_fal_erase,
	},
	.write_gran = QSPI_FAL_WRITE_GRAN,
};

/* ------------------------------------------------------------------ *
 *  Diagnostics: FDB_PRINT / FAL_PRINTF -> the reset-persistent log ring
 * ------------------------------------------------------------------ */

/*
 * FlashDB and FAL emit one message as several printf fragments; the line
 * assembler (svc/log.h) puts them back together.  It is static and unguarded,
 * which is safe because every FlashDB and FAL path runs under the hold on the
 * thread the record names, so there is never a second thread inside it.  A
 * fragment from any other thread (only a path that called FlashDB without the
 * hold, which FAL then refuses) goes to the log as it is instead of through it.
 */
static struct log_line kv_log;

void kv_fdb_print(const char *fmt, ...)
{
	char frag[LOG_LINE_ASM_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(frag, sizeof frag, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof frag - 1)
		n = (int)sizeof frag - 1;   /* vsnprintf reports the untruncated length */
	if (kv_owns_flag())
		log_line_feed(&kv_log, LOG_TAG, frag, (size_t)n);
	else
		log_write_bytes(LOG_LEVEL_INF, LOG_TAG, "(unheld) ", frag, (size_t)n,
		                NULL);
}

/* ------------------------------------------------------------------ *
 *  Assertion hook
 * ------------------------------------------------------------------ */

/* Sticky: once FlashDB has asserted, its internal state is not to be trusted, so
 * svc/kv/kv.c latches the database out of service.  See fdb_cfg.h. */
static volatile int kv_fdb_asserted;

void kv_fdb_assert_failed(const char *expr, const char *func)
{
	kv_fdb_asserted = 1;
	LOG_ERR("FlashDB assert: %s in %s()", expr, func);
}

int kv_fdb_assert_tripped(void)
{
	return kv_fdb_asserted;
}

/* ------------------------------------------------------------------ *
 *  The shared store's state and facts (svc/kv/kv_board.h)
 * ------------------------------------------------------------------ */

/* Plain .bss: zero is "not initialised yet". */
struct kv_state kv_board_state;

/* The store's lines go to the reset-persistent ring under the "kv" tag. */
_Static_assert(KV_REPORT_ERR == LOG_LEVEL_ERR && KV_REPORT_WRN == LOG_LEVEL_WRN &&
               KV_REPORT_INF == LOG_LEVEL_INF,
               "kv report weights must be this board's log levels");

static void kv_qspi_report(unsigned level, const char *fmt, ...)
{
	va_list ap;

	if (level > LOG_COMPILE_LEVEL)   /* the LOG_*() macros' build-time floor */
		return;
	va_start(ap, fmt);
	log_vwrite(level, LOG_TAG, fmt, ap);
	va_end(ap);
}

static int kv_qspi_ready(void)
{
	return qspi_flash_ready();
}

/* kv_init(): no limit.  It runs on the configuration thread at boot, ahead of
 * every console, so in practice it finds both free at once. */
static int kv_qspi_hold_wait(void)
{
	return (kv_hold_take(TX_WAIT_FOREVER) == KV_OK) ? 0 : -1;
}

static int kv_qspi_hold_try(void)
{
	return kv_hold_take(KV_HOLD_WAIT_TICKS);
}

/* FlashDB's lock callback runs only inside an entry point's hold: it takes and
 * gives nothing, and fails (latching the store offline) if the record does not
 * name this thread. */
static int kv_qspi_db_lock(void)
{
	return kv_owns_flag() ? 0 : -1;
}

static void kv_qspi_db_unlock(void)
{
}

const struct kv_facts kv_board_facts = {
	.part_name    = KV_PART_NAME,
	.device_ready = kv_qspi_ready,
	.hold_wait    = kv_qspi_hold_wait,
	.hold_try     = kv_qspi_hold_try,
	.hold_release = kv_hold_give,
	.db_lock      = kv_qspi_db_lock,
	.db_unlock    = kv_qspi_db_unlock,
	.report       = kv_qspi_report,
};
