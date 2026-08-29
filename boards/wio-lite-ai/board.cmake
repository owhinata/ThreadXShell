# ============================================================================
#  Wio Lite AI (STM32H725AEI6) board definition.
#
#  Included by the top-level CMakeLists.txt with BOARD=wio-lite-ai.  It owns
#  everything board-specific: the MCU flags, the HAL/CMSIS pins, the linker
#  script, the peripheral options, the two firmwares and the post-build gates.
#
#  Firmwares (both run from the internal flash app partition 0x08020000 and are
#  loaded over DFU -- see the "DFU only" rule in the project CLAUDE.md):
#    shell -- ThreadX + the interactive CLI console over USB CDC.
#    blink -- LED blink reference app.
#      `dfu-util -d 0483:df11 -a 0 -D <fw>.bin` while the bootloader is in DFU
#      mode (hold USER/PF1 at reset); convenience targets `dfu-shell`/`dfu-blink`.
#
#  [!] The DFU bootloader at internal flash 0x08000000 IS built here (`boot`),
#  but only as a REFERENCE BUILD -- into boot-reference/, and with no target that
#  can write it anywhere.  It is a separate, independent tree
#  (boards/wio-lite-ai/boot/) that shares no source and no header with the app;
#  flashing sector 0 is the one operation that can brick the board, and the only
#  surviving board is #2.  The point of building it is CONTINUITY: a HAL, TinyUSB,
#  toolchain or board.cmake change must not silently break the bootloader tree.
#  boot-reference/boot.bin is checked against a golden hash and put through
#  cmake/check_boot_safety.py -- see the bootloader section further down.
#
#  [!] This app does NOT configure the clock tree.  It inherits the 550 MHz /
#  PLL3Q 48 MHz USB / FLASH latency 3 the bootloader set up; src/system_stm32h7xx.c
#  is a custom SystemInit that does FPU + VTOR + TCM initialisation only.
# ============================================================================

# The name the `system` command prints; substituted into cmake/cli_version.h.in
# by the top-level CMakeLists.txt AFTER this file has been included.
set(BOARD_FW_NAME "Wio Lite AI ThreadX Shell")

# --- Target / common build options -----------------------------------------
# Cortex-M7 with the double-precision FPU (fpv5-d16) on the STM32H725.
set(MCU_OPTS
    -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)

set(HAL_DIR    "${CMAKE_SOURCE_DIR}/lib/stm32h7xx_hal_driver")
set(CMSIS_DEV  "${CMAKE_SOURCE_DIR}/lib/cmsis_device_h7")
set(CMSIS_CORE "${CMAKE_SOURCE_DIR}/lib/cmsis_core")
set(LDSCRIPT_APP "${BOARD_DIR}/ldscript/STM32H725AEIx_IROM.ld") # app (internal 0x08020000)
set(LDSCRIPT_ROM "${BOARD_DIR}/ldscript/STM32H725AEIx_ROM.ld")  # bootloader (0x08000000)

# stm32h7xx_hal_conf.h from the upstream template (its default HSE_VALUE of
# 25 MHz already matches the board crystal).
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
configure_file("${HAL_DIR}/Inc/stm32h7xx_hal_conf_template.h"
               "${GEN_DIR}/stm32h7xx_hal_conf.h" COPYONLY)

# Usage requirements shared by the target (includes, defs, MCU flags, link).
add_library(bsp_iface INTERFACE)
target_include_directories(bsp_iface INTERFACE
    "${BOARD_DIR}/include"
    "${GEN_DIR}"
    "${HAL_DIR}/Inc"
    "${CMSIS_DEV}/Include"
    "${CMSIS_CORE}/Include")
target_compile_definitions(bsp_iface INTERFACE USE_HAL_DRIVER STM32H725xx)
target_compile_options(bsp_iface INTERFACE
    ${MCU_OPTS} -Wall -fdata-sections -ffunction-sections -g -gdwarf-2)
target_link_options(bsp_iface INTERFACE
    ${MCU_OPTS} -specs=nano.specs -specs=nosys.specs
    -Wl,--gc-sections -Wl,--print-memory-usage)   # ldscript (-T) is added per firmware

# Compile every HAL driver source; each file self-guards on its
# HAL_<module>_MODULE_ENABLED define, so unused ones become empty objects.
file(GLOB HAL_SOURCES "${HAL_DIR}/Src/*.c")
list(FILTER HAL_SOURCES EXCLUDE REGEX "_template\\.c$")

# TinyUSB's source root.  The bootloader uses it too; the app's own subset of it
# is APP_TINYUSB_SOURCES further down.
set(TUSB_DIR "${CMAKE_SOURCE_DIR}/lib/tinyusb/src")

# ============================================================================
#  Bootloader: the DFU bootloader that owns internal flash sector 0
#  (0x08000000, 128 KB).  REFERENCE BUILD ONLY -- nothing here can flash it.
# ============================================================================
# The bootloader tree (boot/) is INDEPENDENT of the app: it shares no source and
# no header with it, it configures the clock tree the app then inherits, and it
# is what survives a bad app image.  It is built here for exactly one reason --
# so that a HAL, TinyUSB, toolchain or board.cmake change cannot break it
# unnoticed.  Its bytes are frozen against a golden hash and the linked image is
# put through cmake/check_boot_safety.py.
#
# [!] There is deliberately NO target that writes this image anywhere:
#   - no `flash-boot`.  An ST-Link write into sector 0 is the one operation that
#     bricks the board, and the only surviving board is owhinata/wio-lite-ai#2.  A build system that
#     CAN brick eventually will; the recovery procedure stays prose, in
#     boot/README.md.
#   - no `dfu-boot`.  That target would flash the BOOTLOADER into the APP
#     partition.  Measured against this tree, what it would actually do:
#     iflash.c's range check keeps it out of sector 0; the erase in
#     dfu_callbacks.c runs only when offset % IFLASH_SECTOR_SIZE (0x20000) == 0,
#     and a 30,524 B transfer only ever reaches offset 0 -- so sector 1 alone is
#     erased (not 2 or 3), the boot payload minus its first 32 B is written
#     there, and the vector-last commit is REFUSED because a bootloader reset
#     vector lies outside the app window.  Net result: the app stops booting, DFU
#     still recovers it, and one erase cycle of a ~10k budget is spent.  Cheap to
#     write down here, expensive to discover on the bench.
#
# The artifacts land in boot-reference/ instead of next to shell.bin so that a
# tab-completed `dfu-util -D ...` in the build directory cannot reach them, and
# so the directory name itself says what the image is for.
set(BOOT_DIR     "${BOARD_DIR}/boot")
set(BOOT_OUT_DIR "${CMAKE_BINARY_DIR}/boot-reference")
# objcopy will not create a parent directory, and neither will the linker's -Map.
file(MAKE_DIRECTORY "${BOOT_OUT_DIR}")

# --- boot_iface: the bootloader's usage requirements ------------------------
# Deliberately NOT bsp_iface.  This is the same set minus ${BOARD_DIR}/include --
# the app's headers -- so that a bootloader source which reaches for one fails to
# COMPILE.  That is the mechanical form of "the boot tree shares nothing with the
# app" (AGENTS.md invariant 6); a comment saying so would not survive a hurried
# edit.  All five bootloader translation units compile warning-free without it,
# and the -I ORDER is otherwise the donor's, which is part of why the image comes
# out bit-identical.
#
# ${BOOT_DIR} must come BEFORE ${TUSB_DIR}: TinyUSB resolves
# #include "tusb_config.h" along -I order and the app has a competing one in
# src/ with CFG_TUD_DFU=0.  Picking that one up compiles AND links, and yields a
# bootloader with no DFU class -- a failure that only a flashed board would show.
# (check_boot_safety.py C5 looks for the class in the image as well.)
add_library(boot_iface INTERFACE)
target_include_directories(boot_iface INTERFACE
    "${BOOT_DIR}"
    "${TUSB_DIR}"
    "${GEN_DIR}"
    "${HAL_DIR}/Inc"
    "${CMSIS_DEV}/Include"
    "${CMSIS_CORE}/Include")
target_compile_definitions(boot_iface INTERFACE
    USE_HAL_DRIVER STM32H725xx
    CFG_TUSB_MCU=OPT_MCU_STM32H7)     # selects the dwc2 port inside TinyUSB
target_compile_options(boot_iface INTERFACE
    ${MCU_OPTS} -Wall -fdata-sections -ffunction-sections -g -gdwarf-2)
target_link_options(boot_iface INTERFACE
    ${MCU_OPTS} -specs=nano.specs -specs=nosys.specs
    -Wl,--gc-sections -Wl,--print-memory-usage)  # ldscript (-T) comes from rom_finalize

