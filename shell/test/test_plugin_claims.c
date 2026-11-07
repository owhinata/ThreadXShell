/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for svc/plugin/plugin_claims (issue #159): the open model's
 * container claims and the `nn info` lines that print them, moved out of
 * wio-lite-ai's adapter so f746g-disco takes the same ones.
 *
 * What is pinned: the snapshot rule (an odd transition counter is "a load is in
 * progress", whatever the store holds), and the lines for each state -- none, a
 * model-only container (which still names where it came from: on f746g-disco
 * that line carries the CRC the load printed), a plugin running or only
 * validated -- with the board's own "from" words passed through unchanged.
 */
#include "plugin_claims.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void ok(const char *what, int cond)
{
	if (!cond) {
		printf("  FAIL %s\n", what);
		failures++;
	} else {
		printf("  ok   %s\n", what);
	}
}

static char   out[2048];
static size_t used;
static int    refuse_after = -1;   /* refuse the Nth write (0-based), or -1 */
static int    writes;

static int sink(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	if (refuse_after >= 0 && writes++ >= refuse_after)
		return -1;
	if (used + len < sizeof out) {
		memcpy(out + used, s, len);
		used += len;
		out[used] = '\0';
	}
	return (int)len;
}

static void clear(void)
{
	used = 0u;
	out[0] = '\0';
	writes = 0;
	refuse_after = -1;
}

static unsigned lines(void)
{
	unsigned n = 0u;
	const char *p;

	for (p = out; (p = strstr(p, "\r\n")) != NULL; p += 2)
		n++;
	return n;
}

static void fill(struct plugin_claims *c, int has_plugin)
{
	memset(c, 0, sizeof *c);
	c->view.has_plugin = (uint8_t)has_plugin;
	strcpy(c->view.name, "blazeface");
	strcpy(c->view.build_id, "abc1234");
	c->view.digest    = 0x12345678u;
	c->view.file_size = 4100u;
	c->view.mem_size  = 8900u;
	c->view.link_addr = 0x20048000u;
	strcpy(c->from, "/bf.nnc, 194232 B, file crc32 12E129EE");
}

/* `nn info`'s copy, belonging to the state @p seq -- or carrying no value. */
static struct nn_svc_info copy_;

static const struct nn_svc_info *at(uint32_t seq)
{
	memset(&copy_, 0, sizeof copy_);
	copy_.seq_known = (uint8_t)((seq & 1u) == 0u);
	copy_.seq = seq;
	return &copy_;
}

static const struct nn_svc_info *unknown(uint32_t seq)
{
	memset(&copy_, 0, sizeof copy_);
	copy_.seq = seq;
	return &copy_;
}

static void test_store(void)
{
	struct plugin_claims_store st;
	struct plugin_claims c, got;

	printf("the store and its snapshot:\n");
	memset(&st, 0, sizeof st);
	ok("empty: NONE",
	   plugin_claims_get(&st, 0u, at(0u), &got) == PLUGIN_CLAIMS_NONE);
	fill(&c, 1);
	plugin_claims_set(&st, &c);
	memset(&got, 0, sizeof got);
	ok("set: VALID, a whole copy",
	   plugin_claims_get(&st, 2u, at(2u), &got) == PLUGIN_CLAIMS_VALID &&
	   memcmp(&got, &c, sizeof c) == 0);
	/* [!] THREE REASONS, ONE ANSWER (issue #199): odd, a value other than
	 * the copy's, and a copy with no value are all BUSY, claims stored or
	 * not -- and *out is not touched. */
	memset(&got, 0x5A, sizeof got);
	ok("[!] an odd counter is BUSY even with claims stored",
	   plugin_claims_get(&st, 3u, at(3u), &got) == PLUGIN_CLAIMS_BUSY);
	ok("[!] a counter the copy is not of is BUSY (a load ran since)",
	   plugin_claims_get(&st, 4u, at(2u), &got) == PLUGIN_CLAIMS_BUSY);
	ok("[!] a copy with no value is BUSY, even at the same number",
	   plugin_claims_get(&st, 2u, unknown(2u), &got) == PLUGIN_CLAIMS_BUSY);
	ok("[!] a NULL copy is BUSY",
	   plugin_claims_get(&st, 2u, NULL, &got) == PLUGIN_CLAIMS_BUSY);
	ok("...and none of them wrote the snapshot", got.from[0] == 0x5A);
	plugin_claims_set(&st, NULL);
	ok("cleared: NONE",
	   plugin_claims_get(&st, 4u, at(4u), &got) == PLUGIN_CLAIMS_NONE);
	ok("cleared, and a copy of another state: BUSY, not NONE",
	   plugin_claims_get(&st, 4u, at(2u), &got) == PLUGIN_CLAIMS_BUSY);
}

static void test_lines(void)
{
	struct plugin_claims c;

	printf("the `nn info` lines:\n");
	clear();
	plugin_claims_info(sink, NULL, PLUGIN_CLAIMS_BUSY, NULL, 0,
	                   0x20048000u, 32768u, 640u);
	ok("busy: one line, the model line's words for busy (issue #199)",
	   strcmp(out, "plugin  : -- another nn command is using the model; ask "
	               "again when it returns\r\n") == 0);
	ok("...never \"in progress\"", strstr(out, "progress") == NULL);

	clear();
	plugin_claims_info(sink, NULL, PLUGIN_CLAIMS_NONE, NULL, 0,
	                   0x20048000u, 32768u, 640u);
	ok("none: the reservation, one line", lines() == 1u &&
	   strcmp(out, "plugin  : (none) -- reservation 32768 B at 0x20048000\r\n")
	   == 0);

	fill(&c, 0);
	clear();
	plugin_claims_info(sink, NULL, PLUGIN_CLAIMS_VALID, &c, 0,
	                   0x20048000u, 32768u, 640u);
	ok("[!] a model-only container still names where it came from",
	   lines() == 2u &&
	   strstr(out, "plugin  : (none in this container) -- reservation 32768 B "
	               "at 0x20048000\r\n") == out &&
	   strstr(out, "  from  : /bf.nnc, 194232 B, file crc32 12E129EE\r\n"));
	ok("...and no image line", strstr(out, "  image :") == NULL);

	fill(&c, 1);
	clear();
	plugin_claims_info(sink, NULL, PLUGIN_CLAIMS_VALID, &c, 0,
	                   0x20048000u, 32768u, 640u);
	ok("a plugin not running: validated, not loaded",
	   strstr(out, "plugin  : blazeface (build abc1234, crc 12345678), "
	               "validated, not loaded\r\n") == out);
	ok("...then from, then image, then the stack lines",
	   strstr(out, "  from  : /bf.nnc,") != NULL &&
	   strstr(out, "  image : 4100 B file / 8900 B mem") != NULL &&
	   strstr(out, "  from  :") < strstr(out, "  image :") &&
	   lines() >= 5u);

	clear();
	plugin_claims_info(sink, NULL, PLUGIN_CLAIMS_VALID, &c, 1,
	                   0x20048000u, 32768u, 640u);
	ok("a plugin running says so",
	   strstr(out, "crc 12345678), running\r\n") != NULL);

	clear();
	refuse_after = 0;
	plugin_claims_info(sink, NULL, PLUGIN_CLAIMS_VALID, &c, 1,
	                   0x20048000u, 32768u, 640u);
	ok("a refused write stops the lines", writes == 1 && used == 0u);

	clear();
	plugin_claims_info(NULL, NULL, PLUGIN_CLAIMS_VALID, &c, 1,
	                   0x20048000u, 32768u, 640u);
	ok("a NULL writer writes nothing", used == 0u);
}

int main(void)
{
	printf("test_plugin_claims\n");
	test_store();
	test_lines();
	if (failures != 0) {
		printf("test_plugin_claims: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("test_plugin_claims: all passed\n");
	return 0;
}
