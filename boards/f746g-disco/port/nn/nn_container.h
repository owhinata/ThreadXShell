/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_container.h
 * @brief   Is a file read for `nn model load --path` a plugin container, and
 *          what is the CRC-32 of the bytes that were read (issue #159).
 *
 * [!] ONE FUNCTION, TWO PRINTERS, ONE BYTE STRING.  The closing check of an
 * asset on this board is "the CRC the board prints = the receipt's CRC" (there
 * is no `blob list` here).  Two places print it: the SD reader in
 * cmds/cmd_nn_board.c, right after a whole-file read (it has the console), and
 * `nn info`'s plugin lines once the backend adopted the container
 * (port/nn/nn_svc_f746_model.c).  Both call THIS over the same (buf, len) --
 * the staging slot the reader filled and the length it returned -- so the two
 * numbers are the CRC of the same bytes, by construction rather than by two
 * computations kept alike.  test/test_nn_model_container.c pins it.
 *
 * tflm only (the backends that can take a container in place).  Owns nothing.
 */
#ifndef NN_CONTAINER_H
#define NN_CONTAINER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @return 1 if the @p len bytes at @p buf probe as a plugin container
 *         (svc/plugin/plugin_load.c's plugin_probe), with @p crc set to the
 *         CRC-32/ISO-HDLC of ALL @p len bytes -- the value zlib.crc32 and the
 *         asset receipt print for the same file; 0 otherwise (@p crc untouched).
 *         Probing is not validating: a container this firmware refuses still
 *         answers 1, because its CRC is still what the operator compares.
 */
int nn_container_crc(const void *buf, uint32_t len, uint32_t *crc);

#ifdef __cplusplus
}
#endif

#endif /* NN_CONTAINER_H */
