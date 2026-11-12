/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the clean-room YMODEM-CRC receiver (owhinata/wio-lite-ai#19
 * M5, svc/ymodem.c :: ymodem_recv).  Pure: no HAL/ThreadX/shell.
 *
 * Two harnesses, because they prove different things:
 *
 *   1. DUPLEX LOOPBACK (pthread).  ymodem_send() runs in one thread and
 *      ymodem_recv() in another, wired together by two blocking byte FIFOs.  A
 *      "capture the sender's TX, then replay it into the receiver" scheme could
 *      NOT do this: the sender only advances when it sees the receiver's 'C' and
 *      per-block ACKs, so a canned replay never exercises the real handshake.
 *      This works because each side gets its OWN struct ym_xfer (its own mark
 *      and areas, see sender_xfer / recv_xfer below) and the two share no other
 *      mutable state.  The firmware defines one owner, so there a send and a
 *      receive refuse each other; the single-owner tests below check that.
 *      Asserts byte-exact delivery and YM_OK (i.e. the batch actually closed).
 *
 *   2. SCRIPTED TRANSCRIPTS (single-threaded).  Error paths are driven from a
 *      canned byte stream so each failure is deterministic: CRC damage -> NAK ->
 *      recovery, a duplicate block, an out-of-order block, a sink that refuses
 *      the file, a batch that never closes, and short-block trimming.
 *      The script mock answers a timeout_ms == 0 poll with YM_IO_TIMEOUT, which
 *      is what the wire really looks like: after a bad block the sender is
 *      waiting for our NAK, so nothing is buffered for recv_drain() to eat.
 */
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ymodem.h"

/* protocol bytes (mirrored for the test's own frame building / assertions) */
#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define CRC_C 0x43
#define CPMEOF 0x1A

/* ======================================================================== *
 *  1. duplex loopback over two blocking FIFOs
 * ======================================================================== */

#define CHAN_CAP 65536u

struct chan {
	uint8_t         buf[CHAN_CAP];
	size_t          head, tail;     /* head = read pos, tail = write pos */
	pthread_mutex_t mu;
	pthread_cond_t  cv;
};

static void chan_init(struct chan *c)
{
	c->head = c->tail = 0;
	pthread_mutex_init(&c->mu, NULL);
	pthread_cond_init(&c->cv, NULL);
}

static size_t chan_len(const struct chan *c) { return c->tail - c->head; }

static int chan_getc(void *ctx, unsigned timeout_ms)
{
	struct chan *c = ctx;
	int          b = YM_IO_TIMEOUT;

	pthread_mutex_lock(&c->mu);
	if (chan_len(c) == 0 && timeout_ms > 0) {
		struct timeval  now;
		struct timespec ts;

		gettimeofday(&now, NULL);
		ts.tv_sec  = now.tv_sec + (time_t)(timeout_ms / 1000u);
		ts.tv_nsec = now.tv_usec * 1000L + (long)(timeout_ms % 1000u) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		while (chan_len(c) == 0)
			if (pthread_cond_timedwait(&c->cv, &c->mu, &ts) != 0)
				break;
	}
	if (chan_len(c) > 0) {
		b = c->buf[c->head % CHAN_CAP];
		c->head++;
		pthread_cond_broadcast(&c->cv);
	}
	pthread_mutex_unlock(&c->mu);
	return b;
}

static int chan_put(void *ctx, const uint8_t *src, size_t len)
{
	struct chan *c = ctx;

	pthread_mutex_lock(&c->mu);
	for (size_t i = 0; i < len; i++) {
		while (chan_len(c) >= CHAN_CAP)
			pthread_cond_wait(&c->cv, &c->mu);   /* reader drains */
		c->buf[c->tail % CHAN_CAP] = src[i];
		c->tail++;
	}
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);
	return 0;
}

/* ---- pattern source / capturing sink -------------------------------------- */

/*
 * [!] POSITION-DEPENDENT, WITH A PERIOD LONGER THAN A BLOCK.  This used to be
 * `pos & 0xFF`, which repeats every 256 bytes -- so every 1024-byte block of
 * the transfer carried IDENTICAL content, and a receiver that delivered a
 * duplicate block and dropped the next one would produce the right length and
 * the right bytes.  A test whose data cannot tell one block from another cannot
 * see a protocol that loses track of which block it is on.
 */
static uint8_t pat_byte(uint32_t pos)
{
	return (uint8_t)((pos * 2654435761u) >> 24);
}

struct pat_src { uint32_t pos, size; };

