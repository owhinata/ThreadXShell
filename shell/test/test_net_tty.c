/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the one telnet console (shell/cmds/net_tty.c, issue #221),
 * compiled as it is, over a fake NetX TCP server socket and packet pool
 * (nx_shim/), fake ThreadX event flags and masked sections (shim/tx_api.h)
 * and a CLI that only records the notifications.  The listener thread is not
 * run: one pass (net_tty_listener_step()) is called directly, and the IP
 * thread's callbacks are the functions the console registered with the fake.
 *
 * The fake socket keeps NetX's states and its two traps: a handshake that
 * fails leaves the socket LISTENING but still bound to the dead peer, and
 * accept on such a socket leaks a packet (issue #202); relisten refuses a
 * socket that is not CLOSED.  An accept can be scripted to complete (the
 * establish callback runs, then an optional hook "while NetX resumes the
 * accept"), to fail its handshake, or to fail at once.
 *
 * What is pinned:
 *  - init creates the flags and the listener (priority, stack) and sets the
 *    ready mark last; the port is taken once, before init it is ignored;
 *  - a create / callback / listen failure: "not listening", the step and
 *    NetX's answer, the listener stops and calls nothing more;
 *  - ONE ACCEPT PER CONNECTION: a failed handshake goes unaccept -> relisten
 *    before the next arm, and nothing leaks; an unaccept, relisten or arm
 *    that fails stops the listener without another accept; a pending
 *    connection at relisten is success;
 *  - the publish, by the order of the establishment, a peer's close and a
 *    link fall around the accept's return (a table): only "established, no
 *    fall since, peer there, still ESTABLISHED" publishes;
 *  - a fall after the publish ends the session at the first pass, before any
 *    wait; a wake that arrives after the comparison is still there when the
 *    pass waits; a fall while listening does not end the next session;
 *  - each of the three ends: the gate closed, the ring thrown away (counted),
 *    the instance told, the next read returning one 0x03 -- even with the
 *    receive ring full -- and nothing after it until the next publish (the
 *    gone connection's bytes are consumed, never returned), and the socket
 *    back to listening, from LISTEN or TIME_WAIT; the fake's unaccept refuses
 *    what NetX refuses (ESTABLISHED, SYN_RECEIVED, CLOSED and unbound) and
 *    its disconnect leaves what NetX leaves (LISTEN still bound, or TIME_WAIT);
 *  - `dropped` counts what a session had queued and lost at its end, not what
 *    is written with the gate closed (the editor's ^C and prompt);
 *  - nothing a session queued reaches the next one (the transmit ring, the
 *    receive ring, the cancel mark), and the ring is emptied before a session
 *    is live;
 *  - IAC stripped, 0xFF doubled, the negotiation's six bytes whole or not at
 *    all, the gate read inside the writers' masked section;
 *  - a packet longer than the extraction buffer read to its end; a full
 *    receive ring counted;
 *  - the refusals by reason, and the short retry while bytes are left;
 *  - the "is it the telnet console" question, a background job included;
 *  - the `net info` lines in each state, and a failed write stops them;
 *  - the worker's entry only wakes: no NetX call, no masked section.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tx_api.h"
#include "nx_api.h"
#include "cli_instance.h"
#include "net_core.h"
#include "net_tty.h"

/* ---- the board's objects ------------------------------------------------------ */

#define T_RX_RING   32u
#define T_EXTRACT   16u
#define T_TX_RING   64u

static NX_IP          fake_ip;
static NX_PACKET_POOL fake_pool;
static uint8_t        t_rx_buf[T_RX_RING];
static uint8_t        t_extract[T_EXTRACT];
static uint8_t        t_tx_buf[T_TX_RING];
static ULONG          t_stack[128];

struct net_core_state net_core_board_state;     /* only link_falls is read */

struct net_tty_state net_tty_board_state;
const struct net_tty_facts net_tty_board_facts = {
	.ip             = &fake_ip,
	.pool           = &fake_pool,
	.rx_buf         = t_rx_buf,  .rx_size      = sizeof t_rx_buf,
	.extract_buf    = t_extract, .extract_size = sizeof t_extract,
	.tx_buf         = t_tx_buf,  .tx_size      = sizeof t_tx_buf,
	.stack          = t_stack,   .stack_size   = sizeof t_stack,
	.tx_queue_depth = 4u,
	.rto_ticks      = 2000u,
	.rto_retries    = 10u,
	.rto_shift      = 1u,
	.other_console  = "SERIAL",
};

NET_TTY_TRANSPORT_DEFINE(t_tr);
static struct cli_instance t_sh;
static struct cli_instance t_job;     /* a background job launched from t_sh */
static struct cli_instance t_other;   /* another console */

#define T_TCP_TIME_WAIT 11u     /* nx_api.h's NX_TCP_TIMED_WAIT */

#define S  (&net_tty_board_state)
#define NC (&net_core_board_state)

/* ---- the fakes ------------------------------------------------------------------- */

enum acc_outcome { ACC_OK = 0, ACC_HANDSHAKE_FAIL, ACC_FAIL_NOW };

#define LOG_MAX  64
#define PKT_MAX  8
#define PKT_DATA 128

struct fpkt {
	NX_PACKET p;
	uint8_t data[PKT_DATA];
	int in_use;
};

struct fake {
	/* the masked sections */
	int irq_depth, irq_sections;
	void (*on_disable)(void);    /* run once at the next disable */
	/* the event flags */
	ULONG flags;
	int flags_fail, flags_created;
	int waits;
	ULONG wait_req, wait_opt, flags_at_wait;
	void (*on_block)(void);      /* a wait that would block */
	int sets;
	/* the thread */
	int thread_fail, threads;
	VOID *stack; ULONG stack_size; UINT prio;
	/* the CLI */
	int n_rx, n_tx, n_conn;
	int live_at_tx;              /* live seen by the last notify_tx      */
	int cancel_at_rx;            /* the cancel mark seen by notify_rx    */
	void (*on_conn)(void);
	char err[256];
	/* the socket */
	int created, listening, bound, stale;
	UINT state;
	UINT port;
	VOID (*rx_cb)(NX_TCP_SOCKET *);
	VOID (*disc_cb)(NX_TCP_SOCKET *);
	VOID (*est_cb)(NX_TCP_SOCKET *);
	VOID (*q_cb)(NX_TCP_SOCKET *);
	ULONG cfg_depth, cfg_rto, cfg_retries, cfg_shift;
	UINT fail_create, fail_rx_notify, fail_est_notify, fail_q_notify;
	UINT fail_listen, fail_unaccept, fail_relisten;
	int pending;                 /* relisten finds a queued SYN          */
	enum acc_outcome acc;
	UINT acc_rc;
	int no_est_cb;               /* complete without the establish call  */
	void (*acc_hook)(void);      /* after the establish callback         */
	UINT est_state;              /* the state accept leaves (0: ESTAB)   */
	int accepts, leaks, disconnects, unaccept_refused;
	int disc_time_wait;          /* disconnect leaves TIME_WAIT          */
	/* packets */
	struct fpkt pk[PKT_MAX];
	int allocated;
	UINT alloc_rc, append_rc;
	UINT send_rc;
	void (*on_send)(void);
	int sends;
	uint8_t wire[4096];
	size_t wire_len;
	/* received packets, queued for nx_tcp_socket_receive() */
	struct fpkt *rxq[PKT_MAX];
	int rxq_n;
	/* NetX calls, in order, and any inside a masked section */
	char log[LOG_MAX][16];
	int nlog;
	int masked_netx;
};

static struct fake fk;

static void note(const char *what)
{
	if (fk.irq_depth != 0)
		fk.masked_netx++;
	if (fk.nlog < LOG_MAX)
		snprintf(fk.log[fk.nlog++], sizeof fk.log[0], "%s", what);
}

/* ThreadX */

ULONG tx_time_get(void) { return 0u; }

UINT tx_shim_disable(void)
{
	void (*h)(void) = fk.on_disable;

	fk.on_disable = NULL;
	if (h != NULL)
		h();                     /* "preempted just before the section" */
	fk.irq_depth++;
	fk.irq_sections++;
	return 0u;
}

void tx_shim_restore(UINT saved)
{
	(void)saved;
	fk.irq_depth--;
}

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *g, CHAR *name)
{
	(void)g; (void)name;
	if (fk.flags_fail)
		return 0x06u;
	fk.flags_created++;
	return TX_SUCCESS;
}

UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT opt)
{
	(void)g; (void)opt;
	fk.flags |= flags;
	fk.sets++;
	return TX_SUCCESS;
}

UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP *g, ULONG req, UINT opt,
                        ULONG *got, ULONG wait)
{
	(void)g;
	fk.waits++;
	fk.wait_req = req;
	fk.wait_opt = wait;
	fk.flags_at_wait = fk.flags;
	if ((fk.flags & req) == 0u && wait != TX_NO_WAIT && fk.on_block != NULL)
		fk.on_block();
	if ((fk.flags & req) == 0u)
		return TX_NO_EVENTS;
	*got = fk.flags & req;
	if (opt == TX_OR_CLEAR)
		fk.flags &= ~req;
	return TX_SUCCESS;
}

UINT tx_thread_create(TX_THREAD *t, CHAR *name, VOID (*entry)(ULONG),
                      ULONG input, VOID *stack, ULONG size, UINT prio,
                      UINT preempt, ULONG slice, UINT autostart)
{
	(void)t; (void)name; (void)entry; (void)input; (void)preempt;
	(void)slice; (void)autostart;
	if (fk.thread_fail)
		return 0x0Fu;
	fk.threads++;
	fk.stack = stack;
	fk.stack_size = size;
	fk.prio = prio;
	return TX_SUCCESS;
}

/* The CLI: notifications are recorded. */

void cli_transport_notify_rx(struct cli_instance *sh)
{
	(void)sh;
	fk.n_rx++;
	fk.cancel_at_rx = S->cancel;
}

void cli_transport_notify_tx(struct cli_instance *sh)
{
	(void)sh;
	fk.n_tx++;
	fk.live_at_tx = S->live;
}

void cli_transport_notify_conn(struct cli_instance *sh)
{
	(void)sh;
	fk.n_conn++;
	if (fk.on_conn != NULL)
		fk.on_conn();
}

int cli_error(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;

	(void)sh;
	va_start(ap, fmt);
	vsnprintf(fk.err, sizeof fk.err, fmt, ap);
	va_end(ap);
	return 0;
}

/* NetX: the socket */

UINT nx_tcp_socket_create(NX_IP *ip, NX_TCP_SOCKET *sock, CHAR *name,
                          ULONG tos, ULONG frag, UINT ttl, ULONG window,
                          VOID (*urgent)(NX_TCP_SOCKET *),
                          VOID (*disc)(NX_TCP_SOCKET *))
{
	(void)ip; (void)name; (void)tos; (void)frag; (void)ttl; (void)window;
	(void)urgent;
	note("create");
	if (fk.fail_create)
		return fk.fail_create;
	fk.created++;
	fk.state = NX_TCP_CLOSED;
	sock->nx_tcp_socket_state = NX_TCP_CLOSED;
	fk.disc_cb = disc;
	return NX_SUCCESS;
}

UINT nx_tcp_socket_receive_notify(NX_TCP_SOCKET *sock, VOID (*cb)(NX_TCP_SOCKET *))
{
	(void)sock;
	note("rx_notify");
	if (fk.fail_rx_notify)
		return fk.fail_rx_notify;
	fk.rx_cb = cb;
	return NX_SUCCESS;
}

UINT nx_tcp_socket_establish_notify(NX_TCP_SOCKET *sock, VOID (*cb)(NX_TCP_SOCKET *))
{
	(void)sock;
	note("est_notify");
	if (fk.fail_est_notify)
		return fk.fail_est_notify;
	fk.est_cb = cb;
	return NX_SUCCESS;
}

UINT nx_tcp_socket_queue_depth_notify_set(NX_TCP_SOCKET *sock,
                                          VOID (*cb)(NX_TCP_SOCKET *))
{
	(void)sock;
	note("q_notify");
	if (fk.fail_q_notify)
		return fk.fail_q_notify;
	fk.q_cb = cb;
	return NX_SUCCESS;
}

UINT nx_tcp_socket_transmit_configure(NX_TCP_SOCKET *sock, ULONG depth,
                                      ULONG timeout, ULONG retries, ULONG shift)
{
	(void)sock;
	note("configure");
	fk.cfg_depth = depth;
	fk.cfg_rto = timeout;
	fk.cfg_retries = retries;
	fk.cfg_shift = shift;
	return NX_SUCCESS;
}

static void set_state(NX_TCP_SOCKET *sock, UINT st)
{
	fk.state = st;
	sock->nx_tcp_socket_state = st;
}

UINT nx_tcp_server_socket_listen(NX_IP *ip, UINT port, NX_TCP_SOCKET *sock,
                                 UINT q, VOID (*cb)(NX_TCP_SOCKET *, UINT))
{
	(void)ip; (void)q; (void)cb;
	note("listen");
	if (fk.fail_listen)
		return fk.fail_listen;
	fk.port = port;
	fk.listening = 1;
	fk.bound = 0;
	set_state(sock, NX_TCP_LISTEN_STATE);
	return NX_SUCCESS;
}

UINT nx_tcp_server_socket_accept(NX_TCP_SOCKET *sock, ULONG wait)
{
	note("accept");
	if (wait != NX_WAIT_FOREVER)
		fk.leaks += 100;          /* the console never arms with a limit */
	if (fk.state == NX_TCP_ESTABLISHED)
		return NX_SUCCESS;
	if (fk.state != NX_TCP_LISTEN_STATE && fk.state != NX_TCP_SYN_RECEIVED)
		return NX_NOT_LISTEN_STATE;
	/* NetX's trap: bound to a peer that is gone -> a packet per call. */
	if (fk.bound && fk.stale)
		fk.leaks++;
	fk.accepts++;
	switch (fk.acc) {
	case ACC_FAIL_NOW:
		return fk.acc_rc;
	case ACC_HANDSHAKE_FAIL:
		set_state(sock, NX_TCP_LISTEN_STATE);
		fk.bound = 1;
		fk.stale = 1;
		return NX_NOT_CONNECTED;
	default:
		break;
	}
	set_state(sock, NX_TCP_ESTABLISHED);
	fk.bound = 1;
	fk.stale = 0;
	if (!fk.no_est_cb && fk.est_cb != NULL)
		fk.est_cb(sock);         /* the IP thread, before the resume */
	if (fk.acc_hook != NULL)
		fk.acc_hook();
	if (fk.est_state != 0u)
		set_state(sock, fk.est_state);
	return NX_SUCCESS;
}

/* NetX's judgement (nx_tcp_server_socket_unaccept.c): CLOSE_WAIT and later,
 * or CLOSED while still bound, first go back to LISTEN; anything not in
 * LISTEN then -- ESTABLISHED, SYN_RECEIVED, CLOSED and unbound -- is refused. */
UINT nx_tcp_server_socket_unaccept(NX_TCP_SOCKET *sock)
{
	note("unaccept");
	if (fk.fail_unaccept)
		return fk.fail_unaccept;
	if (fk.state >= NX_TCP_CLOSE_WAIT ||
	    (fk.state == NX_TCP_CLOSED && fk.bound))
		set_state(sock, NX_TCP_LISTEN_STATE);
	if (fk.state != NX_TCP_LISTEN_STATE) {
		fk.unaccept_refused++;
		return NX_NOT_LISTEN_STATE;
	}
	fk.bound = 0;
	fk.stale = 0;
	set_state(sock, NX_TCP_CLOSED);
	return NX_SUCCESS;
}

UINT nx_tcp_server_socket_relisten(NX_IP *ip, UINT port, NX_TCP_SOCKET *sock)
{
	(void)ip;
	note("relisten");
	if (fk.fail_relisten)
		return fk.fail_relisten;
	if (fk.state != NX_TCP_CLOSED)
		return NX_NOT_CLOSED;
	if (fk.bound)
		return NX_ALREADY_BOUND;
	if (port != fk.port)
		fk.leaks += 1000;         /* listening on another port */
	set_state(sock, NX_TCP_LISTEN_STATE);
	if (fk.pending) {
		fk.bound = 1;
		fk.stale = 0;
		return NX_CONNECTION_PENDING;
	}
	return NX_SUCCESS;
}

/* A server socket after disconnect (nx_tcp_socket_disconnect.c): back to
 * LISTEN, still bound to the port table with the peer's details gone -- or,
 * when the FIN exchange left it there, TIME_WAIT. */
UINT nx_tcp_socket_disconnect(NX_TCP_SOCKET *sock, ULONG wait)
{
	(void)wait;
	note("disconnect");
	fk.disconnects++;
	if (fk.state < NX_TCP_ESTABLISHED)
		return NX_NOT_CONNECTED;
	if (fk.disc_time_wait) {
		set_state(sock, T_TCP_TIME_WAIT);
	} else {
		set_state(sock, NX_TCP_LISTEN_STATE);
		fk.stale = 1;
	}
	fk.bound = 1;
	return NX_SUCCESS;
}

/* NetX: packets */

static struct fpkt *pkt_get(void)
{
	for (int i = 0; i < PKT_MAX; i++)
		if (!fk.pk[i].in_use) {
			memset(&fk.pk[i], 0, sizeof fk.pk[i]);
			fk.pk[i].in_use = 1;
			fk.pk[i].p.fake = &fk.pk[i];
			fk.allocated++;
			return &fk.pk[i];
		}
	return NULL;
}

UINT nx_packet_release(NX_PACKET *p)
{
	struct fpkt *f = (struct fpkt *)p->fake;

	f->in_use = 0;
	fk.allocated--;
	return NX_SUCCESS;
}

UINT nx_packet_allocate(NX_PACKET_POOL *pool, NX_PACKET **out, ULONG type,
                        ULONG wait)
{
	struct fpkt *f;

	(void)pool; (void)type; (void)wait;
	note("alloc");
	if (fk.alloc_rc)
		return fk.alloc_rc;
	f = pkt_get();
	if (f == NULL)
		return NX_NO_PACKET;
	*out = &f->p;
	return NX_SUCCESS;
}

UINT nx_packet_data_append(NX_PACKET *p, VOID *data, ULONG n,
                           NX_PACKET_POOL *pool, ULONG wait)
{
	struct fpkt *f = (struct fpkt *)p->fake;

	(void)pool; (void)wait;
	note("append");
	if (fk.append_rc)
		return fk.append_rc;
	if (p->nx_packet_length + n > PKT_DATA)
		return NX_NO_PACKET;
	memcpy(f->data + p->nx_packet_length, data, n);
	p->nx_packet_length += n;
	return NX_SUCCESS;
}

UINT nx_tcp_socket_send(NX_TCP_SOCKET *sock, NX_PACKET *p, ULONG wait)
{
	struct fpkt *f = (struct fpkt *)p->fake;

	(void)sock; (void)wait;
	note("send");
	fk.sends++;
	if (fk.on_send != NULL)
		fk.on_send();
	if (fk.send_rc)
		return fk.send_rc;
	if (fk.wire_len + p->nx_packet_length <= sizeof fk.wire) {
		memcpy(fk.wire + fk.wire_len, f->data, p->nx_packet_length);
		fk.wire_len += p->nx_packet_length;
	}
	f->in_use = 0;                /* NetX owns it now */
	fk.allocated--;
	return NX_SUCCESS;
}

UINT nx_tcp_socket_receive(NX_TCP_SOCKET *sock, NX_PACKET **out, ULONG wait)
{
	(void)sock; (void)wait;
	if (fk.rxq_n == 0)
		return NX_NO_PACKET;
	*out = &fk.rxq[0]->p;
	memmove(&fk.rxq[0], &fk.rxq[1], sizeof fk.rxq[0] * (size_t)(PKT_MAX - 1));
	fk.rxq_n--;
	return NX_SUCCESS;
}

UINT nx_packet_data_extract_offset(NX_PACKET *p, ULONG off, VOID *buf,
                                   ULONG cap, ULONG *got)
{
	struct fpkt *f = (struct fpkt *)p->fake;
	ULONG n;

	if (off >= p->nx_packet_length) {
		*got = 0u;
		return NX_SUCCESS;
	}
	n = p->nx_packet_length - off;
	if (n > cap)
		n = cap;
	memcpy(buf, f->data + off, n);
	*got = n;
	return NX_SUCCESS;
}

/* ---- harness ---------------------------------------------------------------------- */

static int failures;

static const char *calls(void)
{
	static char b[1024];
	size_t n = 0;

	b[0] = '\0';
	for (int i = 0; i < fk.nlog && n < sizeof b - 20; i++)
		n += (size_t)snprintf(b + n, sizeof b - n, "%s%s", i ? ", " : "",
		                      fk.log[i]);
	return b;
}

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       calls: %s\n", calls());
		failures++;
	}
}

