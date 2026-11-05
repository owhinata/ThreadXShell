/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_writer.c
 * @brief   The store functions the shared `blob` command reaches
 *          (svc/blob/blob_svc.h, issue #194).
 *
 * The same split as wio-lite-ai's src/blob_writer.c: the staging buffer, the
 * reservation (the busy flag of src/blob.c), and thin wraps of src/blob.c and
 * port/qspi that add the log lines a transfer leaves behind -- the console
 * belongs to the sender while a write runs.  Nothing here takes a console.
 *
 * [!] EVERY PROGRAM AND READ-BACK IS HELD TO THE STORE'S INTERVAL.  The
 * coordinator only ever asks for addresses inside the slot it chose, so this
 * refuses nothing in a correct run; it is here so that a wrong address from
 * anywhere cannot reach the reserved first megabyte or the unallocated top
 * through the store's writer.  (wio-lite-ai's writer has no such check -- its
 * nor_write() only bounds the device.)
 *
 * [!] A PROGRAM IS NOT READ BACK HERE, as on wio-lite-ai.  qspi_flash_write()
 * checks the part's flag status register after every page (P_ERR) and reports
 * a failed page; the payload is then read back whole by the coordinator against
 * the CRC of the stream before the magic page goes down, and the body page
 * carries its own CRC, which the decoder checks -- a body page that programmed
 * wrongly reads `invalid`, never `valid`.
 */
#include "blob.h"

#include "blob_svc.h"
#include "log.h"
#include "qspi_flash.h"

/*
 * The staging buffer the shared coordinator programs from, lays the two header
 * pages out in and reads the verify back through (svc/blob/blob_write.h).
 *
 * Internal SRAM .bss.  QSPI is driven by the CPU in indirect mode
 * (port/qspi/qspi_flash.c: HAL_QSPI_Transmit / _Receive, no DMA), so the buffer
 * needs no particular memory and no cache maintenance -- it only has to be
 * somewhere every console thread can address, which the DTCM is too; plain
 * .bss lets the linker place it.  1 KB, one YMODEM block, so the payload goes
 * down in the pieces it arrives in and one program holds the QSPI mutex for
 * four page programs.  Static and single, because its one user at a time is
 * whoever holds the busy flag -- the write's reservation.
 */
#define BLOB_STAGE_BYTES  1024u
_Static_assert(BLOB_STAGE_BYTES % BLOB_HDR_PAGE == 0u,
               "the staging buffer must be whole header pages");
static uint8_t blob_stage_buf[BLOB_STAGE_BYTES] __attribute__((aligned(4)));

uint8_t *blob_svc_stage(uint32_t *len)
{
	*len = BLOB_STAGE_BYTES;
	return blob_stage_buf;
}

void blob_svc_interval(uint32_t *lo, uint32_t *hi, uint32_t *unit)
{
	*lo = BLOB_REGION_BASE;
	*hi = BLOB_REGION_END;
	*unit = BLOB_ERASE_UNIT;
}

int blob_svc_ready(void)
{
	return qspi_flash_ready();
}

/* The reservation is the busy flag: it serialises a write or an erase against
 * each other, against another console's, against `qspi erase` / `qspi test`
 * and against the nn reader.  The token is a constant -- there is only one
 * flag. */
int blob_svc_reserve(uint32_t *token)
{
	*token = 0u;
	if (blob_busy_acquire() != BLOB_OK)
		return BLOB_ERR_BUSY;
	*token = 1u;
	return BLOB_OK;
}

void blob_svc_unreserve(uint32_t token)
{
	(void)token;
	blob_busy_release();
}

int blob_svc_erase_header(uint32_t token, unsigned slot)
{
	(void)token;
	return blob_erase_header(slot);
}

int blob_svc_erase_span(uint32_t token, uint32_t addr, uint32_t len,
                        blob_svc_tick_fn tick, void *ctx, uint32_t *done,
                        int *cancelled)
{
	int rc;

	(void)token;
	rc = blob_erase_span(addr, len, tick, ctx, done, cancelled);
	if (rc != BLOB_OK && rc != BLOB_ERR_CANCEL)
		log_write(LOG_LEVEL_ERR, "blob", "erase 0x%lx+%lu: %d (%lu done)",
		          (unsigned long)addr, (unsigned long)len, rc,
		          (unsigned long)*done);
	return rc;
}

/* Inside [BLOB_REGION_BASE, BLOB_REGION_END), by subtraction so a length near
 * 2^32 cannot wrap into looking contained. */
static int inside(uint32_t addr, uint32_t len)
{
	return len != 0u && addr >= BLOB_REGION_BASE && addr < BLOB_REGION_END &&
	       len <= BLOB_REGION_END - addr;
}

int blob_svc_program(uint32_t token, uint32_t addr, const void *data,
                     uint32_t len)
{
	int rc;

	(void)token;
	if (!inside(addr, len)) {
		log_write(LOG_LEVEL_ERR, "blob", "program 0x%lx+%lu: outside the "
		          "store", (unsigned long)addr, (unsigned long)len);
		return QSPI_FLASH_ERR_PARAM;
	}
	rc = qspi_flash_write(addr, data, len);
	/* To the log: this runs with the console handed to the protocol. */
	if (rc != QSPI_FLASH_OK)
		log_write(LOG_LEVEL_ERR, "blob", "program 0x%lx+%lu: %d",
		          (unsigned long)addr, (unsigned long)len, rc);
	return rc;
}

int blob_svc_read_back(uint32_t addr, void *buf, uint32_t len)
{
	if (!inside(addr, len))
		return -1;
	return qspi_flash_read(addr, buf, len) == QSPI_FLASH_OK ? 0 : -1;
}

int blob_svc_stat_reserved(unsigned slot, struct blob_info *out)
{
	return blob_stat_reserved(slot, out);
}
