/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_shell.c
 * @brief   TCP network shell (telnet) over NetX Duo (issue #49 P4).  See nx_shell.h.
 *
 * One session at a time (N=1): the CLI §14 KILL/uninit lifecycle is not
 * implemented, so a static cli_instance is reused across connections rather than
 * created/destroyed per client.  The server thread owns listen/accept/relisten;
 * NetX callbacks (IP-thread context) only push to the RX ring / set flags; the
 * cli_instance thread runs the shell, resets a fresh session on CLI_EVT_CONN, and
 * queues its output for the server thread to transmit.
 *
 * OUTPUT PATH (issue #6).  The server thread is the SOLE transmitter: it is the
 * only caller of nx_tcp_socket_send() and the only consumer of the TX ring, and it
 * also owns accept / disconnect / unaccept / relisten.  Writers -- the CLI thread,
 * a background-job worker, and printf from this instance's thread (which
 * backend/cli_backend_uart.c hands over here) -- only append bytes to the ring.
 *
 * That split is what makes the three hard parts fall out:
 *
 *   Coalescing.  The core stages output in 32-byte chunks, so transmitting from
 *   write() split one line-editor redraw across several TCP segments and paid a
 *   peer ACK for each.  write() now only fills the ring and the flush hook (which
 *   the core calls at the end of every bracketed output call) wakes the drain, so
 *   a whole redraw leaves as one segment.  Nothing is delayed to achieve it: the
 *   unit boundary was always known, the old code just could not see it.
 *
 *   Recovery.  A refusal NetX has no callback for -- an empty packet pool -- had
 *   no wake-up source at all, so the output was simply dropped.  The drain re-arms
 *   a bounded wait whenever the ring is non-empty, which covers exactly those.
 *
 *   Session boundaries.  Because one thread both sends and re-accepts, there is no
 *   window in which a check ("is this still my session?") can be overtaken by a
 *   reconnect before the send.  The write gate is read inside the producers'
 *   critical section, so clearing it is a clean cut.
 *
 * Modelled on boards/wio-lite-ai/src/net_shell.c, simplified for this board: a
 * single always-armed session, no start/stop command, no autoarm.
 */
#include "nx_api.h"
#include "stm32f7xx_hal.h"    /* __get_PRIMASK / __disable_irq / __set_PRIMASK   */

#include "nx_shell.h"
#include "nx_glue.h"          /* nx_net_ip / nx_net_pool */

#include "cli_instance.h"     /* struct cli_transport(_api), CLI_EVT_*, notify_* */
#include "cli_uart_ring.h"    /* the reusable SPSC/MPSC byte ring                */

#define LOG_TAG "nshell"
#include "log.h"

#define NX_SHELL_PORT        23u
#define NX_SHELL_WINDOW      2048u      /* TCP receive window                     */
#define NX_SHELL_TX_QUEUE    8u         /* cap TX queue depth -> protect eth_pool */
#define NX_SHELL_MSS         1400u      /* max bytes per TX packet                */
#define NX_SHELL_RX_RING     512u
#define NX_SHELL_EXTRACT     1500u      /* per-packet RX extraction buffer        */
#define NX_SHELL_PRIORITY    14

/*
 * Output the CLI threads hand over before they have to wait.  It is what turns
 * ~32-byte writes into MSS-sized segments, so it wants to be several MSS; 4 KB
 * holds a `coremark` report or a `membench` table outright.  Bigger output (a
 * `dmesg`) still fits -- it just makes the writer wait for the drain, which is the
 * flow control working, not a failure.
 */
#define NX_SHELL_TX_RING     4096u

/*
 * Server thread stack.  Was 1024 with a measured peak of 496 B when this thread
 * only accepted and dispatched; it now also runs the packet allocate / append /
 * send path, so it is sized like the wio console's (measured 2560 B after the same
 * move).  Re-measure with `thread` after any change to the drain.
 */
#define NX_SHELL_STACK       2560u

/* Segments pushed per drain pass before going back round the loop, so a
   disconnect is still noticed promptly in the middle of a long report. */
#define NX_SHELL_TX_BURST    8u

/*
 * Packets of the shared pool this console will not take (nx_glue.c owns the pool;
 * the receive path allocates from it with NX_NO_WAIT and drops the frame when that
 * fails).  Advisory -- an unlocked read of a counter NetX maintains -- which is the
 * right weight for a policy whose only job is to keep an output burst from being
 * the reason a frame was dropped.  It is NOT a hard reservation.
 */
#define NX_SHELL_POOL_RESERVE 8u

/*
 * How long the drain waits before retrying when the socket has just refused.  This
 * is what makes the refusals NetX cannot signal (an empty packet pool, a window
 * that has not opened) recoverable rather than fatal.  Armed only while the ring is
 * non-empty, so an idle console still blocks for ever and costs nothing.
 */
#define NX_SHELL_DRAIN_POLL  50u        /* ticks (1 tick = 1 ms) */

/*
 * The console's no-progress deadline, published through cli_transport::tx_timeout
 * (the default CLI_TX_TIMEOUT of 1 s applies to every other backend).  It bounds
 * how long the core waits for room in the ring before dropping the rest of a
 * command's output.
 *
 * It MUST exceed the socket's retransmit timeout: shell_socket_setup() configures
 * 2 s with a shift of 1, so the first retransmit lands at 2 s and the second at 6 s.
 * Recovering a dropped segment means waiting out that first retransmit, and the
 * 1 s default turned every such loss into a truncated report.  5 s covers it with
 * margin and deliberately does NOT cover the second -- past that the cost is being
 * tied to a peer that is alive but not reading.  The core keeps the wait
 * interruptible (it also wakes on RX and polls for Ctrl+C), and it applies the
 * long deadline to command output only: the line editor keeps CLI_TX_TIMEOUT, so
 * the keyboard never freezes for 5 s (cli_core.c, cli_tx_deadline).
 */
#define NX_SHELL_TX_DEADLINE 5000u

/* Server thread events. */
#define NX_SHELL_EVT_TX      0x1u       /* output queued, or the socket took more */
#define NX_SHELL_EVT_DISC    0x2u       /* the peer disconnected                  */

/* PRIMASK critical section, as in backend/cli_backend_uart.c.  Nests safely inside
   ThreadX's own PRIMASK sections, and restores the previous mask rather than
   enabling unconditionally. */
#define NSH_CRIT_ENTER()  do { uint32_t _pm = __get_PRIMASK(); __disable_irq()
#define NSH_CRIT_EXIT()   __set_PRIMASK(_pm); } while (0)

/* ---- transport backend (cli_transport_api over the TCP socket) ------------- */

struct cli_tcp {
	struct cli_instance *sh;            /* set by tcp_init from tr->sh           */
	struct cli_uart_ring rx_ring;       /* IP-thread producer, CLI-thread consumer */
	struct cli_uart_ring tx_ring;       /* CLI/bg producers, server thread consumer */
	uint8_t              rx_buf[NX_SHELL_RX_RING];
	uint8_t              tx_buf[NX_SHELL_TX_RING];
	/*
	 * The write gate.  SET by the CLI thread (tcp_session_begin, once the
	 * connection is confirmed), CLEARED by the server thread's teardown and by the
	 * disconnect callback on the NetX IP thread.  Producers read it INSIDE their
	 * critical section, which is what makes a clear a hard cut-off: nothing can be
	 * appended between the clear and the discard that follows it.
	 */
	volatile uint8_t     connected;
};

static struct cli_tcp        tcp_ctx;
static NX_TCP_SOCKET         sock;
static TX_THREAD             server_thread;
static UCHAR                 server_stack[NX_SHELL_STACK];
static TX_EVENT_FLAGS_GROUP  nsh_evt;
static volatile int          session_live;      /* a connection is currently accepted
                                                   (server sets 1 after accept, the
                                                   disconnect cb clears it) -- gates
                                                   session_begin so a CLI_EVT_CONN
                                                   that the CLI thread processes only
                                                   AFTER the client already vanished
                                                   does not resurrect `connected`.  */
static int                   iac_state;         /* telnet IAC strip (IP-thread dom) */
static uint8_t               extract_buf[NX_SHELL_EXTRACT];

/*
 * Output statistics, reported by `net info` (issue #6).  The refusal reasons are
 * kept APART on purpose: merging them is what hid the equivalent bug on the other
 * board -- a single "tx waited" counter cannot distinguish the designed
 * back-pressure from the refusal that has no wake-up source, and the report reads
 * as healthy while output is being dropped.
 *
 *   tx_qdepth  the socket's transmit queue was full.  The DESIGNED back-pressure:
 *              a peer ACK releases a packet and the queue-depth callback wakes the
 *              drain.  A large number is normal.
 *   tx_win     the peer's window had no room.  NetX cannot signal this to an
 *              NX_NO_WAIT sender, so the drain poll is what recovers it.  Expected
 *              to be ~0; a large number means the peer is not reading.
 *   tx_nobuf   the packet pool was empty or below this console's reserve.  THIS is
 *              the sizing signal: output was competing with the receive path.
 *   tx_err     any other nx_tcp_socket_send() failure (e.g. NX_NOT_CONNECTED from a
 *              disconnect race).  Kept out of tx_nobuf so it cannot dilute it.
 *   tx_dropped bytes thrown away because the session ended: what write() swallowed
 *              with no client attached, plus whatever was still queued when the
 *              peer went.  Swallowing is the contract (req §11 -- a console nobody
 *              is attached to must never make the shell wait), but it has to be
 *              VISIBLE, or "the output vanished" has no counter to point at.
 *              Counted only once a client has connected at least once: the
 *              instance draws its width probe and first prompt when its thread
 *              starts, long before anyone attaches, and reporting those ~20 bytes
 *              as a loss would put a permanent non-zero in the one field whose
 *              whole job is to read zero when nothing went wrong.
 */
static uint32_t st_sessions;
static uint32_t st_tx_segs, st_tx_bytes, st_tx_hiwater;
static uint32_t st_tx_qdepth, st_tx_win, st_tx_nobuf, st_tx_err, st_tx_dropped;

/* Bytes lost with no client attached.  See st_tx_dropped. */
static void tx_count_dropped(size_t n)
{
	if (st_sessions != 0u)
		st_tx_dropped += (uint32_t)n;
}

static int tcp_init(struct cli_transport *tr)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;

	cli_uart_ring_init(&c->rx_ring, c->rx_buf, sizeof c->rx_buf);
	cli_uart_ring_init(&c->tx_ring, c->tx_buf, sizeof c->tx_buf);
	c->sh = tr->sh;
	c->connected = 0;
	return 0;
}

