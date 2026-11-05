/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_svc.h
 * @brief   What the shared `blob` command needs from a board's store
 *          (issue #194).
 *
 * The one `blob` command (shell/cmds/cmd_blob.c) is compiled once per board and
 * reaches the board's store through the functions below, linked by these fixed
 * names, as the nn and camera commands reach theirs (svc/nn/nn_svc.h,
 * svc/camera/camera_svc.h).  Each board implements them in its own
 * boards/<board>/src/ files.
 *
 * [!] NOTHING HERE TAKES A struct cli_instance.  Printing, waiting, Ctrl+C and
 * the console belong to the command and to the board's shell-layer hooks
 * (shell/cmds/blob_cmd_board.h); a store function only answers.  Every int
 * return is a svc/blob/blob_err.h result unless its comment says otherwise.
 *
 * WHAT A BOARD STATES AT COMPILE TIME is in its svc/blob_svc_config.h:
 *
 *   BLOB_SVC_HAS_STORE    1 when the functions below exist on this build
 *   BLOB_SVC_HAS_RECEIVE  1 when the console can receive a file into a slot
 *                         (the board's blob_board_receive())
 *
 * `blob` is registered only when both are 1: a store that cannot be filled
 * from the console would offer `blob erase` with no way back.  With either 0
 * the command still COMPILES -- the storage audit requires every board to
 * compile every shared command -- and registers nothing.  The board's words
 * (BLOB_SVC_PART, BLOB_SVC_ADDR_FMT, ...) are listed in cmd_blob.c, where they
 * are used.
 *
 * TWO KINDS OF EXCLUSION, said once.  The reads (blob_stat, blob_read,
 * blob_verify) take whatever their board needs for one call and give it back
 * (a busy flag is not taken at all on wio-lite-ai and f746g-disco -- f746g-disco's
 * only look at it and answer BUSY while it is held; a lease is, on
 * grove-vision-ai-v2).  Erasing and writing go under ONE reservation held
 * across the whole command (blob_svc_reserve): the busy flag, or the NOR
 * writer reservation.  The _reserved forms are for use under it.
 */
#ifndef BLOB_SVC_H
#define BLOB_SVC_H

#include <stdint.h>

#include "blob_err.h"
#include "blob_map.h"
#include "blob_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the table and the device ---------------------------------------------- */

/** Is the shipped slot table one this store may act on?  @p bad receives the
 *  offending slot when the verdict names one. */
enum blob_map_verdict blob_check_map(unsigned *bad);

/** Where @p slot is.  Any out pointer may be NULL. */
int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max);

/** The interval the table is carved from and the header unit at the top of
 *  every slot -- for `blob free`'s uncarved line and `blob info`'s header line. */
void blob_svc_interval(uint32_t *lo, uint32_t *hi, uint32_t *unit);

/** Non-zero when the part is up, or comes up by itself on first use; 0 when
 *  it never came up this boot (asked before a `blob write` erases anything). */
int blob_svc_ready(void);

/* ---- reads, each taking and giving back its own exclusion ---------------- */

int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why);
int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len);
int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed);

/* ---- the reservation, for an erase or a write ----------------------------- */

/** Take it.  BLOB_OK and a non-zero @p token, or BUSY / FAULT / STATE with
 *  @p token 0. */
int  blob_svc_reserve(uint32_t *token);
/** Give it back.  Called once, on every path that reserved. */
void blob_svc_unreserve(uint32_t token);

/** Retire @p slot's blob: erase its header unit and nothing else. */
int  blob_svc_erase_header(uint32_t token, unsigned slot);

/** Before each erase unit; non-zero stops the erase before it. */
typedef int (*blob_svc_tick_fn)(void *ctx, uint32_t done, uint32_t total);

/**
 * Erase [@p addr, @p addr + @p len) under the reservation, cancellable through
 * @p tick between units.  Returns the BOARD WRITER'S status, 0 on success --
 * not a blob_err.h value: it is what the coordinator logs as vendor_rc, and it
 * treats anything but 0 as "do not program".  The board logs the details.
 */
int  blob_svc_erase_span(uint32_t token, uint32_t addr, uint32_t len,
                         blob_svc_tick_fn tick, void *ctx, uint32_t *done,
                         int *cancelled);

/** Program @p len bytes under the reservation.  The board writer's status, 0
 *  on success (as blob_svc_erase_span). */
int  blob_svc_program(uint32_t token, uint32_t addr, const void *data,
                      uint32_t len);

/** Read back what was programmed, under the reservation.  0 or -1. */
int  blob_svc_read_back(uint32_t addr, void *buf, uint32_t len);

/** A slot's header, read under the reservation (the write chooses its slot
 *  from these).  A blob_err.h result. */
int  blob_svc_stat_reserved(unsigned slot, struct blob_info *out);

/**
 * The staging buffer a `blob write` programs from, lays the header pages out in
 * and reads the verify back through: placed by the board where its writer can
 * reach it, one user at a time (the holder of the reservation).  @p len
 * receives its size, a whole number of BLOB_HDR_PAGE.
 */
uint8_t *blob_svc_stage(uint32_t *len);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_SVC_H */
