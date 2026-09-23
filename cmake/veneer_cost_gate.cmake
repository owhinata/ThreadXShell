# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# ============================================================================
#  veneer_cost_gate() -- the firmware side of the plugin stack budget (#112)
#
#  A plugin's stack declaration charges VENEER_BASE_COST at every crossing into
#  the base, for the stack the FIRMWARE spends below a veneer.  That number is
#  the board's.  This makes the build check it against the image it ships:
#  cmake/check_veneer_base_cost.py derives the deepest chain below each veneer
#  from the linked firmware and fails unless the declaration covers it, and the
#  success of that check is a build artifact -- a stamp -- that the firmware's
#  delivery targets and every container's pack depend on.  Nothing that has not
#  passed it leaves the build.
#
#  veneer_cost_gate(
#      FIRMWARE       <target>          the app firmware target, as it ships
#      MAP            <path>            that link's map
#      DECLARED       <bytes>           the board's VENEER_BASE_COST -- the SAME
#                                       variable every add_plugin() is given
#      ROOTS          <veneer=func ...> the firmware function behind each veneer
#                                       of the plugin ABI; one per veneer
#      PREBUILT_ROOTS <dir ...>         archives under these are the toolchain's
#                                       or a vendor's and need no record
#      DELIVERY       <target ...>      the targets that put this firmware on
#                                       the device; each waits for the check
#  )
#
#  [!] THE BOARD STATES FACTS; THE HELPER DERIVES THE REST.  What the board
#  passes is what only the board knows: which function it binds to each veneer,
#  what it declares, where its vendor's archives live, what flashes it.  What
#  gets -fstack-usage is NOT passed: the helper walks the firmware's own link
#  inputs -- $<TARGET_OBJECTS:> sources and linked OBJECT / STATIC libraries,
#  through interface libraries -- and instruments every compile it finds.  A
#  board-maintained list of TUs would be a second inventory, free to miss the
#  one the chain goes through; the check then fails for a missing record, which
#  is safe, but the list would be the thing to keep right.  Anything outside
#  the firmware's link inputs (wio's boot reference build, a test binary) is
#  never touched.
#
#  [!] THE REGISTRATION IS A TARGET AND A STAMP, NOT A FLAG.  add_plugin()
#  refuses to configure on a board that has not called this: a plugin whose
#  charge nothing checks is exactly the state #112 exists to end.
#
#  [!] AND DECLARED IS ALSO WHAT THE FIRMWARE ADDS (issue #111).  A manifest no
#  longer carries the veneer cost; the device's loader adds its own.  So the
#  number checked here is handed to the firmware by THIS helper, as the compile
#  definition PLUGIN_VENEER_BASE_COST on every compile of the image (the same
#  set that gets -fstack-usage), and to the host container verifier through
#  veneer_cost_gate_declared().  A board that restated it in a variable of its
#  own could check one number and load with another.  And a -D is not a
#  guarantee either -- a later -D or #define of the same name overrides it with
#  a warning -- so the stamp also waits for cmake/check_policy_probe.py, which
#  reads the veneer_cost the board's policy ACTUALLY HOLDS out of the shipped
#  image (PLUGIN_POLICY_PROBE, svc/plugin_load.h) and refuses anything but the
#  number that was checked.
# ============================================================================

# Script mode: what a LINK invalidates, removed before every link.
#
# [!] THE STAMP IS A CLAIM ABOUT ONE IMAGE, SO THE LINK THAT REPLACES THAT IMAGE
# RETRACTS IT.  Without this, building the image alone (`ninja shell.img`, or
# any target that needs the ELF but not the check) relinks and leaves the
# previous run's passing stamp behind, newer-looking than it is: the delivery
# targets still wait for the check, but the stamp itself goes on asserting that
# the firmware now on disk passed.  A stamp that can outlive what it describes
# is not evidence.  Removing it here means it can only ever mean "the image as
# it is now was checked".
#
# [!] AND ONLY THIS TARGET'S LTO RECORDS, ALSO BEFORE EVERY LINK.  A link that
# makes fewer partitions than the last one would otherwise leave the old ones
# behind, and the check refuses a record the map does not load.
if(CMAKE_SCRIPT_MODE_FILE AND (DEFINED VENEER_GATE_CLEAN_LTRANS
                               OR DEFINED VENEER_GATE_CLEAN_STAMP))
    if(DEFINED VENEER_GATE_CLEAN_STAMP)
        file(REMOVE "${VENEER_GATE_CLEAN_STAMP}")
    endif()
    if(DEFINED VENEER_GATE_CLEAN_LTRANS)
        get_filename_component(_dir "${VENEER_GATE_CLEAN_LTRANS}" DIRECTORY)
        get_filename_component(_base "${VENEER_GATE_CLEAN_LTRANS}" NAME)
        file(GLOB _old "${_dir}/${_base}.ltrans*.ltrans.su")
        if(_old)
            file(REMOVE ${_old})
        endif()
    endif()
    return()