static int tcp_enable(struct cli_transport *tr)
{
	(void)tr;                           /* the socket is armed by nx_shell_init    */
	return 0;
}

static int tcp_read(struct cli_transport *tr, uint8_t *data, size_t cap)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;

	/* SPSC: the receive callback (IP thread) is the only producer, the CLI thread
	   the only consumer -- lock-free, like the UART backend. */
	return (int)cli_uart_ring_get_buf(&c->rx_ring, data, cap);
}

/* Wake the drain.  The ring, never this flag, is the source of truth. */
static void tcp_tx_kick(void)
{
	(void)tx_event_flags_set(&nsh_evt, NX_SHELL_EVT_TX, TX_OR);
}

/*
 * ---- the write path --------------------------------------------------------
 *
 * cli_transport_api.write() is a NON-BLOCKING contract (cli_instance.h): enqueue
 * what fits, return the count, and let the core wait on CLI_EVT_TX for the rest.
 * Here that is all it does -- no NetX call, no socket, nothing that can block or be
 * torn down.  Several threads reach it (the CLI thread, a bg-job worker sharing
 * this transport, printf handed over by the UART backend), so the ring is MPSC and
 * the append runs under a PRIMASK critical section.
 */
static int tcp_write(struct cli_transport *tr, const uint8_t *data, size_t len)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;
	size_t i = 0, count;

	/* Not connected: swallow it.  Returning `len` is what keeps the shell from
	   wedging on a console nobody is attached to (req §11) -- a short count would
	   send the core to wait for room in a ring that nothing will drain, and it
	   would sit there until the 5 s deadline for every write.  Counted so the
	   loss is observable rather than silent. */
	if (!c->connected) {
		tx_count_dropped(len);
		return (int)len;
	}

	NSH_CRIT_ENTER();
	if (c->connected) {                 /* re-read under the gate; see the note */
		while (i < len && cli_uart_ring_put(&c->tx_ring, data[i]))
			i++;
	}
	count = cli_uart_ring_count(&c->tx_ring);
	NSH_CRIT_EXIT();

	if ((uint32_t)count > st_tx_hiwater)
		st_tx_hiwater = (uint32_t)count;

	/*
	 * Deliberately NO kick in the normal case: the core stages in 32-byte chunks,
	 * so waking the drain here would transmit a third of a redraw and pay a peer
	 * ACK for it.  tcp_flush() does the waking once the whole unit is in.
	 *
	 * EXCEPT when we could not take it all.  Then the core parks on CLI_EVT_TX
	 * waiting for room, the only source of room is the drain, and the drain's only
	 * other wake-up is an end-of-unit that cannot arrive because the unit is stuck
	 * here.  This is what keeps the flow-control contract from deadlocking.
	 */
	if (i < len)
		tcp_tx_kick();
	/* Told to stop rather than told to wait: the session ended while we were in
	   the loop, so swallow the remainder exactly as the entry check does -- and
	   count it, since the bytes already in the ring are about to be discarded by
	   the teardown too. */
	if (i < len && !c->connected) {
		tx_count_dropped(len - i);
		return (int)len;
	}
	return (int)i;
}

