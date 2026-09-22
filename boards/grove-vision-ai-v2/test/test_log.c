/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for svc/log.c -- the RAM log ring -- and its two ways in (#112).
 *
 * The REAL log.c is compiled here, by #include, so that the ring (a static)
 * can be read back directly and compared byte for byte.  The device header it
 * includes is log_shim/WE2_device.h, which traces the PRIMASK accessors and the
 * barriers; log.c's own LOG_PERSIST_TAP (compiled in by -DLOG_HOST_TEST, out
 * of the firmware) traces the read-backs.  So what is asserted is not only
 * what the ring holds afterwards but the ORDER the append does things in.
 *
 * [!] EVERY RING PROPERTY IS CHECKED THROUGH BOTH WAYS IN.  log_write() formats
 * and log_write_bytes() does not; they are supposed to end in one append.  A
 * property checked through only one of them would stay green if the other grew
 * its own, subtly different, copy -- so each case runs once per writer, and the
 * two are also required to lay down byte-identical records for the same text.
 *
 * This file is shared in shape with wio-lite-ai's test_log.c; the log.c they
 * test are separate files (a board port each), so the tests are too.
 */
#include "log.c"

#include <stdio.h>
#include <stdlib.h>

/* ---- what the board supplies on hardware --------------------------------- */

unsigned long _tx_timer_system_clock;       /* log.c reads it for ts_ms */
uint32_t      log_test_primask;

/* ---- the trace ------------------------------------------------------------ */

struct ev {
	char     kind;          /* I E M S (shim), P (read-back) */
	uint32_t primask;       /* PRIMASK as the event saw it */
	uint32_t head, seq;
	const void *addr;       /* P only */
	uint32_t len;           /* P only */
};

static struct ev evs[64];
static unsigned  nev;

static void trace_reset(void)
{
	nev = 0u;
}

void log_test_event(char kind)
{
	if (nev < sizeof evs / sizeof evs[0]) {
		evs[nev].kind    = kind;
		evs[nev].primask = log_test_primask;
		evs[nev].head    = g_log.head;
		evs[nev].seq     = g_log.seq;
		evs[nev].addr    = NULL;
		evs[nev].len     = 0u;
	}
	nev++;
}

void log_test_persist(const void *addr, uint32_t len)
{
	log_test_event('P');
	if (nev <= sizeof evs / sizeof evs[0]) {
		evs[nev - 1u].addr = addr;
		evs[nev - 1u].len  = len;
	}
}

/* ---- harness -------------------------------------------------------------- */

static int failures;

static void check(const char *what, int ok)
{
	if (!ok) {
		printf("  FAIL %s\n", what);
		failures++;
	}
}

/* A fresh, empty, ready ring: invalid magic -> log_init() rebuilds it and
 * writes its boot marker, which log_clear() then drops. */
static void fresh(void)
{
	memset(&g_log, 0x5A, sizeof g_log);
	log_ready = 0u;
	log_test_primask = 0u;
	log_init();
	log_clear();
	log_set_level(LOG_LEVEL_DBG);
}

/* The two ways in, as one signature, so that each case runs through both. */
typedef void (*writer_t)(unsigned level, const char *tag, const char *text);

static void w_fmt(unsigned level, const char *tag, const char *text)
{
	log_write(level, tag, "%s", text);
}

static void w_bytes(unsigned level, const char *tag, const char *text)
{
	log_write_bytes(level, tag, NULL, text, strlen(text), NULL);
}

static const struct {
	const char *name;
	writer_t    w;
} writers[] = {
	{ "log_write", w_fmt },
	{ "log_write_bytes", w_bytes },
};

/* Every record present, oldest first, into @p out (max @p cap); returns how
 * many. */
static unsigned drain(struct log_record *out, unsigned cap)
{
	struct log_iter it;
	unsigned n = 0u;
	log_iter_start(&it);
	while (n < cap && log_iter_next(&it, &out[n]))
		n++;
	return n;
}

static struct log_record recs[1024];

/* ---- cases ---------------------------------------------------------------- */

