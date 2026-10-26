/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    reboot_gate.h
 * @brief   Which camera stop result lets `reboot` reset the board (issue #68).
 *
 * WHY `reboot` ASKS THE CAMERA FIRST.  NVIC_SystemReset() resets the CPU and
 * nothing outside it: the sensor keeps streaming and the datapath keeps its
 * EDM watchdog armed.  That watchdog (5 s) then expires AFTER the next boot's
 * NVIC sweep in main.c, so its pending interrupt is delivered on the first
 * enable of irq 143 -- and the first `camera stream start` of the new boot
 * hangs the console (reproduced on hardware, issue #68).  `camera stream stop`
 * before `reboot` never shows it.  So `reboot` runs the board's one stop
 * routine, camera_stream_stop(), and resets only on a CONFIRMED stop.
 *
 * [!] EXACTLY ONE CODE PERMITS THE RESET, enumerated rather than excluded:
 * CAM_OK, which camera_stream_stop() also returns for "nothing was streaming".
 * Every other code proves nothing about the hardware (camera.h's success-only
 * join), and an unknown negative value is terminal -- so the table answers
 * REBOOT_REFUSE_UNKNOWN for anything it was not told about, and the zero value
 * of the enum is a refusal.
 *
 * [!] THE CODES BELOW ARE MIRRORED, NOT INCLUDED (camera.h pulls in the whole
 * datapath, which a host compiler cannot take).  cmd_system.c static-asserts
 * each one against camera.h, so a renumbered code is a build failure on the
 * board rather than a table deciding about values nobody returns.
 *
 * Pure, so test/test_reboot_gate.c walks it; whether the command actually
 * asks the stop, and resets only on REBOOT_RESET, is cmd_system.c's and is
 * not covered there.
 */
#ifndef REBOOT_GATE_H
#define REBOOT_GATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors of camera.h -- see the note above. */
#define REBOOT_CAM_OK        0
#define REBOOT_CAM_TIMEOUT (-3)
#define REBOOT_CAM_STATE   (-4)
#define REBOOT_CAM_LOCKED  (-8)

/** What `reboot` does with the stop's answer. */
enum reboot_verdict {
	REBOOT_REFUSE_UNKNOWN = 0, /**< a code nobody documented: terminal    */
	REBOOT_REFUSE_LOCKED,      /**< the API stayed locked; never asked    */
	REBOOT_REFUSE_LOST,        /**< the producer never acknowledged       */
	REBOOT_REFUSE_STATE,       /**< poisoned, or never created            */
	REBOOT_RESET,              /**< the camera is confirmed stopped       */
};

/**
 * @param stop_rc  what camera_stream_stop() returned
 * @return REBOOT_RESET for CAM_OK only; a refusal naming why otherwise.
 */
enum reboot_verdict reboot_verdict_of(int stop_rc);

/**
 * One English, ASCII sentence for a refusal, for the operator.  NULL for
 * REBOOT_RESET (there is nothing to explain); never NULL for a refusal,
 * including a verdict value outside the enum.
 */
const char *reboot_refusal_text(enum reboot_verdict v);

#ifdef __cplusplus
}
#endif

#endif /* REBOOT_GATE_H */
