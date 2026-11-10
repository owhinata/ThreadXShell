/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_board.h
 * @brief   What a board gives the shared key-value store (issue #214).
 *
 * svc/kv/kv.c is the one implementation of the store over FlashDB.  It OWNS NO
 * STORAGE (cmake/check_no_mutable_storage.py, per board) and cannot include
 * tx_api.h or a board header (svc/README.md).  So the board DEFINES the two
 * objects below under their fixed names -- its mutable state, and a const table of
 * its partition's name and of the functions that take and give back its device --
 * and kv.c reaches them through these extern declarations, resolved at link time
 * (the shape of shell/cmds/net_core.h and svc/nn/nn_dispatch_state.h).  Nothing is
 * registered at run time: a pointer the shared file remembered would itself be
 * mutable storage in it, and FlashDB's lock callback takes no context argument
 * that could carry one.
 *
 * Under the shared code the board also owns, unchanged:
 *   - the FAL device (its init / read / write / erase) and the partition table,
 *     in the fal_cfg.h FAL includes by name;
 *   - FlashDB's own log and assert hooks named in svc/kv/fdb_cfg.h
 *     (kv_fdb_print(), kv_fdb_assert_failed(), kv_fdb_assert_tripped()), with the
 *     line assembler and the assert mark behind them.
 *
 * Every line the store writes to the log goes through facts.report() (the board's
 * log is the board's: svc/ has no log.h).
 */
#ifndef SVC_KV_BOARD_H
#define SVC_KV_BOARD_H

#include <flashdb.h>

#include "kv.h"
#include "kv_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The weight of a line handed to kv_facts.report(); the board maps them onto its
 * log's levels (and checks the mapping at build time). */
#define KV_REPORT_ERR  0u
#define KV_REPORT_WRN  1u
#define KV_REPORT_INF  2u

/**
 * The board's mutable state for kv.c.  The board defines it without an
 * initializer: all zero is "not initialised yet" (KV_STATE_INIT is 0).
 *
 * db, status, geom and the walk's scratch are only touched with the device held
 * (the board's hold, below), except status, which kv_state() reads unlocked as
 * one int.  lock_broken is set from inside FlashDB's lock callback.
 */
struct kv_state {
	struct fdb_kvdb db;          /**< FlashDB keeps all of a database's state here */
	int             status;      /**< KV_STATE_*                                    */
	struct kv_info  geom;        /**< partition geometry, filled at init            */
	/** Set if FlashDB's lock callback ever failed.  FlashDB cannot be told about
	 *  it at the time (its callback returns nothing), so the next outer call
	 *  refuses instead. */
	volatile int    lock_broken;
	/*
	 * Scratch for the walk.  Here rather than automatic because a walk needs a
	 * whole record plus its decoded form (~660 B together) and would otherwise
	 * charge that to whichever thread happened to call `kv list`.  Safe unguarded
	 * because the walk only ever runs with the device held, so there is never a
	 * second thread inside it.
	 */
	uint8_t         walk_raw[KV_REC_MAX];
	struct kv_value walk_val;
};

/**
 * The board's constant facts for kv.c.
 *
 * Two holds, because the board may need them to differ: the OUTER hold is taken
 * by every public entry point around the whole operation (and by kv_init() and
 * kv_format() around theirs), the INNER lock is FlashDB's own callback, which
 * FlashDB takes again inside an operation the outer hold already covers.  Both
 * are recursive on the device as far as kv.c is concerned.
 */
struct kv_facts {
	/** The FAL partition the store lives in (a string literal: FlashDB keeps
	 *  the pointer). */
	const char *part_name;
	/** Nonzero when the flash device came up.  Checked before anything else. */
	int  (*device_ready)(void);
	/** Take the outer hold, waiting as long as it takes (kv_init()):
	 *  0, or nonzero when it cannot be had at all. */
	int  (*hold_wait)(void);
	/** Take the outer hold with the board's bounded wait (every other entry
	 *  point): KV_OK, KV_ERR_BUSY when the wait ran out, KV_ERR_STATE when it
	 *  cannot be had.  Any other value is taken as KV_ERR_STATE. */
	int  (*hold_try)(void);
	/** Give back the outer hold taken by either of the two above. */
	void (*hold_release)(void);
	/** FlashDB's lock callback: 0, or nonzero when the lock could not be taken
	 *  (the store then latches itself offline). */
	int  (*db_lock)(void);
	/** FlashDB's unlock callback. */
	void (*db_unlock)(void);
	/** One line for the board's log, at KV_REPORT_* weight. */
	void (*report)(unsigned level, const char *fmt, ...)
		__attribute__((format(printf, 2, 3)));
};

/** The board's two objects (wio-lite-ai: port/flashdb/fal_nor.c; f746g-disco:
 *  port/flashdb/fal_qspi.c). */
extern struct kv_state       kv_board_state;
extern const struct kv_facts kv_board_facts;

#ifdef __cplusplus
}
#endif

#endif /* SVC_KV_BOARD_H */
