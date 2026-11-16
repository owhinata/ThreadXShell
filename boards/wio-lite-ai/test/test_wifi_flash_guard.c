/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #235 -- `wifi flash` on the telnet console.  info / read / backup / write all
 * take the WiFi link down (flash_claim()), which ends the telnet session they were
 * typed on, so each must be refused BEFORE it disarms the re-association or asks the
 * bridge down, and -- with valid arguments, as every case below has -- before it
 * prints anything (write's `confirm` prompt included; a malformed offset is still
 * reported first).
 * imginfo touches no link and must not be refused.  The same four from a console
 * that is not telnet do reach the teardown -- which is what shows the counters below
 * can see it at all.
 *
 * Issue #244 -- the same four as a background job (`... &`) from that other console:
 * the console claim refuses a job, so each must be refused before the disarm and the
 * teardown too, with the one "run in the foreground" line under its own name (write
 * without `confirm` included: asking for `confirm` would be advice the re-run cannot
 * follow).
 *
 * cmds/cmd_wifi_flash.c and src/rtl_link.c are compiled unchanged; wifi_flash_fake.c
 * stands in for what they call.  The handlers are reached through the exported
 * subcommand table.
 */
#include "cli_instance.h"
#include "cmd_wifi_priv.h"

#include <stdio.h>
#include <string.h>

extern struct cli_instance *fk_telnet_sh;
extern int fk_guard_calls, fk_disarm, fk_bridge_down, fk_console_claim, fk_psram, fk_out;
extern const char *fk_refused_as, *fk_fg_refused_as;

static struct cli_instance sh;
static struct cli_instance job = { .fg = &sh };   /* `... &` typed on sh */
static int failures;

static int run_on(struct cli_instance *on, const char *name, int telnet, int argc,
                  char **argv)
{
	const struct cli_cmd *c;

	fk_telnet_sh = telnet ? &sh : NULL;   /* the guard refuses only this console */
	fk_guard_calls = fk_disarm = fk_bridge_down = fk_console_claim = fk_psram = 0;
	fk_out = 0;
	fk_refused_as = fk_fg_refused_as = NULL;
	for (c = wifi_flash_subcmds; c->name != NULL; c++)
		if (strcmp(c->name, name) == 0)
			return c->handler(on, argc, argv);
	printf("FAIL: no `wifi flash %s` in the table\n", name);
	failures++;
	return -1;
}

static int run(const char *name, int telnet, int argc, char **argv)
{
	return run_on(&sh, name, telnet, argc, argv);
}

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

int main(void)
{
	static char a_info[] = "info", a_read[] = "read", a_backup[] = "backup";
	static char a_write[] = "write", a_imginfo[] = "imginfo";
	static char a_off[] = "0x0", a_confirm[] = "confirm";
	char *v_info[] = { a_info }, *v_read[] = { a_read, a_off };
	char *v_backup[] = { a_backup }, *v_imginfo[] = { a_imginfo };
	char *v_write[] = { a_write, a_off }, *v_write_ok[] = { a_write, a_off, a_confirm };
	const struct {
		const char *name; int argc; char **argv; const char *as, *bg_as;
	} t[] = {
		{ "info",   1, v_info,     "wifi flash info",  "wifi flash info" },
		{ "read",   2, v_read,     "wifi flash read",  "wifi flash read" },
		/* the YMODEM question, first; it does not ask about `&` */
		{ "backup", 1, v_backup,   "xfer",             "wifi flash backup" },
		{ "write",  2, v_write,    "wifi flash write", "wifi flash write" },  /* no `confirm` */
		{ "write",  3, v_write_ok, "wifi flash write", "wifi flash write" },
	};
	size_t i;
	int before;

	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		int rc = run(t[i].name, 1, t[i].argc, t[i].argv);

		check(rc == 1, "telnet: refused with 1");
		check(fk_disarm == 0 && fk_bridge_down == 0 && fk_console_claim == 0,
		      "telnet: nothing taken down");
		check(fk_out == 0, "telnet: nothing printed before the refusal");
		check(fk_refused_as != NULL && strcmp(fk_refused_as, t[i].as) == 0,
		      "telnet: refused under the subcommand's own name");

		rc = run(t[i].name, 0, t[i].argc, t[i].argv);
		if (t[i].argc == 2 && strcmp(t[i].name, "write") == 0)
			check(fk_disarm == 0 && fk_out > 0, "usb: write asks for `confirm` first");
		else
			check(fk_disarm == 1 && fk_bridge_down == 1, "usb: the teardown is reached");
		check(rc == 1, "usb: the fakes make it fail afterwards");

		/* issue #244: `wifi flash <sub> &` from the USB console */
		before = failures;
		rc = run_on(&job, t[i].name, 0, t[i].argc, t[i].argv);
		check(rc == 1, "bg: refused with 1");
		check(fk_disarm == 0 && fk_bridge_down == 0 && fk_console_claim == 0,
		      "bg: nothing taken down before the refusal");
		check(fk_refused_as == NULL, "bg: not refused as telnet");
		check(fk_out == 1 && fk_fg_refused_as != NULL &&
		      strcmp(fk_fg_refused_as, t[i].bg_as) == 0,
		      "bg: only the foreground refusal, under the subcommand's own name");
		if (failures != before)
			printf("  (after `wifi flash %s` argc %d: disarm %d, bridge down %d, "
			       "claim %d, lines %d, refused as %s)\n", t[i].name, t[i].argc,
			       fk_disarm, fk_bridge_down, fk_console_claim, fk_out,
			       fk_fg_refused_as ? fk_fg_refused_as : "(none)");
	}

	/* A job launched from telnet is refused as telnet, before the `&` question. */
	run_on(&job, "info", 1, 1, v_info);
	check(fk_refused_as != NULL && fk_fg_refused_as == NULL && fk_disarm == 0,
	      "telnet bg: refused as telnet, nothing taken down");

	run("imginfo", 1, 1, v_imginfo);
	check(fk_guard_calls == 0 && fk_refused_as == NULL && fk_psram == 1,
	      "telnet: imginfo is not refused");

	if (failures) {
		printf("test_wifi_flash_guard: %d FAILED\n", failures);
		return 1;
	}
	printf("test_wifi_flash_guard: OK\n");
	return 0;
}
