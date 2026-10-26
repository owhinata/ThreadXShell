# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# ============================================================================
#  asset_model_source() / add_asset() -- one sendable container (issue #126)
#
#  `--target asset-<name>` produces build/<board>/asset/<name>.nnc: a container
#  that has been through every gate, published only after passing
#  (cmake/build_asset.py), and a receipt.  Until #126 each board carried its own
#  copy of this rule (grove_add_asset / wio_add_asset), near-identical, and each
#  wrote the plugin stack policy out again as seven --policy-stack lines.
#
#  asset_model_source(<name>
#      OVERRIDE_VAR <cache var>  the operator's own copy instead of the pin
#      OUT_VAR      <var>        receives the path of the model to ingest,
#                                and <var>_SHA256 the hash its first consumer
#                                must check ("" = not a pin, nothing to check)
#      FILE <path> | URL <git> COMMIT <sha> PATH_IN <p> SHA256 <hash>)
#
#  [!] THE PIN IS CHECKED TWICE, FROM ONE VALUE (issue #132).  The fetch checks
#  the file it publishes; the FIRST CONSUMER of that file checks the bytes it
#  reads, inside its own command -- because once the file exists, ninja reruns
#  only the consumer if it changes, and an edit after the fetch would otherwise
#  flow straight into a container.  The consumer is the packer when the model is
#  packed as fetched (MODEL_SHA256 below), or a board ingest step behind
#  cmake/run_with_pinned_input.py.  FILE is a path, not a hash pin (it is
#  guarded by whatever pins the tree it lives in), and an override is not
#  hash-checked: both give "".
#
#  add_asset(<name>
#      MODEL   <path>            the model as packed, after the board's ingest
#      MODEL_SHA256 <hash>       REQUIRED, but may be empty: the pin the packer
#                                re-checks on the bytes it packs, i.e.
#                                asset_model_source()'s <var>_SHA256 when MODEL
#                                is that source unchanged; "" when MODEL is an
#                                override, a FILE, or an ingest's output (the
#                                ingest's first step checked the pin)
#      PLUGIN  <name>            a plugin this board built with add_plugin()
#      SLOT    <n>               checked for capacity, printed on the receipt
#      PACKER LAYOUT CONTAINER_VERIFIER SLOT_TABLE <path>
#      MODEL_VERIFIER <path>     REQUIRED, but may be empty: a board with no
#                                host C++ compiler passes "", and build_asset.py
#                                refuses at build time rather than publishing
#                                an unchecked model -- the firmware still
#                                configures
#      [VERIFY_ARGS <arg ...>]   the board's model checks (Grove: --blazeface)
#      BUILD_ID TARGET_ID LINK_ADDR CAPACITY <value>
#      RECEIPT_STEPS <text ...>  [RECEIPT_THEN <text ...>]
#  )
#
#  [!] WHAT IS NOT AN ARGUMENT IS THE POINT.  The stack policy is the board's
#  plugin_stack_table(), the veneer cost is veneer_cost_gate()'s DECLARED, and
#  the plugin's image and bounds are where add_plugin() put them -- each looked
#  up from the registration it belongs to, so an asset cannot be packed against
#  numbers the firmware does not hold.  What a board passes is what only the
#  board knows: its tools, its target word and reservation, its model checks,
#  and the commands its receipt prints.  The ingest between the two calls (strip
#  and vela on Grove, nothing on wio) is the board's own.
#
#  [!] THE GATES RUN AT BUILD TIME, NOT AT SEND TIME, AND NEITHER TARGET IS IN
#  ALL.  The send is picocom's `sb -k` of whatever path is typed: the receipt's
#  CRC, compared with `blob list` afterwards, is the only "built bytes = stored
#  bytes" check.  And a tree with no access to the model host must still build
#  the firmware (issue #94's lesson), so neither the fetch nor the asset is a
#  dependency of anything in ALL.
# ============================================================================
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/plugin_stack_table.cmake")
set(_ADD_ASSET_DIR "${CMAKE_CURRENT_LIST_DIR}")
# The wrapper a board puts in front of an ingest tool that is the pin's first
# consumer (issue #132); see asset_model_source() above.
set(ASSET_RUN_PINNED "${CMAKE_CURRENT_LIST_DIR}/run_with_pinned_input.py")

