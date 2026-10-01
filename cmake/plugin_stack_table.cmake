# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# ============================================================================
#  plugin_stack_table() -- a board's plugin stack allowances, stated once
#  (issue #126, Epic #122 parity P12)
#
#  What each plugin slot may ask of the stack it runs on is ONE fact with four
#  consumers: the firmware's policy (the device's loader refuses a container
#  that asks for more), the plugin image gate (add_plugin()'s --entry), the host
#  container verifier (the asset rule's --policy-stack), and the read-back below.
#  Until #126 a board wrote it out four times -- a compile definition, each
#  plugin's ENTRIES, and an asset rule's seven --policy-stack -- and nothing
#  compared them after the one-off sentinel check of issue #119.
#
#  plugin_stack_table(
#      DEFINE_ON  <target>          the compile that holds the board's
#                                   struct plugin_policy
#      ALLOWANCES <MACRO=bytes ...> each allowance, defined on DEFINE_ON as
#                                   MACRO=<bytes>u -- the names the firmware's
#                                   own slot -> allowance table already uses
#      SLOTS      <slot=MACRO ...>  every slot of the ABI, exactly once, and
#                                   the allowance it is declared against
#  )
#
#  add_plugin(), add_asset() and veneer_cost_gate() take the per-slot numbers
#  from here.  None of them accepts a number of its own: add_plugin() refuses
#  ENTRIES, and the probe check in veneer_cost_gate() refuses a board that
#  registered no table.
#
#  [!] THE FIRMWARE STILL STATES ITS OWN SLOT -> ALLOWANCE MAPPING, AND THAT IS
#  WHAT THE READ-BACK COMPARES WITH.  Grove's port/npu/nn_plugin_stack.h and
#  wio's nn_svc_wio.c map each slot to one of the macros defined here, next to
#  the asserts that hold every allowance under the threads its slot runs on.
#  cmake/check_policy_probe.py reads the seven numbers the shipped policy
#  actually holds and compares them with this table, slot by slot.  So it
#  catches a later -D or #define of an allowance (CMAKE_C_FLAGS wins over the
#  definition made here, with only a warning), in either direction, and a
#  firmware mapping that disagrees with SLOTS below -- but only where the two
#  allowances differ in VALUE.  Where they are equal (today every allowance on
#  both boards is 1,024 B) the device enforces exactly the numbers the host
#  checked, so the disagreement is invisible and, for as long as it stays so,
#  harmless.
#
#  [!] AND IT DOES NOT CHECK THE TABLE.  The numbers here are a board's
#  declaration; whether one still fits the thread it runs on is the firmware's
#  _Static_assert (strictly below each stack) and the board README's measured
#  depths.  Changing a number here moves every consumer together, which is the
#  point -- and also why no comparison among them can say the number is right.
# ============================================================================
include_guard(GLOBAL)

# The ABI's slot names, in no particular order (the order is the header's and
# reaches each consumer by NAME).  A transcription, but every consumer of the
# table fails closed against it going stale: the probe check and the asset build
# each require every slot of THEIR copy of the ABI -- check_plugin_image.ABI,
# pinned to svc/plugin_abi.h by run_plugin_gate_tests.py, and the abi_layout.json
# the host compiler prints from that header -- and refuse a name they do not
# know.
set(_PLUGIN_STACK_SLOTS entry shapes_ok decode draw report param_set param_get)

