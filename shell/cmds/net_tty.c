/* shared-command: optional BOARD_HAS_NETX */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_tty.c
 * @brief   The one telnet console over NetX Duo (issue #221).  See net_tty.h.
 *
 * [!] THIS FILE OWNS NO STORAGE: the state and the facts are the board's
 * (net_tty_board_state / net_tty_board_facts), reached by name.
 *
 * [!] NO BOARD IN HERE: no `#ifdef <BOARD>`, no board header, no HAL, no log.
 * What differs between boards is in the facts; what the console did is in the
 * state and reaches the operator through `net info`.
 *
 * Replaces f746g-disco's port/netxduo/nx_shell.c and wio-lite-ai's
 * src/net_shell.c, which had the same design (owhinata/stm32f746g-disco#49 P4,
 * owhinata/wio-lite-ai#23 U4-2, #48, #49): a receive ring fed by the IP
 * thread, a transmit ring drained by the listener alone, IAC stripped on the
 * way in and 0xFF doubled on the way out.
 */
#include "net_tty.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "cli.h"
#include "cli_config.h"       /* CLI_INSTANCE_PRIORITY */
#include "cli_instance.h"     /* struct cli_transport(_api), notify_*  */
#include "fmt.h"
#include "net_core.h"         /* net_core_board_state.link_falls      */

/*
 * The NetX configuration this file needs, declared where it is used, so a board
 * without it fails here at build time rather than at boot with "not listening"
 * (issue #221; the board's port/netxduo/nx_user.h sets both):
 *  - the establish callback, which takes the fall count; without it no
 *    connection is ever published (nx_api.h turns its absence into
 *    NX_DISABLE_EXTENDED_NOTIFY_SUPPORT, and the call answers NX_NOT_SUPPORTED);
 *  - the queue-depth callback, the drain's wake after a peer ACK.
 */
#ifdef NX_DISABLE_EXTENDED_NOTIFY_SUPPORT
#error "net_tty.c needs NX_ENABLE_EXTENDED_NOTIFY_SUPPORT (the establish callback) in nx_user.h"
#endif
#ifndef NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY
#error "net_tty.c needs NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY in nx_user.h"
#endif

/* The listener accepts while a console computes, and must not run before the
 * link worker's notification has returned (it is above the worker). */
CLI_STATIC_ASSERT(NET_TTY_PRIORITY < CLI_INSTANCE_PRIORITY,
                  "the telnet listener must outrank the consoles");
CLI_STATIC_ASSERT(NET_TTY_PRIORITY < NET_CORE_WORKER_PRIORITY,
                  "the telnet listener must outrank the link worker");

/* The listener's events.  None carries anything: the state does. */
#define NET_TTY_EVT_PORT ((ULONG)0x1u)   /* the port was handed over        */
#define NET_TTY_EVT_TX   ((ULONG)0x2u)   /* output queued / socket took more */
#define NET_TTY_EVT_DISC ((ULONG)0x4u)   /* the peer disconnected            */
#define NET_TTY_EVT_FELL ((ULONG)0x8u)   /* the link worker handled a fall   */

/* The longest line of `net info` here: about 100 characters of text and ten
 * 32-bit numbers. */
#define NET_TTY_LINE_MAX 192u

/* ---- small pieces ----------------------------------------------------------- */

/* Wake the listener.  The rings and the state, never this flag, are the
 * truth; before init there is no group to set. */
static void net_tty_wake(ULONG what)
{
	if (net_tty_board_state.ready)
		(void)tx_event_flags_set(&net_tty_board_state.evt, what, TX_OR);
}

/* net_core's link fall count.  Callers are inside a masked section: the
 * notification writes it inside one (net_core.c). */
static ULONG net_tty_falls_now(void)
{
	return net_core_board_state.link_falls;
}

/*
 * Drop everything queued.  CALL ONLY WITH THE GATE ALREADY CLOSED: what stops
 * a writer appending straight after this is the gate being read inside the
 * writers' masked section.  The TX notify after it wakes a writer parked on
 * CLI_EVT_TX, which re-enters write() and returns on the closed gate.  What
 * it drops was accepted for a session and never sent: that is the loss
 * `dropped` counts (bytes only get in while the gate is open).
 */
static void net_tty_tx_discard(void)
{
	struct net_tty_state *s = &net_tty_board_state;
	size_t n = cli_uart_ring_count(&s->tx);

	if (n != 0u) {
		cli_uart_ring_advance_tail(&s->tx, n);
		s->st.tx_dropped += (uint32_t)n;
	}
	if (s->sh != NULL)
		cli_transport_notify_tx(s->sh);
}

/* ---- the transport ------------------------------------------------------------ */

static int net_tty_tr_init(struct cli_transport *tr)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;

	TX_DISABLE
	s->connected = 0u;
	TX_RESTORE
	s->sh = tr->sh;              /* cli_init() set it before calling this */
	return 0;
}

static int net_tty_tr_enable(struct cli_transport *tr)
{
	(void)tr;                    /* the listener opens the socket */
	return 0;
}

/*
 * The cancel mark first, then the ring.  The mark is how the end of a session
 * reaches a running command: cli_cancel_poll() reads through here, and a mark
 * does not depend on room in the ring.  After an end, until the instance
 * begins the next session (session_begin), what the ring holds may belong to
 * a connection that is gone: it is consumed
 * here and not returned, so no leftover line runs in the idle editor.  The
 * ring has one writer (the IP thread) and this one reader.
 */
static int net_tty_tr_read(struct cli_transport *tr, uint8_t *data, size_t cap)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;
	uint8_t c, drop, junk;
	size_t n = 0u;

	(void)tr;
	if (cap == 0u)
		return 0;
	TX_DISABLE
	c = s->cancel;
	s->cancel = 0u;
	drop = s->rx_discard;
	TX_RESTORE
	if (c != 0u)
		data[n++] = 0x03u;
	if (drop) {
		while (cli_uart_ring_get(&s->rx, &junk))
			;
		return (int)n;
	}
	n += cli_uart_ring_get_buf(&s->rx, data + n, cap - n);
	return (int)n;
}

