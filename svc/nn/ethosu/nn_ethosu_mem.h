/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_ethosu_mem.h
 * @brief   The activation arena the Ethos-U backend plans into (issue #169):
 *          declared here, sized by the board's compile definition, placed by
 *          the board.
 *
 * svc/nn/ethosu/nn_ethosu.cc owns no storage (cmake/check_no_mutable_storage.py
 * audits it), so the arena is DEFINED by the board in a C translation unit, where
 * its placement gate looks it up by this plain name (grove-vision-ai-v2:
 * port/nn/npu/npu_arena.c, .nn_arena in SRAM, NOLOAD, 32-byte aligned and a whole
 * number of cache lines -- the cache protocol in npu_cache.c maintains it whole).
 * The NPU is a bus master: the arena must be somewhere it can reach, never TCM.
 *
 * [!] ONE COMPILE DEFINITION, AND NO FALLBACK.  NN_ETHOSU_ARENA_KB is the board's
 * (its cmake passes it to the firmware and to the host gate that plans a model into
 * the same reservation); a missing one is a build error rather than a default.
 */
#ifndef NN_ETHOSU_MEM_H
#define NN_ETHOSU_MEM_H

#include <stdint.h>

#ifndef NN_ETHOSU_ARENA_KB
#error "NN_ETHOSU_ARENA_KB is not defined: the board's cmake passes it"
#endif

#define NN_ETHOSU_ARENA_BYTES  ((uint32_t)(NN_ETHOSU_ARENA_KB) * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/** The NPU's activation arena.  The board's placement; NPU-visible. */
extern uint8_t nn_ethosu_arena[NN_ETHOSU_ARENA_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* NN_ETHOSU_MEM_H */