# --- rom_finalize: the bootloader's counterpart to firmware_finalize --------
# Kept structurally parallel to firmware_finalize() further down so the two stay
# diffable.  It differs in exactly four ways, and every one of them is
# load-bearing:
#
#  1. -T${LDSCRIPT_ROM}: 0x08000000 / 128 KB.  Spilling into sector 1 (the app
#     partition) is a LINK ERROR rather than a runtime surprise.
#  2. It creates NO flash target.  firmware_finalize's dfu-${tgt} would become
#     "flash the bootloader into the app partition" -- the damage estimate is in
#     the section header above, and it is why this function exists at all.
#  3. The artifacts go to boot-reference/, not next to the app images.
#  4. The artifact stem is the fixed string "boot", not ${tgt}.  The logical
#     target is boot_image (the user-facing `boot` is a phony wrapper), so
#     deriving names from ${tgt} would leave boot.elf -- which comes from
#     OUTPUT_NAME -- sitting next to boot_image.bin/.hex/.map.
#
# Single-config Ninja only: Ninja Multi-Config appends a per-config subdirectory
# to RUNTIME_OUTPUT_DIRECTORY, which would make every path below (including the
# ones handed to the safety gate) depend on $<CONFIG>.  Every board in this
# repository is configured with -G Ninja.
function(rom_finalize tgt)
    target_link_options(${tgt} PRIVATE
        "-T${LDSCRIPT_ROM}" "-Wl,-Map=${BOOT_OUT_DIR}/boot.map,--cref")
    set_target_properties(${tgt} PROPERTIES
        LINK_DEPENDS             "${LDSCRIPT_ROM}"
        OUTPUT_NAME              "boot"
        RUNTIME_OUTPUT_DIRECTORY "${BOOT_OUT_DIR}")
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary -S $<TARGET_FILE:${tgt}> "${BOOT_OUT_DIR}/boot.bin"
        COMMAND "${CMAKE_OBJCOPY}" -O ihex     $<TARGET_FILE:${tgt}> "${BOOT_OUT_DIR}/boot.hex"
        COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:${tgt}>
        BYPRODUCTS "${BOOT_OUT_DIR}/boot.bin" "${BOOT_OUT_DIR}/boot.hex"
                   "${BOOT_OUT_DIR}/boot.map"
        COMMENT "objcopy -> boot-reference/boot.bin/.hex (REFERENCE build -- do NOT flash)")
endfunction()

# --- boot_image: the bootloader executable ----------------------------------
# The donor's source list in the donor's ORDER.  That order is the linker's input
# order, and this image is required to come out bit-identical to the donor's
# build of the same commit -- so it is not free to be tidied.
#
# The stock CMSIS system_stm32h7xx.c, NOT the app's src/system_stm32h7xx.c: the
# bootloader is the thing that CONFIGURES the clock tree (HSI -> 550 MHz, PLL3Q
# 48 MHz for USB, FLASH latency 3), while the app's custom SystemInit
# deliberately leaves RCC/PWR/FLASH alone because it INHERITS that tree.
#
# BOOT_TINYUSB_SOURCES is APP_TINYUSB_SOURCES plus the DFU class.  The two lists
# are spelled out separately on purpose: the app and the bootloader are allowed
# to diverge, and one shared list is how they would lose that freedom.
set(BOOT_TINYUSB_SOURCES
    "${TUSB_DIR}/tusb.c"
    "${TUSB_DIR}/common/tusb_fifo.c"
    "${TUSB_DIR}/device/usbd.c"
    "${TUSB_DIR}/class/dfu/dfu_device.c"
    "${TUSB_DIR}/class/cdc/cdc_device.c"
    "${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c"
    "${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c")

# HAL_SOURCES is shared with the app targets, and that is fine: it is a list of
# upstream submodule files, and each target recompiles them into its own objects
# with its own flags.  Do NOT "optimise" that into a shared OBJECT library -- it
# is the one refactor that would put a single set of flags into both images and
# turn this separation back into coupling, and its commit message would say
# "build time".
set(BOOT_SOURCES
    "${BOOT_DIR}/main.c"
    "${BOOT_DIR}/clock.c"
    "${BOOT_DIR}/iflash.c"
    "${BOOT_DIR}/usb_descriptors.c"
    "${BOOT_DIR}/dfu_callbacks.c"
    "${CMSIS_DEV}/Source/Templates/system_stm32h7xx.c"
    "${CMSIS_DEV}/Source/Templates/gcc/startup_stm32h725xx.s"
    ${BOOT_TINYUSB_SOURCES}
    ${HAL_SOURCES})
add_executable(boot_image ${BOOT_SOURCES})
target_link_libraries(boot_image PRIVATE boot_iface)
target_compile_options(boot_image PRIVATE -O2)

# LTO is forbidden for this target, not merely "not requested".  The safety gate
# reads the call graph out of the LINKED image, and cross-translation-unit
# inlining of the HAL flash writers into iflash.c would delete the very edges it
# checks.  -flto can arrive from CMAKE_INTERPROCEDURAL_OPTIMIZATION (including
# its per-config variants, which OVERRIDE the generic property), from a
# directory-level add_compile_options(), or from CMAKE_C_FLAGS.
#
# -fno-lto is deliberately NOT used: it would change the compile command line and
# put the donor bit-identity requirement at risk.  Instead the property is forced
# off generically AND per configuration, and check_boot_safety.py then reads the
# REAL commands out of compile_commands.json -- because a property is not a
# command line, and only the command line decides.  (The app's `shell` target
# uses LTO on purpose, so this is a boot-only rule, not a board-wide ban.)
set_property(TARGET boot_image PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
foreach(_cfg IN LISTS CMAKE_CONFIGURATION_TYPES ITEMS "${CMAKE_BUILD_TYPE}")
    if(_cfg)
        string(TOUPPER "${_cfg}" _cfg_up)
        set_property(TARGET boot_image PROPERTY
                     INTERPROCEDURAL_OPTIMIZATION_${_cfg_up} FALSE)
    endif()
endforeach()
unset(_cfg)
unset(_cfg_up)
# Per-target compile-command export (a CMake 3.20 addition, which is exactly this
# repository's minimum).  It only writes JSON -- it does not change what is
# compiled -- and it is what lets the gate audit the actual flags.
set_property(TARGET boot_image PROPERTY EXPORT_COMPILE_COMMANDS ON)

rom_finalize(boot_image)

# --- The sector-0 safety gate (cmake/check_boot_safety.py) ------------------
# The golden image.  This is the REPRODUCIBILITY BASELINE for donor
# owhinata/wio-lite-ai @ 09468bb -- established by rebuilding the donor in a
# fresh build directory, not read off a stale artefact.  It is NOT a claim about
# the byte string currently in board #2's sector 0: the only readback-verified
# log the donor left behind belongs to the pre-owhinata/wio-lite-ai#25 XIP build (30,332 B), and no
# readback hash was ever recorded for this image.  M3 asserts nothing about
# hardware behaviour.
set(BOOT_GOLDEN_SHA256
    "97ba7060d3fcd45aa3c1f585746add46e047240c7c0440c6514e50f6d0337bdf")
set(BOOT_GOLDEN_SIZE 30524)          # text 30328 / data 192 / bss 4424

# The escape hatch for a deliberate re-freeze (a HAL, TinyUSB or toolchain bump
# genuinely changes the image).  It excuses the golden hash and NOTHING else:
# every other check still runs, the ELF<->bin join included -- which is what
# stops the override from being usable to smuggle in a different binary.  A cache
# entry outlives the reason it was set, so the gate warns on every build while
# this is on.
option(BOOT_ALLOW_IMAGE_DRIFT
       "Allow boot.bin to differ from the golden hash (deliberate re-freeze)" OFF)
set(_boot_drift_flag "")
if(BOOT_ALLOW_IMAGE_DRIFT)
    set(_boot_drift_flag --allow-image-drift)
endif()

# The translation units the compile-command audit must find, written from the
# very list that was handed to add_executable().  Generated rather than
# hand-maintained on purpose: a hand-written list would drift and this one
# cannot.  What it buys is not "nobody deleted a source" -- the golden hash
# covers that -- but "the compile database really does describe this target", so
# the LTO audit can never inspect an empty set and report a pass.
string(REPLACE ";" "\n" _boot_tu_list "${BOOT_SOURCES}")
file(WRITE "${BOOT_OUT_DIR}/boot_translation_units.txt" "${_boot_tu_list}\n")
unset(_boot_tu_list)

set(BOOT_PRECHECK_STAMP "${BOOT_OUT_DIR}/boot_precheck.stamp")

# boot_precheck reads SOURCES ONLY.  Never a build artefact, and in particular
# never $<TARGET_FILE:boot_image> -- CMake adds a dependency on any target named
# by TARGET_FILE, which would close the cycle boot_image -> boot_precheck ->
# boot_image.
#
# It always runs, and on success it rewrites the stamp.  boot_image lists that
# stamp in LINK_DEPENDS, so the stamp is always newer than the previous image and
# boot_image RELINKS ON EVERY BUILD.  That is the whole mechanism: build events
# fire only when a target is actually rebuilt, so this is what makes the
# POST_BUILD checks below unconditional -- there is no up-to-date state for a
# default build, `--target boot` or `--target boot_image` to slip through.
# (add_dependencies alone would NOT do this: it orders the two targets but gives
# Ninja no reason to consider the link dirty.)
#
# The cost is one link of a 30 KB image per build; the objects are reused, so
# nothing recompiles.  That is the deliberate trade -- rebuilding from inputs we
# trust beats verifying artefacts that may already have been tampered with.
add_custom_target(boot_precheck ALL
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_boot_safety.py" precheck
            --board-dir "${BOARD_DIR}"
            --boot-dir "${BOOT_DIR}"
            --manifest "${BOARD_DIR}/cmake/boot_manifest.sha256"
            --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
            --object-dir "boot_image.dir"
            --translation-units "${BOOT_OUT_DIR}/boot_translation_units.txt"
            --stamp "${BOOT_PRECHECK_STAMP}"
            ${_boot_drift_flag}
    BYPRODUCTS "${BOOT_PRECHECK_STAMP}"
    COMMENT "check the frozen bootloader sources and compile commands"
    VERBATIM)

# APPEND, not set: rom_finalize() already put the ROM linker script in this
# property, and replacing it would drop the "relink when the linker script
# changes" edge.  The stamp is a DEPENDENCY only -- it is never handed to the
# linker, because a new link input would change the image and break the
# bit-identity requirement.
set_property(TARGET boot_image APPEND PROPERTY
             LINK_DEPENDS "${BOOT_PRECHECK_STAMP}")
add_dependencies(boot_image boot_precheck)

# POST_BUILD, attached AFTER rom_finalize's: build events run in the order they
# were added, and this one reads the boot.bin that one produces.
add_custom_command(TARGET boot_image POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_boot_safety.py" postlink
            --elf $<TARGET_FILE:boot_image>
            --bin "${BOOT_OUT_DIR}/boot.bin"
            --object-dir "${CMAKE_BINARY_DIR}/CMakeFiles/boot_image.dir"
            --translation-units "${BOOT_OUT_DIR}/boot_translation_units.txt"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            --objcopy "${CMAKE_OBJCOPY}"
            --golden-sha256 "${BOOT_GOLDEN_SHA256}"
            --golden-size "${BOOT_GOLDEN_SIZE}"
            ${_boot_drift_flag}
    COMMENT "check sector-0 safety of the bootloader image"
    VERBATIM)
unset(_boot_drift_flag)

# `boot` is what a person types.  A phony wrapper, so that the name people reach
# for cannot be the one that is up to date and silent.
add_custom_target(boot ALL
    COMMENT "bootloader REFERENCE build -> boot-reference/ (not flashable from here)")
add_dependencies(boot boot_image)

# ============================================================================
#  Apps: blink and the ThreadX shell
#  Internal flash app partition 0x08020000 (sectors 1-3); flashed over DFU
#  (dfu-util / dfu-<tgt>)
# ============================================================================
# --- Post-build artifacts + DFU flash target -------------------------------
# App firmwares (blink, shell): linked at 0x08020000, emitted as raw .bin (what the
# DFU bootloader programs over dfu-util) + .hex.  Overflowing the 384 KB partition
# is a link error -- it cannot silently grow into the bootloader's sector.
function(firmware_finalize tgt)
    target_link_options(${tgt} PRIVATE "-T${LDSCRIPT_APP}" -Wl,-Map=${tgt}.map,--cref)
    set_target_properties(${tgt} PROPERTIES LINK_DEPENDS "${LDSCRIPT_APP}")
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary -S $<TARGET_FILE:${tgt}> ${tgt}.bin
        COMMAND "${CMAKE_OBJCOPY}" -O ihex   $<TARGET_FILE:${tgt}> ${tgt}.hex
        COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:${tgt}>
        BYPRODUCTS ${tgt}.bin ${tgt}.hex
        COMMENT "objcopy -> ${tgt}.bin/.hex")

    # `dfu-<tgt>`: program the app to the internal partition over the custom DFU
    # bootloader.  Put the board in DFU mode first (hold USER/PF1 at reset -- the
    # red LED stays on).  Named per firmware so app targets never collide on one
    # `dfu` target.  (TinyUF2 and its .uf2 drive are gone: the bootloader that
    # replaced it takes a raw .bin over dfu-util, so no UF2 is produced.)
    add_custom_target(dfu-${tgt}
        COMMAND dfu-util -d 0483:df11 -a 0 -D ${tgt}.bin
        DEPENDS ${tgt}
        USES_TERMINAL
        COMMENT "dfu-util -> ${tgt}.bin to internal flash 0x08020000 (hold PF1 at reset for DFU mode)")