static int pat_read(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got)
{
	struct pat_src *s = ctx;
	uint32_t        n = s->size - s->pos;

	if (n > want)
		n = want;
	for (uint32_t i = 0; i < n; i++)
		dst[i] = pat_byte(s->pos + i);
	s->pos += n;
	*got = n;
	return 0;
}

struct cap_sink {
	uint8_t *buf;
	uint32_t cap, len;
	uint32_t declared;
	char     name[64];
	int      begins, rejects;
};

static int cap_begin(void *ctx, const char *name, uint32_t size)
{
	struct cap_sink *s = ctx;

	s->begins++;
	if (s->rejects)
		return -1;
	snprintf(s->name, sizeof s->name, "%s", name);
	s->declared = size;
	if (size > s->cap)
		return -1;
	s->len = 0;
	return 0;
}

static int cap_write(void *ctx, const uint8_t *data, uint32_t len)
{
	struct cap_sink *s = ctx;

	assert(s->len + len <= s->cap);      /* receiver overran the declared size */
	memcpy(s->buf + s->len, data, len);
	s->len += len;
	return 0;
}

/*
 * The ym_io vtable carries a single ctx for both directions, so each endpoint
 * gets a tiny adapter holding its own rx/tx channel pair.
 */
struct endpoint { struct chan *rx, *tx; };

static int ep_getc(void *ctx, unsigned timeout_ms)
{
	return chan_getc(((struct endpoint *)ctx)->rx, timeout_ms);
}

static int ep_put(void *ctx, const uint8_t *buf, size_t len)
{
	return chan_put(((struct endpoint *)ctx)->tx, buf, len);
}

/*
 * The working areas a board would own (issue #230).  The loopback runs a send
 * and a receive AT THE SAME TIME, so it needs two owners -- one per side, each
 * with its own areas -- where a board defines exactly one.
 */
static struct ym_send_area s_send_a, r_send_a;
static struct ym_recv_area s_recv_a, r_recv_a;
static struct ym_xfer      sender_xfer = { .send = &s_send_a, .recv = &s_recv_a };
static struct ym_xfer      recv_xfer = { .send = &r_send_a, .recv = &r_recv_a };

struct send_arg {
	struct endpoint  ep;
	struct pat_src   src;
	const char      *name;
	enum ym_result   rc;
};

static void *run_sender(void *arg)
{
	struct send_arg *a = arg;
	struct ym_io     io = { &a->ep, ep_getc, ep_put };
	struct ym_source src = { &a->src, a->name, a->src.size, pat_read };

	a->rc = ymodem_send(&sender_xfer, &io, &src);
	return NULL;
}

/* One full send<->recv round trip of @size bytes; asserts byte-exact delivery. */
static void loopback(uint32_t size, const char *name)
{
	static struct chan a2b, b2a;    /* static: 64 KB each, off the thread stack */
	struct send_arg    sa;
	struct endpoint    rep;
	struct cap_sink    sink;
	struct ym_io       rio;
	struct ym_sink     vs = { &sink, cap_begin, cap_write };
	struct ym_recv_diag diag;
	pthread_t          th;
	enum ym_result     rrc;

	chan_init(&a2b);
	chan_init(&b2a);

	memset(&sa, 0, sizeof sa);
	sa.ep.rx = &b2a; sa.ep.tx = &a2b;
	sa.src.pos = 0; sa.src.size = size;
	sa.name = name;

	memset(&sink, 0, sizeof sink);
	sink.cap = size ? size : 1u;
	sink.buf = malloc(sink.cap);
	assert(sink.buf != NULL);

	rep.rx = &a2b; rep.tx = &b2a;
	rio.ctx = &rep; rio.getc = ep_getc; rio.put = ep_put;

	assert(pthread_create(&th, NULL, run_sender, &sa) == 0);
	rrc = ymodem_recv(&recv_xfer, &rio, &vs, &diag);
	pthread_join(th, NULL);

	assert(sa.rc == YM_OK);
	assert(rrc == YM_OK);                          /* batch actually closed */
	assert(diag.blocks >= 2u && diag.bad_crc == 0u && diag.bad_seq == 0u &&
	       diag.short_read == 0u && diag.first_kind == -1);
	assert(sink.begins == 1);
	assert(strcmp(sink.name, name) == 0);
	assert(sink.declared == size);
	assert(sink.len == size);
	for (uint32_t i = 0; i < size; i++)
		assert(sink.buf[i] == pat_byte(i));
	free(sink.buf);
}

/* ======================================================================== *
 *  2. scripted transcripts
 * ======================================================================== */

struct script {
	uint8_t *in;          /* bytes the "sender" will produce, in order */
	size_t   inlen, inpos;
	uint8_t  out[8192];   /* bytes the receiver emitted */
	size_t   outlen;
};

