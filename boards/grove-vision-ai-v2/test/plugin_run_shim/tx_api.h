/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for ThreadX, for test_plugin_run_lease.c only (issue #127):
 * port/plugin/plugin_run.c asks which thread is running, and nothing else.
 */
#ifndef PLUGIN_RUN_SHIM_TX_API_H
#define PLUGIN_RUN_SHIM_TX_API_H

typedef struct TX_THREAD_STRUCT {
	int unused;
} TX_THREAD;

TX_THREAD *tx_thread_identify(void);

#endif
