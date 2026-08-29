# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# The audit target behind cmake/check_no_mutable_storage.py (issues #97, #50).
#
# A file shared by three boards may own no mutable storage.  svc/blazeface.c was
# the first: each board passes in its own candidate scratch, so the scratch keeps
# that board's placement and that board's residency gate keeps naming a symbol the
# board owns.  shell/cmds/cmd_nn.c is the second, for the same reason -- a static
# added to the one shared `nn` command would land in memory no board placed, and
# one of the three has no residency gate that would notice.  This compiles a
# shared file once more, on its own, and refuses any allocated writable section in
# the result.
#
# [!] IT MUST BE THE BOARD'S OWN COMPILE, or the gate is answering about a
# different object than the one that ships.  Two things were got wrong first and
# are the reason this lives in a function rather than three times in three
# board.cmake files:
#
#   1. INHERITING INCLUDES AND DEFINITIONS IS NOT ENOUGH.  The architecture flags
#      (-mcpu, -mthumb, -mfpu, -mfloat-abi, -mcmse) come from the board's
#      INTERFACE library, not from CMAKE_C_FLAGS, so a target that only copied
#      INCLUDE_DIRECTORIES and COMPILE_DEFINITIONS compiled for the compiler's
#      DEFAULT machine.  Linking the interface library is what makes the audit the
#      board's compile.  Measured before the fix: the audit's whole flag list was
#      "-O0 -fno-lto -fno-common".
#   2. AN AUDIT THAT COMPILES A DIFFERENT TRANSLATION UNIT ANSWERS A DIFFERENT
#      QUESTION.  The first attempt at fixing (1) compiled a generated wrapper
#      that #included the shared file -- which made `__INCLUDE_LEVEL__` 1 here and
#      0 in the shipped build, so storage behind `#if __INCLUDE_LEVEL__ == 0`
#      would exist only where nobody looked.  Auditing at -O0 while the build ships
#      -Os/-O2/-O3 has the same shape through `__OPTIMIZE__`.
#
# So the audit compiles the REAL source with the consuming target's real options,
# and overrides exactly one thing: -fno-lto, because two of the three boards build
# with LTO and an LTO object is bytecode with no sections to inspect.  Everything
# that decides WHETHER storage exists -- definitions, includes, architecture,
# optimisation, per-source options -- is the shipped build's.
#
# [!] WHAT IT COPIES, EXACTLY: the interface library it links, plus the consumer's
# INCLUDE_DIRECTORIES, COMPILE_DEFINITIONS and COMPILE_OPTIONS.  It does NOT copy
# usage requirements from any OTHER library the consumer links, nor the target's
# COMPILE_FLAGS, C_STANDARD or compile features.  Verified equal on all three
# boards today, flag for flag; if a board later gives this file options by one of
# those routes, the audit stops being the board's compile and this list is where
# to start.
#
# [!] -fno-common is NOT set, deliberately.  It only suppresses tentative
# definitions, and an explicit __attribute__((common)) survives it with no section
# for the checker to find.  check_no_mutable_storage.py refuses COMMON symbols
# outright instead, which covers both.
#
function(add_shared_storage_gate)
    cmake_parse_arguments(DSG "" "NAME;SOURCE;IFACE;CONSUMER" "" ${ARGN})
    if(NOT DSG_NAME OR NOT DSG_SOURCE OR NOT DSG_IFACE)
        message(FATAL_ERROR
            "add_shared_storage_gate: NAME, SOURCE and IFACE are all required")
    endif()
    if(NOT EXISTS "${DSG_SOURCE}")
        message(FATAL_ERROR
            "add_shared_storage_gate: no such source:\n  ${DSG_SOURCE}")
    endif()

    # [!] The REAL source, not a copy or a wrapper: see the note above.
    add_library(${DSG_NAME} OBJECT EXCLUDE_FROM_ALL "${DSG_SOURCE}")
    # The board's real compile: architecture, includes and definitions together.
    target_link_libraries(${DSG_NAME} PRIVATE ${DSG_IFACE})
    # ...and everything the consuming target adds on top, INCLUDING its compile
    # options, so the optimisation level and any board-specific define are the
    # ones that decide what this file contains in the shipped image.
    if(DSG_CONSUMER)
        target_include_directories(${DSG_NAME} PRIVATE
            $<TARGET_PROPERTY:${DSG_CONSUMER},INCLUDE_DIRECTORIES>)
        target_compile_definitions(${DSG_NAME} PRIVATE
            $<TARGET_PROPERTY:${DSG_CONSUMER},COMPILE_DEFINITIONS>)
        target_compile_options(${DSG_NAME} PRIVATE
            $<TARGET_PROPERTY:${DSG_CONSUMER},COMPILE_OPTIONS>)
    endif()
    # The one override: an LTO object is bytecode with no sections to read.
    #
    # [!] NOT "last so it wins" -- per-source options come after this, as Grove's
    # -O3 does.  A per-source -flto would therefore land after it and turn the
    # audit object back into bytecode.  That is caught today, but by accident: a
    # slim LTO object carries __gnu_lto_slim, which the checker's COMMON pass
    # reports.  Written down because relying on an accident is fine only while
    # somebody knows it is one.
    target_compile_options(${DSG_NAME} PRIVATE -fno-lto)

    add_custom_target(${DSG_NAME}_check
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/cmake/check_no_mutable_storage.py"
                --objdump "${CMAKE_OBJDUMP}" --nm "${CMAKE_NM}"
                --label "${DSG_SOURCE} (${BOARD})"
                $<TARGET_OBJECTS:${DSG_NAME}>
        COMMENT "check the shared decoder owns no mutable storage"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_dependencies(${DSG_NAME}_check ${DSG_NAME})
endfunction()
