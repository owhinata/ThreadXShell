/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_load_host.h
 * @brief   The host's entry to the container parse, with a policy argument
 *          (issue #165).  Host only: the container verifier and the host tests.
 *
 * [!] NEVER IN A FIRMWARE.  A firmware's loader reads the board's policy by
 * name (plugin_parse(), plugin_load.h); an entry that takes one would let it be
 * handed a policy other than the one the build reads back.  The firmware link
 * is checked for this name (cmake/check_policy_entry.py).
 */
#ifndef PLUGIN_LOAD_HOST_H
#define PLUGIN_LOAD_HOST_H

#include "plugin_load.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  plugin_parse(), with @p pol in place of the board's policy.
 *
 * The same checks in the same order (svc/plugin/plugin_load_body.h); @p pol
 * must not be NULL.
 */
enum plugin_result plugin_parse_with_policy(const void *buf, size_t len,
                                            const struct plugin_policy *pol,
                                            struct plugin_view *out);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_LOAD_HOST_H */
