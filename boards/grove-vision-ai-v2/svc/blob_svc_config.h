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

/** The store is the external NOR's blob region (src/blob.c, src/blob_writer.c). */
#define BLOB_SVC_HAS_STORE    1
/** UART0 receives YMODEM (cmds/cmd_xfer.c). */
#define BLOB_SVC_HAS_RECEIVE  1

#define BLOB_SVC_PART         "NOR"
#define BLOB_SVC_ADDR_FMT     "0x%08lx"
#define BLOB_SVC_BASE_HEAD    "base      "  /* as wide as BLOB_SVC_ADDR_FMT */
/* ~80 ms per 4 KB sector, measured on this die (#49 Step 2 item 6; the table
 * is in the board README), so ~1.3 s per 64 KB.  No maximum is documented for
 * the part fitted (issue #89). */
#define BLOB_SVC_ERASE_MS_TYP 1280u
#define BLOB_SVC_ERASE_MS_MAX 0u
#define BLOB_SVC_CANCEL_UNIT  "sectors"
#define BLOB_SVC_HEADER_UNIT  "sector"            /* 4 KB, the part's erase sector */
#define BLOB_SVC_SENDER       "`sb -k <file>` (lrzsz YMODEM batch)"
/* Same cap as `devmem dump`. */
#define BLOB_SVC_READ_MAX     256u
#define BLOB_SVC_HELP         "named assets on the external NOR (models etc.)"
/* [!] "EMPTY" IS ABOUT THE HEADER.  This board ships with factory SenseCraft
 * data inside the region, and a slot whose two header pages happen to be
 * erased reads `empty` with all of that still underneath it. */
#define BLOB_SVC_LIST_NOTE \
	"[!] empty = no blob header.  The payload underneath may still hold\r\n" \
	"    data -- this board ships with factory content in this region.\r\n" \
	"    invalid = bytes this port cannot read; `blob info <slot>` says why.\r\n"

#endif /* BLOB_SVC_CONFIG_H */
