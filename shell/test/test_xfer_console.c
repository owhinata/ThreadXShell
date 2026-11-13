/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the one YMODEM-over-console wiring (shell/cmds/
 * xfer_console.c, issue #231) as the boards compile it, over fakes that record
 * every call in order: the console claim / release / read / write / flush and
 * the three output calls (cli.h), log_write(), tx_time_get(), and the protocol
 * core's ymodem_busy() / ymodem_send() / ymodem_recv() (svc/ymodem.h), whose
 * results the test chooses and which read one byte through the IO vtable they
 * were handed.
 *
 * Built once per shape of the board table (xfer_console_board_facts, defined
 * here):
 *   FULL      owner + a telnet question the test can make refuse (f746g-disco
 *             and wio-lite-ai name net_tty_guard)
 *   NOTELNET  owner + xfer_console_no_telnet (grove-vision-ai-v2)
 *   NOOWNER   the owner field left NULL
 *   NOGUARD   the telnet field left NULL
 *
 * Pinned (nothing typed at a console shows the order of these):
 *   - the telnet refusal comes before the claim and before the notice, in the
 *     send and in the receive, and the receive then touches nothing;
 *   - "another transfer is running" is said after the claim and before the
 *     notice, and the claim is given back;
 *   - every exit after a claim gives it back exactly once (each protocol
 *     result), and a refused claim gives nothing back; the receive never
 *     claims or releases;
 *   - a send from a caller that already holds the console leaves the
 *     caller's claim as it was, on every path (issue #231);
 *   - with a field of the table empty, the telnet question, the busy question,
 *     the send and the receive all refuse, touching nothing (no claim, no
 *     flush, no read, no protocol call);
 *   - the "no telnet" answer never refuses and prints nothing;
 *   - while sending a 0x03 is an abort, while receiving it is a data byte;
 *   - against a peer that never stops, each discard (before, after, the
 *     drain after a failure) stops at 256 KB and says so, and the flow goes
 *     on (issue #231, D7).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "log.h"
#include "tx_api.h"
#include "xfer_console.h"

#if !defined(XFER_TEST_SHAPE_FULL) && !defined(XFER_TEST_SHAPE_NOTELNET) && \
    !defined(XFER_TEST_SHAPE_NOOWNER) && !defined(XFER_TEST_SHAPE_NOGUARD)
#error "define one XFER_TEST_SHAPE_*"
#endif

static int failures;

static void check(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

/* ---- the call record ------------------------------------------------------ */

static char t_rec[8192];

static void rec(const char *fmt, ...)
{
	size_t n = strlen(t_rec);
	va_list ap;

	if (n + 2 >= sizeof t_rec)
		return;
	va_start(ap, fmt);
	vsnprintf(t_rec + n, sizeof t_rec - n - 1, fmt, ap);
	va_end(ap);
	strcat(t_rec, "|");
}

/* Position of @p what in the record, or -1. */
static int at(const char *what)
{
	const char *p = strstr(t_rec, what);

	return p ? (int)(p - t_rec) : -1;
}

__attribute__((unused)) static int count(const char *what)
{
	int n = 0;
	const char *p = t_rec;

	while ((p = strstr(p, what)) != NULL) {
		n++;
		p += strlen(what);
	}
	return n;
}

/* ---- the console fakes ---------------------------------------------------- */

static char t_console_obj;
static struct cli_instance *const t_sh = (struct cli_instance *)&t_console_obj;

static int t_claim_rc;            /* what cli_console_claim() answers */
static int t_held;                /* claims not yet given back */
static int t_rx[16];              /* bytes the peer sends while the core runs */
static int t_rx_n, t_rx_i;

/*
 * Where the transfer is: before the protocol core runs, inside it, after it.
 * The queued bytes above arrive only while the core runs (the ring is empty
 * before and after).  A peer that never stops (t_endless) has a byte ready
 * on every read, in every phase; its reads are counted per phase and not
 * written to the record, which would not hold them.
 */
enum { T_BEFORE, T_CORE, T_AFTER };
static int t_phase;
static int t_endless;
static unsigned long t_reads[3];
static int t_flush_cut;           /* the stand-in flush stopped an endless peer */

/* One byte from the peer, or -1: the ring as cli_read_byte() and
 * cli_rx_flush() both see it. */
static int peer_byte(void)
{
	t_reads[t_phase]++;
	if (t_endless)
		return 0x43;
	if (t_phase == T_CORE && t_rx_i < t_rx_n)
		return t_rx[t_rx_i++];
	return -1;
}

int cli_console_claim(struct cli_instance *sh)
{
	(void)sh;
	rec("claim");
	if (t_claim_rc == 0)
		t_held++;
	return t_claim_rc;
}

void cli_console_release(struct cli_instance *sh)
{
	(void)sh;
	rec("release");
	t_held--;
}

int cli_read_byte(struct cli_instance *sh, unsigned timeout_ms)
{
	(void)sh;
	(void)timeout_ms;
	if (!t_endless)
		rec("read");
	return peer_byte();
}

int cli_write(struct cli_instance *sh, const void *data, size_t len)
{
	(void)sh;
	(void)data;
	rec("write");
	return (int)len;
}

/* The core's flush reads until the ring is empty, with no bound
 * (shell/core/cli_core.c).  Against an endless peer that would not return;
 * this stand-in stops at 4 MiB and says so, so a wiring that still uses it
 * there fails the cap cases instead of hanging the run. */
void cli_rx_flush(struct cli_instance *sh)
{
	unsigned long n = 0;

	(void)sh;
	rec("flush");
	while (peer_byte() >= 0) {
		if (++n >= 4ul * 1024ul * 1024ul) {
			t_flush_cut = 1;
			break;
		}
	}
}

static void out(const char *kind, const char *fmt, va_list ap)
{
	char line[256];

	vsnprintf(line, sizeof line, fmt, ap);
	rec("%s:%s", kind, line);
}

int cli_print(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;

	(void)sh;
	va_start(ap, fmt);
	out("print", fmt, ap);
	va_end(ap);
	return 0;
}

int cli_error(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;

	(void)sh;
	va_start(ap, fmt);
	out("error", fmt, ap);
	va_end(ap);
	return 0;
}

int cli_warn(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;

	(void)sh;
	va_start(ap, fmt);
	out("warn", fmt, ap);
	va_end(ap);
	return 0;
}

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	(void)level;
	(void)tag;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	rec("log:%s", line);
}

ULONG tx_time_get(void)
{
	return 0u;
}

/* ---- the protocol-core fakes ---------------------------------------------- */

__attribute__((unused)) static struct ym_xfer t_owner;
static const struct ym_xfer *t_owner_seen;
static int t_busy;                /* what ymodem_busy() answers */
static enum ym_result t_result;   /* what ymodem_send() / _recv() answer */
static int t_getc_seen;           /* what the vtable's getc gave the core */

int ymodem_busy(const struct ym_xfer *x)
{
	rec("busy?");
	t_owner_seen = x;
	return t_busy;
}

enum ym_result ymodem_send(struct ym_xfer *x, const struct ym_io *io,
                           const struct ym_source *src)
{
	(void)src;
	rec("ym_send");
	t_owner_seen = x;
	t_phase = T_CORE;
	t_getc_seen = io->getc(io->ctx, 10u);
	t_phase = T_AFTER;
	return t_result;
}

/* svc/ymodem.c's own value, written out: the real file is not linked here. */
void ymodem_recv_diag_init(struct ym_recv_diag *d)
{
	memset(d, 0, sizeof *d);
	d->first_kind = -1;
	d->first_seq = -1;
	d->first_nseq = -1;
}

enum ym_result ymodem_recv(struct ym_xfer *x, const struct ym_io *io,
                           const struct ym_sink *sink, struct ym_recv_diag *diag)
{
	(void)sink;
	rec("ym_recv");
	t_owner_seen = x;
	diag->blocks = 7u;
	t_phase = T_CORE;
	t_getc_seen = io->getc(io->ctx, 10u);
	t_phase = T_AFTER;
	return t_result;
}

/* ---- the board table, per shape ------------------------------------------- */

static int t_telnet_refuses;      /* FULL: the question says "telnet" */

#if defined(XFER_TEST_SHAPE_FULL)
static int t_guard(struct cli_instance *sh, const char *what)
{
	rec("guard:%s", what);
	if (!t_telnet_refuses)
		return 0;
	cli_error(sh, "%s: refused on the telnet console\r\n", what);
	return 1;
}
const struct xfer_console_facts xfer_console_board_facts = {
	.owner = &t_owner, .telnet = t_guard,
};
#elif defined(XFER_TEST_SHAPE_NOTELNET)
const struct xfer_console_facts xfer_console_board_facts = {
	.owner = &t_owner, .telnet = xfer_console_no_telnet,
};
#elif defined(XFER_TEST_SHAPE_NOOWNER)
static int t_guard(struct cli_instance *sh, const char *what)
{
	(void)sh;
	rec("guard:%s", what);
	return 0;
}
const struct xfer_console_facts xfer_console_board_facts = {
	.owner = NULL, .telnet = t_guard,
};
#else /* NOGUARD */
const struct xfer_console_facts xfer_console_board_facts = {
	.owner = &t_owner, .telnet = NULL,
};
#endif

/* ---- helpers --------------------------------------------------------------- */

static struct ym_source t_src = { NULL, "frame.raw", 1234u, NULL };
static struct ym_sink   t_sink;

static void reset(void)
{
	t_rec[0] = '\0';
	t_claim_rc = 0;
	t_held = 0;
	t_rx_n = t_rx_i = 0;
	t_busy = 0;
	t_result = YM_OK;
	t_getc_seen = 99;
	t_owner_seen = NULL;
	t_telnet_refuses = 0;
	t_phase = T_BEFORE;
	t_endless = 0;
	t_reads[0] = t_reads[1] = t_reads[2] = 0;
	t_flush_cut = 0;
}

__attribute__((unused)) static void rx(int byte)
{
	t_rx[t_rx_n++] = byte;
}

#define NOTICE "print:ymodem: sending '"

/* Where the first discard of the RX ring is in the record: a flush, or a
 * read before the protocol core (the wiring may do either). */
__attribute__((unused)) static int discard_at(void)
{
	int f = at("flush|"), r = at("read|");

	if (f < 0)
		return r;
	if (r < 0)
		return f;
	return f < r ? f : r;
}

/* ---- the shapes with a whole table ---------------------------------------- */

#if defined(XFER_TEST_SHAPE_FULL) || defined(XFER_TEST_SHAPE_NOTELNET)
static void test_send_results(void)
{
	static const enum ym_result res[] = {
		YM_OK, YM_ERR_CANCEL, YM_ERR_TIMEOUT, YM_ERR_SOURCE, YM_ERR_IO,
		YM_ERR_SINK, YM_ERR_PROTO, YM_ERR_BUSY,
	};
	char what[96];
	unsigned i;

	printf("send: every protocol result gives the claim back once\n");
	for (i = 0; i < sizeof res / sizeof res[0]; i++) {
		int rc;

		reset();
		t_result = res[i];
		rc = xfer_send_source(t_sh, &t_src);
		snprintf(what, sizeof what, "result %d: rc %d, one claim, one release, "
		         "nothing held", (int)res[i], rc);
		check(what, rc == (res[i] == YM_OK ? 0 : 1) && count("claim|") == 1 &&
		            count("release|") == 1 && t_held == 0);
		snprintf(what, sizeof what, "result %d: notice, then the protocol, "
		         "then the release last", (int)res[i]);
		check(what, at("claim|") < at(NOTICE) && at(NOTICE) < at("ym_send|") &&
		            at("ym_send|") < at("release|") &&
		            (int)strlen(t_rec) == at("release|") + (int)strlen("release|"));
		check("the board's owner reached the protocol core",
		      t_owner_seen == &t_owner);
	}
}

static void test_send_refusals(void)
{
	printf("send: refusals before the notice\n");

	reset();
	t_claim_rc = -2;
	check("a background job: refused, nothing to give back",
	      xfer_send_source(t_sh, &t_src) == 1 && count("release|") == 0 &&
	      at(NOTICE) < 0 && at("ym_send|") < 0 &&
	      at("error:ymodem: cannot run in the background") >= 0);

	reset();
	t_claim_rc = -1;
	check("the console busy: refused, nothing to give back",
	      xfer_send_source(t_sh, &t_src) == 1 && count("release|") == 0 &&
	      at(NOTICE) < 0 && at("error:ymodem: console busy") >= 0);

	reset();
	t_busy = 1;
	check("another transfer: refused after the claim, before the notice, "
	      "claim given back",
	      xfer_send_source(t_sh, &t_src) == 1 &&
	      at("claim|") < at("busy?|") &&
	      at("busy?|") < at("error:ymodem: another transfer is running") &&
	      at("error:ymodem: another transfer is running") < at("release|") &&
	      at(NOTICE) < 0 && at("ym_send|") < 0 && t_held == 0 &&
	      count("release|") == 1);
	check("  ...asked of the board's owner", t_owner_seen == &t_owner);

	reset();
	t_busy = 1;
	check("the busy question alone: 1 and the sentence",
	      xfer_refuse_if_busy(t_sh) == 1 &&
	      at("error:ymodem: another transfer is running") >= 0);
	reset();
	check("the busy question alone: 0 and silent",
	      xfer_refuse_if_busy(t_sh) == 0 && at("error:") < 0 && at("print:") < 0);
}

static void test_ctrl_c(void)
{
	printf("0x03: an abort while sending, data while receiving\n");

	reset();
	rx(3);
	xfer_send_source(t_sh, &t_src);
	check("send: the core reads YM_IO_ABORT", t_getc_seen == YM_IO_ABORT);

	reset();
	rx(4);
	xfer_send_source(t_sh, &t_src);
	check("send: another byte passes", t_getc_seen == 4);

	reset();
	rx(3);
	{
		struct ym_recv_diag d;

		ymodem_recv_diag_init(&d);
		xfer_recv_sink_locked(t_sh, &t_sink, &d);
	}
	check("receive: the core reads 0x03", t_getc_seen == 3);
}

static void test_recv(void)
{
	static const enum ym_result res[] = {
		YM_OK, YM_ERR_CANCEL, YM_ERR_TIMEOUT, YM_ERR_IO, YM_ERR_SINK,
		YM_ERR_PROTO, YM_ERR_BUSY,
	};
	struct ym_recv_diag d;
	char what[96];
	unsigned i;

	printf("receive: the caller holds the console\n");
	for (i = 0; i < sizeof res / sizeof res[0]; i++) {
		int rc;

		reset();
		t_result = res[i];
		ymodem_recv_diag_init(&d);
		rc = xfer_recv_sink_locked(t_sh, &t_sink, &d);
		snprintf(what, sizeof what, "result %d: rc %d, no claim, no release, "
		         "board owner", (int)res[i], rc);
		check(what, rc == (res[i] == YM_OK ? 0 : 1) && count("claim|") == 0 &&
		            count("release|") == 0 && t_owner_seen == &t_owner &&
		            discard_at() >= 0 && discard_at() < at("ym_recv|") &&
		            count("log:") >= 1);
	}
	reset();
	t_result = YM_ERR_BUSY;
	ymodem_recv_diag_init(&d);
	xfer_recv_sink_locked(t_sh, &t_sink, &d);
	check("a refusal by the core is said",
	      at("error:ymodem: another transfer is running") > at("ym_recv|"));
}

/*
 * issue #231: a caller that already holds the console (wio-lite-ai's `wifi
 * flash backup`) calls the one send entry.  The claim it makes is one more
 * on top of the caller's, and every path gives back exactly that one: the
 * count after the call is the count before it.
 */
static void test_send_nested(void)
{
	static const enum ym_result res[] = {
		YM_OK, YM_ERR_CANCEL, YM_ERR_TIMEOUT, YM_ERR_SOURCE, YM_ERR_IO,
		YM_ERR_BUSY,
	};
	char what[112];
	unsigned i;

	printf("send from a caller that already holds the console\n");
	for (i = 0; i < sizeof res / sizeof res[0]; i++) {
		int rc;

		reset();
		t_held = 1;                     /* the caller's own claim */
		t_result = res[i];
		rc = xfer_send_source(t_sh, &t_src);
		snprintf(what, sizeof what, "result %d: rc %d, the caller's claim is "
		         "as it was (1), one claim and one release of its own",
		         (int)res[i], rc);
		check(what, rc == (res[i] == YM_OK ? 0 : 1) && t_held == 1 &&
		            count("claim|") == 1 && count("release|") == 1 &&
		            at(NOTICE) >= 0);
	}

	reset();
	t_held = 1;
	t_busy = 1;
	check("another transfer: refused before the notice, the caller's claim "
	      "as it was",
	      xfer_send_source(t_sh, &t_src) == 1 && t_held == 1 &&
	      count("release|") == 1 && at(NOTICE) < 0);

	reset();
	t_held = 1;
	t_claim_rc = -3;                    /* the claim count at its limit */
	check("a claim refused at the limit: nothing given back, the caller's "
	      "claim as it was",
	      xfer_send_source(t_sh, &t_src) == 1 && t_held == 1 &&
	      count("release|") == 0 && at(NOTICE) < 0 &&
	      at("error:ymodem: console busy") >= 0);
}
#endif

#if defined(XFER_TEST_SHAPE_FULL) || defined(XFER_TEST_SHAPE_NOTELNET)
/*
 * issue #231 (D7): every discard in the wiring -- before the transfer, after
 * it, and the drain after a failure -- stops at 256 KB against a peer that
 * never stops, says so on the console and in the log, and the flow goes on
 * as before: the transfer still runs, the result is still returned, the claim
 * is still given back.
 */
#define T_CAP 262144ul

static void test_discard_cap(void)
{
	struct ym_recv_diag d;
	const char *warn = "warn:ymodem: the peer is STILL sending -- stopped "
	                   "discarding after 262144 B";
	int rc;

	printf("a peer that never stops: every discard stops at 256 KB\n");

	reset();
	t_endless = 1;
	rc = xfer_send_source(t_sh, &t_src);
	check("send OK: the transfer runs and its result is returned",
	      rc == 0 && at("ym_send|") >= 0 && at("print:ymodem: sent ") >= 0);
	check("send OK: 256 KB discarded before the transfer, then it stopped",
	      t_reads[T_BEFORE] == T_CAP && !t_flush_cut);
	check("send OK: 256 KB discarded after the transfer, then it stopped",
	      t_reads[T_AFTER] == T_CAP);
	check("send OK: said twice on the console and twice in the log",
	      count(warn) == 2 && count("log:drain cap hit at 262144 B") == 2);
	check("send OK: the claim given back", t_held == 0 &&
	      count("release|") == 1);

	reset();
	t_endless = 1;
	t_result = YM_ERR_TIMEOUT;
	rc = xfer_send_source(t_sh, &t_src);
	check("send failed: 256 KB after the transfer, 256 KB more in the drain",
	      rc == 1 && t_reads[T_BEFORE] == T_CAP &&
	      t_reads[T_AFTER] == 2ul * T_CAP && !t_flush_cut);
	check("send failed: said three times, the result still printed",
	      count(warn) == 3 && count("log:drain cap hit at 262144 B") == 3 &&
	      at("error:ymodem: timeout") >= 0 && t_held == 0);

	reset();
	t_endless = 1;
	ymodem_recv_diag_init(&d);
	rc = xfer_recv_sink_locked(t_sh, &t_sink, &d);
	check("receive OK: 256 KB before, 256 KB after, said twice, result kept",
	      rc == 0 && at("ym_recv|") >= 0 && t_reads[T_BEFORE] == T_CAP &&
	      t_reads[T_AFTER] == T_CAP && count(warn) == 2 && !t_flush_cut);

	reset();
	t_endless = 1;
	t_result = YM_ERR_CANCEL;
	ymodem_recv_diag_init(&d);
	rc = xfer_recv_sink_locked(t_sh, &t_sink, &d);
	check("receive failed: 256 KB before, 512 KB after, said three times",
	      rc == 1 && t_reads[T_BEFORE] == T_CAP &&
	      t_reads[T_AFTER] == 2ul * T_CAP && count(warn) == 3 &&
	      count("log:drain cap hit at 262144 B") == 3 && !t_flush_cut);

	/* A quiet line: nothing said, the core's timing kept. */
	reset();
	check("a quiet line: nothing said",
	      xfer_send_source(t_sh, &t_src) == 0 && count("warn:") == 0 &&
	      count("cap hit") == 0);
}
#endif

#if defined(XFER_TEST_SHAPE_FULL)
static void test_telnet(void)
{
	struct ym_recv_diag d;

	printf("telnet: refused first, touching nothing\n");

	reset();
	t_telnet_refuses = 1;
	check("send: refused, the question asked as \"ymodem\", before any claim "
	      "or notice",
	      xfer_send_source(t_sh, &t_src) == 1 && at("guard:ymodem|") == 0 &&
	      at("claim|") < 0 && at(NOTICE) < 0 && at("ym_send|") < 0 &&
	      at("flush|") < 0 && at("read|") < 0);

	reset();
	t_telnet_refuses = 1;
	ymodem_recv_diag_init(&d);
	check("receive: refused, nothing touched, the record left as initialised",
	      xfer_recv_sink_locked(t_sh, &t_sink, &d) == 1 &&
	      at("guard:ymodem|") == 0 && at("ym_recv|") < 0 && at("flush|") < 0 &&
	      at("read|") < 0 && d.blocks == 0u && d.first_kind == -1);

	reset();
	t_telnet_refuses = 1;
	check("the shared question: 1 on telnet",
	      xfer_refuse_if_telnet(t_sh) == 1 && at("guard:ymodem|") == 0);
	reset();
	check("the shared question: 0 elsewhere, silent",
	      xfer_refuse_if_telnet(t_sh) == 0 && at("error:") < 0);

	reset();
	check("send: the question is the first thing it does",
	      xfer_send_source(t_sh, &t_src) == 0 && at("guard:ymodem|") == 0);
	reset();
	ymodem_recv_diag_init(&d);
	check("receive: the question is the first thing it does",
	      xfer_recv_sink_locked(t_sh, &t_sink, &d) == 0 &&
	      at("guard:ymodem|") == 0);
}
#endif

#if defined(XFER_TEST_SHAPE_NOTELNET)
static void test_no_telnet(void)
{
	struct ym_recv_diag d;

	printf("no telnet console: the shared answer never refuses\n");
	reset();
	check("the answer itself: 0, silent",
	      xfer_console_no_telnet(t_sh, "ymodem") == 0 && t_rec[0] == '\0');
	reset();
	check("the shared question: 0, silent",
	      xfer_refuse_if_telnet(t_sh) == 0 && t_rec[0] == '\0');
	reset();
	check("send runs", xfer_send_source(t_sh, &t_src) == 0 &&
	                   at("ym_send|") >= 0);
	reset();
	ymodem_recv_diag_init(&d);
	check("receive runs", xfer_recv_sink_locked(t_sh, &t_sink, &d) == 0 &&
	                      at("ym_recv|") >= 0);
}
#endif

#if defined(XFER_TEST_SHAPE_NOOWNER) || defined(XFER_TEST_SHAPE_NOGUARD)
static void test_empty_field(void)
{
	struct ym_recv_diag d;
	const char *say = "error:ymodem: refused -- this firmware's transfer table";

	printf("a field of the table left empty: every entry refuses, touching "
	       "nothing\n");

	reset();
	check("the telnet question refuses and says why",
	      xfer_refuse_if_telnet(t_sh) == 1 && at(say) == 0 && at("guard:") < 0);
	reset();
	check("the busy question refuses and says why",
	      xfer_refuse_if_busy(t_sh) == 1 && at(say) == 0 && at("busy?|") < 0);
	reset();
	check("send refuses before any claim, flush, read or notice",
	      xfer_send_source(t_sh, &t_src) == 1 && at(say) == 0 &&
	      at("claim|") < 0 && at("flush|") < 0 && at("read|") < 0 &&
	      at(NOTICE) < 0 && at("ym_send|") < 0 && at("guard:") < 0);
	reset();
	ymodem_recv_diag_init(&d);
	check("receive refuses, touching nothing, the record left as initialised",
	      xfer_recv_sink_locked(t_sh, &t_sink, &d) == 1 && at(say) == 0 &&
	      at("flush|") < 0 && at("read|") < 0 && at("ym_recv|") < 0 &&
	      d.blocks == 0u);
	check("nothing held", t_held == 0);
}
#endif

int main(void)
{
#if defined(XFER_TEST_SHAPE_FULL)
	printf("test_xfer_console (FULL)\n");
	test_send_results();
	test_send_refusals();
	test_ctrl_c();
	test_recv();
	test_send_nested();
	test_discard_cap();
	test_telnet();
#elif defined(XFER_TEST_SHAPE_NOTELNET)
	printf("test_xfer_console (NOTELNET)\n");
	test_send_results();
	test_send_refusals();
	test_ctrl_c();
	test_recv();
	test_send_nested();
	test_discard_cap();
	test_no_telnet();
#else
	printf("test_xfer_console (%s)\n",
#if defined(XFER_TEST_SHAPE_NOOWNER)
	       "NOOWNER"
#else
	       "NOGUARD"
#endif
	);
	test_empty_field();
#endif
	if (failures) {
		printf("test_xfer_console: %d FAILED\n", failures);
		return 1;
	}
	printf("test_xfer_console: all passed\n");
	return 0;
}