/*
 * "The unit of output is complete" -- the core calls this from cli_unlock(), i.e.
 * at the end of every bracketed output call, nested ones included (issue #49).
 * THIS is what transmits: tcp_write() only fills the ring, so a whole line-editor
 * redraw reaches the drain in one piece and leaves as one TCP segment.
 *
 * It does not wait and does not touch the socket, so the output lock it is called
 * under is held for the length of an event-flag set and nothing more.
 */
static void tcp_flush(struct cli_transport *tr)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;

	if (cli_uart_ring_count(&c->tx_ring) != 0u)
		tcp_tx_kick();
}

/* Telnet negotiation sent at the start of each session so a telnet client enters
   character-at-a-time mode (no local echo / no line buffering), which is what the
   CLI's interactive line editor needs.  IAC WILL ECHO (server echoes) + IAC WILL
   SUPPRESS-GO-AHEAD.  (A raw `nc` client shows these 6 bytes as harmless garbage
   before the prompt -- the session is telnet-first per the issue.) */
static const uint8_t telnet_charmode[] = {
	0xFFu, 0xFBu, 0x01u,   /* IAC WILL ECHO                  */
	0xFFu, 0xFBu, 0x03u,   /* IAC WILL SUPPRESS_GO_AHEAD     */
};

static void tcp_session_begin(struct cli_transport *tr)
{
	struct cli_tcp *c = (struct cli_tcp *)tr->ctx;
	uint8_t junk;
	int     armed;

	/* Called by the CLI thread on CLI_EVT_CONN, after the editor state was reset
	   and before the prompt is drawn.  Drain any bytes left in the ring from a
	   previous session HERE (consumer side) so the RX ring stays SPSC. */
	while (cli_uart_ring_get(&c->rx_ring, &junk))
		;

	/*
	 * ONE critical section for the gate.  This is the only output path the core
	 * does not call under the output lock (cli_core.c calls session_begin straight
	 * from its event loop), and reading session_live separately from publishing
	 * `connected` would let a disconnect land in between: the stale 1 would reopen
	 * the gate on a dead socket and the negotiation would be queued for whoever
	 * connects next.
	 */
	NSH_CRIT_ENTER();
	armed = (session_live != 0);
	c->connected = (uint8_t)(armed ? 1 : 0);
	if (armed && cli_uart_ring_free(&c->tx_ring) >= sizeof telnet_charmode) {
		for (size_t i = 0; i < sizeof telnet_charmode; i++)
			(void)cli_uart_ring_put(&c->tx_ring, telnet_charmode[i]);
	}
	NSH_CRIT_EXIT();

	if (armed)
		tcp_tx_kick();
}