static void case_roundtrip(const char *wn, writer_t w)
{
	char what[128];
	fresh();
	uint32_t seq0 = g_log.seq;
	w(LOG_LEVEL_WRN, "tagtagtagX", "hello");
	w(LOG_LEVEL_INF, "t2", "");
	unsigned n = drain(recs, 8);
	snprintf(what, sizeof what, "%s: two records round-trip", wn);
	check(what, n == 2u &&
	            strcmp(recs[0].text, "hello") == 0 &&
	            recs[0].level == LOG_LEVEL_WRN &&
	            strcmp(recs[0].tag, "tagtagta") == 0 &&   /* cut to 8 */
	            recs[0].seq == seq0 &&
	            strcmp(recs[1].text, "") == 0 &&
	            recs[1].seq == seq0 + 1u);
}

/* The two ways in lay down byte-identical records for the same text (bar the
 * sequence number, which each record takes in turn). */
static void case_same_bytes(void)
{
	static const char *texts[] = {
		"", "a", "abc", "abcd", "exactly-sixteen!",
		"0123456789012345678901234567890123456789012345678901234567890123"
		"4567890123456789012345678901234567890123",           /* 104 */
	};
	for (unsigned t = 0u; t < sizeof texts / sizeof texts[0]; t++) {
		uint8_t a[LOG_REC_MAX], b[LOG_REC_MAX];
		uint32_t la, lb;

		fresh();
		uint32_t h0 = g_log.head;
		w_fmt(LOG_LEVEL_INF, "same", texts[t]);
		la = g_log.head - h0;
		ring_get(h0, a, la);

		fresh();
		h0 = g_log.head;
		w_bytes(LOG_LEVEL_INF, "same", texts[t]);
		lb = g_log.head - h0;
		ring_get(h0, b, lb);

		char what[128];
		snprintf(what, sizeof what,
		         "log_write and log_write_bytes write the same record "
		         "(text length %zu)", strlen(texts[t]));
		check(what, la == lb && la >= LOG_REC_MIN &&
		            memcmp(a, b, la) == 0);
	}
}

/* Level: above the threshold nothing is written; above DBG clamps to DBG. */
static void case_level(const char *wn, writer_t w)
{
	char what[128];
	fresh();
	log_set_level(LOG_LEVEL_WRN);
	uint32_t h0 = g_log.head;
	w(LOG_LEVEL_INF, "lvl", "dropped");
	w(LOG_LEVEL_DBG, "lvl", "dropped");
	snprintf(what, sizeof what, "%s: a level above the threshold writes "
	         "nothing", wn);
	check(what, g_log.head == h0);
	w(LOG_LEVEL_ERR, "lvl", "kept");
	snprintf(what, sizeof what, "%s: a level at or below it is kept", wn);
	check(what, drain(recs, 4) == 1u && strcmp(recs[0].text, "kept") == 0);

	fresh();
	w(9u, "lvl", "clamped");
	snprintf(what, sizeof what, "%s: a level above DBG is stored as DBG", wn);
	check(what, drain(recs, 4) == 1u && recs[0].level == LOG_LEVEL_DBG);

	fresh();
	log_ready = 0u;
	h0 = g_log.head;
	w(LOG_LEVEL_ERR, "lvl", "before init");
	snprintf(what, sizeof what, "%s: nothing is written before log_init()",
	         wn);
	check(what, g_log.head == h0);
}

/* Wrap: a record that would straddle the physical end goes to offset 0 behind
 * a SKIP that fills the tail fragment; every live record reads back. */