static int calls_are(const char *want)
{
	return strcmp(calls(), want) == 0;
}

static void clear_calls(void)
{
	fk.nlog = 0;
	fk.masked_netx = 0;
}

/* A packet from the peer, then the IP thread's receive notify. */
static void peer_sends(const void *data, size_t n)
{
	struct fpkt *f = pkt_get();

	memcpy(f->data, data, n);
	f->p.nx_packet_length = (ULONG)n;
	fk.rxq[fk.rxq_n++] = f;
	fk.rx_cb(NULL);
}

/* The peer closes: the IP thread's disconnect callback. */
static void peer_closes(void)
{
	fk.disc_cb(NULL);
}

/* The link falls: net_core's notification counts it (IP thread), then the
 * worker wakes the console. */
static void link_falls_count_only(void)
{
	NC->link_falls++;
}

static void link_falls(void)
{
	NC->link_falls++;
	net_tty_link_fell();
}

static int step(void)
{
	return net_tty_listener_step();
}

/* What the instance reads now (as the core does). */
static int tr_read(uint8_t *buf, size_t cap)
{
	return t_tr.api->read(&t_tr, buf, cap);
}

static int tr_write(const void *d, size_t n)
{
	return t_tr.api->write(&t_tr, (const uint8_t *)d, n);
}