function(plugin_stack_table)
    cmake_parse_arguments(T "" "DEFINE_ON" "ALLOWANCES;SLOTS" ${ARGN})
    foreach(_req DEFINE_ON ALLOWANCES SLOTS)
        if(NOT DEFINED T_${_req} OR "${T_${_req}}" STREQUAL "")
            message(FATAL_ERROR "plugin_stack_table(): ${_req} is required")
        endif()
    endforeach()
    if(T_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "plugin_stack_table(): unrecognised argument(s):\n  "
            "${T_UNPARSED_ARGUMENTS}")
    endif()
    get_property(_prev GLOBAL PROPERTY PLUGIN_STACK_TABLE)
    if(_prev)
        message(FATAL_ERROR
            "plugin_stack_table(): already registered.  One board has one "
            "policy, and a second table would be a second declaration of it.")
    endif()
    if(NOT TARGET "${T_DEFINE_ON}")
        message(FATAL_ERROR
            "plugin_stack_table(): DEFINE_ON '${T_DEFINE_ON}' is not a target")
    endif()

    set(_macros "")
    set(_defs "")
    foreach(_a IN LISTS T_ALLOWANCES)
        if(NOT _a MATCHES "^([A-Z_][A-Z0-9_]*)=([0-9]+)$")
            message(FATAL_ERROR
                "plugin_stack_table(): ALLOWANCES takes MACRO=bytes, got '${_a}'")
        endif()
        set(_m "${CMAKE_MATCH_1}")
        set(_v "${CMAKE_MATCH_2}")
        # [!] ZERO IS NOT AN ALLOWANCE.  The loader reads a limit of 0 as "this
        # board refuses the slot", so a 0 here would ship a firmware that
        # refuses every container on a slot it calls -- and a stack_limit of 0
        # is not "not measured yet" either.  A slot a board does not call is not
        # something either board has; when one does, it is spelled out, not
        # reached by an empty variable.
        if(NOT _v MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR
                "plugin_stack_table(): allowance ${_m} is '${_v}'.  It must be "
                "a positive byte count: the loader reads 0 as a refused slot.")
        endif()
        if(_m IN_LIST _macros)
            message(FATAL_ERROR
                "plugin_stack_table(): allowance ${_m} is declared twice")
        endif()
        list(APPEND _macros "${_m}")
        set(_val_${_m} "${_v}")
        set(_used_${_m} FALSE)
        list(APPEND _defs "${_m}=${_v}u")
    endforeach()

    set(_seen "")
    foreach(_s IN LISTS T_SLOTS)
        if(NOT _s MATCHES "^([a-z_]+)=([A-Z_][A-Z0-9_]*)$")
            message(FATAL_ERROR
                "plugin_stack_table(): SLOTS takes slot=MACRO, got '${_s}'")
        endif()
        set(_slot "${CMAKE_MATCH_1}")
        set(_m "${CMAKE_MATCH_2}")
        if(NOT _slot IN_LIST _PLUGIN_STACK_SLOTS)
            message(FATAL_ERROR
                "plugin_stack_table(): '${_slot}' is not a plugin slot "
                "(${_PLUGIN_STACK_SLOTS})")
        endif()
        if(_slot IN_LIST _seen)
            message(FATAL_ERROR
                "plugin_stack_table(): slot ${_slot} is declared twice")
        endif()
        if(NOT _m IN_LIST _macros)
            message(FATAL_ERROR
                "plugin_stack_table(): slot ${_slot} is declared against "
                "${_m}, which ALLOWANCES does not declare")
        endif()
        list(APPEND _seen "${_slot}")
        set(_from_${_slot} "${_m}")
        set(_used_${_m} TRUE)
    endforeach()
    # Every slot, because a missing row is a slot the firmware's policy still
    # holds a number for -- one nothing on the host side would then check.
    foreach(_slot IN LISTS _PLUGIN_STACK_SLOTS)
        if(NOT _slot IN_LIST _seen)
            message(FATAL_ERROR
                "plugin_stack_table(): slot ${_slot} has no row.  Every slot "
                "of the ABI is declared, once.")
        endif()
    endforeach()
    # And every allowance is some slot's: one no slot takes would still be
    # defined into the firmware, where its mapping can name it, while the table
    # says nothing runs against it.
    foreach(_m IN LISTS _macros)
        if(NOT _used_${_m})
            message(FATAL_ERROR
                "plugin_stack_table(): allowance ${_m} is declared against no "
                "slot")
        endif()
    endforeach()

    set(_limits "")
    set(_rows "")
    foreach(_slot IN LISTS _PLUGIN_STACK_SLOTS)
        set(_m "${_from_${_slot}}")
        list(APPEND _limits "${_slot}=${_val_${_m}}")
        list(APPEND _rows "${_slot}=${_m}")
    endforeach()
    set_property(GLOBAL PROPERTY PLUGIN_STACK_TABLE "${_limits}")
    set_property(GLOBAL PROPERTY PLUGIN_STACK_TABLE_ROWS "${_rows}")
    # The firmware's half: the allowances, by the names its own mapping uses.
    target_compile_definitions("${T_DEFINE_ON}" PRIVATE ${_defs})
endfunction()

# plugin_stack_table_limits(<var> <who>) -- the table as <slot>=<bytes>, one per
# slot.  Refuses, naming <who>, on a board that registered none: an empty list
# here would reach a gate as "no limits", which is not a policy.
function(plugin_stack_table_limits _var _who)
    get_property(_limits GLOBAL PROPERTY PLUGIN_STACK_TABLE)
    if(NOT _limits)
        message(FATAL_ERROR
            "${_who}: no plugin_stack_table() registered.  The per-slot stack "
            "allowances are the board's one declaration "
            "(cmake/plugin_stack_table.cmake); nothing takes them from "
            "anywhere else.")
    endif()
    set(${_var} "${_limits}" PARENT_SCOPE)
endfunction()