static void case_wrap(const char *wn, writer_t w)
{
	char what[160];
	char txt[32];
	fresh();
	unsigned written = 0u, skipped = 0u;
	/* Write until a record lands behind a SKIP, then a little more. */
	for (unsigned i = 0u; i < 2000u && skipped < 3u; i++) {
		uint32_t o = g_log.head & (LOG_RING_DATA_SIZE - 1u);
		snprintf(txt, sizeof txt, "rec-%u-%s", i, (i & 1u) ? "odd" : "e");
		uint32_t rec = LOG_HDR_SIZE + (((uint32_t)strlen(txt) + 1u + 3u) & ~3u);
		int will_skip = o + rec > LOG_RING_DATA_SIZE;
		w(LOG_LEVEL_INF, "wrap", txt);
		written++;
		if (will_skip) {
			skipped++;
			uint32_t w0;
			memcpy(&w0, &g_log.data[o], sizeof w0);
			snprintf(what, sizeof what, "%s: the tail fragment at %u holds "
			         "a SKIP reaching the end", wn, (unsigned)o);
			check(what, ((w0 >> 16) & 0xFFu) == LOG_LEVEL_SKIP &&
			            ((w0 >> 24) & 0xFFu) == LOG_REC_MAGIC &&
			            o + (w0 & 0xFFFFu) == LOG_RING_DATA_SIZE);
			uint32_t r0;
			memcpy(&r0, &g_log.data[0], sizeof r0);
			snprintf(what, sizeof what, "%s: the wrapped record starts at "
			         "offset 0", wn);
			check(what, (r0 & 0xFFFFu) == rec &&
			            ((r0 >> 16) & 0xFFu) == LOG_LEVEL_INF);
		}
	}
	snprintf(what, sizeof what, "%s: the ring wrapped", wn);
	check(what, skipped == 3u);
	/* Every surviving record reads back, in sequence, and the newest is the
	 * last one written. */
	unsigned n = drain(recs, 1024);
	int in_order = n > 0u;
	for (unsigned k = 1u; k < n; k++)
		in_order = in_order && recs[k].seq == recs[k - 1u].seq + 1u;
	snprintf(txt, sizeof txt, "rec-%u-%s", written - 1u,
	         ((written - 1u) & 1u) ? "odd" : "e");
	snprintf(what, sizeof what, "%s: after wrapping, the records read back "
	         "contiguous and newest last", wn);
	check(what, in_order && strcmp(recs[n - 1u].text, txt) == 0);
}

/* Eviction: the oldest records go, whole, to make room; the survivors are a
 * contiguous tail of what was written. */
static void case_evict(const char *wn, writer_t w)
{
	char what[160];
	char txt[LOG_MSG_MAX + 1];
	fresh();
	const unsigned total = 600u;        /* ~600 * 44 B > 8 KiB several times */
	for (unsigned i = 0u; i < total; i++) {
		snprintf(txt, sizeof txt, "evict-%04u-padding-padding", i);
		w(LOG_LEVEL_INF, "ev", txt);
	}
	unsigned n = drain(recs, 1024);
	int ok = n > 10u && n < total;
	for (unsigned k = 1u; k < n; k++)
		ok = ok && recs[k].seq == recs[k - 1u].seq + 1u;
	snprintf(txt, sizeof txt, "evict-%04u-padding-padding", total - 1u);
	ok = ok && strcmp(recs[n - 1u].text, txt) == 0;
	snprintf(txt, sizeof txt, "evict-%04u-padding-padding", total - n);
	ok = ok && strcmp(recs[0].text, txt) == 0;
	snprintf(what, sizeof what, "%s: old records are evicted whole; the "
	         "survivors are the newest %u, contiguous", wn, n);
	check(what, ok);
	snprintf(what, sizeof what, "%s: the ring never holds more than its "
	         "size", wn);
	check(what, (uint32_t)(g_log.head - g_log.tail) <= g_log.size);
}

/* Maximum length: the text is cut to LOG_MSG_MAX, never beyond. */
static void case_maxlen(const char *wn, writer_t w)
{
	char what[128];
	char big[300];
	memset(big, 'x', sizeof big - 1u);
	big[sizeof big - 1u] = '\0';
	fresh();
	w(LOG_LEVEL_INF, "max", big);
	snprintf(what, sizeof what, "%s: a long text is cut to LOG_MSG_MAX (%u)",
	         wn, (unsigned)LOG_MSG_MAX);
	check(what, drain(recs, 4) == 1u && strlen(recs[0].text) == LOG_MSG_MAX &&
	            g_log.head - g_log.tail == LOG_REC_MAX);
}

/* PRIMASK: every event of the append sees it set, and it is put back exactly
 * as it was -- including when it was already set (a fault handler logging). */
static void case_primask(const char *wn, writer_t w)
{
	char what[160];
	for (uint32_t before = 0u; before <= 1u; before++) {
		fresh();
		log_test_primask = before;
		trace_reset();
		w(LOG_LEVEL_INF, "pm", "masked");
		int inside = 0, ok = 1, sawI = 0, sawE = 0;
		for (unsigned k = 0u; k < nev && k < 64u; k++) {
			if (evs[k].kind == 'I') {
				inside = 1;
				sawI = 1;
				continue;
			}
			if (evs[k].kind == 'E') {
				inside = 0;
				sawE = 1;
				continue;
			}
			/* every barrier and read-back is inside the section */
			ok = ok && inside && evs[k].primask == 1u;
		}
		snprintf(what, sizeof what, "%s: the ring is only touched with "
		         "PRIMASK set (entered with %u)", wn, (unsigned)before);
		check(what, ok && sawI && sawE);
		snprintf(what, sizeof what, "%s: PRIMASK is restored to %u", wn,
		         (unsigned)before);
		check(what, log_test_primask == before);
	}
}

