# ============================================================================
#  Grove Vision AI V2 (Himax HX6538 WiseEye2) board definition.
#
#  Included by the top-level CMakeLists.txt with BOARD=grove-vision-ai-v2.
#  It owns everything board-specific: the SDK acquisition, the MCU flags, the
#  linker script, the one firmware and the post-build gates.
#
#  Firmware:
#    shell -- ThreadX + the interactive CLI console on UART0 (the board's USB-C
#             is a CH343P USB-UART bridge into PB0/PB1; 921600 8N1).
#
#  The chip has NO USB device controller: flashing goes over the same UART --
#  reset the board, the flash-resident Himax bootloader offers an xmodem menu,
#  and `--target flash` drives it (a manual step: press reset when prompted).
#  The flashed .img is a FULL image (bootloader + 2nd bootloader + memory
#  descriptors + signed app); rewriting the bootloader region every flash is
#  the vendor-standard flow.  Recovery from a corrupted flash: the in-chip
#  64 KB boot ROM + the BOOT_OPT strap -- see this board's README.md.
#
#  [!] This app does NOT configure the clock tree (same doctrine as the Wio):
#  it runs on whatever the bootloader programmed and reads the frequency back
#  through the SCU driver.  It runs entirely in the TrustZone SECURE state
#  (SDK "SEC_ONLY": SAU disabled, whole address space secure), which is why
#  ThreadX is built with TX_SINGLE_MODE_SECURE (port/threadx/tx_user.h).
#
#  The app is NOT XIP: the 2nd bootloader loads the ELF into ITCM (code,
#  0x10000000 secure alias) and DTCM (data, 0x30000000); see ldscript/.
# ============================================================================

# The name the `system` command prints; substituted into cmake/cli_version.h.in
# by the top-level CMakeLists.txt AFTER this file has been included.
set(BOARD_FW_NAME "Grove Vision AI V2 ThreadX Shell")

# The SDK startup file is C++ (startup_WE2_ARMCM55.cc).  The toolchain file
# already names CMAKE_CXX_COMPILER precisely so that this works (see the
# comment there); the top-level project() declares C and ASM only.
enable_language(CXX)

# --- Himax SDK (configure-time pinned fetch; NOT a submodule) ---------------
# Provides GROVE_SDK_ROOT / GROVE_SDK_APP_DIR, FATALs on any fetch problem.
include("${BOARD_DIR}/cmake/himax_sdk.cmake")
set(SDK "${GROVE_SDK_APP_DIR}")

# --- Target / common build options -----------------------------------------
# Cortex-M55 hard-float.  No explicit -mfpu: -mcpu=cortex-m55 enables the full
# FP + MVE (Helium) extension set, which is what the prebuilt driver archives
# were built against.  These flags MUST reach the ASM sources too (they gate
# __ARM_FP, which the ThreadX port asm uses to decide whether s16-s31 are part
# of a thread's context) -- hence they ride on the INTERFACE target.
set(MCU_OPTS -mcpu=cortex-m55 -mthumb -mfloat-abi=hard)

set(LDSCRIPT_APP "${BOARD_DIR}/ldscript/HX6538_CM55M_S.ld")

# --- Flash partition map (issues #44, #45, #85, #94) -------------------------
# Declared HERE, above everything that uses it, because two consumers need it:
# the flashing targets at the bottom of this file, and the firmware itself --
# cmd_nor.c is compiled with the partition edges so that `nor scan`'s labels and
# the layout the host checks cannot drift apart (issue #86).  Leaving them below
# meant the compile definitions expanded to nothing, which the C compiler caught
# only because an empty initialiser is a syntax error.
#
# check_flash_partitions.py turns this map into a checked property.  It runs as
# the first command of every flashing target, so a layout mistake stops before
# the serial port is opened.
#
# [!] THE MODELS ARE NOT PARTITIONS ANY MORE (issue #94, #49 Step 4b).  There
# were two -- `model-cls` at 0xB7B000 and `model-det` at 0xD20000, each with its
# own GROVE_MODEL_*_{FILE,ADDR,RESERVED} and a `--target flash-model-*` that
# wrote it -- plus a `blob-tail` reservation whose only reason to exist was that
# those two sat between the store and the bootloader's slot header.  Since issue
# #93 `nn open <name>` reads a model out of an asset slot, under the lease its
# CRC was checked with, so nothing reads a fixed address any more and the split
# has nothing left to protect.  Deleting them was gated on hardware evidence,
# recorded on issue #94 first: both models verify in the store, both open by
# name, and both run.
#
# [!] THE OLD COPIES ARE NOT ERASED BY THIS.  The table below covers those
# blocks and the writer may now reach them; the bytes stay where they are until
# something writes there.

# Fixed NOR / bootloader geometry, and everything that derives only from it
# (issue #85).  Separate file because those five numbers are MEASUREMENTS, not
# settings: they are plain variables and a disagreeing -D is a hard error, which
# is the enforcement -- see the header there, and test/test_flash_geometry.py.
include("${BOARD_DIR}/cmake/flash_geometry.cmake")

# [!] The asset store: reserved by issue #85, given a format by #92, given the
# rest of the part by #94.  It has a bounded WRITER since issue #88 Part C --
# port/nor/nor_write.c, with `nor erase` / `nor write` and `blob write` on top
# of it -- and this is the only partition that writer will touch.
#
# ONE RUN, from the end of the firmware slots to the START of the bootloader's
# slot header.  Its end IS GROVE_SLOT_HDR_ADDR rather than a number that agrees
# with it: the seam's interval, `nor scan`'s labels, nor_seam_limits in the ELF
# and check_flash_partitions.py's expectations all descend from that one
# declaration, so there is no pair of values that could drift.
#
# [!] WHAT IS THERE TODAY IS NOT ALL OURS, AND THE FIRST WRITE DESTROYS IT.  The
# factory SenseCraft firmware left a FlashDB KVDB at 0x300000 -- FlashDB's
# sector magic, at the offset FDB_WRITE_GRAN = 32 puts it -- and data at
# 0x400000 and 0x500000.  Nothing in this port reads any of it, and reflashing
# the factory image would not bring its contents back.  Accepted deliberately
# (2026-08-23) in exchange for the space.
set(GROVE_BLOB_ADDR "${GROVE_FW_RESERVED}")
math(EXPR GROVE_BLOB_RESERVED "${GROVE_SLOT_HDR_ADDR} - ${GROVE_BLOB_ADDR}"
     OUTPUT_FORMAT HEXADECIMAL)

# [!] THE DELETED CACHE ENTRIES ARE DROPPED, NOT LEFT TO ROT.  These six were
# CACHE STRING / CACHE FILEPATH, so a build directory configured before #94
# still carries them -- visible in cmake-gui, settable with -D, and read by
# nothing.  A layout value that can still be set and has no effect is worse than
# one that is gone: it reads as part of the map.  Same reasoning as
# GROVE_FW_RESERVED in flash_geometry.cmake.  (GROVE_BLOB_END and the
# GROVE_BLOB_TAIL_* pair went too, but they were derived plain variables and
# leave nothing behind.)
foreach(_dead GROVE_MODEL_CLS_FILE GROVE_MODEL_CLS_ADDR GROVE_MODEL_CLS_RESERVED
              GROVE_MODEL_DET_FILE GROVE_MODEL_DET_ADDR GROVE_MODEL_DET_RESERVED)
    unset(${_dead} CACHE)
endforeach()
unset(_dead)

set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

# The SDK's peripheral-IP configuration: which IP blocks exist (IP_<ip>) and
# which instances are populated (IP_INST_<inst>).  Copied verbatim from the
# SDK's drv_onecore_cm55m_s_only.mk via the donor build; the prebuilt
# libdriver.a and the SDK headers were built against exactly this set, so it
# is not a menu -- treat it as part of the ABI.
set(SDK_IP_LIST
    scu uart spi i3c_mst isp iic mb timer watchdog rtc
    cdm edm jpeg xdma dp inp tpg inp1bitparser sensorctrl
    gpio i2s pdm i3c_slv vad swreg_aon swreg_lsc dma
    ppc pmu mpc hxautoi2c_mst csirx csitx adcc pwm
    inpovparser adcc_hv u55 2x2 5x5)
set(SDK_IP_INSTANCES
    RTC0 RTC1 RTC2
    TIMER0 TIMER1 TIMER2 TIMER3 TIMER4 TIMER5 TIMER6 TIMER7 TIMER8
    WDT0 WDT1
    DMA0 DMA1 DMA2 DMA3
    UART0 UART1 UART2
    IIC_HOST_SENSOR IIC_HOST IIC_HOST_MIPI
    IIIC_SLAVE0 IIIC_SLAVE1
    SSPI_HOST QSPI_HOST OSPI_HOST SSPI_SLAVE
    GPIO_G0 GPIO_G1 GPIO_G2 GPIO_G3 SB_GPIO AON_GPIO
    I2S_HOST I2S_SLAVE
    PWM0 PWM1 PWM2 ADCC ADCC_HV)
set(SDK_IP_DEFINES "")
foreach(_ip IN LISTS SDK_IP_LIST)
    list(APPEND SDK_IP_DEFINES "IP_${_ip}")
endforeach()
foreach(_inst IN LISTS SDK_IP_INSTANCES)
    list(APPEND SDK_IP_DEFINES "IP_INST_${_inst}")
endforeach()

# Usage requirements shared by the target (includes, defs, MCU flags, link).
add_library(bsp_iface INTERFACE)
target_include_directories(bsp_iface INTERFACE
    "${BOARD_DIR}/include"
    "${GEN_DIR}"
    # SDK header surface, donor-identical.  The SDK bundles its own CMSIS
    # (core_cm55.h etc.), so no lib/ CMSIS mirror is involved.
    "${SDK}/CMSIS"
    "${SDK}/CMSIS/Driver/Include"
    "${SDK}/device"
    "${SDK}/device/inc"
    "${SDK}/device/clib"
    "${SDK}/drivers"
    "${SDK}/drivers/inc"
    "${SDK}/drivers/seconly_inc"
    "${SDK}/board"
    "${SDK}/board/epii_evb"
    "${SDK}/board/epii_evb/config"
    "${SDK}/interface"
    "${SDK}/library/common"
    "${SDK}/library/pwrmgmt"
    "${SDK}/library/pwrmgmt/seconly_inc"
    "${SDK}/customer/sec_inc/seeed"
    "${SDK}/trustzone"
    "${SDK}/trustzone/tz_cfg")
