/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the console hand-over of a binary transfer (issue #229):
 * cli_console_claim() / cli_console_release() as the firmware builds them
 * (shell/core/cli_printf.c), over the host output lock (host_glue.c), which
 * counts its takes and releases and can be made to fail.
 *
 * Every case also checks that the lock is held by exactly what the mark says:
 * across the case, (takes - releases) changes by the same amount as the mark.
 * The counters are compared by their change, never by their value.
 *
 *   (a) one console: claim -> marked, release -> not marked; a background job
 *       is refused with -2 and leaves the mark alone;
 *   (f) the lock cannot be taken: -1, and neither the mark nor the lock moves.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"

CLI_BACKEND_DUMMY_DEFINE(tr);
static struct cli_instance sh;   /* the foreground console */
static struct cli_instance job;  /* a background-job worker of sh */

static void setup(void)
{
	memset(&sh, 0, sizeof sh);
	memset(&job, 0, sizeof job);
	sh.tr  = &tr;
	tr.sh  = &sh;
	job.tr = &tr;                /* a worker aliases its foreground's transport */
	job.fg = &sh;
	cli_dummy_clear_output(&tr);
	cli_dummy_reset_stats(&tr);
	cli_test_set_lock_fail(0);
	assert(cli_xfer_active == 0);
}

/* Lock takes not yet given back, since the program started. */
static int held(void)
{
	return (int)(cli_test_lock_gets() - cli_test_lock_puts());
}

static void test_one_console(void)
{
	setup();
	int h0 = held();

	assert(cli_console_claim(&sh) == 0);
	assert(cli_xfer_active != 0);
	assert(held() - h0 == 1);

	cli_console_release(&sh);
	assert(cli_xfer_active == 0);
	assert(held() - h0 == 0);

	/* A background job is refused before the lock is touched. */
	unsigned g0 = cli_test_lock_gets();
	assert(cli_console_claim(&job) == -2);
	assert(cli_xfer_active == 0);
	assert(cli_test_lock_gets() == g0);
	assert(held() - h0 == 0);
}

static void test_lock_fails(void)
{
	setup();
	int      h0 = held();
	unsigned g0 = cli_test_lock_gets();
	unsigned p0 = cli_test_lock_puts();

	cli_test_set_lock_fail(1);
	assert(cli_console_claim(&sh) == -1);
	assert(cli_xfer_active == 0);
	assert(cli_test_lock_gets() == g0 && cli_test_lock_puts() == p0);
	cli_test_set_lock_fail(0);

	/* And the console is not left unusable: the next claim works. */
	assert(cli_console_claim(&sh) == 0);
	assert(cli_xfer_active != 0);
	cli_console_release(&sh);
	assert(cli_xfer_active == 0);
	assert(held() - h0 == 0);
}

int main(void)
{
	test_one_console();
	test_lock_fails();
	printf("OK: console claim / one console / lock fails pass\n");
	return 0;
}