static int script_getc(void *ctx, unsigned timeout_ms)
{
	struct script *s = ctx;

	/* A zero timeout is recv_drain() asking "is anything already buffered?".
	 * On the wire the answer is no -- the sender is waiting on our ACK/NAK. */
	if (timeout_ms == 0)
		return YM_IO_TIMEOUT;
	if (s->inpos >= s->inlen)
		return YM_IO_TIMEOUT;
	return s->in[s->inpos++];
}

static int script_put(void *ctx, const uint8_t *buf, size_t len)
{
	struct script *s = ctx;

	assert(s->outlen + len <= sizeof s->out);
	memcpy(s->out + s->outlen, buf, len);
	s->outlen += len;
	return 0;
}

/* Append one framed block to @p dst. */
static size_t put_block(uint8_t *dst, size_t at, uint8_t kind, uint8_t seq,
                        const uint8_t *data, uint16_t dl, int corrupt_crc)
{
	uint16_t crc;

	dst[at++] = kind;
	dst[at++] = seq;
	dst[at++] = (uint8_t)~seq;
	memcpy(dst + at, data, dl);
	crc = ymodem_crc16(dst + at, dl);
	if (corrupt_crc)
		crc ^= 0xFFFFu;
	at += dl;
	dst[at++] = (uint8_t)(crc >> 8);
	dst[at++] = (uint8_t)(crc & 0xFFu);
	return at;
}

/* Build a 128-byte block-0 payload: "name\0size\0", zero padded. */
static void build_b0(uint8_t *p, const char *name, const char *size)
{
	size_t i = 0;

	memset(p, 0, 128);
	while (*name)
		p[i++] = (uint8_t)*name++;
	i++;
	while (*size)
		p[i++] = (uint8_t)*size++;
}

static int count_byte(const struct script *s, uint8_t b)
{
	int n = 0;

	for (size_t i = 0; i < s->outlen; i++)
		if (s->out[i] == b)
			n++;
	return n;
}

/* The record the last run_script() wrote: the caller's, not the protocol's. */
static struct ym_recv_diag last_diag;

static enum ym_result run_script(struct script *s, struct cap_sink *sink)
{
	struct ym_io   io = { s, script_getc, script_put };
	struct ym_sink vs = { sink, cap_begin, cap_write };

	/* Garbage first: ymodem_recv() must set the caller's record on entry
	 * (issue #230), so nothing of this survives into the counts. */
	memset(&last_diag, 0xA5, sizeof last_diag);
	return ymodem_recv(&recv_xfer, &io, &vs, &last_diag);
}

/* ======================================================================== *
 *  3. one transfer at a time (issue #230)
 * ======================================================================== *
 *
 * A board defines ONE struct ym_xfer and passes it to every transfer, so
 * these share one owner, one_xfer.  A second transfer is started from inside
 * the running one's io callback -- the same thread, so the refusal is
 * deterministic, and what it proves is the same as from another console:
 * while the mark is held, a send or a receive on that owner is refused with
 * YM_ERR_BUSY having touched nothing.  Checks count failures instead of
 * aborting, so one run lists every one that fails.
 */

static struct ym_send_area one_send_a;
static struct ym_recv_area one_recv_a;
static struct ym_xfer      one_xfer = { .send = &one_send_a, .recv = &one_recv_a };

static int one_failures;

static void one_check(const char *what, const char *detail, int ok)
{
	if (!ok) {
		printf("  FAIL %s: %s\n", what, detail);
		one_failures++;
	}
}

/* An io / sink / source that only counts: a refused transfer must leave all
 * of it at zero. */
struct probe { int getcs, puts, begins, writes, reads; };

static int probe_getc(void *ctx, unsigned timeout_ms)
{
	(void)timeout_ms;
	((struct probe *)ctx)->getcs++;
	return YM_IO_TIMEOUT;
}

static int probe_put(void *ctx, const uint8_t *buf, size_t len)
{
	(void)buf; (void)len;
	((struct probe *)ctx)->puts++;
	return 0;
}

static int probe_begin(void *ctx, const char *name, uint32_t size)
{
	(void)name; (void)size;
	((struct probe *)ctx)->begins++;
	return 0;
}

static int probe_write(void *ctx, const uint8_t *data, uint32_t len)
{
	(void)data; (void)len;
	((struct probe *)ctx)->writes++;
	return 0;
}

static int probe_read(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got)
{
	(void)dst; (void)want;
	((struct probe *)ctx)->reads++;
	*got = 0;
	return 0;
}

