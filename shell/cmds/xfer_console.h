/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    xfer_console.h
 * @brief   YMODEM over the console: the one wiring for every board (issue #231).
 *
 * shell/cmds/xfer_console.c wires the protocol core (svc/ymodem.c, transport-
 * agnostic) to the raw-console API (cli_console_claim / cli_read_byte /
 * cli_write / cli_rx_flush) for a source or sink the caller supplies: the
 * console hand-over, the RX flushes, the drain after a failed transfer, the
 * one-line result of a send and the log lines of both directions.  It is not
 * a command (there is no `xfer` command; issue #227 removed f746g-disco's).
 * Until issue #231 each board had its own cmds/cmd_xfer.c saying and doing the
 * same (issue #228 made the three alike).
 *
 * [!] xfer_console.c OWNS NO STORAGE (cmake/check_no_mutable_storage.py, per
 * board).  The board DEFINES xfer_console_board_facts below under that fixed
 * name, in its cmds/cmd_xfer_board.c: a pointer to its one struct ym_xfer
 * (svc/ymodem.h: the running mark and the two working areas, static in that
 * file and placed by the board) and its answer to "is this the telnet
 * console".  There is no default definition: a board that does not define it
 * fails to link.  This file is the only caller of ymodem_send() /
 * ymodem_recv(), so every transfer on a board uses that board's one owner.
 *
 * [!] AN EMPTY FIELD REFUSES.  When either field is NULL, every entry below
 * refuses before it touches the console (no claim, no flush, no read), so a
 * table written with a field left out cannot quietly lose the telnet refusal
 * or reach the protocol core without an owner.  Note the other direction is
 * not caught here: a board with a telnet console that names
 * xfer_console_no_telnet links, passes every check and loses the refusal --
 * which is what reading the shipped table back is for.
 */
#ifndef XFER_CONSOLE_H
#define XFER_CONSOLE_H

#include "ymodem.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * "Would this console destroy what a raw transfer needs" -- the telnet
 * question.  On the console it refuses it prints why and returns non-zero;
 * anywhere else it returns 0.  The boards with a telnet console name
 * net_tty_guard() (shell/cmds/net_tty.h); a board without one names
 * xfer_console_no_telnet().
 */
typedef int (*xfer_console_telnet_fn)(struct cli_instance *sh, const char *what);

/** What a board hands the wiring.  Both fields are required. */
struct xfer_console_facts {
	struct ym_xfer        *owner;     /**< the board's one transfer owner */
	xfer_console_telnet_fn telnet;    /**< the telnet question, see above */
};

/** Defined by the board (cmds/cmd_xfer_board.c), read by xfer_console.c. */
extern const struct xfer_console_facts xfer_console_board_facts;

/**
 * The answer of a board that has no telnet console: never refuses, prints
 * nothing.  For xfer_console_facts.telnet on such a board.
 */
int xfer_console_no_telnet(struct cli_instance *sh, const char *what);

/**
 * Is a raw transfer refused on this console (issue #228)?  0 when it may run;
 * 1 when it is refused, having said why: the telnet console (the board's
 * question, as "ymodem"), or a board table with a field left empty.  The send
 * and receive entries ask it as their first statement; a caller that would
 * break something before it gets to one of them (erase a slot, take a link
 * down) asks it itself first.
 */
int xfer_refuse_if_telnet(struct cli_instance *sh);

/**
 * Send @p src to the PC over the console using YMODEM: refused on the telnet
 * console before anything else is printed; claim the console (counted per
 * console, cli.h) and refuse a background job; refuse when another transfer
 * is running; then print the two-line notice, flush RX, run ymodem_send(),
 * flush again, drain until the line goes quiet if it failed (issue #228),
 * print a one-line result and log it, and release the console.  The caller
 * owns @p src and any underlying resource (frame validity) for the duration.
 * A local Ctrl+C aborts: while sending, every byte read back is protocol.
 * Returns 0 on a completed transfer, 1 on any failure / cancel / refusal.
 *
 * [!] FOR THE DURATION, THIS FUNCTION IS THE ONLY PERMITTED READER OF THE RX
 * RING: a source must not call cli_cancel_requested(), whose poll drains the
 * ring and discards every byte that is not 0x03 -- here, the receiver's ACK /
 * 'C' / CAN.
 */
int xfer_send_source(struct cli_instance *sh, const struct ym_source *src);

/**
 * xfer_send_source() for a caller that ALREADY holds the console (wio-lite-ai's
 * `wifi flash backup`): refused on the telnet console, then flush RX, run
 * ymodem_send(), flush again, drain if it failed, and print the one-line
 * result.  No claim, no notice, no "another transfer is running" question
 * (the caller asks it before its own notice).  Kept only until that caller
 * uses xfer_send_source() too, which is safe since claims are counted per
 * console (issue #229); removing this entry is a later step of issue #231.
 * Returns 0 on a completed transfer, 1 otherwise.
 */
int xfer_send_source_locked(struct cli_instance *sh, const struct ym_source *src);

/**
 * Receive one YMODEM file into @p sink.  The caller ALREADY holds the console
 * (cli_console_claim).  Refused on the telnet console; flushes RX, runs
 * ymodem_recv(), flushes again, and drains until the line goes quiet if it
 * failed.  Prints nothing but a refusal: the result and the counters go to the
 * log, and the caller says what was stored (issue #228).  The counters are
 * written to @p diag, the caller's record, which it has set with
 * ymodem_recv_diag_init() (issue #230): a refusal here leaves it at that value.
 *
 * A non-zero return means the batch did NOT complete -- including all the data
 * arriving but the sender's closing block not (ymodem_recv() reports that as
 * YM_ERR_TIMEOUT by design), so the caller discards what the sink accumulated.
 * Returns 0 when the batch completed, 1 otherwise.
 *
 * [!] FOR THE DURATION, THIS FUNCTION IS THE ONLY PERMITTED READER OF THE RX
 * RING: nothing in the sink may call cli_cancel_requested() -- its poll would
 * discard the sender's file.
 *
 * [!] AND THERE IS NO LOCAL Ctrl+C.  While receiving, the stream is the file,
 * so 0x03 is ordinary data -- about one byte in 256 of binary content, and it
 * lands in block CRCs too -- and cannot be told from a keypress.  wio-lite-ai
 * treated it as an abort and corrupted every `wifi flash imgload` until it
 * stopped.  Cancel from the PC instead (lrzsz sends CAN, which the protocol
 * core honours), or let the handshake time out.
 */
int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink,
                          struct ym_recv_diag *diag);

/**
 * Would a transfer started now be refused because another one is running
 * (issue #230)?  0 when it may start; 1 when it would be refused, having
 * printed "ymodem: another transfer is running -- run it again when that one
 * ends" (or, with a field of the board table left empty, why nothing can
 * run).  Ask it right before printing a notice that tells the operator to
 * start the PC side, so a refused transfer does not tell them to.  Only an
 * early answer: the protocol core decides, and a refusal it makes later (a
 * transfer started on another console in between) prints the same sentence
 * and is handled as any other failure.
 */
int xfer_refuse_if_busy(struct cli_instance *sh);

#ifdef __cplusplus
}
#endif

#endif /* XFER_CONSOLE_H */
