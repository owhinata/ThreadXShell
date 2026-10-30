/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_container.c
 * @brief   See nn_container.h (issue #159).
 */
#include "nn_container.h"

#include "crc32.h"
#include "plugin_load.h"

int nn_container_crc(const void *buf, uint32_t len, uint32_t *crc)
{
	if (buf == NULL || crc == NULL)
		return 0;
	if (plugin_probe(buf, (size_t)len) != PLUGIN_KIND_CONTAINER)
		return 0;
	*crc = crc32_update(0u, buf, (size_t)len);
	return 1;
}