target_compile_definitions(bsp_iface INTERFACE
    # Toolchain / core selection (SDK cmsis_core layer)
    __GNU__ __NEWLIB__ ARMCM55 CM55_BIG
    # Device (SDK device layer): silicon rev 3.0, WLCSP65 (the Grove board's
    # package), 0.9 V core -- donor-identical.
    IC_VERSION=30 IC_PACKAGE_WLCSP65 COREV_0P9V
    # Board flavour + libraries the compiled SDK sources expect
    seeed EPII_EVB LIB_COMMON LIB_PWRMGMT
    # TrustZone: whole app secure, SAU disabled (trustzone_cfg.c SEC_ONLY path)
    TRUSTZONE TRUSTZONE_CFG TRUSTZONE_SEC TRUSTZONE_SEC_ONLY
    # [!] The RTOS seam: removes the SDK's strong SysTick_Handler/SVC_Handler
    # and its SysTick_Config() calls (device/system_WE2_ARMCM55.c,
    # device/WE2_core.c) so ThreadX can own the tick and the vectors.
    ENABLE_OS
    ${SDK_IP_DEFINES})
target_compile_options(bsp_iface INTERFACE
    ${MCU_OPTS} -Wall -fdata-sections -ffunction-sections -g -gdwarf-2
    # CMSE intrinsics: SystemInit and trustzone_cfg.c compile TZ paths under
    # __ARM_FEATURE_CMSE == 3, which only -mcmse provides.  C/C++ only -- the
    # assembler has no such option.
    $<$<COMPILE_LANGUAGE:C,CXX>:-mcmse>
    # The SDK startup is C++; keep it freestanding like the donor build.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti -fno-exceptions -fno-threadsafe-statics>)
target_link_options(bsp_iface INTERFACE
    ${MCU_OPTS} -specs=nano.specs -specs=nosys.specs
    # Donor parity: the secure-only build carries --cmse-implib even though no
    # import library is consumed; kept identical to reduce bring-up variables.
    -Wl,--cmse-implib
    # The 2nd bootloader loads code into ITCM (RAM), so the image inherently
    # has a "RWX" LOAD segment; the donor build silences the same warning.
    -Wl,--no-warn-rwx-segments
    -Wl,--gc-sections -Wl,--print-memory-usage)

# --- SDK sources compiled from source ---------------------------------------
# The peripheral drivers themselves are PREBUILT (prebuilt_libs/gnu/libdriver.a
# -- no sources exist in the SDK).  What compiles from source is the thin layer
# the archive links back into: the device core (runtime vector install +
# cache/TCM helpers), startup, SystemInit, the driver/timer interface shims and
# the TrustZone SEC_ONLY configuration.
#
# Deliberately NOT compiled (reviewed decision, see the board README):
#  - board/epii_evb/board.c        -- calls console_setup(), which lives in the
#                                     SDK clib we do not link
#  - device/clib/*                 -- console + printf retarget; this port owns
#                                     _write/_sbrk (src/retarget.c)
#  - library/common/xprintf.c      -- includes console_io.h; libdriver's one
#                                     unresolved x* symbol (xprintf) is
#                                     satisfied by src/xprintf_shim.c instead,
#                                     which routes into the dmesg log ring
#  - interface/timer_interface.c   -- issue #30.  Its only content is
#                                     hx_drv_timer_cm55x_delay_ms/_us, which
#                                     forward to the TIMER_ID_3 vendor entry
#                                     points; port/sdk_seam/timer_seam.c owns
#                                     those two symbols now (see the --wrap
#                                     block below).  Compiling it too would
#                                     leave a shadow definition of a name the
#                                     placement gate bars, kept alive only by
#                                     --gc-sections; dropping it makes a
#                                     removed --wrap flag a LINK ERROR instead.
set(SDK_SOURCES
    "${SDK}/device/WE2_core.c"
    "${SDK}/device/system_WE2_ARMCM55.c"
    "${SDK}/device/startup_WE2_ARMCM55.cc"
    "${SDK}/interface/driver_interface.c"
    "${SDK}/trustzone/tz_cfg/trustzone_cfg.c"
    "${SDK}/board/epii_evb/pinmux_init.c"
    "${SDK}/board/epii_evb/platform_driver_init.c")

# --- Vendor timer API seam (issue #30) --------------------------------------
# The prebuilt camera archives call four hx_drv_timer_* entry points.  This
# port bars that whole prefix from the image (check_placement_budget.py, and
# AGENTS.md records it as an invariant) because TIMER2 is the execution-profile
# time source and no name-based check can tell which timer id a generic call
# carries -- so linking those archives unchanged would fail the build.
#
# The seam redirects the four references to board-owned implementations
# (port/sdk_seam/timer_seam.c) that never call __real_*.  Disassembly of
# libsensordp.a shows all 41 hw_start/hw_stop call sites pass a constant
# TIMER_ID_0 and the delays resolve to TIMER_ID_3, so this is a GATE conflict,
# not a hardware conflict -- and after the wrap no hx_drv_timer_* symbol except
# the permitted hx_drv_timer_init survives, which leaves the gate and the
# invariant untouched.  __wrap_-prefixed names do not match the barred prefix.
#
# An argument-inspecting gate was considered and rejected: AGENTS.md makes both
# the blanket ban AND "do not weaken the gate" invariants, and a general
# argument analysis would be a brittle whole-program binary pass (tail calls,
# address-taken relocations, function pointers, linker veneers).
set(SDK_TIMER_WRAP_SYMBOLS
    hx_drv_timer_hw_start
    hx_drv_timer_hw_stop
    hx_drv_timer_cm55x_delay_ms
    hx_drv_timer_cm55x_delay_us)
set(SDK_TIMER_WRAP_FLAGS "")
foreach(_sym IN LISTS SDK_TIMER_WRAP_SYMBOLS)
    list(APPEND SDK_TIMER_WRAP_FLAGS "-Wl,--wrap=${_sym}")
endforeach()

set(LIBDRIVER  "${SDK}/prebuilt_libs/gnu/libdriver.a")
set(LIBPWRMGMT "${SDK}/prebuilt_libs/gnu/libpwrmgmt.a")

# The camera datapath archives (issue #35).  libsensordp.a is the sensor
# control / INP / demosaic / WDMA datapath library and libextdevice.a the CIS
# (sensor I2C) layer.  Both are prebuilt -- the SDK ships no sources for them
# and its own makefile "build" rule is a copy out of prebuilt_libs/.
#
# libcommon.a stays OUT of the link on purpose: it defines console_getchar /
# console_putchar and the SysTick helpers that check_placement_budget.py bars.
# Leaving it out costs nothing -- the two archives above resolve against
# libdriver.a plus eight symbols this port already owns (board_delay_ms and the
# two wrapped delays from port/sdk_seam/timer_seam.c, xprintf from
# src/xprintf_shim.c, and four drv_interface_* from the SDK's
# interface/driver_interface.c, which SDK_SOURCES compiles).
# QSPI NOR access (issue #44): needed ONLY to enable the memory-mapped read
# window the model is parsed through.  Its erase/write entry points are on
# check_placement_budget.py's forbidden list -- this flash holds the
# bootloader, and --gc-sections means their presence would mean a caller.
set(LIBSPIEEPROM "${SDK}/prebuilt_libs/gnu/lib_spi_eeprom.a")
set(LIBSENSORDP  "${SDK}/prebuilt_libs/gnu/libsensordp.a")
set(LIBEXTDEVICE "${SDK}/prebuilt_libs/gnu/libextdevice.a")

# --- Vendor NOR write-path seam (issue #88) ----------------------------------
# The comment above is what the QSPI archive was linked for in issue #44, and it
# stops being the whole story here: issue #49's blob needs a WRITE path on the
# same part that carries the bootloader, the firmware image and the
# bootloader's slot header.
#
# So the four inner entry points are redirected to port/sdk_seam/nor_seam.c,
# which bounds erase and program to the `blob` interval in units this die has
# actually been measured erasing, and refuses chip erase and the word-at-a-time
# programmer outright -- see nor_seam.h for what that does and does not prove.
#
# THE INNER (hx_lib_qspi_*) NAMES, NOT THE OUTER (hx_lib_spi_*) ONES.  The
# outer forms in spi_eeprom_comm.o are thin forwarders that pick a bus by id and
# tail into these; wrapping the outer pair would leave the inner ones reachable
# directly.  Wrapping the inner ones covers both, because the forwarder's own
# call is an undefined reference that --wrap rewrites.
#
# [!] AND UNLIKE THE TIMER SEAM, THIS ONE CALLS __real_.  That makes the claim a
# statement about WHO MAY REACH the vendor code rather than about whether it is
# present, which no check over the finished ELF can settle -- so
# cmake/check_nor_seam.py audits relocations in the linker's inputs and
# classifies them live/discarded from the linker's own map.
set(SDK_NOR_WRAP_SYMBOLS
    hx_lib_qspi_eeprom_erase_sector
    hx_lib_qspi_eeprom_write
    hx_lib_qspi_eeprom_erase_all
    hx_lib_qspi_eeprom_word_write)
set(SDK_NOR_WRAP_FLAGS "")
foreach(_sym IN LISTS SDK_NOR_WRAP_SYMBOLS)
    list(APPEND SDK_NOR_WRAP_FLAGS "-Wl,--wrap=${_sym}")
endforeach()

# --- ThreadX ----------------------------------------------------------------
# Core sources + the Cortex-M55/GNU port asm.  The port ships its example
# _tx_initialize_low_level in example_build/ (outside the src/ glob), so the
# board supplies its own in port/threadx/tx_glue.c -- same pattern as the
# other two boards.  ONE executable compiled uniformly
# (TX_INCLUDE_USER_DEFINE_FILE + port/threadx on the include path) so the
# ThreadX core, the shell core and the app agree on the TX_THREAD layout (ABI).
# tx_user.h defines TX_SINGLE_MODE_SECURE, which also compiles the port's six
# secure-stack sources down to empty objects.
set(TX_DIR  "${CMAKE_SOURCE_DIR}/lib/threadx")
set(TX_PORT "${TX_DIR}/ports/cortex_m55/gnu")
file(GLOB TX_CORE "${TX_DIR}/common/src/*.c")
list(FILTER TX_CORE EXCLUDE REGEX "tx_misra\\.c$")
file(GLOB TX_ASM  "${TX_PORT}/src/*.S")
list(FILTER TX_ASM  EXCLUDE REGEX "tx_misra\\.S$")
# Execution Profile Kit (`thread` cpu%, issue #25).  It lives under utility/,
# not common/src, so the TX_CORE glob above does not pick it up -- add it
# explicitly.  Its time source is Himax TIMER2, brought up and owned by
# port/threadx/tx_glue.c (see tx_user.h).
set(TX_EPK "${TX_DIR}/utility/execution_profile_kit/tx_execution_profile.c")

# ThreadX idle WFI sleep (TX_ENABLE_WFI in tx_user.h).  Default ON; build with
# -DBSP_ENABLE_WFI=OFF for a busy-idle variant that is easier to attach over
# SWD (a WFI-sleeping core needs connect-under-reset).  The define has to reach
# the port ASSEMBLY too -- tx_thread_schedule.S is what contains the WFI --
# which is why it rides on the `shell` target rather than on one source file.
option(BSP_ENABLE_WFI "Enable ThreadX idle WFI power saving" ON)

# --- Shell sources ----------------------------------------------------------
# Board-independent files come from the shared shell/ and svc/ trees; the ones
# that reach for the SDK drivers or the HX6538 memory map live under this
# board's own backend/, cmds/ and svc/.
set(SHELL_SOURCES
    "${CMAKE_SOURCE_DIR}/shell/core/cli_core.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_complete.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_registry.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_edit.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_history.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_job.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_parse.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_printf.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_session.c"
    "${BOARD_DIR}/backend/cli_backend_uart.c"
    "${CMAKE_SOURCE_DIR}/shell/backend/cli_backend_dummy.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_builtin.c"
    "${BOARD_DIR}/cmds/cmd_system.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_thread.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_console.c"
    "${BOARD_DIR}/cmds/cmd_free.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_sleep.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_watch.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_jobs.c"
    "${BOARD_DIR}/cmds/cmd_devmem.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_dmesg.c"
    "${BOARD_DIR}/cmds/cmd_crash.c"
    "${BOARD_DIR}/cmds/bench_gate.c"
    "${BOARD_DIR}/cmds/cmd_coremark.c"
    "${BOARD_DIR}/cmds/cmd_membench.c"
    "${BOARD_DIR}/cmds/cmd_epk.c"
    "${BOARD_DIR}/cmds/cmd_lcd.c"
    "${BOARD_DIR}/cmds/cmd_mve.c"
    "${BOARD_DIR}/cmds/cmd_camera.c"
    "${BOARD_DIR}/cmds/cmd_nn.c"
    "${BOARD_DIR}/cmds/cmd_nor.c"
    "${BOARD_DIR}/cmds/cmd_blob.c"
    # YMODEM receive over the console (issue #92).  The protocol core is the
    # shared svc/ one; cmd_xfer.c is the twenty lines that wire it to the raw
    # console API.  Receive only -- nothing on this board produces a file to
    # send -- and a third copy of that wiring on purpose: sharing it needs a
    # board hook for "not over telnet", which is a mechanism and a separate
    # issue.
    "${BOARD_DIR}/cmds/cmd_xfer.c"
    "${CMAKE_SOURCE_DIR}/svc/ymodem.c"
    "${CMAKE_SOURCE_DIR}/svc/fmt.c"
    # CRC-32/ISO-HDLC (issue #92).  The blob store stamps every asset with the
    # checksum of the stream that arrived; wio's blob borrows FlashDB's, which
    # this board does not link.  Freestanding, and host-tested against zlib and
    # against a table-free reference (shell/test/test_crc32.c).
    "${CMAKE_SOURCE_DIR}/svc/crc32.c"
    # Camera frame ring (issue #35).  Freestanding: it depends on <stdint.h>
    # and an injected lock vtable only, which is why the same file serves all
    # three boards and has a host unit test (shell/test/test_frame_pipeline.c).
    "${CMAKE_SOURCE_DIR}/svc/frame_pipeline.c"
    "${BOARD_DIR}/svc/timebase.c"
    "${BOARD_DIR}/svc/log.c")

# --- CoreMark object library (run as the shell `coremark` command) ----------
# Built once at -O3 -funroll-loops and linked into the shell firmware below;
# cmd_coremark.c calls coremark_main().  core_main.c is compiled with
# -Dmain=coremark_main so its main() does not clash with the app main() in
# src/main.c.  MEM_METHOD=MEM_STATIC puts the 2 KB working set in .bss (DTCM):
# the newlib heap here is only 8 KB (ldscript __HEAP_SIZE) while DTCM has
# ~180 KB spare, so the wio port's malloc-per-run trade does not apply.
# ITERATIONS=0 -> CoreMark auto-calibrates the run time.
#
# [!] -fno-tree-vectorize is the ONE place this option survived issue #42, and
# the reason changed with it.  It is no longer about MVE safety -- the ban is
# gone and the hardware preserves VPR -- it is about BASELINE COMPARABILITY: the
# published 3.13 CoreMark/MHz was measured with these flags, and a CoreMark
# score means nothing apart from the flags it was built with (which is why
# core_portme.h reports them).  A vectorised score would be a perfectly valid
# different result; taking it means re-measuring and restating every comparison
# that quotes the old one, which is a deliberate step and not a side effect of
# lifting a ban.
set(CMK_DIR "${CMAKE_SOURCE_DIR}/lib/coremark")
add_library(coremark_obj OBJECT
    "${CMK_DIR}/core_list_join.c"
    "${CMK_DIR}/core_main.c"
    "${CMK_DIR}/core_matrix.c"
    "${CMK_DIR}/core_state.c"
    "${CMK_DIR}/core_util.c"
    "${BOARD_DIR}/port/coremark/core_portme.c")
target_link_libraries(coremark_obj PUBLIC bsp_iface)
target_include_directories(coremark_obj PRIVATE
    "${CMK_DIR}" "${BOARD_DIR}/port/coremark"
    "${BOARD_DIR}/port/threadx"                 # tx_user.h (tick rate)
    "${TX_DIR}/common/inc" "${TX_PORT}/inc"     # tx_time_get()
    "${TX_DIR}/utility/execution_profile_kit")  # tx_api.h pulls it under EPK
target_compile_definitions(coremark_obj PRIVATE
    TX_INCLUDE_USER_DEFINE_FILE
    ITERATIONS=0 MEM_METHOD=MEM_STATIC)
target_compile_options(coremark_obj PRIVATE -O3 -funroll-loops -fno-tree-vectorize)
# Rename the benchmark entry so it does not collide with the app main().
set_source_files_properties("${CMK_DIR}/core_main.c" PROPERTIES
    COMPILE_DEFINITIONS "main=coremark_main")

# --- TFLite Micro + Ethos-U55 core driver (issue #44) ------------------------
# Built FROM SOURCE, not from prebuilt_libs/.  The only 2412-tag archive the SDK
# ships is the CMSIS-NN variant, and CMSIS-NN is Helium code.  That was a bar to
# linking it while issue #42's ban stood; the ban is gone, but nothing is gained
# by swapping a source build for an archive whose kernels this configuration
# does not register.
# Building from source costs nothing here because there is nothing to replace:
# the op resolver registers AddEthosU() and NOTHING else, so not one CPU kernel
# is linked.  A Vela-compiled model folds every conv/pool/activation into the
# single `ethos-u` custom operator, which is why the donor's classification app
# gets away with MicroMutableOpResolver<1>.
#
# The file list is the transitive closure of that configuration, derived from
# the SDK's own tflmtag2412_u55tag2411.mk and then trimmed to what actually
# links -- roughly a fifth of the ~130 sources the .mk names, because all the
# reference and CMSIS-NN kernels drop out with the kernels themselves.
set(TFLM "${SDK}/library/inference/tflmtag2412_u55tag2411")
set(ETHOSU_DRV "${TFLM}/third_party/ethos_u_core_driver")

add_library(tflm_obj OBJECT
    # Arm Ethos-U core driver (source, unlike the peripheral drivers)
    "${ETHOSU_DRV}/src/ethosu_driver.c"
    "${ETHOSU_DRV}/src/ethosu_device_u55_u65.c"
    "${ETHOSU_DRV}/src/ethosu_pmu.c"
    # The single operator this port registers
    "${TFLM}/tensorflow/lite/micro/kernels/ethos_u/ethosu.cc"
    "${TFLM}/tensorflow/lite/micro/kernels/kernel_util.cc"
    # Interpreter + allocator + memory planning
    "${TFLM}/tensorflow/lite/micro/micro_interpreter.cc"
    "${TFLM}/tensorflow/lite/micro/micro_interpreter_graph.cc"
    "${TFLM}/tensorflow/lite/micro/micro_interpreter_context.cc"
    "${TFLM}/tensorflow/lite/micro/micro_allocator.cc"
    "${TFLM}/tensorflow/lite/micro/micro_allocation_info.cc"
    "${TFLM}/tensorflow/lite/micro/micro_context.cc"
    "${TFLM}/tensorflow/lite/micro/micro_op_resolver.cc"
    "${TFLM}/tensorflow/lite/micro/micro_resource_variable.cc"
    "${TFLM}/tensorflow/lite/micro/micro_profiler.cc"
    "${TFLM}/tensorflow/lite/micro/micro_log.cc"
    "${TFLM}/tensorflow/lite/micro/micro_utils.cc"
    "${TFLM}/tensorflow/lite/micro/micro_time.cc"
    "${TFLM}/tensorflow/lite/micro/memory_helpers.cc"
    "${TFLM}/tensorflow/lite/micro/debug_log.cc"
    "${TFLM}/tensorflow/lite/micro/flatbuffer_utils.cc"
    "${TFLM}/tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc"
    "${TFLM}/tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc"
    "${TFLM}/tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc"
    "${TFLM}/tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc"
    "${TFLM}/tensorflow/lite/micro/memory_planner/linear_memory_planner.cc"
    "${TFLM}/tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.cc"
    "${TFLM}/tensorflow/lite/micro/tflite_bridge/micro_error_reporter.cc"
    # Schema / type plumbing
    "${TFLM}/tensorflow/lite/core/api/flatbuffer_conversions.cc"
    "${TFLM}/tensorflow/lite/core/api/tensor_utils.cc"
    "${TFLM}/tensorflow/lite/core/c/common.cc"
    "${TFLM}/tensorflow/lite/kernels/kernel_util.cc"
    "${TFLM}/tensorflow/lite/kernels/internal/common.cc"
    "${TFLM}/tensorflow/lite/kernels/internal/quantization_util.cc"
    "${TFLM}/tensorflow/lite/kernels/internal/tensor_ctypes.cc"
    "${TFLM}/tensorflow/lite/kernels/internal/runtime_shape.cc"
    "${TFLM}/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc"
    "${TFLM}/tensorflow/compiler/mlir/lite/schema/schema_utils.cc")

# [!] The inference timeout has ONE definition, and it is in the C header
# (issue #48).  Parsed rather than restated so the two cannot drift: the value
# is load-bearing for camera.c's stop join, which budgets two of these waits.
# A configure-time FATAL_ERROR is the point -- silently falling back to a
# default here would put the number back to being written twice.
file(STRINGS "${BOARD_DIR}/port/npu/npu_hw.h" _npu_timeout_line
     REGEX "^#define[ \t]+NPU_INFERENCE_TIMEOUT_TICKS[ \t]+[0-9]+u?[ \t]*$")
if(NOT _npu_timeout_line)
    message(FATAL_ERROR
            "could not find NPU_INFERENCE_TIMEOUT_TICKS in port/npu/npu_hw.h; "
            "it is the single source of truth for the ethos-u inference wait")
endif()
string(REGEX REPLACE "^#define[ \t]+NPU_INFERENCE_TIMEOUT_TICKS[ \t]+([0-9]+u?)[ \t]*$"
       "\\1" GROVE_NPU_INFERENCE_TIMEOUT_TICKS "${_npu_timeout_line}")

target_link_libraries(tflm_obj PUBLIC bsp_iface)
target_include_directories(tflm_obj PUBLIC
    "${TFLM}"
    "${TFLM}/third_party/flatbuffers/include"
    "${TFLM}/third_party/gemmlowp"
    "${TFLM}/third_party/ruy"
    "${ETHOSU_DRV}/include")
target_compile_definitions(tflm_obj PUBLIC
    TFLM2412_U55TAG2411
    TF_LITE_STATIC_MEMORY          # no dynamic tensor resizing; arena only
    TF_LITE_MCU_DEBUG_LOG
    ETHOS_U ETHOSU55 ETHOSU_ARCH=u55
    ETHOSU_LOG_SEVERITY=ETHOSU_LOG_WARN
    # Finite, in ThreadX ticks -- npu_rtos.c defines the unit.  The header
    # would otherwise default this to "wait forever" and a lost NPU interrupt
    # would suspend the calling shell job with no way back.
    #
    # [!] PARSED from port/npu/npu_hw.h, not written here (issue #48).  The
    # value used to exist in both places with only this one live, so the
    # header's constant was dead and would have drifted the first time somebody
    # tuned it -- and since #48 the number is load-bearing for the camera's
    # stop join, which reasons about two of these waits.
    ETHOSU_SEMAPHORE_WAIT_INFERENCE=${GROVE_NPU_INFERENCE_TIMEOUT_TICKS})
# -fno-tree-vectorize is GONE from here (issue #42).  It was on the whole set
# because MVE is available to every translation unit and the predication scan
# would have failed the build on what the auto-vectoriser emitted.  The scan is
# deleted and the ban with it; nothing here needs the compiler held back.
#
# -Wno-* : the SDK's TFLM snapshot is upstream code compiled here with warnings
# the rest of this port keeps on.  Scoped to this target only.
target_compile_options(tflm_obj PRIVATE
    -Os
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter -Wno-sign-compare>)

# --- The shell firmware ------------------------------------------------------
# The sources compile into an OBJECT library rather than straight into the
# executable so the seam probe below can link the SAME objects a second time
# without recompiling them (issue #30).  Nothing else changes: `shell` still
# links exactly this object set in exactly this order.
# [!] ONE VARIABLE FOR THE SHARED DECODER'S PATH, used by the source list, the
# -O3 property and the storage gate below (issue #97).  set_source_files_properties()
# SILENTLY IGNORES a path that is not a source of the target, so a file moved out
# from under it loses its optimisation level with no warning and no gate -- and
# the board README's frame rates depend on this one being built -O3.  Spelling the
# path once removes the way those two can drift apart; the checks after the target
# is defined catch the other way, where the path is stale on both.
get_filename_component(GROVE_SHARED_DECODER
                       "${CMAKE_SOURCE_DIR}/svc/blazeface.c" ABSOLUTE)
if(NOT EXISTS "${GROVE_SHARED_DECODER}")
    message(FATAL_ERROR
        "shared BlazeFace decoder not found:\n  ${GROVE_SHARED_DECODER}\n"
        "This board builds it (issue #97); it is not optional.")
endif()

add_library(shell_objs OBJECT
    "${BOARD_DIR}/src/main.c"
    "${BOARD_DIR}/src/fault.c"
    "${BOARD_DIR}/src/retarget.c"
    "${BOARD_DIR}/src/malloc_lock.c"
    "${BOARD_DIR}/src/xprintf_shim.c"
    # The blob staging reservation (issue #92, #49 Step 2).  64 KB of the
    # loadable SRAM window claimed for the buffer a transfer programs out of.
    # It lands before the transfer that fills it because it is a claim on
    # memory: check_placement_budget.py pins symbol -> size -> section ->
    # region, and settling that while the window has 430 KB spare is cheaper
    # than settling it when something needs the room.
    "${BOARD_DIR}/src/blob_stage.c"
    # The asset store (issue #92).  blob_map.c carves the seam's writable
    # interval into slots and blob_state.c decides what the bytes at the top of
    # one mean -- both pure, both walked by test/test_blob_{map,state}.c on the
    # host -- and blob.c is the part that needs the board: the lease, the cache
    # invalidate, and the arithmetic that turns a slot into a pointer into the
    # memory-mapped window.
    "${BOARD_DIR}/src/blob_map.c"
    "${BOARD_DIR}/src/blob_state.c"
    "${BOARD_DIR}/src/blob.c"
    # The write coordinator (issue #92).  Takes its operations as a vtable so
    # that test/test_blob_write.c can fail each of them and watch the
    # reservation and the console claim come back exactly once -- which is the
    # part of a transfer hardware cannot demonstrate.
    "${BOARD_DIR}/src/blob_write.c"
    "${BOARD_DIR}/port/threadx/fp_enforce.c"
    "${BOARD_DIR}/port/threadx/tx_glue.c"
    "${BOARD_DIR}/port/sdk_seam/timer_seam.c"
    "${BOARD_DIR}/port/sdk_seam/epk_irq_wrap.c"
    # External NOR lifecycle (issue #86).  Owns the QSPI master, the XIP window
    # and the interrupt the vendor library uses for DMA -- which used to be
    # brought up inside npu_hw_init()'s EPK snapshot, so `nn close` disabled it.
    "${BOARD_DIR}/port/nor/nor_state.c"
    "${BOARD_DIR}/port/nor/nor_flash.c"
    # The bounded write transaction (issue #88 Part C).  nor_span.c is the pure
    # arithmetic the host test walks -- which bytes a request names, and what an
    # erase rounds them to -- and nor_write.c is the one object that may call
    # the vendor's erase and program entry points (see NOR_SEAM_CALLERS below).
    "${BOARD_DIR}/port/nor/nor_span.c"
    "${BOARD_DIR}/port/nor/nor_write.c"
    # The bounded door to the vendor's NOR write path (issue #88).  In
    # port/sdk_seam/ and not port/nor/ because it is the same KIND of thing as
    # timer_seam.c: a board-owned definition of a name -Wl,--wrap redirects.
    "${BOARD_DIR}/port/sdk_seam/nor_seam.c"
    "${BOARD_DIR}/port/lcd/lcd_st7789.c"
    "${BOARD_DIR}/port/camera/cam_convert.c"
    "${BOARD_DIR}/port/camera/cam_mipi_calc.c"
    "${BOARD_DIR}/port/camera/cam_auto.c"
    "${BOARD_DIR}/port/camera/cam_dp.c"
    "${BOARD_DIR}/port/camera/cam_wdma3.c"
    "${BOARD_DIR}/port/camera/cam_sensor.c"
    "${BOARD_DIR}/port/camera/cam_sensor_ov5647.c"
    "${BOARD_DIR}/port/camera/cam_state.c"
    "${BOARD_DIR}/port/camera/cam_edm.c"
    "${BOARD_DIR}/port/camera/camera.c"
    "${BOARD_DIR}/port/camera/cam_lcd_sink.c"
    # Ethos-U55 inference glue (issue #44).  The C++ interpreter is contained
    # behind port/npu/npu.h; nothing above it sees a TFLite type.
    "${BOARD_DIR}/port/npu/npu_tflm.cc"
    "${BOARD_DIR}/port/npu/npu_arena.c"
    "${BOARD_DIR}/port/npu/npu_rtos.c"
    "${BOARD_DIR}/port/npu/npu_cache.c"
    "${BOARD_DIR}/port/npu/npu_payload.c"
    "${BOARD_DIR}/port/npu/npu_model_scan.cc"
    "${BOARD_DIR}/port/npu/npu_hw.c"
    "${BOARD_DIR}/port/npu/nn_preproc.c"
    "${BOARD_DIR}/port/npu/nn_overlay.c"
    # Model-specific post-processing (issue #45), now SHARED with the other two
    # boards (issue #97).  The decoder is above npu.h, which stays model-agnostic;
    # it sees tensor DESCRIPTORS and not the interpreter, which is what lets the
    # host test drive the real decoder.  nn_decoder.c is this board's translation
    # from npu_tensor into svc/tensor.h -- the only file here that knows both --
    # and it owns the decoder's state and scratch, because the shared TU owns
    # nothing (see the storage gate below).
    "${GROVE_SHARED_DECODER}"
    "${BOARD_DIR}/port/npu/nn_decoder.c"
    ${SHELL_SOURCES}
    ${SDK_SOURCES}
    ${TX_CORE} ${TX_ASM} ${TX_EPK})

add_executable(shell $<TARGET_OBJECTS:shell_objs>)
target_link_libraries(shell PRIVATE bsp_iface coremark_obj tflm_obj
    -Wl,--start-group "${LIBDRIVER}" "${LIBPWRMGMT}"
                      "${LIBSENSORDP}" "${LIBEXTDEVICE}" "${LIBSPIEEPROM}" -Wl,--end-group)
# CoreMark's canonical report prints its score with %f; pull in newlib's float
# printf (newlib-nano omits it by default).  This is also why src/malloc_lock.c
# exists: that conversion allocates from the heap, now from several threads.
target_link_options(shell PRIVATE -u _printf_float
    ${SDK_TIMER_WRAP_FLAGS} ${SDK_NOR_WRAP_FLAGS})
target_include_directories(shell_objs PRIVATE
    "${BOARD_DIR}/src"
    "${BOARD_DIR}/port/threadx"
    "${BOARD_DIR}/port/sdk_seam"
    "${BOARD_DIR}/port/nor"
    "${BOARD_DIR}/port/lcd"
    "${BOARD_DIR}/port/camera"
    "${BOARD_DIR}/port/npu"
    # Header surface of the two camera archives (issue #35): the CIS (sensor
    # I2C) layer and the sensor datapath library.  Not in bsp_iface because
    # only port/camera/ has any business calling them.
    "${SDK}/external/cis"
    "${SDK}/library/sensordp/inc"
    "${SDK}/library/spi_eeprom"
    # The IMX219 mode table (.i) is included from the SDK tree rather than
    # copied, so it stays tied to the pinned SHA (issue #35).
    "${SDK}/app/scenario_app/tflm_yolov8_od/cis_sensor/cis_imx219"
    "${SDK}/app/scenario_app/tflm_yolov8_od/cis_sensor/cis_ov5647"
    "${BOARD_DIR}/backend"
    "${BOARD_DIR}/cmds"         # bench_gate.h: shared by coremark + membench
    "${CMAKE_SOURCE_DIR}/shell/include"
    "${CMAKE_SOURCE_DIR}/shell/core"
    "${CMAKE_SOURCE_DIR}/shell/backend"
    "${CMAKE_SOURCE_DIR}/shell/cmds"
    "${CMAKE_SOURCE_DIR}/svc"
    "${BOARD_DIR}/svc"          # log.h / timebase.h: the board's services,
                                # which the shared cmd_dmesg.c / cmd_sleep.c
                                # consume
    "${TX_DIR}/common/inc"
    "${TX_DIR}/utility/execution_profile_kit"   # tx_execution_profile.h
    "${TX_PORT}/inc")
target_link_libraries(shell_objs PRIVATE bsp_iface tflm_obj)
target_compile_definitions(shell_objs PRIVATE
    TX_INCLUDE_USER_DEFINE_FILE        # -> port/threadx/tx_user.h
    BSP_ENABLE_WFI=$<BOOL:${BSP_ENABLE_WFI}>   # gates TX_ENABLE_WFI (tx_user.h)
    CLI_ENABLE_DANGEROUS_CMDS=1        # reboot / devmem / crash
    CLI_INSTANCE_STACK_SIZE=4096       # headroom for cli_print (wio parity)
    CLI_BG_JOB_STACK_SIZE=4096
    # CM55M core clock as configured by the bootloader: 400 MHz, CONFIRMED on
    # hardware 2026-08-13 (the banner prints the runtime SCU read-back and
    # warns on any mismatch with this constant -- it printed 400000000 Hz and
    # no warning).  The compile-time SDK config is a 24 MHz placeholder and
    # must never be used for this; udelay() reads SystemCoreClock directly.
    CLI_CPU_CYCLES_PER_US=400
    # NN_MODEL_{CLS,DET}_OFFSET are GONE (issue #93) and so are the model
    # partitions they used to name (issue #94).  `nn open <name>` reads the
    # model out of the asset store, under the lease its CRC was checked with,
    # so no compiled-in address is left here -- and an unreferenced constant is
    # one nobody notices going stale.
    # `nor scan` labels its extents with these, and they are the SAME variables
    # check_flash_partitions.py consumes -- so the labels on the device and the
    # layout the host checks cannot drift apart (issues #45, #85, #86).
    # TWO edges for three labels: everything below the first is firmware,
    # everything below the second is blob, and the rest of the part is the
    # bootloader's slot header.
    NOR_PART_FW_END=${GROVE_FW_RESERVED}
    NOR_PART_BLOB_END=${GROVE_SLOT_HDR_ADDR}
    # [!] THE SAME NUMBER, AND STILL A SEPARATE DEFINITION (issue #94).  blob
    # now ends exactly where the slot header begins, so one variable feeds both
    # -- but "the last address a writer may reach" and "where the bootloader
    # keeps its backup header" are two facts, and port/nor/nor_flash.c reads the
    # second one to pick an XIP probe whose CONTENT it knows.  Spelling that
    # probe as blob's end would make it follow blob: give the top of the part to
    # some future partition and the probe would quietly start reading that
    # instead of the header, with every check still passing.  Issue #88 made
    # this mistake once, with the erase granularity and the header's span.
    NOR_PART_SLOT_HDR=${GROVE_SLOT_HDR_ADDR}
    # The erase unit the seam permits (issue #88).  From the same measured
    # geometry check_flash_partitions.py rounds destruction footprints with, so
    # what the firmware refuses and what the host checks are one number.
    NOR_ERASE_GRAN=${GROVE_ERASE_GRAN})
target_compile_options(shell_objs PRIVATE -Os)

# [!] THE CAMERA/NN/LCD PIXEL LOOPS NO LONGER CARRY -fno-tree-vectorize
# (issue #42).  They carried it because -mcpu=cortex-m55 makes MVE available and
# check_mve_predication.py failed the build on predicated MVE -- a gate whose
# premise the Armv8-M ARM contradicts and which, per issue #66, could not detect
# one instruction it named.  Both are gone, so these loops are compiled the way
# every other one here is.
#
# What replaced the ban is an enforcement rather than a check: FPCCR.ASPEN is
# set, read back and fail-stopped on before kernel entry (port/threadx/
# fp_enforce.c), which is what makes the hardware stack VPR with the rest of the
# caller-saved vector state.  `mve` on the console is the experiment that
# watches a pattern survive a context switch.
#
# [!] Measured at the time: with the pinned GCC 15.2 at -Os, removing the option
# changes nothing these files emit -- no MVE appears.  It is permission, not a
# speed-up, and any claim that it made something faster has to come from
# `camera stats`, not from here.
#
# --- and then the level itself became a free choice (issue #76) --------------
#
# These four are the translation units with a PER-FRAME pixel loop, and they are
# built -O3 while the rest of the firmware stays -Os.  Two things make that a
# cheap trade here and neither is obvious from the outside:
#
#   - the code executes from ITCM, so a bigger function costs nothing but space.
#     There is no instruction cache to spill: TCM is tightly coupled.  The usual
#     "-Os because -O3 thrashes the cache" reasoning does not apply.
#   - space is not scarce.  The four cost +4,882 B together, and
#     check_placement_budget.py keeps that honest -- it PRINTS the headroom on
#     every link, which is the number to read rather than one written down here
#     (it was "~69 KB" for a long time after it had become 34,880 B; see the
#     board README for how to measure it).
#
# [!] AND IT IS NOT ABOUT MVE.  With the ban lifted the auto-vectoriser does fire
# here, but only on straight-line code in the COLD functions -- the frame's two
# biggest CPU stages, cam_bgr_planar_to_rgb565_wb() (pack) and nn_preproc_fill()
# (prep), emit no vector register at any level.  #58 made pack a LUT gather and
# #60 made prep a recurrence, and neither shape auto-vectorises.  What this
# option buys them is ordinary scalar quality: unrolling and scheduling.  The
# numbers that justify it are in issue #76 and the board README, measured on the
# board -- if they ever stop justifying it, take it out.
set(GROVE_O3_SOURCES
    "${BOARD_DIR}/port/camera/cam_convert.c"
    "${GROVE_SHARED_DECODER}"
    "${BOARD_DIR}/port/npu/nn_preproc.c"
    "${BOARD_DIR}/port/lcd/lcd_st7789.c")
set_source_files_properties(${GROVE_O3_SOURCES}
    TARGET_DIRECTORY shell_objs
    PROPERTIES COMPILE_OPTIONS "-O3")
# [!] AND THEN CHECK THAT EACH PATH IS ACTUALLY A SOURCE OF THE TARGET.  EXISTS
# alone is not enough: a file can still be on disk at the old path while no
# longer being compiled into shell_objs, and set_source_files_properties() says
# nothing either way.  This is the only thing standing between "the decoder moved"
# and "the frame rate quietly dropped" (issue #97).
get_target_property(_grove_objs_sources shell_objs SOURCES)
foreach(_o3 IN LISTS GROVE_O3_SOURCES)
    get_filename_component(_o3_abs "${_o3}" ABSOLUTE)
    set(_o3_found FALSE)
    foreach(_src IN LISTS _grove_objs_sources)
        get_filename_component(_src_abs "${_src}" ABSOLUTE)
        if(_src_abs STREQUAL _o3_abs)
            set(_o3_found TRUE)
            break()
        endif()
    endforeach()
    if(NOT _o3_found)
        message(FATAL_ERROR
            "-O3 is set on a file that shell_objs does not build:\n  ${_o3}\n"
            "CMake ignores that silently, so the file would be built at the "
            "default level and nothing would say so.  Fix the path, or drop it "
            "from GROVE_O3_SOURCES.")
    endif()
    # [!] AND THAT THE OPTION IS ACTUALLY IN EFFECT.  Membership alone is not the
    # property: a later set_source_files_properties() that cleared COMPILE_OPTIONS,
    # or appended its own -O, would leave this file a source of the target and
    # built at some other level, with the check above still satisfied.  Per-source
    # options come last on the command line, so the LAST -O here is the one that
    # decides.
    get_source_file_property(_o3_opts "${_o3_abs}"
                             TARGET_DIRECTORY shell_objs COMPILE_OPTIONS)
    set(_o3_last "")
    foreach(_opt IN LISTS _o3_opts)
        if(_opt MATCHES "^-O")
            set(_o3_last "${_opt}")
        endif()
    endforeach()
    if(NOT _o3_last STREQUAL "-O3")
        message(FATAL_ERROR
            "the effective optimisation for\n  ${_o3}\nis '${_o3_last}', not -O3 "
            "(its per-source options are: ${_o3_opts}).\nThese four files carry the "
            "per-frame pixel loops and the board README's frame rates are measured "
            "with them at -O3; a different level here is a silent regression.")
    endif()
endforeach()
# --- the shared decoder must own no storage (issue #97) -----------------------
#
# See cmake/decoder_storage_gate.cmake for what this checks and why it has to be
# THIS board's compile rather than a generic one.
include("${CMAKE_SOURCE_DIR}/cmake/decoder_storage_gate.cmake")
add_decoder_storage_gate(NAME grove_decoder_audit SOURCE "${GROVE_SHARED_DECODER}"
                         IFACE bsp_iface CONSUMER shell_objs)
add_dependencies(shell grove_decoder_audit_check)

# And the negative tests for that checker, run with THIS board's cross compiler
# so the __arm__-only fixture is meaningful (it passes under the host compiler,
# which is the whole point of it).
add_custom_target(grove_decoder_storage_fixtures
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/cmake/fixtures/run_storage_gate_tests.py"
            --cc "${CMAKE_C_COMPILER}" --objdump "${CMAKE_OBJDUMP}"
            --nm "${CMAKE_NM}" --cflags "-mcpu=cortex-m55 -mthumb"
    COMMENT "storage gate negative tests (cross compiler)"
    VERBATIM)
add_dependencies(shell grove_decoder_storage_fixtures)

target_link_options(shell PRIVATE
    "-T${LDSCRIPT_APP}" -Wl,-Map=shell.map,--cref)
set_target_properties(shell PROPERTIES LINK_DEPENDS "${LDSCRIPT_APP}")

# [!] THE MAP IS DELETED BEFORE EVERY LINK (issue #88).  check_nor_seam.py
# decides which input sections survived --gc-sections from this map, and a map
# left over from an earlier link would answer that question about a different
# link -- silently, and in the permissive direction, because the sections the
# rule is about are the ones a stale map would still call discarded.  Removing
# it first means a map can only exist because THIS link wrote it.  (The gate
# also cross-checks the map's addresses against the ELF; this is the half that
# does not depend on the check being right.)
#
# BYPRODUCTS names the map as an output of this edge, which is what makes a
# deleted map get rebuilt.  Without it ninja tracks only shell.elf, so removing
# the map (or a stray fixture run doing it) leaves the gate reading a file
# nothing will regenerate.
add_custom_command(TARGET shell PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_BINARY_DIR}/shell.map"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/shell.map")

