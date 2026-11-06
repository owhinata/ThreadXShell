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
 * board -- "usage: nn model load load <...>" -- because the `model` parent
 * and the `load` leaf shared one argument string while their paths differ by a
 * word.  Only the joined line shows that, so this drives the joined line: the
 * dummy backend in, the capture log out, with stub nn_svc_* adapters behind it.
 *
 * Also (issue #131 P19): which model-state line a refused load prints, the
 * BUSY refusal printing none -- the same printer, so the same harness.
 *
 * And (issue #196) what the one grammar answers: a bare word reaches the
 * adapter as the name, an option word gets the "write the name alone"
 * sentence, a wrong count the usage -- the same on every board.
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

/* What the usage must print -- written out, not rebuilt from the macro, so a
 * wrong expansion cannot agree with itself.  One form on every board since
 * issue #196. */
#define SHAPE    "a bare name (all three boards)"
#define SOURCES  "<name>"
#define A_SOURCE "person"

/* The sentence an option word gets, around the word that was typed. */
#define DASH_LINE(w) \
	"nn: '" w "' is not a model name -- write the name alone (a name never " \
	"starts with '-')\r\n"

/* ---- stub adapters: nothing below is reached by a wrong invocation ------- */

static int load_calls;
/* What the stub load answers and what `nn info` says was open before it -- set
 * by the refusal cases below, reset to "refused SPEC, nothing open" between
 * lines. */
static int load_status;
static enum nn_model_state load_state;
static uint8_t info_model_active;
/* The name the adapter was handed, as the parser copied it. */
static char load_name[NN_SPEC_NAME_MAX + 1];

void nn_svc_info(struct nn_svc_info *out)
{
	memset(out, 0, sizeof *out);
	out->model_active = info_model_active;
}
void nn_svc_info_extra(nn_svc_write_fn write, void *ctx) { (void)write; (void)ctx; }
void nn_svc_model_load(const struct nn_spec *spec, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	load_calls++;
	strncpy(load_name, spec->name, sizeof load_name - 1u);
	res->status = load_status;
	res->claim = (uint8_t)NN_CLAIM_NONE;
	*state = load_state;
}
void nn_svc_model_unload(struct nn_op_result *res) { res->status = NN_SVC_OK; }
int  nn_svc_tensors_pin(void) { return NN_SVC_ERR_STATE; }
void nn_svc_tensors_unpin(void) { }
int  nn_svc_output_count(void) { return NN_SVC_ERR_STATE; }
int  nn_svc_output(unsigned i, struct tensor_desc *o) { (void)i; (void)o; return NN_SVC_ERR_STATE; }
int  nn_svc_input(struct tensor_desc *o) { (void)o; return NN_SVC_ERR_STATE; }
void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *d, int max,
                           struct nn_report_capture *rep,
                           struct nn_result_extra *ext, struct nn_op_result *res)
{
	(void)snap; (void)d; (void)max; (void)rep; (void)ext;
	res->status = NN_SVC_ERR_STATE;
}
int  nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out) { *out = *in; return NN_SVC_OK; }
int  nn_svc_thresh_get(unsigned *milli) { *milli = NN_SVC_THRESH_NONE; return NN_SVC_OK; }
int  nn_svc_thresh_set(unsigned milli) { (void)milli; return NN_SVC_ERR_STATE; }

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
	memset(load_name, 0, sizeof load_name);
	load_status = NN_SVC_ERR_SPEC;
	load_state = NN_MODEL_EMPTY;
	info_model_active = 0;
}

