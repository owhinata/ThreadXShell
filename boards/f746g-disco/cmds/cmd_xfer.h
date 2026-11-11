/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.h
 * @brief   Shared YMODEM-over-VCP send and receive entry points
 * (owhinata/stm32f746g-disco#50).
 *
 * cmd_xfer.c owns the console hand-over + YMODEM driving for an injected byte
 * source or sink.  `camera send` (cmd_camera_board.c) builds a RAM-frame source
 * and calls xfer_send_source(), so the console claim / RX flush / progress
 * messaging / result mapping live in one place; `blob write`
 * (cmd_blob_board.c) receives through xfer_recv_sink_locked().  There is no
 * `xfer` command (its `xfer send sd <path>` was removed in issue #227).
 */
#ifndef CMD_XFER_H
#define CMD_XFER_H

#include "ymodem.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * Send @p src to the PC over the shell's VCP using YMODEM: claim the console
 * (hold the output lock, enter raw mode), flush RX, run ymodem_send(), then
 * restore the console and print a one-line result.  The caller owns @p src and
 * any underlying resource (frame validity) for the duration.  On the telnet
 * console it is refused before anything else is printed (issue #228).
 * Returns 0 on a completed transfer, 1 on any failure / cancel.
 */
int xfer_send_source(struct cli_instance *sh, const struct ym_source *src);

/**
 * Receive one YMODEM file into @p sink (issue #194, for `blob write`).  The
 * caller ALREADY holds the console (cli_console_claim).  Flushes RX, runs
 * ymodem_recv(), flushes again, and drains until the line goes quiet if it
 * failed.  Refused on the telnet console (issue #228; `blob write` asks the
 * same question itself before it erases).  Returns 0 when the batch completed,
 * 1 otherwise.
 *
 * [!] FOR THE DURATION, THIS FUNCTION IS THE ONLY PERMITTED READER OF THE RX
 * RING: nothing in the sink may call cli_cancel_requested(), whose poll drains
 * the ring and discards every byte that is not 0x03 -- here, the file.
 *
 * [!] AND THERE IS NO LOCAL Ctrl+C.  While receiving, the stream is the file,
 * so 0x03 is ordinary data; cancel from the PC (lrzsz sends CAN, which the
 * protocol core honours).  grove-vision-ai-v2's cmd_xfer.h tells how
 * wio-lite-ai learned this.
 */
int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink);

#ifdef __cplusplus
}
#endif

#endif /* CMD_XFER_H */
