# ============================================================================
#  add_plugin() -- build one plugin image (issue #78 Step 1a/1b; shared in #106)
#
#  A plugin is a SECOND program: prelinked at an absolute address, linked with
#  its own script and its own flags, and it must never reach the firmware's
#  link, the image generator or the firmware's own gates.  The sources are
#  repository-wide (asset/), because they are: their include graph reaches only
#  svc/, and the one board-specific thing in them was the MEMORY block, which is
#  now the board's own fragment.
#
#  [!] THE BOARD'S FACTS ARE ARGUMENTS, NOT GLOBALS.  This helper reads no
#  GROVE_*, no BOARD_DIR and no board-selected variable; everything that differs
#  per board is passed in.  That is what makes "board-independent" checkable
#  rather than asserted -- a second board using this cannot silently inherit the
#  first one's -mcpu, address or gate.
#
#  [!] AND THE OWNED SOURCE ROOTS ARE DERIVED HERE, NEVER ACCEPTED.  See the
#  membership rule below: a root argument would let a caller pass the repository
#  root and exempt every shared file from the no-mutable-storage audit.  This
#  file knows where asset/ is because it knows where IT is.
#
#  add_plugin(<name>
#      CFLAGS       <...>        compile flags, carrying the board's -mcpu
#      ARCH_FLAGS   <-m...>      the board's architecture flags, and ONLY those
#      MEMORY_LD    <path>       the board's .plugin MEMORY fragment
#      IMAGE_BASE   <addr>       the reservation, as the GATE is told it --
#      IMAGE_END    <addr>         see below for why this is not the fragment
#      FORBIDDEN    <sym ...>    entry points a plugin may never reach here
#      VENEER_BASE_COST <bytes>  stack the base spends behind one veneer
#      TARGET_ID    <word>       the board's plugin target word, which the gate
#                                checks against what the image was built for
#      OUT_DIR      <dir>        build dir to put <name>/ under
#      OUT_VAR      <var>        list variable the plugin.elf path is appended to
#      ENTRIES      <sym=limit>  every slot the plugin exports
#      [SOURCES     <.c ...>]    the plugin's own extra sources
#      [AUDIT_SHARED <path ...>] sources from outside the owned roots
#  )
# ============================================================================