# --- Vendor timer seam probe (issue #30) -------------------------------------
# The probe is the same objects as `shell`, plus libsensordp.a / libextdevice.a,
# plus FORCED references to the datapath entry points that reach the vendor
# timer API.  It was introduced in M-G3a, when `shell` did not link the archives
# at all and every wrapper was garbage-collected out of it, so the seam's
# property had nothing real to be asserted against.
#
# Since issue #35 `shell` links the archives and calls into them for real, and
# check_timer_seam.py runs on the firmware image itself (gate 4 above).  The
# probe is KEPT anyway, for one reason: the negative tests in
# cmake/fixtures/run_fixture_tests.py are built by re-linking it with a --wrap
# removed (F1) or the archives dropped (F2), and those variants only isolate the
# GATE's behaviour as long as the link has no other reason to fail.  Re-pointing
# them at `shell` would make F2 fail on undefined references from port/camera/
# instead of on the gate -- a negative test that passes for the wrong reason.
# The forced-reference list keeps the probe's coverage independent of whichever
# entry points port/camera/ happens to call today.
#
# Cheap: the objects are already compiled (shell_objs), so this is one extra
# link.  It is a build target, not an artifact -- nothing flashes it.
set(SEAM_PROBE_FORCED
    sensordplib_set_hxcsc_wdma3
    sensordplib_set_hw5x5_wdma3
    sensordplib_set_raw_wdma2
    sensordplib_retrigger_capture
    sensordplib_start_swreset
    sensordplib_set_sensorctrl_inp_wi_crop
    sensordplib_set_rtc_start
    hx_drv_cis_init
    hx_drv_cis_set_reg
    hx_drv_cis_setRegTable)
