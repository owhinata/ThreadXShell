/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_target.h
 * @brief   The plugin target word THIS translation unit is being compiled for
 *          (issue #108).
 *
 * A board declares its plugin target word once, in its board.cmake, and hands
 * that one value to the packer, the host container verifier and its own
 * firmware policy.  Until issue #108 nothing checked the value itself -- the
 * three consumers agreed with each other because they read the same variable.
 * This header is the firmware's half of the check: it derives the word from the
 * compiler's own predefined macros, so a board adapter can write
 *
 *     _Static_assert(BOARD_TARGET_ID == PLUGIN_TARGET_ID_HERE, "...");
 *
 * and a word that does not describe the environment this firmware provides
 * fails the firmware's build.  The other half is the plugin image gate
 * (cmake/check_plugin_image.py), which derives what it can from the plugin ELF.
 *
 * [!] THE CMSE BIT IS CHECKED HERE AND NOWHERE ELSE.  It describes the base's
 * security state, not how the plugin was compiled (see PLUGIN_TARGET_CMSE), and
 * no image records it -- so the gate masks it out and says so, and this is the
 * only place a wrong CMSE bit can be caught.
 *
 * TWO TRAPS, both measured with the pinned arm-none-eabi-gcc 15.2:
 *
 *   - __ARM_FP DOES NOT IDENTIFY THE FPU ON ITS OWN.  It is 4 for f746's
 *     fpv5-sp-d16, and 14 for BOTH wio's fpv5-d16 and Grove's fp-armv8; only
 *     __ARM_ARCH (7 vs 8) separates the last two.  So the PAIR is mapped, never
 *     __ARM_FP alone.
 *   - CMSE IS TESTED AS __ARM_FEATURE_CMSE == 3, NOT defined(...).  A
 *     Cortex-M55 build WITHOUT -mcmse still defines it, as 1 (the architecture
 *     has the extension; only -mcmse makes this code a Secure image).  #ifdef
 *     would set the bit on a build that is not Secure.
 *
 * [!] ONLY THE PAIRS A BOARD HERE BUILDS PLUGINS FOR ARE MAPPED, and anything
 * else is a compile error rather than a guess.  (7, 4) is deliberately absent:
 * it is what f746's fpv5-sp-d16 predefines, and ALSO exactly what a Cortex-M4
 * with fpv4-sp-d16 predefines -- the macros cannot tell the two apart, and f746
 * builds no plugins.  Mapping it would state a CPU this header cannot know.
 * (The gate CAN tell them apart on the plugin side, but not by the core's name:
 * an M7 object records Tag_CPU_name "7E-M", the same as an M4.  What differs is
 * Tag_FP_arch -- FPv4 against FPv5 -- which __ARM_FP does not carry.)
 */
#ifndef PLUGIN_TARGET_H
#define PLUGIN_TARGET_H

#include "plugin_abi.h"

#if !defined(__arm__) || !defined(__ARM_ARCH_PROFILE) || \
    __ARM_ARCH_PROFILE != 'M'
#error "plugin_target.h describes an M-profile firmware build; include it only there"
#endif

#if __ARM_ARCH == 7 && defined(__ARM_ARCH_7EM__) && defined(__ARM_FP) && \
    __ARM_FP == 14
/* -mcpu=cortex-m7 -mfpu=fpv5-d16 (wio-lite-ai) */
#define PLUGIN_CPU_HERE  PLUGIN_CPU_CORTEX_M7
#define PLUGIN_FPU_HERE  PLUGIN_FPU_FPV5_D16
#elif __ARM_ARCH == 8 && defined(__ARM_ARCH_8M_MAIN__) && defined(__ARM_FP) && \
    __ARM_FP == 14 && !defined(__ARM_FEATURE_PAUTH) && !defined(__ARM_FEATURE_BTI)
/*
 * -mcpu=cortex-m55 (grove-vision-ai-v2).
 *
 * [!] NOT EVERY v8.1-M CORE WITH THIS FPU IS AN M55.  A Cortex-M85 has the same
 * architecture and FPU macros; by default it also predefines
 * __ARM_FEATURE_PAUTH and __ARM_FEATURE_BTI (PACBTI), which an M55 cannot, so
 * those are refused here.  -mcpu=cortex-m85+nopacbti predefines EXACTLY what an
 * M55 does (measured, GCC 15.2) and passes here.  The macros cannot know; the
 * linked firmware can -- v8.1-M images record the core's name -- so
 * cmake/check_target_word.py checks the same word against the firmware
 * image's own attributes after the link, and the plugin gate does the same for
 * each plugin.
 */
#define PLUGIN_CPU_HERE  PLUGIN_CPU_CORTEX_M55
#define PLUGIN_FPU_HERE  PLUGIN_FPU_FP_ARMV8
#else
#error "no plugin target is defined for this (__ARM_ARCH, __ARM_FP) pair and feature set"
#endif

#if defined(__ARM_PCS_VFP)
#define PLUGIN_FLOAT_ABI_HERE  PLUGIN_FLOAT_ABI_HARD
#else
/* softfp passes floats in core registers too: for a CALL, it is soft. */
#define PLUGIN_FLOAT_ABI_HERE  PLUGIN_FLOAT_ABI_SOFT
#endif

#if defined(__ARM_BIG_ENDIAN)
#define PLUGIN_BIG_ENDIAN_HERE 1
#else
#define PLUGIN_BIG_ENDIAN_HERE 0
#endif

#if defined(__ARM_FEATURE_CMSE) && __ARM_FEATURE_CMSE == 3
#define PLUGIN_CMSE_HERE 1
#else
#define PLUGIN_CMSE_HERE 0
#endif

/** The word this translation unit's own build describes. */
#define PLUGIN_TARGET_ID_HERE                                                 \
	PLUGIN_TARGET_ID(PLUGIN_CPU_HERE, PLUGIN_FPU_HERE,                    \
	                 PLUGIN_FLOAT_ABI_HERE, PLUGIN_BIG_ENDIAN_HERE,       \
	                 PLUGIN_CMSE_HERE)

#endif /* PLUGIN_TARGET_H */
