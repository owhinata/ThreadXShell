/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_tty.h
 * @brief   The one telnet console over NetX Duo, resident from boot, for every
 *          board with a NetX IP stack (issue #221, epic #212).
 *
 * shell/cmds/net_tty.c is the console: the transport's function table, the
 * receive and transmit rings, the telnet IAC handling, the listener thread,
 * the `net info` lines and the question "is this console the telnet one".  A
 * board defines the transport object (NET_TTY_TRANSPORT_DEFINE), binds a
 * CLI_INSTANCE_DEFINE to it with its own prompt, places the buffers and the
 * stack, and says where the port comes from and which of its commands refuse
 * to run on this console.
 *
 * [!] net_tty.c OWNS NO STORAGE (cmake/check_no_mutable_storage.py, per
 * board).  The board DEFINES the two objects below under their fixed names --
 * its mutable state and its constant facts -- and net_tty.c reaches them
 * through these extern declarations (the same shape as net_core.h).  NetX's
 * socket callbacks take no context argument, so nothing else could carry them.
 *
 * RESIDENT.  net_tty_init() (from tx_application_define(), after the board
 * created its NetX objects and net_core_init()) only creates the event flags
 * and the listener thread.  The thread waits for the port, which the board
 * hands over ONCE (net_tty_port_set(); a second call is ignored), then creates
 * the socket and listens -- NetX refuses both from the init context.  The
 * socket is never deleted: there is no start and no stop.  It listens with no
 * address and stays listening across link falls.  A create or listen that
 * fails leaves "not listening", the step and NetX's answer in the state for
 * `net info`, and is not retried.
 *
 * ONE ACCEPT PER CONNECTION.  The listener arms nx_tcp_server_socket_accept()
 * once and waits in it for ever.  A handshake that fails (NX_NOT_CONNECTED)
 * goes through unaccept -> relisten before the next arm: NetX leaves such a
 * socket listening while still bound, and arming it again would leak a packet
 * per call (issue #202, owhinata/wio-lite-ai#23).  An unaccept, a relisten or
 * an arm that fails stops the listener ("not listening", until reset).
 *
 * THE LINK.  net_core.c's notification (IP thread) counts every link fall in
 * net_core_state.link_falls, in the masked section that records the level.
 * The establish callback (IP thread too) copies that count when the
 * connection becomes established, so "did this session start before or after
 * the fall" is a comparison of two numbers taken on one thread.  Before a
 * session is published, and before every wait of the session, the listener
 * compares them; a difference ends the session.  The link worker calls
 * net_tty_link_fell() after each fall it handled, holding no lock: it only
 * wakes the listener.  THE COUNT DECIDES, THE EVENT ONLY WAKES.
 *
 * THE END OF A SESSION -- the peer's FIN / RST, a link fall, the socket out of
 * ESTABLISHED -- is one routine: close the write gate, throw away the transmit
 * ring, raise the cancel mark (the transport's read returns one 0x03 ahead of
 * the receive ring, so it reaches a running command even with the ring full)
 * and the receive discard (what the ring still holds, and anything until the
 * instance begins the next session, is consumed by the read and never
 * returned), tell the instance
 * it has input, disconnect (bounded), unaccept, relisten.
 *
 * Threads: the listener (NET_TTY_PRIORITY) owns the socket and is the only
 * sender; the IP thread runs the callbacks (receive, disconnect, establish,
 * queue depth), which only move bytes into the receive ring and set flags;
 * the CLI threads only write into the transmit ring.  This file takes no lock
 * of its own -- only ThreadX interrupt-masked sections and what NetX takes
 * inside its calls -- and never the address lock.
 */
#ifndef NET_TTY_H
#define NET_TTY_H

#include <stdbool.h>
#include <stdint.h>

#include "tx_api.h"
#include "nx_api.h"

#include "cli_uart_ring.h"
#include "net_svc.h"          /* net_svc_write_fn */

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;
struct cli_transport_api;

/** The port when the board has no other (both boards' default). */
#define NET_TTY_PORT_DEFAULT 23u

/** The listener's priority: below the IP threads (11, 12) and the DHCP
 *  thread (13), above the link worker (15) and the consoles (16), so a
 *  connection is accepted while a command computes. */
#define NET_TTY_PRIORITY 14u

/**
 * The console's no-progress deadline in ticks (ms), published through
 * cli_transport::tx_timeout.  It must exceed the socket's first retransmit
 * (the board's facts.rto_ticks), which the board checks statically: a lost
 * segment is recovered only after that retransmit, and a shorter deadline
 * drops the rest of a command's output (owhinata/wio-lite-ai#48).  It
 * deliberately does not cover a second retransmit of the same segment.
 */
#define NET_TTY_TX_DEADLINE 5000u

/** TCP receive window, bytes per sent segment, segments per drain pass, the
 *  pool packets the drain leaves to the receive path, the retry wait after a
 *  refusal (ticks), and the bounded FIN wait (ticks). */
#define NET_TTY_WINDOW       2048u
#define NET_TTY_MSS          1400u
#define NET_TTY_TX_BURST     8u
#define NET_TTY_POOL_RESERVE 8u
#define NET_TTY_DRAIN_POLL   50u
#define NET_TTY_DISC_WAIT    1000u

/** The listener: no port yet / listening / stopped for good. */
enum net_tty_listen {
	NET_TTY_L_NOPORT = 0,
	NET_TTY_L_LISTENING,
	NET_TTY_L_FAILED,
};

/** The step that stopped the listener (net_tty_state.fail_op). */
enum net_tty_op {
	NET_TTY_OP_NONE = 0,
	NET_TTY_OP_CREATE,       /**< nx_tcp_socket_create()            */
	NET_TTY_OP_NOTIFY,       /**< a callback NetX would not take    */
	NET_TTY_OP_LISTEN,       /**< nx_tcp_server_socket_listen()     */
	NET_TTY_OP_ACCEPT,       /**< nx_tcp_server_socket_accept()     */
	NET_TTY_OP_UNACCEPT,     /**< nx_tcp_server_socket_unaccept()   */
	NET_TTY_OP_RELISTEN,     /**< nx_tcp_server_socket_relisten()   */
};

/** How the last connection ended (net_tty_state.last_end). */
enum net_tty_end {
	NET_TTY_END_NONE = 0,
	NET_TTY_END_PEER,        /**< the peer's FIN / RST                */
	NET_TTY_END_LINK,        /**< the link fell                       */
	NET_TTY_END_LEFT,        /**< the socket left ESTABLISHED         */
};

/** Where the listener thread is (net_tty_state.phase). */
enum net_tty_phase {
	NET_TTY_PH_PORT = 0,     /**< waiting for the port                */
	NET_TTY_PH_ACCEPT,       /**< arming accept and waiting in it     */
	NET_TTY_PH_SESSION,      /**< a published session                 */
	NET_TTY_PH_STOPPED,      /**< failed; does nothing more           */
};

/** The console's counters since boot, for `net info`.  The refusals are
 *  kept apart: q is the designed back-pressure, w a peer that does not
 *  read, buf the shared pool, err anything else. */
struct net_tty_stats {
	uint32_t sessions;       /**< sessions published                  */
	uint32_t rx_drops;       /**< bytes lost: the receive ring full   */
	uint32_t tx_segs;        /**< TCP segments sent                   */
	uint32_t tx_bytes;       /**< bytes sent (telnet-encoded)         */
	uint32_t tx_hiwater;     /**< the transmit ring's peak, bytes     */
	uint32_t tx_qdepth;      /**< refused: transmit queue full        */
	uint32_t tx_win;         /**< refused: the peer's window          */
	uint32_t tx_nobuf;       /**< refused: pool empty / reserve       */
	uint32_t tx_err;         /**< refused: any other answer           */
	uint32_t tx_dropped;     /**< bytes a session queued and lost at
	                          *   its end (writes with the gate closed,
	                          *   no session, are not counted)       */
};

/**
 * The board's mutable state for net_tty.c, defined without an initializer.
 * The transport's init (cli_init()) writes @ref sh; net_tty_init() the rest.
 *
 * In ThreadX interrupt-masked sections (TX_DISABLE / TX_RESTORE): the write
 * gate with what is written into the transmit ring; live, peer_gone and the
 * gate together (the disconnect callback, the publish, the end); est_seen with
 * est_falls (the establish callback); the port with port_given; the cancel
 * mark's test-and-clear.  The receive ring has one writer (the IP thread) and
 * one reader (the instance thread); the transmit ring many writers (masked)
 * and one reader (the listener).
 */
struct net_tty_state {
	/** The instance bound to the transport (set by the transport's init). */
	struct cli_instance *sh;
	struct cli_uart_ring rx;
	struct cli_uart_ring tx;
	/** The write gate: opened by the instance thread in session_begin,
	 *  closed by the end of a session and by the disconnect callback. */
	volatile uint8_t connected;
	/** A session is published (the listener sets it, the end and the
	 *  disconnect callback clear it); session_begin opens the gate only
	 *  while it is set. */
	volatile uint8_t live;
	/** The disconnect callback ran for this connection. */
	volatile uint8_t peer_gone;
	/** The transport's next read returns one 0x03 first. */
	volatile uint8_t cancel;
	/** Set by the end of a connection, cleared by the instance thread in
	 *  session_begin after it emptied the receive ring: until then the
	 *  transport's read consumes what the ring holds and returns none of it,
	 *  so a connection's leftover bytes never reach the editor -- not even
	 *  after a command that never read ran past the next publish (the ring
	 *  keeps its one writer, the IP thread). */
	volatile uint8_t rx_discard;
	/** The establish callback ran for this connection; est_falls is valid. */
	volatile uint8_t est_seen;
	/** Telnet IAC receive state (IP thread only). */
	uint8_t iac;
	/** net_tty_init() created everything (written once). */
	uint8_t ready;
	/** net_tty_port_set() was called (the first call wins). */
	uint8_t port_given;
	/** enum net_tty_phase (listener only). */
	uint8_t phase;
	/** enum net_tty_listen, enum net_tty_op, enum net_tty_end. */
	volatile uint8_t listen;
	uint8_t fail_op;
	uint8_t last_end;
	uint16_t port;
	/** NetX's answer at the step that stopped the listener. */
	UINT fail_status;
	/** The link fall count at establishment / of the published session. */
	ULONG est_falls;
	ULONG sess_falls;
	NX_TCP_SOCKET sock;
	TX_THREAD thread;
	TX_EVENT_FLAGS_GROUP evt;
	struct net_tty_stats st;
};

/** The board's constant facts for net_tty.c. */
struct net_tty_facts {
	NX_IP          *ip;
	NX_PACKET_POOL *pool;
	/** The receive ring, the per-packet extraction buffer (IP thread) and
	 *  the transmit ring, placed by the board (CPU-only memory). */
	uint8_t *rx_buf;      ULONG rx_size;
	uint8_t *extract_buf; ULONG extract_size;
	uint8_t *tx_buf;      ULONG tx_size;
	/** The listener's stack. */
	void    *stack;       ULONG stack_size;
	/** nx_tcp_socket_transmit_configure(): queue depth, first retransmit
	 *  (ticks), retries, shift. */
	UINT     tx_queue_depth;
	ULONG    rto_ticks;
	UINT     rto_retries;
	UINT     rto_shift;
	/** The console a refused command is sent to ("the %s console"). */
	const char *other_console;
};

extern struct net_tty_state       net_tty_board_state;
extern const struct net_tty_facts net_tty_board_facts;

/** The transport's function table (shared, constant). */
extern const struct cli_transport_api net_tty_api;

/** Define the board's transport object @p _name over the shared table.  The
 *  board includes cli_instance.h for struct cli_transport. */
#define NET_TTY_TRANSPORT_DEFINE(_name)                                        \
	struct cli_transport _name = {                                         \
		.api        = &net_tty_api,                                    \
		.sh         = NULL,                                            \
		.ctx        = NULL,                                            \
		.tx_timeout = NET_TTY_TX_DEADLINE,                             \
	}

/**
 * Create the event flags and the listener thread; the ready mark is set last,
 * only when both succeeded.  From tx_application_define(), after the board
 * created its NetX objects and net_core_init(), and after cli_init() of the
 * instance bound to the transport.  Object creation only.  @return 0 or -1.
 */
int net_tty_init(void);

/**
 * The port to listen on.  The FIRST call wins and wakes the listener; any
 * later call is ignored.  Ignored before net_tty_init() succeeded.  Never
 * blocks (safe before the scheduler starts).
 */
void net_tty_port_set(uint16_t port);

/**
 * "The link fell" -- from the link worker, after it handled a fall and
 * released the address lock.  Only wakes the listener: no NetX call, no lock,
 * no wait, nothing counted (the count is net_core's).
 */
void net_tty_link_fell(void);

/**
 * One pass of the listener thread: wait for the port and open the socket, or
 * arm accept and wait in it, or one session pass (compare, drain, wait).  The
 * thread runs this until it returns -1 (stopped); the host test calls it
 * directly.  @return 0, or -1 once the listener has stopped.
 */
int net_tty_listener_step(void);

/** True when @p sh is the telnet console's instance, or a background job
 *  launched from it (sh->fg). */
bool net_tty_is_console(const struct cli_instance *sh);

/**
 * A board command that would destroy the session it runs on asks this first:
 * on the telnet console it prints why (naming the board's other console) and
 * returns non-zero; anywhere else it returns 0.
 */
int net_tty_guard(struct cli_instance *sh, const char *what);

/**
 * The telnet lines of `net info`, every value sampled first (one masked
 * section) and then written one whole line per call of @p write.  The board
 * calls it at the head of its own lines.  @return 0, or -1 after the write
 * that failed.
 */
int net_tty_write_lines(net_svc_write_fn write, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NET_TTY_H */