endfunction()

add_executable(blink
    "${BOARD_DIR}/src/blink.c"
    "${BOARD_DIR}/src/system_stm32h7xx.c"
    "${CMSIS_DEV}/Source/Templates/gcc/startup_stm32h725xx.s"
    ${HAL_SOURCES})
target_link_libraries(blink PRIVATE bsp_iface)
target_compile_options(blink PRIVATE -O2)

firmware_finalize(blink)

# --- shell: interactive Eclipse ThreadX CLI console -------------------------
# App at 0x08020000 (firmware_finalize -> dfu-shell): ThreadX + an interactive
# CLI over USB CDC.  Reuses src/system_stm32h7xx.c (clock-free SystemInit) so the
# app inherits the bootloader's 550 MHz clock tree.  ThreadX core
# (common/src) + Cortex-M7/GNU port asm; the app supplies its own
# _tx_initialize_low_level + shared SysTick handler in port/threadx/tx_glue.c.
# ONE executable compiled uniformly (TX_INCLUDE_USER_DEFINE_FILE + port/threadx on
# the include path) so the ThreadX core, the shell core and the app agree on the
# TX_THREAD layout (ABI).
set(TX_DIR  "${CMAKE_SOURCE_DIR}/lib/threadx")
set(TX_PORT "${TX_DIR}/ports/cortex_m7/gnu")
file(GLOB TX_CORE "${TX_DIR}/common/src/*.c")
list(FILTER TX_CORE EXCLUDE REGEX "tx_misra\\.c$")
file(GLOB TX_ASM  "${TX_PORT}/src/*.S")
list(FILTER TX_ASM  EXCLUDE REGEX "tx_misra\\.S$")
# Execution Profile Kit (`thread` cpu%).  It lives under utility/, not
# common/src, so the TX_CORE glob above does not pick it up -- add it explicitly.
set(TX_EPK "${TX_DIR}/utility/execution_profile_kit/tx_execution_profile.c")

# ---- NetX Duo ---------------------------------------------------------------
# The host's own TCP/IP stack.  Its "Ethernet MAC" is src/nx_link_driver.c, which puts
# raw frames on the RTL8720 link's DATA channel -- there is no MAC and no DMA here.
# Compiled into the same single executable as everything else, so every translation
# unit that sees NX_PACKET agrees on its layout (NX_INCLUDE_USER_DEFINE_FILE +
# port/netxduo on the include path), the same ABI argument as ThreadX above.
# The whole common/src glob is taken: the IPv6 sources compile to empty objects
# because nx_user.h defines NX_DISABLE_IPV6, and --gc-sections drops the rest.
set(NX_DIR "${CMAKE_SOURCE_DIR}/lib/netxduo")
file(GLOB NX_CORE "${NX_DIR}/common/src/*.c")
list(FILTER NX_CORE EXCLUDE REGEX "nx_ram_network_driver")
# DHCP is the only addon: `net dhcp` needs a client once the module's is out of the way.
set(NX_ADDONS "${NX_DIR}/addons/dhcp/nxd_dhcp_client.c")

# CDC-only TinyUSB device subset (the bootloader's set minus the DFU class).
set(APP_TINYUSB_SOURCES
    "${TUSB_DIR}/tusb.c"
    "${TUSB_DIR}/common/tusb_fifo.c"
    "${TUSB_DIR}/device/usbd.c"
    "${TUSB_DIR}/class/cdc/cdc_device.c"
    "${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c"
    "${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c")

# Shell core (ThreadX-aware; cli_core.c is the only tx_* caller) + the USB CDC and
# dummy backends + the ports of the HW-independent commands + the clean-room printf.
#
# Board-independent files come from the shared shell/ and svc/ trees; the ones that
# reach for the HAL, a board peripheral or the H725 memory map live under this
# board's own backend/, cmds/ and svc/.  The ORDER below is the donor's, unchanged:
# it is the linker's input order, and the appends further down depend on being
# appended after the option() that gates them.
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
    "${BOARD_DIR}/backend/cli_backend_usbcdc.c"
    "${CMAKE_SOURCE_DIR}/shell/backend/cli_backend_dummy.c"
    "${BOARD_DIR}/backend/cli_backend_log.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_builtin.c"
    "${BOARD_DIR}/cmds/cmd_system.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_thread.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_console.c"
    "${BOARD_DIR}/cmds/cmd_free.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_sleep.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_watch.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_jobs.c"
    "${BOARD_DIR}/cmds/cmd_devmem.c"
    "${BOARD_DIR}/cmds/cmd_psram.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_dmesg.c"
    "${BOARD_DIR}/cmds/cmd_crash.c"
    "${BOARD_DIR}/cmds/cmd_wdt.c"
    "${BOARD_DIR}/cmds/cmd_coremark.c"
    "${BOARD_DIR}/cmds/cmd_membench.c"
    "${BOARD_DIR}/cmds/cmd_wifi.c"
    "${BOARD_DIR}/cmds/cmd_wifi_flash.c"
    "${BOARD_DIR}/cmds/cmd_net.c"
    "${BOARD_DIR}/cmds/cmd_wifi_link.c"
    "${BOARD_DIR}/cmds/cmd_xfer.c"
    "${CMAKE_SOURCE_DIR}/svc/fmt.c"
    "${BOARD_DIR}/svc/timebase.c"
    "${CMAKE_SOURCE_DIR}/svc/ymodem.c"
    # The camera frame ring + sink dispatch.  Nothing calls it
    # yet (the DCMI producer is wired to it later); it is here so the engine and
    # its host tests land before the hardware work that depends on them.
    # --gc-sections drops it from the image until it has a caller.
    "${CMAKE_SOURCE_DIR}/svc/frame_pipeline.c"
    "${CMAKE_SOURCE_DIR}/svc/gfx_rot.c"
    "${BOARD_DIR}/svc/log.c")