/* Persistence order, no wrap: seq bumped before head (DMB), head committed,
 * DSB, the record's span read back, the header read back LAST, DSB. */
static void case_persist_order(const char *wn, writer_t w)
{
	char what[200];
	fresh();
	uint32_t h0 = g_log.head, s0 = g_log.seq;
	const char *text = "persist-me";
	uint32_t rec = LOG_HDR_SIZE + (((uint32_t)strlen(text) + 1u + 3u) & ~3u);
	trace_reset();
	w(LOG_LEVEL_INF, "po", text);
	/* expected: I, M, S, P(span), P(hdr), S, E */
	int ok = nev == 7u &&
	         evs[0].kind == 'I' &&
	         evs[1].kind == 'M' && evs[1].seq == s0 + 1u && evs[1].head == h0 &&
	         evs[2].kind == 'S' && evs[2].head == h0 + rec &&
	         evs[3].kind == 'P' &&
	         evs[3].addr == &g_log.data[h0 & (LOG_RING_DATA_SIZE - 1u)] &&
	         evs[3].len == rec &&
	         evs[4].kind == 'P' && evs[4].addr == (const void *)&g_log &&
	         evs[4].len == offsetof(struct log_ring, data) &&
	         evs[5].kind == 'S' &&
	         evs[6].kind == 'E';
	snprintf(what, sizeof what, "%s: seq before head, then the body read "
	         "back, then the header last, between two DSBs (%u events)",
	         wn, nev);
	check(what, ok);
}

/* Persistence order across a wrap: the SKIP is committed (DMB, head past it)
 * before the record, and the read-back covers the SKIP fragment and the record
 * -- the span straddles, so it is read back in two pieces -- then the header. */
static void case_persist_wrap(const char *wn, writer_t w)
{
	char what[200];
	fresh();
	const char *text = "wrap-persist";
	uint32_t rec = LOG_HDR_SIZE + (((uint32_t)strlen(text) + 1u + 3u) & ~3u);
	/* Advance head to just short of the end, so the next record straddles. */
	while (((g_log.head & (LOG_RING_DATA_SIZE - 1u)) + rec) <=
	       LOG_RING_DATA_SIZE)
		w(LOG_LEVEL_INF, "fill", "f");
	uint32_t h0 = g_log.head, s0 = g_log.seq;
	uint32_t o = h0 & (LOG_RING_DATA_SIZE - 1u);
	uint32_t skip = LOG_RING_DATA_SIZE - o;
	trace_reset();
	w(LOG_LEVEL_INF, "po", text);
	/* expected: I, M(skip), M(seq), S, P(frag), P(rec at 0), P(hdr), S, E */
	int ok = nev == 9u &&
	         evs[0].kind == 'I' &&
	         evs[1].kind == 'M' && evs[1].head == h0 && evs[1].seq == s0 &&
	         evs[2].kind == 'M' && evs[2].head == h0 + skip &&
	         evs[2].seq == s0 + 1u &&
	         evs[3].kind == 'S' && evs[3].head == h0 + skip + rec &&
	         evs[4].kind == 'P' && evs[4].addr == &g_log.data[o] &&
	         evs[4].len == skip &&
	         evs[5].kind == 'P' && evs[5].addr == &g_log.data[0] &&
	         evs[5].len == rec &&
	         evs[6].kind == 'P' && evs[6].addr == (const void *)&g_log &&
	         evs[7].kind == 'S' &&
	         evs[8].kind == 'E';
	snprintf(what, sizeof what, "%s: across a wrap the SKIP commits first, "
	         "and the straddling span is read back in two pieces before the "
	         "header (%u events)", wn, nev);
	check(what, ok);
}

/* ---- the byte way in only ------------------------------------------------- */