static int diag_is_init(const struct ym_recv_diag *d)
{
	struct ym_recv_diag z;

	memset(&z, 0x5A, sizeof z);
	ymodem_recv_diag_init(&z);
	return memcmp(&z, d, sizeof z) == 0;
}

/* A receive started on one_xfer now must be refused and touch nothing. */
static void refused_recv(const char *what)
{
	struct probe        p;
	struct ym_io        io = { &p, probe_getc, probe_put };
	struct ym_sink      sk = { &p, probe_begin, probe_write };
	struct ym_recv_diag d;
	enum ym_result      rc;

	memset(&p, 0, sizeof p);
	memset(&d, 0xA5, sizeof d);
	rc = ymodem_recv(&one_xfer, &io, &sk, &d);
	one_check(what, "a second receive is refused with YM_ERR_BUSY",
	          rc == YM_ERR_BUSY);
	one_check(what, "the refused receive reads and writes no byte on its io",
	          p.getcs == 0 && p.puts == 0);
	one_check(what, "the refused receive never calls its sink",
	          p.begins == 0 && p.writes == 0);
	one_check(what, "(f) the refused receive's record is the "
	          "nothing-received value", diag_is_init(&d));
}

/* A send started on one_xfer now must be refused and touch nothing. */
static void refused_send(const char *what)
{
	struct probe     p;
	struct ym_io     io = { &p, probe_getc, probe_put };
	struct ym_source src = { &p, "x.bin", 10u, probe_read };
	enum ym_result   rc;

	memset(&p, 0, sizeof p);
	rc = ymodem_send(&one_xfer, &io, &src);
	one_check(what, "a second send is refused with YM_ERR_BUSY",
	          rc == YM_ERR_BUSY);
	one_check(what, "the refused send reads and writes no byte on its io "
	          "(not even the CAN teardown)", p.getcs == 0 && p.puts == 0);
	one_check(what, "the refused send never reads its source", p.reads == 0);
}

/* ---- (a) (c) (e): refusals while a SEND runs ------------------------------ */

/* The sender's endpoint, which fires the refusals once, at its Nth read. */
struct hooked_ep {
	struct endpoint ep;
	int             calls, fire_at;
	void          (*fire)(void);
};

static int hooked_getc(void *ctx, unsigned timeout_ms)
{
	struct hooked_ep *h = ctx;

	if (++h->calls == h->fire_at)
		h->fire();
	return chan_getc(h->ep.rx, timeout_ms);
}

static int hooked_put(void *ctx, const uint8_t *buf, size_t len)
{
	return chan_put(((struct hooked_ep *)ctx)->ep.tx, buf, len);
}

static int fired;

static void fire_during_send(void)
{
	const char *what = "(a)(c)(e) while a send runs";

	fired++;
	one_check(what, "(e) the question says a start now would be refused",
	          ymodem_busy(&one_xfer) == 1);
	refused_recv("(a) a receive while a send runs");
	refused_send("(c) a second send while a send runs");
	one_check(what, "the refusals did not give the running send's mark away",
	          ymodem_busy(&one_xfer) == 1);
}

/* The PC side of the send: an ordinary receive on its own owner. */
struct pc_recv_arg {
	struct endpoint ep;
	struct cap_sink sink;
	enum ym_result  rc;
};

static void *run_pc_receiver(void *arg)
{
	struct pc_recv_arg *a = arg;
	struct ym_io        io = { &a->ep, ep_getc, ep_put };
	struct ym_sink      vs = { &a->sink, cap_begin, cap_write };
	struct ym_recv_diag d;

	a->rc = ymodem_recv(&recv_xfer, &io, &vs, &d);
	return NULL;
}

static void one_during_send(void)
{
	static struct chan        a2b, b2a;
	static struct pc_recv_arg pc;
	static uint8_t            buf[5000];
	struct hooked_ep          h;
	struct pat_src            ps = { 0u, sizeof buf };
	struct ym_io              io = { &h, hooked_getc, hooked_put };
	struct ym_source          src = { &ps, "send.bin", sizeof buf, pat_read };
	pthread_t                 th;
	enum ym_result            rc;
	int                       same = 1;

	chan_init(&a2b);
	chan_init(&b2a);
	memset(&pc, 0, sizeof pc);
	pc.ep.rx = &a2b; pc.ep.tx = &b2a;
	pc.sink.buf = buf; pc.sink.cap = sizeof buf;
	memset(&h, 0, sizeof h);
	h.ep.rx = &b2a; h.ep.tx = &a2b;
	h.fire_at = 3;              /* after block 0's ACK: mid-transfer */
	h.fire = fire_during_send;
	fired = 0;

	assert(pthread_create(&th, NULL, run_pc_receiver, &pc) == 0);
	rc = ymodem_send(&one_xfer, &io, &src);
	pthread_join(th, NULL);

	one_check("(a) while a send runs", "the refusals were made", fired == 1);
	for (uint32_t i = 0; i < sizeof buf && pc.sink.len == sizeof buf; i++)
		if (buf[i] != pat_byte(i))
			same = 0;
	one_check("(a) while a send runs", "the running send still succeeds, "
	          "byte for byte", rc == YM_OK && pc.rc == YM_OK &&
	          pc.sink.len == sizeof buf && same);
	one_check("(e) after the send", "the question says a start now may go",
	          ymodem_busy(&one_xfer) == 0);
}

