# ============================================================================
#  STM32F746G-DISCO -- the upstream submodules this board builds against.
#
#  Included by the top-level CMakeLists.txt BEFORE board.cmake; see the header of
#  boards/wio-lite-ai/submodules.cmake for why the list is per board and why the
#  entries are sentinel FILES rather than directories.
#
#  Ten mirrors: the F7 HAL + CMSIS device, the CMSIS core (shared with the H7
#  board), CoreMark, and the Eclipse ThreadX family this firmware links --
#  ThreadX, FileX + LevelX (the QSPI NOR filesystem), GUIX (the LCD GUI) and
#  NetX Duo (the Ethernet stack) -- plus the ST OV5640 camera driver.
#
#  [!] The first configure of this board is heavy: lib/guix is ~1.5 GB on its own.
#  That cost is confined here, which is the point of the split.
# ============================================================================
set(_submodule_sentinels
    "lib/stm32f7xx_hal_driver/Src/stm32f7xx_hal.c"
    "lib/cmsis_core/Include/core_cm7.h"
    "lib/cmsis_device_f7/Include/stm32f746xx.h"
    "lib/threadx/common/src/tx_thread_create.c"
    "lib/coremark/core_main.c"
    "lib/filex/common/src/fx_media_open.c"
    "lib/levelx/common/src/lx_nor_flash_open.c"
    "lib/guix/common/src/gx_system_initialize.c"
    "lib/netxduo/common/src/nx_ip_create.c"
    "lib/ov5640/ov5640.c")