# --- CoreMark object library (run as the shell `coremark` command) ----------
# Built once at -O3 -funroll-loops and linked into the shell firmware below;
# cmd_coremark.c calls coremark_main().  core_main.c is compiled with
# -Dmain=coremark_main so its main() does not clash with the app main() in
# src/main.c.  MEM_METHOD=MEM_MALLOC malloc's the TOTAL_DATA_SIZE (2 KB)
# working set from the newlib heap at each run's start and frees it at the end (off
# both the shell thread stack and permanent .bss); cmd_coremark.c pre-flights the
# alloc.  ITERATIONS=0 -> CoreMark auto-calibrates the run time.
set(CMK_DIR "${CMAKE_SOURCE_DIR}/lib/coremark")
add_library(coremark_obj OBJECT
    "${CMK_DIR}/core_list_join.c"
    "${CMK_DIR}/core_main.c"
    "${CMK_DIR}/core_matrix.c"
    "${CMK_DIR}/core_state.c"
    "${CMK_DIR}/core_util.c"
    "${BOARD_DIR}/port/coremark/core_portme.c")
target_link_libraries(coremark_obj PUBLIC bsp_iface)   # HAL_GetTick for timing
target_include_directories(coremark_obj PRIVATE
    "${CMK_DIR}" "${BOARD_DIR}/port/coremark")
target_compile_definitions(coremark_obj PRIVATE
    ITERATIONS=0 MEM_METHOD=MEM_MALLOC MEM_LOCATION="HEAP")
target_compile_options(coremark_obj PRIVATE -O3 -funroll-loops)
# Rename the benchmark entry so it does not collide with the app main().
set_source_files_properties("${CMK_DIR}/core_main.c" PROPERTIES
    COMPILE_DEFINITIONS "main=coremark_main")

# (cli_version.h is generated by the top-level CMakeLists.txt, right after this
# file is included -- it needs BOARD_FW_NAME, which is set at the top of it.
# GEN_DIR is already on the bsp_iface include path, so cmd_system.c can say
# #include "cli_version.h".)

# RTL8720DN download flashloader stub: the on-device flasher
# (src/rtl8720_flash.c) uploads this Realtek AmebaD ROM flashloader into the module's
# SRAM.  It is a Realtek-proprietary blob, so it is NOT committed -- fetch it at
# configure time into the build tree and embed it into GEN_DIR from there.
#
# Nothing outside the build tree is written, which is what keeps `rm -rf build/`
# equivalent to a clean checkout -- and what keeps the build from ever reading
# _ref/, a git-ignored dump of local reference material that a plain clone does
# not have.
#
# The pin is a commit, not a branch: a branch would let the blob change underneath us,
# and this one is uploaded into the module's SRAM and then executed.  EXPECTED_HASH
# both verifies the download and makes CMake skip it when the cached copy already
# matches, so re-configuring an existing build tree does not touch the network.
set(RTL_FLASHLOADER_URL
    "https://raw.githubusercontent.com/Seeed-Studio/ambd_flash_tool/de56db9716f9cbf5641fe6e440b1c96033c41962/imgtool_flashloader_amebad.bin")
set(RTL_FLASHLOADER_SHA256
    "9307121385cb390dfd2da64da2c6c515f17b5a9556b3d04021487c9b9f220b55")
# Offline / air-gapped escape hatch: point this at any local copy of the 4688 B blob.
# It is hash-checked exactly like the download -- a wrong file here would be uploaded
# to the module and run, so "the user supplied it" is not a reason to trust it.
set(RTL_FLASHLOADER_BIN "" CACHE FILEPATH
    "Local imgtool_flashloader_amebad.bin (offline builds); empty = fetch into build/")

if(RTL_FLASHLOADER_BIN)
    if(NOT EXISTS "${RTL_FLASHLOADER_BIN}")
        message(FATAL_ERROR "RTL_FLASHLOADER_BIN=${RTL_FLASHLOADER_BIN} does not exist")
    endif()
    file(SHA256 "${RTL_FLASHLOADER_BIN}" _RTL_FL_GOT)
    if(NOT _RTL_FL_GOT STREQUAL RTL_FLASHLOADER_SHA256)
        message(FATAL_ERROR
            "RTL_FLASHLOADER_BIN=${RTL_FLASHLOADER_BIN} is not the AmebaD flashloader\n"
            "  expected SHA256 ${RTL_FLASHLOADER_SHA256}\n"
            "  got      SHA256 ${_RTL_FL_GOT}")
    endif()
    set(_RTL_FL_FILE "${RTL_FLASHLOADER_BIN}")
else()
    set(_RTL_FL_FILE "${CMAKE_BINARY_DIR}/ambd/imgtool_flashloader_amebad.bin")
    file(DOWNLOAD "${RTL_FLASHLOADER_URL}" "${_RTL_FL_FILE}"
         EXPECTED_HASH "SHA256=${RTL_FLASHLOADER_SHA256}"
         TLS_VERIFY ON
         STATUS _RTL_FL_DL)
    list(GET _RTL_FL_DL 0 _RTL_FL_DL_CODE)
    if(NOT _RTL_FL_DL_CODE EQUAL 0)
        file(REMOVE "${_RTL_FL_FILE}")
        message(FATAL_ERROR
            "Could not fetch the AmebaD flashloader stub: ${_RTL_FL_DL}\n"
            "  ${RTL_FLASHLOADER_URL}\n"
            "  For an offline build, pass -DRTL_FLASHLOADER_BIN=/path/to/"
            "imgtool_flashloader_amebad.bin\n"
            "  (4688 B, load 0x00082000; it also ships inside ameba_d_tools-1.0.4 at\n"
            "  bsp/image/, and in github.com/openshwprojects/SharpRTL872xTool).")
    endif()
endif()

file(READ "${_RTL_FL_FILE}" _RTL_FL_HEX HEX)
string(LENGTH "${_RTL_FL_HEX}" _RTL_FL_HEXLEN)
math(EXPR _RTL_FL_LEN "${_RTL_FL_HEXLEN} / 2")
string(REGEX REPLACE "(..)" "0x\\1," _RTL_FL_ARRAY "${_RTL_FL_HEX}")
file(WRITE "${GEN_DIR}/rtl8720_flashloader_data.c"
"/* AUTO-GENERATED at configure time from ${_RTL_FL_FILE}.\n"
" * Do NOT edit and do NOT commit -- it is a Realtek-proprietary blob. */\n"
"#include <stdint.h>\n"
"const uint8_t rtl8720_flashloader[] = { ${_RTL_FL_ARRAY} };\n"
"const uint32_t rtl8720_flashloader_len = ${_RTL_FL_LEN};\n")

# IWDG watchdog: ON builds in the IWDG1 arming + priority-5 petter thread
# + the `wdt` starve test; OFF compiles them out entirely (`wdt info` then reports
# "disabled (build)").  Turn OFF for SWD sessions that must hold a breakpoint longer
# than the ~3 s IWDG timeout (the app does not touch DBGMCU to freeze it under debug).
option(BSP_ENABLE_IWDG "Enable the IWDG watchdog and wdt shell command" ON)
# The shell firmware is built for SIZE, not speed.  The app partition is
# 384 KB and the image had reached 82.5% of it with on-device AI not even
# started; -Os plus link-time optimisation gives back ~54 KB without touching the
# memory map, the MPU or the DFU update path -- the three things every other option
# (strings to the external NOR, XIP, feature toggles) would have had to.
# The hot paths are protected elsewhere and stay untouched: the interrupt paths live
# in ITCM (and cmake/check_itcm_residency.py below keeps them there),
# and CoreMark keeps its own -O3 -funroll-loops because the score IS the deliverable.
#
# Both knobs exist because -Os -flto makes single-stepping over SWD much worse.  For a
# debugging session build with -DBSP_OPT_LEVEL=-O2 -DBSP_ENABLE_LTO=OFF; the residency
# check runs in that configuration too.  BSP_OPT_LEVEL is a STRING, not a boolean --
# option() would silently turn it into ON/OFF.
set(BSP_OPT_LEVEL "-Os" CACHE STRING "Optimisation level for the shell firmware")
option(BSP_ENABLE_LTO "Build the shell firmware with link-time optimisation" ON)
# ThreadX idle WFI sleep (TX_ENABLE_WFI in tx_user.h).  Default ON; build
# with -DBSP_ENABLE_WFI=OFF for a busy-idle (no-sleep) variant that is easier to
# attach over SWD (a WFI-sleeping core needs connect-under-reset).
option(BSP_ENABLE_WFI "Enable ThreadX idle WFI power saving" ON)
# OCTOSPI1 APS6408 8 MB PSRAM at 0x90000000.  BSP_ENABLE_PSRAM builds the
# MPU non-cacheable window + `psram` command; BSP_PSRAM_INIT_IN_APP does the OCTOSPI1
# bring-up app-side (app-first validation).  Once the register values are validated
# the bring-up moves into the bootloader and BSP_PSRAM_INIT_IN_APP is turned OFF.
option(BSP_ENABLE_PSRAM "Enable OCTOSPI1 APS6408 PSRAM (MPU window + psram command)" ON)
option(BSP_PSRAM_INIT_IN_APP "Bring up OCTOSPI1 PSRAM app-side (app-first validation)" ON)
# Which NN backend this build links.  Declared HERE, above the BSP_ENABLE_*
# options, because BSP_ENABLE_SD's default depends on it -- the rest of the NN wiring
# stays further down with the other source lists.
#
# DEFAULT tflm SINCE ISSUE #98.  This board can afford it and the other two cannot:
# its models come from the NOR blob region at RUNTIME, so nothing has to be baked
# into the image and a clean tree still configures with no arguments.  (f746g-disco
# stays `null` for exactly that reason -- its tflm bakes a .tflite in and this repo
# ships none, so defaulting it would break `cmake -DBOARD=f746g-disco` for everyone.)
#
# [!] IT COSTS MOST OF WHAT IS LEFT OF THE 384 KB APP PARTITION.  Measured at the
# commit that made this change: 305,584 B (77.7%) as `null`, 350,096 B (89.0%) as
# `tflm` with the microSD dropped -- which is what the BSP_ENABLE_SD default below
# does, and why it does it.  Keeping the SD as well is 384,572 B (97.8%), leaving
# 8,644 B: enough to link today and not enough to add anything.  If a future change
# needs the SD back, it needs the space audited first, not just -DBSP_ENABLE_SD=ON.
#
# Existing build directories are unaffected: this is a CACHE default, so a tree
# already configured as `null` stays `null` until someone reconfigures it.
set(CONFIG_NN_BACKEND "tflm" CACHE STRING "NN inference backend: null | tflm")
set_property(CACHE CONFIG_NN_BACKEND PROPERTY STRINGS null tflm)