/*
 * NON-BLOCKING (cli_instance.h): enqueue what fits, return the count.  No NetX
 * call: the listener is the only sender.  Several threads reach it (the
 * instance, a background job through sh->fg, printf handed over by a board's
 * backend), so the append is masked -- and the gate is read INSIDE that
 * section, which makes the end's "close the gate, then discard" a hard cut.
 * A literal 0xFF goes in as IAC IAC (RFC 854), both halves or neither.
 */
static int net_tty_tr_write(struct cli_transport *tr, const uint8_t *data,
                            size_t len)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;
	size_t i = 0u, count;
	int open;

	(void)tr;
	/* Nobody attached: swallow it, so the shell never waits on a console
	 * nobody drains.  Not a loss of anything a session was promised (the
	 * editor's ^C and prompt after an end land here), so not counted. */
	if (!s->connected)
		return (int)len;
	TX_DISABLE
	open = (s->connected != 0u);
	if (open) {
		while (i < len) {
			uint8_t b = data[i];

			if (b == 0xFFu) {
				if (cli_uart_ring_free(&s->tx) < 2u)
					break;
				(void)cli_uart_ring_put(&s->tx, 0xFFu);
				(void)cli_uart_ring_put(&s->tx, 0xFFu);
			} else if (!cli_uart_ring_put(&s->tx, b)) {
				break;
			}
			i++;
		}
	}
	count = cli_uart_ring_count(&s->tx);
	TX_RESTORE

	if ((uint32_t)count > s->st.tx_hiwater)
		s->st.tx_hiwater = (uint32_t)count;
	/* No wake in the normal case: flush() wakes the drain once the whole
	 * unit is in, so a redraw leaves as one segment.  A short write is the
	 * exception -- the core then waits for room only the drain makes. */
	if (i < len)
		net_tty_wake(NET_TTY_EVT_TX);
	/* The gate closed under us: the rest is swallowed like the entry check
	 * (and not counted the same way; what was already in the ring is). */
	if (i < len && (!open || !s->connected))
		return (int)len;
	return (int)i;
}

/* "That was a whole unit of output" (cli_unlock()): wake the drain. */
static void net_tty_tr_flush(struct cli_transport *tr)
{
	(void)tr;
	if (cli_uart_ring_count(&net_tty_board_state.tx) != 0u)
		net_tty_wake(NET_TTY_EVT_TX);
}