/* ---- (b) (c) (e) (f): refusals while a RECEIVE runs ----------------------- */

struct hooked_script {
	struct script s;
	int           calls, fire_at;
	void        (*fire)(void);
};

static int hooked_script_getc(void *ctx, unsigned timeout_ms)
{
	struct hooked_script *h = ctx;

	if (++h->calls == h->fire_at)
		h->fire();
	return script_getc(&h->s, timeout_ms);
}

static int hooked_script_put(void *ctx, const uint8_t *buf, size_t len)
{
	return script_put(&((struct hooked_script *)ctx)->s, buf, len);
}

static struct ym_recv_diag one_running_diag;

static void fire_during_recv(void)
{
	const char         *what = "(b)(c)(e)(f) while a receive runs";
	struct ym_recv_diag before = one_running_diag;

	fired++;
	one_check(what, "fired mid-transfer (block 0 already counted)",
	          before.blocks == 1u);
	one_check(what, "(e) the question says a start now would be refused",
	          ymodem_busy(&one_xfer) == 1);
	refused_send("(b) a send while a receive runs");
	refused_recv("(c) a second receive while a receive runs");
	one_check(what, "the refusals did not give the running receive's mark "
	          "away", ymodem_busy(&one_xfer) == 1);
	one_check(what, "(f) the running receive's record is untouched",
	          memcmp(&before, &one_running_diag, sizeof before) == 0);
}

/* A good one-file batch of @p len (<= 1024) pattern bytes into @p in. */
static size_t one_good_batch(uint8_t *in, uint32_t len)
{
	uint8_t b0[128], blk[1024], nullb0[128];
	char    num[16];
	size_t  n = 0;

	snprintf(num, sizeof num, "%lu", (unsigned long)len);
	build_b0(b0, "one.bin", num);
	for (uint32_t i = 0; i < 1024u; i++)
		blk[i] = i < len ? pat_byte(i) : CPMEOF;
	memset(nullb0, 0, sizeof nullb0);
	n = put_block(in, n, SOH, 0, b0, 128, 0);
	n = put_block(in, n, STX, 1, blk, 1024, 0);
	in[n++] = EOT;
	n = put_block(in, n, SOH, 0, nullb0, 128, 0);
	return n;
}

static void one_during_recv(void)
{
	static uint8_t              in[4096];
	static uint8_t              buf[1024];
	static struct hooked_script h;
	struct cap_sink             sink;
	struct ym_io                io = { &h, hooked_script_getc, hooked_script_put };
	struct ym_sink              vs = { &sink, cap_begin, cap_write };
	enum ym_result              rc;
	int                         same = 1;

	memset(&h, 0, sizeof h);
	h.s.in = in;
	h.s.inlen = one_good_batch(in, 1000u);
	h.fire_at = 200;            /* in block 1's body, block 0 accepted */
	h.fire = fire_during_recv;
	memset(&sink, 0, sizeof sink);
	sink.buf = buf; sink.cap = sizeof buf;
	fired = 0;

	rc = ymodem_recv(&one_xfer, &io, &vs, &one_running_diag);

	one_check("(b) while a receive runs", "the refusals were made", fired == 1);
	for (uint32_t i = 0; i < 1000u && sink.len == 1000u; i++)
		if (buf[i] != pat_byte(i))
			same = 0;
	one_check("(b) while a receive runs", "the running receive still "
	          "succeeds, byte for byte", rc == YM_OK && sink.len == 1000u &&
	          same && one_running_diag.blocks == 3u);
	one_check("(e) after the receive", "the question says a start now may go",
	          ymodem_busy(&one_xfer) == 0);
}

/* ---- (d): every way a transfer fails gives the mark back ------------------ */

static int fail_read(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got)
{
	(void)ctx; (void)dst; (void)want;
	*got = 0;
	return -1;
}

