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
 * Bytes of staging, and therefore the largest chunk one transaction can take.
 *
 * [!] A CEILING, NOT THE CHUNK SIZE.  What a transfer actually programs per
 * transaction is settled by measurement (#49 Step 2, implementation order item
 * 6: how long a program of 1/4/16/32/64 KB takes against how long the sender
 * waits before retransmitting).  This is how much room that measurement is
 * allowed to ask for, chosen as the largest chunk the transaction budget was
 * written around -- 1 erase + 27 programs + body + magic = 30 transactions for
 * the 1,704,672 B classification model.
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