/* The instance thread, on CLI_EVT_CONN. */
static void session_begin(void)
{
	t_tr.api->session_begin(&t_tr);
}

/* A fresh console: transport inited (as cli_init does), net_tty_init(). */
static void fresh(void)
{
	memset(&fk, 0, sizeof fk);
	memset(S, 0, sizeof *S);
	memset(NC, 0, sizeof *NC);
	memset(&t_sh, 0, sizeof t_sh);
	memset(&t_job, 0, sizeof t_job);
	memset(&t_other, 0, sizeof t_other);
	memset(&fake_pool, 0, sizeof fake_pool);
	fake_pool.nx_packet_pool_available = 30u;
	t_sh.tr = &t_tr;
	t_tr.sh = &t_sh;
	t_job.fg = &t_sh;
	t_job.tr = &t_tr;
	(void)t_tr.api->init(&t_tr);
	(void)net_tty_init();
}

/* Fresh and listening on @p port (one step: the port, the open). */
static void listening(uint16_t port)
{
	fresh();
	net_tty_port_set(port);
	(void)step();
	clear_calls();
}

/* Listening, then one accepted and published session, its gate open, the
 * negotiation sent and the counters (but the session count) cleared. */
static void in_session(void)
{
	uint32_t sessions;

	listening(23u);
	(void)step();                /* accept -> published */
	session_begin();             /* the instance thread */
	(void)step();                /* the negotiation goes out */
	sessions = S->st.sessions;
	memset(&S->st, 0, sizeof S->st);
	S->st.sessions = sessions;
	fk.wire_len = 0;
	fk.flags = 0u;
	clear_calls();
	fk.n_rx = fk.n_tx = fk.n_conn = 0;
}

