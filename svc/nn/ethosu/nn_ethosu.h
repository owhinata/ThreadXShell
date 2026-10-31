/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_ethosu.h
 * @brief   What the Ethos-U backend says to a board's adapter beyond nn.h
 *          (issue #169).  C, so the adapter can include it.
 *
 * The backend itself is reached through svc/nn/nn.h like every other: the adapter
 * calls nn_model_reload() with the model's address in the board's read window and
 * its length, under the lease that keeps those bytes in place, and the backend
 * refuses a range outside the window or a length below its floor itself.  The
 * one thing nn.h has no word for is the sentence for its own refusals.
 */
#ifndef NN_ETHOSU_H
#define NN_ETHOSU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One sentence for a code nn_model_reload() returned on this backend, never NULL.
 * Words the in-place refusals nn.h added for it (NN_MODEL_ERR_WINDOW, _IDENT,
 * _PAYLOAD) and hands every other code to nn_model_strerror().  The board passes
 * this as its nn_core_model_backend::strerror.
 */
const char *nn_ethosu_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* NN_ETHOSU_H */