# On-board microSD over SDMMC1 + IDMA.  ON builds the block driver
# (port/sd), its .axi_dma bounce buffer and the `sd` command.  Turn OFF to build a
# firmware with no bus master at all -- useful when bisecting a D-cache coherency
# suspicion, since SDMMC1's IDMA is the only DMA engine this app enables.
#
# [!] The DEFAULT flips to OFF for the tflm backend.  That build adds
# ~103 KB of interpreter and kernels to a 384 KB partition, and the microSD costs
# 34,432 B of flash plus 18,624 B of AXI-SRAM -- the latter being the scarce memory,
# since it is the only RAM a bus master can address.  Nothing on the AI path
# uses it: models come from the external NOR's blob region, which is the whole reason
# that route was chosen.  Turning SD off gives back a third of what TFLM costs and
# takes nothing off the inference path.
#
# It is a DEFAULT and not a forced value: `-DBSP_ENABLE_SD=ON` still wins, because
# option() leaves an already-defined cache entry alone.  The message below makes the
# coupling visible -- a peripheral disappearing as a side effect of an unrelated switch
# is the kind of thing that should never be discovered by wondering where `sd` went.
if(CONFIG_NN_BACKEND STREQUAL "tflm" AND NOT DEFINED BSP_ENABLE_SD)
    message(STATUS "tflm backend: defaulting BSP_ENABLE_SD=OFF "
                   "(saves 34,432 B flash + 18,624 B AXI-SRAM; "
                   "models come from the NOR blob region, not the SD card). "
                   "Pass -DBSP_ENABLE_SD=ON to keep the microSD and FileX.")
    set(BSP_ENABLE_SD OFF CACHE BOOL
        "Enable the microSD card (SDMMC1) and the sd command")
endif()
option(BSP_ENABLE_SD "Enable the microSD card (SDMMC1) and the sd command" ON)

# The block driver, the FileX media stack and the command are one unit -- cmd_sd.c
# is the only caller of port/sd and port/filex, so they are added or omitted
# together.  This append must stay BELOW the option() above: SHELL_SOURCES is set
# earlier in the file, when BSP_ENABLE_SD is still undefined, so appending there
# silently drops the driver and the link fails on sd_card_init().
#
# FileX (eclipse-threadx/filex, MIT) is GLOB'd whole
# and configured through port/filex/fx_user.h, the same pattern as ThreadX and NetX
# Duo -- one executable compiled uniformly so every translation unit agrees on the
# FX_MEDIA / FX_FILE layout.  --gc-sections drops the feature paths nothing calls.
#
# fs_cmd_core.c is board-independent (it talks FileX, not SDMMC) and so lives in the
# shared tree; cmd_sd.c is this board's, and names the device through its own
# cmds/fs_devices.h.
if(BSP_ENABLE_SD)
    set(FX_DIR "${CMAKE_SOURCE_DIR}/lib/filex")
    file(GLOB FX_CORE "${FX_DIR}/common/src/*.c")
    list(FILTER FX_CORE EXCLUDE REGEX "simulat")     # RAM-disk demo drivers
    list(APPEND SHELL_SOURCES
        "${BOARD_DIR}/port/sd/sd_card.c"
        "${BOARD_DIR}/port/filex/fx_sd_driver.c"
        "${BOARD_DIR}/port/filex/sd_fs_glue.c"
        "${CMAKE_SOURCE_DIR}/shell/cmds/fs_cmd_core.c"
        "${BOARD_DIR}/cmds/cmd_sd.c"
        ${FX_CORE})
endif()

# The FPC-40 RGB LCD over LTDC + DMA2D.  ON builds the display driver
# and the `lcd` command; the two frame buffers land in the OCTOSPI1 PSRAM, so this
# only does anything useful with BSP_ENABLE_PSRAM.  Turn OFF to build a firmware
# whose LTDC never touches the clock tree (ltdc_clock_init briefly stops PLL3,
# which is also the USB console's clock) or never reads OCTOSPI1 -- useful when
# bisecting a PLL3/USB or a PSRAM-bandwidth suspicion.  Also drops the LTDC
# interlock inside psram_acquire().
option(BSP_ENABLE_LCD "Enable the FPC-40 RGB LCD (LTDC) and the lcd command" ON)


# Driver and command move together -- cmd_lcd.c is the only caller of port/ltdc --
# and, like the SD block above, this append must stay BELOW the option().
if(BSP_ENABLE_LCD)
    list(APPEND SHELL_SOURCES
        "${BOARD_DIR}/port/ltdc/ltdc_display.c"
        "${BOARD_DIR}/port/ltdc/st7789_rgb.c"
        "${BOARD_DIR}/cmds/cmd_lcd.c")
endif()

# The external W25Q128 NOR on OCTOSPI2 and the
# configuration key-value store that lives on it.  ON builds the NOR driver
# (port/nor) and the `nor` command; the FlashDB key-value layer is added to this
# same block as it lands.  Turn OFF for a firmware that never configures the
# OCTOSPI2 pins or registers -- useful when bisecting anything that suspects the
# second OCTOSPI of disturbing the PSRAM on the first.
#
# NOTE: this does NOT open the memory-mapped window at 0x70000000.  The driver is
# indirect-only and src/mpu.c keeps its no-access + XN region over that quarter
# either way -- see port/nor/nor_flash.h for why that fence stays.
option(BSP_ENABLE_KV "Enable the external NOR flash (OCTOSPI2) and the config KV store" ON)

# Driver, database and commands move together, and -- like the SD/LCD/camera
# blocks -- this append must stay BELOW the option().
#
# FlashDB (armink/FlashDB, Apache-2.0) is taken as three core
# sources plus the FAL it ships in its own port/ directory.  Unlike ThreadX/FileX/
# NetX Duo above, this one is NOT GLOB'd whole: fdb_tsdb.c (time-series database)
# and fdb_file.c (filesystem-backed storage) are features port/flashdb/fdb_cfg.h
# does not enable, and fal_rtt.c is the RT-Thread integration and does not build
# outside it.  Configuration comes from port/flashdb (fdb_cfg.h + fal_cfg.h), which
# is why that directory is on the include path before FlashDB's own.
if(BSP_ENABLE_KV)
    set(FDB_DIR "${CMAKE_SOURCE_DIR}/lib/flashdb")
    list(APPEND SHELL_SOURCES
        "${BOARD_DIR}/port/nor/nor_flash.c"
        "${BOARD_DIR}/port/flashdb/fal_nor.c"
        "${BOARD_DIR}/src/kv.c"
        "${BOARD_DIR}/src/kv_boot.c"
        "${BOARD_DIR}/src/blob.c"
        "${BOARD_DIR}/cmds/cmd_nor.c"
        "${BOARD_DIR}/cmds/cmd_kv.c"
        "${BOARD_DIR}/cmds/cmd_blob.c"
        "${FDB_DIR}/src/fdb.c"
        "${FDB_DIR}/src/fdb_kvdb.c"
        "${FDB_DIR}/src/fdb_utils.c"
        "${FDB_DIR}/port/fal/src/fal.c"
        "${FDB_DIR}/port/fal/src/fal_flash.c"
        "${FDB_DIR}/port/fal/src/fal_partition.c")
endif()

# The FPC-24 DVP camera.  Phase 1 builds only the sensor-side bring-up
# (XCLK on TIM5_CH3, SCCB on I2C4, PWDN/RESETB) and the `camera` command; DCMI and
# DMA arrive in phase 2.  Turn OFF for a firmware that leaves TIM5, I2C4 and the
# camera pins untouched -- useful when bisecting a timer or D3-domain suspicion.
option(BSP_ENABLE_CAMERA "Enable the FPC-24 DVP camera (DCMI) and the camera command" ON)