endif()

set(_VENEER_GATE_SELF "${CMAKE_CURRENT_LIST_FILE}")
set(_VENEER_GATE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(veneer_cost_gate)
    cmake_parse_arguments(G "" "FIRMWARE;MAP;DECLARED"
                          "ROOTS;PREBUILT_ROOTS;DELIVERY" ${ARGN})
    foreach(_req FIRMWARE MAP DECLARED ROOTS PREBUILT_ROOTS DELIVERY)
        if(NOT DEFINED G_${_req} OR "${G_${_req}}" STREQUAL "")
            message(FATAL_ERROR "veneer_cost_gate(): ${_req} is required")
        endif()
    endforeach()
    if(G_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "veneer_cost_gate(): unrecognised argument(s):\n  "
            "${G_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT G_DECLARED MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "veneer_cost_gate(): DECLARED must be a positive byte count, got "
            "'${G_DECLARED}'.  Zero is not a cost.")
    endif()
    foreach(_r IN LISTS G_ROOTS)
        if(NOT _r MATCHES "^[A-Za-z_][A-Za-z0-9_]*=[A-Za-z_][A-Za-z0-9_.]*$")
            message(FATAL_ERROR
                "veneer_cost_gate(): ROOTS takes VENEER=FUNCTION, got '${_r}'")
        endif()
    endforeach()
    if(NOT TARGET "${G_FIRMWARE}")
        message(FATAL_ERROR
            "veneer_cost_gate(): FIRMWARE '${G_FIRMWARE}' is not a target")
    endif()
    get_target_property(_type "${G_FIRMWARE}" TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR
            "veneer_cost_gate(): FIRMWARE '${G_FIRMWARE}' is a ${_type}, not "
            "the executable that ships")
    endif()
    get_property(_prev GLOBAL PROPERTY VENEER_GATE_FIRMWARE)
    if(_prev)
        message(FATAL_ERROR
            "veneer_cost_gate(): already registered for '${_prev}'.  One board "
            "ships one firmware; a second registration would be a second "
            "declaration of the same cost.")
    endif()

    set(_stamp "${CMAKE_BINARY_DIR}/veneer_cost/${G_FIRMWARE}.checked")
    set_property(GLOBAL PROPERTY VENEER_GATE_FIRMWARE "${G_FIRMWARE}")
    set_property(GLOBAL PROPERTY VENEER_GATE_TARGET "veneer_cost_check")
    set_property(GLOBAL PROPERTY VENEER_GATE_STAMP "${_stamp}")
    set_property(GLOBAL PROPERTY VENEER_GATE_MAP "${G_MAP}")
    set_property(GLOBAL PROPERTY VENEER_GATE_DECLARED "${G_DECLARED}")
    set_property(GLOBAL PROPERTY VENEER_GATE_ROOTS "${G_ROOTS}")
    set_property(GLOBAL PROPERTY VENEER_GATE_PREBUILT "${G_PREBUILT_ROOTS}")
    set_property(GLOBAL PROPERTY VENEER_GATE_DELIVERY "${G_DELIVERY}")

    # The target exists from here on, so add_plugin() and a board's asset rules
    # can name it; the command that produces the stamp is added when configure
    # ends, once every add_plugin() has recorded what it was given.
    add_custom_target(veneer_cost_check ALL DEPENDS "${_stamp}")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
                   CALL _veneer_cost_gate_finish)
endfunction()

# veneer_cost_gate_declared(<var>) -- the DECLARED this board registered.
#
# For the asset rules, which must hand the host container verifier the same c
# the firmware adds at load time.  Refuses to answer on a board that has not
# registered, rather than handing back an empty string.
function(veneer_cost_gate_declared _var)
    get_property(_declared GLOBAL PROPERTY VENEER_GATE_DECLARED)
    if(NOT _declared)
        message(FATAL_ERROR
            "veneer_cost_gate_declared(): no veneer_cost_gate() registered")
    endif()
    set(${_var} "${_declared}" PARENT_SCOPE)
