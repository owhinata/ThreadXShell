/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared microSD mount glue (shell/cmds/sd_fs_glue.c, issue
 * #233) as the boards compile it, with the REAL FileX media driver
 * (shell/cmds/fx_sd_driver.c) under it and the REAL `sd` command and
 * filesystem bodies (shell/cmds/cmd_sd.c, fs_cmd_core.c) over it, through the
 * REAL dispatcher.  Underneath, sd_fake.c: a card behind the board's table,
 * FileX's open / close / abort (open runs the driver's INIT and boot read as
 * FileX does), the mutexes and the log, recording every call in order and
 * whether the mount lock and the exclusive slot were held at it.
 *
 * Built once per shape of the card table: FULL, and each field left empty
 * (SD_FAKE_SHAPE_NO*).
 *
 * Pinned (nothing typed at a console shows these):
 *   - init creates the two mutexes and runs fx_system_initialize() once,
 *     touching no card;
 *   - the first use mounts once; a mounted media is not opened again;
 *   - a card that stops answering is evicted -- aborted, never closed (no
 *     flush to a card that may be gone), released, then mounted afresh -- but
 *     only when no other operation is inside; with one inside, busy and
 *     nothing thrown away;
 *   - with the exclusive slot held the normal entry is busy and touches no
 *     card; the entry for a holder of the slot mounts and does not evict;
 *   - `sd info` / `sd read` are refused while mounted, the exclusive slot
 *     while an operation is inside;
 *   - an empty slot is "no card" (no hint); a card that is not FAT is an I/O
 *     error with the hint, even right after an empty-slot attempt (the probe
 *     result is not carried over);
 *   - the operations' count goes back to zero;
 *   - with a field of the table empty, both mount entries refuse with
 *     FS_ERR_UNWIRED before the mount lock, FileX or the card, and every
 *     filesystem subcommand says so in one line without the mount hint.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "fs_cmd_core.h"
#include "sd_card_svc.h"
#include "sd_cmd_board.h"
#include "sd_fake.h"

#if defined(SD_FAKE_SHAPE_NOPROBE) || defined(SD_FAKE_SHAPE_NOSTATUS) || \
    defined(SD_FAKE_SHAPE_NOINFO) || defined(SD_FAKE_SHAPE_NOREAD) || \
    defined(SD_FAKE_SHAPE_NOWRITE) || defined(SD_FAKE_SHAPE_NORELEASE)
#define SHAPE_EMPTY_FIELD 1
#if defined(SD_FAKE_SHAPE_NOPROBE)
#define SHAPE_NAME "NOPROBE"
#elif defined(SD_FAKE_SHAPE_NOSTATUS)
#define SHAPE_NAME "NOSTATUS"
#elif defined(SD_FAKE_SHAPE_NOINFO)
#define SHAPE_NAME "NOINFO"
#elif defined(SD_FAKE_SHAPE_NOREAD)
#define SHAPE_NAME "NOREAD"
#elif defined(SD_FAKE_SHAPE_NOWRITE)
#define SHAPE_NAME "NOWRITE"
#else
#define SHAPE_NAME "NORELEASE"
#endif
#else
#define SHAPE_EMPTY_FIELD 0
#endif

static int failures;

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

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
	sd_fake_reset();
	sd_fake_put_vbr(0, 62333952u);     /* a FAT superfloppy */
}

static void run(const char *line)
{
	cli_dummy_clear_output(&tr0);
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

__attribute__((unused)) static int out_count(const char *needle)
{
	int n = 0;
	const char *p = cli_dummy_output_str(&tr0);

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		printf("       calls were: \"%s\"\n", sd_fake_calls());
		printf("       log was   : \"%s\"\n", sd_fake_log());
		failures++;
	}
}

/* The record from @p from on, so a check sees only what came after it. */
__attribute__((unused)) static int at_after(int from, const char *what)
{
	const char *p = strstr(sd_fake_calls() + from, what);

	return p ? (int)(p - sd_fake_calls()) : -1;
}

/* Mount through the normal entry and forget the record. */
__attribute__((unused)) static void mount_now(void)
{
	FX_MEDIA *m = NULL;

	(void)sd_media_acquire(&m);
}

__attribute__((unused)) static int lock_balanced(void)
{
	return sd_fake.lock_gets == sd_fake.lock_puts && !sd_fake.lock_held &&
	       sd_fake.lock_nested == 0 && sd_fake.dir_gets == sd_fake.dir_puts;
}

#if !SHAPE_EMPTY_FIELD
static void t_init(void)
{
	printf("init\n");
	reset();
	sd_fs_glue_init();
	expect("two mutexes and one fx_system_initialize, no card",
	       sd_fake.mutexes_created == 2 && sd_fake.fx_inits == 1 &&
	       sd_fake_at("mutex_create:sd_mount") >= 0 &&
	       sd_fake_at("mutex_create:sd_dir") >= 0 &&
	       sd_fake_at("probe") < 0 && sd_fake_at("status") < 0 &&
	       sd_fake_at("media_open") < 0);
}

