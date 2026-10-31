/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nncam_input.h
 * @brief   The shared input admission test's reasons, as this board's
 *          nn_camera_start() codes and words (issue #162).
 *
 * nn_camera_start() asks svc/nn/nn_input_contract.h's nn_input_admit() whether
 * the camera can fill the model input, and answers with an NNCAM_ERR_* code
 * (port/nn/nn_camera.h).  This is the one table between the two:
 *
 *   NN_INPUT_OK              NNCAM_OK
 *   NN_INPUT_ERR_ABSENT      NNCAM_ERR_MODEL     (its existing sentence)
 *   NN_INPUT_ERR_DTYPE       NNCAM_ERR_IN_DTYPE
 *   NN_INPUT_ERR_RANK        NNCAM_ERR_IN_RANK
 *   NN_INPUT_ERR_BATCH       NNCAM_ERR_IN_BATCH
 *   NN_INPUT_ERR_EMPTY       NNCAM_ERR_IN_EMPTY
 *   NN_INPUT_ERR_CHANNELS    NNCAM_ERR_IN_CHANNELS
 *   NN_INPUT_ERR_SCALE       NNCAM_ERR_QUANT
 *   NN_INPUT_ERR_ZERO_POINT  NNCAM_ERR_IN_ZP
 *   NN_INPUT_ERR_ALIGN       NNCAM_ERR_IN_ALIGN
 *   NN_INPUT_ERR_BYTES       NNCAM_ERR_IN_BYTES
 *   anything else            NNCAM_ERR_MODEL     (fail closed: a refusal)
 *
 * [!] NO REASON IS FOLDED INTO ANOTHER, and none into the band-tiling refusal
 * (NNCAM_ERR_GEOM), which is this board's own check after the shared one.
 * NNCAM_ERR_MODEL is shared with "no model open" on purpose: its sentence
 * already says "no model loaded, or it has no usable input tensor".
 *
 * Every other code in the table is worded from nn_input_refusal_name() through
 * NNCAM_INPUT_DETAIL_FMT, so each reason reads differently.
 *
 * Pure, no storage: test/test_nncam_input.c walks it.
 */
#ifndef NNCAM_INPUT_H
#define NNCAM_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/** The sentence for a worded code: nn_input_refusal_name(reason), then the hint. */
#define NNCAM_INPUT_DETAIL_FMT \
	"the model input cannot take camera pixels: %s%s (`nn info`)"

/** @return the NNCAM_* code nn_camera_start() returns for @p reason
 *          (an enum nn_input_refusal value); NNCAM_ERR_MODEL for an unknown one. */
int nncam_input_err_of(int reason);

/** @return the enum nn_input_refusal reason whose words @p nncam_rc carries, or
 *          0 if @p nncam_rc is not worded from the shared reasons (then the
 *          board's own sentence applies). */
int nncam_input_reason_of(int nncam_rc);

/** @return the board's extra advice for @p reason ("" when none); never NULL. */
const char *nncam_input_hint(int reason);

#ifdef __cplusplus
}
#endif

#endif /* NNCAM_INPUT_H */