# Driver and command move together -- cmd_camera.c is the only caller of
# port/camera -- and, like the two blocks above, this append must stay BELOW the
# option().
if(BSP_ENABLE_CAMERA)
    list(APPEND SHELL_SOURCES
        "${BOARD_DIR}/port/camera/camera.c"
        "${BOARD_DIR}/port/camera/ov2640_regs.c"
        # The band stream now has two consumers (the preview and
        # the NN ingest), and port/camera holds exactly one callback.  The mux is
        # gated on the camera ALONE -- the NN client needs no display, and running
        # inference with the preview off is the fastest configuration this board has.
        "${BOARD_DIR}/src/cam_band.c"
        # The camera -> inference glue.  Camera-only for the same
        # reason as the mux; the overlay it feeds is drawn by cam_preview.c, which
        # is the file already gated on the pair.
        "${BOARD_DIR}/src/nn_camera.c"
        "${BOARD_DIR}/cmds/cmd_camera.c")
    # The LCD preview needs BOTH drivers, so it is the one
    # source gated on the pair.  With either option off there is nothing to glue
    # and the file is simply not built.
    if(BSP_ENABLE_LCD)
        list(APPEND SHELL_SOURCES "${BOARD_DIR}/src/cam_preview.c")
    endif()
endif()

# On-device NN inference.  Unlike the blocks above there is no
# BSP_ENABLE_AI: the BSP_ENABLE_* switches all name a piece of HARDWARE, and their
# point is to build a firmware that leaves those pins and peripherals alone while
# bisecting a suspicion.  This is a software layer with no hardware of its own, so
# what varies is not whether it exists but WHICH RUNTIME it dispatches to.
#
# Exactly one backend translation unit is compiled, and that is the whole selection
# mechanism: nn.c has one `extern const struct nn_backend_vt nn_backend_vt_selected`
# and the linker resolves it against whichever backend was added here.  No source
# file has an #ifdef for this, and the default `null` build needs no -D at all.
#
# If the app partition ever gets tight again, a `none` value that drops
# the whole layer is the natural place to add the toggle.
#
# CONFIG_NN_BACKEND itself is declared much earlier in this file, above the BSP_ENABLE_*
# block, because BSP_ENABLE_SD's default depends on which backend was selected.
set(NN_SOURCES "${BOARD_DIR}/port/nn/nn.c"
               "${BOARD_DIR}/port/nn/nn_svc_wio.c")  # the shared `nn` command's adapter
# Which buffers the cacheable-carve-out gate must find in the image depends on which
# backend was selected, and CMake is the only place that knows.  When the list was
# hard-coded in the script, the script itself refused every tflm build -- the `null`
# stub's buffers are not in that image.
# [!] ONE VARIABLE FOR THE SHARED DECODER'S PATH (issue #97): the source list and
# the residency gate below must name the same file, and spelling it twice is how
# they drift.
get_filename_component(WIO_SHARED_DECODER
                       "${CMAKE_SOURCE_DIR}/svc/blazeface.c" ABSOLUTE)
if(NOT EXISTS "${WIO_SHARED_DECODER}")
    message(FATAL_ERROR
        "shared BlazeFace decoder not found:\n  ${WIO_SHARED_DECODER}\n"
        "This board builds it (issue #97); it is not optional.")
endif()

set(NN_PSRAM_AI_REQUIRED "")
if(CONFIG_NN_BACKEND STREQUAL "null")
    list(APPEND NN_SOURCES "${BOARD_DIR}/port/nn/nn_null.c")
    list(APPEND NN_PSRAM_AI_REQUIRED
         --require null_in_buf --require null_box_buf --require null_scr_buf)
elseif(CONFIG_NN_BACKEND STREQUAL "tflm")
    # TensorFlow Lite Micro.  The backend translation unit is NOT
    # added to SHELL_SOURCES: it is C++ and lives in the `tflm` static library that
    # this file builds, together with the fetched tflite-micro + CMSIS-NN tree.  Only
    # nn.c stays here, and it finds nn_backend_vt_selected in that archive at link
    # time -- the same one-definition mechanism the `null` build uses.
    include("${BOARD_DIR}/cmake/tflite-micro.cmake")
    list(APPEND NN_PSRAM_AI_REQUIRED
         --require nn_tflm_arena --require nn_tflm_model_buf)
    # Tells the board's nn_svc_config.h that a RUNTIME MODEL LOADER exists, so
    # the shared `nn` command registers `model load` (issue #50).  The same
    # spelling f746g-disco already uses: the capability follows the backend, not
    # the board, because the same board answers differently in two builds.
    set(NN_BACKEND_DEFINE CONFIG_NN_BACKEND_TFLM=1)
else()
    message(FATAL_ERROR "CONFIG_NN_BACKEND must be 'null' or 'tflm'")
endif()
# membench's cacheable row is in the carve-out whichever backend is built.
list(APPEND NN_PSRAM_AI_REQUIRED --require psram_ai_bench_buf)
# Model-specific post-processing, above the model-agnostic nn API.  The decoder
# itself is SHARED with the other two boards since issue #97; port/nn/nn_decoder.c
# is this board's half -- nn_tensor -> tensor_desc, and the ownership of the
# decoder's state and its candidate scratch, because the shared translation unit
# owns no storage at all.
#
# Built UNCONDITIONALLY -- including in the `null` build, whose stub tensors it
# simply does not recognise (the decoder returns BF_ERR_MODEL without touching
# anything).  That is what lets `ai dets` be registered unconditionally, and
# keeping the reference in every build is also what keeps the --require line below
# honest: a symbol --gc-sections dropped would be reported as "no such object in
# the image", which reads like a placement regression and is not one.
#
# [!] The two anchor tables are gone -- the shared decoder computes the centres --
# so only the candidate scratch is still placed, and it is the BOARD's object, not
# the shared file's.  That is the whole reason the scratch is passed in: it keeps
# the residency gate pointing at something this board owns.
list(APPEND NN_SOURCES "${WIO_SHARED_DECODER}"
                       "${CMAKE_SOURCE_DIR}/svc/nn_det_record.c"
                       "${BOARD_DIR}/port/nn/nn_decoder.c")
list(APPEND NN_PSRAM_AI_REQUIRED --require nn_dec_scratch)
# [!] AND THE STATE MUST STAY OUT (issue #97).  The scratch requirement above says
# nothing about the decoder's state, and moving `nn_dec` / `nn_dec_ready` into the
# carve-out would leave every gate green -- while .psram_ai is NOLOAD, so the
# threshold would come up holding the previous run's bytes and a `ready` flag that
# followed it would survive a warm reset still saying ready, skipping
# initialisation over stale state.  It would also break the fail-soft rule: the
# shell runs when the PSRAM bring-up failed, and `ai thresh` has to keep answering.
list(APPEND NN_PSRAM_AI_REQUIRED
     --internal-ram nn_dec --internal-ram nn_dec_ready)
# The other half of that gate: PSRAM buffers a bus master owns, which must stay OUT
# of the cacheable carve-out.  Named here rather than in the script for the same
# reason as the DTCM list above -- each belongs to a BSP_ENABLE_* option, and a name
# demanded from a build that never compiled it is reported as "no such object in the
# image", which reads like a placement regression and is not one.  That is why
# check_dtcm_residency.py was fixed the same way: while this gate kept
# its hard-coded list, BSP_ENABLE_CAMERA=OFF still could not be built.
if(BSP_ENABLE_LCD)
    list(APPEND NN_PSRAM_AI_REQUIRED --noncacheable ltdc_fb)   # LTDC scan-out + DMA2D
endif()
if(BSP_ENABLE_CAMERA)
    list(APPEND NN_PSRAM_AI_REQUIRED --noncacheable cam_ring)  # DCMI via DMA2_Stream1
    list(APPEND NN_PSRAM_AI_REQUIRED --noncacheable cam_frame) # DCMI single capture
endif()
list(APPEND SHELL_SOURCES ${NN_SOURCES}
     "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_nn.c"      # the one shared command
     "${CMAKE_SOURCE_DIR}/shell/cmds/nn_cmd_core.c" # its pure half
     "${BOARD_DIR}/cmds/cmd_nn_board.c")            # this board's `nn stream`

# The MLPerf Tiny v1.4 benchmark harness.  Like CONFIG_NN_BACKEND above and
# for the same reason, this is NOT a BSP_ENABLE_* switch -- those all name a piece of
# hardware, and the point of each is a firmware that leaves those pins alone while
# bisecting a suspicion.  There is no MLPerf peripheral.
#
# OFF by default because it is not free: it needs the ~340 MB upstream mirror, it adds
# ~28 KB of harness and gp_buff to a partition that is already 93% full, and the
# operator ops it needs (NN_TFLM_OPS=mlperf) cost more flash again.  A benchmark run is
# a deliberate act, so turning it on is one too.
option(CONFIG_MLPERF_TINY "Build the MLPerf Tiny v1.4 benchmark harness and `mlperf`" OFF)
if(CONFIG_MLPERF_TINY)
    # Required, not merely recommended.  The harness runs whatever model `ai model
    # load` put in the interpreter, and the `null` backend has no interpreter --
    # nn_model_reload() returns NN_ERR_NOSUP.  The build would succeed and the command
    # would refuse every model, which is the least useful place to discover this.
    if(NOT CONFIG_NN_BACKEND STREQUAL "tflm")
        message(FATAL_ERROR
            "CONFIG_MLPERF_TINY=ON needs CONFIG_NN_BACKEND=tflm (it benchmarks a model "
            "the interpreter loads at run time; the `null` backend has no interpreter). "
            "Configure with -DCONFIG_NN_BACKEND=tflm -DNN_TFLM_OPS=mlperf.")
    endif()
    # Not fatal: the operator set is a separate decision, and someone may deliberately
    # be measuring a narrower one.  But `blazeface` cannot run a single MLPerf model,
    # so saying nothing would let the failure surface as an unexplained refusal from
    # `ai model load` instead.
    if(NOT NN_TFLM_OPS STREQUAL "mlperf" AND NOT NN_TFLM_OPS STREQUAL "extended")
        message(WARNING
            "CONFIG_MLPERF_TINY=ON with NN_TFLM_OPS=${NN_TFLM_OPS}: none of the MLPerf "
            "Tiny models will load (they need FULLY_CONNECTED, AVERAGE_POOL_2D and "
            "SOFTMAX).  Add -DNN_TFLM_OPS=mlperf.")
    endif()
    include("${BOARD_DIR}/cmake/mlperf-tiny.cmake")
    list(APPEND SHELL_SOURCES "${BOARD_DIR}/cmds/cmd_mlperf.c")
