/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The .plugin reservation, as this board declares it (issue #106).
 *
 * [!] THIS IS THE BOARD'S HALF OF A LINKER SCRIPT SPLIT IN TWO.  The other half
 * -- asset/common/plugin.ld -- is the ABI's: its SECTIONS order IS the file
 * format svc/plugin_abi.h defines, so it belongs with the ABI and is shared by
 * every board.  What is genuinely this board's is the address, and it is the
 * only thing here.
 *
 * [!] AND THE ADDRESS IS DECLARED MORE THAN ONCE, ON PURPOSE.  The firmware's
 * own script pins the same reservation, board.cmake states it again to the
 * shared check_plugin_image.py (GROVE_PLUGIN_GATE_BASE/END, issue #108), and
 * check_placement_budget.py holds it as its own literal.  A plugin that
 * read the address out of the firmware would agree with whatever it was handed;
 * the gates exist to catch the day the declarations diverge.  DO NOT generate
 * this file and a gate's constants from one CMake variable -- that would turn
 * four independent statements into one, and the gates into decoration.
 *
 * Prelinked, so moving it invalidates every container already in a blob store.
 */
MEMORY
{
  PLUGIN (rwx) : ORIGIN = 0x341E0000, LENGTH = 128K
}
