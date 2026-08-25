/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    crc32.h
 * @brief   CRC-32/ISO-HDLC (the one zlib and `crc32` print) over a byte stream,
 *          accumulated one chunk at a time.
 *
 * Added for the Grove blob asset store (#49 Step 2 / #92): every stored asset is
 * stamped with the CRC-32 of its payload computed AS IT ARRIVES over YMODEM, and
 * `blob verify` re-reads the flash and recomputes it.  Wio's blob does the same
 * thing with FlashDB's fdb_calc_crc32(), which it gets for free because its KV
 * store already links FlashDB; Grove has no FlashDB (that is Step 3) and is not
 * pulling one in for a checksum, hence this freestanding service.  It depends on
 * <stddef.h>/<stdint.h> alone -- no cli_instance, no ThreadX, no HAL -- so it
 * sits in svc/ next to fmt and ymodem.
 *
 * Two properties matter to callers and neither is visible in the signature:
 *
 *   A. starting from 0 this is standard CRC-32/ISO-HDLC -- the same number as
 *      Python's zlib.crc32 or the `crc32` CLI on the PC, so a value printed by
 *      `blob list` can be checked against the file that was sent;
 *   B. feeding the previous result back in continues the same CRC, so a 1.7 MB
 *      file delivered as ~1665 separate 1024-byte YMODEM blocks ends up with the
 *      value it would have had in one call.  A zero-length chunk is the identity.
 *
 * [!] This function inverts at BOTH ends already, exactly as fdb_calc_crc32()
 * does, so the usual "init 0xFFFFFFFF, complement the result" wrapper inverts
 * twice and yields a different number.  Do not add it.  An earlier revision of
 * the donor's plan (owhinata/wio-lite-ai#10) called for exactly that wrapper; it
 * would have produced a board that disagreed with the host with no way to tell
 * whether the CRC, the transfer or the flash was at fault.  shell/test/test_crc32.c
 * case D pins the trap so nobody re-adds it, and cases A/B/E pin (A) and (B).
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Continue the CRC-32 in @p crc over @p len bytes at @p buf and return it.
 * Pass 0 for @p crc on the first call; pass the previous return value to carry
 * on.  @p len may be 0 (@p buf is then not dereferenced and @p crc comes back
 * unchanged), which is what a receiver handing the sink an empty write needs.
 */
uint32_t crc32_update(uint32_t crc, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC32_H */
