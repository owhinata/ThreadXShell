/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the shared plugin loader (svc/plugin_exec.c, issue #110).
 *
 * WHAT THIS CHECKS, AND WHY IT IS NOT A DEVICE TEST.  What the loader must get
 * right is an ORDER -- unload before deciding whether there is anything to
 * load, copy before cache maintenance, caches before the executability check,
 * publish before the branch, unpublish before the slot is touched again -- and
 * every one of those is invisible from the console.  A board can only show that
 * a plugin ran; it cannot show that the fault reporter would have named it had
 * a fault arrived between two particular instructions.  So the port hooks are
 * the observation points: each one records when it was called and what the
 * world looked like from there.
 *
 * [!] THE ENTRY POINT IS REALLY BRANCHED TO.  The alternative -- an "entry
 * absent" manifest -- exercises a path plugin_parse() refuses to produce, and
 * would leave the publish-before-branch rule checked by nothing.  The image
 * this test copies in is twelve bytes of position-independent machine code that
 * jumps to a C function here, so the branch is the real one, the argument
 * arrives, and the callee can look back at what the loader published.  That
 * makes the test host-specific; on any other host it SKIPs rather than
 * pretending, and the ordering cases above still run everywhere.
 */
#define _GNU_SOURCE
#include "plugin_exec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#define HAVE_EXEC_RESERVATION 1
#else
#define HAVE_EXEC_RESERVATION 0
#endif

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

/* ---- the reservation ----------------------------------------------------- */

#define RES_BYTES 4096u

static uint8_t *res_lo;
static uint8_t *res_hi;

static struct plugin_exec_state st;

/* ---- what the hooks saw -------------------------------------------------- */

static struct {
	int      seq;                  /* next sequence number to hand out      */
	int      source_at, caches_at, exec_at, entry_at;
	int      published_at_source;  /* was a plugin still published there?   */
	int      published_at_exec;
	int      published_at_entry;
	int      copied_at_caches;     /* had the image landed by then?         */
	int      copied_at_exec;
	uint32_t caches_base, caches_len;
	int      source_refuse, exec_refuse, entry_refuse;
} saw;

static struct plugin_exec_env env;   /* defined below; the hooks need it     */

static int published(void)
{
	/* Any address inside the reservation will do: what is being asked is
	 * whether the fault reporter would name something right now. */
	uint32_t off = 0u;

	return plugin_exec_attribute(&env, (uint32_t)(uintptr_t)res_lo + 4u,
	                             &off) != NULL;
}

static int image_landed(void)
{
	return res_lo[0] != 0u;
}

static int hook_source(const void *container, uintptr_t token, const char **why)
{
	(void)container; (void)token;
	saw.source_at = ++saw.seq;
	saw.published_at_source = published();
	if (saw.source_refuse) {
		if (why != NULL)
			*why = "the test refused it";
		return -1;
	}
	return 0;
}

static void hook_caches(uint32_t base, uint32_t len)
{
	saw.caches_at = ++saw.seq;
	saw.caches_base = base;
	saw.caches_len = len;
	saw.copied_at_caches = image_landed();
}

static int hook_exec(uint32_t lo, uint32_t hi, const char **why)
{
	(void)lo; (void)hi;
	saw.exec_at = ++saw.seq;
	saw.copied_at_exec = image_landed();
	saw.published_at_exec = published();
	if (saw.exec_refuse) {
		if (why != NULL)
			*why = "the test refused it";
		return -1;
	}
	return 0;
}

static const struct plugin_exec_port port = {
	.sync_caches = hook_caches,
	.exec_ok     = hook_exec,
	.source_ok   = hook_source,
};

/*
 * [!] NOT const HERE, THOUGH A BOARD'S IS.  A board states its reservation with
 * link-time constants and keeps the binding in .rodata, which is what makes it
 * safe to reach from a fault handler.  This test has to choose the reservation
 * at run time, and casting const away to write it is how the first draft of
 * this file earned a segmentation fault.
 */
static struct plugin_exec_env env = {
	.state  = &st,
	.port   = &port,
	.res_lo = NULL,       /* filled in by main() */
	.res_hi = NULL,
};

/* ---- the container ------------------------------------------------------- */

/*
 * A "container" is just bytes here: plugin_load.c has already validated the one
 * the device gets, and this file is testing what happens afterwards.  The view
 * is therefore built by hand, which is also the only way to reach states
 * plugin_parse() refuses to emit.
 */
#define IMAGE_OFF   64u
/*
 * Where the stub goes inside the image.  EVEN, and that is a host constraint
 * rather than a claim: on the target a slot offset carries the Thumb bit and
 * plugin_load.c refuses one that does not, so the loader passes it through
 * untouched -- nothing here checks that, and an odd offset would simply be an
 * unaligned x86 branch.
 */
#define ENTRY_OFF   16u

static uint8_t container[512];
static struct plugin_view view;

static int entry_called;
static int entry_saw_published;
static const struct plugin_base_api *entry_saw_base;

static int test_entry(const struct plugin_base_api *base)
{
	entry_called++;
	entry_saw_base = base;
	entry_saw_published = published();
	saw.entry_at = ++saw.seq;
	saw.published_at_entry = entry_saw_published;
	return saw.entry_refuse ? 1 : 0;
}

#if HAVE_EXEC_RESERVATION
/* movabs rax, <test_entry>; jmp rax -- position independent, calls nothing. */
static void put_entry_stub(uint8_t *at, void *target)
{
	uint64_t t = (uint64_t)(uintptr_t)target;

	at[0] = 0x48; at[1] = 0xB8;            /* movabs rax, imm64 */
	memcpy(at + 2, &t, sizeof t);
	at[10] = 0xFF; at[11] = 0xE0;          /* jmp rax           */
}
#endif

static const struct plugin_base_api base_api = {
	.version = PLUGIN_ABI_VERSION,
	.size    = sizeof(struct plugin_base_api),
};

static void reset(int with_entry)
{
	memset(&saw, 0, sizeof saw);
	memset(container, 0, sizeof container);
	memset(&view, 0, sizeof view);
	memset(res_lo, 0, RES_BYTES);
	entry_called = 0;
	entry_saw_published = -1;
	entry_saw_base = NULL;

	view.has_plugin = 1u;
	view.image_off  = IMAGE_OFF;
	view.file_size  = 32u;
	view.mem_size   = 64u;
	view.link_addr  = (uint32_t)(uintptr_t)res_lo;
	memcpy(view.name, "tester", 7);
	memcpy(view.build_id, "abc1234", 8);
	for (unsigned i = 0u; i < (unsigned)PLUGIN_SLOT_COUNT; i++)
		view.slot[i] = PLUGIN_SLOT_ABSENT;
	if (with_entry)
		view.slot[PLUGIN_SLOT_ENTRY] = ENTRY_OFF;

	/* A recognisable first byte, so image_landed() means what it says. */
	container[IMAGE_OFF] = 0xA5u;
#if HAVE_EXEC_RESERVATION
	put_entry_stub(container + IMAGE_OFF + ENTRY_OFF, (void *)test_entry);
#endif
}

/* ---- cases --------------------------------------------------------------- */

static void test_arguments(void)
{
	printf("arguments and an empty binding:\n");
	CHECK(plugin_exec_load(NULL, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_ARG,
	      "a null binding is refused");
	CHECK(plugin_exec_load(&env, NULL, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_ARG,
	      "a null view is refused");
	CHECK(plugin_exec_load(&env, &view, NULL, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_ARG,
	      "a null container is refused");
	CHECK(plugin_exec_load(&env, &view, container, 0u, NULL, NULL) ==
	              PLUGIN_RUN_ARG,
	      "a null base vtable is refused -- the plugin would be handed it");
	CHECK(plugin_exec_active(&env) == 0 && plugin_exec_slot(&env, 0u) == NULL,
	      "nothing is active and no slot resolves after a refusal");
	CHECK(plugin_exec_attribute(&env, (uint32_t)(uintptr_t)res_lo, NULL) ==
	              NULL,
	      "the fault reporter names nothing");
}

static void test_refusals(void)
{
	const char *why;

	printf("refusals, and what each one leaves behind:\n");

	reset(1);
	view.has_plugin = 0u;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_NO_PLUGIN,
	      "a container carrying only a model is NO_PLUGIN, not an error");
	CHECK(saw.source_at == 0 && saw.caches_at == 0,
	      "and nothing was read or copied for it");

	reset(1);
	view.mem_size = RES_BYTES + 1u;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_TOO_BIG,
	      "an image larger than the reservation is refused");
	CHECK(!image_landed(), "and nothing was copied into it");

	reset(1);
	view.link_addr = (uint32_t)(uintptr_t)res_lo + 16u;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_TOO_BIG,
	      "an image prelinked at another address is refused");
	CHECK(!image_landed(), "and nothing was copied into it");

	reset(1);
	saw.source_refuse = 1;
	why = NULL;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, &why) ==
	              PLUGIN_RUN_NO_SOURCE,
	      "an unpinned source is refused");
	CHECK(why != NULL && strcmp(why, "the test refused it") == 0,
	      "and the port's reason is returned, not logged");
	CHECK(!image_landed(), "and nothing was copied before the source was ok");

	reset(1);
	saw.exec_refuse = 1;
	why = NULL;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, &why) ==
	              PLUGIN_RUN_MPU,
	      "a reservation that is not executable is refused");
	CHECK(why != NULL && strcmp(why, "the test refused it") == 0,
	      "and that port's reason is returned too");
	CHECK(plugin_exec_active(&env) == 0 && !published(),
	      "a refusal after the copy still publishes nothing");

	reset(0);              /* no ENTRY slot */
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_ENTRY,
	      "a manifest with no entry point is refused rather than branched to 0");
	CHECK(plugin_exec_active(&env) == 0 && !published(),
	      "and nothing is left published");
}

