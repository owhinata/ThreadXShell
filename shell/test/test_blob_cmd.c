/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared `blob` command (shell/cmds/cmd_blob.c, issue #194)
 * through the REAL dispatcher and the REAL write coordinator, over a stub store
 * (blob_svc_stub.c), once per shape (blob_cfg/blob_svc_config.h).
 *
 * No gate compares what a command prints, so the sentences are pinned here:
 *  - each store result's refusal, and that the board's own sentence wins;
 *  - each `blob write` outcome's lines, the verify-failed and the
 *    magic-failed wording (#194 step 1's leftovers);
 *  - that a write refused before a slot was chosen says no slot number in the
 *    log, and one that chose a slot says it;
 *  - that a header which could not be read while choosing names its slot and
 *    the store's answer, and that no other early refusal gains a reason;
 *  - that 2-digit slots keep every label's colon in one column;
 *  - that `blob erase` erases only under the reservation and gives it back;
 *  - and, in the NOSTORE / NORECV shapes, that nothing is registered at all.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "blob_err.h"
#include "blob_svc_config.h"
#ifndef BLOB_SVC_HAS_RX_LEVEL
#define BLOB_SVC_HAS_RX_LEVEL 0   /* as shell/cmds/cmd_blob.c takes it */
#endif
#include "blob_svc_stub.h"
#include "log.h"

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;
static int failures;

static void reset(void)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(NULL, NULL);
	blob_stub_reset();
}

static void run(const char *line)
{
	cli_dummy_clear_output(&tr0);
	blob_test_log_clear();
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		printf("       log was: \"%s\"\n", blob_test_log());
		failures++;
	}
}

#if BLOB_SVC_HAS_STORE && BLOB_SVC_HAS_RECEIVE

static int logged(const char *needle)
{
	return strstr(blob_test_log(), needle) != NULL;
}

static void store(unsigned slot, const char *name, uint32_t len, uint32_t crc)
{
	blob_stub.state[slot] = BLOB_VALID;
	strcpy(blob_stub.name[slot], name);
	blob_stub.length[slot] = len;
	blob_stub.crc[slot] = crc;
}

/* The column of the first ':' on the line that starts with @p prefix. */
static int colon_col(const char *prefix)
{
	const char *out = cli_dummy_output_str(&tr0), *p = strstr(out, prefix), *c;

	if (p == NULL)
		return -1;
	c = strchr(p, ':');
	return c ? (int)(c - p) : -1;
}

/* ---- refusals -------------------------------------------------------------- */

static void t_refusals(void)
{
	static const struct { int rc; const char *line; } r[] = {
		{ BLOB_ERR_IO,    "blob: TESTNOR read/program/erase failed\r\n" },
		{ BLOB_ERR_STATE, "blob: TESTNOR device down\r\n" },
		{ BLOB_ERR_BUSY,  "blob: another blob operation is running\r\n" },
		{ BLOB_ERR_FAULT, "blob: the TESTNOR port is faulted (reset "
		                  "required)\r\n" },
		{ BLOB_ERR_EMPTY, "blob: that slot holds no header to check "
		                  "against\r\n" },
		{ -77,            "blob: failed (-77)\r\n" },
	};
	char what[96];
	unsigned i;

	for (i = 0u; i < sizeof r / sizeof r[0]; i++) {
		reset();
		blob_stub.stat_rc = r[i].rc;
		run("blob info 3\r");
		snprintf(what, sizeof what, "a stat answering %d says its sentence",
		         r[i].rc);
		expect(what, has(r[i].line));
	}

	/* [!] BUSY AND FAULT ARE DIFFERENT INSTRUCTIONS: one sentence each, and
	 * neither borrowed by the other. */
	reset();
	blob_stub.stat_rc = BLOB_ERR_FAULT;
	run("blob list\r");
	expect("a faulted part is not called busy",
	       has("faulted") && !has("running"));
	reset();
	blob_stub.stat_rc = BLOB_ERR_BUSY;
	run("blob list\r");
	expect("a busy part is not called faulted",
	       has("running") && !has("faulted"));

	/* The board's own sentence wins where it has one. */
	reset();
	blob_stub.stat_rc = BLOB_ERR_BUSY;
	blob_stub.say_rc = BLOB_ERR_BUSY;
	run("blob info 3\r");
	expect("the board's sentence replaces the shared one",
	       has("the board's own sentence for -4") &&
	       !has("another blob operation"));

	reset();
	run("blob info 13\r");
	expect("a slot past the table is refused",
	       has("blob: no such slot (0..12), or a range outside the payload"));
	reset();
	run("blob info 4294967295\r");
	expect("2^32-1 is not a slot", has("no such slot (0..12)"));
}

