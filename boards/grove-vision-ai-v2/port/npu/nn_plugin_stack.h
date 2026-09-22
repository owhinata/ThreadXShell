/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_plugin_stack.h
 * @brief   What each plugin callback may ask of the stack it runs on (issue #119).
 *
 * The per-slot stack allowances of this board's plugin policy, and the asserts
 * that keep every one of them below each thread its slot can be called on.
 * nn_svc_grove.c initialises nn_plugin_policy.stack_limit from
 * GROVE_PLUGIN_STACK_LIMITS.  The board's host test (test/test_plugin_stack.py)
 * compiles THIS header with numbers of its own, so what it watches refuse is
 * what the firmware refuses -- a copy of these comparisons in a test would go on
 * passing with the ones below deleted.
 *
 * Where each slot runs.  Every console command can also be sent to the
 * background with `&`, which runs it on a job thread instead:
 *
 *   entry       `nn model load` -> plugin_run_load()     console / bg
 *   shapes_ok   the `nn stream start` admission          console / bg
 *   decode      `nn stream` (nn_overlay_process)         producer
 *               `nn run`, `nn dets` (nn_decode_into)     console / bg
 *   draw        the panel thread, under the panel guard  panel
 *   report      `nn run`, `nn dets`                      console / bg
 *   param_set   `nn thresh`                              console / bg
 *   param_get   `nn thresh`, `nn info`                   console / bg
 *
 * [!] NO SLOT RUNS ON THE PRODUCER ALONE.  Until issue #119, entry, shapes_ok
 * and decode were declared against the producer thread (4,096 of its 8,192 B),
 * yet all three are called on a 4,096 B shell stack: an allowance EQUAL to the
 * stack it runs on, which is a check that cannot refuse the case it exists for.
 * A slot reached from more than one thread is declared against the SHALLOWEST,
 * so decode takes the shell's figure, and the producer keeps an assert of its
 * own below.
 *
 * [!] THE ASSERTS ARE A BACKSTOP, NOT THE DERIVATION.  An allowance is sound
 * only if, on every path that reaches its slot,
 *
 *     allowance <= stack - depth at the plugin's entry - 208 B - margin
 *
 * (208 B: the reserve for exception frames; board.cmake derives it).  "Strictly
 * below the stack" is far weaker than that.  It is here because it is the one
 * part a compiler can check, and because its failure -- an allowance equal to
 * the stack -- is exactly what shipped before issue #119.
 *
 * The ceilings are the threads' own numbers: CLI_INSTANCE_STACK_SIZE and
 * CLI_BG_JOB_STACK_SIZE from cli_config.h (board.cmake sets both), and
 * CAM_PRODUCER_STACK_BYTES and CAM_PANEL_STACK_BYTES, which the INCLUDER brings
 * with camera.h and cam_lcd_sink.h.  Those two are not included here: camera.h
 * reaches into the Himax SDK, which a host compiler cannot take, and the test
 * that compiles this header has to be able to set each ceiling on its own.
 */
#ifndef NN_PLUGIN_STACK_H
#define NN_PLUGIN_STACK_H

#include "cli_config.h"   /* CLI_INSTANCE_STACK_SIZE, CLI_BG_JOB_STACK_SIZE */
#include "plugin_abi.h"   /* PLUGIN_SLOT_* */

#if !defined(GROVE_PLUGIN_STACK_SHELL) || !defined(GROVE_PLUGIN_STACK_PANEL)
#error "board.cmake must define GROVE_PLUGIN_STACK_SHELL and GROVE_PLUGIN_STACK_PANEL"
#endif
#if defined(GROVE_PLUGIN_STACK_PRODUCER)
#error "GROVE_PLUGIN_STACK_PRODUCER was retired by issue #119: no slot runs on the producer alone"
#endif
#if !defined(CAM_PRODUCER_STACK_BYTES) || !defined(CAM_PANEL_STACK_BYTES)
#error "include camera.h and cam_lcd_sink.h before nn_plugin_stack.h"
#endif

/* Seven rows, one per slot the ABI has.  A slot added there would otherwise be
 * left at zero here, which plugin_load.c reads as "this board refuses it". */
_Static_assert(PLUGIN_SLOT_COUNT == 7,
               "plugin stack [slots]: a new plugin slot needs a row in "
               "nn_plugin_stack.h");

/* Slot -> the allowance it is declared against: the table above. */
#define GROVE_PLUGIN_LIMIT_ENTRY      GROVE_PLUGIN_STACK_SHELL
#define GROVE_PLUGIN_LIMIT_SHAPES_OK  GROVE_PLUGIN_STACK_SHELL
#define GROVE_PLUGIN_LIMIT_DECODE     GROVE_PLUGIN_STACK_SHELL
#define GROVE_PLUGIN_LIMIT_DRAW       GROVE_PLUGIN_STACK_PANEL
#define GROVE_PLUGIN_LIMIT_REPORT     GROVE_PLUGIN_STACK_SHELL
#define GROVE_PLUGIN_LIMIT_PARAM_SET  GROVE_PLUGIN_STACK_SHELL
#define GROVE_PLUGIN_LIMIT_PARAM_GET  GROVE_PLUGIN_STACK_SHELL

/*
 * What nn_plugin_policy.stack_limit is initialised with.  The asserts below name
 * the same seven macros rather than the two allowances, so a slot moved to
 * another allowance is checked against its own threads without anybody having
 * to remember to add an assert for it.
 */
#define GROVE_PLUGIN_STACK_LIMITS {                                  \
	[PLUGIN_SLOT_ENTRY]     = GROVE_PLUGIN_LIMIT_ENTRY,          \
	[PLUGIN_SLOT_SHAPES_OK] = GROVE_PLUGIN_LIMIT_SHAPES_OK,      \
	[PLUGIN_SLOT_DECODE]    = GROVE_PLUGIN_LIMIT_DECODE,         \
	[PLUGIN_SLOT_DRAW]      = GROVE_PLUGIN_LIMIT_DRAW,           \
	[PLUGIN_SLOT_REPORT]    = GROVE_PLUGIN_LIMIT_REPORT,         \
	[PLUGIN_SLOT_PARAM_SET] = GROVE_PLUGIN_LIMIT_PARAM_SET,      \
	[PLUGIN_SLOT_PARAM_GET] = GROVE_PLUGIN_LIMIT_PARAM_GET,      \
}