static void test_order(void)
{
	printf("the order the hooks are called in:\n");

	reset(1);
	saw.exec_refuse = 1;   /* stop before the branch; the order so far is the point */
	(void)plugin_exec_load(&env, &view, container, 0u, &base_api, NULL);

	CHECK(saw.source_at == 1 && saw.caches_at == 2 && saw.exec_at == 3,
	      "source, then caches, then executability");
	CHECK(saw.copied_at_caches == 1,
	      "the image had landed before cache maintenance");
	CHECK(saw.copied_at_exec == 1,
	      "and was still there when executability was judged");
	CHECK(saw.published_at_exec == 0,
	      "[!] nothing is published before the reservation is judged executable");
	CHECK(saw.caches_base == (uint32_t)(uintptr_t)res_lo &&
	              saw.caches_len == RES_BYTES,
	      "[!] the WHOLE reservation is maintained, not just the image");
}

static void test_unload_precedes_the_has_plugin_test(void)
{
	printf("[!] the previous plugin is dropped before the new request is judged:\n");

#if HAVE_EXEC_RESERVATION
	reset(1);
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_OK,
	      "a plugin is loaded");
	CHECK(plugin_exec_active(&env) == 1, "and is active");

	/* Now a container with no plugin at all.  The previous one must be gone
	 * before NO_PLUGIN is returned -- a caller reading that as success would
	 * otherwise decode a new model with an old model's decoder. */
	memset(&saw, 0, sizeof saw);
	view.has_plugin = 0u;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_NO_PLUGIN,
	      "a model-only container returns NO_PLUGIN");
	CHECK(plugin_exec_active(&env) == 0 && !published(),
	      "[!] and the PREVIOUS plugin is no longer active or attributable");

	reset(1);
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_OK,
	      "a plugin is loaded again");
	memset(&saw, 0, sizeof saw);
	saw.source_refuse = 1;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_NO_SOURCE,
	      "the next load is refused at the source check");
	CHECK(saw.published_at_source == 0,
	      "[!] and the previous plugin was already unpublished when it was asked");
	CHECK(plugin_exec_active(&env) == 0,
	      "a refused replacement leaves nothing active, not the old one");
