/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_cmd_core.h
 * @brief   What the shared `nor` (shell/cmds/cmd_nor.c, issue #250) lends a
 *          board's own `nor` subcommands.
 *
 * One thing: the report of an operation that ran under the board's hold, so
 * a board's own subcommand (grove-vision-ai-v2's `nor cycle`) says its result
 * in the same lines, and with the same dmesg line, as `nor erase` and
 * `nor write`.  The telnet and background question is nor_console.h's.
 */
#ifndef NOR_CMD_CORE_H
#define NOR_CMD_CORE_H

#include <stdint.h>

#include "nor_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * Print what @p r says (the JEDEC line when it has one, then one outcome) and
 * log one line to dmesg -- the console drops what a command prints after a
 * Ctrl+C (shell/core/cli_core.c), and the log is where it can still be read.
 *
 * @param what  "erase", "write", "cycle": the log line's word
 * @param addr, len  the request (len 0: an operation on no data)
 * @return 0 for NOR_SVC_OK, 1 otherwise
 */
int nor_cmd_report(struct cli_instance *sh, const char *what, uint32_t addr,
                   uint32_t len, const struct nor_svc_result *r);

/** The busy / down line of a hold that was not taken ("nor: busy -- ..."),
 *  logged like nor_cmd_report().  @return 1. */
int nor_cmd_not_held(struct cli_instance *sh, const char *what, uint32_t addr,
                     uint32_t len, const struct nor_svc_result *r);

#ifdef __cplusplus
}
#endif

#endif /* NOR_CMD_CORE_H */
