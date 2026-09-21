/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The .plugin reservation, as this board declares it (issue #108 = #78 Step 3a).
 *
 * [!] THIS IS THE BOARD'S HALF OF A LINKER SCRIPT SPLIT IN TWO.  The other half
 * -- asset/common/plugin.ld -- is the ABI's: its SECTIONS order IS the file
 * format svc/plugin_abi.h defines, so it belongs with the ABI and is shared by
 * every board.  What is genuinely this board's is the address, and it is the
 * only thing here.
 *
 * The top 32 KB of AXI-SRAM.  Why there -- and why that is a stated exception to
 * this board's "AXI-SRAM is for bus masters" rule -- is written on the .plugin
 * section in STM32H725AEIx_IROM.ld.
 *
 * [!] AND THE ADDRESS IS DECLARED MORE THAN ONCE, ON PURPOSE.  The firmware's
 * own script pins the same reservation, board.cmake states it again to the
 * shared check_plugin_image.py (WIO_PLUGIN_GATE_BASE/END), and
 * cmake/check_plugin_reservation.py holds it as its own literal.  A plugin that
 * read the address out of the firmware would agree with whatever it was handed;
 * the gates exist to catch the day the declarations diverge.  DO NOT generate
 * this file and a gate's constants from one CMake variable -- that would turn
 * four independent statements into one, and the gates into decoration.
 *
 * Prelinked, so moving it invalidates every container built for this board.
 */
MEMORY
{
  PLUGIN (rwx) : ORIGIN = 0x24048000, LENGTH = 32K
}
