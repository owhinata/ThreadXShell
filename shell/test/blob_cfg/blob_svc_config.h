/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A stand-in for a board's blob_svc_config.h (issue #194), so
 * shell/test/test_blob_cmd.c can build the shared `blob` command in each shape
 * a board may declare.  run_host_tests.sh picks the shape:
 *
 *   BLOB_TEST_SHAPE_FULL     a store that the console can fill (wio / Grove)
 *   BLOB_TEST_SHAPE_NOSTORE  no store at all (wio with BSP_ENABLE_KV off)
 *   BLOB_TEST_SHAPE_NORECV   a store, and no receive yet (f746 until 4c)
 *   BLOB_TEST_SHAPE_RXLEVEL  FULL, and the console reports its RX ring's
 *                            high-water (f746)
 *
 * The words are written to be told apart from every board's, so a test that
 * passes is reading the configuration, not a board's file.
 */
#ifndef BLOB_SVC_CONFIG_H
#define BLOB_SVC_CONFIG_H

#if defined(BLOB_TEST_SHAPE_NOSTORE)
#define BLOB_SVC_HAS_STORE    0
#define BLOB_SVC_HAS_RECEIVE  1
#elif defined(BLOB_TEST_SHAPE_NORECV)
#define BLOB_SVC_HAS_STORE    1
#define BLOB_SVC_HAS_RECEIVE  0
#else
#define BLOB_SVC_HAS_STORE    1
#define BLOB_SVC_HAS_RECEIVE  1
#endif

#define BLOB_SVC_PART         "TESTNOR"
#define BLOB_SVC_ADDR_FMT     "0x%06lx"
#define BLOB_SVC_BASE_HEAD    "base    "
#define BLOB_SVC_ERASE_MS_TYP 1000u
#define BLOB_SVC_ERASE_MS_MAX 2000u
#define BLOB_SVC_CANCEL_UNIT  "test units"
#define BLOB_SVC_HEADER_UNIT  "test header unit"
#define BLOB_SVC_SENDER       "`test-send <file>`"
#define BLOB_SVC_READ_MAX     512u
#define BLOB_SVC_HELP         "test blob store"
#if defined(BLOB_TEST_SHAPE_RXLEVEL)
#define BLOB_SVC_HAS_RX_LEVEL 1
#endif
#define BLOB_SVC_LIST_NOTE    "[!] test note under the list\r\n"

#endif /* BLOB_SVC_CONFIG_H */
