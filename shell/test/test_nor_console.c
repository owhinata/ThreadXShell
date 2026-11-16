/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #250 -- where an erase or a write of the NOR may run
 * (shell/cmds/nor_console.c), compiled unchanged in both shapes the boards build:
 *
 *   SHELL_HAS_NET_TTY=1 (f746g-disco, wio-lite-ai): the telnet console, a job
 *     launched from it and a job of the local console are refused, each named
 *     as what it is, with the local console's name from the board's net_tty
 *     facts; the local console's foreground goes ahead and prints nothing.
 *     The telnet question is asked of every call (it is the board's only
 *     answer to "is this telnet"), so a refusal that stops asking it fails.
 *   SHELL_HAS_NET_TTY=0 (grove-vision-ai-v2): a background job is refused and
 *     the foreground goes ahead; the line names no telnet console.  net_tty.h
 *     is not included and net_tty_is_console() is not defined here, so a
 *     shape that still asked it would not link.
 *
 * The telnet stand-in answers as shell/cmds/net_tty.c does (a job answers for
 * its launcher); the real one is shell/test/test_net_tty.c's.
 */
#include "nor_console.h"

#include "cli.h"
#include "cli_instance.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct cli_instance usb, telnet;
static struct cli_instance usb_job = { .fg = &usb };
static struct cli_instance telnet_job = { .fg = &telnet };

#if SHELL_HAS_NET_TTY
#include "net_tty.h"

static int asked;

bool net_tty_is_console(const struct cli_instance *sh)
{
	const struct cli_instance *owner = (sh->fg != NULL) ? sh->fg : sh;

	asked++;
	return owner == &telnet;
}
const struct net_tty_facts net_tty_board_facts = { .other_console = "USB CDC" };
#endif

static char text[1024];
static size_t text_len;
static int printed;

int cli_error(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)sh;
	printed++;
	va_start(ap, fmt);
	n = vsnprintf(text + text_len, sizeof text - text_len, fmt, ap);
	va_end(ap);
	if (n > 0)
		text_len += ((size_t)n < sizeof text - text_len) ? (size_t)n
		                                                 : sizeof text - text_len - 1;
	return n;
}

int cli_print(struct cli_instance *sh, const char *fmt, ...)
{
	(void)sh;
	(void)fmt;
	printed++;                    /* nothing here may print through cli_print */
	return 0;
}

static int failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n  printed %d: %s\n", what, printed, text);
		failures++;
	}
}

static int run(struct cli_instance *sh)
{
	text_len = 0;
	text[0] = '\0';
	printed = 0;
#if SHELL_HAS_NET_TTY
	asked = 0;
#endif
	return nor_console_refuse(sh, "nor");
}

static int has(const char *s) { return strstr(text, s) != NULL; }

/* One refused line, whole, ending in CR LF, under the command's name. */
static void refused(const char *what)
{
	check(printed == 1, what);
	check(strncmp(text, "nor: ", 5) == 0, what);
	check(text_len >= 2 && strcmp(text + text_len - 2, "\r\n") == 0, what);
}

int main(void)
{
#if SHELL_HAS_NET_TTY
	check(run(&usb) == 0 && printed == 0 && asked == 1,
	      "telnet board: the USB foreground goes ahead, asked once, silent");

	check(run(&telnet) != 0 && asked == 1, "telnet console refused");
	refused("telnet console");
	check(has("foreground of the USB CDC console; this is the telnet console\r\n"),
	      "telnet console: says where it may run and where this is");

	check(run(&telnet_job) != 0 && asked == 1, "job of telnet refused");
	refused("job of telnet");
	check(has("; this is a background job of the telnet console\r\n"),
	      "job of telnet: named as a job of the telnet console");

	check(run(&usb_job) != 0 && asked == 1, "job of USB refused");
	refused("job of USB");
	check(has("foreground of the USB CDC console; this is a background job\r\n"),
	      "job of USB: named as a background job");
	check(!has("this is a background job of the telnet"), "job of USB: not telnet");

	printf("test_nor_console (telnet): %s\n", failures ? "FAILED" : "OK");
#else
	check(run(&usb) == 0 && printed == 0,
	      "no telnet: the foreground goes ahead, silent");

	check(run(&usb_job) != 0, "no telnet: a background job refused");
	refused("no telnet: background job");
	check(strcmp(text, "nor: erasing or writing the flash runs only in the "
	                   "foreground; this is a background job\r\n") == 0,
	      "no telnet: the whole sentence");
	check(!has("telnet") && !has("console"), "no telnet: names no console");

	/* What a board without telnet calls the other instance is still only a
	 * question of fg: an instance that is not a job goes ahead. */
	check(run(&telnet) == 0 && printed == 0, "no telnet: any interactive instance");
	(void)telnet_job;

	printf("test_nor_console (no telnet): %s\n", failures ? "FAILED" : "OK");
#endif
	return failures ? 1 : 0;
}