/* ---- listing, the columns -------------------------------------------------- */

static void t_list(void)
{
	reset();
	store(3u, "model", 3000u, 0x12345678u);
	store(12u, "late", 100u, 0x0000ABCDu);
	run("blob list\r");
	expect("the header", has("slot  base        size  state         length"
	                         "     crc32  name\r\n"));
	expect("a 1-digit row",
	       has("   3  0x400000    512K  valid           3000  12345678  "
	           "model\r\n"));
	expect("a 2-digit row",
	       has("  12  0x7c0000    256K  valid            100  0000ABCD  "
	           "late\r\n"));
	expect("an empty row", has("   0  0x100000   1024K  empty             --"
	                           "        --  --\r\n"));
	expect("the count", has("2 of 13 slots hold a blob\r\n"));
	expect("the board's note", has("[!] test note under the list\r\n"));

	reset();
	run("blob free\r");
	expect("free counts every empty slot",
	       has("free     : 13 slot(s), largest payload 1044480 B\r\n"));
	expect("nothing is uncarved",
	       has("uncarved : 0 B of 0x100000 .. 0x800000 named by no slot"));
}

/* [!] THE COLON OF EVERY LABEL IN ONE COLUMN, 2-digit slots too. */
static void t_columns(void)
{
	int a, b, c;

	reset();
	store(3u, "model", 3000u, 0x12345678u);
	blob_stub.verify_crc = 0x12345678u;
	run("blob verify 3\r");
	expect("a 1-digit verify",
	       has("slot 3   : PASS, 3000 B, crc32 12345678\r\n"));
	a = colon_col("slot 3");

	reset();
	store(12u, "late", 100u, 0x0000ABCDu);
	blob_stub.verify_crc = 0x0000ABCDu;
	run("blob verify 12\r");
	expect("a 2-digit verify", has("slot 12  : PASS, 100 B, crc32 0000ABCD"));
	b = colon_col("slot 12");

	reset();
	run("blob info 12\r");
	c = colon_col("slot     :");
	expect("the colon is in one column (1 digit, 2 digits, `info`)",
	       a == 9 && b == 9 && c == 9);

	reset();
	store(12u, "late", 100u, 0x0000ABCDu);
	blob_stub.verify_crc = 0x1u;
	run("blob verify 12\r");
	expect("a 2-digit verify failure",
	       has("slot 12  : FAIL, read back 00000001, header says 0000ABCD"));
}

/* ---- write ----------------------------------------------------------------- */