# [!] AND THE NOR SEAM'S FOUR WRAPPERS (issue #88), for the same reason and a
# sharper one.  Until issue #88 Part C lands there is NO caller for the write
# path in the firmware, so every wrapper is garbage-collected out of `shell` and
# check_nor_seam.py would be asserting its rules over an empty set -- green, and
# unable to fail, which is the gate shape this repository has already been
# bitten by twice (issues #66, #42).
#
# Forcing the four makes the probe carry the link the gate is about: the two
# permitted wrappers become live, their __real_ references pull the vendor's
# erase_sector and write into the image, and the two refusing wrappers become
# live WITHOUT pulling anything -- which is the property that lets
# check_placement_budget.py go on barring erase_all and word_write by absence.
#
# [!] AND A DROPPED --wrap FLAG BECOMES A LINK ERROR HERE.  With the flag gone
# __real_hx_lib_qspi_eeprom_write resolves to nothing, and the forced reference
# means the wrapper cannot be collected away from the problem.  That is stronger
# than any check over the output, which is why the fixture for it asserts a
# failed link rather than a gate diagnostic.
list(APPEND SEAM_PROBE_FORCED
    __wrap_hx_lib_qspi_eeprom_erase_sector
    __wrap_hx_lib_qspi_eeprom_write
    __wrap_hx_lib_qspi_eeprom_erase_all
    __wrap_hx_lib_qspi_eeprom_word_write)
