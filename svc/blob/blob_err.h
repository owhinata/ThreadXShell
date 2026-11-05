/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_err.h
 * @brief   What a board's blob store answers: one set of results for every
 *          board (issue #194).
 *
 * Each board's store (boards/<board>/src/blob.c) returns these, and so does
 * every function its blob.h declares.  Until issue #194 each board numbered its
 * own: wio-lite-ai and f746g-disco used the list below, grove-vision-ai-v2 had
 * busy -2, table -3 and fault -4.  One `blob` command for three boards needs
 * one meaning per number, so the boards' numbers are now these.
 *
 * [!] THE NUMBERS ARE WIO-LITE-AI'S, KEPT, AND FAULT IS NEW AT THE END.  They
 * are printed raw in a few places ("failed (%d)", "could not be verified
 * (%d)"), so keeping two boards' numbers keeps their text; only Grove's busy,
 * table and fault moved.  shell/test/test_blob_err.c pins every value.
 *
 * [!] DO NOT FOLD ONE INTO ANOTHER.  Each is a different instruction to the
 * operator, and two of them decide whether trying again can ever work:
 *
 *   BUSY   somebody else holds the store or the part just now -- retry
 *   FAULT  TERMINAL: the part's port latched a failure and only a reset clears
 *          it.  Told apart from BUSY on purpose: on grove-vision-ai-v2 the
 *          lease is refused for both reasons, and calling a terminal failure
 *          busy has a caller retrying forever (issue #79's rule).
 *   STATE  the part never came up (driver not initialised) -- not "faulted":
 *          nothing latched, the board simply has no store this boot
 *   IO     the part refused this one read, program or erase
 *
 * Which board can produce which (a board that cannot tell a case apart does
 * not invent it):
 *
 *   PARAM STATE IO BUSY EMPTY CRC MAP CANCEL DUPLICATE  wio-lite-ai, f746g-disco
 *   PARAM       IO BUSY EMPTY CRC MAP                   grove-vision-ai-v2
 *                                                        (+ FAULT; IO only from
 *                                                        src/blob_writer.c's
 *                                                        header erase, #194)
 *
 * Plain macros, not an enum, because every store function returns an int that
 * is either one of these or BLOB_OK, and the boards compare them as ints.
 */
#ifndef BLOB_ERR_H
#define BLOB_ERR_H

#define BLOB_OK              0
#define BLOB_ERR_PARAM      -1   /* no such slot, or a range outside it            */
#define BLOB_ERR_IO         -2   /* the part refused a read, program or erase      */
#define BLOB_ERR_STATE      -3   /* the part never came up this boot               */
#define BLOB_ERR_BUSY       -4   /* somebody else holds the store or the part      */
#define BLOB_ERR_EMPTY      -5   /* the slot holds no header to check against      */
#define BLOB_ERR_CRC        -6   /* read-back does not match the stored CRC-32     */
#define BLOB_ERR_MAP        -7   /* the slot table does not fit the interval       */
#define BLOB_ERR_CANCEL     -8   /* an erase stopped because the operator asked    */
#define BLOB_ERR_DUPLICATE  -9   /* more than one VALID slot carries the name      */
#define BLOB_ERR_FAULT     -10   /* TERMINAL: the part's port is faulted; reset    */

#endif /* BLOB_ERR_H */
