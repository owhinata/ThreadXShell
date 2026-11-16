/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    sdram_owner.h
 * @brief   Who is using the SDRAM for a test or a benchmark: nobody, `membench`
 *          or `sdram test` (issue #251).
 *
 * `membench` reads and writes its 64 KB buffer in bank0, which `sdram test`
 * overwrites, and until issue #251 neither looked at the other.  One word now
 * says which of the two runs; each takes it before it starts and gives it back
 * on every exit, so each refuses while the other runs -- and `membench`, as
 * before, refuses a second `membench`.  The test takes it as one of its steps
 * (cmds/cmd_sdram.c).  `membench` takes it for every region it measures, not
 * only for the SDRAM rows: the user's decision on issue #251 (a test keeps the
 * FMC busy, which disturbs the other rows too).
 *
 * The word is this board's (cmds/cmd_sdram.c), not the shared command's: a
 * shared translation unit owns no mutable storage.  Taken and tested under a
 * brief PRIMASK, as the old `membench` guard was: interrupt-safe, no one-time
 * init, and a take and its give may run on different threads.
 */
#ifndef SDRAM_OWNER_H
#define SDRAM_OWNER_H

enum sdram_owner {
	SDRAM_OWNER_NONE = 0,
	SDRAM_OWNER_MEMBENCH,
	SDRAM_OWNER_TEST,
};

/** Take the word for @p who.  @return SDRAM_OWNER_NONE when taken, otherwise
 *  who holds it (nothing changed). */
enum sdram_owner sdram_owner_take(enum sdram_owner who);

/** Give it back; only the holder calls this. */
void sdram_owner_give(void);

#endif /* SDRAM_OWNER_H */