function(asset_model_source _name)
    cmake_parse_arguments(A "" "OVERRIDE_VAR;OUT_VAR;FILE;URL;COMMIT;PATH_IN;SHA256"
                          "" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "asset_model_source(${_name}): unrecognised: ${A_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_req OVERRIDE_VAR OUT_VAR)
        if(NOT DEFINED A_${_req} OR "${A_${_req}}" STREQUAL "")
            message(FATAL_ERROR "asset_model_source(${_name}): ${_req} is required")
        endif()
    endforeach()
    # One source: a committed file, or a pin that names all four of its parts.
    # A pin with a part missing would reach fetch_model.cmake as an empty -D.
    if(A_FILE)
        foreach(_k URL COMMIT PATH_IN SHA256)
            if(DEFINED A_${_k})
                message(FATAL_ERROR
                    "asset_model_source(${_name}): FILE and ${_k} both given")
            endif()
        endforeach()
    else()
        foreach(_k URL COMMIT PATH_IN SHA256)
            if(NOT DEFINED A_${_k} OR "${A_${_k}}" STREQUAL "")
                message(FATAL_ERROR
                    "asset_model_source(${_name}): no FILE, and the pin has no "
                    "${_k}")
            endif()
        endforeach()
    endif()

    set(_model_dir "${CMAKE_BINARY_DIR}/model/${_name}")
    # An operator's own copy instead of the pin.  NOT hash-checked: an override
    # deliberately supplies different content, and checking it against the
    # upstream pin would make the escape hatch unusable.  The board's ingest,
    # the pack, the gates and the device's validator stand behind it.
    set(${A_OVERRIDE_VAR} "" CACHE FILEPATH
        "Local model for asset '${_name}' instead of the pinned upstream one")
    if(${A_OVERRIDE_VAR})
        set(_src "${${A_OVERRIDE_VAR}}")
        set(_sha "")
        message(STATUS "asset ${_name}: model OVERRIDDEN by ${_src} "
                       "(not the pin, not hash-checked)")
    elseif(A_FILE)
        set(_src "${A_FILE}")
        set(_sha "")
    else()
        set(_sha "${A_SHA256}")
        set(_src "${_model_dir}/fetched.tflite")
        add_custom_command(
            OUTPUT "${_src}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DURL=${A_URL}" "-DCOMMIT=${A_COMMIT}"
                    "-DPATH_IN=${A_PATH_IN}" "-DSHA256=${A_SHA256}"
                    "-DOUT=${_src}" "-DWORK=${_model_dir}/fetch-work"
                    "-DOVERRIDE=${A_OVERRIDE_VAR}"
                    -P "${_ADD_ASSET_DIR}/fetch_model.cmake"
            DEPENDS "${_ADD_ASSET_DIR}/fetch_model.cmake"
            COMMENT "asset ${_name}: fetch the pinned model"
            VERBATIM)
    endif()
    # [!] THE HASH TRAVELS AS A VARIABLE DERIVED FROM OUT_VAR, never as a second
    # literal: the SHA256 argument above is the one source for both the fetch
    # and the consumer's check, and a board passes ${<var>_SHA256} on.  The
    # fetched path is also recorded, so add_asset() can refuse a board that
    # packs the pinned file and passes MODEL_SHA256 "" -- the one way to drop
    # the check by mistake.
    set(${A_OUT_VAR} "${_src}" PARENT_SCOPE)
    set(${A_OUT_VAR}_SHA256 "${_sha}" PARENT_SCOPE)
    if(_sha)
        # Keyed by the REALPATH (relative = from where the pack command runs),
        # so a spelling difference (./, .., relative) cannot drop this net.
        get_filename_component(_key "${_src}" REALPATH
                               BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        set_property(GLOBAL PROPERTY "ASSET_MODEL_PIN:${_key}" "${_sha}")
    endif()
endfunction()

function(add_asset _name)
    cmake_parse_arguments(A ""
        "MODEL;MODEL_SHA256;PLUGIN;SLOT;PACKER;LAYOUT;CONTAINER_VERIFIER;SLOT_TABLE;MODEL_VERIFIER;BUILD_ID;TARGET_ID;LINK_ADDR;CAPACITY"
        "VERIFY_ARGS;RECEIPT_STEPS;RECEIPT_THEN" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "add_asset(${_name}): unrecognised: ${A_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_req MODEL PLUGIN SLOT PACKER LAYOUT CONTAINER_VERIFIER SLOT_TABLE
                 BUILD_ID TARGET_ID LINK_ADDR CAPACITY RECEIPT_STEPS)
        if(NOT DEFINED A_${_req} OR "${A_${_req}}" STREQUAL "")
            message(FATAL_ERROR "add_asset(${_name}): ${_req} is required")
        endif()
    endforeach()
    # [!] MODEL_VERIFIER MUST BE PRESENT, EVEN WHEN IT IS EMPTY.  Empty is a
    # board saying "no host C++ compiler, so no model gate", which build_asset.py
    # turns into a refusal at build time.  Absent would be a board that forgot
    # it -- and the two would otherwise look the same.
    if(NOT DEFINED A_MODEL_VERIFIER
       AND NOT "MODEL_VERIFIER" IN_LIST A_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR
            "add_asset(${_name}): MODEL_VERIFIER is required (pass \"\" when "
            "this build has no model gate; the asset then refuses to build)")
    endif()
    # [!] AND SO MUST MODEL_SHA256 (issue #132), for the same reason: empty is
    # "this model is not the pin as fetched", absent is a board that forgot.
    if(NOT DEFINED A_MODEL_SHA256
       AND NOT "MODEL_SHA256" IN_LIST A_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR
            "add_asset(${_name}): MODEL_SHA256 is required (pass "
            "asset_model_source()'s <var>_SHA256, or \"\" when MODEL is not the "
            "pinned file as fetched)")
    endif()
    # The same normalisation as the record in asset_model_source().
    get_filename_component(_key "${A_MODEL}" REALPATH
                           BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_property(_pin GLOBAL PROPERTY "ASSET_MODEL_PIN:${_key}")
    if(_pin AND NOT "${A_MODEL_SHA256}" STREQUAL "${_pin}")
        message(FATAL_ERROR
            "add_asset(${_name}): MODEL is the pinned fetch ${A_MODEL}, so "
            "MODEL_SHA256 must be its pin (asset_model_source()'s "
            "<var>_SHA256), got '${A_MODEL_SHA256}'")
    endif()
    if(NOT A_SLOT MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "add_asset(${_name}): SLOT must be a slot number, got '${A_SLOT}'")
    endif()
    if(NOT A_PLUGIN MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
        message(FATAL_ERROR "add_asset(${_name}): bad PLUGIN '${A_PLUGIN}'")
    endif()
    get_property(_plugin_dir GLOBAL PROPERTY ADD_PLUGIN_OUT_${A_PLUGIN})
    if(NOT _plugin_dir)
        message(FATAL_ERROR
            "add_asset(${_name}): no add_plugin(${A_PLUGIN}) on this board")
    endif()
    get_property(_veneer_gate GLOBAL PROPERTY VENEER_GATE_TARGET)
    if(NOT _veneer_gate)
        message(FATAL_ERROR
            "add_asset(${_name}): no veneer_cost_gate() registered")
    endif()
    # The c the firmware adds at load time (issue #111), from the helper that
    # checked it and compiled it in -- not a board variable that starts out
    # equal.
    veneer_cost_gate_declared(_veneer_cost)
    # [!] AND THE STACK POLICY FROM THE BOARD'S TABLE (issue #126): the same
    # registration the plugin gate's --entry and the firmware's policy come
    # from, and the one the firmware's policy is read back and compared with.
    plugin_stack_table_limits(_limits "add_asset(${_name})")
    set(_policy "")
    foreach(_l IN LISTS _limits)
        list(APPEND _policy --policy-stack "${_l}")
    endforeach()

    set(_nnc "${CMAKE_BINARY_DIR}/asset/${_name}.nnc")
    add_custom_command(
        OUTPUT "${_nnc}"
        COMMAND "${CMAKE_COMMAND}" -E env
                "ASSET_NM=${CMAKE_NM}" "ASSET_OBJCOPY=${CMAKE_OBJCOPY}"
                "${Python3_EXECUTABLE}" "${_ADD_ASSET_DIR}/build_asset.py"
                --name "${_name}" --model "${A_MODEL}"
                # One token with "=" so an empty value survives as "".
                "--model-sha256=${A_MODEL_SHA256}"
                --plugin-elf "${_plugin_dir}/plugin.elf"
                --plugin-stacks "${_plugin_dir}/plugin.stacks.json"
                --packer "${A_PACKER}" --layout "${A_LAYOUT}"
                --model-verifier "${A_MODEL_VERIFIER}"
                --container-verifier "${A_CONTAINER_VERIFIER}"
                "--verify-args=${A_VERIFY_ARGS}"
                --build-id "${A_BUILD_ID}"
                --target-id "${A_TARGET_ID}"
                --link-addr "${A_LINK_ADDR}"
                --capacity "${A_CAPACITY}"
                ${_policy}
                --veneer-cost "${_veneer_cost}"
                # The declaration names a slot, and the packed size is known
                # here, so the fit is checked before the device would erase
                # that slot to discover it.
                --slot "${A_SLOT}" --slot-table "${A_SLOT_TABLE}"
                --out "${_nnc}"
        DEPENDS "${A_MODEL}" "${_plugin_dir}/plugin.elf"
                "${_plugin_dir}/plugin.stacks.json"
                "${A_PACKER}" "${A_LAYOUT}"
                "${A_CONTAINER_VERIFIER}" "${A_SLOT_TABLE}"
                "${_ADD_ASSET_DIR}/build_asset.py"
                ${A_MODEL_VERIFIER}
                # [!] No container is packed before the firmware passes its
                # veneer-cost check and policy read-back (issues #112, #126)
                # -- even built by path.
                ${_veneer_gate}
        COMMENT "asset ${_name}: pack, verify what was packed, publish"
        VERBATIM)

    # [!] THE RECEIPT PRINTS FROM A PHONY, not from the command above -- that
    # one does not rerun once its output is current, so the number an operator
    # needs would appear exactly once and never again.  The commands it prints
    # are the board's own: how a slot is written differs per board.
    set(_receipt "")
    foreach(_s IN LISTS A_RECEIPT_STEPS)
        list(APPEND _receipt --step "${_s}")
    endforeach()
    foreach(_s IN LISTS A_RECEIPT_THEN)
        list(APPEND _receipt --then "${_s}")
    endforeach()
    add_custom_target(asset-${_name}
        COMMAND "${Python3_EXECUTABLE}" "${_ADD_ASSET_DIR}/asset_receipt.py"
                "${_nnc}.json" "${A_SLOT}" ${_receipt}
        DEPENDS "${_nnc}"
        VERBATIM)
endfunction()
