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
}

/* ---- erase ----------------------------------------------------------------- */

static void t_erase(void)
{
	reset();
	store(4u, "x", 10u, 1u);
	run("blob erase 4\r");
	expect("an erase erases the header under the reservation and says so",
	       has("erasing  : slot 4 header at 0x480000 (one sector)\r\n") &&
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
	reset();
	run("blob read 12 258040 64\r");
	expect("a read is cut at the end of the payload",
	       has("0003eff8") && !has("0003f000"));
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
	t_erase();
	t_read();
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
