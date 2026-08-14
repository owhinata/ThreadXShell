/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    timer_seam.h
 * @brief   Board-owned replacement for the vendor timer API (issue #30).
 *
 * The prebuilt camera archives (libsensordp.a, libextdevice.a) call four
 * vendor timer entry points.  This port bars the whole hx_drv_timer_* API from
 * the image (check_placement_budget.py; AGENTS.md records it as an invariant)
 * because TIMER2 is the execution-profile time source and no name-based check
 * can tell which timer id a generic call carries.  Linking those archives as
 * they are would therefore fail the build.
 *
 * The seam resolves that WITHOUT weakening the gate: the four symbols are
 * redirected with -Wl,--wrap to the implementations in timer_seam.c, which
 * never call __real_*.  Nothing named hx_drv_timer_* except the one permitted
 * hx_drv_timer_init survives into the ELF, so the gate and the invariant stay
 * exactly as they were.
 *
 * See the board README for the disassembly evidence that the archives only
 * ever ask for TIMER_ID_0 (all 41 call sites pass a constant 0) and that the
 * delay entry points resolve to TIMER_ID_3 -- i.e. that this is a GATE
 * conflict, not a hardware conflict with TIMER2.
 */
#ifndef GROVE_TIMER_SEAM_H
#define GROVE_TIMER_SEAM_H

#include <stdint.h>

/**
 * @brief  First fault the seam latched, or NULL if it has never refused a call.
 *
 * The wrappers cannot log: hx_drv_timer_hw_stop() is reached from the vendor's
 * own Timer0 ISR callback, so a logging or blocking failure path there would
 * run inside an interrupt.  They latch a string literal instead and this
 * accessor surfaces it (the `epk` command prints it).
 */
const char *grove_timer_seam_fault(void);

/** @brief  How many times the seam refused a call (id != 0, bad config). */
uint32_t grove_timer_seam_refusals(void);

#endif /* GROVE_TIMER_SEAM_H */