/* ---- init and the port ------------------------------------------------------------ */

static void check_init(void)
{
	fresh();
	expect("init: the flags and the listener at priority 14 on the board's stack",
	       fk.flags_created == 1 && fk.threads == 1 &&
	       fk.prio == NET_TTY_PRIORITY && fk.stack == t_stack &&
	       fk.stack_size == sizeof t_stack && S->ready == 1 &&
	       S->listen == NET_TTY_L_NOPORT && fk.nlog == 0);

	memset(&fk, 0, sizeof fk);
	memset(S, 0, sizeof *S);
	fk.flags_fail = 1;
	expect("init: a flags failure leaves it not ready",
	       net_tty_init() == -1 && S->ready == 0 && fk.threads == 0);
	net_tty_port_set(23u);
	expect("init: ... and the port is ignored, nothing woken",
	       S->port_given == 0 && fk.sets == 0);
	memset(&fk, 0, sizeof fk);
	fk.thread_fail = 1;
	expect("init: a thread failure leaves it not ready",
	       net_tty_init() == -1 && S->ready == 0);

	fresh();
	net_tty_port_set(2323u);
	net_tty_port_set(23u);
	expect("port: the first call wins, one wake", S->port == 2323u &&
	       fk.sets == 1);
	expect("port: the listener opens on it", step() == 0 &&
	       calls_are("create, rx_notify, est_notify, q_notify, configure, listen") &&
	       fk.port == 2323u && S->listen == NET_TTY_L_LISTENING &&
	       S->phase == NET_TTY_PH_ACCEPT && fk.masked_netx == 0);
	expect("port: the board's transmit settings",
	       fk.cfg_depth == 4u && fk.cfg_rto == 2000u && fk.cfg_retries == 10u &&
	       fk.cfg_shift == 1u);

	fresh();
	fk.on_block = NULL;
	expect("port: no port, the listener waits and opens nothing",
	       step() == 0 && fk.nlog == 0 && fk.wait_req == 0x1u &&
	       fk.wait_opt == TX_WAIT_FOREVER);
}