endif()

add_executable(shell
    "${BOARD_DIR}/src/main.c"
    "${BOARD_DIR}/src/usb_cdc.c"
    "${BOARD_DIR}/src/usb_descriptors.c"
    "${BOARD_DIR}/src/retarget.c"
    "${BOARD_DIR}/src/fault.c"
    "${BOARD_DIR}/src/iwdg.c"
    "${BOARD_DIR}/src/malloc_lock.c"
    "${BOARD_DIR}/src/mpu.c"
    "${BOARD_DIR}/src/psram.c"
    "${BOARD_DIR}/src/rtl8720.c"
    "${BOARD_DIR}/src/rtl8720_flash.c"
    "${BOARD_DIR}/src/rtl8720_img.c"
    "${GEN_DIR}/rtl8720_flashloader_data.c"
    "${BOARD_DIR}/src/erpc.c"
    "${BOARD_DIR}/src/link_data.c"
    "${BOARD_DIR}/src/wifi_rpc.c"
    "${BOARD_DIR}/src/wifi_auto.c"
    "${BOARD_DIR}/src/wifi_connect.c"
    "${BOARD_DIR}/src/rtl_link.c"
    "${BOARD_DIR}/src/net_shell.c"
    "${BOARD_DIR}/src/nx_echo.c"
    "${BOARD_DIR}/src/nx_link_driver.c"
    "${BOARD_DIR}/src/nx_net.c"
    "${BOARD_DIR}/port/threadx/tx_glue.c"
    "${BOARD_DIR}/src/system_stm32h7xx.c"
    "${CMSIS_DEV}/Source/Templates/gcc/startup_stm32h725xx.s"
    ${SHELL_SOURCES}
    ${APP_TINYUSB_SOURCES}
    ${TX_CORE} ${TX_ASM} ${TX_EPK}
    ${NX_CORE} ${NX_ADDONS}
    ${HAL_SOURCES})
target_link_libraries(shell PRIVATE bsp_iface coremark_obj)
# CoreMark's canonical report prints its score with %f; pull in newlib's float
# printf (newlib-nano omits it by default).
target_link_options(shell PRIVATE -u _printf_float)
target_include_directories(shell PRIVATE
    "${BOARD_DIR}/src"                           # board app headers + tusb_config.h
    "${BOARD_DIR}/port/threadx"
    "${BOARD_DIR}/port/sd"                       # sd_card.h
    "${BOARD_DIR}/port/filex"                    # fx_user.h + the SD glue
    "${BOARD_DIR}/port/ltdc"                     # ltdc_display.h
    "${BOARD_DIR}/port/camera"                   # camera.h
    "${BOARD_DIR}/port/nor"                      # nor_flash.h
    "${BOARD_DIR}/port/nn"                       # nn.h / nn_backend.h
    "${BOARD_DIR}/port/nn/models"                # blazeface.h
    # Our fdb_cfg.h / fal_cfg.h must be found BEFORE FlashDB's own inc/,
    # which ships fdb_cfg_template.h only -- FlashDB includes <fdb_cfg.h> by name.
    "${BOARD_DIR}/port/flashdb"
    "${CMAKE_SOURCE_DIR}/lib/flashdb/inc"
    "${CMAKE_SOURCE_DIR}/lib/flashdb/port/fal/inc"
    "${CMAKE_SOURCE_DIR}/lib/filex/common/inc"
    "${CMAKE_SOURCE_DIR}/lib/filex/ports/cortex_m7/gnu/inc"
    "${CMAKE_SOURCE_DIR}/shell/include"
    "${CMAKE_SOURCE_DIR}/shell/core"
    "${CMAKE_SOURCE_DIR}/shell/backend"
    # The shared commands' own headers: this board's cmds/ live in a different
    # directory now, so cmd_sd.c / cmd_camera.c can no longer reach fs_cmd_core.h
    # through the "same directory first" rule for quoted includes.  Nothing else
    # is in there, so no existing include can be shadowed by adding it.
    "${CMAKE_SOURCE_DIR}/shell/cmds"
    "${BOARD_DIR}/backend"                       # cli_backend_usbcdc.h / _log.h
    "${CMAKE_SOURCE_DIR}/svc"
    "${BOARD_DIR}/svc"                           # log.h / timebase.h: the board's
                                                 # services, which the shared
                                                 # cmd_dmesg.c / cmd_sleep.c consume
    "${TUSB_DIR}"
    "${TX_DIR}/common/inc"
    "${TX_DIR}/utility/execution_profile_kit"   # tx_execution_profile.h
    "${TX_PORT}/inc"
    "${NX_DIR}/common/inc"                      # NetX Duo
    "${NX_DIR}/ports/cortex_m7/gnu/inc"
    "${NX_DIR}/addons/dhcp"
    "${BOARD_DIR}/port/netxduo")                # nx_user.h
target_compile_definitions(shell PRIVATE
    CFG_TUSB_MCU=OPT_MCU_STM32H7
    TX_INCLUDE_USER_DEFINE_FILE
    NX_INCLUDE_USER_DEFINE_FILE        # port/netxduo/nx_user.h
    # The DHCP client shares the one packet pool instead of creating its own, and its
    # thread sits just below the NetX IP thread (11) so IP processing always wins.
    NX_DHCP_CLIENT_USER_CREATE_PACKET_POOL
    NX_DHCP_THREAD_PRIORITY=13
    CLI_ENABLE_DANGEROUS_CMDS=1        # reboot / devmem / crash
    CLI_INSTANCE_STACK_SIZE=4096       # headroom for cli_print + coremark
    CLI_BG_JOB_STACK_SIZE=4096
    CLI_USBCDC_TX_BUFFER_SIZE=4096     # holds a full CoreMark report burst
    # The console also RECEIVES bulk data (`wifi imgload` takes a
    # firmware image over YMODEM).  The backend drops -- and counts -- a byte
    # whenever this ring is full, so the default 256 B is too tight for a 1029 B
    # YMODEM block; 4 KB makes an overrun require the shell thread to be starved
    # for four whole blocks.  `wifi imgload` reports the drop count either way.
    CLI_USBCDC_RX_BUFFER_SIZE=4096
    ${NN_BACKEND_DEFINE}               # set only for a backend with a loader
    BSP_ENABLE_IWDG=$<BOOL:${BSP_ENABLE_IWDG}>   # IWDG1 arm + petter + wdt
    BSP_ENABLE_WFI=$<BOOL:${BSP_ENABLE_WFI}>      # gates TX_ENABLE_WFI (tx_user.h)
    BSP_ENABLE_PSRAM=$<BOOL:${BSP_ENABLE_PSRAM}>          # PSRAM window + psram cmd
    BSP_PSRAM_INIT_IN_APP=$<BOOL:${BSP_PSRAM_INIT_IN_APP}>  # app-side OCTOSPI1 bring-up
    FX_INCLUDE_USER_DEFINE_FILE             # port/filex/fx_user.h
    BSP_ENABLE_SD=$<BOOL:${BSP_ENABLE_SD}>                  # SDMMC1 microSD + sd cmd
    BSP_ENABLE_LCD=$<BOOL:${BSP_ENABLE_LCD}>                # LTDC LCD + lcd cmd
    BSP_ENABLE_CAMERA=$<BOOL:${BSP_ENABLE_CAMERA}>          # DVP camera + camera cmd
    BSP_ENABLE_KV=$<BOOL:${BSP_ENABLE_KV}>)                 # OCTOSPI2 NOR + KV store
# -Os + LTO, plus two flags that disable optimisations which are legal C
# but wrong for THIS firmware.  Both are cheap (measured: 1,008 B and 224 B) and both
# only start to matter under LTO, which propagates these inferences ACROSS
# translation units for the first time:
#
#  -fno-strict-aliasing            NetX Duo and FileX cast packet buffers to protocol
#                                  headers constantly.  Whole-program alias analysis is
#                                  exactly the condition under which those type-punning
#                                  violations stop being harmless.
#  -fno-delete-null-pointer-checks ITCM lives at 0x00000000 on this part, so address 0
#                                  is a real, deliberately accessed location -- our
#                                  SystemInit writes and reads back the whole 64 KB
#                                  (src/system_stm32h7xx.c).  Without this, "code
#                                  dereferenced this pointer, so it cannot be NULL" is
#                                  free to travel between translation units.
set(SHELL_OPT_FLAGS ${BSP_OPT_LEVEL} -fno-strict-aliasing -fno-delete-null-pointer-checks)
if(BSP_ENABLE_LTO)
    list(APPEND SHELL_OPT_FLAGS -flto=auto)
