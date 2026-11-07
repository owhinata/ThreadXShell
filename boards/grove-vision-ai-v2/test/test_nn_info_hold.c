/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for port/nn/nn_info_hold.c (issue #199): where this board's `nn
 * info` model section comes from, and whether the copy carries a value of the
 * load order's transition counter.
 *
 * Pinned: the gate taken, a stream and an ordinary operation (`nn run`, `nn
 * bench`, `nn out`, another `nn info`) carry the counter when it is even; a
 * load or an unload (NN_OWNER_SWAP) carries none at ANY of its three moments
 * -- before the counter goes odd, while it is odd, after it is even again with
 * the model changed -- nor does NONE with the gate refused or an owner no enum
 * names; an odd counter carries none from anyone; and the sections are what
 * they were before issue #199 (the gate's, a stream's, BUSY otherwise).  None of
 * these holders can be lined up from this board's one console.
 */
#include <stdio.h>
#include <string.h>

#include "nn_info_hold.h"
#include "nn_param_calls.h"
#include "nn_svc.h"

static int fails, checks;

#define CHECK(cond, what)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			printf("  FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__); \
			fails++;                                            \
		}                                                           \
	} while (0)

static void hold_case(const char *what, int taken, unsigned owner,
                      uint32_t seq, unsigned want_from, int want_known)
{
	struct nn_info_hold h;
	char line[200];

	memset(&h, 0x5A, sizeof h);
	nn_info_hold_of(taken, owner, seq, &h);
	snprintf(line, sizeof line, "%s: from %u (want %u), known %u (want %d), "
	         "seq %lu", what, (unsigned)h.from, want_from,
	         (unsigned)h.seq_known, want_known, (unsigned long)h.seq);
	CHECK(h.from == want_from && h.seq_known == (uint8_t)want_known &&
	      h.seq == (want_known ? seq : 0u), line);
}

/* What the shared check makes of the copy, against the counter as it is now. */
static int stands(const struct nn_info_hold *h, uint32_t now)
{
	struct nn_svc_info i;

	memset(&i, 0, sizeof i);
	i.seq_known = h->seq_known;
	i.seq       = h->seq;
	return nn_svc_info_seq_stands(&i, now);
}

int main(void)
{
	static const unsigned every_owner[] = {
		NN_OWNER_NONE, NN_OWNER_OP, NN_OWNER_STREAM, NN_OWNER_SWAP, 4u,
		0xFFu,
	};
	struct nn_info_hold h;
	size_t k;

	printf("nn_info_hold_of: the sections and the value\n");

	/* The gate taken: everything, and the counter it holds still.  The owner
	 * is not consulted. */
	for (k = 0; k < sizeof every_owner / sizeof every_owner[0]; k++) {
		char what[64];

		snprintf(what, sizeof what, "gate taken, owner word %u",
		         every_owner[k]);
		hold_case(what, 1, every_owner[k], 6u, NN_INFO_FROM_GATE, 1);
	}
	hold_case("gate taken, counter 0 (no load in this build)", 1,
	          NN_OWNER_OP, 0u, NN_INFO_FROM_GATE, 1);

	/* Held by a stream or an ordinary operation: they keep a load out. */
	hold_case("a stream holds it", 0, NN_OWNER_STREAM, 6u,
	          NN_INFO_FROM_STREAM, 1);
	hold_case("a run / bench holds it", 0, NN_OWNER_OP, 6u,
	          NN_INFO_FROM_BUSY, 1);
	hold_case("a stream, counter 0 (no load in this build)", 0,
	          NN_OWNER_STREAM, 0u, NN_INFO_FROM_STREAM, 1);

	/* [!] A LOAD OR AN UNLOAD HOLDS IT: no value at any of its moments. */
	hold_case("SWAP, before the counter goes odd", 0, NN_OWNER_SWAP, 6u,
	          NN_INFO_FROM_BUSY, 0);
	hold_case("SWAP, while it is odd", 0, NN_OWNER_SWAP, 7u,
	          NN_INFO_FROM_BUSY, 0);
	hold_case("SWAP, even again after the model changed", 0, NN_OWNER_SWAP,
	          8u, NN_INFO_FROM_BUSY, 0);

	/* Owners this board does not write with the gate refused. */
	hold_case("NONE with the gate refused", 0, NN_OWNER_NONE, 6u,
	          NN_INFO_FROM_BUSY, 0);
	hold_case("an owner no enum names (4)", 0, 4u, 6u, NN_INFO_FROM_BUSY, 0);
	hold_case("an owner no enum names (255)", 0, 0xFFu, 6u,
	          NN_INFO_FROM_BUSY, 0);

	/* An odd counter is no one state, whoever holds the gate. */
	hold_case("gate taken, odd counter", 1, NN_OWNER_OP, 7u,
	          NN_INFO_FROM_GATE, 0);
	hold_case("a stream, odd counter", 0, NN_OWNER_STREAM, 7u,
	          NN_INFO_FROM_STREAM, 0);
	hold_case("a run / bench, odd counter", 0, NN_OWNER_OP, 7u,
	          NN_INFO_FROM_BUSY, 0);

	/* And what the shared check then says: a copy with a value stands only
	 * while the counter still holds it; one without never does, even when
	 * the counter happens to equal what its seq field says. */
	printf("nn_svc_info_seq_stands: on this board's copies\n");
	nn_info_hold_of(0, NN_OWNER_STREAM, 6u, &h);
	CHECK(stands(&h, 6u), "a stream's copy stands while the counter is 6");
	CHECK(!stands(&h, 7u), "...not once a load has begun");
	CHECK(!stands(&h, 8u), "...nor after a load ran whole");
	nn_info_hold_of(1, NN_OWNER_OP, 0u, &h);
	CHECK(stands(&h, 0u), "no load in this build: the copy stands");
	nn_info_hold_of(0, NN_OWNER_SWAP, 0u, &h);
	CHECK(!stands(&h, 0u), "a SWAP holder's copy never stands, even at 0");
	nn_info_hold_of(0, NN_OWNER_SWAP, 8u, &h);
	CHECK(!stands(&h, 8u), "...nor at the value the load left");

	if (fails) {
		printf("test_nn_info_hold: %d of %d checks FAILED\n", fails,
		       checks);
		return 1;
	}
	printf("test_nn_info_hold: all %d checks passed\n", checks);
	return 0;
}