/* Character-at-a-time mode for a telnet client: IAC WILL ECHO, IAC WILL
 * SUPPRESS-GO-AHEAD.  Stored raw (they ARE IAC sequences) and whole. */
static const uint8_t net_tty_charmode[] = {
	0xFFu, 0xFBu, 0x01u,
	0xFFu, 0xFBu, 0x03u,
};

/*
 * The instance thread, on CLI_EVT_CONN, between resetting the editor and
 * drawing the prompt -- so between commands: the previous session's
 * foreground command has finished, and a cancel mark still set has nothing
 * left to cancel (left set, it would reach the new editor as a ^C).  The
 * receive ring is drained here, on the reader's side.  The gate opens only
 * while the session is live, read in the same masked section that opens it
 * and stores the negotiation.
 */
static void net_tty_tr_session_begin(struct cli_transport *tr)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;
	uint8_t junk;
	uint8_t armed;

	(void)tr;
	TX_DISABLE
	s->cancel = 0u;
	TX_RESTORE
	while (cli_uart_ring_get(&s->rx, &junk))
		;
	TX_DISABLE
	/* Only here, after the ring was emptied on this side: until the instance
	 * reaches this, whatever the ring holds may still be a gone connection's
	 * (a command that never read could have run past the next publish). */
	s->rx_discard = 0u;
	armed = s->live;
	s->connected = armed;
	if (armed && cli_uart_ring_free(&s->tx) >= sizeof net_tty_charmode)
		(void)cli_uart_ring_put_buf(&s->tx, net_tty_charmode,
		                            sizeof net_tty_charmode);
	TX_RESTORE
	if (armed)
		net_tty_wake(NET_TTY_EVT_TX);
}

const struct cli_transport_api net_tty_api = {
	.init          = net_tty_tr_init,
	.enable        = net_tty_tr_enable,
	.write         = net_tty_tr_write,
	.read          = net_tty_tr_read,
	.uninit        = NULL,
	.update        = NULL,
	.session_begin = net_tty_tr_session_begin,
	.flush         = net_tty_tr_flush,
};

/* ---- NetX callbacks (the IP thread: rings and flags only) -------------------- */

/* Drop IAC + command [+ option]; IAC IAC is one literal 0xFF.  1 when @p b is
 * negotiation, 0 when it is shell data. */
static int net_tty_iac_consume(uint8_t b)
{
	struct net_tty_state *s = &net_tty_board_state;

	switch (s->iac) {
	case 0u:
		if (b == 0xFFu) { s->iac = 1u; return 1; }
		return 0;
	case 1u:
		if (b == 0xFFu) { s->iac = 0u; return 0; }
		if (b >= 0xFBu && b <= 0xFEu) { s->iac = 2u; return 1; }
		s->iac = 0u;
		return 1;
	default:
		s->iac = 0u;
		return 1;
	}
}

/* Every packet, in pieces of the extraction buffer to its end. */
static VOID net_tty_rx_notify(NX_TCP_SOCKET *sock)
{
	struct net_tty_state *s = &net_tty_board_state;
	const struct net_tty_facts *f = &net_tty_board_facts;
	NX_PACKET *pkt;
	int stored = 0;

	while (nx_tcp_socket_receive(sock, &pkt, NX_NO_WAIT) == NX_SUCCESS) {
		ULONG off = 0u;

		while (off < pkt->nx_packet_length) {
			ULONG got = 0u, i;

			if (nx_packet_data_extract_offset(pkt, off, f->extract_buf,
			                                  f->extract_size, &got)
			    != NX_SUCCESS || got == 0u)
				break;
			off += got;
			for (i = 0u; i < got; i++) {
				if (net_tty_iac_consume(f->extract_buf[i]))
					continue;
				if (!cli_uart_ring_put(&s->rx, f->extract_buf[i]))
					s->st.rx_drops++;
				else
					stored = 1;
			}
		}
		(void)nx_packet_release(pkt);
	}
	if (stored && s->sh != NULL)
		cli_transport_notify_rx(s->sh);
}

/* The peer's FIN / RST: close the gate and mark the connection gone in one
 * section (session_begin and the publish read them together), then wake the
 * listener, which ends the session. */