endfunction()

# Every non-imported target that compiles objects into @p fw's image: fw itself,
# the object libraries its SOURCES take by $<TARGET_OBJECTS:>, and the OBJECT /
# STATIC libraries it links, transitively and through interface libraries.
function(_veneer_cost_gate_compiles fw result)
    set(_todo "${fw}")
    set(_seen "")
    set(_compile "")
    while(_todo)
        list(POP_FRONT _todo _t)
        if(_t IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_t}")
        get_target_property(_imported "${_t}" IMPORTED)
        if(_imported)
            continue()                  # prebuilt: nothing here compiles it
        endif()
        get_target_property(_type "${_t}" TYPE)
        if(_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$")
            list(APPEND _compile "${_t}")
            get_target_property(_srcs "${_t}" SOURCES)
            set(_links_prop LINK_LIBRARIES)
        elseif(_type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_srcs "${_t}" INTERFACE_SOURCES)
            set(_links_prop INTERFACE_LINK_LIBRARIES)
        else()
            message(FATAL_ERROR
                "veneer_cost_gate(): '${_t}' (${_type}) is linked into ${fw}, "
                "and this helper does not know how that kind of target "
                "contributes code")
        endif()
        foreach(_s IN LISTS _srcs)
            string(REGEX MATCHALL "\\$<TARGET_OBJECTS:[^>]+>" _objs "${_s}")
            foreach(_o IN LISTS _objs)
                string(REGEX REPLACE "^\\$<TARGET_OBJECTS:([^>]+)>$" "\\1"
                       _name "${_o}")
                list(APPEND _todo "${_name}")
            endforeach()
        endforeach()
        foreach(_p ${_links_prop} INTERFACE_LINK_LIBRARIES)
            get_target_property(_libs "${_t}" ${_p})
            foreach(_l IN LISTS _libs)
                if(_l MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                    set(_l "${CMAKE_MATCH_1}")
                endif()
                if(_l MATCHES "\\$<")
                    # [!] A generator expression here could name a target this
                    # walk would then miss.  None does today; refuse rather than
                    # guess at what it evaluates to.
                    message(FATAL_ERROR
                        "veneer_cost_gate(): '${_t}' links '${_l}', and a "
                        "generator expression in a link list cannot be walked "
                        "here")
                endif()
                if(TARGET "${_l}")
                    list(APPEND _todo "${_l}")
                endif()
            endforeach()
        endforeach()
    endwhile()
    set(${result} "${_compile}" PARENT_SCOPE)
endfunction()

function(_veneer_cost_gate_targets directory result)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        _veneer_cost_gate_targets("${_child}" _sub)
        list(APPEND _targets ${_sub})
    endforeach()
    set(${result} "${_targets}" PARENT_SCOPE)
endfunction()

function(_veneer_cost_gate_finish)
    get_property(fw GLOBAL PROPERTY VENEER_GATE_FIRMWARE)
    get_property(gate GLOBAL PROPERTY VENEER_GATE_TARGET)
    get_property(stamp GLOBAL PROPERTY VENEER_GATE_STAMP)
    get_property(map GLOBAL PROPERTY VENEER_GATE_MAP)
    get_property(declared GLOBAL PROPERTY VENEER_GATE_DECLARED)
    get_property(roots GLOBAL PROPERTY VENEER_GATE_ROOTS)
    get_property(prebuilt GLOBAL PROPERTY VENEER_GATE_PREBUILT)
    get_property(delivery GLOBAL PROPERTY VENEER_GATE_DELIVERY)
    get_property(costs GLOBAL PROPERTY VENEER_GATE_PLUGIN_COSTS)
    get_property(printers GLOBAL PROPERTY VENEER_GATE_PRINTER_LIMITS)

    # [!] ONE DECLARATION: what is checked here is what every plugin was charged
    # with.  Two values that merely start out equal would let a plugin be packed
    # against a charge nothing verified.
    foreach(_c IN LISTS costs)
        string(REPLACE "=" ";" _kv "${_c}")
        list(GET _kv 0 _plugin)
        list(GET _kv 1 _cost)
        if(NOT _cost STREQUAL declared)
            message(FATAL_ERROR
                "veneer_cost_gate(): plugin ${_plugin} is charged "
                "VENEER_BASE_COST ${_cost}, but the firmware is checked "
                "against ${declared}.  They are one fact and must be one "
                "value.")
        endif()
    endforeach()

    # The witnesses: every compile that feeds the image, and the LTO link.
    _veneer_cost_gate_compiles("${fw}" _compiles)
    foreach(_t IN LISTS _compiles)
        target_compile_options("${_t}" PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:-fstack-usage>)
        # [!] THE CHARGE THE LOADER ADDS (issue #111), onto the same compiles:
        # whichever TU holds the board's plugin_policy, it is one of these.  A
        # board does not pass it -- see the header of this file.
        target_compile_definitions("${_t}" PRIVATE
            PLUGIN_VENEER_BASE_COST=${declared}u)
    endforeach()
    target_link_options("${fw}" PRIVATE -fstack-usage)
    # [!] THE PROBE IS A LINK ROOT (issue #111).  __attribute__((used)) keeps it
    # from the compiler, not from --gc-sections, and nothing in the image reads
    # it -- so without this both boards linked it away and the check below
    # (rightly) refused.  --require-defined also makes a board that exports no
    # policy fail at the link, naming the symbol, and marks it referenced for
    # the LTO plugin.
    target_link_options("${fw}" PRIVATE
        "-Wl,--require-defined=plugin_policy_probe")
    add_custom_command(TARGET "${fw}" PRE_LINK
        COMMAND "${CMAKE_COMMAND}"
                "-DVENEER_GATE_CLEAN_LTRANS=$<TARGET_FILE:${fw}>"
                "-DVENEER_GATE_CLEAN_STAMP=${stamp}"
                -P "${_VENEER_GATE_SELF}"
        VERBATIM)

    set(_args "")
    foreach(_r IN LISTS roots)
        list(APPEND _args "--root=${_r}")
    endforeach()
    foreach(_p IN LISTS prebuilt)
        list(APPEND _args --prebuilt-root "${_p}")
    endforeach()
    foreach(_p IN LISTS printers)
        list(APPEND _args --printer-limit "${_p}")
    endforeach()

    # [!] THE STAMP IS DELETED BEFORE THE CHECK and written only after it
    # passes, so a failed check leaves no stamp for a later build to trust.
    # The check re-runs whenever the image, either script (the veneer set lives
    # in check_plugin_image.py) or its command line -- roots, declaration,
    # printer limits -- changes.  The witness records are not listed: each is
    # written by the same compile or link that rewrites the image.
    get_filename_component(_stamp_dir "${stamp}" DIRECTORY)
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stamp_dir}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
        COMMAND "${Python3_EXECUTABLE}"
                "${_VENEER_GATE_DIR}/check_veneer_base_cost.py"
                "$<TARGET_FILE:${fw}>"
                --map "${map}"
                --link-dir "${CMAKE_BINARY_DIR}"
                --declared "${declared}"
                --ltrans-prefix "$<TARGET_FILE:${fw}>"
                ${_args}
        # [!] AND WHAT THE FIRMWARE WILL ACTUALLY CHARGE (issue #111): the -D
        # above can be overridden, so the value is read back from the image.
        COMMAND "${Python3_EXECUTABLE}"
                "${_VENEER_GATE_DIR}/check_policy_probe.py"
                "$<TARGET_FILE:${fw}>" --declared "${declared}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${fw}"
                "${_VENEER_GATE_DIR}/check_veneer_base_cost.py"
                "${_VENEER_GATE_DIR}/check_plugin_image.py"
                "${_VENEER_GATE_DIR}/check_policy_probe.py"
                "${_VENEER_GATE_SELF}"
        COMMENT "check_veneer_base_cost.py (VENEER_BASE_COST ${declared} B against the stack below each veneer of ${fw})"
        VERBATIM)

    # Delivery: nothing puts this firmware on the device before it passes.
    foreach(_d IN LISTS delivery)
        if(NOT TARGET "${_d}")
            message(FATAL_ERROR
                "veneer_cost_gate(): DELIVERY '${_d}' is not a target")
        endif()
        add_dependencies("${_d}" "${gate}")
    endforeach()
    # And no container is packed before it passes: every asset-* waits for it,
    # found by name so that a new asset cannot be added past it.
    _veneer_cost_gate_targets("${CMAKE_SOURCE_DIR}" _all)
    foreach(_t IN LISTS _all)
        if(_t MATCHES "^asset-")
            add_dependencies("${_t}" "${gate}")
        endif()
    endforeach()
endfunction()
