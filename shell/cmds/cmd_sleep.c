/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_sleep.c
 * @brief   `sleep` (seconds) and `usleep` (microseconds) delay commands
 * (owhinata/stm32f746g-disco#21).
 *
 * `sleep N`  -- block N seconds, cancellable with Ctrl+C.  Built on cli_sleep()
 *               (owhinata/stm32f746g-disco#16): it waits on the instance event
 * flags, so a 0x03 wakes it and the dispatcher prints "^C".
 * `usleep N` -- busy-wait N microseconds on the DWT cycle counter (udelay, CPU
 *               clock; CLI_CPU_CYCLES_PER_US is the board's cycles/us).  Short,
 *               CPU-bound, NOT interruptible -- capped small; use `sleep` for
 *               long delays.
 *
 * Linked into the threadx executable only (like cmd_system.c / cmd_thread.c).
 * Clean-room design; no third-party code reused.
 */
#include <stdint.h>

#include "cli.h"
#include "timebase.h"   /* udelay (DWT cycle-counter busy-wait) */

static int cmd_sleep(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t sec;

	(void)argc;
	if (cli_parse_u32(argv[1], &sec) != 0 || sec > CLI_SLEEP_MAX_SEC) {
		cli_error(sh, "sleep: bad seconds '%s' (0..%u)\r\n",
		          argv[1], (unsigned)CLI_SLEEP_MAX_SEC);
		return 1;
	}
	/* Cancellable (Ctrl+C): cli_sleep returns non-zero on cancel; the dispatcher
	 * then prints "^C" via cancel_req.  1 tick == 1 ms, so sec*1000 ticks. */
	return cli_sleep(sh, sec * 1000u) ? 1 : 0;
}

static int cmd_usleep(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t us;

	(void)argc;
	if (cli_parse_u32(argv[1], &us) != 0 || us > CLI_USLEEP_MAX_US) {
		cli_error(sh, "usleep: bad microseconds '%s' (0..%u)\r\n",
		          argv[1], (unsigned)CLI_USLEEP_MAX_US);
		return 1;
	}
	udelay(us);   /* busy-wait; not interruptible (kept small by the cap) */
	return 0;
}

CLI_CMD_REGISTER_USAGE(sleep, NULL, "sleep N seconds (Ctrl+C cancels)", "<seconds>",
                       cmd_sleep, 2, 0);
CLI_CMD_REGISTER_USAGE(usleep, NULL, "busy-wait N microseconds (not interruptible)",
                       "<microseconds>", cmd_usleep, 2, 0);
