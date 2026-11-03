/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_load_host.c
 * @brief   The host's entry to the container parse (issue #165).  See
 *          plugin_load_host.h.  [!] Never linked into a firmware.
 */
#include "plugin_load_host.h"

#include "plugin_load_body.h"

enum plugin_result plugin_parse_with_policy(const void *buf, size_t len,
                                            const struct plugin_policy *pol,
                                            struct plugin_view *out)
{
	return parse_checked(buf, len, pol, out);
}