static const struct cli_transport_api cli_tcp_api = {
	.init          = tcp_init,
	.enable        = tcp_enable,
	.write         = tcp_write,
	.read          = tcp_read,
	.uninit        = NULL,
	.update        = NULL,
	.session_begin = tcp_session_begin,
	.flush         = tcp_flush,
};

struct cli_transport nx_shell_transport = {
	.api        = &cli_tcp_api,
	.sh         = NULL,                 /* set by cli_init()                       */
	.ctx        = &tcp_ctx,
	.tx_timeout = NX_SHELL_TX_DEADLINE,
};

/* ---- NetX callbacks (IP-thread context: flag/ring/notify only) ------------ */

/* Telnet IAC strip: drop 0xFF + command [+ option] negotiation; 0xFF 0xFF -> one
   literal 0xFF.  Returns 1 if @p b is consumed (dropped), 0 if it is shell data. */
static int iac_consume(uint8_t b)
{
	switch (iac_state) {
	case 0:
		if (b == 0xFFu) { iac_state = 1; return 1; }
		return 0;
	case 1:
		if (b == 0xFFu) { iac_state = 0; return 0; }   /* IAC IAC -> literal 0xFF  */
		if (b >= 0xFBu && b <= 0xFEu) { iac_state = 2; return 1; } /* WILL/WONT/DO/DONT */
		iac_state = 0; return 1;                       /* other 2-byte command     */
	default:                                           /* option byte after WILL.. */
		iac_state = 0; return 1;
	}
}

