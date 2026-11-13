/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer_board.c
 * @brief   This board's part of YMODEM over the console (issue #231): the one
 *          transfer owner and the telnet question, for shell/cmds/xfer_console.c.
 *
 * The wiring itself is shared (shell/cmds/xfer_console.h); `camera send`
 * (cmd_camera_board.c), `blob write` (cmd_blob_board.c) and `wifi flash
 * backup` / `imgload` (cmd_wifi_flash.c) go through it.
 */
#include "xfer_console.h"
#include "net_tty.h"     /* net_tty_guard(): the telnet console refuses (#228) */

/* ---- the transfers' working areas (issue #230) ---------------------------- *
 *
 * svc/ymodem.c and shell/cmds/xfer_console.c hold no writable storage; this
 * board owns its one struct ym_xfer and the two areas it points at, static
 * here, and xfer_console.c (the only caller of ymodem_send() / ymodem_recv())
 * reaches them through the table below only, so every transfer (`camera
 * send`, `blob write`, `wifi flash backup` and `imgload`) uses this one.
 *
 * The areas are ordinary .bss (the owner, which holds two pointers, is .data),
 * so AXI-SRAM -- where svc/ymodem.c's buffers already were.
 * Only the CPU touches them (io_put() copies the bytes into the console's TX
 * ring), and CPU-only data goes to DTCM first and to AXI-SRAM when DTCM has
 * no room (README "Memory map"): the two areas (2,053 B) are more than DTCM
 * had left at issue #230 (1,848 B).  The shared nn state (#161) and the telnet
 * state (#221) are placed the same way.
 */
static struct ym_send_area s_ym_send;
static struct ym_recv_area s_ym_recv;
static struct ym_xfer      s_ym = { .send = &s_ym_send, .recv = &s_ym_recv };

/* This board has the telnet console (shell/cmds/net_tty.c): a raw transfer is
 * refused there with its words. */
const struct xfer_console_facts xfer_console_board_facts = {
	.owner  = &s_ym,
	.telnet = net_tty_guard,
};
