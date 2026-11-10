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
#define KV_SVC_MEDIUM       "external NOR"

/*
 * What `kv format yes` says before it starts.  The time is inherent: FlashDB
 * erases all 256 sectors of the 1 MB partition individually while laying down
 * its headers, at up to 400 ms each on the W25Q128JV (measured ~14 s on
 * board #2).
 */
#define KV_SVC_FORMAT_SAYS  "erasing 1 MB and rebuilding -- about 15 s..."

#endif /* KV_SVC_CONFIG_H */