static void shell_rx_notify(NX_TCP_SOCKET *s)
{
	NX_PACKET *pkt;

	while (nx_tcp_socket_receive(s, &pkt, NX_NO_WAIT) == NX_SUCCESS) {
		ULONG copied = 0;

		nx_packet_data_extract_offset(pkt, 0, extract_buf, sizeof extract_buf,
		                              &copied);
		nx_packet_release(pkt);
		for (ULONG i = 0; i < copied; i++) {
			uint8_t b = extract_buf[i];

			if (iac_consume(b))
				continue;
			cli_uart_ring_put(&tcp_ctx.rx_ring, b);    /* drop on full             */
		}
	}
	if (tcp_ctx.sh != NULL)
		cli_transport_notify_rx(tcp_ctx.sh);
}

static void shell_disconnect_cb(NX_TCP_SOCKET *s)
{
	(void)s;
	/* Peer FIN/RST.  Shut the write gate and mark the session dead in ONE critical
	   section (session_begin reads the pair atomically), then wake the server
	   thread, which discards whatever is still queued, completes the close and
	   relistens.  NetX calls this on the ESTABLISHED FIN; disconnect_complete only
	   fires AFTER the app's own disconnect(), so this is the wake source. */
	NSH_CRIT_ENTER();
	tcp_ctx.connected = 0;
	session_live = 0;
	NSH_CRIT_EXIT();

	iac_state = 0;                      /* same IP-thread domain as iac_consume */
	(void)tx_event_flags_set(&nsh_evt, NX_SHELL_EVT_DISC, TX_OR);
}

/*
 * Window update / queue depth freed: wake the DRAIN, not the CLI.  The CLI is
 * waiting for room in the ring and only the drain can make it; telling the core
 * "space freed" when nothing has been sent is what the old code got wrong.
 */
static void shell_tx_notify(NX_TCP_SOCKET *s)
{
	(void)s;
	tcp_tx_kick();
}

/* ---- server thread: the sole transmitter ---------------------------------- */

/*
 * Push queued output.  SERVER THREAD ONLY -- it is the sole consumer of the ring
 * and the sole caller of nx_tcp_socket_send().
 *
 * Returns the number of segments sent.  Stops at the first refusal and leaves the
 * bytes in the ring; the caller then arms a bounded wait and tries again, which is
 * what covers the refusals NetX has no callback for.
 */