static void t_first_use(void)
{
	FX_MEDIA *m = NULL, *m2 = NULL;
	UINT rc, rc2;

	printf("the first use mounts, once\n");
	reset();
	rc = sd_media_acquire(&m);
	expect("mounted: success, the board's media, the mark set",
	       rc == FX_SUCCESS && m == &sd_fs_board_state.media &&
	       sd_is_mounted() && sd_fake.opened);
	expect("  ...nothing asked before the open, the probe inside it, "
	       "under the mount lock",
	       sd_fake_at("media_open[L-]") == 0 && sd_fake_at("status") < 0 &&
	       sd_fake_at("probe[L-]") > sd_fake_at("media_open") &&
	       sd_fake_count("media_open") == 1);
	expect("  ...the log: the layout and the mount",
	       strstr(sd_fake_log(), "fxsd: superfloppy (VBR@0), 62333952 blocks\n") &&
	       strstr(sd_fake_log(), "sdfs: media mounted\n"));
	rc2 = sd_media_acquire(&m2);
	expect("mounted already: the card's state asked, not opened again",
	       rc2 == FX_SUCCESS && m2 == m && sd_fake_count("media_open") == 1 &&
	       sd_fake_count("status") == 1 && sd_fake.double_open == 0);
	expect("the lock given back on every way out", lock_balanced());
}

static void t_evict(void)
{
	FX_MEDIA *m = NULL;
	UINT rc;
	int from;

	printf("a card that stopped answering: evicted when alone\n");
	reset();
	mount_now();
	(void)sd_op_begin();                 /* this caller's own operation */
	from = (int)strlen(sd_fake_calls());
	sd_fake.status_rc = SD_ERR_TIMEOUT;  /* swapped: the old RCA is silent */
	rc = sd_media_acquire(&m);
	expect("aborted (no flush), released, mounted afresh",
	       rc == FX_SUCCESS && m == &sd_fs_board_state.media &&
	       at_after(from, "status") >= 0 &&
	       at_after(from, "status") < at_after(from, "media_abort") &&
	       at_after(from, "media_abort") < at_after(from, "release") &&
	       at_after(from, "release") < at_after(from, "media_open") &&
	       sd_fake_at("media_close") < 0 && sd_is_mounted());
	expect("  ...all of it under the mount lock",
	       at_after(from, "media_abort[L-]") >= 0 &&
	       at_after(from, "release[L-]") >= 0 && at_after(from, "media_open[L-]") >= 0);
	expect("  ...and it says so in the log",
	       strstr(sd_fake_log(), "sdfs: card removed/changed; media evicted\n") !=
	       NULL);
	sd_op_end();

	reset();
	mount_now();
	(void)sd_op_begin();
	from = (int)strlen(sd_fake_calls());
	sd_fake.present = 0;                 /* pulled out while mounted */
	rc = sd_media_acquire(&m);
	expect("a card pulled out: evicted, then the remount says no card",
	       rc == FS_ERR_NO_CARD && at_after(from, "media_abort") >= 0 &&
	       at_after(from, "release") >= 0 && at_after(from, "media_open") >= 0 &&
	       sd_fake_at("media_close") < 0 && !sd_is_mounted());
	sd_op_end();
	expect("the lock given back on every way out", lock_balanced());

	printf("a card that stopped answering, another operation inside\n");
	reset();
	mount_now();
	(void)sd_op_begin();                 /* the other console's */
	(void)sd_op_begin();                 /* this caller's */
	from = (int)strlen(sd_fake_calls());
	sd_fake.status_rc = SD_ERR_TIMEOUT;
	rc = sd_media_acquire(&m);
	expect("busy, and nothing thrown away",
	       rc == FS_ERR_BUSY && at_after(from, "status") >= 0 &&
	       sd_fake_at("media_abort") < 0 && sd_fake_at("release") < 0 &&
	       sd_fake_at("media_close") < 0 && sd_fake_count("media_open") == 1 &&
	       sd_is_mounted() && sd_fake.opened);
	sd_op_end();
	sd_op_end();
	expect("the lock given back on every way out", lock_balanced());
}

