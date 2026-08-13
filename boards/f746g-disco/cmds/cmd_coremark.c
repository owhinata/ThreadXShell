/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_coremark.c
 * @brief   `coremark` built-in shell command: run the EEMBC CoreMark benchmark.
 *
 * Replaces the former standalone bare-metal CoreMark image.  The benchmark is
 * built once into the coremark_obj OBJECT library (the lib/coremark sources plus
 * the port in port/coremark, compiled at -O3 -funroll-loops with
 * MEM_METHOD=MEM_STATIC) and linked into the threadx firmware; this handler just
 * calls into it.
 *
 * core_main.c is compiled with -Dmain=coremark_main (see CMakeLists.txt) so the
 * benchmark entry does not collide with the firmware main() -- hence the local
 * declaration below.  It runs synchronously in the calling shell instance thread
 * (~12 s while CoreMark auto-calibrates), so the prompt is blocked until it
 * finishes; the LD1 heartbeat thread (higher priority) keeps blinking.  The data
 * block is static (MEM_STATIC), so it lives in .bss, not on this thread's stack.
 *
 * Output: CoreMark prints its canonical report via ee_printf -> printf, which the
 * UART backend's strong _write routes to the console OF THE THREAD THAT RAN IT --
 * the VCP's TX ring for the VCP instance, or (via uart_write_other) the telnet
 * instance's own transport when the command was typed there.  The earlier claim
 * that it always lands on the VCP was wrong; backend/cli_backend_uart.c has routed
 * per-instance since the telnet console arrived.  Either way the handoff is
 * bracketed by the same output lock as cli_print, so the two never interleave, and
 * the timed region itself does no I/O, so TX back-pressure cannot perturb the
 * score.  Registered into the `shell`/threadx executable only (never the host test
 * harness), like cmd_system.c / cmd_thread.c / cmd_devmem.c.
 *
 * A singleton guard rejects a second concurrent run (`coremark &` twice, or one
 * per console): the EEMBC port keeps global timing/seed/result state that
 * overlapping runs would corrupt (issue #4).  Not a dangerous command (read-only
 * CPU benchmark), so it is not gated behind CLI_ENABLE_DANGEROUS_CMDS.
 *
 * Clean-room glue; the CoreMark sources themselves are EEMBC's (Apache-2.0).
 */
#include "cli.h"
#include "stm32f7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include <stdint.h>

/* CoreMark entry, renamed from main() by -Dmain=coremark_main on core_main.c. */
int coremark_main(void);

/* Reentrancy guard (issue #4): only one CoreMark run at a time, across the two
 * console instances and background jobs.  Plain flag test-set under a brief PRIMASK
 * critical section -- interrupt-safe, no one-time init, and acquire / release may
 * run on different threads.  Same shape as the membench guard. */
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
	(void)argc;
	(void)argv;

	if (!coremark_try_acquire()) {
		cli_error(sh, "coremark: already running\r\n");
		return 1;
	}

	/* Not cooperatively cancellable (issue #16): coremark_main() is a single
	 * blocking call into the read-only EEMBC submodule with no poll point, and it
	 * prints via ee_printf -> printf (not the shell's cli_tx_send_blocking), so
	 * neither a cli_cancel_requested() check nor the TX-blocked RX wake applies.
	 * Ctrl+C during the run is ignored; it just queues for the next prompt. */
	cli_info(sh, "Running CoreMark (auto-calibrated, ~12s; not interruptible)...\r\n");
	coremark_main();   /* canonical report via printf -> this thread's console */

	coremark_busy = 0u;   /* single cleanup point: guard cleared on every exit */
	return 0;
}

CLI_CMD_REGISTER(coremark, NULL, "run the EEMBC CoreMark benchmark (~12s)",
                 cmd_coremark, 1, 0);