static unsigned tcp_tx_drain(void)
{
	NX_PACKET_POOL *pool = (NX_PACKET_POOL *)nx_net_pool();
	unsigned n;

	if (pool == NX_NULL || !session_live)
		return 0;

	for (n = 0u; n < NX_SHELL_TX_BURST; n++) {
		const uint8_t *p;
		size_t         run = cli_uart_ring_contig(&tcp_ctx.tx_ring, &p);
		NX_PACKET     *pkt = NX_NULL;
		UINT           s;

		if (run == 0u)
			break;
		if (run > NX_SHELL_MSS)
			run = NX_SHELL_MSS;

		/* Leave the receive path some pool (advisory, see NX_SHELL_POOL_RESERVE). */
		if (pool->nx_packet_pool_available <= NX_SHELL_POOL_RESERVE) {
			st_tx_nobuf++;
			break;
		}
		if (nx_packet_allocate(pool, &pkt, NX_TCP_PACKET, NX_NO_WAIT) != NX_SUCCESS) {
			st_tx_nobuf++;
			break;
		}
		if (nx_packet_data_append(pkt, (VOID *)p, (ULONG)run, pool,
		                          NX_NO_WAIT) != NX_SUCCESS) {
			nx_packet_release(pkt);
			st_tx_nobuf++;
			break;
		}
		s = nx_tcp_socket_send(&sock, pkt, NX_NO_WAIT);
		if (s != NX_SUCCESS) {
			/* NetX did not take it, so we still own it. */
			nx_packet_release(pkt);
			if (s == NX_TX_QUEUE_DEPTH)
				st_tx_qdepth++;
			else if (s == NX_WINDOW_OVERFLOW)
				st_tx_win++;
			else
				st_tx_err++;
			break;
		}

		/* Sent: free the ring space and tell the core, which may be parked in
		   cli_tx_send_blocking() waiting for exactly this.  The target is the
		   FOREGROUND instance (tr->sh): a bg-job worker shares this transport but
		   waits on its own group, and the core posts CLI_EVT_TX to tr->sh. */
		cli_uart_ring_advance_tail(&tcp_ctx.tx_ring, run);
		st_tx_bytes += (uint32_t)run;
		st_tx_segs++;
		if (tcp_ctx.sh != NULL)
			cli_transport_notify_tx(tcp_ctx.sh);
	}
	return n;
}

/*
 * Drop everything queued.  Advances the tail by a count it has already read, so it
 * cannot run past a producer's head -- but that alone would not make it a clean
 * cut: what stops a producer appending straight after it is the write gate being
 * read inside the producers' critical section.  CALL ONLY WITH `connected`
 * ALREADY CLEARED.
 *
 * Always followed by the TX notify, so a writer blocked on CLI_EVT_TX wakes,
 * re-enters write() and returns at once on the closed gate instead of waiting out
 * the deadline.
 */
static void tcp_tx_discard(void)
{
	size_t n = cli_uart_ring_count(&tcp_ctx.tx_ring);

	if (n) {
		cli_uart_ring_advance_tail(&tcp_ctx.tx_ring, n);
		tx_count_dropped(n);            /* queued for a session that ended */
	}
	if (tcp_ctx.sh != NULL)
		cli_transport_notify_tx(tcp_ctx.sh);
}

/* Create the socket + listen FROM THE SERVER THREAD: nx_tcp_socket_create /
   _listen are thread-only (the error-checking layer returns NX_CALLER_ERROR from
   the tx_application_define init context), so they must run after the scheduler
   starts -- same as the P3 echo server.  Returns 0 on success. */
static int shell_socket_setup(NX_IP *ip)
{
	if (nx_tcp_socket_create(ip, &sock, "net-shell", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
	                         NX_IP_TIME_TO_LIVE, NX_SHELL_WINDOW, NX_NULL,
	                         shell_disconnect_cb) != NX_SUCCESS) {
		LOG_ERR("socket create failed");
		return -1;
	}
	if (nx_tcp_socket_receive_notify(&sock, shell_rx_notify) != NX_SUCCESS) {
		LOG_ERR("receive_notify failed");
		nx_tcp_socket_delete(&sock);
		return -1;
	}
	nx_tcp_socket_window_update_notify_set(&sock, shell_tx_notify);
	nx_tcp_socket_queue_depth_notify_set(&sock, shell_tx_notify);
	/* 2 s retransmit timeout, 10 retries, shift 1 -- the basis of
	   NX_SHELL_TX_DEADLINE above.  Keep the three in step. */
	nx_tcp_socket_transmit_configure(&sock, NX_SHELL_TX_QUEUE,
	                                 2u * NX_IP_PERIODIC_RATE, 10, 1);
	if (nx_tcp_server_socket_listen(ip, NX_SHELL_PORT, &sock, 1, NX_NULL)
	    != NX_SUCCESS) {
		LOG_ERR("listen :%u failed", (unsigned)NX_SHELL_PORT);
		nx_tcp_socket_delete(&sock);
		return -1;
	}
	LOG_INF("listening on :%u (telnet)", (unsigned)NX_SHELL_PORT);
	return 0;
}