static void t_owner(void)
{
	FX_MEDIA *m = NULL;
	UINT rc;
	int from;

	printf("the exclusive slot held\n");
	reset();
	expect("taken", sd_exclusive_begin() == FX_SUCCESS && sd_is_busy());
	rc = sd_media_acquire(&m);
	expect("the normal entry: busy, no card call, no open",
	       rc == FS_ERR_BUSY && m == NULL && sd_fake_at("status") < 0 &&
	       sd_fake_at("probe") < 0 && sd_fake_at("media_open") < 0);
	rc = sd_media_mount_exclusive(&m);
	expect("the holder's entry mounts, with the slot held",
	       rc == FX_SUCCESS && m == &sd_fs_board_state.media &&
	       sd_fake_at("media_open[LB]") >= 0 && sd_fake_at("status") < 0);
	from = (int)strlen(sd_fake_calls());
	sd_fake.status_rc = SD_ERR_TIMEOUT;
	rc = sd_media_mount_exclusive(&m);
	expect("mounted already: no eviction (no state asked, nothing aborted)",
	       rc == FX_SUCCESS && at_after(from, "status") < 0 &&
	       sd_fake_at("media_abort") < 0 && sd_fake_at("release") < 0 &&
	       sd_fake_count("media_open") == 1);
	sd_exclusive_end();
	expect("given back", !sd_is_busy() && lock_balanced());

	reset();
	sd_fake.present = 0;
	(void)sd_exclusive_begin();
	rc = sd_media_mount_exclusive(&m);
	expect("the holder's entry on an empty slot: no card",
	       rc == FS_ERR_NO_CARD && !sd_is_mounted());
	sd_exclusive_end();

	printf("the slots against each other\n");
	reset();
	mount_now();
	expect("raw refused while mounted", sd_raw_begin() == FS_ERR_BUSY &&
	       !sd_is_busy());
	(void)sd_op_begin();
	expect("exclusive refused while an operation is inside",
	       sd_exclusive_begin() == FS_ERR_BUSY && !sd_is_busy());
	sd_op_end();
	expect("exclusive taken once none is", sd_exclusive_begin() == FX_SUCCESS);
	expect("an operation refused while it is held", sd_op_begin() == FS_ERR_BUSY &&
	       sd_fs_board_state.active_ops == 0);
	expect("a second exclusive refused", sd_exclusive_begin() == FS_ERR_BUSY);
	sd_exclusive_end();
	expect("the operations' count back at zero",
	       sd_fs_board_state.active_ops == 0 && lock_balanced());

	reset();
	mount_now();
	from = (int)strlen(sd_fake_calls());
	run("sd info\r");
	expect("`sd info` while mounted: refused, the card not probed",
	       has("sd: filesystem mounted or busy; run `sd umount` first\r\n") &&
	       at_after(from, "probe") < 0 && at_after(from, "status") < 0 &&
	       !sd_is_busy());
	run("sd read 0\r");
	expect("`sd read` while mounted: refused, nothing read",
	       has("sd: filesystem mounted or busy; run `sd umount` first\r\n") &&
	       at_after(from, "read@") < 0 && !sd_is_busy());
}

static void t_no_card(void)
{
	FX_MEDIA *m = NULL;
	UINT rc;

	printf("an empty slot, then a card that is not FAT\n");
	reset();
	sd_fake.present = 0;
	rc = sd_media_acquire(&m);
	expect("empty: no card, nothing mounted, no status asked first",
	       rc == FS_ERR_NO_CARD && !sd_is_mounted() &&
	       sd_fake_at("status") < 0 && sd_fake_at("media_open") == 0 &&
	       sd_fake.opened == 0);
	run("sd ls\r");
	expect("`sd ls`: one line, no hint",
	       has("sd: mount failed: no card (0xf1)\r\n") &&
	       out_count("sd: ") == 1 && !has("no FAT filesystem?"));

	sd_fake.present = 1;
	memset(sd_fake_block(0), 0, SD_BLOCK_SIZE);   /* signed, but not FAT */
	sd_fake_block(0)[510] = 0x55u;
	sd_fake_block(0)[511] = 0xAAu;
	rc = sd_media_acquire(&m);
	expect("not FAT right after the empty slot: an I/O error, not no card",
	       rc == FX_IO_ERROR && !sd_is_mounted() &&
	       sd_fs_board_state.last_probe_rc == SD_OK);
	run("sd ls\r");
	expect("`sd ls`: the error and the hint",
	       has("sd: mount failed: I/O error (0x90)\r\n") &&
	       has("sd: no FAT filesystem? insert a FAT-formatted card or run "
	           "`sd format yes`\r\n"));
	expect("  ...and the log says why",
	       strstr(sd_fake_log(), "fxsd: no FAT VBR/MBR at LBA0\n") != NULL);

	sd_fake_put_vbr(0, 62333952u);
	rc = sd_media_acquire(&m);
	expect("a FAT card after both: mounted", rc == FX_SUCCESS && sd_is_mounted());
	expect("the operations' count back at zero after the commands",
	       sd_fs_board_state.active_ops == 0 && lock_balanced());
}