static int fail_put(void *ctx, const uint8_t *buf, size_t len)
{
	(void)ctx; (void)buf; (void)len;
	return -1;
}

/* After a failed transfer: the question says go, and a good receive runs. */
static void one_after(const char *what, enum ym_result want, enum ym_result got)
{
	static uint8_t      in[4096];
	static uint8_t      buf[1024];
	struct script       s;
	struct cap_sink     sink;
	struct ym_io        io = { &s, script_getc, script_put };
	struct ym_sink      vs = { &sink, cap_begin, cap_write };
	struct ym_recv_diag d;

	one_check(what, "failed the way it should", got == want);
	one_check(what, "(d) the question says the next one may start",
	          ymodem_busy(&one_xfer) == 0);
	memset(&s, 0, sizeof s);
	s.in = in;
	s.inlen = one_good_batch(in, 100u);
	memset(&sink, 0, sizeof sink);
	sink.buf = buf; sink.cap = sizeof buf;
	one_check(what, "(d) the next transfer starts and completes",
	          ymodem_recv(&one_xfer, &io, &vs, &d) == YM_OK && sink.len == 100u);
}

static void one_failures_give_back(void)
{
	static uint8_t   in[2048];
	static uint8_t   buf[1024];
	struct script    s;
	struct cap_sink  sink;
	struct ym_io     io = { &s, script_getc, script_put };
	struct ym_io     bad_io = { &s, script_getc, fail_put };
	struct ym_sink   vs = { &sink, cap_begin, cap_write };
	struct pat_src   ps = { 0u, 3000u };
	struct ym_source src = { &ps, "s.bin", 3000u, pat_read };
	struct ym_source bad_src = { &ps, "s.bin", 3000u, fail_read };
	struct ym_recv_diag d;
	uint8_t          b0[128], blk[1024];
	size_t           n;

#define ONE_RESET(...) do {                                             \
		static const uint8_t bytes_[] = { 0, __VA_ARGS__ };     \
		memset(&s, 0, sizeof s);                                \
		memcpy(in, bytes_ + 1, sizeof bytes_ - 1);              \
		s.in = in; s.inlen = sizeof bytes_ - 1;                 \
		memset(&sink, 0, sizeof sink);                          \
		sink.buf = buf; sink.cap = sizeof buf;                  \
		ps.pos = 0u;                                            \
	} while (0)

	ONE_RESET(CAN, CAN);
	one_after("(d) a send the receiver cancels", YM_ERR_CANCEL,
	          ymodem_send(&one_xfer, &io, &src));
	ONE_RESET();
	one_after("(d) a send that times out", YM_ERR_TIMEOUT,
	          ymodem_send(&one_xfer, &io, &src));
	ONE_RESET(CRC_C, ACK, CRC_C);
	one_after("(d) a send whose source fails", YM_ERR_SOURCE,
	          ymodem_send(&one_xfer, &io, &bad_src));
	ONE_RESET(CRC_C);
	one_after("(d) a send whose transport fails", YM_ERR_IO,
	          ymodem_send(&one_xfer, &bad_io, &src));
	ONE_RESET(CAN, CAN);
	one_after("(d) a receive the sender cancels", YM_ERR_CANCEL,
	          ymodem_recv(&one_xfer, &io, &vs, &d));
	ONE_RESET();
	one_after("(d) a receive that times out", YM_ERR_TIMEOUT,
	          ymodem_recv(&one_xfer, &io, &vs, &d));
	ONE_RESET();
	one_after("(d) a receive whose transport fails", YM_ERR_IO,
	          ymodem_recv(&one_xfer, &bad_io, &vs, &d));

	ONE_RESET();
	build_b0(b0, "a.bin", "1024");
	memset(blk, 0x5A, sizeof blk);
	n = put_block(in, 0, SOH, 0, b0, 128, 0);
	s.inlen = n;
	sink.rejects = 1;
	one_after("(d) a receive the sink refuses", YM_ERR_SINK,
	          ymodem_recv(&one_xfer, &io, &vs, &d));

	ONE_RESET();
	n = put_block(in, 0, SOH, 0, b0, 128, 0);
	n = put_block(in, n, STX, 7, blk, 1024, 0);    /* out of order */
	s.inlen = n;
	one_after("(d) a receive with a protocol error", YM_ERR_PROTO,
	          ymodem_recv(&one_xfer, &io, &vs, &d));
#undef ONE_RESET
}

