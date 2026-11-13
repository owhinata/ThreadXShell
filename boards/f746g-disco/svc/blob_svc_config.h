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

/** The store is the QSPI NOR (src/blob.c). */
#define BLOB_SVC_HAS_STORE    1
/** The VCP (USART1) receives YMODEM over the shared wiring
 *  (shell/cmds/xfer_console.c); telnet is refused by the table's question
 *  (cmds/cmd_xfer_board.c). */
#define BLOB_SVC_HAS_RECEIVE  1
/** The VCP's RX ring reports its high-water (backend/cli_backend_uart.c), and
 *  `blob write` prints it next to the drops (decision D6). */
#define BLOB_SVC_HAS_RX_LEVEL 1

#define BLOB_SVC_PART         "QSPI"
#define BLOB_SVC_ADDR_FMT     "0x%06lx"
#define BLOB_SVC_BASE_HEAD    "base    "    /* as wide as BLOB_SVC_ADDR_FMT */
/* N25Q128A 64 KB SECTOR ERASE (0xD8): 0.7 s typical, the figure
 * port/qspi/qspi_flash.h quotes from the part's data sheet (tSE), and 3 s
 * maximum, which is also ST's N25Q128A_SECTOR_ERASE_MAX_TIME (3000 ms) in the
 * Cube BSP's n25q128a.h.  An expectation printed before the wait, nothing
 * else; the measured figure belongs in the board README (issue #194). */
#define BLOB_SVC_ERASE_MS_TYP 700u
#define BLOB_SVC_ERASE_MS_MAX 3000u
#define BLOB_SVC_CANCEL_UNIT  "64 KB sectors"
#define BLOB_SVC_HEADER_UNIT  "4 KB subsector"    /* N25Q128A: its sectors are 64 KB */
#define BLOB_SVC_SENDER       "`sb <file>` (lrzsz YMODEM batch; `sz` will NOT work)"
#define BLOB_SVC_READ_MAX     1024u
#define BLOB_SVC_HELP         "named assets on the QSPI NOR (models etc.)"
#define BLOB_SVC_LIST_NOTE \
	"[!] empty = no blob header; the payload underneath may still hold data.\r\n" \
	"    invalid = bytes this firmware cannot read (the QSPI filesystem used\r\n" \
	"    before issue #194 among them); `blob info <slot>` says why, `blob erase\r\n" \
	"    <slot>` clears it.\r\n"

#endif /* BLOB_SVC_CONFIG_H */