set(SEAM_PROBE_FORCE_FLAGS "")
foreach(_sym IN LISTS SEAM_PROBE_FORCED)
    list(APPEND SEAM_PROBE_FORCE_FLAGS "-Wl,-u,${_sym}")
endforeach()

add_executable(seam_probe $<TARGET_OBJECTS:shell_objs>)
target_link_libraries(seam_probe PRIVATE bsp_iface coremark_obj tflm_obj
    -Wl,--start-group "${LIBDRIVER}" "${LIBPWRMGMT}"
                      "${LIBSENSORDP}" "${LIBEXTDEVICE}" "${LIBSPIEEPROM}" -Wl,--end-group)
target_link_options(seam_probe PRIVATE
    -u _printf_float ${SDK_TIMER_WRAP_FLAGS} ${SDK_NOR_WRAP_FLAGS}
    ${SEAM_PROBE_FORCE_FLAGS}
    "-T${LDSCRIPT_APP}" -Wl,-Map=seam_probe.map,--cref)
set_target_properties(seam_probe PROPERTIES LINK_DEPENDS "${LDSCRIPT_APP}")
add_custom_command(TARGET seam_probe PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_BINARY_DIR}/seam_probe.map"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/seam_probe.map")

# --- The NOR seam's linker inputs (issue #88) --------------------------------
# check_nor_seam.py audits relocations in the linker's INPUTS and classifies
# each one live or discarded from the map.  For that to mean anything it has to
# know it saw every input: an object it never opened is an object whose calls it
# never looked at.  So board.cmake writes down what it hands the linker, the
# gate compares that against the map's own LOAD list, and anything in neither
# the manifest nor the toolchain is a refusal.
#
# Generated rather than restated because $<TARGET_OBJECTS:> is the same list the
# link line is built from -- a hand-written copy would drift, and a manifest
# that has drifted is one that stops covering whatever was added.
#
# `shell` and `seam_probe` link the SAME inputs (the probe differs only in
# forced references and where its output goes), so one manifest serves both.
set(NOR_SEAM_MANIFEST "${GEN_DIR}/nor_seam_inputs.txt")
file(GENERATE OUTPUT "${NOR_SEAM_MANIFEST}" CONTENT
"$<JOIN:$<TARGET_OBJECTS:shell_objs>,\n>
$<JOIN:$<TARGET_OBJECTS:coremark_obj>,\n>
$<JOIN:$<TARGET_OBJECTS:tflm_obj>,\n>
${LIBDRIVER}
${LIBPWRMGMT}
${LIBSENSORDP}
${LIBEXTDEVICE}
${LIBSPIEEPROM}
")

# Where the compiler's own inputs (crt*.o, libc_nano.a, libgcc.a) come from.
# The gate needs the boundary so it can tell "an input nobody declared" from
# "an input the compiler driver adds"; it still checks that the latter do not
# reference the write path.
get_filename_component(_grove_gcc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(GROVE_TOOLCHAIN_ROOT "${_grove_gcc_bin}" DIRECTORY)

# The objects allowed to call the wrapped names.  ONE (issue #88 Part C): the
# transaction in port/nor/nor_write.c, which bounds every call before it makes
# it and reads the array back afterwards.
#
# [!] THE ENTRY IS AN OBJECT, NOT A DIRECTORY OR A SYMBOL.  port/nor/ also holds
# the lifecycle, and nor_flash.c is where a future "just erase it here" would be
# most tempting to write; naming the file rather than the directory is what
# makes that a gate failure instead of a diff nobody has to justify.
set(NOR_SEAM_CALLERS "port/nor/nor_write.c.obj")
set(NOR_SEAM_CALLER_FLAGS "")
foreach(_obj IN LISTS NOR_SEAM_CALLERS)
    list(APPEND NOR_SEAM_CALLER_FLAGS "--allow-caller" "${_obj}")
endforeach()

set(NOR_SEAM_GATE_ARGS
    --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
    --manifest "${NOR_SEAM_MANIFEST}"
    --toolchain-root "${GROVE_TOOLCHAIN_ROOT}"
    --link-dir "${CMAKE_BINARY_DIR}"
    --seam-object "port/sdk_seam/nor_seam.c.obj"
    ${NOR_SEAM_CALLER_FLAGS}
    --writable-lo "${GROVE_BLOB_ADDR}"
    --writable-hi "${GROVE_SLOT_HDR_ADDR}"
    --erase-unit "${GROVE_ERASE_GRAN}")

# The same gate as on `shell`, on the probe link.  Both run: the probe keeps the
# forced-reference coverage, `shell` is the image that actually ships.
add_custom_command(TARGET seam_probe POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_timer_seam.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            --require-archives
            "$<TARGET_FILE:seam_probe>"
    COMMENT "check_timer_seam.py (no vendor timer code survives the --wrap)")
# And the NOR seam's gate, WITH --require-live-wrappers.  This is the link where
# the write path exists, so it is the link where the rules are about something.
add_custom_command(TARGET seam_probe POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_nor_seam.py"
            ${NOR_SEAM_GATE_ARGS}
            --map "${CMAKE_BINARY_DIR}/seam_probe.map"
            --require-live-wrappers
            "$<TARGET_FILE:seam_probe>"
    COMMENT "check_nor_seam.py (only the seam reaches the NOR write path)")

# Make the probe part of the default build: a seam that is only checked when
# somebody remembers to ask is not a gate.
add_dependencies(shell seam_probe)

# --- Image generation --------------------------------------------------------
# The Himax image generator turns the ELF into the flashable .img (bootloader +
# 2nd bootloader + memory descriptors + signed app; signed with the dev keys
# the tool ships).  The whole tool tree is copied into the build dir once at
# configure time because the tool writes into its own directory.
set(IMAGE_GEN_DIR "${CMAKE_BINARY_DIR}/image_gen")
if(NOT EXISTS "${IMAGE_GEN_DIR}/we2_local_image_gen")
    file(COPY "${GROVE_SDK_ROOT}/we2_image_gen_local/" DESTINATION "${IMAGE_GEN_DIR}")
endif()

add_custom_command(TARGET shell POST_BUILD
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:shell>"
    # Remove every prior output FIRST: the vendor tool is a black box, and a
    # run that failed partway while exiting 0 must not leave a stale
    # output.img + JSON pair for the copy below and the coherence gate to
    # accept.  After this rm, an output.img can only exist because THIS run
    # produced it (the copy fails the build otherwise).
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${IMAGE_GEN_DIR}/output_case1_sec_wlcsp"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${IMAGE_GEN_DIR}/input_case1_secboot/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf"
    # The tool's input path/name is fixed by its project json.
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:shell>"
            "${IMAGE_GEN_DIR}/input_case1_secboot/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf"
    COMMAND ./we2_local_image_gen project_case1_blp_wlcsp.json
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${IMAGE_GEN_DIR}/output_case1_sec_wlcsp/output.img"
            "${CMAKE_BINARY_DIR}/shell.img"
    WORKING_DIRECTORY "${IMAGE_GEN_DIR}"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/shell.img"
    COMMENT "we2_local_image_gen -> shell.img")

# --- Post-build gates --------------------------------------------------------
# 1. Image coherence: everything the linker placed must actually be inside the
#    generated image (the tool processes sections individually and silently
#    drops names it does not know), and the shell command registry must sit
#    inside the .rodata coverage.  Runs AFTER the image generation above --
#    POST_BUILD commands execute in declaration order.
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_image_coherence.py"
            --objdump "${CMAKE_OBJDUMP}" --nm "${CMAKE_NM}"
            --objcopy "${CMAKE_OBJCOPY}"
            --preprocess-json "${IMAGE_GEN_DIR}/output_case1_sec_wlcsp/DEBUG_APP_PREPROCESS.json"
            --image-gen-dir "${IMAGE_GEN_DIR}"
            --img "${CMAKE_BINARY_DIR}/shell.img"
            "$<TARGET_FILE:shell>"
    COMMENT "check_image_coherence.py (ELF sections vs generated .img)")
# 2. Placement / budget: ITCM/DTCM usage + headroom, vector table residency,
#    static stacks in DTCM, and no references to the SDK's SysTick-touching or
#    console APIs (they must have been dead-stripped / never called).
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_placement_budget.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:shell>"
    COMMENT "check_placement_budget.py (ITCM/DTCM budget + forbidden refs)")