static VOID net_tty_disconnect_cb(NX_TCP_SOCKET *sock)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;

	(void)sock;
	TX_DISABLE
	s->connected = 0u;
	s->live = 0u;
	s->peer_gone = 1u;
	TX_RESTORE
	net_tty_wake(NET_TTY_EVT_DISC);
}

/* ESTABLISHED, before NetX resumes the accept (nx_tcp_socket_state_syn_received.c):
 * take the link's fall count on the thread that counts the falls.  The IAC
 * state starts again with the connection. */
static VOID net_tty_establish_cb(NX_TCP_SOCKET *sock)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;

	(void)sock;
	TX_DISABLE
	s->est_falls = net_tty_falls_now();
	s->est_seen = 1u;
	TX_RESTORE
	s->iac = 0u;
}

/* A peer ACK freed the transmit queue: wake the drain (not the CLI -- only
 * the drain makes room in the ring). */
static VOID net_tty_queue_notify(NX_TCP_SOCKET *sock)
{
	(void)sock;
	net_tty_wake(NET_TTY_EVT_TX);
}

/* ---- the listener ---------------------------------------------------------------- */

/* Stop for good: "not listening", the step and NetX's answer. */
static int net_tty_stop(uint8_t op, UINT status)
{
	struct net_tty_state *s = &net_tty_board_state;

	s->fail_op = op;
	s->fail_status = status;
	s->listen = NET_TTY_L_FAILED;
	s->phase = NET_TTY_PH_STOPPED;
	return -1;
}

/* Create the socket, register the callbacks, listen.  The socket is never
 * deleted afterwards. */
static int net_tty_open(void)
{
	struct net_tty_state *s = &net_tty_board_state;
	const struct net_tty_facts *f = &net_tty_board_facts;
	UINT st;

	st = nx_tcp_socket_create(f->ip, &s->sock, "net-tty", NX_IP_NORMAL,
	                          NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
	                          NET_TTY_WINDOW, NX_NULL, net_tty_disconnect_cb);
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_CREATE, st);
	st = nx_tcp_socket_receive_notify(&s->sock, net_tty_rx_notify);
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_NOTIFY, st);
	/* Without it no connection would ever be published (net_tty_accept()). */
	st = nx_tcp_socket_establish_notify(&s->sock, net_tty_establish_cb);
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_NOTIFY, st);
	/*
	 * The only transmit-side callback that fires for an NX_NO_WAIT sender
	 * (NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY).  The window-update one is
	 * called only for a sender suspended in NetX
	 * (nx_tcp_socket_state_transmit_check.c), which this never is; the
	 * drain's retry wait covers the window and the pool.
	 */
	st = nx_tcp_socket_queue_depth_notify_set(&s->sock, net_tty_queue_notify);
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_NOTIFY, st);
	(void)nx_tcp_socket_transmit_configure(&s->sock, f->tx_queue_depth,
	                                       f->rto_ticks, f->rto_retries,
	                                       f->rto_shift);
	st = nx_tcp_server_socket_listen(f->ip, s->port, &s->sock, 1u, NX_NULL);
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_LISTEN, st);
	s->listen = NET_TTY_L_LISTENING;
	s->phase = NET_TTY_PH_ACCEPT;
	return 0;
}

/* Back to listening after a connection: unaccept, relisten ("a connection
 * is pending" is success); the next pass arms accept.  Either failing stops
 * the listener -- arming again over it is the leak of issue #202. */
static int net_tty_rearm(void)
{
	struct net_tty_state *s = &net_tty_board_state;
	UINT st;

	st = nx_tcp_server_socket_unaccept(&s->sock);
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_UNACCEPT, st);
	st = nx_tcp_server_socket_relisten(net_tty_board_facts.ip, s->port,
	                                   &s->sock);
	if (st != NX_SUCCESS && st != NX_CONNECTION_PENDING)
		return net_tty_stop(NET_TTY_OP_RELISTEN, st);
	s->phase = NET_TTY_PH_ACCEPT;
	return 0;
}

/*
 * The one end of a connection, published or not: close the gate, throw away
 * what was queued for it, cancel what runs for it, wake the instance (a
 * command waiting for input wakes and reads the 0x03), disconnect (bounded;
 * its answer changes nothing), back to listening.
 */