static void t_write(void)
{
	reset();
	run("blob write model 12\r");
	expect("a stored write says so",
	       has("stored   : 'model' in slot 12, 3000 B, crc32 ") &&
	       has("verify   : PASS (read back "));
	expect("the announcement keeps the colon column for slot 12",
	       has("slot 12  : 0x7c0000, 256 KB -- erasing it first, so "
	           "whatever is there\r\n") &&
	       has("           is gone even if the transfer fails.  About 4 s "
	           "(at most 8 s);\r\n") &&
	       has("           Ctrl+C stops it between test units.  The file may "
	           "be up to 258048 B\r\n") &&
	       has("           then start the sender: `test-send <file>`,\r\n"));
	expect("the counters and the drops",
	       has("ymodem   : 3 blocks ok, 0 bad-crc") &&
	       has("rx drops : 0") && has("  (clean)\r\n"));
	expect("the log names the slot",
	       logged("blob: write 'model' slot 12: stored, 262144 B erased, "
	              "3000 B received"));
	expect("reserved and given back once",
	       blob_stub.reserves == 1u && blob_stub.unreserves == 1u);
	expect("the receive got a record set to \"nothing received\" (#230)",
	       blob_stub.diag_was_init);

	/* [!] A RECEIVE THAT STOPS BEFORE THE PROTOCOL RUNS (issue #230): the
	 * line after the transfer reads this write's record as the command set
	 * it -- nothing received -- not an earlier transfer's counts. */
	reset();
	blob_stub.recv_early = 1;
	run("blob write model 12\r");
	expect("a receive stopped before the protocol prints zero counters",
	       has("ymodem   : 0 blocks ok, 0 bad-crc, 0 bad-seq, 0 short-read, "
	           "0 header-timeouts\r\n") && blob_stub.diag_was_init &&
	       blob_stub.receives == 1u && !has("stored   :"));

	/* [!] REFUSED BEFORE A SLOT WAS CHOSEN: no slot number anywhere. */
	reset();
	run("blob write newname\r");
	expect("a new name with no slot is refused",
	       has("blob: a new name needs a slot") ||
	       has("say which one: `blob write newname <slot>`"));
	expect("its log line names no slot",
	       logged("blob: write 'newname': ") && logged("no slot chosen") &&
	       !logged("slot 0"));

	reset();
	blob_stub.reserve_rc = BLOB_ERR_BUSY;
	run("blob write model 3\r");
	expect("a refused reservation is busy, with no slot in the log",
	       has("blob: another blob operation is running") &&
	       logged("no slot chosen") && !logged("slot 0"));

	reset();
	blob_stub.reserve_rc = BLOB_ERR_FAULT;
	run("blob write model 3\r");
	expect("a reservation refused on a faulted port says faulted",
	       has("faulted") && !has("running"));

	/* The verify disagreed: the magic was never written. */
	reset();
	blob_stub.corrupt_read_back = 1;
	run("blob write model 5\r");
	expect("a failed verify says the slot is incomplete, not valid",
	       has("blob: verify FAIL -- read back ") &&
	       has("Slot 5 is left\r\n      incomplete (not valid); `blob write "
	           "model 5` sends it again\r\n"));

	/* The magic page refused: the state is not known. */
	reset();
	blob_stub.fail_magic = 1;
	run("blob write model 5\r");
	expect("a failed magic says the state is not known",
	       has("blob: the last header page did not take -- slot 5 may read "
	           "valid, invalid or\r\n      incomplete; `blob list` says "
	           "which (the payload was read back and matched)\r\n"));

	/* [!] A HEADER THAT COULD NOT BE READ WHILE CHOOSING (#194): the write
	 * ends refused before any slot is chosen.  Slot 4 fails, after 0..3 were
	 * read, so the number printed is the one that failed and not slot 0.
	 * Two answers: one only the shared sentence has, one the board speaks
	 * for. */
	reset();
	blob_stub.stat_reserved_rc = BLOB_ERR_IO;
	blob_stub.stat_reserved_from = 4u;
	run("blob write model 5\r");
	expect("an unreadable header names its slot and the store's answer",
	       has("blob: refused -- the header of slot 4 could not be read; "
	           "nothing was erased\r\n"
	           "blob: TESTNOR read/program/erase failed\r\n"));
	expect("... with no generic counters line",
	       !has("declared") && !has("ymodem   :"));
	expect("... its log line has the slot, the answer and no slot chosen",
	       logged("blob: write 'model': refused, slot 4's header could not "
	              "be read (store answered -2), no slot chosen"));
	expect("... nothing erased, reserved and given back once",
	       blob_stub.erase_spans == 0u && blob_stub.reserves == 1u &&
	       blob_stub.unreserves == 1u);

	reset();
	blob_stub.stat_reserved_rc = BLOB_ERR_STATE;
	blob_stub.stat_reserved_from = 0u;
	blob_stub.say_rc = BLOB_ERR_STATE;
	run("blob write model 5\r");
	expect("a driver-down answer on slot 0 is the board's sentence",
	       has("blob: refused -- the header of slot 0 could not be read; "
	           "nothing was erased\r\n"
	           "blob: the board's own sentence for -3\r\n") &&
	       !has("TESTNOR device down") &&
	       logged("slot 0's header could not be read (store answered -3)") &&
	       blob_stub.unreserves == 1u);

	/* Any other refusal before the choice keeps the line it had: a staging
	 * buffer the coordinator refuses says no slot and no reason. */
	reset();
	blob_stub.stage_len = 1u;
	run("blob write model 5\r");
	expect("another refusal before the choice prints what it did",
	       has("blob: refused (0 B erased, 0 B received of 0 declared, 0 tx)"
	           "\r\n") && !has("header of slot") &&
	       logged("blob: write 'model': refused, no slot chosen") &&
	       !logged("could not be read") && blob_stub.reserves == 0u);

	reset();
	blob_stub.raw_refuse = 1;
	run("blob write model 5\r");
	expect("a console that may not carry it is refused before anything",
	       has("test: not on this console") && blob_stub.reserves == 0u);

	reset();
	blob_stub.ready = 0;
	run("blob write model 5\r");
	expect("a part that never came up is refused before anything",
	       has("blob: TESTNOR device down") && blob_stub.reserves == 0u);

	reset();
	store(5u, "other", 10u, 1u);
	run("blob write model 5\r");
	expect("an occupied slot is refused, with the way out",
	       has("`blob erase <slot>` first, on purpose (`blob list` shows "
	           "what is where)") && blob_stub.erase_spans == 0u);

	/* [!] A CONSOLE AT ITS CLAIM LIMIT (issue #229) cannot take the claim the
	 * receive needs.  The question before the erase is the claim's own, so
	 * it is refused before anything -- not after a slot was destroyed -- and
	 * the lock is left as it was.  The count is placed there, not reached. */
	reset();
	sh0.console_claims = UINT16_MAX;
	{
		unsigned g0 = cli_test_lock_gets(), p0 = cli_test_lock_puts();

		run("blob write model 5\r");
		expect("a console at its claim limit is refused before the erase",
		       blob_stub.reserves == 0u && blob_stub.erase_spans == 0u &&
		       blob_stub.erase_headers == 0u && blob_stub.receives == 0u &&
		       blob_stub.programs == 0u && blob_stub.state[5] == BLOB_EMPTY);
		expect("... not as a background job",
		       !has("cannot run in the background"));
		expect("... the count is left at the limit",
		       sh0.console_claims == UINT16_MAX);
		expect("... and every lock taken was given back",
		       cli_test_lock_gets() - g0 == cli_test_lock_puts() - p0);
	}
	sh0.console_claims = 0u;
}