static void case_bytes_only(void)
{
	/* Unterminated: exactly len bytes, whatever follows them. */
	{
		char buf[16];
		memset(buf, 'Z', sizeof buf);
		memcpy(buf, "abcdef", 6);           /* no NUL within 16 */
		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", NULL, buf, 6u, NULL);
		check("bytes: an unterminated run stores exactly its length",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, "abcdef") == 0);
	}
	/* Empty: a record with only the prefix (the adapter drops empty ones;
	 * the append itself writes what it is given). */
	{
		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", "x", 0u, " ...");
		check("bytes: zero bytes -> the prefix alone, no cut marker",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, "plugin: ") == 0);
		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", NULL, 7u, " ...");
		check("bytes: a NULL run is read as empty, never dereferenced",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, "plugin: ") == 0);
		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", NULL, "abc", 3u, NULL);
		check("bytes: a NULL prefix and marker are empty",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, "abc") == 0);
	}
	/* Embedded NUL: the text ends there, as a string would; the cut marker
	 * still follows when the LENGTH was too long. */
	{
		static const char nul5[5] = { 'a', 'b', '\0', 'c', 'd' };
		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", nul5, 5u, " ...");
		check("bytes: an embedded NUL ends the text",
		      drain(recs, 4) == 1u &&
		              strcmp(recs[0].text, "plugin: ab") == 0);
		char longnul[200];
		memset(longnul, 'q', sizeof longnul);
		longnul[2] = '\0';
		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", longnul,
		                sizeof longnul, " ...");
		check("bytes: an embedded NUL in a text too long still shows the "
		      "cut marker",
		      drain(recs, 4) == 1u &&
		              strcmp(recs[0].text, "plugin: qq ...") == 0);
	}
	/* The cut and its marker, at the boundary: prefix 8 + marker 4 in a
	 * LOG_MSG_MAX line. */
	{
		const uint32_t room = LOG_MSG_MAX - 8u;         /* no marker */
		char src[300];
		for (unsigned i = 0u; i < sizeof src; i++)
			src[i] = (char)('A' + i % 26u);
		char want[LOG_MSG_MAX + 1];

		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", src, room, " ...");
		snprintf(want, sizeof want, "plugin: %.*s", (int)room, src);
		check("bytes: a text that exactly fits is kept whole, no marker",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, want) == 0 &&
		              strlen(recs[0].text) == LOG_MSG_MAX);

		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", src, room + 1u,
		                " ...");
		snprintf(want, sizeof want, "plugin: %.*s ...", (int)(room - 4u),
		         src);
		check("bytes: one byte over is cut so that ' ...' still fits",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, want) == 0 &&
		              strlen(recs[0].text) == LOG_MSG_MAX);

		fresh();
		log_write_bytes(LOG_LEVEL_INF, "b", "plugin: ", src,
		                (size_t)-1, " ...");
		check("bytes: a length of SIZE_MAX is cut, not wrapped round",
		      drain(recs, 4) == 1u && strcmp(recs[0].text, want) == 0);
	}
	/* A prefix and marker that alone overflow the line: the APPEND's own cut
	 * is what keeps the record inside LOG_REC_MAX -- the byte way in can hand
	 * it more than LOG_MSG_MAX here. */
	{
		char pre[101], mark[11], src[50];
		memset(pre, 'p', 100);
		pre[100] = '\0';
		memset(mark, 'm', 10);
		mark[10] = '\0';
		memset(src, 's', sizeof src);
		fresh();
		uint32_t h0 = g_log.head;
		log_write_bytes(LOG_LEVEL_INF, "b", pre, src, sizeof src, mark);
		check("bytes: a prefix + marker longer than the line is still cut to "
		      "LOG_MSG_MAX by the append",
		      drain(recs, 4) == 1u && strlen(recs[0].text) == LOG_MSG_MAX &&
		              g_log.head - h0 == LOG_REC_MAX);
	}
}

int main(void)
{
	for (unsigned k = 0u; k < sizeof writers / sizeof writers[0]; k++) {
		const char *wn = writers[k].name;
		writer_t w = writers[k].w;
		case_roundtrip(wn, w);
		case_level(wn, w);
		case_wrap(wn, w);
		case_evict(wn, w);
		case_maxlen(wn, w);
		case_primask(wn, w);
		case_persist_order(wn, w);
		case_persist_wrap(wn, w);
	}
	case_same_bytes();
	case_bytes_only();

	if (failures) {
		printf("test_log: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_log: all cases pass (both ways in)\n");
	return 0;
}
