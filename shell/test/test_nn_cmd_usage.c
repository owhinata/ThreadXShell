/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: what `nn model` / `nn model load` print on a wrong invocation,
 * through the REAL dispatcher and the REAL shell/cmds/cmd_nn.c (issue #122).
 *
 * WHY THROUGH THE DISPATCHER.  The usage line is assembled in two places: the
 * dispatcher prints "usage: <command path> " and the command supplies what
 * follows.  Each half looks right on its own, and the line was wrong on every
 * board -- "usage: nn model load load <--slot <n>>" -- because the `model` parent
 * and the `load` leaf shared one argument string while their paths differ by a
 * word.  Only the joined line shows that, so this drives the joined line: the
 * dummy backend in, the capture log out, with stub nn_svc_* adapters behind it.
 *
 * Built once per source set a board declares (run_host_tests.sh passes the
 * NN_SVC_HAS_MODEL_* macros), so the list each board prints is pinned too.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "nn_svc.h"
#include "nn_svc_config.h"

/* The source list each shape must print -- written out, not rebuilt from the
 * macros, so a wrong expansion cannot agree with itself. */
#if defined(NN_SVC_HAS_MODEL_SLOT) && NN_SVC_HAS_MODEL_SLOT
#define SHAPE    "slot (wio-lite-ai)"
#define SOURCES  "<--slot <n>>"
#elif defined(NN_SVC_HAS_MODEL_NAME) && NN_SVC_HAS_MODEL_NAME
#define SHAPE    "name + addr (grove-vision-ai-v2)"
#define SOURCES  "<--name <name> | --addr <addr> <len>>"
#elif defined(NN_SVC_HAS_MODEL_PATH) && NN_SVC_HAS_MODEL_PATH
#define SHAPE    "path + builtin (f746g-disco)"
#define SOURCES  "<--path <p> | builtin>"
#else
#error "build this test with a board's NN_SVC_HAS_MODEL_* set"
#endif

/* ---- stub adapters: nothing below is reached by a wrong invocation ------- */

static int load_calls;

void nn_svc_info(struct nn_svc_info *out) { memset(out, 0, sizeof *out); }
void nn_svc_info_extra(nn_svc_write_fn write, void *ctx) { (void)write; (void)ctx; }
void nn_svc_model_load(const struct nn_spec *spec, nn_svc_read_fn read,
                       void *ctx, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	(void)spec; (void)read; (void)ctx;
	load_calls++;
	res->status = NN_SVC_ERR_SPEC;
	res->claim = (uint8_t)NN_CLAIM_NONE;
	*state = NN_MODEL_EMPTY;
}
void nn_svc_model_unload(struct nn_op_result *res) { res->status = NN_SVC_OK; }
int  nn_svc_tensors_pin(void) { return NN_SVC_ERR_STATE; }
void nn_svc_tensors_unpin(void) { }
int  nn_svc_output_count(void) { return NN_SVC_ERR_STATE; }
int  nn_svc_output(unsigned i, struct tensor_desc *o) { (void)i; (void)o; return NN_SVC_ERR_STATE; }
int  nn_svc_input(struct tensor_desc *o) { (void)o; return NN_SVC_ERR_STATE; }
void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *d, int max,
                           struct nn_report_capture *rep, struct nn_op_result *res)
{
	(void)snap; (void)d; (void)max; (void)rep;
	res->status = NN_SVC_ERR_STATE;
}
int  nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out) { *out = *in; return NN_SVC_OK; }
int  nn_svc_thresh_get(unsigned *milli) { *milli = NN_SVC_THRESH_NONE; return NN_SVC_OK; }
int  nn_svc_thresh_set(unsigned milli) { (void)milli; return NN_SVC_ERR_STATE; }
#if defined(NN_SVC_HAS_MODEL_PATH) && NN_SVC_HAS_MODEL_PATH
int nn_board_read_file(void *ctx, const char *path, void *buf, uint32_t cap,
                       uint32_t *len)
{
	(void)ctx; (void)path; (void)buf; (void)cap; (void)len;
	return -1;
}
#endif

/* ---- harness (the same shape as test_integration.c) --------------------- */

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
	load_calls = 0;
}

static void run_line(const char *line)
{
	reset();
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

static int failures;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		failures++;
	}
}

int main(void)
{
	printf("test_nn_cmd_usage (%s):\n", SHAPE);

	/* The leaf, too few words: the dispatcher's line, with the path once. */
	run_line("nn model load\r");
	expect("`nn model load` is refused by the dispatcher",
	       has("load: invalid number of arguments"));
	expect("and its usage names the path once, then this board's sources",
	       has("usage: nn model load " SOURCES "\r\n"));
	expect("never \"load load\"", !has("load load"));
	expect("the adapter was not called", load_calls == 0);

	/* The leaf, too many words: the same line. */
	run_line("nn model load a b c d e\r");
	expect("`nn model load` with too many words gets the same usage",
	       has("usage: nn model load " SOURCES "\r\n") && !has("load load"));

	/* The parser's own line, after a bare word: the same sources. */
	run_line("nn model load bogus\r");
	expect("a bare word is refused by the grammar, before the adapter",
	       has("'bogus' is not a model source") && load_calls == 0);
	expect("and the parser's usage line agrees with the dispatcher's",
	       has("usage: nn model load " SOURCES "\r\n") && !has("load load"));

	/*
	 * [!] THE PARENT'S .usage IS NOT PRINTED, and this pins that rather than
	 * pretending otherwise.  `model` has no handler, and the parser answers
	 * NO_HANDLER before it counts arguments, so a bare or over-long `nn model`
	 * never reaches the usage branch.  If that changes, this is where the
	 * parent's spelling ("load <...>" after "nn model") gets seen for the
	 * first time -- check it then.
	 */
	run_line("nn model\r");
	expect("a bare `nn model` asks for a subcommand",
	       has("model: missing or unknown subcommand") && !has("usage:"));
	run_line("nn model a b c d e f\r");
	expect("so does an over-long one",
	       has("model: missing or unknown subcommand") && !has("load load"));

	if (failures != 0) {
		printf("test_nn_cmd_usage: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_cmd_usage: all passed\n");
	return 0;
}
