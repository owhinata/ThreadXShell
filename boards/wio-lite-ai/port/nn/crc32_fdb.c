/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    crc32_fdb.c
 * @brief   svc/crc32.h's contract, served by the CRC-32 this firmware already
 *          links (issue #108).
 *
 * svc/plugin_load.c checks a container's plugin digest with crc32_update().  This
 * board already carries a CRC-32/ISO-HDLC -- FlashDB's fdb_calc_crc32(), which
 * the blob store stamps every asset with -- and test/test_crc32.c pins the two
 * properties crc32_update() promises of it: that starting from 0 gives the
 * standard CRC (0xCBF43926 for "123456789"), and that feeding a result back in
 * continues the same CRC.  Both implementations invert on entry and exit, so the
 * call passes straight through; wrapping it again would double-invert.
 *
 * [!] WHY NOT SIMPLY LINK svc/crc32.c.  Two implementations of one function in
 * a 384 KB partition would be the cheapest thing in the image to not have -- and
 * the one this board already trusts for its asset CRCs is the one pinned by a
 * host test against the value `blob list` prints.
 */
#include "crc32.h"

#include <flashdb.h>

uint32_t crc32_update(uint32_t crc, const void *buf, size_t len)
{
	return fdb_calc_crc32(crc, buf, len);
}
