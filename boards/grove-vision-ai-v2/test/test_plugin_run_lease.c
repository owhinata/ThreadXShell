/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the loader's two plugin-lease checks (issue #127,
 * port/plugin/plugin_run.c): plugin_run_load() refuses a caller that does not
 * hold the lease before the shared loader touches anything, and the exec_ok
 * hook asks again immediately before entry().
 *
 * WHY THIS EXISTS.  A load is the one path into the plugin that destroys it:
 * the shared loader unpublishes the previous plugin and copies the new image
 * over the reservation it ran from.  Without the lease, that could happen under
 * a decode.  No console can aim a load at that window, and a correct build
 * never reaches either refusal -- so the only place they are ever seen to say no
 * is here.
 *
 * The REAL plugin_run.c and svc/plugin_exec.c are compiled; the device, ThreadX
 * and the NOR lease are stubs (test/plugin_run_shim).  The MPU is configured to
 * REFUSE in every case, so a load that passes both lease checks ends at
 * PLUGIN_RUN_MPU and never branches into the image -- which on a host would be
 * a jump into data.  "It got as far as the MPU" is what proves the lease let it
 * through; "the reservation is untouched" is what proves a refusal came first.
 */
#include "plugin_run.h"
#include "plugin_lease.h"
#include "plugin_mpu.h"
#include "nn_probe.h"
#include "npu_hw.h"
#include "nor_flash.h"
#include "tx_api.h"
#include "WE2_device.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- the reservation, as the linker script would place it ------------------ */

/* One block with a symbol at each end: two C arrays would not be adjacent. */
__asm__(".data\n"
        ".balign 64\n"
        ".globl __plugin_start\n"
        "__plugin_start:\n"
        ".space 1024\n"
        ".globl __plugin_end\n"
        "__plugin_end:\n");
extern uint8_t __plugin_start[], __plugin_end[];

/* ---- stubs ------------------------------------------------------------------ */

MPU_Type test_mpu;

static TX_THREAD self_thread;

TX_THREAD *tx_thread_identify(void)
{
	return &self_thread;
}

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level;
	(void)tag;
	(void)fmt;
}

void npu_cache_clean(const void *p, size_t len)
{
	(void)p;
	(void)len;
}

int nor_lease_held(uint32_t token)
{
	(void)token;
	return 1;
}

static unsigned probe_arms, probe_notes;

void nn_probe_pending_arm(struct nn_probe_pending *p, const void *who)
{
	(void)p;
	(void)who;
	probe_arms++;
}

void nn_probe_pending_take(struct nn_probe_pending *p, const void *who,
                           uintptr_t sp)
{
	(void)p;
	(void)who;
	(void)sp;
}

enum nn_probe_settle nn_probe_pending_settle(struct nn_probe_pending *p,
                                             int entered, uintptr_t *sp)
{
	(void)p;
	(void)entered;
	(void)sp;
	return NN_PROBE_SETTLE_NONE;
}

void nn_probe_note(unsigned slot, uintptr_t sp, uint32_t extra)
{
	(void)slot;
	(void)sp;
	(void)extra;
	probe_notes++;
}

void nn_probe_discard(unsigned slot)
{
	(void)slot;
}

/*
 * The lease.  Who holds it is a variable; a script, when set, answers the next
 * few questions in order -- which is how a case makes the first check pass and
 * the hook's fail, standing in for a path that reached the loader some other
 * way.
 */
enum lease_owner { LEASE_NONE, LEASE_OTHER, LEASE_SELF };

static enum lease_owner lease_owner;
static const int       *held_script;
static unsigned         held_script_n, held_asked;
static unsigned         unheld;

int plugin_lease_held(void)
{
	unsigned i = held_asked++;

	if (held_script != NULL && i < held_script_n)
		return held_script[i];
	return lease_owner == LEASE_SELF;
}

void plugin_lease_note_unheld(void)
{
	unheld++;
}

/* ---- reporting ------------------------------------------------------------ */

static int failures;

