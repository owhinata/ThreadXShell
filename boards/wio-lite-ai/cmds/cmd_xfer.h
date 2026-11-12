/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.h
 * @brief   YMODEM-over-console send and receive entry points.
 *
 * cmd_xfer.c owns the console hand-over + YMODEM driving for an injected byte
 * source or sink, so every sender (owhinata/wio-lite-ai#19's `wifi flash backup`,
 * and `camera send`) and every receiver (`wifi flash imgload`, `blob write`) shares
 * one copy of the RX flush / drain / result reporting.  No `xfer` command is
 * planned (issue #227 removed f746g-disco's).
 *
 * Ported from ../stm32f746g-disco (its owhinata/stm32f746g-disco#50) for
 * owhinata/wio-lite-ai#19 M4, without its `xfer` command.  The receive side has
 * only the "already held" entry: both receivers claim the console themselves.
 */
#ifndef CMD_XFER_H
#define CMD_XFER_H

#include "ymodem.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * Send @p src to the PC over the shell's console using YMODEM, assuming the
 * caller ALREADY holds the console (cli_console_claim).  Flushes RX, runs
 * ymodem_send(), flushes RX again, drains until the line goes quiet if it
 * failed, and prints a one-line result (also logged).
 *
 * Use this from a command that must keep the console claimed across the whole
 * operation (e.g. `wifi flash backup`, which holds an RTL8720 download session
 * open around the transfer): cli_console_claim() is NOT reentrant -- it sets the
 * plain `cli_xfer_active` flag that cli_console_release() unconditionally clears
 * (shell/core/cli_printf.c), so a nested claim/release pair would drop raw mode
 * while the outer caller still needs it.
 *
 * CRITICAL -- console RX ownership: for the duration of ymodem_send() the ONLY
 * permitted reader of the console RX ring is this function's io_getc().  A data
 * source must NOT poll cli_cancel_requested() (directly or via an abort hook):
 * cli_cancel_poll() drains the RX ring and DISCARDS every non-0x03 byte, which
 * would swallow the receiver's ACK/'C'/CAN and break the protocol.  Sources run
 * with their abort hook disabled; a local Ctrl+C is picked up by io_getc() and
 * mapped to YM_IO_ABORT instead.
 *
 * Returns 0 on a completed transfer, 1 on any failure / cancel.
 */
int xfer_send_source_locked(struct cli_instance *sh, const struct ym_source *src);

/**
 * Claim the console, call xfer_send_source_locked(), then release it.  This is
 * the f746 `xfer_send_source()` semantic, for callers that do not already own
 * the console.  On the telnet console it is refused before anything else is
 * printed (issue #228).  Returns 0 on a completed transfer, 1 on any failure / cancel
 * (including a background-job rejection).
 */
int xfer_send_source(struct cli_instance *sh, const struct ym_source *src);

/**
 * Receive a YMODEM file from the PC into @p sink over the shell's console,
 * assuming the caller ALREADY holds it (cli_console_claim).  The mirror of
 * xfer_send_source_locked(): flushes RX, runs ymodem_recv() and flushes RX again.
 * Unlike the send it prints nothing (issue #228): the result and the counters go
 * to the log, and the caller says what was staged or stored and that it was not.
 * Added for owhinata/wio-lite-ai#19 M5 (`wifi flash imgload`).
 *
 * CRITICAL -- console RX ownership: exactly as on the send side, for the duration
 * of ymodem_recv() the ONLY permitted reader of the console RX ring is this
 * function's io_getc_recv().  Nothing in the sink may poll cli_cancel_requested()
 * (directly or via an abort hook): cli_cancel_poll() drains the RX ring and
 * DISCARDS every non-0x03 byte, which here would swallow the SENDER's block data
 * and destroy the transfer.
 *
 * NO LOCAL Ctrl+C, unlike the send side: while receiving, the incoming stream is
 * the file itself, so a 0x03 is ordinary data (~1 byte in 256 of binary content,
 * and it lands in block CRCs too) and cannot be told apart from a keypress --
 * treating it as an abort corrupted every transfer until io_getc_recv() stopped
 * doing so.  Abort from the PC instead (cancel the sender; lrzsz emits CAN, which
 * the protocol core honours), or let the handshake time out.
 *
 * A non-zero return means the batch did NOT complete -- including the case where
 * all the data arrived but the sender's closing block never did (ymodem_recv()
 * reports that as YM_ERR_TIMEOUT by design).  Callers must discard whatever the
 * sink accumulated in that case.  Returns 0 on a completed transfer, 1 otherwise.
 *
 * ON FAILURE the console RX is drained until the line goes quiet, not merely
 * flushed once: a peer that has just been sent CAN keeps retrying for a moment,
 * and anything still arriving when this returns would reach the line editor and be
 * EXECUTED as a command (owhinata/wio-lite-ai#10 saw a rejected sender's filename
 * run at the prompt).  So a failed transfer costs up to a second of draining
 * before it reports; a successful one is unchanged.  The drain is a mitigation,
 * not a proof of quiescence -- a peer that pauses past the quiet window, or one
 * still talking at the byte cap (which says so, loudly), can still get bytes to
 * the prompt.
 */
int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink);

#ifdef __cplusplus
}
#endif

#endif /* CMD_XFER_H */