# 3. (was the MVE predication scan; deleted by issue #42.)  It barred predicated
#    MVE because the ThreadX port was believed not to save VPR.  The Armv8-M ARM
#    says the HARDWARE saves it -- with FPCCR.ASPEN set, which is now enforced
#    and read back before kernel entry (port/threadx/fp_enforce.c) -- and the
#    scan could not have caught anything anyway: the pinned objdump does not
#    decode MVE (issue #66).  check_placement_budget.py requires the enforcement
#    symbol in its place, which works because --gc-sections drops an uncalled
#    function; cmake/fixtures/ proves that requirement bites.
# 4. Timer seam: no vendor timer code survives the --wrap.  Since issue #35 the
#    camera archives are in THIS link, so the firmware image is now the real
#    subject of this check -- seam_probe (below) keeps running it too, because
#    the negative tests in cmake/fixtures/ are built by re-linking the probe.
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_timer_seam.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            --require-archives
            "$<TARGET_FILE:shell>"
    COMMENT "check_timer_seam.py (no vendor timer code survives the --wrap)")
# 5. NOR write-path seam (issue #88): every reference to the vendor's erase and
#    program entry points comes from port/sdk_seam/nor_seam.c, and the interval
#    the seam enforces is the one this file declared.  NOT --require-live-
#    wrappers here: until Part C's writer lands, the firmware has no caller and
#    the whole seam is garbage-collected out of it -- which is a correct image
#    and a vacuous check, so seam_probe carries the forced references and runs
#    the same gate over a link where the write path is real.
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_nor_seam.py"
            ${NOR_SEAM_GATE_ARGS}
            --map "${CMAKE_BINARY_DIR}/shell.map"
            "$<TARGET_FILE:shell>"
    COMMENT "check_nor_seam.py (only the seam reaches the NOR write path)")

