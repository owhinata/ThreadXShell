/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_system.c
 * @brief   System built-in shell commands: version / uptime / reboot.
 *
 * Grove Vision AI V2 port of the wio-lite-ai cmd_system.c.  Differences:
 *  - No HAL: uptime reads the ThreadX tick (1 ms, tx_glue SysTick), identity
 *    comes from the architectural SCB->CPUID (there is no vendor IDCODE/UID
 *    register documented for the HX6538 -- no public TRM).
 *  - The core clock line prints the RUNTIME SCU read-back (SystemCoreClock)
 *    next to the tx_glue snapshot, which is the value SysTick runs from.
 *
 * reboot is a *dangerous* command, compiled in only when
 * CLI_ENABLE_DANGEROUS_CMDS is set.  Clean-room design; no code reused.
 */
#include "cli.h"
#include "cli_version.h"     /* CLI_FW_NAME / CLI_FW_VERSION / CLI_GIT_DESC */

#include "WE2_device.h"
#include "tx_api.h"
#include "tx_glue.h"
#include "cli_backend_uart.h"   /* cli_grove_uart_stats (console health) */

#include <stdint.h>

static int cmd_version(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t cpuid = SCB->CPUID;
	uint32_t part  = (cpuid & SCB_CPUID_PARTNO_Msk) >> SCB_CPUID_PARTNO_Pos;
	uint32_t var   = (cpuid & SCB_CPUID_VARIANT_Msk) >> SCB_CPUID_VARIANT_Pos;
	uint32_t rev   = (cpuid & SCB_CPUID_REVISION_Msk) >> SCB_CPUID_REVISION_Pos;

	(void)argc;
	(void)argv;

	cli_print(sh, "%s v%s (%s)\r\n", CLI_FW_NAME, CLI_FW_VERSION, CLI_GIT_DESC);
	cli_print(sh, "Built:    %s %s\r\n", __DATE__, __TIME__);
	cli_print(sh, "ThreadX:  %u.%u.%u\r\n",
	          THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION);
	cli_print(sh, "MCU:      Himax HX6538 WiseEye2, CM55M "
	              "(CPUID part 0x%03lx r%lup%lu)\r\n",
	          (unsigned long)part, (unsigned long)var, (unsigned long)rev);
	cli_print(sh, "Core:     %lu Hz (SCU read-back; SysTick from %lu Hz)\r\n",
	          (unsigned long)SystemCoreClock, (unsigned long)tx_glue_core_hz());
	cli_print(sh, "App:      ITCM 0x10000000 + DTCM 0x30000000 "
	              "(loaded by the 2nd bootloader; not XIP)\r\n");

	/* Console health (issue #25).  The UART0 vector is wrapped at runtime for
	 * the cpu% accounting; these two counters are how you tell whether that
	 * wrapper is costing bytes.  Both should stay at 0 across heavy paste. */
	{
		uint32_t rx_dropped = 0u, err_events = 0u;

		if (cli_grove_uart_stats(&rx_dropped, &err_events))
			cli_print(sh, "Console:  UART0 921600, rx_dropped %lu, "
			              "err_events %lu\r\n",
			          (unsigned long)rx_dropped,
			          (unsigned long)err_events);
	}
	return 0;
}

/* uptime from the 1 kHz ThreadX tick (wraps after ~49.7 days). */
static int cmd_uptime(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t ms   = (uint32_t)tx_time_get() *
	                (1000u / TX_TIMER_TICKS_PER_SECOND);
	uint32_t secs = ms / 1000u;
	uint32_t days = secs / 86400u;
	uint32_t hh   = (secs % 86400u) / 3600u;
	uint32_t mm   = (secs % 3600u) / 60u;
	uint32_t ss   = secs % 60u;

	(void)argc;
	(void)argv;

	cli_print(sh, "up %lud %02lu:%02lu:%02lu (%lu ms)\r\n",
	          (unsigned long)days, (unsigned long)hh, (unsigned long)mm,
	          (unsigned long)ss, (unsigned long)ms);
	return 0;
}

#if CLI_ENABLE_DANGEROUS_CMDS
/*
 * reboot: software reset via SCB->AIRCR SYSRESETREQ.  The board re-enters the
 * Himax bootloader (its xmodem menu window included), then the app.  Sleep
 * briefly first so the UART TX ring drains the goodbye message.
 */
static int cmd_reboot(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cli_print(sh, "rebooting...\r\n");
	tx_thread_sleep(50);          /* ~50 ms: let the TX ring drain */
	NVIC_SystemReset();           /* does not return */
	return 0;                     /* unreachable */
}
#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_CMD_REGISTER(version, NULL, "show firmware/MCU version", cmd_version, 1, 0);
CLI_CMD_REGISTER(uptime,  NULL, "show uptime since boot (~49.7d wrap)", cmd_uptime, 1, 0);
#if CLI_ENABLE_DANGEROUS_CMDS
CLI_CMD_REGISTER(reboot,  NULL, "reboot the board (immediate)", cmd_reboot, 1, 0);
#endif