static int one_at_a_time(void)
{
	one_check("(e) before anything", "the question says a start now may go",
	          ymodem_busy(&one_xfer) == 0);
	one_during_send();
	one_during_recv();
	one_failures_give_back();
	if (one_failures) {
		printf("  one transfer at a time: %d check(s) FAILED\n", one_failures);
		return 1;
	}
	printf("  one transfer at a time: ok\n");
	return 0;
}

int main(void)
{
	/* ---- 1. duplex loopback ---- */
	loopback(1, "one.bin");
	loopback(100, "frame.raw");
	loopback(128, "b128.bin");
	loopback(1024, "b1024.bin");
	loopback(1025, "b1025.bin");
	/* crosses the seq 255 -> 0 wrap */
	loopback(260u * 1024u, "wrap.bin");
	/* the owhinata/wio-lite-ai#19 M5 stock-image size */
	loopback(0xDC000u, "stock.bin");
	printf("  duplex loopback: ok\n");

	static uint8_t in[16384];
	static uint8_t data[1024], data2[1024];
	uint8_t        b0[128], nullb0[128];
	struct script  s;
	struct cap_sink sink;
	static uint8_t sinkbuf[4096];

	for (int i = 0; i < 1024; i++) {
		data[i]  = pat_byte((uint32_t)i);
		data2[i] = pat_byte((uint32_t)i + 1024u);   /* block 2, not block 1 */
	}
	memset(nullb0, 0, sizeof nullb0);

	/* ---- K. the "nothing received" value (issue #230) ---- */
	{
		struct ym_recv_diag z;

		memset(&z, 0xA5, sizeof z);
		ymodem_recv_diag_init(&z);
		assert(z.blocks == 0u && z.bad_crc == 0u && z.bad_seq == 0u &&
		       z.short_read == 0u && z.timeouts == 0u);
		assert(z.first_kind == -1 && z.first_seq == -1 && z.first_nseq == -1);
		assert(z.first_got == 0u && z.first_want == 0u &&
		       z.first_crc_want == 0u && z.first_crc_got == 0u);
	}

#define SCRIPT_BEGIN()  do {                                    \
		memset(&s, 0, sizeof s);                        \
		s.in = in; s.inlen = 0;                         \
		memset(&sink, 0, sizeof sink);                  \
		sink.buf = sinkbuf; sink.cap = sizeof sinkbuf;  \
	} while (0)

	/* ---- L. CRC damage on block 1 -> NAK -> the resend completes ---- */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		build_b0(b0, "a.bin", "1024");
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, STX, 1, data, 1024, 1);   /* bad CRC */
		n = put_block(in, n, STX, 1, data, 1024, 0);   /* the resend */
		in[n++] = EOT;
		n = put_block(in, n, SOH, 0, nullb0, 128, 0);
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_OK);
		assert(sink.len == 1024);
		assert(memcmp(sink.buf, data, 1024) == 0);     /* exactly once */
		assert(count_byte(&s, NAK) == 1);              /* the bad block was NAKed */
		/* The record came back to the caller: block 0, the resend and the
		 * closing block 0 accepted, the first rejection is the damaged STX. */
		assert(last_diag.blocks == 3u && last_diag.bad_crc == 1u);
		assert(last_diag.bad_seq == 0u && last_diag.short_read == 0u);
		assert(last_diag.first_kind == STX && last_diag.first_seq == 1 &&
		       last_diag.first_nseq == 0xFE);
		assert(last_diag.first_got == 1024u && last_diag.first_want == 1024u);
		assert(last_diag.first_crc_want != last_diag.first_crc_got);
	}

	/* ---- M. duplicate block (our ACK was lost) -> re-ACKed and dropped ---- */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		build_b0(b0, "a.bin", "2048");
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, STX, 1, data, 1024, 0);
		n = put_block(in, n, STX, 1, data, 1024, 0);   /* duplicate seq 1 */
		n = put_block(in, n, STX, 2, data2, 1024, 0);
		in[n++] = EOT;
		n = put_block(in, n, SOH, 0, nullb0, 128, 0);
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_OK);
		assert(sink.len == 2048);                      /* not 3072 */
		/* [!] AND THE RIGHT 2048.  The two data blocks carry different bytes
		 * on purpose: with identical content, a receiver that delivered the
		 * DUPLICATE and dropped seq 2 would produce the same length and the
		 * same bytes as one that got it right. */
		assert(memcmp(sink.buf, data, 1024) == 0);
		assert(memcmp(sink.buf + 1024, data2, 1024) == 0);
		assert(count_byte(&s, NAK) == 0);
	}

	/* ---- N. out-of-order block -> YM_ERR_PROTO + CAN teardown ---- */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		build_b0(b0, "a.bin", "4096");
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, STX, 1, data, 1024, 0);
		n = put_block(in, n, STX, 7, data, 1024, 0);   /* jumped */
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_ERR_PROTO);
		assert(count_byte(&s, CAN) >= 5);
	}

	/* ---- O. sink refuses the file -> YM_ERR_SINK, no data delivered ---- */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		sink.rejects = 1;
		build_b0(b0, "a.bin", "1024");
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, STX, 1, data, 1024, 0);
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_ERR_SINK);
		assert(sink.begins == 1);
		assert(sink.len == 0);
		assert(count_byte(&s, CAN) >= 5);
	}

	/* ---- P. batch never closes (stream ends after EOT) -> TIMEOUT, not OK ---- */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		build_b0(b0, "a.bin", "1024");
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, STX, 1, data, 1024, 0);
		in[n++] = EOT;
		s.inlen = n;                                   /* no closing block 0 */

		assert(run_script(&s, &sink) == YM_ERR_TIMEOUT);
		assert(sink.len == 1024);                      /* data arrived ... */
		assert(count_byte(&s, CAN) >= 5);              /* ... but it is NOT success */
	}

	/* ---- Q. declared size trims the padded short block ---- */
	{
		size_t  n = 0;
		uint8_t shortblk[128];
		SCRIPT_BEGIN();
		memcpy(shortblk, data, 100);
		memset(shortblk + 100, CPMEOF, 28);
		build_b0(b0, "a.bin", "100");
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, SOH, 1, shortblk, 128, 0);
		in[n++] = EOT;
		n = put_block(in, n, SOH, 0, nullb0, 128, 0);
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_OK);
		assert(sink.declared == 100);
		assert(sink.len == 100);                       /* padding trimmed */
		assert(memcmp(sink.buf, data, 100) == 0);
	}

	/* ---- R. an immediate null block 0 = the sender offered no file ---- */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		n = put_block(in, n, SOH, 0, nullb0, 128, 0);
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_ERR_CANCEL);
		assert(sink.begins == 0);
		assert(last_diag.blocks == 1u && last_diag.first_kind == -1 &&
		       last_diag.timeouts == 0u);
	}

	/* ---- T. EOT before the declared size is delivered -> NOT success ----
	 * A truncated file must not be able to reach the closing block and be reported
	 * as OK; the caller would otherwise stage a short image and program it. */
	{
		size_t n = 0;
		SCRIPT_BEGIN();
		build_b0(b0, "a.bin", "2048");           /* promises 2048 ... */
		n = put_block(in, n, SOH, 0, b0, 128, 0);
		n = put_block(in, n, STX, 1, data, 1024, 0);   /* ... delivers 1024 */
		in[n++] = EOT;
		n = put_block(in, n, SOH, 0, nullb0, 128, 0);  /* and closes the batch */
		s.inlen = n;

		assert(run_script(&s, &sink) == YM_ERR_PROTO);
		assert(count_byte(&s, CAN) >= 5);
	}

	/* ---- U. malformed block 0 -> rejected before any data is accepted ---- */
	{
		size_t  n = 0;
		uint8_t bad[128];

		/* U1: a decimal size that overflows 32 bits (would wrap to a small value
		 * and slip past the sink's capacity check). */
		SCRIPT_BEGIN();
		build_b0(bad, "a.bin", "99999999999");
		n = put_block(in, n, SOH, 0, bad, 128, 0);
		s.inlen = n;
		assert(run_script(&s, &sink) == YM_ERR_PROTO);
		assert(sink.begins == 0);

		/* U2: an unterminated name field (no NUL anywhere in the block). */
		SCRIPT_BEGIN();
		memset(bad, 'x', sizeof bad);
		n = 0;
		n = put_block(in, n, SOH, 0, bad, 128, 0);
		s.inlen = n;
		assert(run_script(&s, &sink) == YM_ERR_PROTO);
		assert(sink.begins == 0);
	}

	/* ---- S. a silent sender -> TIMEOUT after the handshake budget ---- */
	{
		SCRIPT_BEGIN();
		s.inlen = 0;
		assert(run_script(&s, &sink) == YM_ERR_TIMEOUT);
		assert(count_byte(&s, CRC_C) > 1);             /* it kept poking with 'C' */
		/* Every 'C' waited out its header timeout, and nothing arrived. */
		assert(last_diag.timeouts == 41u && last_diag.blocks == 0u &&
		       last_diag.first_kind == -1);
	}

	/* ---- 3. one transfer at a time (issue #230) ---- */
	if (one_at_a_time() != 0)
		return 1;

	printf("test_ymodem_recv: all assertions passed\n");
	return 0;
}