static void check_open_failures(void)
{
	static const struct { const char *name; int which; uint8_t op; } t[] = {
		{ "create",   0, NET_TTY_OP_CREATE },
		{ "rx",       1, NET_TTY_OP_NOTIFY },
		{ "est",      2, NET_TTY_OP_NOTIFY },
		{ "q",        3, NET_TTY_OP_NOTIFY },
		{ "listen",   4, NET_TTY_OP_LISTEN },
	};
	char what[96];

	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		fresh();
		switch (t[i].which) {
		case 0: fk.fail_create = 0x0Du; break;
		case 1: fk.fail_rx_notify = 0x0Du; break;
		case 2: fk.fail_est_notify = 0x0Du; break;
		case 3: fk.fail_q_notify = 0x0Du; break;
		default: fk.fail_listen = 0x0Du; break;
		}
		net_tty_port_set(23u);
		snprintf(what, sizeof what, "open: %s fails -> not listening, the step "
		         "and the answer kept, stopped", t[i].name);
		expect(what, step() == -1 && S->listen == NET_TTY_L_FAILED &&
		       S->fail_op == t[i].op && S->fail_status == 0x0Du &&
		       S->phase == NET_TTY_PH_STOPPED);
		clear_calls();
		snprintf(what, sizeof what, "open: %s ... not retried", t[i].name);
		expect(what, step() == -1 && fk.nlog == 0 && fk.accepts == 0);
	}
}

/* ---- one accept per connection ---------------------------------------------------- */

static void check_accept_once(void)
{
	listening(23u);
	fk.acc = ACC_HANDSHAKE_FAIL;
	expect("handshake: a failed handshake -> unaccept, relisten",
	       step() == 0 && calls_are("accept, unaccept, relisten") &&
	       S->phase == NET_TTY_PH_ACCEPT && S->listen == NET_TTY_L_LISTENING);
	clear_calls();
	fk.acc = ACC_OK;
	expect("handshake: ... then the next arm, and no packet leaked",
	       step() == 0 && calls_are("accept") && fk.leaks == 0 &&
	       S->phase == NET_TTY_PH_SESSION && fk.n_conn == 1);

	listening(23u);
	fk.acc = ACC_HANDSHAKE_FAIL;
	for (int i = 0; i < 20; i++)
		(void)step();
	fk.acc = ACC_OK;
	(void)step();
	{
		int acc = 0, un = 0, re = 0;

		for (int i = 0; i < fk.nlog; i++) {
			acc += strcmp(fk.log[i], "accept") == 0;
			un += strcmp(fk.log[i], "unaccept") == 0;
			re += strcmp(fk.log[i], "relisten") == 0;
		}
		expect("handshake: twenty cut handshakes, then a client: one arm each, "
		       "each after unaccept + relisten, no leak, the client published",
		       acc == 21 && un == 20 && re == 20 && fk.leaks == 0 &&
		       S->phase == NET_TTY_PH_SESSION && fk.n_conn == 1);
	}

	listening(23u);
	fk.acc = ACC_HANDSHAKE_FAIL;
	fk.fail_unaccept = 0x2Du;
	expect("handshake: an unaccept that fails stops the listener",
	       step() == -1 && S->listen == NET_TTY_L_FAILED &&
	       S->fail_op == NET_TTY_OP_UNACCEPT && S->fail_status == 0x2Du);
	clear_calls();
	expect("handshake: ... and accept is not called again",
	       step() == -1 && fk.nlog == 0 && fk.leaks == 0);

	listening(23u);
	fk.acc = ACC_HANDSHAKE_FAIL;
	fk.fail_relisten = 0x23u;
	expect("handshake: a relisten that fails stops the listener",
	       step() == -1 && S->fail_op == NET_TTY_OP_RELISTEN &&
	       S->fail_status == 0x23u && S->listen == NET_TTY_L_FAILED);
	clear_calls();
	expect("handshake: ... and accept is not called again",
	       step() == -1 && fk.nlog == 0);

	listening(23u);
	fk.acc = ACC_HANDSHAKE_FAIL;
	fk.pending = 1;
	expect("handshake: relisten with a connection pending is success",
	       step() == 0 && S->listen == NET_TTY_L_LISTENING &&
	       S->phase == NET_TTY_PH_ACCEPT);

	listening(23u);
	fk.acc = ACC_FAIL_NOW;
	fk.acc_rc = NX_NOT_LISTEN_STATE;
	expect("arm: an accept that fails at once stops the listener",
	       step() == -1 && S->fail_op == NET_TTY_OP_ACCEPT &&
	       S->fail_status == NX_NOT_LISTEN_STATE && fk.accepts == 1);
}

/* ---- the publish ---------------------------------------------------------------------- */

static void hook_fall(void)  { link_falls(); }
static void hook_close(void) { peer_closes(); }
static void hook_fall_up(void) { link_falls_count_only(); }

