/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_shell.h
 * @brief   TCP network shell (telnet) over NetX Duo (owhinata/stm32f746g-disco#49
 * P4).
 *
 * Bridges the clean-room CLI shell's transport abstraction to a NetX TCP server
 * socket: a single network shell session on port 23 (telnet/nc), bound to one
 * cli_instance.  This file owns the socket lifecycle + the cli_transport vtable
 * implementation (it needs both nx_api.h and the shell headers, so it lives in
 * the NetX glue layer rather than shell/backend, keeping the shell library free
 * of any NetX dependency).
 *
 * Wiring: src/main.c defines a cli_instance bound to @ref nx_shell_transport and
 * adds it to shells[], then calls nx_shell_init() once the instances have
 * started.  The instance's own thread runs the shell; this module only feeds its
 * RX ring from the socket, sends its output, and signals (re)connect/disconnect.
 */
#ifndef NX_SHELL_H
#define NX_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cli_transport;

/** The transport a cli_instance binds to (CLI_INSTANCE_DEFINE in src/main.c). */
extern struct cli_transport nx_shell_transport;

/** Create the server socket, listen on port 23, spawn the server thread.  Call
 *  after nx_net_init() and after the shell instances have started.  Returns 0,
 *  or <0 (IP not up / socket / listen / thread create failed). */
int nx_shell_init(void);

/**
 * Output-path statistics for `net info` (issue #6).  The three refusal reasons are
 * separate on purpose: @ref tx_qdepth is the designed back-pressure and a large
 * value is normal, @ref tx_win means the peer is not reading, and @ref tx_nobuf is
 * the one that says the shared packet pool is the constraint.  @ref tx_hiwater is
 * the deepest the TX ring has ever been -- how close this console came to making
 * the shell wait.
 */
struct nx_shell_stats {
	bool     connected;    /**< a client is attached and output is enabled     */
	uint32_t sessions;     /**< clients accepted since boot                    */
	uint32_t tx_segs;      /**< TCP segments transmitted                       */
	uint32_t tx_bytes;     /**< payload bytes transmitted                      */
	uint32_t tx_hiwater;   /**< peak TX ring occupancy, bytes                  */
	uint32_t tx_qdepth;    /**< refused: socket transmit queue full            */
	uint32_t tx_win;       /**< refused: peer window closed                    */
	uint32_t tx_nobuf;     /**< refused: packet pool empty / below the reserve */
	uint32_t tx_err;       /**< refused: any other send error                  */
	uint32_t tx_dropped;   /**< bytes discarded because the session ended      */
};

/** Fill @p out; false if the network is not up (nothing to report). */
bool nx_shell_stats_get(struct nx_shell_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_SHELL_H */
