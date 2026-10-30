/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for src/fault_plugin.c (issue #159 = #78 Step 4b): a fault whose pc
 * is inside the loaded plugin is attributed ("plugin +offset") and ends in a
 * reset; anywhere else -- no plugin, a pc one past the image, an unreadable
 * frame -- keeps this board's dump-then-halt.  Under a debugger a plugin fault
 * halts instead of resetting.
 *
 * A plugin cannot be made to fault on demand on the board, so this is where the
 * decision is seen.  The REAL svc/plugin/plugin_run_core.c and plugin_exec.c
 * answer the attribution, over a loader state this test publishes by hand (the
 * shape plugin_exec_load() leaves after a successful load).
 */
#include "fault_plugin.h"
#include "plugin_run_core.h"
#include "plugin_lease_api.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, what)                                                     \
	do {                                                                      \
		if (cond) {                                                           \
			printf("  ok   %s\n", (what));                                    \
		} else {                                                              \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);       \
			fails++;                                                          \
		}                                                                     \
	} while (0)

/* ---- the board's binding, stubbed ----------------------------------------- */

static uint8_t res[1024];

int plugin_run_mpu_ok(uint32_t lo, uint32_t hi, const char **why)
{
	(void)lo; (void)hi; (void)why;
	return -1;
}

void plugin_run_log(int error, const char *text)
{
	(void)error; (void)text;
}

void plugin_run_note_entry(uintptr_t sp)
{
	(void)sp;
}

int plugin_lease_held(void)
{
	return 1;
}

void plugin_lease_note_unheld(void)
{
}

static struct plugin_exec_state state;
static const struct plugin_exec_port port = PLUGIN_RUN_PORT(NULL);

const struct plugin_exec_env plugin_run_env = {
	.state  = &state,
	.port   = &port,
	.res_lo = res,
	.res_hi = res + sizeof res,
};

#define BASE 0x20048000u
#define LEN  0x1000u

static void publish(int on)
{
	memset(&state, 0, sizeof state);
	if (!on)
		return;
	state.slot.base = BASE;
	state.slot.len  = LEN;
	strcpy(state.slot.name, "blazeface");
	state.active = &state.slot;
}

int main(void)
{
	const char *who;
	uint32_t off;

	printf("test_fault_plugin\n");

	publish(0);
	who = "x";
	CHECK(fault_plugin_end(BASE + 4u, 1, 0, &who, &off) == FAULT_END_DUMP_HALT &&
	      who == NULL, "no plugin loaded: today's dump-and-halt, nothing named");

	publish(1);
	off = 0u;
	CHECK(fault_plugin_end(BASE + 0x123u, 1, 0, &who, &off) == FAULT_END_RESET &&
	      who != NULL && strcmp(who, "blazeface") == 0 && off == 0x123u,
	      "[!] a pc inside the plugin: named with its offset, and reset");
	CHECK(fault_plugin_end(BASE, 1, 0, &who, &off) == FAULT_END_RESET &&
	      off == 0u, "...its first byte too");
	CHECK(fault_plugin_end(BASE + LEN - 1u, 1, 0, &who, &off) ==
	      FAULT_END_RESET, "...and its last");
	CHECK(fault_plugin_end(BASE + LEN, 1, 0, &who, &off) ==
	      FAULT_END_DUMP_HALT && who == NULL,
	      "one past the image is not the plugin's");
	CHECK(fault_plugin_end(BASE - 2u, 1, 0, &who, &off) ==
	      FAULT_END_DUMP_HALT && who == NULL,
	      "nor is the firmware below it (`crash` behaves as today)");
	CHECK(fault_plugin_end(BASE + 4u, 0, 0, &who, &off) ==
	      FAULT_END_DUMP_HALT && who == NULL,
	      "an unreadable frame attributes nothing");
	CHECK(fault_plugin_end(BASE + 4u, 1, 1, &who, &off) ==
	      FAULT_END_DEBUG_HALT && who != NULL,
	      "under a debugger, a plugin fault is named and halts for SWD");
	CHECK(fault_plugin_end(BASE + 4u, 1, 0, NULL, NULL) == FAULT_END_RESET,
	      "NULL outputs are allowed");

	if (fails) {
		printf("test_fault_plugin: %d FAILURE(S)\n", fails);
		return 1;
	}
	printf("test_fault_plugin: all passed\n");
	return 0;
}