static void shell_server_entry(ULONG arg)
{
	NX_IP *ip = (NX_IP *)nx_net_ip();

	(void)arg;
	if (ip == NULL || shell_socket_setup(ip) != 0)
		return;                         /* telnet disabled; everything else runs    */

	for (;;) {
		ULONG flags = 0;

		/* Clear any stale events before listening for the next client. */
		(void)tx_event_flags_get(&nsh_evt, NX_SHELL_EVT_TX | NX_SHELL_EVT_DISC,
		                         TX_OR_CLEAR, &flags, TX_NO_WAIT);

		if (nx_tcp_server_socket_accept(&sock, NX_WAIT_FOREVER) != NX_SUCCESS)
			continue;

		/* Order matters (issue #6): the ring is emptied BEFORE the session is
		   published, so nothing a previous session queued can reach this client.
		   `connected` is still 0 here -- the CLI thread raises it in
		   session_begin, gated on session_live, once it reaches CLI_EVT_CONN. */
		tcp_tx_discard();
		st_sessions++;
		session_live = 1;
		if (tcp_ctx.sh != NULL)
			cli_transport_notify_conn(tcp_ctx.sh);
		LOG_INF("client connected");

		/* Session loop: transmit until the peer goes away.  The wait is bounded
		   only while there is something to send, so an idle console sleeps. */
		for (;;) {
			ULONG f = 0;
			ULONG wait = (cli_uart_ring_count(&tcp_ctx.tx_ring) != 0u)
			             ? (ULONG)NX_SHELL_DRAIN_POLL : TX_WAIT_FOREVER;

			(void)tx_event_flags_get(&nsh_evt,
			                         NX_SHELL_EVT_TX | NX_SHELL_EVT_DISC,
			                         TX_OR_CLEAR, &f, wait);

			/* DISC beats TX: draining into a socket the peer has closed only
			   burns packets from the shared pool. */
			if ((f & NX_SHELL_EVT_DISC) || !session_live)
				break;

			/* A pass that hit the burst cap stopped for fairness, not because
			   the socket refused -- come straight back instead of sleeping out
			   the retry poll, or a long report would leave in 8-segment bursts
			   50 ms apart. */
			if (tcp_tx_drain() == NX_SHELL_TX_BURST &&
			    cli_uart_ring_count(&tcp_ctx.tx_ring) != 0u)
				tcp_tx_kick();
		}

		/* Teardown, in the order the next session depends on. */
		NSH_CRIT_ENTER();
		tcp_ctx.connected = 0;
		session_live = 0;
		NSH_CRIT_EXIT();
		tcp_tx_discard();
		LOG_INF("client disconnected");

		/* Complete the close handshake (bounded), then re-arm the listen slot. */
		nx_tcp_socket_disconnect(&sock, NX_IP_PERIODIC_RATE);
		nx_tcp_server_socket_unaccept(&sock);
		nx_tcp_server_socket_relisten(ip, NX_SHELL_PORT, &sock);
	}
}

bool nx_shell_stats_get(struct nx_shell_stats *out)
{
	if (out == NULL || nx_net_ip() == NULL)
		return false;

	out->connected  = (tcp_ctx.connected != 0u);
	out->sessions   = st_sessions;
	out->tx_segs    = st_tx_segs;
	out->tx_bytes   = st_tx_bytes;
	out->tx_hiwater = st_tx_hiwater;
	out->tx_qdepth  = st_tx_qdepth;
	out->tx_win     = st_tx_win;
	out->tx_nobuf   = st_tx_nobuf;
	out->tx_err     = st_tx_err;
	out->tx_dropped = st_tx_dropped;
	return true;
}

int nx_shell_init(void)
{
	/* Only ThreadX object creation here (safe in tx_application_define): the
	   socket create + listen run in the server thread (thread-only NetX APIs). */
	if (nx_net_ip() == NULL)
		return -1;
	if (tx_event_flags_create(&nsh_evt, "nshe") != TX_SUCCESS) {
		LOG_ERR("event group create failed");
		return -1;
	}
	if (tx_thread_create(&server_thread, "net-shell", shell_server_entry, 0,
	                     server_stack, sizeof server_stack,
	                     NX_SHELL_PRIORITY, NX_SHELL_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("server thread create failed");
		tx_event_flags_delete(&nsh_evt);
		return -1;
	}
	return 0;
}
