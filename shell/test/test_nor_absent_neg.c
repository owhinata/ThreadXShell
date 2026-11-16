/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #250 -- what a board does not have is declared once, in its
 * svc/nor_svc_config.h, and a board whose code disagrees does not compile.
 * Compiled by run_host_tests.sh in three shapes, each of which must FAIL:
 *
 *   NEG_READ_LEFT  the board says NOR_SVC_HAS_READ 0 (NOR_TEST_SHAPE_ABSENT)
 *                  and still defines nor_svc_board_read() -- svc/nor/nor_svc.h
 *                  has taken the name for something that is not a function;
 *   NEG_TEST_NO_READ  NOR_SVC_HAS_TEST 1 without NOR_SVC_HAS_READ -- the test
 *                  reads through it, nor_svc.h's #error;
 *   NEG_NO_SAYS    a subcommand set to 0 with no line to say -- nor_svc.h's
 *                  #error.
 */
#if defined(NEG_TEST_NO_READ)
#define NOR_SVC_HAS_READ  0
#define NOR_SVC_HAS_WRITE 1
#define NOR_SVC_HAS_TEST  1
#define NOR_SVC_NO_READ_SAYS ""
#define NOR_SVC_CONFIG_H
#elif defined(NEG_NO_SAYS)
#define NOR_SVC_HAS_READ  0
#define NOR_SVC_HAS_WRITE 1
#define NOR_SVC_HAS_TEST  0
#define NOR_SVC_NO_TEST_SAYS ""
#define NOR_SVC_CONFIG_H
#endif

#include "nor_svc.h"

#if defined(NEG_READ_LEFT)
void nor_svc_board_read(uint32_t addr, uint8_t *buf, uint32_t len,
                        struct nor_svc_result *r)
{
	(void)addr; (void)buf; (void)len; (void)r;
}
#endif

int nor_absent_neg_unused;
