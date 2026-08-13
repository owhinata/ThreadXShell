/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_coremark.c
 * @brief   `coremark` shell command: run the EEMBC CoreMark benchmark (#25).
 *
 * The benchmark is built once into the coremark_obj OBJECT library (the
 * lib/coremark sources plus the port in port/coremark, compiled at -O3
 * -funroll-loops -fno-tree-vectorize with MEM_METHOD=MEM_STATIC) and linked
 * into the shell firmware; this handler just calls into it.
 *
 * core_main.c is compiled with -Dmain=coremark_main (see board.cmake) so the
 * benchmark entry does not collide with the firmware main() -- hence the local
 * declaration below.  It runs synchronously in the calling shell instance's
 * thread (~10-100 s while CoreMark auto-calibrates), so the prompt is blocked
 * until it finishes.
 *
 * [!] A SCORE IS NOT COMPARABLE ON ITS OWN.  On this board the code and the
 * working set both live in tightly-coupled memory (ITCM/DTCM -- the app is not
 * XIP), the data block is MEM_STATIC, and the build is scalar (MVE
 * auto-vectorisation is off; the ThreadX Cortex-M55 port does not preserve VPR
 * across a context switch).  CoreMark's own report prints all three, and the
 * board README says so again; quoting the number without them is meaningless.
 *
 * Entry conditions are the shared benchmark gate (cmds/bench_gate.c): the
 * ThreadX tick must be running -- it is CoreMark's time base here, and the
 * auto-calibration loop would spin forever without it -- and the SCU core
 * frequency must be the one iterations/MHz is going to be divided by.
 *
 * A singleton guard rejects a second concurrent run (`coremark &` twice): the
 * EEMBC port keeps global timing/seed state and a single static working set
 * that overlapping runs would corrupt.  Not a dangerous command (a read-only
 * CPU benchmark), so it is not gated behind CLI_ENABLE_DANGEROUS_CMDS.
 *
 * Clean-room glue; the CoreMark sources themselves are EEMBC's (Apache-2.0).
 */
#include "cli.h"
#include "bench_gate.h"

#include "WE2_device.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

/* CoreMark entry, renamed from main() by -Dmain=coremark_main on core_main.c. */
int coremark_main(void);

/* Reentrancy guard: only one CoreMark run at a time (fg cli thread + bg workers). */
static volatile uint8_t coremark_busy;

static int coremark_try_acquire(void)
{
	uint32_t pm = __get_PRIMASK();
	int ok;

	__disable_irq();
	ok = !coremark_busy;
	if (ok)
		coremark_busy = 1u;
	__set_PRIMASK(pm);
	return ok;
}

static int cmd_coremark(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t core_hz = 0u;
	int      rc = 0;

	(void)argc;
	(void)argv;

	if (!coremark_try_acquire()) {
		cli_error(sh, "coremark: already running\r\n");
		return 1;
	}

	if (!bench_gate_check(sh, "coremark", &core_hz)) {
		rc = 1;
		goto done;
	}

	/* Not cooperatively cancellable: coremark_main() is a single blocking
	 * call into the read-only EEMBC submodule with no poll point, and it
	 * prints via ee_printf -> printf (not the shell's buffered output).
	 * Ctrl+C during the run is ignored; it just queues for the next prompt. */
	cli_info(sh, "Running CoreMark at %lu MHz (auto-calibrated, ~10-100s; "
	             "not interruptible)...\r\n",
	         (unsigned long)(core_hz / 1000000u));
	coremark_main();   /* canonical report via printf -> this thread's console */

	/* CoreMark's report prints iterations and seconds but not the clock the
	 * score should be normalised by; without it "Iterations/Sec" cannot be
	 * turned into CoreMark/MHz.  Re-read it rather than reprinting the entry
	 * value: this run just spent up to 100 s not looking, which is ample time
	 * for the clock tree to have moved underneath it. */
	cli_print(sh, "Core clock (SCU read-back): %lu Hz\r\n",
	          (unsigned long)core_hz);
	if (!bench_gate_recheck(sh, "coremark", core_hz))
		rc = 1;

done:
	coremark_busy = 0u;   /* single cleanup point: guard cleared on every exit */
	return rc;
}

CLI_CMD_REGISTER(coremark, NULL, "run the EEMBC CoreMark benchmark (~10-100s)",
                 cmd_coremark, 1, 0);
