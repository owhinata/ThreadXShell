/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    crc32.c
 * @brief   CRC-32/ISO-HDLC, nibble at a time, from a 64-byte table (#92).
 *
 * Clean-room: the table below is not transcribed from another implementation --
 * it is the sixteen 4-bit remainders of the reflected generator polynomial
 * 0xEDB88320 (the reflection of the CRC-32 polynomial 0x04C11DB7), i.e.
 *
 *     t[n] = n shifted through the polynomial four times:
 *            for (k = 0; k < 4; k++) n = (n >> 1) ^ (0xEDB88320 & -(n & 1));
 *
 * so every entry is derivable from the one constant above.  shell/test/test_crc32.c
 * case E recomputes the whole CRC bit by bit straight from that polynomial and
 * compares, over all 256 single-byte inputs (which drive both lookups through all
 * 16 entries) and over a 1.7 MB stream.  It shares no table with this file, so
 * neither a mistyped entry nor a table regenerated from a different polynomial can
 * pass -- not even with the expected values in the test edited to match.
 *
 * Why a nibble (64 B) rather than the usual byte-wide table (1 KB): on Grove the
 * whole thing lands in ITCM, where the budget is counted in kilobytes, and the
 * stream it accompanies is nowhere near CPU-bound -- YMODEM at 921600 baud delivers
 * ~92 KB/s, against two table lookups per byte at 400 MHz.  (`blob verify` re-reads
 * through the XIP window instead; which of the CRC and the QSPI read dominates
 * there has not been measured.)  Cost at -Os for cortex-m55: ~60 B of code plus the
 * 64 B table.
 */
#include <stddef.h>
#include <stdint.h>

#include "crc32.h"

/* The 4-bit remainders of 0xEDB88320; see the file comment for how they arise. */
static const uint32_t crc32_nibble[16] = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
	0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
	0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
	0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

/* Invert on entry and on exit so that a caller starting from 0 gets the standard
 * CRC-32/ISO-HDLC value and can feed the result straight back in to continue.
 * fdb_calc_crc32() -- what wio's blob uses -- has exactly these semantics; see
 * the "do not wrap this" note in crc32.h. */
uint32_t crc32_update(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		crc = (crc >> 4) ^ crc32_nibble[crc & 0x0Fu];
		crc = (crc >> 4) ^ crc32_nibble[crc & 0x0Fu];
	}

	return ~crc;
}
