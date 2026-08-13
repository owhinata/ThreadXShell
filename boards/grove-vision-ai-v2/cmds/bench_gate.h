/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    bench_gate.h
 * @brief   Shared entry gate for this board's CPU/memory benchmarks (#25).
 *
 * `coremark` and `membench` both report absolute numbers, and both of them are
 * only as true as the clock they are derived from.  On this part that clock is
 * not a constant the firmware chose -- the app inherits whatever the bootloader
 * programmed and reads it back through the SCU -- and there is no public TRM to
 * check the read-back against.  So the two commands share one gate that refuses
 * to run rather than print a number nobody can interpret.
 */
#ifndef BENCH_GATE_H
#define BENCH_GATE_H

#include <stdint.h>

struct cli_instance;

/**
 * Decide whether a benchmark may run, and hand back the core clock to scale
 * its results by.
 *
 * @param sh       shell instance the refusal is reported on
 * @param cmd      command name, used in the refusal text
 * @param core_hz  on success, the verified CM55M frequency in Hz
 * @return 1 to proceed; 0 after having printed why not
 */
int bench_gate_check(struct cli_instance *sh, const char *cmd,
                     uint32_t *core_hz);

/**
 * Re-read the core clock after a run and report if it moved.  A CoreMark run
 * lasts 10-100 s, which is plenty of time for something to reprogram the clock
 * tree underneath it; the entry check alone would then have blessed a
 * frequency that stopped being true halfway through.
 *
 * @param sh       shell instance the warning is reported on
 * @param cmd      command name, used in the warning text
 * @param core_hz  the value bench_gate_check() handed out before the run
 * @return 1 if unchanged; 0 after having warned that the results are suspect
 */
int bench_gate_recheck(struct cli_instance *sh, const char *cmd,
                       uint32_t core_hz);

#endif /* BENCH_GATE_H */