/*
 * [!] A NAME THAT STARTS WITH '-' (issue #196): refused by the name rule
 * before anything is taken -- no reservation, no store call, no erase, no
 * receive -- with the new reason and the rule in brackets.  And a `-x` that
 * is already stored (written before the rule) is still listed, still guards
 * its slot against another name, and goes with `blob erase <slot>`.
 */
static void t_dash_name(void)
{
	static const char *const bad[] = {
		"blob write -x 3", "blob write - 3", "blob write --name 3",
	};
	char line[32];
	unsigned i;

	for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		reset();
		store(3u, "keep", 100u, 0x11111111u);
		snprintf(line, sizeof line, "%s\r", bad[i]);
		run(line);
		expect(bad[i], has("blob: name starts with '-' (printable ASCII, "
		                   "no space, not starting with '-', at most 64 "
		                   "characters)\r\n") &&
		       blob_stub.reserves == 0u && blob_stub.store_calls == 0u &&
		       blob_stub.erase_spans == 0u &&
		       blob_stub.erase_headers == 0u &&
		       blob_stub.receives == 0u &&
		       strcmp(blob_stub.name[3], "keep") == 0);
	}

	/* [!] AND INTO AN EMPTY SLOT, where nothing else would refuse it:
	 * blob_write_run() does not look at the name before it erases, so the
	 * check in cmd_blob.c is the only thing in front of the erase.  Judged by
	 * the counts, not by the sentence. */
	reset();
	run("blob write -x 5\r");
	expect("`blob write -x 5` into an empty slot erases and receives nothing",
	       blob_stub.reserves == 0u && blob_stub.erase_spans == 0u &&
	       blob_stub.erase_headers == 0u && blob_stub.receives == 0u &&
	       blob_stub.programs == 0u && blob_stub.state[5] == BLOB_EMPTY);

	/* A '-' that is not the first character is an ordinary name. */
	reset();
	run("blob write a-b 12\r");
	expect("`a-b` is stored", has("stored   : 'a-b' in slot 12"));
	reset();
	run("blob write x- 12\r");
	expect("`x-` is stored", has("stored   : 'x-' in slot 12"));

	/* What is already stored is read as before. */
	reset();
	store(3u, "-x", 3000u, 0x12345678u);
	run("blob list\r");
	expect("a stored `-x` is listed as valid",
	       has("   3  0x400000    512K  valid           3000  12345678  "
	           "-x\r\n"));
	run("blob write other 3\r");
	expect("and it still guards its slot",
	       has("that slot holds something else") &&
	       blob_stub.erase_spans == 0u);
	run("blob erase 3\r");
	expect("and `blob erase <slot>` removes it",
	       has("erased   : the header is gone") &&
	       blob_stub.erase_headers == 1u);
}

