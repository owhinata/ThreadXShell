/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_writer.c
 * @brief   The store functions the shared `blob` command reaches
 *          (svc/blob/blob_svc.h, issue #194).
 *
 * Moved here from cmds/cmd_blob.c when the command became shared, and taking no
 * console: the staging buffer, the reservation (the busy flag), and thin wraps
 * of src/blob.c and port/nor that add the log lines a transfer leaves behind
 * (the console belongs to the sender while a write runs).  Kept apart from
 * src/blob.c so that file still compiles for its host test with nothing but
 * the NOR stand-ins.
 */
#include "blob.h"

#include "blob_svc.h"
#include "log.h"
#include "mem_sections.h"    /* DTCM_BSS: the write's staging buffer */
#include "nor_flash.h"


/*
 * The staging buffer the shared coordinator programs from, lays the two header
 * pages out in and reads the verify back through (svc/blob/blob_write.h).
 *
 * DTCM, because the NOR driver moves every byte with the CPU (indirect mode, no
 * DMA -- port/nor/nor_flash.h), so nothing that cannot reach a TCM ever touches
 * it.  1 KB, one YMODEM block: how long one program holds the NOR mutex against
 * the kv store is what it always was.  Static and single, because its one user
 * at a time is whoever holds the busy flag -- the write's reservation.
 * (Moved here from cmds/cmd_blob.c when the command became shared.)
 */
#define BLOB_STAGE_BYTES  1024u
_Static_assert(BLOB_STAGE_BYTES % BLOB_HDR_PAGE == 0u,
               "the staging buffer must be whole header pages");
static uint8_t blob_stage_buf[BLOB_STAGE_BYTES] DTCM_BSS __attribute__((aligned(4)));

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
	return nor_flash_ready();
}

/* The reservation is the busy flag: it serialises a write or an erase against
 * each other, another console's and `nn model load`.  The token is a constant
 * -- there is only one flag. */
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

int blob_svc_program(uint32_t token, uint32_t addr, const void *data,
                     uint32_t len)
{
	int rc;

	(void)token;
	rc = nor_write(addr, data, len);
	/* To the log: this runs with the console handed to the protocol. */
	if (rc != NOR_OK)
		log_write(LOG_LEVEL_ERR, "blob", "program 0x%lx+%lu: %d",
		          (unsigned long)addr, (unsigned long)len, rc);
	return rc;
}

int blob_svc_read_back(uint32_t addr, void *buf, uint32_t len)
{
	return nor_read(addr, buf, len) == NOR_OK ? 0 : -1;
}

/* Under the busy flag the plain stat is the reserved one: nothing else may
 * write while it is held. */
int blob_svc_stat_reserved(unsigned slot, struct blob_info *out)
{
	return blob_stat(slot, out, NULL);
}