static void run_line(const char *line)
{
	reset();
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

/* A load the stub answers with (status, state), `nn info` having said whether a
 * model was open before it. */
static void run_load(int status, enum nn_model_state state, uint8_t had)
{
	static const char line[] = "nn model load " A_SOURCE "\r";

	reset();
	load_status = status;
	load_state = state;
	info_model_active = had;
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}


static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

/* Any of the lines a refused load may add about the model it left. */
static int has_state_line(void)
{
	return has("nothing is loaded") ||
	       has("the previous model is still active") ||
	       has("the previous model could not be restored") ||
	       has("the new model is open") || has("nn: loaded");
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

	/* The leaf, one word more than the dispatcher takes: its line. */
	run_line("nn model load --addr 0x1 16 x y\r");
	expect("five words after `load` are the dispatcher's",
	       has("load: invalid number of arguments") &&
	       has("usage: nn model load " SOURCES "\r\n") &&
	       !has("is not a model name") && load_calls == 0);

	/* A bare word is a name: it reaches the adapter, copied whole. */
	run_line("nn model load bogus\r");
	expect("a bare word reaches the adapter as the name",
	       load_calls == 1 && strcmp(load_name, "bogus") == 0 &&
	       !has("usage:"));

	/* Two words that are not options: the usage, nothing called. */
	run_line("nn model load a b\r");
	expect("`a b` gets the usage alone and calls nothing",
	       has("usage: nn model load " SOURCES "\r\n") &&
	       !has("is not a model name") && !has("invalid number") &&
	       load_calls == 0);

	/* [!] AN OPTION WORD IS THE GRAMMAR'S REFUSAL (issue #196): the old
	 * tagged forms, the most words the dispatcher lets through (`--addr 0x1
	 * 16 x`), and any other option -- the sentence quoting the word typed,
	 * the usage, no state line, and the adapter never called. */
	{
		static const struct { const char *line, *want; } opt[] = {
			{ "nn model load --name x\r",          DASH_LINE("--name") },
			{ "nn model load --addr 0x1 16 x\r",   DASH_LINE("--addr") },
			{ "nn model load --slot 5\r",          DASH_LINE("--slot") },
			{ "nn model load --path x\r",          DASH_LINE("--path") },
			{ "nn model load -foo\r",              DASH_LINE("-foo") },
			{ "nn model load -\r",                 DASH_LINE("-") },
		};
		unsigned i;

		for (i = 0; i < sizeof opt / sizeof opt[0]; i++) {
			char what[64];

			run_line(opt[i].line);
			snprintf(what, sizeof what, "%.*s gets the option sentence",
			         (int)(strlen(opt[i].line) - 1u), opt[i].line);
			expect(what, has(opt[i].want) && load_calls == 0 &&
			       has("usage: nn model load " SOURCES "\r\n") &&
			       !has_state_line());
		}
		run_line("nn model load\r");
		expect("the usage offers no option",
		       !has("--name") && !has("--slot") && !has("--path") &&
		       !has("builtin") && !has("--addr"));
	}

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

	/*
	 * [!] A BUSY REFUSAL PRINTS NO MODEL STATE (issue #131 P19).  The shared
	 * order answers BUSY only before anything changed, so whatever the state
	 * word says then describes another console's load, caught mid-way -- on
	 * wio it read "nothing is loaded" while the other load had the model down.
	 * Every state is tried, with and without a model before, so a fix that
	 * only skipped one line of the four does not pass.
	 */
	{
		static const enum nn_model_state all[] = {
			NN_MODEL_EMPTY, NN_MODEL_PREVIOUS, NN_MODEL_NEW,
		};
		unsigned i;
		uint8_t had;

		for (i = 0; i < sizeof all / sizeof all[0]; i++) {
			for (had = 0; had <= 1; had++) {
				run_load(NN_SVC_ERR_BUSY, all[i], had);
				expect("a BUSY load reaches the adapter and is reported",
				       load_calls == 1 && has("busy"));
				expect("and says nothing about the model it did not touch",
				       !has_state_line());
			}
		}
	}

	/* The other refusals keep their state lines, word for word. */
	run_load(NN_SVC_ERR_HW, NN_MODEL_EMPTY, 0);
	expect("a non-BUSY refusal with nothing open says so",
	       load_calls == 1 && has("nn: nothing is loaded\r\n"));
	run_load(NN_SVC_ERR_HW, NN_MODEL_EMPTY, 1);
	expect("a non-BUSY refusal that lost the previous model warns",
	       has("the previous model could not be restored -- nothing is "
	           "loaded now\r\n"));
	run_load(NN_SVC_ERR_HW, NN_MODEL_PREVIOUS, 1);
	expect("a non-BUSY refusal that kept the previous model says so",
	       has("nn: the previous model is still active\r\n"));
	run_load(NN_SVC_ERR_HW, NN_MODEL_NEW, 1);
	expect("a non-BUSY refusal that changed the model says so",
	       has("nn: the new model is open, but it has no decoder\r\n"));
	run_load(NN_SVC_OK, NN_MODEL_NEW, 0);
	expect("a load that succeeds says loaded", has("nn: loaded\r\n"));

	if (failures != 0) {
		printf("test_nn_cmd_usage: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_cmd_usage: all passed\n");
	return 0;
}