/* ---- erase ----------------------------------------------------------------- */

static void t_erase(void)
{
	reset();
	store(4u, "x", 10u, 1u);
	run("blob erase 4\r");
	expect("an erase erases the header under the reservation and says so",
	       has("erasing  : slot 4 header at 0x480000 (one test header unit)"
	           "\r\n") &&
	       has("erased   : the header is gone") &&
	       blob_stub.erase_headers == 1u && blob_stub.reserves == 1u &&
	       blob_stub.unreserves == 1u);

	reset();
	blob_stub.reserve_rc = BLOB_ERR_BUSY;
	run("blob erase 4\r");
	expect("a refused reservation erases nothing and gives nothing back",
	       has("another blob operation is running") &&
	       blob_stub.erase_headers == 0u && blob_stub.unreserves == 0u);

	reset();
	blob_stub.erase_hdr_rc = BLOB_ERR_IO;
	run("blob erase 4\r");
	expect("a failed erase still gives the reservation back",
	       has("blob: TESTNOR read/program/erase failed") &&
	       blob_stub.reserves == 1u && blob_stub.unreserves == 1u);
}

/* ---- the drops line (BLOB_SVC_HAS_RX_LEVEL) -------------------------------- */

static void t_rx_line(void)
{
#if BLOB_SVC_HAS_RX_LEVEL
	reset();
	blob_stub.level_high = 1234u;
	blob_stub.level_size = 4096u;
	run("blob write model 5\r");
	expect("the high-water is restarted once, before the erase",
	       blob_stub.level_clears == 1u && blob_stub.erases_at_clear == 0u &&
	       blob_stub.erase_spans == 1u);
	expect("a clean transfer: drops, high-water and ring on one line",
	       has("rx drops : 0, ring high-water 1234 of 4096 B  (clean)\r\n"));

	reset();
	blob_stub.drops = 2u;
	blob_stub.level_high = 4095u;
	blob_stub.level_size = 4096u;
	run("blob write model 5\r");
	expect("a transfer that dropped says NOT CLEAN",
	       has("rx drops : 2, ring high-water 4095 of 4096 B  <-- NOT CLEAN"
	           "\r\n") && !has("(clean)"));

	reset();
	blob_stub.level_rc = -1;
	run("blob write model 5\r");
	expect("a console with no ring gets the plain drops line",
	       has("rx drops : 0  (clean)\r\n") && !has("high-water"));

	/* Refused before the reservation: nothing measured, nothing to say. */
	reset();
	blob_stub.raw_refuse = 1;
	run("blob write model 5\r");
	expect("a refused write restarts nothing",
	       blob_stub.level_clears == 0u && !has("rx drops"));
#else
	reset();
	blob_stub.drops = 1u;
	run("blob write model 5\r");
	expect("without the property: the plain line, no ring asked",
	       has("rx drops : 1  <-- NOT CLEAN\r\n") &&
	       blob_stub.level_clears == 0u && blob_stub.level_reads == 0u);
#endif
}

