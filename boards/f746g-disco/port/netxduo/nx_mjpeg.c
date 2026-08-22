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
#define MJPEG_ACCEPT_TICKS   500u        /* accept timeout -> stop latency         */
#define MJPEG_POLL_TICKS     200u        /* frame wait -> disconnect latency       */
#define MJPEG_ALLOC_TICKS    100u        /* packet alloc wait                      */
#define MJPEG_SEND_TICKS     NX_IP_PERIODIC_RATE  /* per-chunk send wait (1 s)     */
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
} mstats;

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

/* Send @p len bytes in MSS-sized packets (NetX fragments a >MSS payload itself,
   double-spending the pool -- so we chunk).  Returns 0, or -1 on pool/send fail. */
static int mjpeg_send(const uint8_t *data, ULONG len)
{
	NX_PACKET_POOL *pool = (NX_PACKET_POOL *)nx_net_pool();
	ULONG mss = 0, off = 0;

	if (pool == NULL)
		return -1;
	if (nx_tcp_socket_mss_get(&sock, &mss) != NX_SUCCESS || mss == 0
	    || mss > MJPEG_MSS_CAP)
		mss = MJPEG_MSS_CAP;

	while (off < len) {
		NX_PACKET *pkt;
		ULONG n = len - off;

		/* Abort mid-frame on stop/disconnect so a slow peer cannot hold the frame
		   loop past nx_mjpeg_stop()'s bounded teardown wait (a full frame is up to
		   ~180 MSS chunks).  The partial frame breaks this multipart part, but the
		   connection is being torn down anyway. */
		if (!mjpeg_run || !client_connected)
			return -1;
		if (n > mss)
			n = mss;
		if (nx_packet_allocate(pool, &pkt, NX_TCP_PACKET, MJPEG_ALLOC_TICKS)
		    != NX_SUCCESS) {
			mstats.pool_fail++;
			return -1;
		}
		if (nx_packet_data_append(pkt, (VOID *)(data + off), n, pool,
		                          MJPEG_ALLOC_TICKS) != NX_SUCCESS) {
			nx_packet_release(pkt);
			mstats.pool_fail++;
			return -1;
		}
		if (nx_tcp_socket_send(&sock, pkt, MJPEG_SEND_TICKS) != NX_SUCCESS) {
			nx_packet_release(pkt);
			mstats.send_err++;
			return -1;
		}
		off += n;
	}
	return 0;
}

/* Send one multipart part from mjpeg_buf.  Returns 0, or -1 (disconnect). */
static int mjpeg_send_frame(void)
{
	char hdr[96];
	ULONG n = buf_len;                 /* stable while buf_busy (consume waits)   */
	int hl = snprintf(hdr, sizeof hdr,
	                  "--" MJPEG_BOUNDARY "\r\n"
	                  "Content-Type: image/jpeg\r\n"
	                  "Content-Length: %lu\r\n\r\n", (unsigned long)n);

	if (hl < 0 || (size_t)hl >= sizeof hdr)
		return -1;
	if (mjpeg_send((const uint8_t *)hdr, (ULONG)hl) != 0)
		return -1;
	if (mjpeg_send(mjpeg_buf, n) != 0)
		return -1;
	if (mjpeg_send((const uint8_t *)"\r\n", 2u) != 0)
		return -1;
	mstats.sent_frames++;
	mstats.sent_bytes += n;
	return 0;
}

/* ---- HTTP server thread --------------------------------------------------- */

static void mjpeg_disconnect_cb(NX_TCP_SOCKET *s)
{
	(void)s;
	client_connected = 0;          /* serve loop ends; must NOT touch buf_busy   */
}

/* Serve one accepted client: skip its GET, send the multipart header, then push
   frames until disconnect / stop / producer death. */
static void mjpeg_serve(void)
{
	NX_PACKET *req;

	/* Skip the HTTP request (one packet, best-effort -- path is ignored). */
	if (nx_tcp_socket_receive(&sock, &req, MJPEG_POLL_TICKS) == NX_SUCCESS)
		nx_packet_release(req);

	/* Open the output gate BEFORE sending: the per-chunk send abort (mjpeg_send)
	   keys off client_connected, and the producer's consume() begins staging
	   frames into mjpeg_buf from here.  Start with an empty handoff + no stale
	   frame signal. */
	buf_busy = 0;
	while (tx_semaphore_get(&frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	client_connected = 1;

	if (mjpeg_send((const uint8_t *)http_header, sizeof http_header - 1u) != 0) {
		client_connected = 0;
		return;
	}

	for (;;) {
		(void)tx_semaphore_get(&frame_sem, MJPEG_POLL_TICKS);
		if (!mjpeg_run || !client_connected || producer_dead) {
			buf_busy = 0;          /* clear the unsent handoff before leaving     */
			break;
		}
		if (buf_busy) {
			int r = mjpeg_send_frame();

			buf_busy = 0;          /* HTTP thread is the SOLE clearer (all exits) */
			if (r != 0)
				break;             /* send error -> disconnect                    */
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
	   2 s retransmit, 10 retries, x2 backoff. */
	nx_tcp_socket_transmit_configure(&sock, NX_MJPEG_TX_QUEUE,
	                                 2u * NX_IP_PERIODIC_RATE, 10, 1);
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

static void mjpeg_entry(ULONG arg)
{
	NX_IP *ip = (NX_IP *)nx_net_ip();

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
		LOG_INF("listening on :%u (http mjpeg)", (unsigned)NX_MJPEG_PORT);

		/* Serve until an explicit `net mjpeg stop` (mjpeg_run=0) or a non-recover
		   base teardown.  A transient DCMI overrun (base auto-recovering,
		   owhinata/stm32f746g-disco#100) pauses serving and resumes without dropping the listen socket;
		   camera_subscribed() is the single source of truth that tells "released
		   for good" from "paused for recovery" (owhinata/stm32f746g-disco#101, avoids the stale-close
		   race).  A relisten failure breaks out to a full socket re-setup. */
		while (mjpeg_run) {
			/* Accept + serve while the base is delivering (producer_dead=0). */
			while (mjpeg_run && !producer_dead) {
				if (nx_tcp_server_socket_accept(&sock, MJPEG_ACCEPT_TICKS)
				    != NX_SUCCESS)
					continue;      /* timeout -> re-accept (re-checks run)    */
				mstats.conns++;
				LOG_INF("client connected");
				mjpeg_serve();
				LOG_INF("client disconnected (%lu frames)",
				        (unsigned long)mstats.sent_frames);
				nx_tcp_socket_disconnect(&sock, NX_IP_PERIODIC_RATE);
				nx_tcp_server_socket_unaccept(&sock);
				if (nx_tcp_server_socket_relisten(ip, NX_MJPEG_PORT, &sock)
				    != NX_SUCCESS) {
					LOG_ERR("relisten failed");
					break;     /* -> re-setup the socket (below)         */
				}
			}
			if (!mjpeg_run)
				break;             /* explicit stop                          */
			if (!producer_dead)
				break;             /* relisten failed -> full socket re-setup */

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

		/* Stopped: tear the socket down and park. */
		mjpeg_listening = 0;
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
	}
	return mjpeg_run != 0;
}
