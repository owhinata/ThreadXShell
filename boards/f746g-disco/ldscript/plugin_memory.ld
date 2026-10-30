/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The .plugin reservation, as this board declares it (issue #159 = #78 Step 4).
 *
 * [!] THIS IS THE BOARD'S HALF OF A LINKER SCRIPT SPLIT IN TWO.  The other half
 * -- asset/common/plugin.ld -- is the ABI's: its SECTIONS order IS the file
 * format svc/plugin/plugin_abi.h defines, so it belongs with the ABI and is
 * shared by every board.  What is genuinely this board's is the address, and it
 * is the only thing here.  (Step 4a only declares it; add_plugin() takes this
 * file from Step 4b.)
 *
 * The top 32 KB of internal SRAM.  Why there is written on the .plugin section
 * in STM32F746NGHx_FLASH.ld.
 *
 * [!] AND THE ADDRESS IS DECLARED MORE THAN ONCE, ON PURPOSE.  The firmware's
 * own script pins the same reservation, board.cmake states it again to the
 * shared check_plugin_image.py (F746_PLUGIN_GATE_BASE/END), and
 * cmake/check_f746_layout.py holds it as its own literal.  A plugin that read
 * the address out of the firmware would agree with whatever it was handed; the
 * gates exist to catch the day the declarations diverge.  DO NOT generate this
 * file and a gate's constants from one CMake variable.
 *
 * Prelinked, so moving it invalidates every container built for this board.
 */
MEMORY
{
  PLUGIN (rwx) : ORIGIN = 0x20048000, LENGTH = 32K
}
