# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Derive the stateless-service audit from the build, not a second list of TUs.
# The independently generated target inventory witnesses compile DB completeness:
# target-local export OFF and unity compilation can otherwise silently omit TUs.
# Plugin custom commands retain their separate actual-object audit in add_plugin.

function(_storage_collect_targets directory result)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        _storage_collect_targets("${_child}" _child_targets)
        list(APPEND _targets ${_child_targets})
    endforeach()
    set(${result} "${_targets}" PARENT_SCOPE)
endfunction()

function(add_shared_storage_build_gate)
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja" OR CMAKE_CONFIGURATION_TYPES)
        message(FATAL_ERROR "shared storage audit requires single-config Ninja")
    endif()
    if(NOT CMAKE_EXPORT_COMPILE_COMMANDS OR CMAKE_UNITY_BUILD)
        message(FATAL_ERROR "shared storage audit requires compile command export and no unity build")
    endif()
    _storage_collect_targets("${CMAKE_SOURCE_DIR}" _targets)
    set(_inventory "${CMAKE_BINARY_DIR}/shared-storage/inventory")
    set(_index "")
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(NOT _type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        get_target_property(_imported ${_target} IMPORTED)
        if(_imported)
            continue()
        endif()
        get_target_property(_export ${_target} EXPORT_COMPILE_COMMANDS)
        get_target_property(_unity ${_target} UNITY_BUILD)
        if(NOT _export OR _unity)
            message(FATAL_ERROR "shared storage audit: ${_target} must export compile commands and must not use unity builds")
        endif()
        get_target_property(_source_dir ${_target} SOURCE_DIR)
        get_target_property(_binary_dir ${_target} BINARY_DIR)
        get_target_property(_sources ${_target} SOURCES)
        set(_headers "")
        foreach(_source IN LISTS _sources)
            # The current project uses plain sources and TARGET_OBJECTS. Other
            # source genex are evaluated below; source-property genex are not
            # supported because they would require a second property evaluator.
            get_source_file_property(_header "${_source}" TARGET_DIRECTORY ${_target} HEADER_FILE_ONLY)
            if(_header MATCHES "\\$<")
                message(FATAL_ERROR "shared storage audit: unsupported HEADER_FILE_ONLY expression: ${_source}")
            endif()
            if(_header)
                list(APPEND _headers "${_source}")
            endif()
        endforeach()
        list(APPEND _index "${_target}.txt")
        file(GENERATE OUTPUT "${_inventory}/${_target}.txt" CONTENT
            "${_target}\n${_source_dir}\n${_binary_dir}\n[sources]\n$<JOIN:$<TARGET_GENEX_EVAL:${_target},$<TARGET_PROPERTY:${_target},SOURCES>>,\n>\n[header_only]\n$<JOIN:${_headers},\n>\n")
    endforeach()
    if(NOT _index)
        message(FATAL_ERROR "shared storage audit: no compiled targets")
    endif()
    file(GENERATE OUTPUT "${_inventory}/index.txt" CONTENT "$<JOIN:${_index},\n>\n")
    # Deliberately always run, even when the shipping ELF is up to date. There
    # are no success stamps to outlive a rejected compile. Audits are never
    # linked, and cannot overwrite ordinary object files or dependency files.
    add_custom_target(shared_storage_check
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/cmake/check_shared_storage_build.py"
            --root "${CMAKE_SOURCE_DIR}"
            --database "${CMAKE_BINARY_DIR}/compile_commands.json"
            --inventory "${_inventory}"
            --cc "${CMAKE_C_COMPILER}"
            --objdump "${CMAKE_OBJDUMP}" --nm "${CMAKE_NM}"
            --report "${CMAKE_BINARY_DIR}/shared-storage/last-audit.json"
        COMMENT "audit every compiled stateless service in its board context"
        VERBATIM)
    add_dependencies(shell shared_storage_check)
endfunction()