# --- Flash target ------------------------------------------------------------
# xmodem upload to the Himax bootloader: run the target, press the board's
# reset button when the script asks, and it drives the menu + transfer + reboot.
# The same serial device is the console -- close the terminal first.
# [!] Manual target only: every flash rewrites the whole image including the
# bootloader region of the external NOR (W25Q128JW, ~100k cycle endurance).
# Never wire this into an automatic loop.
set(GROVE_SERIAL_PORT "/dev/ttyACM0" CACHE STRING
    "Serial device of the board's CH343P bridge (console + flash channel)")
set(GROVE_SERIAL_BAUDRATE "921600" CACHE STRING
    "Baudrate for the xmodem flash upload")

# The host tools need pyserial + xmodem (flashing) and vela (model
# preparation, issue #45); a build-local venv keeps all of that out of the host
# Python.  Created at configure time.
#
# [!] The INSTALL is keyed on the CONTENT of requirements.txt, not on the venv
# existing.  Gating it on the directory would mean that adding a dependency --
# which is what happened when vela arrived -- silently does nothing in every
# build tree that already configured once, and the failure surfaces much later
# as a missing tool.  A venv left half-installed by an interrupted run heals the
# same way.  (Same reasoning as the Wio board's tflite-micro.cmake states for
# its own venv.)
set(GROVE_VENV "${CMAKE_BINARY_DIR}/venv")
if(NOT EXISTS "${GROVE_VENV}/bin/python")
    message(STATUS "Creating host-tool venv ...")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m venv "${GROVE_VENV}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "python3 -m venv failed (${GROVE_VENV})")
    endif()
endif()
file(SHA256 "${BOARD_DIR}/requirements.txt" _req_hash)
set(GROVE_VENV_STAMP "${GROVE_VENV}/.requirements-${_req_hash}")
if(NOT EXISTS "${GROVE_VENV_STAMP}")
    message(STATUS "Installing host-tool venv requirements "
                   "(pyserial, xmodem, ethos-u-vela) ...")
    execute_process(
        COMMAND "${GROVE_VENV}/bin/python" -m pip install --quiet
                -r "${BOARD_DIR}/requirements.txt"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "pip install -r boards/grove-vision-ai-v2/requirements.txt failed")
    endif()
    # Written only after pip succeeded, so a failed install is retried rather
    # than remembered as done.
    file(WRITE "${GROVE_VENV_STAMP}" "${_req_hash}\n")
endif()

