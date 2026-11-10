/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_svc_config.h
 * @brief   This board's words in the shared `kv` command (issue #214; see
 *          shell/cmds/cmd_kv.c for where each appears).
 *
 * String literals only: the command pastes them into its own format strings,
 * so none may contain a '%'.
 */
#ifndef KV_SVC_CONFIG_H
#define KV_SVC_CONFIG_H

/** What the store is on: the root's help and the first line of `kv info`. */
#define KV_SVC_MEDIUM       "QSPI NOR"

/*
 * What `kv format yes` says before it starts.  The time is inherent and the
 * part's (N25Q128A datasheet: 64 KB erase 0.7 s typical / 3 s max, 4 KB erase
 * 0.25 s / 0.8 s): our wipe of the megabyte is 16 64 KB erases, then FlashDB
 * erases each of the 256 4 KB sectors again while laying down its headers --
 * about 75 s typical, about 4 minutes at the datasheet's maxima.  Every other
 * `kv` command and the blob store's commands answer busy until it is done.
 */
#define KV_SVC_FORMAT_SAYS  "erasing 1 MB and rebuilding -- about 75 s (up to 4 min)..."

#endif /* KV_SVC_CONFIG_H */