static int net_tty_end(uint8_t why)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;

	TX_DISABLE
	s->connected = 0u;
	s->live = 0u;
	TX_RESTORE
	net_tty_tx_discard();
	TX_DISABLE
	s->cancel = 1u;
	s->rx_discard = 1u;
	TX_RESTORE
	if (s->sh != NULL)
		cli_transport_notify_rx(s->sh);
	s->last_end = why;
	(void)nx_tcp_socket_disconnect(&s->sock, NET_TTY_DISC_WAIT);
	return net_tty_rearm();
}

/*
 * Arm accept once and wait in it.  A failed handshake (NetX's cleanup answers
 * NX_NOT_CONNECTED) goes back through unaccept and relisten; any other failure
 * is an arm that did not take, and stops the listener.  A success is published
 * only if the connection is still ESTABLISHED, the peer has not gone, the
 * establish callback ran and the link has not fallen since -- otherwise it is
 * ended like a session.
 */
static int net_tty_accept(void)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;
	uint8_t why = NET_TTY_END_NONE;
	UINT st;

	TX_DISABLE
	s->est_seen = 0u;
	s->peer_gone = 0u;
	TX_RESTORE
	st = nx_tcp_server_socket_accept(&s->sock, NX_WAIT_FOREVER);
	if (st == NX_NOT_CONNECTED)
		return net_tty_rearm();
	if (st != NX_SUCCESS)
		return net_tty_stop(NET_TTY_OP_ACCEPT, st);

	TX_DISABLE
	if (s->est_seen && s->est_falls != net_tty_falls_now())
		why = NET_TTY_END_LINK;
	else if (s->peer_gone)
		why = NET_TTY_END_PEER;
	else if (!s->est_seen ||
	         s->sock.nx_tcp_socket_state != NX_TCP_ESTABLISHED)
		why = NET_TTY_END_LEFT;
	else
		s->sess_falls = s->est_falls;
	TX_RESTORE
	if (why != NET_TTY_END_NONE)
		return net_tty_end(why);

	/* Published: the ring emptied BEFORE the session is live, so nothing an
	 * earlier session queued reaches this client.  live only if the peer is
	 * still there -- a disconnect since the check leaves it clear, and the
	 * first session pass ends it. */
	net_tty_tx_discard();
	s->st.sessions++;
	TX_DISABLE
	if (!s->peer_gone)
		s->live = 1u;
	TX_RESTORE
	s->phase = NET_TTY_PH_SESSION;
	if (s->sh != NULL)
		cli_transport_notify_conn(s->sh);
	return 0;
}

/*
 * Push queued output.  The listener is the only sender and the only reader
 * of the transmit ring.  Stops at the first refusal and leaves the bytes in
 * the ring; the session pass then waits briefly and tries again, which is
 * what recovers the refusals NetX has no callback for.  @return segments sent.
 */
static unsigned net_tty_drain(void)
{
	struct net_tty_state *s = &net_tty_board_state;
	NX_PACKET_POOL *pool = net_tty_board_facts.pool;
	unsigned n;

	if (!s->live)
		return 0u;
	for (n = 0u; n < NET_TTY_TX_BURST; n++) {
		const uint8_t *p;
		size_t run = cli_uart_ring_contig(&s->tx, &p);
		NX_PACKET *pkt = NX_NULL;
		UINT st;

		if (run == 0u)
			break;
		if (run > NET_TTY_MSS)
			run = NET_TTY_MSS;
		/* Leave the receive path some of the pool (advisory: an unlocked read
		 * of NetX's counter). */
		if (pool->nx_packet_pool_available <= NET_TTY_POOL_RESERVE) {
			s->st.tx_nobuf++;
			break;
		}
		if (nx_packet_allocate(pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT)
		    != NX_SUCCESS) {
			s->st.tx_nobuf++;
			break;
		}
		if (nx_packet_data_append(pkt, (VOID *)p, (ULONG)run, pool,
		                          NX_NO_WAIT) != NX_SUCCESS) {
			(void)nx_packet_release(pkt);
			s->st.tx_nobuf++;
			break;
		}
		st = nx_tcp_socket_send(&s->sock, pkt, NX_NO_WAIT);
		if (st != NX_SUCCESS) {
			(void)nx_packet_release(pkt);   /* NetX did not take it */
			if (st == NX_TX_QUEUE_DEPTH)
				s->st.tx_qdepth++;
			else if (st == NX_WINDOW_OVERFLOW)
				s->st.tx_win++;
			else
				s->st.tx_err++;
			break;
		}
		cli_uart_ring_advance_tail(&s->tx, run);
		s->st.tx_bytes += (uint32_t)run;
		s->st.tx_segs++;
		if (s->sh != NULL)
			cli_transport_notify_tx(s->sh);
	}
	return n;
}

