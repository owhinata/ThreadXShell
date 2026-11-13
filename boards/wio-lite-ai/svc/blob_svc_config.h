/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_svc_config.h
 * @brief   What this board's store offers the shared `blob` command, and the
 *          board's words in it (issue #194; see svc/blob/blob_svc.h and
 *          shell/cmds/cmd_blob.c for what each means).
 */
#ifndef BLOB_SVC_CONFIG_H
#define BLOB_SVC_CONFIG_H

/** The store is the external NOR on OCTOSPI2, built with the KV store. */
#if BSP_ENABLE_KV
#define BLOB_SVC_HAS_STORE    1
#else
#define BLOB_SVC_HAS_STORE    0
#endif
/** The USB CDC console receives YMODEM over the shared wiring
 *  (shell/cmds/xfer_console.c). */
#define BLOB_SVC_HAS_RECEIVE  1

#define BLOB_SVC_PART         "NOR"
#define BLOB_SVC_ADDR_FMT     "0x%06lx"
#define BLOB_SVC_BASE_HEAD    "base    "    /* as wide as BLOB_SVC_ADDR_FMT */
/* W25Q128JV 64 KB block erase, typical and maximum (tBE2). */
#define BLOB_SVC_ERASE_MS_TYP 150u
#define BLOB_SVC_ERASE_MS_MAX 2000u
#define BLOB_SVC_CANCEL_UNIT  "64 KB blocks"
#define BLOB_SVC_HEADER_UNIT  "sector"            /* W25Q128JV: 4 KB sector */
#define BLOB_SVC_SENDER       "`sb <file>` (lrzsz YMODEM batch; `sz` will NOT work)"
#define BLOB_SVC_READ_MAX     1024u
#define BLOB_SVC_HELP         "named assets on the external NOR (models etc.)"
#define BLOB_SVC_LIST_NOTE \
	"[!] empty = no blob header; the payload underneath may still hold data.\r\n" \
	"    invalid = bytes this firmware cannot read (a header written before issue #193\r\n" \
	"    among them); `blob info <slot>` says why, `blob erase <slot>` clears it.\r\n"

#endif /* BLOB_SVC_CONFIG_H */
