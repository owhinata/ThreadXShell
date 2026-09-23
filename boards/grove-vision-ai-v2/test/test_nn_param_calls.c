/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the gate's claim and the threshold-call count (issue #122,
 * port/npu/nn_param_calls.c).
 *
 * WHY THIS EXISTS.  `nn thresh` takes no gate, and since a load can replace an
 * open model, a load on the other console could copy a new plugin over the code
 * a threshold call was executing.  The count closes that: a threshold call is refused
 * while a load or unload holds the gate, and a load or unload is refused while
 * one is counted in.  The window it closes is a background job preempted inside a plugin
 * callback -- microseconds, and not something a console can aim at.
 *
 * The sequences below are the interleavings, each step one critical section as
 * the adapter runs it.  [!] WHAT IT DOES NOT COVER: that the adapter runs each
 * of these inside ONE critical section and calls the plugin only between a
 * successful enter and its leave -- three short wrappers in nn_svc_grove.c hold
 * that.
 */
#include <stdio.h>

#include "nn_param_calls.h"

static int fails;

#define CHECK(cond, text)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL %s (line %d)\n", text, __LINE__);      \
			fails++;                                              \
		}                                                             \
	} while (0)

struct gate { uint8_t busy, owner; uint16_t calls; };

static int claim(struct gate *g, uint8_t who)
{
	return nn_gate_claim(&g->busy, &g->owner, g->calls, who);
}

static void release(struct gate *g)
{
	g->busy = 0u;
	g->owner = (uint8_t)NN_OWNER_NONE;
}

static int enter(struct gate *g)
{
	return nn_param_calls_enter(g->busy, g->owner, &g->calls);
}

int main(void)
{
	struct gate g = { 0u, 0u, 0u };
	unsigned w;

	printf("test_nn_param_calls\n");

	/* A call in: a load or unload is refused, and nothing changes. */
	CHECK(enter(&g) == 1 && g.calls == 1u, "enter on a free gate");
	CHECK(claim(&g, NN_OWNER_SWAP) == 0, "swap refused while a call is in");
	CHECK(g.busy == 0u && g.owner == NN_OWNER_NONE, "a refused claim changes nothing");
	/* ...while an ordinary operation or a stream is not: they replace nothing. */
	CHECK(claim(&g, NN_OWNER_OP) == 1 && g.owner == NN_OWNER_OP, "op claims while a call is in");
	release(&g);
	CHECK(claim(&g, NN_OWNER_STREAM) == 1, "stream claims while a call is in");
	release(&g);
	nn_param_calls_leave(&g.calls);
	CHECK(g.calls == 0u, "call counted out");
	CHECK(claim(&g, NN_OWNER_SWAP) == 1 && g.owner == NN_OWNER_SWAP, "swap claims once none is in");

	/* A swap holding the gate: a call is refused, and nothing changes. */
	CHECK(enter(&g) == 0 && g.calls == 0u, "call refused under a swap");
	release(&g);

	/* Any other holder: a threshold still works -- a stream above all. */
	for (w = NN_OWNER_OP; w <= NN_OWNER_STREAM; w++) {
		CHECK(claim(&g, (uint8_t)w) == 1, "claim");
		CHECK(enter(&g) == 1, "call under an op or a stream");
		nn_param_calls_leave(&g.calls);
		release(&g);
	}

	/* Two calls: the swap is refused until both are out. */
	CHECK(enter(&g) == 1 && enter(&g) == 1 && g.calls == 2u, "two calls");
	nn_param_calls_leave(&g.calls);
	CHECK(claim(&g, NN_OWNER_SWAP) == 0, "one call still in");
	nn_param_calls_leave(&g.calls);
	CHECK(claim(&g, NN_OWNER_SWAP) == 1, "none out");
	release(&g);

	/* A held gate refuses every claimant. */
	CHECK(claim(&g, NN_OWNER_OP) == 1, "op");
	for (w = NN_OWNER_OP; w <= NN_OWNER_SWAP; w++)
		CHECK(claim(&g, (uint8_t)w) == 0 && g.owner == NN_OWNER_OP, "held gate refuses");
	release(&g);

	/* Bounds: never below zero, never wrapped to zero. */
	nn_param_calls_leave(&g.calls);
	CHECK(g.calls == 0u, "give at zero stays zero");
	g.calls = (uint16_t)NN_PARAM_CALLS_MAX;
	CHECK(enter(&g) == 0 && g.calls == NN_PARAM_CALLS_MAX, "full refuses, does not wrap");
	CHECK(claim(&g, NN_OWNER_SWAP) == 0, "full refuses a swap");

	if (fails) {
		printf("test_nn_param_calls: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_param_calls: all passed\n");
	return 0;
}
