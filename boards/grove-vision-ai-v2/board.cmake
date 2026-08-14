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
set(LIBSENSORDP  "${SDK}/prebuilt_libs/gnu/libsensordp.a")
set(LIBEXTDEVICE "${SDK}/prebuilt_libs/gnu/libextdevice.a")

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
    "${BOARD_DIR}/cmds/cmd_camera.c"
    "${CMAKE_SOURCE_DIR}/svc/fmt.c"
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
# [!] -fno-tree-vectorize is load-bearing, not tuning: -mcpu=cortex-m55 enables
# MVE, and at -O3 the auto-vectoriser emits PREDICATED MVE (VCTP/VPST) for
# exactly the kind of loops CoreMark is made of.  The ThreadX Cortex-M55 port
# does not save/restore VPR across a context switch, so such code is unsafe in
# a preemptible thread -- check_mve_predication.py fails the build on it.  The
# published score is therefore a SCALAR score; core_portme.h says so in the
# report's own flags line.
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

# --- The shell firmware ------------------------------------------------------
# The sources compile into an OBJECT library rather than straight into the
# executable so the seam probe below can link the SAME objects a second time
# without recompiling them (issue #30).  Nothing else changes: `shell` still
# links exactly this object set in exactly this order.
add_library(shell_objs OBJECT
    "${BOARD_DIR}/src/main.c"
    "${BOARD_DIR}/src/fault.c"
    "${BOARD_DIR}/src/retarget.c"
    "${BOARD_DIR}/src/malloc_lock.c"
    "${BOARD_DIR}/src/xprintf_shim.c"
    "${BOARD_DIR}/port/threadx/tx_glue.c"
    "${BOARD_DIR}/port/sdk_seam/timer_seam.c"
    "${BOARD_DIR}/port/sdk_seam/epk_irq_wrap.c"
    "${BOARD_DIR}/port/lcd/lcd_st7789.c"
    "${BOARD_DIR}/port/camera/cam_convert.c"
    "${BOARD_DIR}/port/camera/cam_mipi_calc.c"
    "${BOARD_DIR}/port/camera/cam_auto.c"
    "${BOARD_DIR}/port/camera/cam_imx219.c"
    "${BOARD_DIR}/port/camera/camera.c"
    "${BOARD_DIR}/port/camera/cam_lcd_sink.c"
    ${SHELL_SOURCES}
    ${SDK_SOURCES}
    ${TX_CORE} ${TX_ASM} ${TX_EPK})

add_executable(shell $<TARGET_OBJECTS:shell_objs>)
target_link_libraries(shell PRIVATE bsp_iface coremark_obj
    -Wl,--start-group "${LIBDRIVER}" "${LIBPWRMGMT}"
                      "${LIBSENSORDP}" "${LIBEXTDEVICE}" -Wl,--end-group)
# CoreMark's canonical report prints its score with %f; pull in newlib's float
# printf (newlib-nano omits it by default).  This is also why src/malloc_lock.c
# exists: that conversion allocates from the heap, now from several threads.
target_link_options(shell PRIVATE -u _printf_float ${SDK_TIMER_WRAP_FLAGS})
target_include_directories(shell_objs PRIVATE
    "${BOARD_DIR}/src"
    "${BOARD_DIR}/port/threadx"
    "${BOARD_DIR}/port/sdk_seam"
    "${BOARD_DIR}/port/lcd"
    "${BOARD_DIR}/port/camera"
    # Header surface of the two camera archives (issue #35): the CIS (sensor
    # I2C) layer and the sensor datapath library.  Not in bsp_iface because
    # only port/camera/ has any business calling them.
    "${SDK}/external/cis"
    "${SDK}/library/sensordp/inc"
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
target_link_libraries(shell_objs PRIVATE bsp_iface)
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
    CLI_CPU_CYCLES_PER_US=400)
target_compile_options(shell_objs PRIVATE -Os)

