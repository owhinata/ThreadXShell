/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_stage.h
 * @brief   The staging buffer a blob write programs out of (#92).
 *
 * A YMODEM transfer arrives 1024 bytes at a time and the NOR is written a
 * chunk at a time, so something has to sit between them.  This is it: bytes
 * accumulate here until a chunk is full, and then one nor_write_program()
 * takes the whole chunk.  The alternative -- one transaction per 1 KB frame --
 * is what the transaction budget refuses: every transaction takes the
 * memory-mapped window down and back up, which writes the NOR's non-volatile
 * status register twice (nor_write.h), so 1,704,672 B of model would cost
 * ~1665 transactions instead of ~27.
 *
 * [!] IT IS IN SRAM FOR THE BUDGET, NOT FOR REACHABILITY.  Nothing here is
 * handed to a DMA engine: nor_write_program() copies out of this buffer a
 * program page at a time into a page buffer it owns, and the vendor's DMA
 * reads THAT.  (The vendor's write also byte-swaps the buffer it is given on
 * one of its paths, which is the other reason that copy exists.)  DTCM would
 * work; it is simply the wrong place to spend 64 KB, since the heap-to-stack
 * gap the placement gate guards is ~152 KB while this window has ~430 KB
 * spare.  The TCM-is-invisible-to-DMA rule that pins .lcd_fb and .cam_raw does
 * not apply to this buffer, and saying so here is cheaper than someone
 * re-deriving it later and moving it for the wrong reason.
 *
 * NOLOAD, like every other reservation in that window: 64 KB of zeros in the
 * image would be 64 KB written to the external NOR on every flash, on the part
 * this project deliberately does not wear out.  Its contents start undefined
 * and every user fills what it reads.
 *
 * [!] ONE USER AT A TIME, AND THE NOR RESERVATION IS WHAT SERIALISES THEM.
 * A `blob write` holds a reservation (#91) across its whole transfer, so no
 * other console command can be inside a NOR write while it runs -- and this
 * buffer only ever holds bytes on their way into one.  Nothing here enforces
 * that; the reservation does, and a second user that did not take one would be
 * refused by the writer before it reached the flash.
 */
#ifndef BLOB_STAGE_H
#define BLOB_STAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bytes of staging, and the chunk one transaction programs: a transfer fills
 * this buffer and hands the whole of it to one nor_write_program().
 *
 * [!] MEASURED, NOT CHOSEN (#49 Step 2, implementation order item 6).  One
 * program transaction on this board costs
 *
 *     0.7 ms  +  2.08 ms per KB      (1/4/16/32/64 KB, whole transaction,
 *                                     R^2 to within the 1 ms tick)
 *
 * -- so the fixed cost of a transaction is under a millisecond and the time is
 * essentially all payload.  Chunk size therefore barely changes how long a
 * transfer takes (4.7 s of programming for the 1,704,672 B classification
 * model at 1 KB, 3.6 s at 64 KB); what it changes is how many TRANSACTIONS
 * there are, and each one takes the memory-mapped window down and back up and
 * writes the NOR's non-volatile status register twice on the way (nor_write.h).
 * 1 KB chunks would be 1,668 transactions for that model and 64 KB chunks are
 * 30, which is the budget #49 Step 2 was planned against: 1 erase + 27
 * programs + body + magic.
 *
 * Nothing in the protocol argues for less: the sender waits 60 s before it
 * retransmits an unacknowledged block (measured against lrzsz 0.12.21 through
 * a pty), and 64 KB costs 134 ms.
 *
 * cmake/check_placement_budget.py states this number independently and fails
 * the build if the two ever disagree.
 */
#define BLOB_STAGE_BYTES   (64u * 1024u)

/**
 * The buffer itself.  Not static and `used`: the placement gate looks it up by
 * name, and a local symbol is what LTO was seen renaming (see npu_arena.c).
 */
extern uint8_t blob_stage_buf[BLOB_STAGE_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* BLOB_STAGE_H */
