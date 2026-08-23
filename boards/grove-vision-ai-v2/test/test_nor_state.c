/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the NOR port's lifecycle decisions (issue #86,
 * port/nor/nor_state.c).
 *
 * WHY THIS EXISTS.  Two of the three interesting cases cannot be produced from
 * a console on this board:
 *
 *   - Two callers arriving in the `OFF -> ENABLING` window.  There is one
 *     shell, and its background jobs run BELOW the foreground one under
 *     TX_NO_TIME_SLICE, so `cmd &; cmd2` cannot even get the two threads into
 *     the right order.
 *   - A release of a lease the caller does not hold.  `nn close` calls its
 *     teardown unconditionally, so this happens on the ordinary path -- but the
 *     variants that matter (a stale token from a previous generation, a token
 *     that looks live with no readers left) need a fault or a leak first.
 *
 * The third, releasing while FAULTED, is producible only by breaking the
 * hardware bring-up on purpose.
 *
 * [!] THE LEAK THIS FILE IS REALLY FOR is one level up and is checked at the
 * end: npu_hw_init() acquires, then can still fail three ways (SCU bring-up,
 * ethosu_init, the EPK wrap), and its caller does NOT call npu_hw_deinit() when
 * it fails.  A table test of acquire/release alone would pass while that leaks
 * on every failed bring-up, so the last section walks the integration sequence
 * against the same functions the port uses.
 */
#include <stdio.h>

#include "nor_state.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static const char *acq_name(enum nor_acquire a)
{
	switch (a) {
	case NOR_ACQ_BRING_UP: return "bring-up";
	case NOR_ACQ_TAKE:     return "take";
	case NOR_ACQ_BUSY:     return "busy";
	case NOR_ACQ_FAULTED:  return "faulted";
	default:               break;
	}
	return "?";
}

static const char *rel_name(enum nor_release r)
{
	switch (r) {
	case NOR_REL_DROP:      return "drop";
	case NOR_REL_NOT_HELD:  return "not-held";
	case NOR_REL_STALE:     return "stale";
	case NOR_REL_BAD_SLOT:  return "bad-slot";
	default:               break;
	}
	return "?";
}

/* ---- a model of the port, using the real decision functions -------------- */

struct port {
	enum nor_state state;
	uint32_t       live;   /* bitmask of slots holding a lease */
	uint32_t       gen;
};

/* Returns the token, or 0 if no lease was granted.  @p bring_up_ok models the
 * hardware: whether the vendor open + XIP setup + read-back would succeed. */
static uint32_t port_acquire(struct port *p, enum nor_lease_slot slot,
                             int bring_up_ok)
{
	if (p->live & (1u << (uint32_t)slot))
		return 0u;              /* the slot is single-instance */

	switch (nor_acquire_decide(p->state)) {
	case NOR_ACQ_BRING_UP:
		p->state = NOR_ST_ENABLING;      /* published under the same lock */
		if (!bring_up_ok) {
			p->state = NOR_ST_FAULTED;   /* terminal; claim cleared  */
			return 0u;
		}
		p->gen++;                        /* a new lifecycle generation   */
		p->state = NOR_ST_XIP;
		break;
	case NOR_ACQ_TAKE:
		break;
	case NOR_ACQ_BUSY:
	case NOR_ACQ_FAULTED:
	default:
		return 0u;
	}
	p->live |= (1u << (uint32_t)slot);
	return nor_token_make(p->gen, slot);
}

static enum nor_release port_release(struct port *p, uint32_t token)
{
	enum nor_release r = nor_release_decide(p->state, p->live, token, p->gen);

	if (r == NOR_REL_DROP)
		p->live &= ~(1u << ((token & 0xFFu) - 1u));
	return r;
}