get_filename_component(_ADD_PLUGIN_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
# [!] THE GATE IS SHARED SINCE ISSUE #108, AND ITS BOARD FACTS ARE ARGUMENTS.
# Until then each board would have carried its own copy, which is the decision
# issue #106 deferred until a second board existed to show where the seam was.
# It is three facts -- the reservation, the forbidden table and the base's cost
# behind a veneer -- plus the target word the gate checks the image against
# (issue #108 too), and all four are required below, so a second board cannot
# inherit the first one's by omission.
set(_ADD_PLUGIN_GATE "${_ADD_PLUGIN_DIR}/check_plugin_image.py")
get_filename_component(_ADD_PLUGIN_ASSET_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../asset" REALPATH)

# grove_add_plugin(<name> SOURCES <extra .c ...> ENTRIES <sym=limit ...>
#                          AUDIT_SHARED <src ...>)
#
# The sources named are the plugin's OWN; asset/common's are added here
# so that a new plugin cannot forget the veneers the gate insists every indirect
# call goes through.
#
# [!] AUDIT_SHARED NAMES A FILE THAT THREE BOARDS COMPILE, and it must own no
# mutable storage (issue #97): each board passes in its own scratch so that the
# scratch keeps that board's placement and that board's residency gate keeps
# naming a symbol the board owns.  A plugin's OWN sources are not audited -- a
# plugin has .bss and is supposed to.
#
# [!] AND IT AUDITS THE REAL OBJECT, INSIDE THE RULE THAT PRODUCES IT.  Until
# issue #104 this file was in shell_objs and cmake/shared_storage_gate.cmake
# recompiled it from that target's properties.  With the firmware no longer
# building it, the compile that ships on this board is the one below -- and
# reconstructing these flags in a separate audit target would inspect an object
# no artifact contains, which is exactly the mistake that helper's header
# records twice.  Auditing the linked object in its own command makes a
# differently-compiled audited copy impossible rather than merely unlikely.
function(add_plugin _name)
    cmake_parse_arguments(P ""
        "MEMORY_LD;IMAGE_BASE;IMAGE_END;VENEER_BASE_COST;TARGET_ID;OUT_DIR;OUT_VAR"
        "CFLAGS;ARCH_FLAGS;SOURCES;ENTRIES;AUDIT_SHARED;FORBIDDEN" ${ARGN})
    # Presence only.  `if(NOT P_x)` would call a literal 0 "missing", which is
    # the wrong refusal for VENEER_BASE_COST 0 -- that one is refused below for
    # what it is.  The numbers are checked as numbers after this.
    foreach(_req MEMORY_LD IMAGE_BASE IMAGE_END VENEER_BASE_COST FORBIDDEN
                 TARGET_ID OUT_DIR OUT_VAR CFLAGS ARCH_FLAGS ENTRIES)
        if(NOT DEFINED P_${_req} OR "${P_${_req}}" STREQUAL "")
            message(FATAL_ERROR "add_plugin(${_name}): ${_req} is required")
        endif()
    endforeach()
    # The gate's reservation is a statement the board makes to the GATE, and a
    # malformed one would reach Python as a traceback in the middle of a build.
    foreach(_req IMAGE_BASE IMAGE_END TARGET_ID)
        if(NOT P_${_req} MATCHES "^0x[0-9A-Fa-f]+$")
            message(FATAL_ERROR
                "add_plugin(${_name}): ${_req} must be a hex number, got "
                "'${P_${_req}}'")
        endif()
    endforeach()
    if(NOT P_VENEER_BASE_COST MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "add_plugin(${_name}): VENEER_BASE_COST must be a positive byte "
            "count, got '${P_VENEER_BASE_COST}'.  Zero is not a cost -- it is "
            "the analysis assuming the base spends nothing behind a veneer.")
    endif()
    if(NOT EXISTS "${P_MEMORY_LD}")
        message(FATAL_ERROR
            "add_plugin(${_name}): no MEMORY fragment at ${P_MEMORY_LD}")
    endif()

    # [!] NO PLUGIN WITHOUT THE FIRMWARE-SIDE CHECK (issue #112).  The charge
    # this plugin is packed against is only true if the firmware's stack below
    # each veneer fits in it, and that is checked by the target and stamp that
    # veneer_cost_gate() registers -- a target and a stamp, not a flag a board
    # could set without having wired anything.
    get_property(_gate  GLOBAL PROPERTY VENEER_GATE_TARGET)
    get_property(_stamp GLOBAL PROPERTY VENEER_GATE_STAMP)
    if(NOT _gate OR NOT _stamp OR NOT TARGET "${_gate}")
        message(FATAL_ERROR
            "add_plugin(${_name}): this board has not registered the firmware "
            "side of the veneer cost (veneer_cost_gate() in "
            "cmake/veneer_cost_gate.cmake).  VENEER_BASE_COST "
            "${P_VENEER_BASE_COST} would be a charge nothing checks against "
            "the firmware that ships.")
    endif()
    # What this plugin was given, for veneer_cost_gate() to compare with what
    # it checks.  The printer bound is the plugin's OWN pl_sbuf_write limit: the
    # printer veneer lands there as well as in the firmware, so the declared
    # cost has to cover it too, and it has to be the limit this plugin actually
    # got rather than a board variable that merely starts out equal.
    set(_printer "")
    foreach(_e IN LISTS P_ENTRIES)
        if(_e MATCHES "^pl_sbuf_write=([0-9]+)$")
            set(_printer "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_printer STREQUAL "")
        message(FATAL_ERROR
            "add_plugin(${_name}): ENTRIES has no pl_sbuf_write=<limit>.  The "
            "printer veneer reaches the plugin's own sink, so its bound is part "
            "of what VENEER_BASE_COST must cover, and an unbounded one cannot "
            "be checked.")
    endif()
    set_property(GLOBAL APPEND PROPERTY VENEER_GATE_PLUGIN_COSTS
                 "${_name}=${P_VENEER_BASE_COST}")
    set_property(GLOBAL APPEND PROPERTY VENEER_GATE_PRINTER_LIMITS
                 "${_name}=${_printer}")

    # [!] THE LINK INPUTS ARE ENUMERATED HERE, AND A CALLER CANNOT ADD ONE.
    # Deriving the owned source roots closes the path a SOURCE takes into the
    # image; it does nothing about the path a LINK INPUT takes.  An unrestricted
    # flag list would let a board pass an object, an archive, a `-l`, or a `-T`
    # of its own, and that code would reach the image having been classified by
    # nothing and audited by nothing -- the same shape of accident as issue #104,
    # arriving through the other door.
    #
    # So the board passes its ARCHITECTURE and nothing else.  Everything that
    # makes a plugin a plugin -- freestanding, gc-sections, the map -- belongs to
    # this helper, because it is not a board's choice.
    foreach(_f ${P_ARCH_FLAGS})
        if(NOT _f MATCHES "^-m[A-Za-z0-9=+._-]*$")
            message(FATAL_ERROR
                "add_plugin(${_name}): ARCH_FLAGS takes the board's -m options "
                "and only those, got:\n  ${_f}\nAnything that can name a link "
                "input (an object, an archive, -l, -L, -T) would put code in the "
                "image that no ownership rule classified and no audit saw.")
        endif()
    endforeach()

    # And the same door in the other half of the link script: a MEMORY fragment
    # is a linker script, and a linker script can name inputs.
    file(READ "${P_MEMORY_LD}" _mem_txt)
    if(_mem_txt MATCHES "(^|[^A-Za-z_])(INPUT|GROUP|INCLUDE)[ \t]*\\(")
        message(FATAL_ERROR
            "add_plugin(${_name}): the MEMORY fragment ${P_MEMORY_LD} names link "
            "inputs (INPUT/GROUP/INCLUDE).  It may declare the reservation and "
            "nothing else -- an input pulled in here would bypass the audit.")
    endif()
    # An argument that landed nowhere.  cmake_parse_arguments() reports these
    # silently in UNPARSED_ARGUMENTS, so without this a stray token is simply
    # ignored.
    #
    # [!] IT IS NOT WHAT CATCHES A MISSPELLED KEYWORD, and it was written
    # believing that it was.  A token after a multi-value keyword CONTINUES that
    # keyword's list rather than becoming unparsed, so `AUDIT_SHARD "<path>"`
    # written after SOURCES appends both to SOURCES and never reaches here.
    # Measured, not reasoned about.  What catches that is the derived membership
    # rule below -- the decoder ends up compiled with nothing auditing it, which
    # is the condition that rule refuses -- and the reason the rule is derived
    # from where a file LIVES rather than from a keyword being spelled right.
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "add_plugin(${_name}): unrecognised argument(s):\n  "
            "${P_UNPARSED_ARGUMENTS}")
    endif()
    # [!] THE NAME IS VALIDATED BEFORE IT REACHES A PATH.  It is interpolated
    # into the owned source roots below, so a name that carries CMake syntax
    # widens them.  Rejecting "/" and ".." is not enough: ";" is the LIST
    # separator, so "foo;tools" is one legal directory component that still
    # splits "${_dir}" into two list elements.  A positive whitelist makes ";",
    # "$", quotes, brackets and generator expressions unrepresentable instead of
    # enumerated -- and it runs here, before ${_dir} exists.
    if(NOT _name MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
        message(FATAL_ERROR
            "add_plugin: bad plugin name '${_name}'.  One path component "
            "matching ^[A-Za-z0-9][A-Za-z0-9._-]*$ -- the name becomes an owned "
            "source root, so anything that can carry CMake or path syntax is "
            "refused rather than escaped.")
    endif()
    if(_name STREQUAL "common" OR _name STREQUAL "tools")
        message(FATAL_ERROR
            "add_plugin: '${_name}' is reserved.  asset/${_name}/ is not a "
            "plugin, and deriving an owned root over it would exempt it from the "
            "no-mutable-storage audit.")
    endif()
    set(_dir "${_ADD_PLUGIN_ASSET_ROOT}/plugins/${_name}")
    set(_out "${P_OUT_DIR}/${_name}")
    # [!] asset/common IS ENUMERATED, NOT GLOBBED, and every file it gains has
    # to be added HERE.  A new common .c that is not in this list simply is not
    # linked: the plugin builds, the gate passes, and the entry point that
    # needed it fails at link time or -- worse, if it was only referenced from
    # one path -- not at all.
    set(_srcs "${_dir}/plugin_main.c"
              "${_ADD_PLUGIN_ASSET_ROOT}/common/plugin_base.c"
              "${_ADD_PLUGIN_ASSET_ROOT}/common/plugin_fmt.c"
              "${_ADD_PLUGIN_ASSET_ROOT}/common/plugin_libc.c"
              "${_ADD_PLUGIN_ASSET_ROOT}/common/plugin_text.c"
              ${P_SOURCES})

    set(_objs "")
    set(_sus "")
    # [!] THE MEMBERSHIP RULE RUNS BOTH WAYS, and the second direction is the one
    # that matters.  Checking only that each AUDIT_SHARED entry is compiled
    # catches a stale path; it does NOT catch the omission -- a shared file left
    # out of AUDIT_SHARED is compiled into the image, linked, and never audited,
    # with nothing to say so.  That is the exact fail-open this interface exists
    # to prevent, so the requirement is derived rather than declared: a source
    # this plugin compiles that is NOT the plugin's own must be audited.
    #
    # "The plugin's own" is TWO DERIVED ROOTS -- asset/common and this plugin's
    # own directory -- and NOT the whole asset/ tree.  Those legally own storage;
    # a plugin has .bss and is supposed to.  Anything reached from outside them is
    # a file other boards also build, and the no-storage rule (issue #97) applies.
    #
    # [!] THE ROOTS ARE DERIVED, NEVER ACCEPTED FROM THE CALLER.  Forgetting a
    # root fails CLOSED (the source must then appear in AUDIT_SHARED); widening
    # one fails OPEN.  A root argument would let a board pass ${CMAKE_SOURCE_DIR}
    # and exempt everything, and no amount of canonicalising sees that mistake.
    # asset/tools/ is deliberately outside both: the packer and the container
    # verifier are host tools, not plugin storage.
    #
    # [!] AND THE COMPARISON IS PHYSICAL, NOT LEXICAL.  ABSOLUTE does not resolve
    # symlinks, so a tracked asset/plugins/<name>/x.c -> ../../../svc/blazeface.c
    # used to start with the owned prefix, compile, and never be audited.
    # REALPATH canonicalises both sides, and the resolved paths are what gets
    # compiled below -- classifying one path and compiling another would put the
    # audit on a file that is not the one linked.
    set(_own_roots "")
    foreach(_r "${_ADD_PLUGIN_ASSET_ROOT}/common" "${_dir}")
        get_filename_component(_r_real "${_r}" REALPATH)
        list(APPEND _own_roots "${_r_real}")
    endforeach()
    set(_srcs_real "")
    foreach(_src ${_srcs})
        if(NOT EXISTS "${_src}")
            message(FATAL_ERROR
                "plugin ${_name}: no such source:\n  ${_src}\nOwnership is "
                "decided by where a file lives, so a path that does not resolve "
                "cannot be classified.  Sources generated at build time are not "
                "supported here.")
        endif()
        get_filename_component(_src_abs "${_src}" REALPATH)
        list(APPEND _srcs_real "${_src_abs}")
    endforeach()
    set(_auds_real "")
    foreach(_aud ${P_AUDIT_SHARED})
        if(NOT EXISTS "${_aud}")
            message(FATAL_ERROR
                "plugin ${_name}: AUDIT_SHARED names a file that does not "
                "exist:\n  ${_aud}")
        endif()
        get_filename_component(_aud_abs "${_aud}" REALPATH)
        list(APPEND _auds_real "${_aud_abs}")
    endforeach()

    foreach(_src_abs ${_srcs_real})
        set(_own FALSE)
        foreach(_root ${_own_roots})
            string(FIND "${_src_abs}" "${_root}/" _at)
            if(_at EQUAL 0)
                set(_own TRUE)
                break()
            endif()
        endforeach()
        list(FIND _auds_real "${_src_abs}" _aud_at)
        if(NOT _own AND _aud_at EQUAL -1)
            string(REPLACE ";" "\n    " _roots_pretty "${_own_roots}")
            message(FATAL_ERROR
                "plugin ${_name} compiles a file from outside its own "
                "roots:\n  ${_src_abs}\nand does not name it in AUDIT_SHARED.  "
                "A file other boards also build must own no mutable storage "
                "(issue #97), and nothing would check it.\nOwned roots:\n    "
                "${_roots_pretty}")
        endif()
    endforeach()

    # And the other direction, which catches a stale path rather than an
    # omission: an AUDIT_SHARED entry this plugin does not compile would audit
    # nothing while looking like it audited something.
    foreach(_aud_abs ${_auds_real})
        list(FIND _srcs_real "${_aud_abs}" _src_at)
        if(_src_at EQUAL -1)
            message(FATAL_ERROR
                "plugin ${_name}: AUDIT_SHARED names a file this plugin does "
                "not compile:\n  ${_aud_abs}\nNothing would audit it, and "
                "nothing would say so.")
        endif()
    endforeach()

    # [!] THE OBJECT IS A BYPRODUCT; THE COMMAND'S OUTPUT IS A SUCCESS STAMP.
    # The compile and the audit are one command, so when the audit REJECTS, the
    # compiler has already written the object -- and whether the next build
    # re-runs that edge is then ninja's dirty logic rather than anything this
    # repo controls.  The stamp makes the question moot: the link depends on it,
    # and it exists only if the audit passed.
    #
    # [!] AND IT IS DELETED BEFORE THE COMPILE, not merely written after the
    # audit.  Writing it after is not enough on its own -- a stamp left by an
    # EARLIER successful build would survive a later compile whose audit
    # rejected, and a relink forced by some other dependency would then consume
    # the rejected object.  A stamp that is never removed records that some build
    # once succeeded, which is not the question being asked.
    set(_objs "")
    set(_sus "")
    set(_stamps "")
    foreach(_src_abs ${_srcs_real})
        get_filename_component(_stem "${_src_abs}" NAME_WE)
        set(_obj "${_out}/${_stem}.o")
        set(_stamp "${_out}/${_stem}.audited")
        set(_audit_cmd "")
        list(FIND _auds_real "${_src_abs}" _aud_at)
        if(NOT _aud_at EQUAL -1)
            set(_audit_cmd
                COMMAND "${Python3_EXECUTABLE}"
                        "${_ADD_PLUGIN_DIR}/check_no_mutable_storage.py"
                        --objdump "${CMAKE_OBJDUMP}" --nm "${CMAKE_NM}"
                        --label "${_src_abs} (plugin ${_name})"
                        "${_obj}")
        endif()
        add_custom_command(
            OUTPUT "${_stamp}"
            BYPRODUCTS "${_obj}" "${_out}/${_stem}.su"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E rm -f "${_stamp}"
            COMMAND "${CMAKE_C_COMPILER}" ${P_CFLAGS}
                    -I "${_dir}"
                    -c "${_src_abs}" -o "${_obj}"
            ${_audit_cmd}
            COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
            DEPENDS "${_src_abs}"
                    "${_ADD_PLUGIN_DIR}/check_no_mutable_storage.py"
            WORKING_DIRECTORY "${_out}"
            COMMENT "plugin ${_name}: cc ${_stem}.c"
            VERBATIM)
        list(APPEND _objs "${_obj}")
        list(APPEND _sus "${_out}/${_stem}.su")
        list(APPEND _stamps "${_stamp}")
    endforeach()

    # Written at configure time, so a deleted wrapper comes back on the next
    # cmake run; it is in the link's DEPENDS above, so a changed one relinks.
    set(PLUGIN_MEMORY_LD "${P_MEMORY_LD}")
    set(PLUGIN_SECTIONS_LD "${_ADD_PLUGIN_ASSET_ROOT}/common/plugin.ld")
    configure_file("${_ADD_PLUGIN_ASSET_ROOT}/common/plugin_link.ld.in"
                   "${_out}/plugin_link.ld" @ONLY)

    add_custom_command(
        OUTPUT "${_out}/plugin.elf" "${_out}/plugin.stacks.json"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out}"
        COMMAND "${CMAKE_C_COMPILER}" -nostdlib -nostartfiles ${P_ARCH_FLAGS}
                -T "${_out}/plugin_link.ld"
                -Wl,--gc-sections -Wl,--no-warn-rwx-segments
                # [!] THE QUOTES GO ROUND THE WHOLE ARGUMENT.  Written as
                # -Wl,-Map="${...}" under VERBATIM, CMake escapes the inner
                # quotes and ld is handed a filename that literally contains
                # them: the map is silently never written, and the only sign is
                # a warning in a build that otherwise succeeds.
                "-Wl,-Map=${_out}/plugin.map"
                ${_objs} -o "${_out}/plugin.elf"
        COMMAND "${Python3_EXECUTABLE}" "${_ADD_PLUGIN_GATE}"
                "${_out}/plugin.elf"
                --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
                # [!] THE BOARD'S OWN STATEMENT OF THE RESERVATION, NOT THE
                # FRAGMENT'S.  The plugin was linked against MEMORY_LD; checking
                # it against an address read out of that same file would pass
                # any address at all.
                --base "${P_IMAGE_BASE}" --end "${P_IMAGE_END}"
                --forbid ${P_FORBIDDEN}
                --veneer-base-cost "${P_VENEER_BASE_COST}"
                # [!] THE WORD THE PACKER WILL STAMP, checked here against the
                # image's own .ARM.attributes (issue #108).  Until then the
                # packer, the verifier and the firmware all read this one value
                # and nothing asked whether it was true.
                --target-id "${P_TARGET_ID}"
                --su ${_sus}
                # [!] EVERY SLOT THE PLUGIN EXPORTS, not just the interesting
                # ones.  The packer refuses to declare a stack for a slot nobody
                # measured, so a short list here does not under-report -- it
                # stops the container being built at all, which is the right
                # direction but a confusing place to discover it.
                #
                # [!] AND ONE NAME THAT IS NOT A SLOT.  pl_sbuf_write is reached
                # through the pl_print_write veneer, and the gate cannot see
                # across a veneer -- it charges a flat allowance there for
                # whatever is on the other side, which is normally the BASE.
                # A plugin-supplied printer puts its own code there instead, so
                # that assumption is bounded here by name rather than trusted to
                # a comment.  Since ABI 2 (issue #111) its bound also reaches
                # the manifest, as S: the loader charges max(c, S) at a
                # crossing, so a later, smaller c cannot under-charge a sink
                # this plugin carries.
                # [!] THE SAME VARIABLES THE FIRMWARE'S POLICY USES.  Written
                # out again here, the gate and the device would be two
                # declarations of one rule, and a plugin could pass the build
                # and be refused on the board -- the shape issue #93 hit.  The
                # numbers are derived where they are set, from the measured
                # call-site depth.
                --entry ${P_ENTRIES}
                --emit-stacks "${_out}/plugin.stacks.json"
        # [!] THE STAMPS, not just the objects: an object exists whether or not
        # its audit passed, so depending on it alone would let a rejected one
        # reach the linker.  The objects and .su files are listed too because
        # they are what this command actually reads.
        DEPENDS ${_stamps} ${_objs} ${_sus}
                "${_out}/plugin_link.ld"
                "${_ADD_PLUGIN_ASSET_ROOT}/common/plugin.ld" "${P_MEMORY_LD}"
                "${_ADD_PLUGIN_GATE}"
        COMMENT "plugin ${_name}: ld + gate -> plugin.elf"
        VERBATIM)

    set(${P_OUT_VAR} ${${P_OUT_VAR}} "${_out}/plugin.elf" PARENT_SCOPE)
endfunction()