#else
	printf("  SKIP no executable 32-bit-reachable reservation on this host\n");
#endif
}

static void test_the_branch(void)
{
#if HAVE_EXEC_RESERVATION
	uint32_t off = 0xFFFFFFFFu;
	const char *who;

	printf("the branch, and what the fault reporter sees across it:\n");

	reset(1);
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_OK,
	      "a well formed plugin loads");
	CHECK(entry_called == 1, "its entry point was really called");
	CHECK(entry_saw_base == &base_api,
	      "and was handed the base vtable it was given");
	CHECK(saw.published_at_entry == 1,
	      "[!] the plugin was ALREADY published when entry ran -- a fault "
	      "inside it names it");
	CHECK(saw.entry_at == 4,
	      "and the branch came after all three hooks");
	CHECK(plugin_exec_active(&env) == 1, "it is active afterwards");
	CHECK(plugin_exec_slot(&env, PLUGIN_SLOT_ENTRY) ==
	              (void *)(res_lo + ENTRY_OFF),
	      "its entry slot resolves to the reservation");
	CHECK(plugin_exec_slot(&env, PLUGIN_SLOT_DECODE) == NULL,
	      "an absent slot resolves to nothing");
	CHECK(plugin_exec_slot(&env, PLUGIN_SLOT_COUNT) == NULL,
	      "and so does an out-of-range one");

	who = plugin_exec_attribute(&env, (uint32_t)(uintptr_t)res_lo + 8u, &off);
	CHECK(who != NULL && strcmp(who, "tester") == 0 && off == 8u,
	      "an address inside the image is attributed, with its offset");
	CHECK(plugin_exec_attribute(&env,
	                            (uint32_t)(uintptr_t)res_lo + view.mem_size,
	                            NULL) == NULL,
	      "one past the image is not -- the range is half-open");

	plugin_exec_unload(&env);
	CHECK(plugin_exec_active(&env) == 0 && !published() &&
	              plugin_exec_slot(&env, PLUGIN_SLOT_ENTRY) == NULL,
	      "unloading forgets all three");
	plugin_exec_unload(&env);
	CHECK(plugin_exec_active(&env) == 0, "and unloading nothing is not an error");

	reset(1);
	saw.entry_refuse = 1;
	CHECK(plugin_exec_load(&env, &view, container, 0u, &base_api, NULL) ==
	              PLUGIN_RUN_ENTRY,
	      "a plugin that refuses its own entry point is refused");
	CHECK(entry_called == 1, "it did run");
	CHECK(plugin_exec_active(&env) == 0 && !published() &&
	              plugin_exec_slot(&env, PLUGIN_SLOT_ENTRY) == NULL,
	      "[!] and is left neither active nor attributable");
