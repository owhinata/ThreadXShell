/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_ethosu_board.cc
 * @brief   The Ethos-U backend's state and this board's facts (issue #169).
 *
 * svc/nn/ethosu/nn_ethosu.cc owns no storage; its state is the object below,
 * bound at link time (svc/nn/ethosu/nn_ethosu_state.h).  C++ because the state
 * holds TFLM objects' storage.  Zero-initialised (.bss), never constructed: the
 * interpreter and the op resolver are placement-new'd into it on first use.
 */
#include "nn_ethosu_state.h"

#include "nor_flash.h"   /* NOR_XIP_BASE, NOR_SIZE: the read window, stated once */

extern "C" {

nn_ethosu_state nn_ethosu_board_state;

/*
 * Model names are the blob names the operator typed (`nn model load --name`), so
 * nothing is prefixed.  The window is the external NOR's XIP alias, the whole part:
 * a model may lie anywhere in it the caller's lease covers, and the backend refuses
 * a range that leaves it.
 */
const struct nn_ethosu_board_facts nn_ethosu_board_facts = {
	"", NOR_XIP_BASE, NOR_SIZE
};

}  /* extern "C" */