# [!] -fno-tree-vectorize on the camera's pixel loops, for the same reason
# coremark_obj carries it (see the note above that option): -mcpu=cortex-m55
# makes MVE available, the ThreadX M55 port does not preserve VPR across a
# context switch, and check_mve_predication.py fails the build on any
# predicated MVE in the image.  -Os does not auto-vectorise today, which is
# exactly why this is stated rather than relied upon -- a later -O2 or a newer
# GCC would turn a 76,800-iteration loop over three planes into precisely the
# code the gate bars, and the failure would surface as a build break in an
# unrelated commit.
set_source_files_properties(
    "${BOARD_DIR}/port/camera/cam_convert.c"
    TARGET_DIRECTORY shell_objs
    PROPERTIES COMPILE_OPTIONS "-fno-tree-vectorize")
target_link_options(shell PRIVATE
    "-T${LDSCRIPT_APP}" -Wl,-Map=shell.map,--cref)
set_target_properties(shell PROPERTIES LINK_DEPENDS "${LDSCRIPT_APP}")

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
set(SEAM_PROBE_FORCE_FLAGS "")
foreach(_sym IN LISTS SEAM_PROBE_FORCED)
    list(APPEND SEAM_PROBE_FORCE_FLAGS "-Wl,-u,${_sym}")
endforeach()

add_executable(seam_probe $<TARGET_OBJECTS:shell_objs>)
target_link_libraries(seam_probe PRIVATE bsp_iface coremark_obj
    -Wl,--start-group "${LIBDRIVER}" "${LIBPWRMGMT}"
                      "${LIBSENSORDP}" "${LIBEXTDEVICE}" -Wl,--end-group)
target_link_options(seam_probe PRIVATE
    -u _printf_float ${SDK_TIMER_WRAP_FLAGS} ${SEAM_PROBE_FORCE_FLAGS}
    "-T${LDSCRIPT_APP}" -Wl,-Map=seam_probe.map,--cref)
set_target_properties(seam_probe PROPERTIES LINK_DEPENDS "${LDSCRIPT_APP}")

# The same gate as on `shell`, on the probe link.  Both run: the probe keeps the
# forced-reference coverage, `shell` is the image that actually ships.
add_custom_command(TARGET seam_probe POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_timer_seam.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            --require-archives
            "$<TARGET_FILE:seam_probe>"
    COMMENT "check_timer_seam.py (no vendor timer code survives the --wrap)")

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
# 3. MVE predication scan: the ThreadX Cortex-M55 port does not save/restore
#    VPR across context switches, so predicated MVE (VCTP/VPST blocks) must not
#    appear in the linked image.  (Plain unpredicated MVE loads in the prebuilt
#    driver are fine -- q4-q7 alias s16-s31, which the port does save.)
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${BOARD_DIR}/cmake/check_mve_predication.py"
            --objdump "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:shell>"
    COMMENT "check_mve_predication.py (no VPR-dependent MVE in the image)")
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

# The flash script needs pyserial + xmodem; a build-local venv keeps that out
# of the host Python.  Created once at configure time.
set(GROVE_VENV "${CMAKE_BINARY_DIR}/venv")
if(NOT EXISTS "${GROVE_VENV}/bin/python")
    message(STATUS "Creating flash-tool venv (pyserial, xmodem) ...")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m venv "${GROVE_VENV}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "python3 -m venv failed (${GROVE_VENV})")
    endif()
    execute_process(
        COMMAND "${GROVE_VENV}/bin/python" -m pip install --quiet
                -r "${BOARD_DIR}/requirements.txt"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "pip install -r boards/grove-vision-ai-v2/requirements.txt failed")
    endif()
endif()

add_custom_target(flash
    COMMAND "${GROVE_VENV}/bin/python" "${GROVE_SDK_ROOT}/xmodem/xmodem_send.py"
            --port=${GROVE_SERIAL_PORT}
            --baudrate=${GROVE_SERIAL_BAUDRATE}
            --protocol=xmodem
            --file=${CMAKE_BINARY_DIR}/shell.img
    DEPENDS shell
    USES_TERMINAL
    COMMENT "xmodem -> shell.img over ${GROVE_SERIAL_PORT} (press reset when asked)")