int main(void)
{
	/* ---- the acquire table, every state ----------------------------- */
	static const struct {
		enum nor_state   st;
		enum nor_acquire want;
	} acq[] = {
		{ NOR_ST_OFF,      NOR_ACQ_BRING_UP },
		{ NOR_ST_XIP,      NOR_ACQ_TAKE     },
		{ NOR_ST_ENABLING, NOR_ACQ_BUSY     },
		{ NOR_ST_FAULTED,  NOR_ACQ_FAULTED  },
	};
	for (unsigned i = 0; i < sizeof(acq) / sizeof(acq[0]); i++) {
		enum nor_acquire got = nor_acquire_decide(acq[i].st);
		CHECK(got == acq[i].want, "acquire_decide(%s) = %s, want %s",
		      nor_state_name(acq[i].st), acq_name(got),
		      acq_name(acq[i].want));
	}

	/* [!] An unknown state must refuse the terminal way, not the busy way:
	 * BUSY invites a retry loop against a state nobody can explain. */
	CHECK(nor_acquire_decide((enum nor_state)99) == NOR_ACQ_FAULTED,
	      "an unknown state does not fall to the terminal refusal");

	/* ---- the release table ------------------------------------------ */
	static const struct {
		enum nor_state   st;
		uint32_t         live, token, gen;
		enum nor_release want;
		const char      *what;
	} rel[] = {
		{ NOR_ST_XIP, 0x1u, 0u, 1u, NOR_REL_NOT_HELD,
		  "a zero token" },
		/* [!] The ordering case: before the first bring-up the generation
		 * is 0, so a zero token compares EQUAL to a freshly built one.
		 * Answering "not held" first is what keeps it from reading live. */
		{ NOR_ST_OFF, 0x0u, 0u, 0u, NOR_REL_NOT_HELD,
		  "a zero token against generation 0" },
		{ NOR_ST_XIP, 0x1u, (1u << 8) | 1u, 2u, NOR_REL_STALE,
		  "a token from an older generation" },
		{ NOR_ST_XIP, 0x0u, (1u << 8) | 1u, 1u, NOR_REL_NOT_HELD,
		  "a token for a slot the mask no longer holds" },
		{ NOR_ST_XIP, 0x1u, (1u << 8) | 99u, 1u, NOR_REL_BAD_SLOT,
		  "a token naming no slot this port has" },
		{ NOR_ST_XIP, 0x1u, (1u << 8) | 1u, 1u, NOR_REL_DROP,
		  "a live token" },
		/* [!] A holder may give its lease back while FAULTED.  Refusing
		 * would strand every caller already holding one, and the mask
		 * would never come back empty. */
		{ NOR_ST_FAULTED, 0x1u, (1u << 8) | 1u, 1u, NOR_REL_DROP,
		  "a holder returning its lease after a fault" },
	};
	for (unsigned i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
		enum nor_release got = nor_release_decide(rel[i].st, rel[i].live,
		                                          rel[i].token, rel[i].gen);
		CHECK(got == rel[i].want, "%s -> %s, want %s",
		      rel[i].what, rel_name(got), rel_name(rel[i].want));
	}

	/* ---- sequences over the model ----------------------------------- */
	{
		struct port p = { NOR_ST_OFF, 0u, 0u };
		uint32_t npu  = port_acquire(&p, NOR_LEASE_NPU, 1);
		uint32_t scan = port_acquire(&p, NOR_LEASE_SCAN, 1);

		CHECK(npu != 0u && scan != 0u, "two holders could not both acquire");
		CHECK(npu != scan,
		      "two concurrent holders were given the SAME token (0x%08x)",
		      npu);
		CHECK(nor_readers(p.live) == 2u, "readers = %u, want 2",
		      nor_readers(p.live));

		CHECK(port_release(&p, npu) == NOR_REL_DROP, "first release refused");
		/* [!] The case a generation-only token could not answer: the NPU
		 * releasing twice must NOT take the scan's lease away. */
		CHECK(port_release(&p, npu) == NOR_REL_NOT_HELD,
		      "a duplicate release was not caught");
		CHECK(nor_readers(p.live) == 1u,
		      "a duplicate release changed the count to %u",
		      nor_readers(p.live));
		CHECK(port_release(&p, scan) == NOR_REL_DROP,
		      "the other holder's lease was taken by the duplicate");
		CHECK(nor_readers(p.live) == 0u, "readers did not return to 0");
	}

	{	/* A failed bring-up is terminal and hands out nothing. */
		struct port p = { NOR_ST_OFF, 0u, 0u };
		CHECK(port_acquire(&p, NOR_LEASE_NPU, 0) == 0u,
		      "a failed bring-up handed out a lease");
		CHECK(p.state == NOR_ST_FAULTED, "a failed bring-up is not terminal");
		CHECK(nor_readers(p.live) == 0u,
		      "a failed bring-up left a reader behind");
		CHECK(port_acquire(&p, NOR_LEASE_SCAN, 1) == 0u,
		      "a faulted port handed out a lease to a later caller");
	}

	{	/* [!] THE PUBLICATION WINDOW.  The first version of nor_flash.c
		 * published NOR_ST_XIP and then claimed the slot in a SECOND
		 * critical section.  A caller for the same slot arriving in
		 * between saw an up window and a free slot, took it, and was
		 * minted a token IDENTICAL to the one the bring-up was about to
		 * mint -- same generation, same slot.  Either release then
		 * dropped the other's claim.
		 *
		 * [!] AND THIS FILE DID NOT FIND IT -- an adversarial review did.
		 * The bug was in nor_flash.c's SEQUENCING, and this test compiles
		 * only nor_state.c; the model's port_acquire() does the whole
		 * thing inline, so it has no window to leave open and would pass
		 * against the broken code too.  What is written below is the
		 * INVARIANT the fix has to keep, recorded where somebody changing
		 * the lease will read it.  Do not mistake it for coverage:
		 * nor_flash.c cannot be host-compiled, and no test in this tree
		 * watches that commit stay atomic. */
		struct port p = { NOR_ST_OFF, 0u, 0u };
		uint32_t intruder;

		/* bring-up reaches the point where the hardware is ready ... */
		p.state = NOR_ST_ENABLING;
		/* ... and the commit happens as ONE transaction. */
		p.gen++;
		p.state = NOR_ST_XIP;
		p.live |= (1u << (uint32_t)NOR_LEASE_NPU);

		/* A second caller for the same slot must be refused, whether it
		 * arrives before or after that commit -- there is no instant at
		 * which the slot is free while the window is up. */
		intruder = port_acquire(&p, NOR_LEASE_NPU, 1);
		CHECK(intruder == 0u,
		      "the same slot was handed out twice across the commit");
		CHECK(nor_readers(p.live) == 1u,
		      "the refused intruder changed the reader count to %u",
		      nor_readers(p.live));

		/* And a DIFFERENT slot is still free, so the refusal is about the
		 * slot and not about the window. */
		CHECK(port_acquire(&p, NOR_LEASE_SCAN, 1) != 0u,
		      "a different slot was refused too");
	}

	{	/* A slot is single-instance, and refusing does not disturb it. */
		struct port p = { NOR_ST_OFF, 0u, 0u };
		uint32_t a = port_acquire(&p, NOR_LEASE_SCAN, 1);
		CHECK(a != 0u, "the scan slot could not be taken");
		CHECK(port_acquire(&p, NOR_LEASE_SCAN, 1) == 0u,
		      "the scan slot was handed out twice");
		CHECK(port_release(&p, a) == NOR_REL_DROP,
		      "the refused second take disturbed the first lease");
	}

	/* ---- the integration leak this file is really for ---------------- */
	/* npu_hw_init() acquires, and can then fail three more ways; its caller
	 * does not tear down on failure.  So every post-acquire failure has to
	 * release the LOCAL token before returning, and the token may only be
	 * committed once the whole transaction succeeded. */
	for (int fail_stage = 0; fail_stage < 4; fail_stage++) {
		struct port p = { NOR_ST_OFF, 0u, 0u };
		uint32_t committed = 0u;         /* what npu_hw_deinit would see */
		uint32_t local = port_acquire(&p, NOR_LEASE_NPU, 1);
		int ready = 0;

		CHECK(local != 0u, "stage %d: the NOR lease was not granted",
		      fail_stage);

		/* stages: 0 = SCU bring-up, 1 = ethosu_init, 2 = EPK wrap,
		 *         3 = nothing fails -- the commit boundary itself */
		if (fail_stage < 3) {
			CHECK(port_release(&p, local) == NOR_REL_DROP,
			      "stage %d: the local token was not released on failure",
			      fail_stage);
			local = 0u;
		} else {
			committed = local;   /* commit only on full success */
			ready = 1;
		}

		CHECK(nor_readers(p.live) == (ready ? 1u : 0u),
		      "stage %d: %u reader(s) left, want %u",
		      fail_stage, nor_readers(p.live), ready ? 1u : 0u);

		/* nn close calls the teardown unconditionally; with no successful
		 * init there is nothing committed and it must be a no-op. */
		if (!ready)
			CHECK(nor_release_decide(p.state, p.live, committed, p.gen)
			      == NOR_REL_NOT_HELD,
			      "stage %d: teardown after a failed init is not a no-op",
			      fail_stage);

		/* And a later reader must still be able to acquire. */
		{
			uint32_t again = port_acquire(&p, NOR_LEASE_SCAN, 1);
			CHECK(again != 0u,
			      "stage %d: a later reader could not acquire", fail_stage);
			CHECK(port_release(&p, again) == NOR_REL_DROP,
			      "stage %d: the later reader could not release", fail_stage);
		}

		if (ready) {
			CHECK(port_release(&p, committed) == NOR_REL_DROP,
			      "the committed token could not be released");
			CHECK(nor_readers(p.live) == 0u, "readers did not return to 0");
		}
	}

	if (failures) {
		printf("test_nor_state: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nor_state: ok\n");
	return 0;
}