static void t_unmount(void)
{
	printf("unmount\n");
	reset();
	expect("not mounted: nothing", sd_media_unmount() == FX_SUCCESS &&
	       sd_fake_at("media_close") < 0);
	mount_now();
	expect("mounted: closed (flushed), not aborted, the mark down",
	       sd_media_unmount() == FX_SUCCESS && sd_fake_at("media_close[L-]") >= 0 &&
	       sd_fake_at("media_abort") < 0 && !sd_is_mounted() &&
	       strstr(sd_fake_log(), "sdfs: media unmounted\n"));
	expect("the lock given back", lock_balanced());
}

static void t_fs_through_glue(void)
{
	printf("the filesystem subcommands through the real glue\n");
	reset();
	run("sd ls\r");
	run("sd mkdir /d\r");
	run("sd write /d/a x\r");
	run("sd cat /d/a\r");
	run("sd rm /d/a\r");
	run("sd df\r");
	expect("mounted once, each body reached FileX, the count back at zero",
	       sd_fake_count("media_open") == 1 && sd_fake_at("fx_dir_first") >= 0 &&
	       sd_fake_at("fx_mkdir") >= 0 && sd_fake_at("fx_fwrite") >= 0 &&
	       sd_fake_at("fx_fread") >= 0 && sd_fake_at("fx_space") >= 0 &&
	       sd_fs_board_state.active_ops == 0 && lock_balanced());
	run("sd umount\r");
	expect("`sd umount`: closed", has("unmounted\r\n") &&
	       sd_fake_count("media_close") == 1 && !sd_is_mounted() && !sd_is_busy());
}
#endif

/* ---- a field of the table left empty --------------------------------------- */

#if SHAPE_EMPTY_FIELD
static void t_unwired(void)
{
	static const char *const fs_lines[] = {
		"sd ls\r", "sd cat /a\r", "sd write /a x\r", "sd rm /a\r",
		"sd mkdir /d\r", "sd df\r",
	};
	static const char *const card_lines[] = {
		"sd info\r", "sd read 0\r", "sd format yes\r",
	};
	const char *say = "sd: mount failed: card driver table has an empty "
	                  "field (0xf2)\r\n";
	FX_MEDIA *m = NULL;
	char what[96];
	unsigned i;

	printf("a field of the card table left empty\n");
	reset();
	expect("the check says so", !sd_card_ops_complete());
	expect("the normal entry: refused, nothing touched at all",
	       sd_media_acquire(&m) == FS_ERR_UNWIRED && m == NULL &&
	       !sd_fake_touched_anything());
	reset();
	(void)sd_exclusive_begin();
	sd_fake_reset();
	sd_fs_board_state.busy = 1;
	expect("the holder's entry: refused, nothing touched at all",
	       sd_media_mount_exclusive(&m) == FS_ERR_UNWIRED && m == NULL &&
	       !sd_fake_touched_anything());

	for (i = 0; i < sizeof fs_lines / sizeof fs_lines[0]; i++) {
		reset();
		run(fs_lines[i]);
		snprintf(what, sizeof what, "`%.*s`: one line, no hint, no FileX or "
		         "card call", (int)strlen(fs_lines[i]) - 1, fs_lines[i]);
		expect(what, has(say) && out_count("sd: ") == 1 &&
		             !has("no FAT filesystem?") && sd_fake_calls()[0] == '\0' &&
		             sd_fs_board_state.active_ops == 0 && lock_balanced());
	}
	for (i = 0; i < sizeof card_lines / sizeof card_lines[0]; i++) {
		reset();
		run(card_lines[i]);
		snprintf(what, sizeof what, "`%.*s`: the command's refusal, nothing "
		         "touched", (int)strlen(card_lines[i]) - 1, card_lines[i]);
		expect(what, has("sd: refused -- this firmware's card driver table "
		                 "(sd_card_board_ops) has an empty field\r\n") &&
		             !sd_fake_touched_anything());
	}
	reset();
	run("sd umount\r");
	expect("`sd umount` with nothing mounted: still answers, no card or "
	       "FileX call",
	       has("not mounted\r\n") &&
	       strcmp(sd_fake_calls(), "slot_released[--]|") == 0);
}
#endif

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
#if !SHAPE_EMPTY_FIELD
	printf("test_sd_fs_glue (FULL)\n");
	t_init();
	t_first_use();
	t_evict();
	t_owner();
	t_no_card();
	t_unmount();
	t_fs_through_glue();
#else
	printf("test_sd_fs_glue (%s)\n", SHAPE_NAME);
	t_unwired();
#endif
	if (failures) {
		printf("test_sd_fs_glue: %d FAILED\n", failures);
		return 1;
	}
	printf("test_sd_fs_glue: all passed\n");
	return 0;
}
