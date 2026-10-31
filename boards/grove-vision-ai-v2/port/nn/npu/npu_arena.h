/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_arena.h
 * @brief   The NN tensor arena's base and size, for the host gate (issue #169).
 *
 * The firmware reaches the arena through svc/nn/ethosu/nn_ethosu_mem.h.  These two
 * exist for scripts/verify_vela_model.cc, which compiles npu_arena.c and plans a
 * model into the real reservation rather than a number repeated beside it.
 */
#ifndef NPU_ARENA_H
#define NPU_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *npu_arena_base(void);
size_t npu_arena_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* NPU_ARENA_H */