static void check_publish(void)
{
	/* What happens around the accept's return, and whether it publishes. */
	static const struct {
		const char *name;
		int fall_before;           /* the link fell before the SYN    */
		void (*hook)(void);        /* after the establishment         */
		int no_est;                /* the establish callback never ran */
		UINT state;                /* the state left (0: ESTABLISHED)  */
		int published;
		uint8_t end;
	} t[] = {
		{ "established, nothing else",           0, NULL,       0, 0, 1, NET_TTY_END_NONE },
		{ "fall, up, then established",          1, NULL,       0, 0, 1, NET_TTY_END_NONE },
		{ "established, then a fall",            0, hook_fall,  0, 0, 0, NET_TTY_END_LINK },
		{ "established, then a fall (no wake)",  0, hook_fall_up, 0, 0, 0, NET_TTY_END_LINK },
		{ "established, then the peer closed",   0, hook_close, 0, 0, 0, NET_TTY_END_PEER },
		{ "established, then CLOSE_WAIT",        0, NULL,       0, NX_TCP_CLOSE_WAIT, 0,
		  NET_TTY_END_LEFT },
		{ "no establish callback",               0, NULL,       1, 0, 0, NET_TTY_END_LEFT },
	};
	char what[128];

	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		listening(23u);
		if (t[i].fall_before)
			link_falls();
		fk.acc_hook = t[i].hook;
		fk.no_est_cb = t[i].no_est;
		fk.est_state = t[i].state;
		(void)step();
		snprintf(what, sizeof what, "publish: %s -> %s", t[i].name,
		         t[i].published ? "published" : "not published, ended");
		if (t[i].published)
			expect(what, fk.n_conn == 1 && S->live == 1 &&
			       S->st.sessions == 1u && S->phase == NET_TTY_PH_SESSION &&
			       calls_are("accept"));
		else
			expect(what, fk.n_conn == 0 && S->live == 0 &&
			       S->st.sessions == 0u && S->phase == NET_TTY_PH_ACCEPT &&
			       S->last_end == t[i].end &&
			       calls_are("accept, disconnect, unaccept, relisten"));
	}

	/* A published session: a fall before the first pass ends it before any
	 * wait (the count is compared first). */
	listening(23u);
	fk.on_conn = hook_fall_up;     /* the fall lands after the check */
	(void)step();
	expect("publish: a fall right after the publish leaves it published",
	       fk.n_conn == 1 && S->phase == NET_TTY_PH_SESSION);
	fk.on_conn = NULL;
	clear_calls();
	fk.waits = 0;
	(void)step();
	expect("publish: ... the first pass ends it on the count, before waiting",
	       fk.waits == 0 && S->last_end == NET_TTY_END_LINK &&
	       calls_are("disconnect, unaccept, relisten"));

	/* The ring is thrown away before the session is live. */
	listening(23u);
	fk.n_tx = 0;
	fk.live_at_tx = -1;
	(void)step();
	expect("publish: the ring is emptied while the session is not yet live",
	       fk.n_tx >= 1 && fk.live_at_tx == 0 && S->live == 1);
}

/* ---- the session ------------------------------------------------------------------------ */

static void check_session_falls(void)
{
	/* A fall while listening does not end the next session. */
	listening(23u);
	link_falls();
	(void)step();
	clear_calls();
	fk.waits = 0;
	(void)step();
	expect("falls: a fall while listening leaves the next session open",
	       S->phase == NET_TTY_PH_SESSION && fk.waits == 1 && fk.nlog == 0);

	/* The boundary: the same count stays, one more ends. */
	in_session();
	(void)step();
	expect("falls: the same count, the session stays", S->live == 1 &&
	       S->phase == NET_TTY_PH_SESSION);
	link_falls();
	(void)step();
	expect("falls: one more fall, the session ends on the link",
	       S->live == 0 && S->last_end == NET_TTY_END_LINK &&
	       S->phase == NET_TTY_PH_ACCEPT && S->listen == NET_TTY_L_LISTENING);

	/* A wake after the comparison is not thrown away before the wait. */
	in_session();
	(void)tr_write("abc", 3);
	fk.on_send = hook_fall;        /* the fall arrives while draining */
	fk.waits = 0;
	(void)step();
	fk.on_send = NULL;
	expect("falls: a wake that came after the comparison is there at the wait",
	       fk.waits == 1 && (fk.flags_at_wait & 0x8u) != 0u &&
	       S->phase == NET_TTY_PH_SESSION);
	(void)step();
	expect("falls: ... and the next pass ends the session",
	       S->last_end == NET_TTY_END_LINK && S->phase == NET_TTY_PH_ACCEPT);
}

static void check_ends(void)
{
	static const char *names[] = { "peer", "link", "left" };
	static const uint8_t ends[] = { NET_TTY_END_PEER, NET_TTY_END_LINK,
	                                NET_TTY_END_LEFT };
	char what[128];
	uint8_t b[8];

	for (int k = 0; k < 3; k++) {
		in_session();
		fk.send_rc = NX_TX_QUEUE_DEPTH;  /* keep bytes queued */
		(void)tr_write("hello", 5);
		(void)step();                    /* refused: stays in the ring */
		fk.send_rc = 0u;
		fk.n_rx = 0;
		fk.cancel_at_rx = 0;
		clear_calls();
		if (k == 0)
			peer_closes();
		else if (k == 1)
			link_falls();
		else
			set_state(&S->sock, NX_TCP_CLOSE_WAIT);
		(void)step();
		snprintf(what, sizeof what, "end (%s): gate closed, ring thrown away "
		         "and counted, back to listening", names[k]);
		expect(what, S->connected == 0 && S->live == 0 &&
		       cli_uart_ring_count(&S->tx) == 0u && S->st.tx_dropped == 5u &&
		       S->last_end == ends[k] && S->phase == NET_TTY_PH_ACCEPT &&
		       S->listen == NET_TTY_L_LISTENING &&
		       calls_are("disconnect, unaccept, relisten") &&
		       fk.masked_netx == 0);
		snprintf(what, sizeof what, "end (%s): the instance told after the "
		         "cancel mark, the next read is one 0x03", names[k]);
		expect(what, fk.n_rx == 1 && fk.cancel_at_rx == 1 &&
		       tr_read(b, sizeof b) == 1 && b[0] == 0x03u &&
		       tr_read(b, sizeof b) == 0);
	}

	/* The cancel reaches a command even with the receive ring full. */
	in_session();
	for (int i = 0; i < 40; i++)
		peer_sends("x", 1);
	expect("end: the receive ring is full (and the overflow counted)",
	       cli_uart_ring_free(&S->rx) == 0u && S->st.rx_drops == 40u - (T_RX_RING - 1u));
	peer_closes();
	(void)step();
	expect("end: with the ring full the first byte read is still the 0x03",
	       tr_read(b, 1) == 1 && b[0] == 0x03u);
	expect("end: ... and after it nothing: the gone connection's bytes are "
	       "consumed, not returned",
	       tr_read(b, sizeof b) == 0 && cli_uart_ring_count(&S->rx) == 0u);

	/* A disconnect that leaves TIME_WAIT still goes back to listening. */
	in_session();
	fk.disc_time_wait = 1;
	link_falls();
	(void)step();
	expect("end: a disconnect into TIME_WAIT -> unaccept, relisten, listening",
	       S->phase == NET_TTY_PH_ACCEPT && S->listen == NET_TTY_L_LISTENING &&
	       fk.unaccept_refused == 0 && fk.state == NX_TCP_LISTEN_STATE);
}

