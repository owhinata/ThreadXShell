/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    psram_guard.h
 * @brief   The OCTOSPI1 guard word: users of the memory and the one command
 *          that reconfigures the controller (issue #142).
 *
 * WHAT IT GUARDS.  Two kinds of caller touch OCTOSPI1:
 *
 *   - USERS read or write the memory through the memory-mapped window: a
 *     scan-out start, a capture, `nn bench`, `nn stream`, an mlperf run.  Any
 *     number of them may run at once -- they move bytes in different parts of
 *     the same memory, and none of them changes the controller.
 *   - A RECONFIGURER changes the controller itself: the `psram` tuning
 *     subcommands, `membench`, `devmem`, `wifi flash`.  It must have the bus to
 *     itself: a memory-mapped access to a half-configured OCTOSPI stalls the AXI
 *     until the watchdog fires.
 *
 * THE WORD.  One 32-bit word, so a decision and the transition it implies are
 * one atomic step:
 *
 *     bit 31        RECONF: a reconfigurer holds the bus
 *     bits 15..0    the number of users holding it
 *
 * A user is admitted while RECONF is clear (and the count has room); a
 * reconfigurer only while the word is ZERO -- no user and no other
 * reconfigurer.  So users exclude a reconfigurer in both directions and never
 * exclude each other.
 *
 * [!] THIS USED TO BE ONE BIT that users took too, so two users excluded each
 * other -- and `nn stream`, which holds its user place for the whole stream,
 * refused every later `lcd on`.  That was a lock taken on something it does
 * not protect (lcd on only sets LTDCEN and the backlight).
 *
 * The reconfigurer's other gates (scan-out live, a camera stream running) are
 * NOT in the word: they are asked AFTER the mark is set ("take, then look"),
 * and a failed look gives the mark back (psram_guard_reconf_give()).
 *
 * WHY PURE.  Each function maps the word to its successor, so the board
 * applies it with one LDREX/STREX retry loop and test/test_psram_guard.c walks
 * every pairing of the contention table on the host.
 */
#ifndef PSRAM_GUARD_H
#define PSRAM_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSRAM_GUARD_RECONF   0x80000000u
#define PSRAM_GUARD_COUNT    0x0000FFFFu
/** Users admitted at once.  Far above anything the shell can start; a word
 *  that reaches it is refused rather than wrapped into the RECONF bit. */
#define PSRAM_GUARD_MAX_USERS 0x00FFu

/* Each takes the current word in @p w and, when it returns 1, stores the
 * successor in *@p next.  0: refused, *@p next untouched. */

/** A user enters: refused while a reconfigurer holds the bus, or when full. */
int psram_guard_user_take(uint32_t w, uint32_t *next);
/** A user leaves: refused (and nothing moves) with no user to give back. */
int psram_guard_user_give(uint32_t w, uint32_t *next);
/** A reconfigurer enters: only on a word that is zero. */
int psram_guard_reconf_take(uint32_t w, uint32_t *next);
/** A reconfigurer leaves (or a failed gate gives the mark back): only on a
 *  word that is exactly the mark. */
int psram_guard_reconf_give(uint32_t w, uint32_t *next);

/** Users holding the bus, for a stats line. */
uint32_t psram_guard_users(uint32_t w);

#ifdef __cplusplus
}
#endif

#endif /* PSRAM_GUARD_H */
