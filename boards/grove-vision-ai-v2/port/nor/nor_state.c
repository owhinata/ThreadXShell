/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_state.c
 * @brief   The NOR lifecycle decisions, as pure functions (issue #86).
 *
 * No hardware, no ThreadX, no logging -- so test/test_nor_state.c compiles this
 * exact file on the host.  See nor_state.h for why the precedence lives apart
 * from the sequencing.
 */
#include "nor_state.h"

enum nor_acquire nor_acquire_decide(enum nor_state st)
{
	/* Enumerated, not tested-for-the-one-that-may-not: a wider test is what
	 * fails open here (nor_state.h). */
	switch (st) {
	case NOR_ST_OFF:
		return NOR_ACQ_BRING_UP;
	case NOR_ST_XIP:
		return NOR_ACQ_TAKE;
	case NOR_ST_ENABLING:
		return NOR_ACQ_BUSY;
	/* A writer has dropped XIP; the alias is not readable until it commits
	 * back (issue #88).  BUSY and not FAULTED: this one really does clear. */
	case NOR_ST_WRITING:
		return NOR_ACQ_BUSY;
	case NOR_ST_FAULTED:
		return NOR_ACQ_FAULTED;
	default:
		break;
	}
	/* An unknown state is a corrupted one.  Refuse the way the terminal state
	 * refuses, rather than the way a busy one does: BUSY invites a retry. */
	return NOR_ACQ_FAULTED;
}

/* Token layout: generation in the high bits, slot+1 in the low byte.  The +1
 * is what keeps slot 0's token from being 0, which is the "I hold nothing"
 * value every holder starts and ends with. */
#define NOR_TOKEN_SLOT_BITS  8u
#define NOR_TOKEN_SLOT_MASK  0xFFu

uint32_t nor_token_make(uint32_t gen, enum nor_lease_slot slot)
{
	if ((unsigned)slot >= (unsigned)NOR_LEASE_SLOTS)
		return 0u;
	return (gen << NOR_TOKEN_SLOT_BITS) | ((uint32_t)slot + 1u);
}

enum nor_release nor_release_decide(enum nor_state st, uint32_t live,
                                    uint32_t token, uint32_t gen)
{
	uint32_t slot;

	(void)st;   /* deliberately not consulted -- see nor_state.h */

	/* "Nothing held" is answered first, and only that ordering is load
	 * bearing: before the first bring-up the generation is 0, so a zero token
	 * would otherwise compare EQUAL to a freshly built one and read as live. */
	if (token == 0u)
		return NOR_REL_NOT_HELD;

	slot = (token & NOR_TOKEN_SLOT_MASK);
	if (slot == 0u || slot > (uint32_t)NOR_LEASE_SLOTS)
		return NOR_REL_BAD_SLOT;
	slot -= 1u;

	if ((token >> NOR_TOKEN_SLOT_BITS) != gen)
		return NOR_REL_STALE;

	/* The slot is what makes a duplicate release detectable: the holder's
	 * second attempt names a slot the mask no longer has. */
	if ((live & (1u << slot)) == 0u)
		return NOR_REL_NOT_HELD;

	return NOR_REL_DROP;
}

uint32_t nor_readers(uint32_t live)
{
	uint32_t n = 0u;

	for (uint32_t i = 0u; i < (uint32_t)NOR_LEASE_SLOTS; i++)
		if (live & (1u << i))
			n++;
	return n;
}

const char *nor_state_name(enum nor_state st)
{
	switch (st) {
	case NOR_ST_OFF:      return "off";
	case NOR_ST_ENABLING: return "enabling";
	case NOR_ST_XIP:      return "xip";
	case NOR_ST_WRITING:  return "writing";
	case NOR_ST_FAULTED:  return "faulted";
	default:              break;
	}
	return "?";
}

enum nor_write nor_write_decide(enum nor_state st, uint32_t live)
{
	/* Enumerated for the same reason acquire is (nor_state.h): the wider
	 * tests here are "not WRITING" -- which starts a writer from OFF, before
	 * the QSPI master is open -- and "state == XIP" alone, which starts one on
	 * top of readers whose window it is about to remove. */
	switch (st) {
	case NOR_ST_XIP:
		/* Read WITH the state, not after it.  A writer that trusted a
		 * separately-sampled reader count would be deciding on two facts
		 * that were never true at the same instant. */
		return (live == 0u) ? NOR_WR_GO : NOR_WR_BUSY;
	case NOR_ST_OFF:
	case NOR_ST_ENABLING:
	case NOR_ST_WRITING:
		return NOR_WR_BUSY;
	case NOR_ST_FAULTED:
		return NOR_WR_FAULTED;
	default:
		break;
	}
	/* Unknown means corrupted.  Refuse terminally rather than invitingly. */
	return NOR_WR_FAULTED;
}