/*
 * One assert per slot and per thread it runs on, each on its own line and each
 * naming itself.  Folded into one assert per thread, all the slots of that
 * thread share a single refusal, so the host test could not tell a missing row
 * from a present one while those slots share an allowance.  Separate, a row
 * deleted here is a name missing from what the test sees fire.
 */
#define GROVE_PLUGIN_STACK_BELOW(slot, stack, what)                     \
	_Static_assert(GROVE_PLUGIN_LIMIT_##slot < (stack),              \
	               "plugin stack [" what "]: an allowance must be "  \
	               "below every stack its slot runs on")
/* A zero limit is plugin_load.c's spelling of "this callback may not run", and
 * this board calls every slot. */
#define GROVE_PLUGIN_STACK_NONZERO(slot, what)                          \
	_Static_assert(GROVE_PLUGIN_LIMIT_##slot > 0u,                   \
	               "plugin stack [" what "]: an allowance of 0 "     \
	               "refuses a slot this board calls")

GROVE_PLUGIN_STACK_NONZERO(ENTRY,     "entry > 0");
GROVE_PLUGIN_STACK_NONZERO(SHAPES_OK, "shapes_ok > 0");
GROVE_PLUGIN_STACK_NONZERO(DECODE,    "decode > 0");
GROVE_PLUGIN_STACK_NONZERO(DRAW,      "draw > 0");
GROVE_PLUGIN_STACK_NONZERO(REPORT,    "report > 0");
GROVE_PLUGIN_STACK_NONZERO(PARAM_SET, "param_set > 0");
GROVE_PLUGIN_STACK_NONZERO(PARAM_GET, "param_get > 0");

GROVE_PLUGIN_STACK_BELOW(ENTRY,     CLI_INSTANCE_STACK_SIZE,  "entry < console");
GROVE_PLUGIN_STACK_BELOW(ENTRY,     CLI_BG_JOB_STACK_SIZE,    "entry < bg");
GROVE_PLUGIN_STACK_BELOW(SHAPES_OK, CLI_INSTANCE_STACK_SIZE,  "shapes_ok < console");
GROVE_PLUGIN_STACK_BELOW(SHAPES_OK, CLI_BG_JOB_STACK_SIZE,    "shapes_ok < bg");
GROVE_PLUGIN_STACK_BELOW(DECODE,    CAM_PRODUCER_STACK_BYTES, "decode < producer");
GROVE_PLUGIN_STACK_BELOW(DECODE,    CLI_INSTANCE_STACK_SIZE,  "decode < console");
GROVE_PLUGIN_STACK_BELOW(DECODE,    CLI_BG_JOB_STACK_SIZE,    "decode < bg");
GROVE_PLUGIN_STACK_BELOW(DRAW,      CAM_PANEL_STACK_BYTES,    "draw < panel");
GROVE_PLUGIN_STACK_BELOW(REPORT,    CLI_INSTANCE_STACK_SIZE,  "report < console");
GROVE_PLUGIN_STACK_BELOW(REPORT,    CLI_BG_JOB_STACK_SIZE,    "report < bg");
GROVE_PLUGIN_STACK_BELOW(PARAM_SET, CLI_INSTANCE_STACK_SIZE,  "param_set < console");
GROVE_PLUGIN_STACK_BELOW(PARAM_SET, CLI_BG_JOB_STACK_SIZE,    "param_set < bg");
GROVE_PLUGIN_STACK_BELOW(PARAM_GET, CLI_INSTANCE_STACK_SIZE,  "param_get < console");
GROVE_PLUGIN_STACK_BELOW(PARAM_GET, CLI_BG_JOB_STACK_SIZE,    "param_get < bg");

#endif /* NN_PLUGIN_STACK_H */