/*
 * One session pass.  The COUNT is compared before every wait, the first one
 * included: a fall between the publish and here, or one whose wake came while
 * the drain ran, is seen now -- and no wake is ever cleared outside the wait
 * itself, so one that arrives after the comparison returns the wait at once.
 */
static int net_tty_session_pass(void)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;
	uint8_t fell, gone;
	ULONG got = 0u, wait;

	TX_DISABLE
	fell = (s->sess_falls != net_tty_falls_now()) ? 1u : 0u;
	gone = s->peer_gone;
	TX_RESTORE
	if (fell)
		return net_tty_end(NET_TTY_END_LINK);
	if (gone)
		return net_tty_end(NET_TTY_END_PEER);
	if (s->sock.nx_tcp_socket_state != NX_TCP_ESTABLISHED)
		return net_tty_end(NET_TTY_END_LEFT);

	/* A pass that hit the burst cap stopped for fairness, not a refusal:
	 * come straight back. */
	if (net_tty_drain() == NET_TTY_TX_BURST &&
	    cli_uart_ring_count(&s->tx) != 0u)
		net_tty_wake(NET_TTY_EVT_TX);
	wait = (cli_uart_ring_count(&s->tx) != 0u) ? (ULONG)NET_TTY_DRAIN_POLL
	                                            : TX_WAIT_FOREVER;
	(void)tx_event_flags_get(&s->evt,
	                         NET_TTY_EVT_TX | NET_TTY_EVT_DISC | NET_TTY_EVT_FELL,
	                         TX_OR_CLEAR, &got, wait);
	return 0;
}

int net_tty_listener_step(void)
{
	struct net_tty_state *s = &net_tty_board_state;
	ULONG got = 0u;

	switch (s->phase) {
	case NET_TTY_PH_PORT:
		if (tx_event_flags_get(&s->evt, NET_TTY_EVT_PORT, TX_OR_CLEAR, &got,
		                       TX_WAIT_FOREVER) != TX_SUCCESS)
			return 0;
		return net_tty_open();
	case NET_TTY_PH_ACCEPT:
		return net_tty_accept();
	case NET_TTY_PH_SESSION:
		return net_tty_session_pass();
	default:
		return -1;
	}
}

static VOID net_tty_listener_entry(ULONG arg)
{
	(void)arg;
	while (net_tty_listener_step() == 0)
		;
	/* Stopped: returning leaves the thread TERMINATED, which `thread` shows
	 * as it is; `net info` says why. */
}

/* ---- init and the board's entries ---------------------------------------------- */

int net_tty_init(void)
{
	struct net_tty_state *s = &net_tty_board_state;
	const struct net_tty_facts *f = &net_tty_board_facts;

	s->ready = 0u;
	cli_uart_ring_init(&s->rx, f->rx_buf, (size_t)f->rx_size);
	cli_uart_ring_init(&s->tx, f->tx_buf, (size_t)f->tx_size);
	s->live = 0u;
	s->peer_gone = 0u;
	s->cancel = 0u;
	s->rx_discard = 0u;
	s->est_seen = 0u;
	s->iac = 0u;
	s->port_given = 0u;
	s->port = 0u;
	s->phase = NET_TTY_PH_PORT;
	s->listen = NET_TTY_L_NOPORT;
	s->fail_op = NET_TTY_OP_NONE;
	s->fail_status = NX_SUCCESS;
	s->last_end = NET_TTY_END_NONE;

	if (tx_event_flags_create(&s->evt, "net-tty") != TX_SUCCESS)
		return -1;
	if (tx_thread_create(&s->thread, "net-tty", net_tty_listener_entry, 0u,
	                     f->stack, f->stack_size, NET_TTY_PRIORITY,
	                     NET_TTY_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START)
	    != TX_SUCCESS)
		return -1;
	s->ready = 1u;               /* last: everything above is in place */
	return 0;
}