endif()
target_compile_options(shell PRIVATE ${SHELL_OPT_FLAGS})
# ...except membench, which is an INSTRUMENT and must keep its calibration.
# Its bandwidth loops are the yardstick the PSRAM was tuned against and the reason
# execution was moved to the internal flash (54 vs 905 MB/s), so a reading
# has to mean the same thing across builds.  -Os disables loop unrolling, which turns
# the read loop from 73 instructions into 47 and makes it LOOP-bound rather than
# memory-bound: measured that way, ITCM, DTCM and cached SRAM all report an identical
# 1453 MB/s and the internal flash reads "859 MB/s" instead of 905 -- numbers about the
# benchmark, not about the memory.  Same reasoning as coremark_obj above; -fno-lto
# keeps the whole-program optimiser from reintroducing the dependency at link time.
set_source_files_properties("${BOARD_DIR}/cmds/cmd_membench.c"
    PROPERTIES COMPILE_OPTIONS "-O2;-fno-lto")
# The SAME flags again at link time: with LTO the final code generation happens in the
# linker, so compile-only flags are advisory at best.  -ffunction-sections /
# -fdata-sections in particular MUST be repeated here -- without them LTO emits one
# monolithic .text, which defeats --gc-sections AND makes every .itcm input-section
# pattern in the linker script stop matching.
target_link_options(shell PRIVATE ${SHELL_OPT_FLAGS} -ffunction-sections -fdata-sections)

# Link the C++ TFLM backend in, and force the final link back onto
# the C driver.  Everything that decision involves is in cmake/tflite-micro.cmake --
# it has to run here rather than in that file because the `shell` target does not exist
# yet where the backend selection above includes it.
if(CONFIG_NN_BACKEND STREQUAL "tflm")
    nn_tflm_attach(shell)
endif()

# The benchmark harness objects.  An OBJECT library, so these land in the
# image exactly as if they had been listed in add_executable -- but compiled with the
# C++ containment flags `shell` itself does not carry (cmake/mlperf-tiny.cmake).  The
# link driver stays C: nn_tflm_attach() above already forces LINKER_LANGUAGE, which is
# required anyway and covers this too.
if(CONFIG_MLPERF_TINY)
    target_link_libraries(shell PRIVATE mlperf_obj)
    target_include_directories(shell PRIVATE "${BOARD_DIR}/port/mlperf")
    target_compile_definitions(shell PRIVATE CONFIG_MLPERF_TINY=1)
endif()

firmware_finalize(shell)

# `flash`: the target name BOTH boards answer to, so the flash step in the docs and
# in the development cycle is one command with the build directory swapped, not a
# per-board incantation to remember.  Here it is an alias for `dfu-shell` -- the
# shell app is what "flash this board" means; blink stays reachable as `dfu-blink`.
#
# It cannot reach sector 0.  It runs exactly the dfu-util command `dfu-shell` runs,
# into the app partition at 0x08020000, over the bootloader that lives in sector 0
# and is never a target of this build (no flash-boot, no dfu-boot -- see the boot
# tree's README).  The name is deliberately NOT the f746 meaning of `flash`: there
# is no ST-Link path here, and internal-flash endurance is ~10k cycles, so this is
# a hand-run command and never a loop.
add_custom_target(flash)
add_dependencies(flash dfu-shell)

# The last line of defence for the ITCM residency of the interrupt paths.
# The linker-script ASSERTs cannot do this job under LTO -- see the
# comment on them in ldscript/STM32H725AEIx_IROM.ld -- because LTO renames the very
# symbols they are written against.  This checks the linked image instead, and is
# deliberately attached to `shell` only: blink shares the linker script but has no
# ThreadX and no RTL8720 UART, so the required-symbol list does not apply to it.
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_itcm_residency.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            $<TARGET_FILE:shell>
    COMMENT "check .itcm residency of the interrupt paths"
    VERBATIM)

# The same kind of gate for the AXI-SRAM / DTCM split, and it guards both
# directions: a stack that quietly returned to AXI-SRAM (budget regression, no error)
# and a DMA buffer that quietly moved to DTCM (the transfer moves nothing, no error).
# `shell` only, like the check above: blink has none of these objects.
#
# The objects that only exist in SOME configurations are named here rather than in the
# script, for the reason spelled out at the top of it: a required symbol the build never
# compiled is reported as "no such object in the image", which reads like a placement
# regression and is not one.  Before this, `BSP_ENABLE_SD=OFF` could not be built at
# all -- the gate failed on sd_bounce -- which defeated the option's entire purpose,
# since it exists to produce a firmware with no bus master while bisecting a D-cache
# suspicion.  The condition on each line below is the same one that adds its source.
set(DTCM_REQUIRED "")
if(BSP_ENABLE_KV)
    list(APPEND DTCM_REQUIRED --require-dtcm kv_boot_stack)      # src/kv_boot.c
endif()
if(BSP_ENABLE_CAMERA)
    list(APPEND DTCM_REQUIRED --require-dtcm cam_producer_stack) # port/camera/camera.c
    list(APPEND DTCM_REQUIRED --require-dtcm nn_worker_stack)    # src/nn_camera.c
    list(APPEND DTCM_REQUIRED --require-axi  cam_band)           # DCMI via DMA2_Stream1
    if(BSP_ENABLE_LCD)
        list(APPEND DTCM_REQUIRED --require-dtcm preview_stack)  # src/cam_preview.c
    endif()
endif()
if(BSP_ENABLE_SD)
    list(APPEND DTCM_REQUIRED --require-axi sd_bounce)           # SDMMC1 IDMA
endif()

add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_dtcm_residency.py"
            --nm "${CMAKE_NM}" ${DTCM_REQUIRED}
            $<TARGET_FILE:shell>
    COMMENT "check .dtcm_bss residency and DMA reachability"
    VERBATIM)

# And the same kind of gate for the cacheable PSRAM carve-out.
# It guards the rule that replaced "everything in PSRAM is safe for DMA" once the top
# 2 MB stopped being non-cacheable, and that rule has no runtime enforcement at all: a
# DMA buffer in there transfers correctly and is then read back stale from the data
# cache, intermittently, only under cache pressure.  `shell` only, like the two above.
# --- the shared decoder must own no storage (issue #97) -----------------------
#
# See cmake/shared_storage_gate.cmake for what this checks and why it has to be
# THIS board's compile rather than a generic one.
include("${CMAKE_SOURCE_DIR}/cmake/shared_storage_gate.cmake")
# The same rule on the one shared `nn` command and its pure half (issue #50).
add_shared_storage_gate(NAME wio_nn_cmd_audit
                        SOURCE "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_nn.c"
                        IFACE bsp_iface CONSUMER shell)
add_dependencies(shell wio_nn_cmd_audit_check)
add_shared_storage_gate(NAME wio_nn_core_audit
                        SOURCE "${CMAKE_SOURCE_DIR}/shell/cmds/nn_cmd_core.c"
                        IFACE bsp_iface CONSUMER shell)
add_dependencies(shell wio_nn_core_audit_check)

add_shared_storage_gate(NAME wio_decoder_audit SOURCE "${WIO_SHARED_DECODER}"
                         IFACE bsp_iface CONSUMER shell)
add_dependencies(shell wio_decoder_audit_check)

add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_psram_ai_residency.py"
            --nm "${CMAKE_NM}" ${NN_PSRAM_AI_REQUIRED}
            $<TARGET_FILE:shell>
    COMMENT "check .psram_ai residency and cache safety"
    VERBATIM)

# The C++ containment gate, for the tflm build only.  This firmware
# had no C++ at all until that phase, and the two things that go wrong when C++ arrives
# in a bare-metal image are both invisible at link time.
#
#   A global constructor runs from __libc_init_array, which the startup code calls
#   immediately BEFORE main -- so before the MPU is programmed, before the caches are
#   on, before the PSRAM window is usable, before log_init() and long before ThreadX.
#   A constructor that touches the carve-out hangs the board with nothing in `dmesg`.
#
#   The exception machinery re-enters through the back door: one undefined reference to
#   the throwing operator new is enough for the linker to extract __cxa_throw and the
#   whole unwinder, in a build compiled -fno-exceptions from end to end.
#
# Both are checked on the linked image, because a linker-script ASSERT cannot do it:
# C++ name mangling defeats DEFINED(sym) selectors the same way LTO's renaming does --
# the condition goes false, the ternary collapses to 1, and the ASSERT passes.
if(CONFIG_NN_BACKEND STREQUAL "tflm")
    add_custom_command(TARGET shell POST_BUILD
        COMMAND "${Python3_EXECUTABLE}"
                "${BOARD_DIR}/cmake/check_cxx_runtime.py"
                --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
                $<TARGET_FILE:shell>
        COMMENT "check C++ runtime containment: no static ctors, no unwinder"
        VERBATIM)

    # Not a gate -- a report.  The shell thread's stack is 4,096 B and DTCM has under
    # 8 KB free, so there is very little room to react if a CMSIS-NN kernel turns out to
    # want a deep frame.  -fstack-usage answers that at build time instead of waiting
    # for a `free` high-water reading to come back from the board.
    add_custom_command(TARGET shell POST_BUILD
        COMMAND "${Python3_EXECUTABLE}"
                "${BOARD_DIR}/cmake/report_tflm_stack.py"
                "${CMAKE_BINARY_DIR}/CMakeFiles/tflm.dir"
        COMMENT "report the deepest tflm stack frames"
        VERBATIM)
endif()
