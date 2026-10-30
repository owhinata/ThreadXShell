/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the loader's plugin-lease checks (issue #130), shared since
 * issue #159 (svc/plugin/plugin_run_core.c, used by wio-lite-ai and
 * f746g-disco): plugin_run_load() refuses a caller that does not hold the lease
 * before the shared loader touches anything, the exec_ok hook asks again
 * immediately before entry(), and plugin_run_unload() refuses too.  The same
 * three checks, and the same shape of test, as grove-vision-ai-v2's since issue
 * #127 -- where the review found the unload was the one entry left unchecked.
 *
 * WHY THIS EXISTS.  A load is the one path into the plugin that destroys it:
 * the shared loader unpublishes the previous plugin and copies the new image
 * over the reservation it ran from.  Without the lease, that could happen under
 * a decode or a draw.  No console can aim a load at that window, and a correct
 * build never reaches either refusal -- so the only place they are ever seen to
 * say no is here.
 *
 * The REAL plugin_run_core.c and svc/plugin/plugin_exec.c are compiled; the
 * board's binding -- its MPU verdict, its cache maintenance, its log, its depth
 * record -- is stubbed here.  The MPU REFUSES in every case, so a load that
 * passes both lease checks ends at PLUGIN_RUN_MPU and never branches into the
 * image -- which on a host would be a jump into data.  "It got as far as the
 * MPU" is what proves the lease let it through; "the reservation is untouched"
 * is what proves a refusal came first.  (Until issue #159 this file was
 * wio-lite-ai's and drove that board's binding through a HAL shim; the binding
 * is now the hardware half only, and its verdict is plugin_mpu_v7m.c's, tested
 * on its own and on each board's real region set.)
 *
 * plugin_run_unload() is checked through -Wl,--wrap on the shared loader's
 * unload: with no plugin publishable on a host, "it did not unpublish" can only
 * be observed as "it did not call".
 */
#include "plugin_run_core.h"
#include "plugin_lease_api.h"

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

/* ---- the board's binding, stubbed ----------------------------------------- */

static unsigned mpu_asks, syncs, log_errors;

/* An MPU that refuses. */
int plugin_run_mpu_ok(uint32_t lo, uint32_t hi, const char **why)
{
	(void)lo;
	(void)hi;
	mpu_asks++;
	if (why != NULL)
		*why = "the test's MPU refuses";
	return -1;
}

static void sync_caches(uint32_t base, uint32_t len)
{
	(void)base;
	(void)len;
	syncs++;
}

void plugin_run_log(int error, const char *text)
{
	(void)text;
	if (error)
		log_errors++;
}

static struct plugin_exec_state state;
static const struct plugin_exec_port port = PLUGIN_RUN_PORT(sync_caches);

const struct plugin_exec_env plugin_run_env = {
	.state  = &state,
	.port   = &port,
	.res_lo = __plugin_start,
	.res_hi = __plugin_end,
};

/* ---- stubs ------------------------------------------------------------------ */

static unsigned entry_notes;

void plugin_run_note_entry(uintptr_t sp)
{
	(void)sp;
	entry_notes++;
}

/* The shared loader's unload, seen from plugin_run_core.c (-Wl,--wrap).  Calls made
 * inside plugin_exec.c itself -- the unload at the start of every load -- do
 * not come through here. */
static unsigned exec_unloads;

void __real_plugin_exec_unload(const struct plugin_exec_env *env);

void __wrap_plugin_exec_unload(const struct plugin_exec_env *env)
{
	exec_unloads++;
	__real_plugin_exec_unload(env);
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

	mpu_asks = 0u;
	syncs    = 0u;
	base.version = PLUGIN_ABI_VERSION;
	base.size    = (uint32_t)sizeof base;

	held_script   = NULL;
	held_script_n = 0u;
	held_asked    = 0u;
	entry_notes   = 0u;
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

/* The container is its own staging region here: the source check wants the
 * bytes inside the region the caller names, and that is what the board's
 * caller passes too. */
static enum plugin_run_result load(void)
{
	return plugin_run_load(&view, container, container,
	                       (uint32_t)sizeof container, &base);
}

/* What this file walks without the lease; cmake/check_lease_entries.py holds
 * it against the functions plugin_run_core.c defines. */
/* LEASE ENTRIES BEGIN */
static const char *const walked[] = { "plugin_run_load", "plugin_run_unload",
                                      "plugin_run_exec_ok" };
/* LEASE ENTRIES END */

int main(void)
{
	static const enum lease_owner not_held[2] = { LEASE_NONE, LEASE_OTHER };
	static const int first_yes_then_no[2] = { 1, 0 };
	enum plugin_run_result r;
	unsigned o, u0;

	printf("test_plugin_run_lease: %s, %s, %s\n", walked[0], walked[1],
	       walked[2]);

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
		expect("  ...the hook was not even reached",
		       entry_notes == 0u, "%u sample(s)", entry_notes);
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
	expect("  ...the caches synced once and the MPU asked once",
	       syncs == 1u && mpu_asks == 1u, "%u / %u", syncs, mpu_asks);

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
	expect("  ...and the MPU was never asked", mpu_asks == 0u, "%u", mpu_asks);

	/* And the hook's flag does not outlive its load: a held load after it is
	 * the MPU's refusal again. */
	setup();
	lease_owner = LEASE_SELF;
	r = load();
	expect("the next held load is judged on its own", r == PLUGIN_RUN_MPU,
	       "got %d", (int)r);

	/* [!] An unload without the lease does nothing and is counted once: it
	 * would clear the slot table a holder may be calling through. */
	for (o = 0u; o < 2u; o++) {
		setup();
		lease_owner  = not_held[o];
		exec_unloads = 0u;
		u0 = unheld;
		plugin_run_unload();
		expect("[!] an unload without the lease does not reach the loader",
		       exec_unloads == 0u, "%s: %u call(s)",
		       o ? "another thread holds it" : "nobody holds it",
		       exec_unloads);
		expect("  ...and is counted once", unheld == u0 + 1u, "%u",
		       unheld - u0);
	}
	setup();
	lease_owner  = LEASE_SELF;
	exec_unloads = 0u;
	u0 = unheld;
	plugin_run_unload();
	expect("held, the unload reaches the loader once", exec_unloads == 1u,
	       "%u call(s)", exec_unloads);
	expect("  ...with nothing counted", unheld == u0, "%u", unheld - u0);

	/* [!] The hook itself, called directly without the lease: refused with the
	 * one reason plugin_run_load() recognises, counted once, the MPU never
	 * asked -- and with it held, the MPU's verdict is the answer. */
	for (o = 0u; o < 2u; o++) {
		const char *why = NULL;
		int rc;

		setup();
		lease_owner = not_held[o];
		u0 = unheld;
		rc = plugin_run_exec_ok(0u, 32u, &why);
		expect("[!] the exec_ok hook without the lease refuses",
		       rc != 0 && why != NULL && strstr(why, "lease") != NULL,
		       "%s: rc %d", o ? "other" : "none", rc);
		expect("  ...counted once, the MPU not asked, the depth noted",
		       unheld == u0 + 1u && mpu_asks == 0u && entry_notes == 1u,
		       "%u / %u / %u", unheld - u0, mpu_asks, entry_notes);
	}
	{
		const char *why = NULL;

		setup();
		lease_owner = LEASE_SELF;
		expect("held, the hook answers with the MPU's verdict",
		       plugin_run_exec_ok(0u, 32u, &why) != 0 && mpu_asks == 1u &&
		       why != NULL && strstr(why, "MPU") != NULL, "%s",
		       why != NULL ? why : "(none)");
	}

	if (failures) {
		printf("test_plugin_run_lease: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_run_lease: all passed\n");
	return 0;
}
