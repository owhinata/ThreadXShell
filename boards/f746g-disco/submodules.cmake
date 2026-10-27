# ============================================================================
#  STM32F746G-DISCO -- the upstream submodules this board builds against.
#
#  Included by the top-level CMakeLists.txt BEFORE board.cmake; see the header of
#  boards/wio-lite-ai/submodules.cmake for why the list is per board and why the
#  entries are sentinel FILES rather than directories.
#
#  Seven mirrors: CoreMark, the Eclipse ThreadX family this firmware links --
#  ThreadX, FileX + LevelX (the QSPI NOR filesystem), GUIX (the LCD GUI) and
#  NetX Duo (the Ethernet stack) -- and the ST OV5640 camera driver.  The ST
#  Cube set (F7 HAL, CMSIS device, CMSIS core) is not here: board.cmake pins it
#  per board and fetches it with cmake/fetch_upstream.cmake (issue #136).
#
#  [!] The first configure of this board is heavy: lib/guix is ~1.5 GB on its own.
#  That cost is confined here, which is the point of the split.
# ============================================================================
set(_submodule_sentinels
    "lib/threadx/common/src/tx_thread_create.c"
    "lib/coremark/core_main.c"
    "lib/filex/common/src/fx_media_open.c"
    "lib/levelx/common/src/lx_nor_flash_open.c"
    "lib/guix/common/src/gx_system_initialize.c"
    "lib/netxduo/common/src/nx_ip_create.c"
    "lib/ov5640/ov5640.c")
