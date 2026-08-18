/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_mve.c
 * @brief   `mve` shell command: prove vector state survives a context switch
 *          (issue #42).
 *
 * WHY THIS EXISTS.  Issue #42 removed a build gate that barred predicated MVE,
 * on the strength of an ARGUMENT: the Armv8-M ARM stacks VPR with the
 * caller-saved floating-point frame, the ThreadX Cortex-M55 port saves the
 * callee-saved half, so the pair is covered.  The argument is sound and the
 * documents are quoted in the board README -- but nobody had ever WATCHED it
 * work on this part, and this repository has an object lesson about that: issue
 * #66's scan could not detect one instruction it named and said OK for months.
 *
 * So this runs the experiment.
 *
 * [!] AND A SLEEP WOULD NOT BE ONE.  A thread that loads a pattern, sleeps and
 * reads it back passes on a BROKEN port: an idle board has nothing else ready,
 * so nobody overwrites the registers and "unchanged" proves nothing at all.
 * The test therefore hires an antagonist -- a higher-priority worker whose only
 * job is to fill the same registers with a different pattern -- and checks that
 * it ran before believing anything it sees.
 *
 * [!] AND NO CALL MAY SIT BETWEEN THE LOAD AND THE SWITCH.  q4-q7 (s16-s31) are
 * callee-SAVED under AAPCS, so if `tx_thread_resume()` were called after the
 * pattern was loaded and happened to use them, its own prologue would spill the
 * pattern to this thread's stack and its epilogue would put it back -- masking a
 * port that never saved a vector register in its life.  Hence the order:
 *
 *    1. mask interrupts, and only then make the worker ready.  The switch
 *       becomes PENDING; ThreadX restores the caller's PRIMASK rather than
 *       clearing it, so it cannot happen yet.
 *    2. in ONE assembly block, with no call in it: load the pattern, sample
 *       CONTROL (FPCA must be set, or the frame this all depends on does not
 *       exist), unmask, take the switch, and read the registers back.
 *    3. compare -- the epoch first.
 *
 * A failure here means the ban was right and issue #42 should be reverted.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "tx_api.h"
#include "WE2_device.h"   /* CMSIS core: __disable_irq / __enable_irq */

#define LOG_TAG "mve"
#include "log.h"

/* Above the shell thread, so making it ready preempts immediately rather than
 * eventually -- and below the camera producer (10) and panel (9), which this
 * has no business outranking. */
#define MVE_WORKER_PRIO   (CLI_INSTANCE_PRIORITY - 1)
#define MVE_WORKER_STACK  1024u

static TX_THREAD mve_worker;
static UCHAR     mve_worker_stack[MVE_WORKER_STACK] __attribute__((aligned(8)));
static volatile uint32_t mve_epoch;

/* Two patterns that cannot be confused with each other, with zero, or with a
 * stale address: every word differs from every other. */
static const uint32_t pattern_a[16] = {
	0xA0000001u, 0xA0000002u, 0xA0000003u, 0xA0000004u,
	0xA0000005u, 0xA0000006u, 0xA0000007u, 0xA0000008u,
	0xA0000009u, 0xA000000Au, 0xA000000Bu, 0xA000000Cu,
	0xA000000Du, 0xA000000Eu, 0xA000000Fu, 0xA0000010u,
};
static const uint32_t pattern_b[16] = {
	0xB0000001u, 0xB0000002u, 0xB0000003u, 0xB0000004u,
	0xB0000005u, 0xB0000006u, 0xB0000007u, 0xB0000008u,
	0xB0000009u, 0xB000000Au, 0xB000000Bu, 0xB000000Cu,
	0xB000000Du, 0xB000000Eu, 0xB000000Fu, 0xB0000010u,
};

/*
 * [!] SIXTEEN BITS, not thirty-two.  VPR.P0 is bits [15:0]; [19:16] and [23:20]
 * are the VPT block's MASK01/MASK23 and [31:24] are RES0, so `vmsr p0` writes
 * the predicate and nothing else.  The first version of this test wrote
 * 0x0F0F0F0F and read back 0x00000F0F, which looked like a failure and was not:
 * the value had survived the switch perfectly, in the width the register has.
 *
 * The MASK fields are deliberately NOT exercised.  Setting them puts the PE
 * inside a VPT block, which would predicate the very instructions this test
 * needs to execute afterwards -- and it would prove nothing extra anyway: VPR
 * is one register in one stack slot, so a P0 that survives is a VPR that
 * survived.
 */
#define VPR_A 0x00000F0Fu
#define VPR_B 0x0000F0F0u

/*
 * The antagonist.  Its whole contribution is to leave DIFFERENT values in the
 * registers this test cares about, and to prove it was here.
 *
 * It returns rather than blocking again: tx_thread_delete() accepts only a
 * completed or terminated thread, so a worker parked on something could not be
 * cleaned up without terminating it first.  Returning also has ThreadX drop its
 * CONTROL.FPCA on the way out, which is the tidiest possible exit for a thread
 * whose entire body was floating-point state.
 */
static void mve_worker_entry(ULONG arg)
{
	(void)arg;

	__asm volatile(
		"vldrw.u32 q4, [%[b], #0]\n\t"
		"vldrw.u32 q5, [%[b], #16]\n\t"
		"vldrw.u32 q6, [%[b], #32]\n\t"
		"vldrw.u32 q7, [%[b], #48]\n\t"
		"vmsr p0, %[vpr]\n\t"
		:
		: [b] "r" (pattern_b), [vpr] "r" (VPR_B)
		: "q4", "q5", "q6", "q7", "memory");

	mve_epoch++;
}

static int cmd_mve(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t got[16];
	uint32_t vpr_got = 0u;
	uint32_t control = 0u;
	uint32_t epoch_before, epoch_after;
	UINT rc;
	int bad = 0;
	unsigned i;

	(void)argc; (void)argv;

	memset(got, 0, sizeof got);
	epoch_before = mve_epoch;

	rc = tx_thread_create(&mve_worker, "mve", mve_worker_entry, 0,
	                      mve_worker_stack, sizeof mve_worker_stack,
	                      MVE_WORKER_PRIO, MVE_WORKER_PRIO,
	                      TX_NO_TIME_SLICE, TX_DONT_START);
	if (rc != TX_SUCCESS) {
		cli_error(sh, "mve: could not create the worker (%u)\r\n", rc);
		return 1;
	}

	/*
	 * [!] The mask goes on BEFORE the worker is made ready, so the switch it
	 * asks for waits for the assembly below instead of happening inside a
	 * call that could have saved the pattern for us.
	 */
	__disable_irq();
	rc = tx_thread_resume(&mve_worker);
	if (rc != TX_SUCCESS) {
		__enable_irq();
		(void)tx_thread_terminate(&mve_worker);
		(void)tx_thread_delete(&mve_worker);
		cli_error(sh, "mve: could not start the worker (%u)\r\n", rc);
		return 1;
	}

	__asm volatile(
		/* Pattern A into the callee-saved half and the predicate. */
		"vldrw.u32 q4, [%[a], #0]\n\t"
		"vldrw.u32 q5, [%[a], #16]\n\t"
		"vldrw.u32 q6, [%[a], #32]\n\t"
		"vldrw.u32 q7, [%[a], #48]\n\t"
		"vmsr p0, %[vpra]\n\t"
		/* FPCA must be set by now -- those were MVE instructions and
		 * FPCCR.ASPEN is enforced at kernel entry.  Sampled here rather
		 * than asserted, so a failure reports the value it saw. */
		"mrs %[ctl], control\n\t"
		/* The switch, taken here and nowhere else. */
		"cpsie i\n\t"
		"isb\n\t"
		/* Back on this thread: read both halves before anything else
		 * can touch them. */
		"vstrw.32 q4, [%[out], #0]\n\t"
		"vstrw.32 q5, [%[out], #16]\n\t"
		"vstrw.32 q6, [%[out], #32]\n\t"
		"vstrw.32 q7, [%[out], #48]\n\t"
		"vmrs %[vpro], p0\n\t"
		: [ctl] "=&r" (control), [vpro] "=&r" (vpr_got)
		: [a] "r" (pattern_a), [vpra] "r" (VPR_A), [out] "r" (got)
		: "q4", "q5", "q6", "q7", "memory");

	epoch_after = mve_epoch;

	/*
	 * [!] The antagonist first.  If it never ran, every comparison below is
	 * a comparison against nothing -- which is precisely how a test like
	 * this passes on a system that would have failed it.
	 */
	if (epoch_after == epoch_before) {
		cli_error(sh, "mve: the worker never ran -- this test proves "
		              "NOTHING about\r\n"
		              "     preservation; it did not happen\r\n");
		bad = 1;
	}
	if ((control & 0x4u) == 0u) {
		cli_error(sh, "mve: CONTROL %08lx has FPCA clear -- no vector "
		              "frame was stacked,\r\n"
		              "     so nothing here was preserved by the "
		              "mechanism under test\r\n",
		          (unsigned long)control);
		bad = 1;
	}

	for (i = 0u; i < 16u; i++)
		if (got[i] != pattern_a[i]) {
			cli_error(sh, "mve: q%u word %u is %08lx, wrote "
			              "%08lx%s\r\n",
			          4u + i / 4u, i % 4u, (unsigned long)got[i],
			          (unsigned long)pattern_a[i],
			          got[i] == pattern_b[i] ?
			              "  (the worker's value)" : "");
			bad = 1;
			break;
		}
	if (vpr_got != VPR_A) {
		cli_error(sh, "mve: VPR is %08lx, wrote %08lx%s\r\n",
		          (unsigned long)vpr_got, (unsigned long)VPR_A,
		          vpr_got == VPR_B ? "  (the worker's value)" : "");
		bad = 1;
	}

	if (mve_worker.tx_thread_state != TX_COMPLETED) {
		cli_error(sh, "mve: the worker is in state %lu, not completed; "
		              "not deleting it\r\n",
		          (unsigned long)mve_worker.tx_thread_state);
		return 1;
	}
	(void)tx_thread_delete(&mve_worker);

	if (bad)
		return 1;

	cli_print(sh, "mve      : q4-q7 and VPR.P0 survived a context switch\r\n");
	cli_print(sh, "  worker : ran (epoch %lu), wrote its own pattern over "
	              "both\r\n", (unsigned long)epoch_after);
	cli_print(sh, "  CONTROL: %08lx (FPCA set, so the frame existed)\r\n",
	          (unsigned long)control);
	cli_print(sh, "           (the hardware owns VPR, the ThreadX port owns "
	              "q4-q7 -- issue #42)\r\n");
	return 0;
}

CLI_CMD_REGISTER(mve, NULL,
                 "prove vector state survives a context switch", cmd_mve, 1, 0);