static void expect(const char *what, int cond, const char *fmt, ...)
{
	va_list ap;

	if (cond) {
		printf("  ok   %s\n", what);
		return;
	}
	printf("  FAIL %s: ", what);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

/* ---- a container ------------------------------------------------------------ */

#define IMAGE_LEN 64u
#define RES_FILL  0xA5u
#define IMG_FILL  0x3Cu

static uint8_t container[256];
static struct plugin_view view;
static struct plugin_base_api base;

static void setup(void)
{
	memset(container, IMG_FILL, sizeof container);
	memset(&view, 0, sizeof view);
	view.has_plugin = 1u;
	view.image_off  = 16u;
	view.file_size  = IMAGE_LEN;
	view.mem_size   = IMAGE_LEN;
	view.link_addr  = (uint32_t)(uintptr_t)__plugin_start;
	memset(__plugin_start, RES_FILL, (size_t)(__plugin_end - __plugin_start));

	/* An MPU that refuses: enabled, no default map, no regions. */
	memset(&test_mpu, 0, sizeof test_mpu);
	test_mpu.CTRL = PLUGIN_MPU_CTRL_ENABLE;
	test_mpu.TYPE = 0u;

	base.version = PLUGIN_ABI_VERSION;
	base.size    = (uint32_t)sizeof base;

	held_script   = NULL;
	held_script_n = 0u;
	held_asked    = 0u;
	probe_arms    = 0u;
	probe_notes   = 0u;
}

static int reservation_untouched(void)
{
	const uint8_t *p;

	for (p = __plugin_start; p < __plugin_end; p++)
		if (*p != RES_FILL)
			return 0;
	return 1;
}

static int image_copied(void)
{
	unsigned i;

	for (i = 0u; i < IMAGE_LEN; i++)
		if (__plugin_start[i] != IMG_FILL)
			return 0;
	return 1;
}

static enum plugin_run_result load(void)
{
	return plugin_run_load(&view, container, 0u, &base);
}

int main(void)
{
	static const enum lease_owner not_held[2] = { LEASE_NONE, LEASE_OTHER };
	static const int first_yes_then_no[2] = { 1, 0 };
	enum plugin_run_result r;
	unsigned o, u0;

	printf("test_plugin_run_lease\n");

	/* Nobody holds it, or another thread does: refused before anything. */
	for (o = 0u; o < 2u; o++) {
		setup();
		lease_owner = not_held[o];
		u0 = unheld;
		r = load();
		expect("a load without the lease is NOT_HELD", r == PLUGIN_RUN_NOT_HELD,
		       "%s: got %d", o ? "another thread holds it" : "nobody holds it",
		       (int)r);
		expect("[!] and the reservation was not touched",
		       reservation_untouched(), "%s: the image was copied over it",
		       o ? "other" : "none");
		expect("  ...the loader was not even armed",
		       probe_arms == 0u, "%u arm(s)", probe_arms);
		expect("  ...and the refusal was counted once",
		       unheld == u0 + 1u, "%u", unheld - u0);
		expect("  ...and has a name of its own",
		       strcmp(plugin_run_why(r), plugin_run_strerror(PLUGIN_RUN_MPU)) != 0 &&
		       strstr(plugin_run_why(r), "lease") != NULL,
		       "'%s'", plugin_run_why(r));
	}

	/* The caller holds it: both checks pass and the load goes on to the MPU,
	 * which this case configured to refuse. */
	setup();
	lease_owner = LEASE_SELF;
	u0 = unheld;
	r = load();
	expect("held, the load passes both lease checks and reaches the MPU",
	       r == PLUGIN_RUN_MPU, "got %d", (int)r);
	expect("  ...having copied the image", image_copied(), "nothing copied");
	expect("  ...with nothing counted", unheld == u0, "%u", unheld - u0);
	expect("  ...and the lease asked twice: before the loader, at the branch",
	       held_asked == 2u, "%u", held_asked);

	/* [!] The hook asks again.  A caller that got past the first check but
	 * does not hold the lease at the branch is refused there -- by name, not
	 * as the MPU refusal the shared loader turns a hook's "no" into. */
	setup();
	lease_owner   = LEASE_SELF;
	held_script   = first_yes_then_no;
	held_script_n = 2u;
	u0 = unheld;
	r = load();
	expect("[!] refused at the branch, the load is NOT_HELD, not MPU",
	       r == PLUGIN_RUN_NOT_HELD, "got %d", (int)r);
	expect("  ...and counted once", unheld == u0 + 1u, "%u", unheld - u0);
	expect("  ...and nothing is active", plugin_run_active() == 0, "active");

	/* And the hook's flag does not outlive its load: a held load after it is
	 * the MPU's refusal again. */
	setup();
	lease_owner = LEASE_SELF;
	r = load();
	expect("the next held load is judged on its own", r == PLUGIN_RUN_MPU,
	       "got %d", (int)r);

	if (failures) {
		printf("test_plugin_run_lease: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_run_lease: all passed\n");
	return 0;
}
