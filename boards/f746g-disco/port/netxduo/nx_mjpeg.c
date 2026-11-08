/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_mjpeg.c
 * @brief   MJPEG-over-HTTP camera streaming server (owhinata/stm32f746g-disco#49 P5).  See
 * nx_mjpeg.h.
 *
 * The camera frame pipeline's "eth_sink": a SYNCHRONOUS copy push sink.  In the
 * producer thread, consume() memcpy's the JPEG frame into a private SDRAM buffer
 * and puts the pin as its LAST STATEMENT.  The HTTP server thread then sends
 * mjpeg_buf out as one multipart/x-mixed-replace part, decoupled from the
 * pipeline.  A single buf_busy flag (set by consume after the copy, cleared by
 * the HTTP thread on every send-exit path) gates the one-deep handoff.
 *
 * [!] This file used to say "in-flight is always 0, exactly like the GUIX sink,
 * so the camera's async teardown stays correct".  True of the consume BODY and
 * false at detach, which is the only moment it was being claimed for:
 * camera_unsubscribe() detaches while the base keeps running, so a publish can
 * already be in flight across the unlink.  It was doubly wrong here, because
 * consume() also set buf_busy and posted the semaphore AFTER the put -- work the
 * count could not see at all.  nx_mjpeg_stop() now drains the pin (issue #72).
 *
 * Lifecycle: one thread created once and parked when stopped; the socket create
 * + listen run in the thread (thread-only NetX APIs).
 */
#include <stdio.h>           /* snprintf */
#include <string.h>          /* memcpy / memset */

#include "nx_api.h"

#include "nx_mjpeg.h"
#include "nx_glue.h"         /* nx_net_ip / nx_net_pool / nx_net_is_up */
#include "camera.h"          /* camera_subscribe_oneshot / camera_subscribed / camera_frame_put */
#include "cam_own.h"         /* the owner lifecycle (issue #72)                */
#include "frame_pipeline.h"  /* struct frame_sink / frame_desc / FRAME_POLICY_* */

#define LOG_TAG "mjpeg"
#include "log.h"

#define NX_MJPEG_PORT        80u
#define NX_MJPEG_WINDOW      2048u       /* TCP receive window (we barely read)   */
#define NX_MJPEG_TX_QUEUE    8u          /* cap unacked TX packets -> protect pool */
#define MJPEG_MSS_CAP        1400u       /* max bytes per TX packet               */
#define MJPEG_BUF_BYTES      262144u     /* = JPEG frame budget (65535*4): a frame
                                            never exceeds it, so no oversized drop */
#define MJPEG_PRIORITY       14          /* below IP(12)/DHCP(13), with net-shell */
#define MJPEG_STACK          2048u
#define MJPEG_ACCEPT_TICKS   500u        /* connect poll slice -> stop latency     */
#define MJPEG_POLL_TICKS     200u        /* frame wait -> disconnect latency       */
#define MJPEG_ALLOC_TICKS    100u        /* packet alloc wait                      */
#define MJPEG_SEND_TICKS     NX_IP_PERIODIC_RATE  /* per-chunk send wait (1 s)     */
/* Retransmission (issue #202).  The first retransmit 300 ms after a send, then
   doubling (shift 1): 0.3 + 0.6 + 1.2 + 2.4 + 4.8 s, so with 4 retries TCP
   resets a connection that has acknowledged nothing for ~9.3 s.  NetX counts
   this timeout in ticks and decrements it every 100 ms
   (nx_tcp_fast_periodic_processing.c:119-124, nx_tcp_enable.c:111), so a
   value that is not a multiple of 100 rounds up.  It used to be a fixed 2 s,
   longer than the 1 s send wait that then gave up on the connection: one lost
   burst of 8 packets was enough to close it. */
#define MJPEG_RTO_TICKS      300u
#define MJPEG_RTO_RETRIES    4u
#define MJPEG_RTO_SHIFT      1u
/* Close a connection only when TCP says it is gone, or when no send has gone
   through for this long (wall clock from the start of the first failing send,
   not a count of sends or retransmits). */
#define MJPEG_STALL_TICKS    (10u * NX_IP_PERIODIC_RATE)
#define MJPEG_BOUNDARY       "mjpegstream"
/* Teardown budgets (ticks, WALL CLOCK; 1 tick = 1 ms).  The sink drain is short
   -- a healthy consume() is one memcpy from its put -- while the server thread
   keeps the 3 s it always had, because it has a socket to tear down.  Wall clock
   rather than a count of sleeps: counting iterations burns the whole budget the
   moment the sleeps stop sleeping (issue #65). */
#define MJPEG_DRAIN_TICKS    100u        /* sink pin -> release                    */
#define MJPEG_SETTLE_TICKS   3000u       /* server thread parks (socket teardown)  */

static const char http_header[] =
	"HTTP/1.0 200 OK\r\n"
	"Connection: close\r\n"
	"Cache-Control: no-cache\r\n"
	"Pragma: no-cache\r\n"
	"Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
	"\r\n";

/* The private copy buffer lives with the NetX traffic in FMC bank2/3 (.sdram.eth),
   away from the camera DMA arena (bank1) and the LTDC surface (bank0) --
   owhinata/stm32f746g-disco#65. */
static uint8_t mjpeg_buf[MJPEG_BUF_BYTES]
	__attribute__((aligned(32), section(".sdram.eth")));

static TX_THREAD     mjpeg_thread;
static UCHAR         mjpeg_stack[MJPEG_STACK];
static NX_TCP_SOCKET sock;
static TX_SEMAPHORE  frame_sem;          /* consume posts; the server sends        */

static volatile int  mjpeg_run;          /* requested running (start=1, stop=0)    */
static volatile int  mjpeg_active;       /* thread owns the socket lifecycle       */
static volatile int  mjpeg_listening;    /* socket actually in LISTEN              */
static volatile int  client_connected;   /* a browser is connected (output gate)   */
static volatile int  producer_dead;      /* camera producer torn down (close cb)   */
static volatile int  buf_busy;           /* mjpeg_buf holds a frame to send         */
static volatile uint32_t buf_len;        /* valid bytes in mjpeg_buf               */
static int           mjpeg_created;      /* the thread has been created once       */

/* The mjpeg server lifecycle (issue #72), serialised by cam_own.h.  It guards
   the SINK -- which entry points may attach it, and the interval in which a stop
   has detached it and is watching its pins fall to zero.  mjpeg_run/mjpeg_active
   stay and describe the server THREAD, which outlives a stop that returned -2. */
static volatile enum cam_own_state mjpeg_own;
static uint8_t       cur_res;            /* enum camera_res of the active stream    */

static struct {
	uint32_t conns;
	uint32_t sent_frames;
	uint32_t sent_bytes;
	uint32_t drop_busy;
	uint32_t drop_oversized;
	uint32_t send_err;
	uint32_t pool_fail;
	/* issue #202: what nx_tcp_socket_send() answered when it failed (send_err
	   is their sum), and the last code. */
	uint32_t send_window;            /* NX_WINDOW_OVERFLOW                       */
	uint32_t send_queue;             /* NX_TX_QUEUE_DEPTH                        */
	uint32_t send_notconn;           /* NX_NOT_CONNECTED                         */
	uint32_t send_nopkt;             /* NX_NO_PACKET                             */
	uint32_t send_other;             /* anything else                            */
	uint32_t send_last;              /* the last failing code                    */
	/* What the connection had sent when that send failed (issue #202). */
	uint32_t fail_pkts;
	uint32_t fail_bytes;
	uint32_t fail_frames;
	/* The re-accept after a client (issue #202).  relisten_fail counts every
	   failure, relisten_pending the NX_CONNECTION_PENDING ones among them --
	   which stays 0 since PENDING is taken as the success it is
	   (relisten_pending_ok below, mjpeg_relisten()). */
	uint32_t relisten_fail;
	uint32_t relisten_pending;
	uint32_t relisten_last;
	uint32_t disconnect_fail;
	uint32_t unaccept_fail;
	/* The second change of issue #202: sends retried instead of closing the
	   connection, why connections were closed, and the passive open. */
	uint32_t send_retried;           /* sends retried with the same bytes        */
	uint32_t stall_max;              /* longest wait for a send to go through, ms */
	uint32_t close_tcp;              /* TCP ended the connection                 */
	uint32_t close_stall;            /* no send went through for 10 s            */
	uint32_t accept_armed;           /* accept armed (once per connection)       */
	uint32_t accept_fail;            /* the arm answered anything else           */
	uint32_t accept_last;            /* its last code                            */
	uint32_t accept_rearm;           /* the arm fell back (reset) and was redone */
	uint32_t relisten_pending_ok;    /* NX_CONNECTION_PENDING taken as success   */
} mstats;

/* Send results inside one connection (issue #202). */
enum mjpeg_tx {
	MJPEG_TX_OK = 0,
	MJPEG_TX_STALLED,       /* nothing went through for MJPEG_STALL_TICKS      */
	MJPEG_TX_GONE,          /* TCP says the connection is gone                 */
	MJPEG_TX_STOP,          /* `net mjpeg stop`                                */
};

/* The send stall clock (issue #202): set at the start of the first failing
   send since the last one that went through.  Server thread only. */
static int   stall_on;
static ULONG stall_t0;

/* The connection being served (issue #202).  Reset at every accept and written
   only by the server thread; outside mstats so that eth_open()'s reset, which
   runs on another thread, does not race them.  conn_pkts / conn_bytes count the
   packets nx_tcp_socket_send() took (HTTP header and part boundaries
   included). */
static volatile uint32_t conn_frames;
static volatile uint32_t conn_pkts;
static volatile uint32_t conn_bytes;

/* ---- the camera frame-pipeline sink (synchronous copy) -------------------- */

static int  eth_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h);
static int  eth_consume(void *ctx, const struct frame_desc *f);
static void eth_close(void *ctx);

static struct frame_sink eth_sink = {
	.name    = "mjpeg",
	.policy  = FRAME_POLICY_DROP,
	.open    = eth_open,
	.consume = eth_consume,
	.close   = eth_close,
};

static int eth_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx; (void)w; (void)h;

	if (fmt != FRAME_FMT_JPEG)
		return -1;                  /* MJPEG only carries JPEG                  */
	/* [!] NO LIFECYCLE TEST HERE, deliberately (issue #72).  Refusing an attach
	   while mjpeg_own is not STARTING/RUNNING looks like the fail-closed thing
	   to do, and it is the wrong scope: a rejected open() makes
	   cam_subs_attach_all() unwind the WHOLE pass, so it fails the base start --
	   and the overrun re-arm, which runs on the producer thread.
	   What it would protect against does not exist either, and for a reason
	   that holds for EVERY attach path rather than a list of them:
	   frame_pipeline_attach() calls open() BEFORE linking the sink into the
	   pipeline's list, so at this point the producer cannot select this sink and
	   the reset below cannot race an eth_consume().  Ownership here is the
	   registry, not a lifecycle flag read from inside a callback.
	   Fresh session reset on attach -- the single reset point.  A normal stop's
	   detach() also calls close() (sets producer_dead), so clearing it here on the
	   next attach prevents a stale producer_dead from instantly ending the new
	   session.  Also clears the handoff flags and drains the frame signal. */
	producer_dead    = 0;
	client_connected = 0;
	buf_busy         = 0;
	buf_len          = 0;
	memset(&mstats, 0, sizeof mstats);
	while (tx_semaphore_get(&frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	return 0;
}

/* [!] PUT LAST.  camera_frame_put() must stay the LAST STATEMENT on every path
   out of this function, as in every consume() on this board (issue #72).  Once
   the pin reaches zero this callback touches nothing the owner owns, which is
   what lets a teardown settle for ONE count.  This sink is where the rule was
   actually being broken: buf_busy and the semaphore post used to happen AFTER
   the put, so a drain watching the pin could see zero while the handoff to the
   HTTP thread was still being made.

   Safe to hold the pin across the handoff: the post is non-blocking
   (tx_semaphore_put), the HTTP thread runs at prio 14 below this producer at 10
   so it cannot preempt us before the put, and what it reads is mjpeg_buf -- a
   copy -- never the ring slot. */
static int eth_consume(void *ctx, const struct frame_desc *f)
{
	(void)ctx;

	/* Producer-thread context, lock-free, slot pre-pinned.  Always put exactly
	   once (synchronous): copy when we can, otherwise drop -- never hold the pin
	   across the network send. */
	if (!mjpeg_run || !client_connected) {
		camera_frame_put(&eth_sink, f);          /* no client -> drop           */
		return 0;
	}
	if (buf_busy) {
		mstats.drop_busy++;                      /* HTTP still sending          */
		camera_frame_put(&eth_sink, f);
		return 0;
	}
	if (f->bytes > MJPEG_BUF_BYTES) {
		mstats.drop_oversized++;                 /* should not happen (= budget) */
		camera_frame_put(&eth_sink, f);
		return 0;
	}
	memcpy(mjpeg_buf, f->data, f->bytes);
	buf_len = f->bytes;
	buf_busy = 1;                                /* hand off to the HTTP thread  */
	(void)tx_semaphore_put(&frame_sem);
	camera_frame_put(&eth_sink, f);              /* LAST -- see the rule above   */
	return 0;
}

static void eth_close(void *ctx)
{
	(void)ctx;
	producer_dead = 1;              /* producer async teardown (e.g. DCMI overrun) */
}

/* ---- TCP send helpers (HTTP-thread context) ------------------------------- */

/* Count and log a failed nx_tcp_socket_send() (issue #202).  Only what is
   seen: the code, what this connection had sent so far, and the socket's own
   view at that moment (state, queued packets, the peer's window). */
static void mjpeg_send_failed(UINT sr)
{
	ULONG state = 0, txq = 0, txwin = 0, rxwin = 0, rexmit = 0, sent = 0;

	mstats.send_err++;
	if (sr == NX_WINDOW_OVERFLOW)
		mstats.send_window++;
	else if (sr == NX_TX_QUEUE_DEPTH)
		mstats.send_queue++;
	else if (sr == NX_NOT_CONNECTED)
		mstats.send_notconn++;
	else if (sr == NX_NO_PACKET)
		mstats.send_nopkt++;
	else
		mstats.send_other++;
	mstats.send_last   = (uint32_t)sr;
	mstats.fail_pkts   = conn_pkts;
	mstats.fail_bytes  = conn_bytes;
	mstats.fail_frames = conn_frames;

	/* The three answers that mean "not now" are retried (mjpeg_send()). */
	if (sr == NX_TX_QUEUE_DEPTH || sr == NX_WINDOW_OVERFLOW || sr == NX_NO_PACKET)
		LOG_WRN("send retried (0x%02x) after %lu pkts / %lu B, %lu frames",
		        (unsigned)sr, (unsigned long)conn_pkts,
		        (unsigned long)conn_bytes, (unsigned long)conn_frames);
	else
		LOG_WRN("send failed (0x%02x) after %lu pkts / %lu B, %lu frames",
		        (unsigned)sr, (unsigned long)conn_pkts,
		        (unsigned long)conn_bytes, (unsigned long)conn_frames);
	/* NetX zeroes the socket's packet counters at accept, so sent / rexmit
	   are this connection's. */
	if (nx_tcp_socket_info_get(&sock, &sent, NX_NULL, NX_NULL, NX_NULL,
	                           &rexmit, NX_NULL, NX_NULL, &state, &txq,
	                           &txwin, &rxwin) == NX_SUCCESS)
		LOG_WRN("socket: state %lu, txq %lu, txwin %lu, rxwin %lu, "
		        "sent %lu, rexmit %lu", (unsigned long)state,
		        (unsigned long)txq, (unsigned long)txwin,
		        (unsigned long)rxwin, (unsigned long)sent,
		        (unsigned long)rexmit);
}

/* A send that did not go through but left the connection up (issue #202): start
   the stall clock at the first one, and say whether it has run out.  @p t_try is
   when the failing attempt began, so the 1 s it waited counts. */
static int mjpeg_stalled(ULONG t_try)
{
	if (!stall_on) {
		stall_on = 1;
		stall_t0 = t_try;
	}
	return (ULONG)(tx_time_get() - stall_t0) >= (ULONG)MJPEG_STALL_TICKS;
}

/* One went through: stop the stall clock, keeping the longest wait. */
static void mjpeg_went_through(void)
{
	if (stall_on) {
		ULONG d = tx_time_get() - stall_t0;

		if (d > mstats.stall_max)
			mstats.stall_max = (uint32_t)d;   /* ticks = ms */
		stall_on = 0;
	}
}

/* Is the socket still in a state nx_tcp_socket_send() serves
   (nx_tcp_socket_send_internal.c:325-333)? */
static int mjpeg_sock_connected(void)
{
	UINT st = sock.nx_tcp_socket_state;

	return st == NX_TCP_ESTABLISHED || st == NX_TCP_CLOSE_WAIT;
}

/* Send @p len bytes in MSS-sized packets (NetX fragments a >MSS payload itself,
   double-spending the pool -- so we chunk).

   [!] A send that does not go through is RETRIED with the same bytes; nothing is
   skipped and the connection is not closed (issue #202).  Every part announces
   its Content-Length, so dropping the rest of a frame would hand a receiver that
   trusts it the next part's bytes as the end of this JPEG.  "Not now" is the
   queue staying full (NX_TX_QUEUE_DEPTH), the peer's window staying closed
   (NX_WINDOW_OVERFLOW) or the pool running dry.  Only TCP saying the connection
   is gone (MJPEG_TX_GONE) or MJPEG_STALL_TICKS without one send going through
   (MJPEG_TX_STALLED) closes it.

   On a failed nx_tcp_socket_send() the packet is ours again, but NetX may have
   sent its head already: a packet longer than the usable window is sent in
   pieces, and after each piece the original is trimmed -- prepend advanced and
   length reduced to what is still unsent (nx_tcp_socket_send_internal.c:972-
   1003) -- before the wait that can fail (:1012-1059, the codes set by
   nx_tcp_transmit_cleanup.c:185-197, or :1084-1104 without a wait).  So what
   went out is the chunk minus the packet's remaining length, and the retry
   packs only the rest, from our own buffer. */
static enum mjpeg_tx mjpeg_send(const uint8_t *data, ULONG len)
{
	NX_PACKET_POOL *pool = (NX_PACKET_POOL *)nx_net_pool();
	ULONG mss = 0, off = 0;
	UINT sr;

	if (pool == NULL)
		return MJPEG_TX_GONE;      /* the stack is gone                            */
	if (nx_tcp_socket_mss_get(&sock, &mss) != NX_SUCCESS || mss == 0
	    || mss > MJPEG_MSS_CAP)
		mss = MJPEG_MSS_CAP;

	while (off < len) {
		NX_PACKET *pkt;
		ULONG n = len - off;
		ULONG t_try, sent;

		/* Abort mid-frame on stop/disconnect so a slow peer cannot hold the frame
		   loop past nx_mjpeg_stop()'s bounded teardown wait (a full frame is up to
		   ~180 MSS chunks).  The partial frame breaks this multipart part, but the
		   connection is being torn down anyway.  mjpeg_run first: a stop clears
		   both. */
		if (!mjpeg_run)
			return MJPEG_TX_STOP;
		if (!client_connected)
			return MJPEG_TX_GONE;  /* the disconnect callback: TCP ended it        */
		if (n > mss)
			n = mss;
		t_try = tx_time_get();
		if (nx_packet_allocate(pool, &pkt, NX_TCP_PACKET, MJPEG_ALLOC_TICKS)
		    != NX_SUCCESS) {
			mstats.pool_fail++;
			goto retry;
		}
		if (nx_packet_data_append(pkt, (VOID *)(data + off), n, pool,
		                          MJPEG_ALLOC_TICKS) != NX_SUCCESS) {
			nx_packet_release(pkt);
			mstats.pool_fail++;
			goto retry;
		}
		sr = nx_tcp_socket_send(&sock, pkt, MJPEG_SEND_TICKS);
		if (sr == NX_SUCCESS) {
			mjpeg_went_through();
			conn_pkts++;
			conn_bytes += n;
			off += n;
			continue;
		}
		/* What NetX had already sent of this chunk (see above). */
		sent = (pkt->nx_packet_length < n) ? n - pkt->nx_packet_length : 0u;
		nx_packet_release(pkt);
		mjpeg_send_failed(sr);
		/* Gone: NX_NOT_CONNECTED, or any other answer from a socket that is
		   no longer connected.  An unknown answer from a connected socket is
		   retried like "not now", under the same 10 s rule. */
		if (sr == NX_NOT_CONNECTED || !mjpeg_sock_connected())
			return MJPEG_TX_GONE;
		if (sent != 0u) {
			mjpeg_went_through();
			conn_bytes += sent;
			off += sent;
		}
retry:
		mstats.send_retried++;
		if (mjpeg_stalled(t_try))
			return MJPEG_TX_STALLED;
	}
	return MJPEG_TX_OK;
}

/* Send one multipart part from mjpeg_buf.  Each mjpeg_send() either delivers
   all its bytes or ends the connection, so a part is never cut short. */
static enum mjpeg_tx mjpeg_send_frame(void)
{
	char hdr[96];
	ULONG n = buf_len;                 /* stable while buf_busy (consume waits)   */
	enum mjpeg_tx r;
	int hl = snprintf(hdr, sizeof hdr,
	                  "--" MJPEG_BOUNDARY "\r\n"
	                  "Content-Type: image/jpeg\r\n"
	                  "Content-Length: %lu\r\n\r\n", (unsigned long)n);

	if (hl < 0 || (size_t)hl >= sizeof hdr)
		return MJPEG_TX_GONE;      /* cannot happen: n is at most 6 digits         */
	r = mjpeg_send((const uint8_t *)hdr, (ULONG)hl);
	if (r != MJPEG_TX_OK)
		return r;
	r = mjpeg_send(mjpeg_buf, n);
	if (r != MJPEG_TX_OK)
		return r;
	r = mjpeg_send((const uint8_t *)"\r\n", 2u);
	if (r != MJPEG_TX_OK)
		return r;
	mstats.sent_frames++;
	mstats.sent_bytes += n;
	conn_frames++;
	return MJPEG_TX_OK;
}

/* ---- HTTP server thread --------------------------------------------------- */

static void mjpeg_disconnect_cb(NX_TCP_SOCKET *s)
{
	(void)s;
	client_connected = 0;          /* serve loop ends; must NOT touch buf_busy   */
}

/* Count why a connection ended (issue #202).  A stop or the producer going away
   is not the connection's doing and is not counted. */
static void mjpeg_count_close(enum mjpeg_tx r)
{
	if (r == MJPEG_TX_GONE) {
		mstats.close_tcp++;
	} else if (r == MJPEG_TX_STALLED) {
		mstats.close_stall++;
		LOG_WRN("no send went through for %u s -- closing",
		        (unsigned)(MJPEG_STALL_TICKS / NX_IP_PERIODIC_RATE));
	}
}

/* Serve one accepted client: skip its GET, send the multipart header, then push
   frames until disconnect / stop / producer death / a 10 s send stall.  While a
   send is retried the producer's frames fall into drop_busy. */
static void mjpeg_serve(void)
{
	NX_PACKET *req;
	enum mjpeg_tx r;

	/* Open the output gate BEFORE sending: the per-chunk send abort (mjpeg_send)
	   keys off client_connected, and the producer's consume() begins staging
	   frames into mjpeg_buf from here.  Start with an empty handoff + no stale
	   frame signal.
	   [!] And BEFORE waiting for the request: the disconnect callback clears
	   client_connected, and one that fires during that wait (a FIN or RST right
	   after the handshake) would be undone by setting it afterwards (issue
	   #202). */
	buf_busy = 0;
	while (tx_semaphore_get(&frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	stall_on = 0;
	client_connected = 1;

	/* Skip the HTTP request (one packet, best-effort -- path is ignored). */
	if (nx_tcp_socket_receive(&sock, &req, MJPEG_POLL_TICKS) == NX_SUCCESS)
		nx_packet_release(req);

	r = mjpeg_send((const uint8_t *)http_header, sizeof http_header - 1u);
	if (r != MJPEG_TX_OK) {
		mjpeg_count_close(r);
		client_connected = 0;
		return;
	}

	for (;;) {
		(void)tx_semaphore_get(&frame_sem, MJPEG_POLL_TICKS);
		if (!mjpeg_run || !client_connected || producer_dead) {
			/* The disconnect callback cleared client_connected: TCP ended it
			   (FIN or RST from the peer, or NetX's own retransmit giving up --
			   nx_tcp_socket_connection_reset.c:131-142). */
			if (mjpeg_run && !client_connected)
				mjpeg_count_close(MJPEG_TX_GONE);
			buf_busy = 0;          /* clear the unsent handoff before leaving     */
			break;
		}
		if (buf_busy) {
			r = mjpeg_send_frame();

			buf_busy = 0;          /* HTTP thread is the SOLE clearer (all exits) */
			if (r != MJPEG_TX_OK) {
				mjpeg_count_close(r);
				break;
			}
		}
	}
	client_connected = 0;
}

static int mjpeg_socket_setup(NX_IP *ip)
{
	if (nx_tcp_socket_create(ip, &sock, "net-mjpeg", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
	                         NX_IP_TIME_TO_LIVE, NX_MJPEG_WINDOW, NX_NULL,
	                         mjpeg_disconnect_cb) != NX_SUCCESS) {
		LOG_ERR("socket create failed");
		return -1;
	}
	/* Cap the TX queue so a fast stream cannot drain eth_pool away from RX/shell;
	   retransmit from 300 ms, doubling, 4 retries (see MJPEG_RTO_TICKS). */
	nx_tcp_socket_transmit_configure(&sock, NX_MJPEG_TX_QUEUE, MJPEG_RTO_TICKS,
	                                 MJPEG_RTO_RETRIES, MJPEG_RTO_SHIFT);
	if (nx_tcp_server_socket_listen(ip, NX_MJPEG_PORT, &sock, 1, NX_NULL)
	    != NX_SUCCESS) {
		LOG_ERR("listen :%u failed", (unsigned)NX_MJPEG_PORT);
		nx_tcp_socket_delete(&sock);
		return -1;
	}
	return 0;
}

static enum cam_drain_step mjpeg_teardown(int *parked);   /* below */

/* Commit the lifecycle FROM THE SERVER THREAD, for the exits that no
   nx_mjpeg_start() / nx_mjpeg_stop() is waiting to commit (issue #72): the base
   cascade auto-stop, and a socket re-setup that failed after the stream was
   already up.  Both leave the thread parked with nobody to answer for it, and a
   lifecycle left at RUNNING would refuse the next `net mjpeg start` with
   "already running" until the user typed a stop nobody needed.

   The take is what makes this safe to call from here: a start unwind (STARTING)
   or a stop (DRAINING) answers HELD, so the caller that IS waiting commits its
   own result and this does nothing.  Call it only when mjpeg_run is already 0 --
   that is what separates a stop from a relisten failure, which passes through
   the same cleanup on its way to rebuilding the socket and carrying on. */
static void mjpeg_thread_commit(void)
{
	enum cam_own_stop act = cam_own_stop_take(&mjpeg_own);
	enum cam_drain_step step;
	int parked;

	if (act != CAM_OWN_STOP_DRAIN && act != CAM_OWN_STOP_RETRY)
		return;
	/* mjpeg_active is already 0 (we are the thread), so the wait inside returns
	   at once with parked; the unsubscribe is a no-op when the base cascade has
	   already deregistered this oneshot sink, and the drain is what matters. */
	step = mjpeg_teardown(&parked);
	cam_own_drain_finish(&mjpeg_own, step, parked);
}

/* End the connection with a 1 s disconnect, but only in the states
   nx_tcp_socket_disconnect() serves (nx_tcp_socket_disconnect.c:107-117:
   ESTABLISHED, SYN_SENT, SYN_RECEIVED, CLOSE_WAIT).  In any other state the
   connection is already over -- typically the peer closed first and TCP has
   moved on, or a reset put the socket back to LISTEN -- and the call would only
   answer NX_NOT_CONNECTED (0x38), a warning for a normal goodbye (issue #202).
   A 1 s wait that runs out (0x41) is still counted and logged. */
static void mjpeg_disconnect(void)
{
	UINT st = sock.nx_tcp_socket_state;
	UINT r;

	if (st != NX_TCP_ESTABLISHED && st != NX_TCP_SYN_SENT
	    && st != NX_TCP_SYN_RECEIVED && st != NX_TCP_CLOSE_WAIT)
		return;
	r = nx_tcp_socket_disconnect(&sock, NX_IP_PERIODIC_RATE);
	if (r != NX_SUCCESS) {
		mstats.disconnect_fail++;
		LOG_WRN("disconnect failed (0x%02x)", (unsigned)r);
	}
}

/* Arm the passive open.  CALL EXACTLY ONCE PER CONNECTION (issue #202, as
   owhinata/wio-lite-ai#23 found for the wio telnet and echo servers).

   [!] Re-entering nx_tcp_server_socket_accept() with a timeout, to stay
   responsive to a stop, is what this file used to do and it is NOT legal.
   Every entry from LISTEN draws a new initial sequence number and, when a SYN
   has already bound the socket, sends a new SYN-ACK
   (nx_tcp_server_socket_accept.c:102-125).  A 500 ms slice that ran out in the
   middle of a handshake therefore answered the peer's ACK with the wrong
   numbers: the peer reset, and the reset left the socket in LISTEN, still in
   the port table, with the peer's address cleared
   (nx_tcp_socket_connection_reset.c:87,
   nx_tcp_socket_block_cleanup.c:74-84 and :125).
   From then on every accept on it allocated a SYN-ACK in
   _nx_tcp_packet_send_control() (nx_tcp_packet_send_control.c:152) that no
   branch sends or frees, since the address family is 0 (:328-335): one packet
   leaked per slice, 36 of the pool's 39 on hardware, after which no client
   could connect.  So arm once with NX_NO_WAIT and wait with
   nx_tcp_socket_state_wait(), which may be re-entered freely; a socket that
   falls back to LISTEN is unaccepted and relistened before it is armed again,
   so accept never sees a bound LISTEN socket without a peer. */
static int mjpeg_arm_accept(void)
{
	UINT s = nx_tcp_server_socket_accept(&sock, NX_NO_WAIT);

	/* NX_IN_PROGRESS is the expected answer with NX_NO_WAIT; NX_SUCCESS means a
	   client had already completed the handshake. */
	if (s == NX_SUCCESS || s == NX_IN_PROGRESS) {
		mstats.accept_armed++;
		return 0;
	}
	mstats.accept_fail++;
	mstats.accept_last = (uint32_t)s;
	LOG_ERR("accept arm failed (0x%02x)", (unsigned)s);
	return -1;
}

/* Put the unaccepted socket back on the listen slot.  Returns 0, or -1 when
   the socket must be re-created.

   NX_CONNECTION_PENDING is a success: a SYN was already queued, and relisten
   has bound the socket to it and left it in LISTEN for the next accept, which
   answers with the SYN-ACK (nx_tcp_server_socket_relisten.c:60-61 and
   :343-396, "this is also a successful status").  It used to be handled as a
   failure, which threw away the second connection a browser opens and rebuilt
   the socket (issue #202). */
static int mjpeg_relisten(NX_IP *ip)
{
	UINT r = nx_tcp_server_socket_relisten(ip, NX_MJPEG_PORT, &sock);

	if (r == NX_SUCCESS)
		return 0;
	if (r == NX_CONNECTION_PENDING) {
		mstats.relisten_pending_ok++;
		LOG_INF("relisten: a connection was already queued (0x%02x)",
		        (unsigned)r);
		return 0;
	}
	mstats.relisten_fail++;
	mstats.relisten_last = (uint32_t)r;
	LOG_ERR("relisten failed (0x%02x)", (unsigned)r);
	return -1;
}

static void mjpeg_entry(ULONG arg)
{
	NX_IP *ip = (NX_IP *)nx_net_ip();
	int armed = 0;             /* accept armed on this socket and not undone       */
	int resetup;               /* relisten / arm failed: re-create the socket      */

	(void)arg;
	for (;;) {
		/* Parked until started (short sleep -> a re-start listens promptly). */
		while (!mjpeg_run)
			tx_thread_sleep(20);

		if (ip == NULL)
			ip = (NX_IP *)nx_net_ip();
		if (ip == NULL || mjpeg_socket_setup(ip) != 0) {
			mjpeg_run = 0;
			mjpeg_active = 0;
			/* During a start, its !listening path unwinds this
			   (owhinata/stm32f746g-disco#101) and the commit below sees HELD.
			   After a RELISTEN failure there is no start waiting, so this is
			   the only thing that hands the sink back and retires the
			   lifecycle (issue #72). */
			mjpeg_thread_commit();
			continue;
		}
		mjpeg_listening = 1;       /* nx_mjpeg_start() waits for this              */
		armed = 0;                 /* a new socket: listening, not yet armed       */
		/* A new socket has no connection yet: `this conn` must not show the
		   last one of the previous stream (issue #202). */
		conn_frames = 0;
		conn_pkts   = 0;
		conn_bytes  = 0;
		resetup = 0;
		LOG_INF("listening on :%u (http mjpeg)", (unsigned)NX_MJPEG_PORT);

		/* Serve until an explicit `net mjpeg stop` (mjpeg_run=0) or a non-recover
		   base teardown.  A transient DCMI overrun (base auto-recovering,
		   owhinata/stm32f746g-disco#100) pauses serving and resumes without dropping the listen socket;
		   camera_subscribed() is the single source of truth that tells "released
		   for good" from "paused for recovery" (owhinata/stm32f746g-disco#101, avoids the stale-close
		   race).  A relisten failure breaks out to a full socket re-setup. */
		while (mjpeg_run) {
			/* Accept + serve while the base is delivering (producer_dead=0).
			   The arm survives a producer pause: `armed` lives outside this loop
			   and only a new socket, a finished connection or a fallen arm clear
			   it -- re-arming an armed socket is exactly the call that must not
			   be repeated (see mjpeg_arm_accept()). */
			while (mjpeg_run && !producer_dead) {
				UINT r, st;

				if (!armed) {
					if (mjpeg_arm_accept() != 0) {
						resetup = 1;
						break; /* -> re-setup the socket (below)         */
					}
					armed = 1;
				}
				/* Poll, do not re-arm.  state_wait touches nothing
				   (nx_tcp_socket_state_wait.c:103-118). */
				if (nx_tcp_socket_state_wait(&sock, NX_TCP_ESTABLISHED,
				                             MJPEG_ACCEPT_TICKS) != NX_SUCCESS) {
					st = sock.nx_tcp_socket_state;
					/* ESTABLISHED: completed between the poll and this read --
					   the next poll returns at once.  SYN_RECEIVED: armed and
					   waiting (with or without a handshake under way). */
					if (st == NX_TCP_ESTABLISHED || st == NX_TCP_SYN_RECEIVED)
						continue;
					/* The arm is gone: LISTEN after a reset or a handshake
					   NetX gave up on (_nx_tcp_socket_block_cleanup() puts a
					   server socket back to LISTEN still bound, with the
					   peer's address cleared), or a connection that came and
					   went between two polls.  Undo and arm again. */
					mstats.accept_rearm++;
					LOG_WRN("socket left the passive open (state %u) -- "
					        "re-arming", (unsigned)st);
					mjpeg_disconnect();
					armed = 0;
					r = nx_tcp_server_socket_unaccept(&sock);
					if (r != NX_SUCCESS) {
						mstats.unaccept_fail++;
						LOG_WRN("unaccept failed (0x%02x)", (unsigned)r);
					}
					if (mjpeg_relisten(ip) != 0) {
						resetup = 1;
						break; /* -> re-setup the socket (below)         */
					}
					continue;      /* arms at the top                        */
				}
				mstats.conns++;
				conn_frames = 0;
				conn_pkts   = 0;
				conn_bytes  = 0;
				LOG_INF("client connected");
				mjpeg_serve();
				/* issue #202: this connection's frames, not the total. */
				LOG_INF("client disconnected (%lu frames)",
				        (unsigned long)conn_frames);
				mjpeg_disconnect();
				armed = 0;
				r = nx_tcp_server_socket_unaccept(&sock);
				if (r != NX_SUCCESS) {
					mstats.unaccept_fail++;
					LOG_WRN("unaccept failed (0x%02x)", (unsigned)r);
				}
				if (mjpeg_relisten(ip) != 0) {
					resetup = 1;
					break;     /* -> re-setup the socket (below)         */
				}
			}
			if (!mjpeg_run)
				break;             /* explicit stop                          */
			/* Relisten / arm failed -> full socket re-setup.  Tested apart from
			   producer_dead: a failure that coincides with a producer pause
			   would otherwise resume on a socket that is off the listen slot. */
			if (resetup || !producer_dead)
				break;

			/* producer_dead: the base tore this sink down.  Released (a
			   non-recover base stop) -> fully stop; still enabled (an overrun
			   recovery is in flight) -> pause for the re-open (eth_open clears
			   producer_dead) or a recovery giveup (camera_subscribed -> 0). */
			if (!camera_subscribed(&eth_sink)) {
				LOG_WRN("camera base stopped -- auto-stopping mjpeg");
				mjpeg_run = 0;
				break;
			}
			LOG_INF("camera base overrun -- mjpeg paused for recovery");
			while (mjpeg_run && producer_dead
			       && camera_subscribed(&eth_sink))
				tx_thread_sleep(20);
			if (mjpeg_run && !producer_dead)
				LOG_INF("camera base recovered -- mjpeg resumed");
			/* else: stop (mjpeg_run=0) or giveup (camera_subscribed=0,
			   producer_dead still 1) -> the outer while re-checks and stops. */
		}

		/* Stopped: tear the socket down and park.  An armed socket that never
		   saw a client sits in SYN_RECEIVED, which unaccept refuses
		   (nx_tcp_server_socket_unaccept.c:107-116); disconnect puts a server
		   socket there back to LISTEN (nx_tcp_socket_disconnect.c:147-202).  It
		   sends a FIN only when the socket has a peer interface, which the
		   listen / relisten that preceded the arm cleared unless a SYN bound
		   it -- and then the peer is real.  A connection that completed just
		   as the stop came (the poll saw ESTABLISHED and the loop then ended)
		   is reset the same way (NX_NO_WAIT: RST, back to LISTEN,
		   nx_tcp_socket_disconnect.c:206-228). */
		mjpeg_listening = 0;
		{
			UINT st = sock.nx_tcp_socket_state;

			if (st == NX_TCP_SYN_RECEIVED || st == NX_TCP_ESTABLISHED
			    || st == NX_TCP_CLOSE_WAIT)
				(void)nx_tcp_socket_disconnect(&sock, NX_NO_WAIT);
		}
		armed = 0;
		nx_tcp_server_socket_unaccept(&sock);
		nx_tcp_server_socket_unlisten(ip, NX_MJPEG_PORT);
		nx_tcp_socket_delete(&sock);
		mjpeg_active = 0;
		/* [!] Commit the lifecycle from here, but ONLY on a real stop (issue
		   #72).  THREE things reach this cleanup and only two of them are stops:
		     - an explicit nx_mjpeg_stop() is waiting.  It owns CAM_OWN_DRAINING
		       already, so the take below answers HELD and it commits its own
		       result -- including the case where it gave up on us and returned
		       -2 after committing SETTLING, which this parking then finishes;
		     - WE STOPPED OURSELVES: the base released this oneshot sink (it is
		       detached AND deregistered by then, so the drain below is on a
		       detached sink and its zero is a decision) and the loop auto-
		       stopped.  No stop is coming at all, and a lifecycle left at
		       RUNNING would refuse the next `net mjpeg start` with "already
		       running" until the user typed a stop nobody needed;
		     - a RELISTEN FAILURE, which is NOT a stop.  It breaks out here with
		       mjpeg_run still 1 to rebuild the socket and CARRY ON SERVING.
		       Committing there would retire a stream that then keeps running --
		       lifecycle IDLE with the sink attached and a live server, and a
		       later `net mjpeg stop` answering "not running".  Its drain would
		       be meaningless as well, taken on a still-ATTACHED sink where zero
		       is a snapshot the producer can undo rather than a decision.
		   The gate is mjpeg_run and NOT !mjpeg_active, because the relisten path
		   clears mjpeg_active on its way through here too. */
		if (!mjpeg_run)
			mjpeg_thread_commit();
		LOG_INF("stopped");
	}
}

/* ---- public API ----------------------------------------------------------- */

/* Stop gating, detach the sink, wait for the producer to hand its frame back and
   for the server thread to park.  Shared by nx_mjpeg_stop() and by the two start
   unwinds, which detach the same sink and owe it the same wait.

   [!] The CALLER must already hold the lifecycle (CAM_OWN_STARTING for an
   unwind, CAM_OWN_DRAINING for a stop) -- entered BEFORE the unsubscribe below,
   so nothing can re-attach the sink and reset the pin count this watches. */
static enum cam_drain_step mjpeg_teardown(int *parked)
{
	enum cam_drain_step step;
	ULONG start;

	/* Order (D3a): stop output gating, then stop, then detach the sink.
	   owhinata/stm32f746g-disco#101: unsubscribe DETACHES ONLY (the base keeps
	   running for other subscribers); a base cascade stop is a separate
	   `camera stream stop`.  Idempotent if the base already released this
	   oneshot sink. */
	client_connected = 0;
	mjpeg_run = 0;
	if (mjpeg_created)
		(void)tx_semaphore_put(&frame_sem);   /* wake the serve loop if waiting */
	camera_unsubscribe(&eth_sink);            /* detach; base keeps running     */

	/* The sink first: a publish can have been in flight across the unlink, and
	   that consume() keeps memcpy'ing into mjpeg_buf until it puts its pin. */
	step = camera_sink_drain(&eth_sink, MJPEG_DRAIN_TICKS);

	/* Then the server thread, which owns the socket teardown. */
	start = tx_time_get();
	while (mjpeg_active && (tx_time_get() - start) < (ULONG)MJPEG_SETTLE_TICKS)
		tx_thread_sleep(10);
	*parked = !mjpeg_active;
	return step;
}

/* Commit a teardown's result, closing the window where the server thread parks
   between the last poll and the commit (it would find DRAINING and leave it
   alone, and nobody else would ever clear the SETTLING just written). */
static void mjpeg_commit(enum cam_drain_step step, int parked)
{
	cam_own_drain_finish(&mjpeg_own, step, parked);
	if (!parked && !mjpeg_active)
		cam_own_settle(&mjpeg_own);
}

int nx_mjpeg_start(void)
{
	struct camera_mode m;
	enum cam_own_start act;
	int rc;

	if (!nx_net_is_up())
		return -1;
	/* Claim the lifecycle first (issue #72): everything below runs inside
	   CAM_OWN_STARTING, so a `net mjpeg stop` racing this is refused rather than
	   interleaved with it, and a start cannot walk into a teardown that has
	   detached the sink and is still watching its pins. */
	act = cam_own_start_take(&mjpeg_own);
	if (act != CAM_OWN_START_GO)
		return -2;                  /* running, or a teardown owns the sink        */
	/* The thread flags are a different question from the lifecycle: they say
	   whether the server thread from a previous stream is still winding down (a
	   stop that returned -2 leaves the lifecycle clear and these set).  The
	   registry test is the third: a sink still registered is somebody's
	   reservation, whatever the flags say (issue #63). */
	if (mjpeg_run || mjpeg_active || camera_subscribed(&eth_sink)) {
		cam_own_start_finish(&mjpeg_own, 0);
		return -2;                  /* still tearing down                          */
	}

	/* owhinata/stm32f746g-disco#101: MJPEG is a JPEG-class subscriber -- it no longer owns/starts
    the base.
	   The base must already be streaming JPEG (`camera format jpeg` + `camera stream
	   start`).  Report the precise reason so `net mjpeg start` never silently opens a
	   port that serves no frames (the owhinata/stm32f746g-disco#97 class of bug). */
	if (!camera_streaming()) {
		cam_own_start_finish(&mjpeg_own, 0);
		return NX_MJPEG_NO_CAPTURE;
	}
	if (camera_get_mode(&m) != 0 || !m.is_jpeg) {
		cam_own_start_finish(&mjpeg_own, 0);
		return NX_MJPEG_FMT_CLASH;  /* base is raster: mjpeg needs JPEG            */
	}

	/* Create the ThreadX objects exactly once (idempotent across start/stop).  The
	   thread parks on !mjpeg_run, so creating it before the sink is attached is
	   harmless and keeps a failed start from re-creating the same semaphore. */
	if (!mjpeg_created) {
		if (tx_semaphore_create(&frame_sem, "mjpeg", 0) != TX_SUCCESS) {
			cam_own_start_finish(&mjpeg_own, 0);
			return -3;
		}
		if (tx_thread_create(&mjpeg_thread, "net-mjpeg", mjpeg_entry, 0,
		                     mjpeg_stack, sizeof mjpeg_stack,
		                     MJPEG_PRIORITY, MJPEG_PRIORITY,
		                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
			tx_semaphore_delete(&frame_sem);
			cam_own_start_finish(&mjpeg_own, 0);
			return -3;
		}
		mjpeg_created = 1;
	}

	/* Attach as a non-persistent JPEG subscriber to the running base.  The pre-check
	   above is just for a precise error message: camera_subscribe_oneshot() is STRICT
	   (attaches to a live JPEG base under one cam_lock, or fails), so if the base
	   stopped since the pre-check this returns an error rather than a ghost idle
	   registration (owhinata/stm32f746g-disco#101).  A successful attach calls eth_open(), which
	   resets the session state + stats and clears producer_dead.  Only then do we claim the
	   lifecycle (mjpeg_run wakes the parked thread). */
	rc = camera_subscribe_oneshot(&eth_sink, CAM_FMT_JPEG);
	if (rc != 0) {
		cam_own_start_finish(&mjpeg_own, 0);
		return rc;
	}

	cur_res = m.res;
	mjpeg_listening = 0;
	mjpeg_active = 1;
	mjpeg_run = 1;

	/* Wait (bounded ~1 s) for the thread to actually listen, so success means the
	   port is open and the sink is attached. */
	for (int i = 0; i < 100 && mjpeg_active && !mjpeg_listening; i++)
		tx_thread_sleep(10);
	if (!mjpeg_listening) {
		int parked;
		/* The sink is attached, so this unwind is a teardown and owes it the
		   same drain a stop does (issue #72) -- the base is live, and a publish
		   may already be in flight into mjpeg_buf. */
		enum cam_drain_step step = mjpeg_teardown(&parked);

		mjpeg_commit(step, parked);
		/* [!] Report the PIN, not just the failed start.  Collapsing both into
		   -3 leaves the caller told "start failed" while the lifecycle sits in
		   PENDING and every retry answers "already running" -- the wedge looks
		   like a bug instead of the retryable state it is. */
		return (step == CAM_DRAIN_DONE) ? -3 : NX_MJPEG_PINS;
	}
	/* owhinata/stm32f746g-disco#101: the base could have been torn down (cascade released this
    oneshot sink)
	   while the thread was coming up -- don't report success for a stream that has
	   already stopped.  camera_subscribed() is the single source of truth. */
	if (!camera_subscribed(&eth_sink)) {
		int parked;
		enum cam_drain_step step = mjpeg_teardown(&parked);

		mjpeg_commit(step, parked);
		return (step == CAM_DRAIN_DONE) ? -3 : NX_MJPEG_PINS;
	}
	cam_own_start_finish(&mjpeg_own, 1);  /* STARTING -> RUNNING                   */
	return 0;
}

int nx_mjpeg_stop(void)
{
	enum cam_own_stop act = cam_own_stop_take(&mjpeg_own);
	enum cam_drain_step step;
	int parked;

	switch (act) {
	case CAM_OWN_STOP_IDLE:
		return -1;                        /* nothing of ours is up                 */
	case CAM_OWN_STOP_HELD:
		return NX_MJPEG_BUSY;             /* a start / another stop owns it        */
	case CAM_OWN_STOP_DRAIN:              /* the running case                      */
	case CAM_OWN_STOP_RETRY:              /* finish what an earlier stop could not */
		break;
	}

	step = mjpeg_teardown(&parked);
	mjpeg_commit(step, parked);
	if (step != CAM_DRAIN_DONE)
		return NX_MJPEG_PINS;             /* sink still pinned: reuse refused      */
	return parked ? 0 : -2;               /* -2: server thread still winding down  */
}

bool nx_mjpeg_stats_get(struct nx_mjpeg_stats *out)
{
	if (out != NULL) {
		out->running        = (mjpeg_run != 0);
		out->client         = (client_connected != 0);
		out->res            = cur_res;
		out->conns          = mstats.conns;
		out->sent_frames    = mstats.sent_frames;
		out->sent_bytes     = mstats.sent_bytes;
		out->drop_busy      = mstats.drop_busy;
		out->drop_oversized = mstats.drop_oversized;
		out->send_err       = mstats.send_err;
		out->pool_fail      = mstats.pool_fail;
		out->send_window      = mstats.send_window;
		out->send_queue       = mstats.send_queue;
		out->send_notconn     = mstats.send_notconn;
		out->send_nopkt       = mstats.send_nopkt;
		out->send_other       = mstats.send_other;
		out->send_last        = mstats.send_last;
		out->fail_pkts        = mstats.fail_pkts;
		out->fail_bytes       = mstats.fail_bytes;
		out->fail_frames      = mstats.fail_frames;
		out->relisten_fail    = mstats.relisten_fail;
		out->relisten_pending = mstats.relisten_pending;
		out->relisten_last    = mstats.relisten_last;
		out->disconnect_fail  = mstats.disconnect_fail;
		out->unaccept_fail    = mstats.unaccept_fail;
		out->conn_frames      = conn_frames;
		out->conn_pkts        = conn_pkts;
		out->conn_bytes       = conn_bytes;
		out->send_retried     = mstats.send_retried;
		out->stall_max        = mstats.stall_max;
		out->close_tcp        = mstats.close_tcp;
		out->close_stall      = mstats.close_stall;
		out->accept_armed     = mstats.accept_armed;
		out->accept_fail      = mstats.accept_fail;
		out->accept_last      = mstats.accept_last;
		out->accept_rearm     = mstats.accept_rearm;
		out->relisten_pending_ok = mstats.relisten_pending_ok;
	}
	return mjpeg_run != 0;
}