void net_tty_port_set(uint16_t port)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_tty_state *s = &net_tty_board_state;
	int first = 0;

	if (!s->ready)
		return;
	TX_DISABLE
	if (!s->port_given) {
		s->port_given = 1u;
		s->port = port;
		first = 1;
	}
	TX_RESTORE
	if (first)
		net_tty_wake(NET_TTY_EVT_PORT);
}

void net_tty_link_fell(void)
{
	net_tty_wake(NET_TTY_EVT_FELL);
}

bool net_tty_is_console(const struct cli_instance *sh)
{
	const struct cli_instance *owner;
	const struct cli_instance *mine = net_tty_board_state.sh;

	if (sh == NULL || mine == NULL)
		return false;
	owner = (sh->fg != NULL) ? sh->fg : sh;   /* a job uses its launcher's */
	return owner == mine;
}

int net_tty_guard(struct cli_instance *sh, const char *what)
{
	if (!net_tty_is_console(sh))
		return 0;
	cli_error(sh, "%s: refused on the telnet console -- it would destroy this "
	              "session. Run it from the %s console.\r\n",
	          what, net_tty_board_facts.other_console);
	return 1;
}

/* ---- `net info` -------------------------------------------------------------------- */

static int net_tty_line(net_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[NET_TTY_LINE_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	return (write(ctx, line, (size_t)n) < 0) ? -1 : 0;
}

static const char *net_tty_op_name(uint8_t op)
{
	switch (op) {
	case NET_TTY_OP_CREATE:   return "socket create";
	case NET_TTY_OP_NOTIFY:   return "callback registration";
	case NET_TTY_OP_LISTEN:   return "listen";
	case NET_TTY_OP_ACCEPT:   return "accept";
	case NET_TTY_OP_UNACCEPT: return "unaccept";
	case NET_TTY_OP_RELISTEN: return "relisten";
	default:                  return "?";
	}
}

static const char *net_tty_end_name(uint8_t why)
{
	switch (why) {
	case NET_TTY_END_PEER: return "the peer closed";
	case NET_TTY_END_LINK: return "the link fell";
	case NET_TTY_END_LEFT: return "it left ESTABLISHED";
	default:               return "none yet";
	}
}

int net_tty_write_lines(net_svc_write_fn write, void *ctx)
{
	TX_INTERRUPT_SAVE_AREA
	const struct net_tty_state *s = &net_tty_board_state;
	struct net_tty_stats st;
	uint8_t listen, op, end, live;
	uint16_t port;
	UINT status;
	int rc;

	if (!s->ready)
		return net_tty_line(write, ctx, "telnet: not available (init failed)\r\n");
	TX_DISABLE
	st = s->st;
	listen = s->listen;
	op = s->fail_op;
	status = s->fail_status;
	end = s->last_end;
	live = s->live;
	port = s->port;
	TX_RESTORE

	if (listen == NET_TTY_L_LISTENING)
		rc = net_tty_line(write, ctx, "telnet: :%u listening, %s, %lu sessions, "
		                  "last ended: %s\r\n", (unsigned)port,
		                  live ? "client attached" : "no client",
		                  (unsigned long)st.sessions, net_tty_end_name(end));
	else if (listen == NET_TTY_L_FAILED)
		rc = net_tty_line(write, ctx, "telnet: NOT listening on :%u -- %s failed "
		                  "(0x%02x); %lu sessions, last ended: %s\r\n",
		                  (unsigned)port, net_tty_op_name(op), (unsigned)status,
		                  (unsigned long)st.sessions, net_tty_end_name(end));
	else
		rc = net_tty_line(write, ctx, "telnet: not listening yet (no port)\r\n");
	if (rc != 0)
		return -1;
	return net_tty_line(write, ctx, "telnet: rx drop %lu, tx %lu seg / %lu B, "
	                    "ring peak %lu B, refused q%lu w%lu buf%lu err%lu, "
	                    "dropped %lu B\r\n",
	                    (unsigned long)st.rx_drops, (unsigned long)st.tx_segs,
	                    (unsigned long)st.tx_bytes, (unsigned long)st.tx_hiwater,
	                    (unsigned long)st.tx_qdepth, (unsigned long)st.tx_win,
	                    (unsigned long)st.tx_nobuf, (unsigned long)st.tx_err,
	                    (unsigned long)st.tx_dropped);
}
