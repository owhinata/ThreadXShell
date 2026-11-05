/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_config.h
 * @brief   What the shared `nn` command has on STM32F746G-DISCO (issue #50).
 *
 * The board says what it HAS; the shared command registers a subcommand only
 * under its macro.  These are properties, never a board identity test.
 */
#ifndef NN_SVC_CONFIG_H
#define NN_SVC_CONFIG_H

/*
 * [!] THIS ONE FOLLOWS THE BACKEND, NOT THE BOARD, and that is the case the
 * capability rule exists for.  Only a backend that interprets a .tflite in RAM
 * can be pointed at a new model at run time; with the `null` backend -- this
 * board's default until issue #130 made it `tflm` (board.cmake), and still a
 * selectable build -- there is nothing to load and `nn model load` would be a
 * subcommand that always refuses.  So the same board answers differently in two
 * builds, and `help` is right in both.
 */
#if defined(CONFIG_NN_BACKEND_TFLM) || defined(CONFIG_NN_BACKEND_STEDGEAI_RELOC)
#define NN_SVC_HAS_MODEL_LOAD  1
/** ...and the source is a blob in the QSPI asset store, named as it was stored
 *  (`nn model load --name <blob>`, issue #194; until then a path on the SD
 *  card, `--path`, which is gone). */
#define NN_SVC_HAS_MODEL_NAME  1
#endif
/* No NN_SVC_HAS_MODEL_BUILTIN: since issue #159 Step 4c no build of this board
 * carries a model it could adopt by name -- tflm loads from the asset store
 * only, like stedgeai_reloc -- so `nn model load builtin` is refused and the usage
 * does not list it.  (null / stedgeai still open their own stub / generated
 * model without a load; that is has_builtin in nn_backend.h, not a source.) */

/** One-shot capture and infer.  The camera is on the DCMI header. */
#define NN_SVC_HAS_CAMERA      1

/** Repeat-invoke timing.  This board already had it as `ai bench`. */
#define NN_SVC_HAS_BENCH       1

/** Input normalisation range: this board writes the camera input through the
 *  shared conversion (svc/nn/nn_input_contract.h), float32 or int8, so the range
 *  is a real choice here in a way it is not on a fixed-transform board. */
#define NN_SVC_HAS_NORM        1

/** Live inference, as `nn stream start/stop/stats`.  One shared implementation
 *  since issue #99; the subscriber worker below it is unchanged.
 *
 *  NOT NN_SVC_HAS_STREAM_TEST: there is no test pattern on this path.  The
 *  `[qqvga|qvga]` argument this command used to take was removed with it --
 *  the port discarded it and took the geometry from the base capture instead,
 *  so it had not selected anything for some time. */
#define NN_SVC_HAS_STREAM      1

#endif /* NN_SVC_CONFIG_H */
