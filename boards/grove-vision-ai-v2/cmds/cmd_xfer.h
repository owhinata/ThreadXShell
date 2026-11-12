/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.h
 * @brief   YMODEM over the console, for this board (#92, #143).
 *
 * Receive for `blob write`, and since issue #143 send for `camera send` (the
 * last capture's planar B/G/R).  There is no `xfer` command here.
 *
 * f746g-disco and wio-lite-ai each have a cmd_xfer.c that says and does the
 * same (issue #228).  What differs: those two refuse on their telnet console
 * (shell/cmds/net_tty.c, shared since issue #221), which this board does not
 * have or link, and wio-lite-ai also has a send entry for a caller that already
 * holds the console (`wifi flash backup`).  Making the three one file is issue
 * #231.
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
 * quiet if it failed.  Prints nothing: the result and the counters go to the
 * log, and the caller says what was stored (issue #228).  The counters are
 * written to @p diag, the caller's record, which it has set with
 * ymodem_recv_diag_init() (issue #230).  Returns 0 when the batch completed,
 * 1 otherwise.
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
int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink,
                          struct ym_recv_diag *diag);

/**
 * @brief  Send @p src to the PC over YMODEM: says what it sends, claims the
 *         console, runs ymodem_send(), prints the one-line result (and logs
 *         it) and releases the console -- the same shape as the other boards'
 *         xfer_send_source().
 *
 * A failed send drains until the line goes quiet before it reports, like the
 * receive above.  A local Ctrl+C aborts (while sending, every byte read back is
 * protocol).  The source's read may refuse (the frame replaced, the camera
 * busy), which ends the transfer as a source error.  A background job is refused: the transfer
 * needs the foreground's RX.  Returns 0 on a completed transfer, 1 otherwise.
 */
int xfer_send_source(struct cli_instance *sh, const struct ym_source *src);

#ifdef __cplusplus
}
#endif

#endif /* CMD_XFER_H */
