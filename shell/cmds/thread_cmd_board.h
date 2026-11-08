/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    thread_cmd_board.h
 * @brief   The state of the shared `thread` command, which the board defines
 *          (issue #206).
 *
 * shell/cmds/cmd_thread.c is one command compiled by every board, and like
 * every shared translation unit it owns no mutable storage
 * (cmake/check_no_mutable_storage.py).  What it has to remember between runs
 * -- the cpu% baseline of the previous run and the guard that keeps two runs
 * from interleaving -- is therefore one object the board DEFINES, and
 * cmd_thread.c reaches it through the extern declaration below, resolved at
 * link time (the same shape as svc/nn/nn_dispatch_state.h).  Nothing is
 * registered at init: a pointer the shared file remembered would itself be
 * mutable storage in the shared file.
 *
 * The state only exists when the ThreadX Execution Profile Kit is built in
 * (TX_EXECUTION_PROFILE_ENABLE, set by the board's tx_user.h): without it the
 * command is a pure read of ThreadX-owned state and keeps nothing.  The
 * board's definition (boards/<board>/cmds/cmd_thread_board.c) is under the same
 * condition, so a build without the kit defines nothing.
 *
 * Zero is the boot state of every field: the board defines the object without
 * an initializer, as plain .bss.
 *
 * [!] ONLY cmd_thread.c AND THE BOARD'S DEFINITION INCLUDE THIS.
 */
#ifndef THREAD_CMD_BOARD_H
#define THREAD_CMD_BOARD_H

#include "tx_api.h"   /* TX_THREAD, UINT; pulls in the board's tx_user.h */

#ifdef TX_EXECUTION_PROFILE_ENABLE
#include "tx_execution_profile.h"   /* EXECUTION_TIME */

#ifdef __cplusplus
extern "C" {
#endif

/** Threads whose cpu% baseline is kept; later ones print "--". */
#define THREAD_CPU_PREV_MAX 16

/** One thread's busy-time total, keyed by its control block. */
struct thread_cpu_snap {
	TX_THREAD     *t;
	EXECUTION_TIME prev;
};

/**
 * What `thread` remembers between runs.  Every field but `busy` is written
 * only while `busy` is held.
 */
struct thread_cmd_state {
	/* The previous committed run: each sampled thread's total, how many were
	 * sampled, and the kit's global totals at that instant. */
	struct thread_cpu_snap prev[THREAD_CPU_PREV_MAX];
	EXECUTION_TIME         prev_thread, prev_isr, prev_idle;
	UINT                   prev_count;

	/* 0 until the first run commits a baseline. */
	int baseline_committed;

	/* Single-run guard, taken and tested under TX_DISABLE. */
	volatile UINT busy;
};

/** The board's one instance (boards/<board>/cmds/cmd_thread_board.c). */
extern struct thread_cmd_state thread_cmd_board_state;

#ifdef __cplusplus
}
#endif

#endif /* TX_EXECUTION_PROFILE_ENABLE */

#endif /* THREAD_CMD_BOARD_H */
