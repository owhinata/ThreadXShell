/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.h
 * @brief   YMODEM receive over the console, for this board (#92).
 *
 * The receive direction only: this board has one thing that wants a file from
 * the PC, and that is `blob write`.  There is no `xfer` command here and no
 * send direction -- nothing on this board produces a file to send.
 *
 * [!] THE THIRD COPY OF THIS WIRING, and deliberately so.  f746g-disco and
 * wio-lite-ai each have a cmd_xfer.c and the three are nearly the same twenty
 * lines; sharing them needs a way for a board to say "not over telnet", which
 * wio needs and this board has no concept of.  Inventing that hook is a
 * mechanism, and a mechanism is where the line gets drawn: it belongs in its
 * own issue rather than inside a transfer that has to land first.
 */
#ifndef CMD_XFER_H
#define CMD_XFER_H

#include "ymodem.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * @brief  Receive one YMODEM file into @p sink.  The caller ALREADY holds the
 *         console (cli_console_claim).
 *
 * Flushes RX, runs ymodem_recv(), flushes again, and drains until the line goes
 * quiet if it failed.  Returns 0 when the batch completed, 1 otherwise.
 *
 * [!] FOR THE DURATION, THIS FUNCTION IS THE ONLY PERMITTED READER OF THE RX
 * RING.  Nothing in the sink may call cli_cancel_requested(): the poll behind
 * it drains the ring and discards every byte that is not 0x03, which here is
 * the sender's file.
 *
 * [!] AND THERE IS NO LOCAL Ctrl+C.  While receiving, the incoming stream is
 * the file, so 0x03 is ordinary data -- about one byte in 256 of binary content
 * -- and cannot be told from a keypress.  wio-lite-ai treated it as an abort
 * and corrupted every transfer until it stopped.  Cancel from the PC instead;
 * lrzsz sends CAN, which the protocol core honours.
 *
 * [!] A FAILED TRANSFER DRAINS BEFORE IT REPORTS.  A sender that has just been
 * sent CAN keeps retrying for a moment, and anything still arriving when this
 * returns reaches the line editor and is EXECUTED -- the donor watched a
 * rejected sender's filename run at the prompt.  The drain is a mitigation and
 * not a proof: a peer that pauses past the quiet window can still get bytes to
 * the prompt.
 */
int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink);

#ifdef __cplusplus
}
#endif

#endif /* CMD_XFER_H */