static void t_read(void)
{
	reset();
	run("blob read 3 0 600\r");
	expect("a read past BLOB_SVC_READ_MAX is refused",
	       has("blob: length must be 1..512"));
	/* [!] Slot 12's payload is 258,048 B (0x3f000).  From 258,040 a 64 B
	 * read is cut to the 8 B that are there: the line at 0x3eff8 and
	 * nothing after it.  The stub's read refuses a range past the payload
	 * as a real store does, so a command that did not cut prints a refusal
	 * instead -- and the bytes handed out are counted.  (Until #194's
	 * review this looked for a "0003f000" line, which a hexdump starting
	 * at 0x3eff8 never prints whether or not it was cut.) */
	reset();
	run("blob read 12 258040 64\r");
	expect("a read is cut at the end of the payload",
	       has("0003eff8") && !has("0003f008") && !has("blob: ") &&
	       blob_stub.read_bytes == 8u);
	reset();
	run("blob read 12 258048 1\r");
	expect("a read that starts at the end is refused, nothing read",
	       has("no such slot (0..12), or a range outside the payload") &&
	       blob_stub.read_bytes == 0u);
}

/* ---- slot numbers past the table ------------------------------------------ */

/* [!] 4294967295 must not become -1 ("no slot given") in `blob write`, where
 * a stored name with no slot means "reuse its slot" -- the slot that would
 * then be erased.  Every subcommand that takes a slot refuses the count
 * itself and 2^32-1 in the parse, before the store is asked anything and
 * before the reservation. */
static void t_slot_bounds(void)
{
	static const char *const lines[] = {
		"blob write model 4294967295\r", "blob write model 13\r",
		"blob erase 4294967295\r", "blob erase 13\r",
		"blob verify 4294967295\r", "blob verify 13\r",
		"blob info 4294967295\r", "blob info 13\r",
		"blob read 4294967295\r", "blob read 13\r",
	};
	unsigned i;
	char what[96];

	for (i = 0u; i < sizeof lines / sizeof lines[0]; i++) {
		reset();
		store(3, "model", 3000u, 0x12345678u);
		run(lines[i]);
		snprintf(what, sizeof what, "refused in the parse: %.*s",
		         (int)strlen(lines[i]) - 1, lines[i]);
		expect(what, has("blob: no such slot (0..12)") &&
		       blob_stub.store_calls == 0u && blob_stub.reserves == 0u &&
		       blob_stub.erase_spans == 0u &&
		       blob_stub.erase_headers == 0u &&
		       blob_stub.state[3] == BLOB_VALID);
	}
	/* The last slot is still a slot, and its header is named in the
	 * board's word for the unit (BLOB_SVC_HEADER_UNIT, issue #194). */
	reset();
	run("blob info 12\r");
	expect("the last slot is taken", has("slot     : 12\r\n"));
	expect("the header unit is the board's word",
	       has("header   : 0x7c0000 .. 0x7c1000 (one test header unit)\r\n"));
}

#endif

int main(void)
{
#if BLOB_SVC_HAS_STORE && BLOB_SVC_HAS_RECEIVE
	printf("test_blob_cmd (full%s):\n",
	       BLOB_SVC_HAS_RX_LEVEL ? ", with the RX ring level" : "");
	t_refusals();
	t_list();
	t_columns();
	t_write();
	t_dash_name();
	t_erase();
	t_read();
	t_slot_bounds();
	t_rx_line();
#else
	printf("test_blob_cmd (%s):\n",
	       BLOB_SVC_HAS_STORE ? "store, no receive" : "no store");
	reset();
	run("blob list\r");
	expect("`blob` is not registered", has("blob: command not found"));
#endif
	if (failures != 0) {
		printf("test_blob_cmd: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_cmd: all passed\n");
	return 0;
}
