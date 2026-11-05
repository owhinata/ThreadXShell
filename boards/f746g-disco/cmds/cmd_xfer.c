/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.c
 * @brief   `xfer` shell command: send a file to the PC over the VCP
 * (owhinata/stm32f746g-disco#50).
 *
 *   xfer send sd <path>        stream a file from the microSD to the PC
 *                              over the same serial port as the shell, using
 *                              YMODEM (receive on the PC with lrzsz `rz`/`rb`).
 *
 * Motivation: pulling a captured camera frame (owhinata/stm32f746g-disco#42) off
 * the board used to mean `camera save sd <p>` then physically removing the microSD
 * card. This streams any sd file out the VCP instead; `camera send`
 * (cmd_camera_board.c) streams the RAM frame directly.  Both share xfer_send_source()
 * below.
 *
 * The protocol core (svc/ymodem.c) is transport-agnostic; here we wire its IO
 * vtable to the new raw-console API (cli_console_claim/cli_read_byte/cli_write/
 * cli_rx_flush) and its source vtable to FileX fx_file_read.  Clean-room.
 *
 * Since issue #194 it also RECEIVES (xfer_recv_sink_locked(), for `blob
 * write`), the same shape as grove-vision-ai-v2's cmd_xfer.c.
 */
#include "cli.h"
#include "cmd_xfer.h"
#include "log.h"
#include "fs_cmd_core.h"
#include "fs_devices.h"       /* fs_sd_device() (send) */

#include <string.h>

/* ---- YMODEM IO adapter: cli_read_byte / cli_write ------------------------- */

static int io_getc(void *ctx, unsigned timeout_ms)
{
	int r = cli_read_byte((struct cli_instance *)ctx, timeout_ms);
	/* Map a local Ctrl+C to a transfer abort; -1/-2 already match
	 * YM_IO_TIMEOUT/YM_IO_ABORT. */
	return (r == 3) ? YM_IO_ABORT : r;
}

static int io_put(void *ctx, const uint8_t *buf, size_t len)
{
	return cli_write((struct cli_instance *)ctx, buf, len) < 0 ? -1 : 0;
}

/* ---- receive (issue #194) ------------------------------------------------- */

/* How long the line must stay quiet after a failed receive, and how much a
 * drain may swallow before it gives up -- grove-vision-ai-v2's numbers. */
#define XFER_DRAIN_QUIET_MS   400u
#define XFER_DRAIN_MAX_BYTES  8192u

static int io_getc_recv(void *ctx, unsigned timeout_ms)
{
	/* [!] 0x03 IS NOT AN ABORT HERE.  See cmd_xfer.h: while receiving, the
	 * stream is the file, and about one byte in 256 of binary content is
	 * 0x03.  The send direction maps it (io_getc above); this must not. */
	return cli_read_byte((struct cli_instance *)ctx, timeout_ms);
}

static void drain_until_quiet(struct cli_instance *sh)
{
	uint32_t dropped = 0u;

	while (dropped < XFER_DRAIN_MAX_BYTES) {
		if (cli_read_byte(sh, XFER_DRAIN_QUIET_MS) < 0)
			return;             /* quiet for the whole window */
		dropped++;
	}
}

int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink)
{
	struct ym_io io = { sh, io_getc_recv, io_put };
	const struct ym_recv_diag *d;
	enum ym_result res;

	cli_rx_flush(sh);                /* type-ahead and the command's newline */
	res = ymodem_recv(&io, sink);
	cli_rx_flush(sh);                /* a trailing 'O' / CAN / garbage tail  */
	if (res != YM_OK)
		drain_until_quiet(sh);

	/* The post-mortem goes to the log, not the console: while a transfer
	 * runs the PC's terminal belongs to the sender. */
	d = ymodem_recv_diag();
	log_write(res == YM_OK ? LOG_LEVEL_INF : LOG_LEVEL_ERR, "xfer",
	          "recv rc=%d blk=%lu crc=%lu seq=%lu shrt=%lu tmo=%lu",
	          (int)res, (unsigned long)d->blocks, (unsigned long)d->bad_crc,
	          (unsigned long)d->bad_seq, (unsigned long)d->short_read,
	          (unsigned long)d->timeouts);
	if (d->first_kind >= 0)
		log_write(LOG_LEVEL_ERR, "xfer",
		          "first bad blk kind=%02X seq=%d body=%lu/%lu",
		          (unsigned)d->first_kind, d->first_seq,
		          (unsigned long)d->first_got, (unsigned long)d->first_want);
	return res == YM_OK ? 0 : 1;
}

int xfer_send_source(struct cli_instance *sh, const struct ym_source *src)
{
	struct ym_io   io = { sh, io_getc, io_put };
	enum ym_result res;

	cli_print(sh, "ymodem: sending '%s' (%lu bytes) over the VCP\r\n",
	          src->name, (unsigned long)src->size);
	cli_print(sh, "ymodem: start the receiver now -- e.g. `rb` (lrzsz YMODEM); "
	              "Ctrl+C aborts\r\n");

	switch (cli_console_claim(sh)) {
	case 0:
		break;
	case -2:
		cli_error(sh, "ymodem: cannot run in the background -- "
		              "drop the trailing '&'\r\n");
		return 1;
	default:
		cli_error(sh, "ymodem: console busy\r\n");
		return 1;
	}
	cli_rx_flush(sh);                  /* drop type-ahead / the command newline */
	res = ymodem_send(&io, src);
	cli_rx_flush(sh);                  /* drop a trailing 'O'/'C'/garbage tail   */
	cli_console_release(sh);

	switch (res) {
	case YM_OK:
		cli_print(sh, "ymodem: sent %lu bytes OK\r\n",
		          (unsigned long)src->size);
		return 0;
	case YM_ERR_CANCEL:
		cli_warn(sh, "ymodem: cancelled\r\n");
		return 1;
	case YM_ERR_TIMEOUT:
		cli_error(sh, "ymodem: timeout -- no receiver? start `rz` on the PC\r\n");
		return 1;
	case YM_ERR_SOURCE:
		cli_error(sh, "ymodem: source read error\r\n");
		return 1;
	case YM_ERR_IO:
	default:
		cli_error(sh, "ymodem: transport error\r\n");
		return 1;
	}
}

/* ---- file source: FileX fx_file_read -------------------------------------- */

static int file_src_read(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got)
{
	FX_FILE *file = (FX_FILE *)ctx;
	ULONG    g = 0;
	UINT     st = fx_file_read(file, dst, want, &g);

	if (st == FX_END_OF_FILE) { *got = 0; return 0; }
	if (st != FX_SUCCESS)
		return -1;
	*got = (uint32_t)g;
	return 0;
}

/* basename: the part after the last '/' or '\\', for the YMODEM block-0 name. */
static const char *xfer_basename(const char *path)
{
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	return base;
}

static int cmd_xfer_send(struct cli_instance *sh, int argc, char **argv)
{
	const struct fs_device *dev;
	FX_MEDIA *media;
	FX_FILE   file;
	struct ym_source src;
	UINT status;
	int  rc;

	(void)argc;

	/* The medium stays a word on the line (only `sd` since issue #194, when
	 * the QSPI NOR stopped being a filesystem), so `xfer send sd <path>`
	 * keeps its meaning. */
	if (strcmp(argv[1], "sd") != 0) {
		cli_error(sh, "xfer: unknown medium '%s' (sd)\r\n", argv[1]);
		return 1;
	}
	dev = fs_sd_device();

	status = dev->op_begin();
	if (status != FX_SUCCESS) {
		cli_error(sh, "xfer: %s: %s\r\n", dev->name, fs_strerror(status));
		return 1;
	}

	media = fs_core_mount(dev, sh);
	if (media == NULL) {
		dev->op_end();
		return 1;
	}

	status = fx_file_open(media, &file, (CHAR *)argv[2], FX_OPEN_FOR_READ);
	if (status != FX_SUCCESS) {
		cli_error(sh, "xfer: open %s: %s (0x%02x)\r\n", argv[2],
		          fs_strerror(status), status);
		dev->op_end();
		return 1;
	}

	src.ctx  = &file;
	src.name = xfer_basename(argv[2]);
	src.size = (uint32_t)file.fx_file_current_file_size;  /* FAT: < 4 GiB */
	src.read = file_src_read;

	rc = xfer_send_source(sh, &src);

	(void)fx_file_close(&file);
	dev->op_end();
	return rc;
}

CLI_SUBCMD_SET_CREATE(xfer_subcmds,
	CLI_CMD_ARG_USAGE(send, NULL, "stream a file to the PC over YMODEM sd <path>",
	                  "sd <path>", cmd_xfer_send, 3, 0),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(xfer, xfer_subcmds,
                 "transfer files to the PC over the VCP (YMODEM)", NULL, 1, 0);