/*
 * A command that never reads (so never sees the cancel) runs from the end of
 * one connection past the publish of the next; the gone connection's line is
 * still in the ring when it returns, and the instance's read loop reads before
 * it gets to CLI_EVT_CONN.  Only the 0x03 may come out.
 */
static void check_unread_past_publish(void)
{
	uint8_t b[64];

	in_session();
	peer_sends("reboot\r", 7);    /* typed while the command runs, unread */
	peer_closes();
	(void)step();                  /* end: nothing has read since */
	(void)step();                  /* the next client, published */
	expect("unread: the next connection is published, the old line still queued",
	       fk.n_conn == 1 && S->live == 1 && cli_uart_ring_count(&S->rx) == 7u);
	expect("unread: the command returns and the loop reads: the 0x03, not the line",
	       tr_read(b, sizeof b) == 1 && b[0] == 0x03u &&
	       tr_read(b, sizeof b) == 0);
	session_begin();
	peer_sends("ls\r", 3);
	expect("unread: after the session begins the new input is read",
	       tr_read(b, sizeof b) == 3 && memcmp(b, "ls\r", 3) == 0);
}

static void check_next_session_clean(void)
{
	uint8_t b[64];

	in_session();
	fk.send_rc = NX_TX_QUEUE_DEPTH;
	(void)tr_write("old-output", 10);
	(void)step();
	fk.send_rc = 0u;
	peer_sends("typed", 5);
	peer_closes();
	(void)step();                 /* end */
	/* A whole line from the gone connection, after the end and before the
	 * next one: the idle editor must never see it. */
	peer_sends("reboot\r", 7);
	expect("next: after the end only the 0x03 is read, the old lines are not",
	       tr_read(b, sizeof b) == 1 && b[0] == 0x03u &&
	       tr_read(b, sizeof b) == 0);
	expect("next: the editor's ^C and prompt after the end are not counted lost",
	       tr_write("^C\r\nnet> ", 10) == 10 && S->st.tx_dropped == 10u);
	/* Bytes a writer still had are swallowed after the end. */
	expect("next: a write after the end is swallowed (gate closed)",
	       tr_write("late", 4) == 4 && cli_uart_ring_count(&S->tx) == 0u);
	(void)step();                 /* the next client, published */
	session_begin();
	expect("next: the new session gets the negotiation only, nothing old",
	       cli_uart_ring_count(&S->tx) == 6u && S->connected == 1 &&
	       tr_read(b, sizeof b) == 0 && S->cancel == 0);
	peer_sends("ls\r", 3);
	expect("next: the new connection's input is read again",
	       tr_read(b, sizeof b) == 3 && memcmp(b, "ls\r", 3) == 0);
	fk.wire_len = 0;
	(void)step();
	expect("next: on the wire, the six negotiation bytes whole",
	       fk.wire_len == 6u && memcmp(fk.wire, "\xFF\xFB\x01\xFF\xFB\x03", 6) == 0);
}

/* ---- the bytes ---------------------------------------------------------------------- */

static void end_gate_now(void)
{
	/* The listener's end, preempting a writer just before its section. */
	S->connected = 0u;
	S->live = 0u;
	cli_uart_ring_advance_tail(&S->tx, cli_uart_ring_count(&S->tx));
}

static void check_bytes(void)
{
	uint8_t b[64];
	int n;

	in_session();
	peer_sends("a\xFF\xFB\x01" "b\xFF\xFF" "c\xFF\xF1" "d", 11);
	n = tr_read(b, sizeof b);
	expect("rx: IAC commands stripped, IAC IAC is one 0xFF",
	       n == 5 && memcmp(b, "ab\xFF" "cd", 5) == 0 && fk.n_rx == 1);

	{
		uint8_t big[40];

		for (int i = 0; i < 40; i++)
			big[i] = (uint8_t)('A' + i % 26);
		fk.n_rx = 0;
		peer_sends(big, 25);       /* longer than the 16-byte extraction */
		n = tr_read(b, sizeof b);
		expect("rx: a packet longer than the extraction buffer is read to its end",
		       n == 25 && memcmp(b, big, 25) == 0 && fk.allocated == 0);
	}

	in_session();
	(void)cli_uart_ring_advance_tail(&S->tx, cli_uart_ring_count(&S->tx));
	expect("tx: 0xFF doubled", tr_write("\x01\xFF\x02", 3) == 3 &&
	       cli_uart_ring_count(&S->tx) == 4u);
	fk.wire_len = 0;
	(void)step();
	expect("tx: ... on the wire as 01 FF FF 02",
	       fk.wire_len == 4u && memcmp(fk.wire, "\x01\xFF\xFF\x02", 4) == 0);

	/* An 0xFF with room for one byte only is refused whole. */
	in_session();
	(void)cli_uart_ring_advance_tail(&S->tx, cli_uart_ring_count(&S->tx));
	{
		uint8_t fill[T_TX_RING];

		memset(fill, 'f', sizeof fill);
		n = tr_write(fill, T_TX_RING - 2u);   /* one slot left */
		expect("tx: an 0xFF with one slot left is not split",
		       n == (int)(T_TX_RING - 2u) && tr_write("\xFF", 1) == 0 &&
		       cli_uart_ring_free(&S->tx) == 1u);
	}

	/* The negotiation needs six slots or none go in. */
	in_session();
	{
		uint8_t fill[T_TX_RING];

		memset(fill, 'f', sizeof fill);
		(void)tr_write(fill, T_TX_RING - 4u);    /* three slots left */
	}
	session_begin();
	expect("tx: the negotiation does not go in by halves",
	       cli_uart_ring_free(&S->tx) == 3u);

	/* The gate is read inside the writers' masked section. */
	in_session();
	(void)cli_uart_ring_advance_tail(&S->tx, cli_uart_ring_count(&S->tx));
	fk.on_disable = end_gate_now;
	n = tr_write("stale", 5);
	expect("tx: the end landing just before a writer's section: nothing queued",
	       n == 5 && cli_uart_ring_count(&S->tx) == 0u);

	/* A write with nobody attached is swallowed and counted. */
	fresh();
	expect("tx: before any session a write is swallowed, not counted",
	       tr_write("prompt", 6) == 6 && S->st.tx_dropped == 0u);
}