#else
	printf("the branch:\n  SKIP no executable 32-bit-reachable reservation\n");
#endif
}

static void test_bss_is_zeroed(void)
{
	printf("the part of the image that has no initialiser:\n");

	reset(1);
	memset(res_lo, 0x5Au, RES_BYTES);
	saw.exec_refuse = 1;          /* stop before the branch; the copy is the point */
	(void)plugin_exec_load(&env, &view, container, 0u, &base_api, NULL);

	CHECK(res_lo[0] == 0xA5u, "the file part was copied");
	CHECK(res_lo[view.file_size] == 0u &&
	              res_lo[view.mem_size - 1u] == 0u,
	      "everything from file_size to mem_size was zeroed");
	CHECK(res_lo[view.mem_size] == 0x5Au,
	      "and nothing past mem_size was touched");
}

int main(void)
{
#if HAVE_EXEC_RESERVATION
	/*
	 * MAP_32BIT so that the reservation's address fits the 32-bit fields the
	 * wire format uses -- the manifest's link_addr and the fault reporter's
	 * base are device addresses and stay uint32_t.  PROT_EXEC because the
	 * whole point of the branch cases is that the branch is real.
	 */
	res_lo = mmap(NULL, RES_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
	              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (res_lo == MAP_FAILED) {
		printf("test_plugin_exec: MAP_32BIT reservation unavailable; "
		       "running the ordering cases only\n");
		static uint8_t fallback[RES_BYTES];
		res_lo = fallback;
	}
#else
	static uint8_t fallback[RES_BYTES];
	res_lo = fallback;
#endif
	res_hi = res_lo + RES_BYTES;

	env.res_lo = res_lo;
	env.res_hi = res_hi;

	printf("test_plugin_exec (svc/plugin_exec.c):\n");
	test_arguments();
	test_refusals();
	test_order();
	test_bss_is_zeroed();
	test_unload_precedes_the_has_plugin_test();
	test_the_branch();

	if (fails != 0) {
		printf("test_plugin_exec: %d FAILED\n", fails);
		return 1;
	}
	printf("test_plugin_exec: all passed\n");
	return 0;
}