# --- Host-side model tools (issue #45) ---------------------------------------
# Two programs that run on the DEVELOPER'S machine, not the board:
#
#   tflite_strip_boundary   removes a model's leading QUANTIZE and trailing
#                           DEQUANTIZEs, so vela has nothing left to leave on
#                           the CPU.  Run it BEFORE vela.
#   verify_vela_model       inspects vela's output and answers, on the host, the
#                           questions the board would otherwise answer by
#                           failing over a serial line after a flash cycle.
#
# They are custom commands driving the host compiler rather than ordinary
# targets because this project cross-compiles: every CMake target here is built
# for cortex-m55, and a program that runs on the developer's machine cannot be
# one of them.  Neither is in ALL -- compiling schema_generated.h plus the
# interpreter costs a few seconds and most builds never need it.
#
# [!] verify_vela_model links THE FIRMWARE'S OWN port/npu/npu_payload.c and
# port/npu/npu_arena.c.  That is the whole design: a host checker that
# reimplemented the payload walk could agree with itself and disagree with the
# board, and one that hardcoded the arena size would drift from the reservation.
# The two .c files need -x c EACH -- one -x c in front of a list applies only to
# the first file (observed on gcc 13.3), and compiling _Static_assert as C++ is
# how that mistake announces itself.
find_program(HOST_CXX NAMES c++ g++ clang++)
if(HOST_CXX)
    set(_ethosu_drv_inc "${ETHOSU_DRV}/include")
    # The same source set tflm_obj builds, minus the three driver .c files
    # (ARM-only) -- Eval()'s driver calls are stubbed in verify_vela_model.cc
    # and abort if reached.  --gc-sections for the same reason the firmware link
    # needs it: kernel_util.cc references an int4 unpacker that nothing in this
    # configuration calls.
    set(_tflm_host_srcs
        "${TFLM}/tensorflow/compiler/mlir/lite/schema/schema_utils.cc"
        "${TFLM}/tensorflow/lite/micro/kernels/ethos_u/ethosu.cc"
        "${TFLM}/tensorflow/lite/micro/kernels/kernel_util.cc"
        "${TFLM}/tensorflow/lite/micro/micro_interpreter.cc"
        "${TFLM}/tensorflow/lite/micro/micro_interpreter_graph.cc"
        "${TFLM}/tensorflow/lite/micro/micro_interpreter_context.cc"
        "${TFLM}/tensorflow/lite/micro/micro_allocator.cc"
        "${TFLM}/tensorflow/lite/micro/micro_allocation_info.cc"
        "${TFLM}/tensorflow/lite/micro/micro_context.cc"
        "${TFLM}/tensorflow/lite/micro/micro_op_resolver.cc"
        "${TFLM}/tensorflow/lite/micro/micro_resource_variable.cc"
        "${TFLM}/tensorflow/lite/micro/micro_profiler.cc"
        "${TFLM}/tensorflow/lite/micro/micro_log.cc"
        "${TFLM}/tensorflow/lite/micro/micro_utils.cc"
        "${TFLM}/tensorflow/lite/micro/micro_time.cc"
        "${TFLM}/tensorflow/lite/micro/memory_helpers.cc"
        "${TFLM}/tensorflow/lite/micro/debug_log.cc"
        "${TFLM}/tensorflow/lite/micro/flatbuffer_utils.cc"
        "${TFLM}/tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc"
        "${TFLM}/tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc"
        "${TFLM}/tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc"
        "${TFLM}/tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc"
        "${TFLM}/tensorflow/lite/micro/memory_planner/linear_memory_planner.cc"
        "${TFLM}/tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.cc"
        "${TFLM}/tensorflow/lite/micro/tflite_bridge/micro_error_reporter.cc"
        "${TFLM}/tensorflow/lite/core/api/flatbuffer_conversions.cc"
        "${TFLM}/tensorflow/lite/core/api/tensor_utils.cc"
        "${TFLM}/tensorflow/lite/core/c/common.cc"
        "${TFLM}/tensorflow/lite/kernels/kernel_util.cc"
        "${TFLM}/tensorflow/lite/kernels/internal/common.cc"
        "${TFLM}/tensorflow/lite/kernels/internal/quantization_util.cc"
        "${TFLM}/tensorflow/lite/kernels/internal/tensor_ctypes.cc"
        "${TFLM}/tensorflow/lite/kernels/internal/runtime_shape.cc"
        "${TFLM}/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc")

    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/tflite_strip_boundary"
        COMMAND "${HOST_CXX}" -std=c++17 -O1 -w
                -I "${TFLM}"
                -I "${TFLM}/third_party/flatbuffers/include"
                # gemmlowp/ruy are reachable from micro_interpreter.h, included
                # for TFLITE_SCHEMA_VERSION alone.
                -I "${TFLM}/third_party/gemmlowp"
                -I "${TFLM}/third_party/ruy"
                "${BOARD_DIR}/scripts/tflite_strip_boundary.cc"
                "${TFLM}/tensorflow/compiler/mlir/lite/schema/schema_utils.cc"
                -o "${CMAKE_BINARY_DIR}/tflite_strip_boundary"
        DEPENDS "${BOARD_DIR}/scripts/tflite_strip_boundary.cc"
        COMMENT "host c++ -> tflite_strip_boundary (strips a model's boundary conversions)"
        VERBATIM)

    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/verify_vela_model"
        COMMAND "${HOST_CXX}" -std=c++17 -O1 -w
                -fno-exceptions -fno-rtti
                -ffunction-sections -fdata-sections
                -I "${TFLM}"
                -I "${TFLM}/third_party/flatbuffers/include"
                -I "${TFLM}/third_party/gemmlowp"
                -I "${TFLM}/third_party/ruy"
                -I "${_ethosu_drv_inc}"
                -I "${BOARD_DIR}/port/npu"
                -DTF_LITE_STATIC_MEMORY -DTF_LITE_MCU_DEBUG_LOG
                -DETHOS_U -DETHOSU55 -DETHOSU_ARCH=u55
                "${BOARD_DIR}/scripts/verify_vela_model.cc"
                -x c "${BOARD_DIR}/port/npu/npu_payload.c"
                -x c "${BOARD_DIR}/port/npu/npu_arena.c"
                -x none
                "${BOARD_DIR}/port/npu/npu_model_scan.cc"
                ${_tflm_host_srcs}
                -Wl,--gc-sections
                -o "${CMAKE_BINARY_DIR}/verify_vela_model"
        DEPENDS "${BOARD_DIR}/scripts/verify_vela_model.cc"
                "${BOARD_DIR}/port/npu/npu_model_scan.cc"
                "${BOARD_DIR}/port/npu/npu_model_scan.h"
                "${BOARD_DIR}/port/npu/npu_payload.c"
                "${BOARD_DIR}/port/npu/npu_payload.h"
                "${BOARD_DIR}/port/npu/npu_arena.c"
                "${BOARD_DIR}/port/npu/npu.h"
                # The bounded flatbuffer check the firmware runs (issue #93).
                # Listed so that changing a limit rebuilds the host gate: the
                # whole point of sharing it is that the two cannot disagree.
                "${BOARD_DIR}/port/npu/npu_verify.h"
        COMMENT "host c++ -> verify_vela_model (checks a model before it is flashed)"
        VERBATIM)

    add_custom_target(model-tools
        DEPENDS "${CMAKE_BINARY_DIR}/tflite_strip_boundary"
                "${CMAKE_BINARY_DIR}/verify_vela_model")
    set(GROVE_SEND_VERIFIER "${CMAKE_BINARY_DIR}/verify_vela_model")
else()
    message(STATUS "grove: no host C++ compiler -- the `model-tools` target is "
                   "unavailable (the firmware itself still builds)")
    # Left EMPTY on purpose, and the script refuses on an empty one.  See below.
    set(GROVE_SEND_VERIFIER "")
endif()

# --- The verified send path (issue #93) ---------------------------------------
#
# `blob write` receives whatever arrives on the wire.  The models used to be
# flashed by `--target flash-model-cls|det`, and the host gate ran INSIDE that
# target, so there was no way to write one without it; over the console there is
# no such chain.  This puts the chain back, on the PC side, as the thing picocom
# is pointed at:
#
#     picocom -b 921600 /dev/ttyACM0 \
#         --send-cmd "${CMAKE_BINARY_DIR}/send_verified_model.sh --profile det"
#
# [!] AND IT IS GENERATED EVEN WITH NO HOST C++, with an empty verifier path,
# because the script refuses on that with a sentence saying why.  Generating
# nothing would make the failure "no such file" from inside picocom's send hook,
# which is a worse thing to read than a refusal -- and the refusal is the fail
# closed the gate is for.  The board-side bounds checks in npu_open() do not
# replace this one: they say a flatbuffer holds together, not that it is a model
# this firmware can run, and they run after the erase and the transfer.
find_program(GROVE_SB_TOOL NAMES sb DOC "lrzsz YMODEM sender, for the model send path")
if(GROVE_SB_TOOL)
    set(GROVE_SEND_SB "${GROVE_SB_TOOL}")
else()
    # Left as the bare name: `sb` may be installed after this build was
    # configured, and PATH at send time is the honest place to look it up.
    set(GROVE_SEND_SB "sb")
endif()
configure_file("${BOARD_DIR}/scripts/send_verified_model.sh.in"
               "${CMAKE_BINARY_DIR}/send_verified_model.sh"
               @ONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                GROUP_READ GROUP_EXECUTE
                                WORLD_READ WORLD_EXECUTE)

# --- The layout check, and the one flashing target left (issues #44, #45, #94)-
#
# [!] THE LAYOUT IS RESERVATIONS, AND IT IS SEPARATE FROM WHAT IS BUILT.
# The first version of this listed the FILES and required all of them, which
# made plain `--target flash` refuse on any tree where the detection model --
# which cannot be committed, being model-zoo licensed -- had not been built by
# hand.  A gate that blocks the operation it is not protecting is a gate
# somebody removes, and then nothing is protected.  So the partitions declare
# ADDRESSES and SIZES, checked against each other with no files present at all,
# and the flashing target names only the artifact it actually writes.
#
# The reservations are erase-block spans, so they are what a write can destroy
# rather than what a file occupies.
#
# [!] THERE USED TO BE FOUR MORE PARTITIONS AND TWO MORE TARGETS (issue #94).
# `model-cls`, `model-det`, `blob-tail` and the `flash-model-*` targets that
# wrote the first two are gone: since issue #93 a model is an asset in the
# store, put there over the console by `blob write` and read by `nn open
# <name>`.  What checked the model before it was transmitted has NOT gone with
# them -- it moved to send_verified_model.sh above, which is the same chain
# (stage, verify, send exactly the file that was verified) on the path the
# console uses.
#
# [!] --image-max, not just the reservation.  The firmware reservation covers
# BOTH slots, but a single image has to fit in ONE -- the bootloader refuses a
# larger one with ERR_IMAGE_SZ, after the serial port is open and a reset has
# been pressed.  Without this, an image between 1 and 2 MB passes the layout
# check and fails on the hardware.
set(GROVE_FLASH_LAYOUT
    "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_flash_partitions.py"
    --flash-size "${GROVE_FLASH_SIZE}"
    --erase-granularity "${GROVE_ERASE_GRAN}"
    --partition "firmware:0x0:${GROVE_FW_RESERVED}"
    --image-max "firmware:${GROVE_FW_SLOT_SIZE}"
    --partition "blob:${GROVE_BLOB_ADDR}:${GROVE_BLOB_RESERVED}"
    --partition "slot-header:${GROVE_SLOT_HDR_ADDR}:${GROVE_SLOT_HDR_RESERVED}")

# `flash` names only the firmware, and now that is the only artifact this build
# writes to the NOR at a fixed address at all.
add_custom_target(flash
    COMMAND ${GROVE_FLASH_LAYOUT}
            --image "firmware:${CMAKE_BINARY_DIR}/shell.img"
            --writing firmware
    COMMAND "${GROVE_VENV}/bin/python" "${GROVE_SDK_ROOT}/xmodem/xmodem_send.py"
            --port=${GROVE_SERIAL_PORT}
            --baudrate=${GROVE_SERIAL_BAUDRATE}
            --protocol=xmodem
            --file=${CMAKE_BINARY_DIR}/shell.img
    DEPENDS shell
    USES_TERMINAL
    COMMENT "xmodem -> shell.img over ${GROVE_SERIAL_PORT} (press reset when asked)")
