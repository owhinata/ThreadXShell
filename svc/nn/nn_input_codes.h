/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_input_codes.h
 * @brief   The camera input admission test's reasons as the start codes and the
 *          sentences of wio-lite-ai and f746g-disco (issue #162).
 *
 * Both boards ask nn_input_admit() (nn_input_contract.h) before a camera stream
 * or a `nn run` starts, and both answer a refusal with the same number and the
 * same sentence.  The numbers are defined HERE, once; each board's
 * port/nn/nn_camera.h takes its NNCAM_ERR_MODEL / NNCAM_ERR_QUANT /
 * NNCAM_ERR_IN_* from these names rather than writing the values again.  A board
 * keeps only its own check that none of them collides with its other start
 * codes (its test/test_nncam_input.c).
 *
 *   NN_INPUT_OK              NN_INPUT_START_OK            0
 *   NN_INPUT_ERR_ABSENT      NN_INPUT_START_ERR_MODEL    -3  (the board's own sentence)
 *   NN_INPUT_ERR_DTYPE       NN_INPUT_START_ERR_DTYPE   -18
 *   NN_INPUT_ERR_RANK        NN_INPUT_START_ERR_RANK    -19
 *   NN_INPUT_ERR_BATCH       NN_INPUT_START_ERR_BATCH   -20
 *   NN_INPUT_ERR_EMPTY       NN_INPUT_START_ERR_EMPTY   -21
 *   NN_INPUT_ERR_CHANNELS    NN_INPUT_START_ERR_CHANNELS -22
 *   NN_INPUT_ERR_SCALE       NN_INPUT_START_ERR_QUANT   -11
 *   NN_INPUT_ERR_ZERO_POINT  NN_INPUT_START_ERR_ZP      -23
 *   NN_INPUT_ERR_ALIGN       NN_INPUT_START_ERR_ALIGN   -24
 *   NN_INPUT_ERR_BYTES       NN_INPUT_START_ERR_BYTES   -25
 *   anything else            NN_INPUT_START_ERR_MODEL    -3  (fail closed: a refusal)
 *
 * [!] NO REASON IS FOLDED INTO ANOTHER.  NN_INPUT_START_ERR_MODEL is also the
 * boards' "no model open" on purpose: their sentence for it says "no model
 * loaded, or it has no usable input tensor".  Every other refusal code is worded
 * from nn_input_refusal_name() through NN_INPUT_START_DETAIL_FMT, so each reason
 * reads differently.
 *
 * Pure, no storage: shell/test/test_nn_input_codes.c walks it.
 */
#ifndef NN_INPUT_CODES_H
#define NN_INPUT_CODES_H

#ifdef __cplusplus
extern "C" {
#endif

#define NN_INPUT_START_OK              0
#define NN_INPUT_START_ERR_MODEL      (-3)  /**< no model, or no usable input tensor  */
#define NN_INPUT_START_ERR_QUANT      (-11) /**< int8 scale not a positive normal     */
#define NN_INPUT_START_ERR_DTYPE      (-18) /**< input neither float32 nor int8       */
#define NN_INPUT_START_ERR_RANK       (-19) /**< input not 4-dimensional              */
#define NN_INPUT_START_ERR_BATCH      (-20) /**< input batch not 1                    */
#define NN_INPUT_START_ERR_EMPTY      (-21) /**< input height or width 0              */
#define NN_INPUT_START_ERR_CHANNELS   (-22) /**< input channels neither 1 nor 3       */
#define NN_INPUT_START_ERR_ZP         (-23) /**< int8 zero point outside -128..127    */
#define NN_INPUT_START_ERR_ALIGN      (-24) /**< float32 buffer not 4-byte aligned    */
#define NN_INPUT_START_ERR_BYTES      (-25) /**< input buffer shorter than its shape  */

/** The sentence for a worded code: nn_input_refusal_name(reason), then the hint. */
#define NN_INPUT_START_DETAIL_FMT \
	"the model input cannot take camera pixels: %s%s (`nn info`)"

/** @return the start code for @p reason (an enum nn_input_refusal value);
 *          NN_INPUT_START_ERR_MODEL for an unknown one. */
int nn_input_start_err_of(int reason);

/** @return the enum nn_input_refusal reason whose words @p code carries, or 0 if
 *          @p code is not worded from the shared reasons (then the board's own
 *          sentence applies). */
int nn_input_start_reason_of(int code);

/** @return the extra advice for @p reason ("" when none); never NULL. */
const char *nn_input_start_hint(int reason);

#ifdef __cplusplus
}
#endif

#endif /* NN_INPUT_CODES_H */