static void check_refusals(void)
{
	in_session();
	(void)tr_write("x", 1);
	fk.send_rc = NX_TX_QUEUE_DEPTH;
	(void)step();
	fk.send_rc = NX_WINDOW_OVERFLOW;
	(void)step();
	fk.send_rc = NX_NOT_CONNECTED;
	(void)step();
	fk.send_rc = 0u;
	fk.alloc_rc = NX_NO_PACKET;
	(void)step();
	fk.alloc_rc = 0u;
	fk.append_rc = NX_NO_PACKET;
	(void)step();
	fk.append_rc = 0u;
	fake_pool.nx_packet_pool_available = NET_TTY_POOL_RESERVE;
	(void)step();
	expect("refused: queue, window and other apart; pool / allocate / append "
	       "as no-buffer; no packet kept",
	       S->st.tx_qdepth == 1u && S->st.tx_win == 1u && S->st.tx_err == 1u &&
	       S->st.tx_nobuf == 3u && fk.allocated == 0 &&
	       cli_uart_ring_count(&S->tx) == 1u);
	expect("refused: with bytes left the wait is the short retry",
	       fk.wait_opt == NET_TTY_DRAIN_POLL);
	fake_pool.nx_packet_pool_available = 30u;
	(void)step();
	expect("refused: ... and the next pass sends it", S->st.tx_segs == 1u &&
	       S->st.tx_bytes == 1u && cli_uart_ring_count(&S->tx) == 0u &&
	       fk.wait_opt == TX_WAIT_FOREVER);
}

/* ---- the question, the lines, the worker's entry ------------------------------------- */

static char wbuf[1024];
static size_t wlen;
static int wcalls, wfail_at;

static int w(void *ctx, const char *s, size_t n)
{
	(void)ctx;
	wcalls++;
	if (wfail_at != 0 && wcalls == wfail_at)
		return -1;
	if (wlen + n < sizeof wbuf) {
		memcpy(wbuf + wlen, s, n);
		wlen += n;
		wbuf[wlen] = '\0';
	}
	return (int)n;
}

static void wreset(void)
{
	wlen = 0;
	wbuf[0] = '\0';
	wcalls = 0;
	wfail_at = 0;
}

static void check_console_and_lines(void)
{
	fresh();
	expect("console: the bound instance and a job launched from it are telnet",
	       net_tty_is_console(&t_sh) && net_tty_is_console(&t_job) &&
	       !net_tty_is_console(&t_other) && !net_tty_is_console(NULL));
	expect("console: the guard refuses there, naming the other console",
	       net_tty_guard(&t_sh, "ymodem") == 1 &&
	       strcmp(fk.err, "ymodem: refused on the telnet console -- it would "
	              "destroy this session. Run it from the SERIAL console.\r\n") == 0);
	fk.err[0] = '\0';
	expect("console: ... and lets the others run",
	       net_tty_guard(&t_other, "ymodem") == 0 && fk.err[0] == '\0');

	fresh();
	wreset();
	expect("lines: no port yet", net_tty_write_lines(w, NULL) == 0 &&
	       strstr(wbuf, "telnet: not listening yet (no port)\r\n") != NULL &&
	       wcalls == 2);

	in_session();
	S->st.rx_drops = 7u;
	wreset();
	expect("lines: listening with a client",
	       net_tty_write_lines(w, NULL) == 0 &&
	       strstr(wbuf, "telnet: :23 listening, client attached, 1 sessions, "
	                    "last ended: none yet\r\n") != NULL &&
	       strstr(wbuf, "telnet: rx drop 7, tx ") != NULL);
	peer_closes();
	(void)step();
	wreset();
	(void)net_tty_write_lines(w, NULL);
	expect("lines: ... after the peer closed",
	       strstr(wbuf, "no client, 1 sessions, last ended: the peer closed") != NULL);

	listening(23u);
	fk.acc = ACC_HANDSHAKE_FAIL;
	fk.fail_relisten = 0x23u;
	(void)step();
	wreset();
	(void)net_tty_write_lines(w, NULL);
	expect("lines: stopped: the step and NetX's answer",
	       strstr(wbuf, "telnet: NOT listening on :23 -- relisten failed (0x23)") != NULL);
	wreset();
	wfail_at = 1;
	expect("lines: a failed write stops them",
	       net_tty_write_lines(w, NULL) == -1 && wcalls == 1);

	memset(S, 0, sizeof *S);
	wreset();
	expect("lines: before init, one line says so",
	       net_tty_write_lines(w, NULL) == 0 &&
	       strstr(wbuf, "telnet: not available") != NULL && wcalls == 1);

	listening(23u);
	fk.irq_sections = 0;
	fk.sets = 0;
	net_tty_link_fell();
	expect("worker entry: only a wake -- no NetX call, no masked section",
	       fk.nlog == 0 && fk.irq_sections == 0 && fk.sets == 1 &&
	       (fk.flags & 0x8u) != 0u);
}

int main(void)
{
	printf("test_net_tty:\n");
	check_init();
	check_open_failures();
	check_accept_once();
	check_publish();
	check_session_falls();
	check_ends();
	check_next_session_clean();
	check_unread_past_publish();
	check_bytes();
	check_refusals();
	check_console_and_lines();
	if (failures != 0) {
		printf("test_net_tty: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_net_tty: all passed\n");
	return 0;
}
