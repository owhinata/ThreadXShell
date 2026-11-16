/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_console.h
 * @brief   Where an erase or a write of the NOR may run: the one question for
 *          every board (issue #250, moved from wio-lite-ai's cmds/cmd_nor.c,
 *          issue #249).
 *
 * The commands that erase or write the external NOR (the shared `nor erase` /
 * `nor write` / `nor test`, shell/cmds/cmd_nor.c, and grove-vision-ai-v2's own
 * `nor cycle`) run in the foreground of the local console only: not on the
 * telnet console, not in a background job launched from it, and not in a
 * background job (`... &`) of the local console.  Each
 * asks nor_console_refuse() once its arguments and its range are known and
 * before it takes the board's hold on the device.
 *
 * WHETHER THE BOARD HAS A TELNET CONSOLE IS NOT THE BOARD'S TO SAY.  It comes
 * from SHELL_HAS_NET_TTY, which the top-level CMakeLists.txt defines for
 * nor_console.c alone, from BOARD_HAS_NETX -- the same variable that decides
 * whether shell/cmds/net_tty.c is compiled (cmake/README.md).  A board
 * cannot write "no telnet" here: with a telnet console the define is 1 and
 * the question goes to net_tty_is_console(), which a build without net_tty.c
 * cannot link.  Left undefined, nor_console.c does not compile.
 *
 * [!] nor_console.c OWNS NO STORAGE (cmake/check_no_mutable_storage.py, per
 * board) and includes no board header.
 */
#ifndef NOR_CONSOLE_H
#define NOR_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * Refuse an erase or a write that does not run in the foreground of the local
 * console.  On refusal it prints one line that says only the fact -- "nor:
 * erase/write only in the <local> foreground, not <telnet | a telnet job | a
 * job>" (without a telnet console: "...only in the foreground, not a job") --
 * and returns non-zero; otherwise it returns 0 and prints nothing.  It reads
 * the instance and, with a telnet console, asks net_tty_is_console(); it
 * touches no device and takes no lock.
 */
int nor_console_refuse(struct cli_instance *sh);

#ifdef __cplusplus
}
#endif

#endif /* NOR_CONSOLE_H */
