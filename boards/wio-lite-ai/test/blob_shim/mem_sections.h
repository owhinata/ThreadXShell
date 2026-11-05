/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for include/mem_sections.h, as src/blob_writer.c uses it
 * (issue #194 4d): on the host there is no DTCM, so the placement attribute is
 * empty and the staging buffer is an ordinary array.
 */
#ifndef BLOB_SHIM_MEM_SECTIONS_H
#define BLOB_SHIM_MEM_SECTIONS_H

#define DTCM_BSS

#endif /* BLOB_SHIM_MEM_SECTIONS_H */
