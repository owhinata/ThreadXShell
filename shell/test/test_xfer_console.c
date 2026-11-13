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
 *     result), and a refused claim gives nothing back; the receive and the
 *     "already held" send never claim or release;
 *   - with a field of the table empty, the telnet question, the busy question,
 *     both sends and the receive all refuse, touching nothing (no claim, no
 *     flush, no read, no protocol call);
 *   - the "no telnet" answer never refuses and prints nothing;
 *   - while sending a 0x03 is an abort, while receiving it is a data byte.
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
static int t_rx[16];              /* bytes cli_read_byte() returns, then -1 */
static int t_rx_n, t_rx_i;

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
	rec("read");
	return (t_rx_i < t_rx_n) ? t_rx[t_rx_i++] : -1;
}

int cli_write(struct cli_instance *sh, const void *data, size_t len)
{
	(void)sh;
	(void)data;
	rec("write");
	return (int)len;
}

void cli_rx_flush(struct cli_instance *sh)
{
	(void)sh;
	rec("flush");
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
	(void)level;
	(void)tag;
	(void)fmt;
	rec("log");
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
	t_getc_seen = io->getc(io->ctx, 10u);
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
	t_getc_seen = io->getc(io->ctx, 10u);
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
}

__attribute__((unused)) static void rx(int byte)
{
	t_rx[t_rx_n++] = byte;
}

#define NOTICE "print:ymodem: sending '"

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
	rx(3);
	xfer_send_source_locked(t_sh, &t_src);
	check("send (held): the core reads YM_IO_ABORT", t_getc_seen == YM_IO_ABORT);

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
		            at("flush|") < at("ym_recv|") && count("log|") >= 1);
	}
	reset();
	t_result = YM_ERR_BUSY;
	ymodem_recv_diag_init(&d);
	xfer_recv_sink_locked(t_sh, &t_sink, &d);
	check("a refusal by the core is said",
	      at("error:ymodem: another transfer is running") > at("ym_recv|"));

	reset();
	check("the held send never claims",
	      xfer_send_source_locked(t_sh, &t_src) == 0 && count("claim|") == 0 &&
	      count("release|") == 0 && at("ym_send|") >= 0 && at(NOTICE) < 0);
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
	check("held send: refused, nothing touched",
	      xfer_send_source_locked(t_sh, &t_src) == 1 &&
	      at("guard:ymodem|") == 0 && at("ym_send|") < 0 && at("flush|") < 0);

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
	check("held send refuses, touching nothing",
	      xfer_send_source_locked(t_sh, &t_src) == 1 && at(say) == 0 &&
	      at("flush|") < 0 && at("ym_send|") < 0);
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
	test_telnet();
#elif defined(XFER_TEST_SHAPE_NOTELNET)
	printf("test_xfer_console (NOTELNET)\n");
	test_send_results();
	test_send_refusals();
	test_ctrl_c();
	test_recv();
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
