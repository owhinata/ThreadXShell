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
 * CLI_ENABLE_DANGEROUS_CMDS is set.  It stops the camera first and refuses
 * when the stop does not confirm (issue #68, cmds/reboot_gate.h).  Clean-room
 * design; no code reused.
 */
#include "cli.h"
#include "cli_version.h"     /* CLI_FW_NAME / CLI_FW_VERSION / CLI_GIT_DESC */

#include "WE2_device.h"
#include "tx_api.h"
#include "tx_glue.h"

#include <stdint.h>

#if CLI_ENABLE_DANGEROUS_CMDS
#include "camera.h"
#include "reboot_gate.h"

/* [!] reboot_gate.c decides about NUMBERS and is compiled on the host where
 * camera.h cannot be included; these tie the two together. */
_Static_assert(REBOOT_CAM_OK      == CAM_OK,          "CAM_OK moved");
_Static_assert(REBOOT_CAM_TIMEOUT == CAM_ERR_TIMEOUT, "CAM_ERR_TIMEOUT moved");
_Static_assert(REBOOT_CAM_STATE   == CAM_ERR_STATE,   "CAM_ERR_STATE moved");
_Static_assert(REBOOT_CAM_LOCKED  == CAM_ERR_LOCKED,  "CAM_ERR_LOCKED moved");
#endif

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
 * Run the camera's one stop routine and say whether its answer permits a
 * reset.  On a refusal the reason is printed here.  camera_stream_stop() is
 * the only camera entry that waits for the API mutex (bounded), and the
 * decision over the state is taken under that mutex inside it -- nothing
 * here reads the camera state beforehand.
 *
 * It is called directly, not through the holder table (svc/camera/
 * cam_holders) or the nn stream machine: those are soft state the reset
 * wipes, and the panel sink's unlink and the holders' release touch no camera
 * hardware.  What has to be true before the reset is only that the producer
 * has been through its quiesce, and a CAM_OK from this call is that proof.
 */
static int reboot_camera_stopped(struct cli_instance *sh)
{
	int rc = camera_stream_stop();
	enum reboot_verdict v = reboot_verdict_of(rc);

	if (v == REBOOT_RESET)
		return 1;
	cli_error(sh, "reboot: refused: %s (camera stop %d)\r\n"
	              "        a soft reset leaves the camera running and the "
	              "next stream start\r\n"
	              "        hangs (issue #68); press RST\r\n",
	          reboot_refusal_text(v), rc);
	return 0;
}

/*
 * reboot: software reset via SCB->AIRCR SYSRESETREQ.  The board re-enters the
 * Himax bootloader (its xmodem menu window included), then the app.
 *
 * [!] THE CAMERA IS STOPPED FIRST, AND THE RESET HAPPENS ONLY ON A CONFIRMED
 * STOP (issue #68).  The reset is the CPU's alone: a stream left running keeps
 * the sensor and the datapath going, the previous boot's EDM watchdog expires
 * after the new boot's NVIC sweep, and the first `camera stream start` that
 * follows hangs the console.  Anything but a confirmed stop is refused, and
 * the operator is sent to the RST button, which resets the whole board.
 *
 * The stop is asked TWICE: once before the goodbye message, so a refusal is
 * printed instead of it, and once after the ~50 ms the UART TX ring needs to
 * drain -- that sleep lets lower-priority threads run, and a background job
 * could start a stream inside it.  The second stop answers CAM_OK at once when
 * nothing did.
 *
 * NOT COVERED: a crash reset through fault.c does not come here, so it does
 * not stop the camera (see the README's reboot note).
 */
static int cmd_reboot(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (!reboot_camera_stopped(sh))
		return 1;
	cli_print(sh, "rebooting (camera stopped)...\r\n");
	tx_thread_sleep(50);          /* ~50 ms: let the TX ring drain */
	if (!reboot_camera_stopped(sh))
		return 1;
	NVIC_SystemReset();           /* does not return */
	return 0;                     /* unreachable */
}
#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_CMD_REGISTER(version, NULL, "show firmware/MCU version", cmd_version, 1, 0);
CLI_CMD_REGISTER(uptime,  NULL, "show uptime since boot (~49.7d wrap)", cmd_uptime, 1, 0);
#if CLI_ENABLE_DANGEROUS_CMDS
CLI_CMD_REGISTER(reboot,  NULL, "stop the camera, then reboot the board", cmd_reboot, 1, 0);
#endif
