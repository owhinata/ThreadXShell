/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_board.cc
 * @brief   This board's tflm backend state and facts (issue #161).
 *
 * svc/nn/tflm/nn_tflm.cc owns no storage; its state is the object below, bound at
 * link time (svc/nn/tflm/nn_tflm_state.h).  C++ because the state holds TFLM objects'
 * storage.  Plain zero-initialised .bss in internal SRAM, as the backend's globals
 * were before #161; no constructor runs (the type is asserted trivial), so nothing
 * here executes before the SDRAM and the clocks are up.
 */
#include "nn_tflm_state.h"

extern "C" {

nn_tflm_state nn_tflm_board_state;

/* Models come off the SD card: `nn info` names one "sd:<path>". */
const struct nn_tflm_board_facts nn_tflm_board_facts = { "sd:" };

}  /* extern "C" */
