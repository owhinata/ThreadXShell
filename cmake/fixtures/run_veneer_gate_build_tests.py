#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Build-level tests for cmake/veneer_cost_gate.cmake (issue #112).

check_veneer_base_cost.py derives the stack the firmware spends below each
plugin veneer; cmake/fixtures/run_veneer_cost_tests.py tests THAT.  What is
tested here is the other half, the part that makes the check a gate: the
wiring.  A check that is correct but that a build can go around is a report,
and every way around it is a CMake / ninja fact, so it is tested by building --
a small real Cortex-M7 project, configured by real cmake and built by real
ninja with the repository's pinned cross compiler, against COPIES of the helper
and both scripts (so a script can be edited here, and a mutated helper passed
with --cmake-dir, without touching the tree).

[!] THE DELIVERIES REFUSE ON THEIR OWN.  The fake `flash` and the fake assets
do not trust the dependency graph under test: each one runs deliver.py, which
fails unless the stamp exists AND is newer than the image.  So "flash ran
before the check", "an asset was packed with no check", and "a stamp from an
earlier image let it through" are each a failure of the delivery itself, not a
property inferred from ninja's plan.

The project mirrors how the boards feed their image, one of each:
  - root.c         in the executable itself (the six veneer roots, bound by
                   bind.c's plugin_board_bindings -- issue #165),
  - objsrc.c       an OBJECT library taken by $<TARGET_OBJECTS:> (Grove's
                   shell_objs),
  - objlink.c      an OBJECT library in target_link_libraries (coremark_obj),
  - lib.c          a STATIC library reached through an INTERFACE library
                   (wio's tflm, Grove's bsp_iface),
  - other.c        an unrelated executable (wio's boot reference build) that
                   must NOT be instrumented.
Each root calls into one of the two object libraries, so a body with no record
below a veneer -- a compile the helper did not find -- fails the check.  The
static library is reached from main() only: its record is asserted to EXIST
(the helper found the compile), but the check cannot use it, because it cannot
yet name the record of an in-tree archive member and refuses one below a veneer
(fail-closed; the C4 hand-over of #112, and wio's libtflm.a).

Cases:
  firmware_alone    `ninja shell` builds the image and leaves no stamp: the
                    check is its own edge, not a side effect of linking.
  flash_waits       then `ninja flash` runs the check before the flash command.
  noop              `ninja` again reruns nothing.
  witnesses         every TU of the image has its record; other.c has none.
  script_change     editing any of its scripts (the veneer set lives in
                    check_plugin_image.py, the decoder and the walk in
                    thumb_stack_analysis.py) reruns the check without a relink.
  image_change      a source edit that relinks the image reruns the check.
  relink_retracts   building the IMAGE alone relinks without checking, and
                    that link removes the stamp: a pass cannot outlive the
                    image it was about.
  failure           DECLARED one below the derived value: the build fails
                    naming the chain, no stamp survives, and neither flash nor
                    either asset proceeds.
  recovery          the derived value again: the stamp comes back and flash
                    proceeds.
  asset_alone       from a fresh tree, `ninja asset-bare` (an asset with no
                    dependency of its own) runs the check first.
  asset_packed      the same for an asset whose pack rule names the gate, as
                    the boards' do.
  shared_analysis   (issue #124) with a real add_plugin() in the tree:
                    editing thumb_stack_analysis.py ALONE reruns both gates --
                    the firmware's check and the plugin's link + gate -- and
                    relinks no firmware; with no edit, nothing reruns; and a
                    refusal added to that module alone fails BOTH gates for
                    that reason (the plugin gate's stack bound is the shared
                    analysis since step 2 of #124), leaves no stamp, and stops
                    flash, both assets and the plugin's asset.
  ltrans_drop       an LTO link that writes fewer partitions than the last
                    (max -> one) still passes: the old records are removed
                    before the link, not left for the check to refuse.
  stack_wiring      (issue #126) the board's plugin_stack_table() reaches
                    the policy TU as its allowances, and the read-back of the
                    shipped image reports the table's numbers slot by slot --
                    following a change of the table.
  stack_override_down / stack_override_up
                    a later -D of one allowance (CMAKE_C_FLAGS), lower and
                    then higher than the table: the image links, and the
                    read-back stops flash and the asset, naming the slot.
  stack_every_slot  each slot's limit, one byte off on its own: refused,
                    naming that slot and no other.
  stack_mismap      the firmware maps draw to the other allowance: refused,
                    because the two allowances differ in value.
  stack_mismap_equal
                    the same mismapping with EQUAL allowances PASSES -- what
                    the read-back cannot see, stated as a case so that it is
                    a documented limit rather than a surprise.
  stack_rows        the probe itself, on the built image, refuses a table
                    with a slot missing or an unknown slot.
  delivery_*        (issue #126, P14) the delivery check over build.ninja:
                    the plain tree passes; a new target that copies the image
                    -- by dependency, by name in its command alone, from
                    another executable's POST_BUILD, or gated through flash
                    but not declared -- is refused BEFORE it runs; DELIVERY
                    naming a target that uses no artifact is refused; a target
                    deferred past the wiring refuses the configure and cannot
                    be run directly; shell separators and braces after the
                    name, and a POST_BUILD copy declared as a BYPRODUCT, are
                    refused; and a script that computes the path itself, or a
                    copy nobody declared, gets through (documented limits).
  cost_wiring       (issue #111) DECLARED reaches BOTH places the loader's c
                    is read from, and follows a change of it: the compile of a
                    TU in the $<TARGET_OBJECTS:> library (where Grove's policy
                    lives) as PLUGIN_VENEER_BASE_COST, and an asset rule's
                    command line through veneer_cost_gate_declared() (where the
                    host container verifier gets it).  A loader test cannot see
                    this: it is handed a policy, never the build's.
  bind_decoy        (issue #165) the table the firmware hands the plugin
                    (plugin_board_bindings) binds log to a function deeper
                    than DECLARED.  Passed until step 3 of #165, while the
                    check walked a ROOTS list naming the shallow
                    nn_plugin_log; now the roots are the table's own words,
                    and pl_base_log is refused in [budget], naming it.
  bind_deep_each    each of the other five members deep on its own: refused
                    for that veneer alone (the veneer per member is stated in
                    this file, not read from the record).
  bind_shape        the table in a writable section, weak, or with a second
                    object of its name: refused; bind_none: no table, the
                    link fails.
  entry_old         an object defining one of the pre-#165 entries that took
                    a base or a vtable's functions (plugin_exec_load,
                    plugin_run_load, nn_active_report, nn_active_core_report,
                    plugin_paint_vtable), or the draw's that took a painter
                    the board built and so could edit (nn_active_draw,
                    nn_active_core_draw, plugin_paint_bind; the review of
                    #165): refused by check_policy_entry.py.
  entry_rename      (review of #165) a -D on the one file that reads the
                    policy or the table by name, renaming what it reads, with
                    and without LTO: refused, the reader not referencing the
                    object the build reads back.
  body_twice        (review of #165) a second firmware file including the
                    parse body: the link fails on the body's guard, with and
                    without LTO (and so does the host file in the firmware's
                    objects, entry_host_tu).
  policy_decoy      (issue #165) the read-back's policy is right, while the
                    code hands an entry that takes a policy a different
                    read-only one whose c is 1.  Passed until step 2 of #165;
                    now refused by check_policy_entry.py, naming the object
                    that defines the entry, before the probe runs.
  entry_clean       (issue #165) the plain tree: every input of the link read
                    (objects and the static library's member), none takes a
                    policy.
  entry_map         the map edited to LOAD an object not on disk, to include
                    a member its archive does not hold, and a member of an
                    archive it did not LOAD: each refused.
  entry_host_tu     the host's real entry (plugin_load_host.c) in the
                    firmware's object library, and in its static library with
                    a caller: refused, naming the definition (the archive
                    member) and the reference.
  entry_lto         the decoy under LTO: refused, the IR read through gcc-nm;
                    the same map with a plain nm is refused, not read as the
                    objects' empty wrappers.
  policy_shape      (issue #165) plugin_board_policy twice (a second, local
                    object of the name), weak, of another size, or a
                    function: each refused by the read-back.
  policy_none       no plugin_board_policy: the link fails naming it.
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
COPIED = ("veneer_cost_gate.cmake", "check_veneer_base_cost.py",
          "thumb_stack_analysis.py", "check_plugin_image.py",
          "check_policy_probe.py", "plugin_stack_table.cmake",
          "check_delivery_gate.py", "add_plugin.cmake",
          "check_policy_entry.py",
          "check_no_mutable_storage.py")
# What add_plugin() compiles a plugin from (shared_analysis), copied so that
# the helper's derived asset root (cmake/../asset) resolves inside the tree.
ASSET_COPIED = ("common", os.path.join("plugins", "cifar10"))

LINKER_SCRIPT = """
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1M
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
SECTIONS {
  .text : { *(.text*) } > FLASH
  .rodata : { *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(COMMON) } > RAM
}
"""

# A frame in every body (the volatile array), and no inlining, so that each
# chain goes through the TU it names -- LTO included.
LEAF = """
__attribute__((noinline, used)) int %s(int x)
{
    volatile char b[%d];
    b[0] = (char)x;
    return b[0] + 1;
}
"""

SOURCES = {
    "root.c": """
extern int obj_src_leaf(int);
extern int obj_link_leaf(int);
extern int lib_leaf(int);
#define ROOT(name, callee, n)                                              \\
    __attribute__((noinline, used)) int name(int x)                        \\
    {                                                                      \\
        volatile char b[n];                                                \\
        b[0] = (char)x;                                                    \\
        return callee(b[0]) + 1;                                           \\
    }
ROOT(nn_plugin_log, obj_src_leaf, 16)
ROOT(nn_active_to_frame, obj_link_leaf, 24)
ROOT(paint_rect, obj_src_leaf, 32)
ROOT(paint_fill_rect, obj_link_leaf, 8)
ROOT(paint_blit, obj_src_leaf, 8)
ROOT(nn_report_write, obj_link_leaf, 8)
int main(void)
{
    return nn_plugin_log(1) + nn_active_to_frame(2) + paint_rect(3)
           + paint_fill_rect(4) + paint_blit(5) + nn_report_write(6)
           + lib_leaf(7);
}
""",
    "objsrc.c": LEAF % ("obj_src_leaf", 40),
    # The board's plugin policy stands here: it reads the c the helper
    # defines, and its SIZE carries the value into the object, where nm reads it.
    "policy.c": """
#include "plugin_load.h"
#ifndef PLUGIN_VENEER_BASE_COST
#error "PLUGIN_VENEER_BASE_COST did not reach this compile"
#endif
__attribute__((used)) char policy_cost_probe[PLUGIN_VENEER_BASE_COST];
/* A board's policy, as nn_svc_grove.c / nn_svc_wio.c write it: under the name
 * the firmware's plugin_parse() reads (svc/plugin/plugin_load_board.c is linked
 * here too), and the stamp waits for check_policy_probe.py to read it back
 * from shell.elf by that name (issue #165).  The allowances arrive from
 * plugin_stack_table(), and this TU maps each slot to one of them on its own,
 * as the boards' firmware does (issue #126). */
#if !defined(FIX_STACK_A) || !defined(FIX_STACK_B)
#error "plugin_stack_table() did not define the allowances on this compile"
#endif
#ifdef FIX_POLICY_MISMAP
#define FIX_DRAW FIX_STACK_A
#else
#define FIX_DRAW FIX_STACK_B
#endif
/* One slot nudged by a byte, so a case can show that EVERY slot is compared
 * -- with A on six of them, an override of A alone would let a check that
 * skipped five go unnoticed. */
#ifndef FIX_BUMP_SLOT
#define FIX_BUMP_SLOT 99
#endif
#define FIX_B_(i) ((FIX_BUMP_SLOT) == (i) ? 1u : 0u)
/* The shapes of the policy the read-back refuses (issue #165): one placed in a
 * writable output section (the const declaration of plugin_load.h stays), one
 * weak, and none at all here -- FIX_POLICY_NONE alone (the link must fail), or
 * with twin.c supplying a global of another size (FIX_POLICY_ARRAY). */
#if defined(FIX_POLICY_RW)
#define FIX_POLICY_ATTR __attribute__((section(".data.fix_policy")))
#elif defined(FIX_POLICY_WEAK)
#define FIX_POLICY_ATTR __attribute__((weak))
#else
#define FIX_POLICY_ATTR
#endif
#if !defined(FIX_POLICY_NONE) && !defined(FIX_POLICY_ARRAY) && \
    !defined(FIX_POLICY_FUNC)
FIX_POLICY_ATTR const struct plugin_policy plugin_board_policy = {
    .stack_limit      = { FIX_STACK_A + FIX_B_(0), FIX_STACK_A + FIX_B_(1),
                          FIX_STACK_A + FIX_B_(2), FIX_DRAW + FIX_B_(3),
                          FIX_STACK_A + FIX_B_(4), FIX_STACK_A + FIX_B_(5),
                          FIX_STACK_A + FIX_B_(6) },
    .veneer_cost      = PLUGIN_VENEER_BASE_COST,
    .stack_accounting = PLUGIN_STACK_ACCOUNTING,
};
#endif


/* (issue #165, policy_decoy) plugin_board_policy above is correct, and the
 * read-back reads it; the code hands an entry that takes a policy another
 * read-only one, with c = 1.  The entry is a stand-in under the host entry's
 * real name and prototype (plugin_load_host.h); check_policy_entry.py refuses
 * a firmware link that holds it.  FIX_CALL_HOST is the same call to the REAL
 * host entry, which a case links in from elsewhere. */
#if defined(FIX_POLICY_DECOY) || defined(FIX_CALL_HOST)
#include "plugin_load_host.h"
__attribute__((used)) const struct plugin_policy fix_policy_used = {
    .stack_limit      = { FIX_STACK_A, FIX_STACK_A, FIX_STACK_A, FIX_STACK_B,
                          FIX_STACK_A, FIX_STACK_A, FIX_STACK_A },
    .veneer_cost      = 1u,
    .stack_accounting = PLUGIN_STACK_ACCOUNTING,
};
#ifdef FIX_POLICY_DECOY
__attribute__((noinline, used)) enum plugin_result
plugin_parse_with_policy(const void *buf, size_t len,
                         const struct plugin_policy *pol,
                         struct plugin_view *out)
{
    (void)buf;
    (void)len;
    (void)out;
    return pol->veneer_cost != 0u ? PLUGIN_OK : PLUGIN_ERR_SHORT;
}
#endif
__attribute__((noinline, used)) enum plugin_result
fix_load(const void *buf, size_t len, struct plugin_view *out)
{
    return plugin_parse_with_policy(buf, len, &fix_policy_used, out);
}
#endif
""",
    # (issue #165) A second object under the policy's name, local, which does
    # not include plugin_load.h (FIX_POLICY_TWIN); or the only definition, a
    # global of another size (FIX_POLICY_ARRAY, with FIX_POLICY_NONE's absence
    # in policy.c).
    "twin.c": """
#if defined(FIX_BIND_TWIN)
__attribute__((used)) static const unsigned plugin_board_bindings[9] = { 1u };
#endif
#if defined(FIX_POLICY_TWIN)
__attribute__((used)) static const unsigned plugin_board_policy[14] = { 1u };
#elif defined(FIX_POLICY_ARRAY)
const unsigned plugin_board_policy[20] = { 1u };
#elif defined(FIX_POLICY_FUNC)
void plugin_board_policy(void) {}
#endif
int fix_twin_unit(void) { return 0; }
""",
    # (issue #165) The board's bindings: the six functions behind the veneers,
    # under the name the loader and the check read.  The roots in root.c are
    # int(int) bodies; the casts give them the members' types, which is all
    # the check reads (a code address).  FIX_DEEP_SLOT puts fix_deep, far
    # deeper than DECLARED, in one member.  The shapes the check refuses:
    # FIX_BIND_RW (a writable section), FIX_BIND_WEAK, FIX_BIND_NONE (no table:
    # the link fails), FIX_BIND_TWIN (twin.c adds a local of the name).
    "bind.c": """
#include "plugin_bindings.h"
extern int nn_plugin_log(int);
extern int nn_active_to_frame(int);
extern int paint_rect(int);
extern int paint_fill_rect(int);
extern int paint_blit(int);
extern int nn_report_write(int);
__attribute__((noinline, used)) int fix_deep(int x)
{
    volatile char b[8192];
    b[0] = (char)x;
    return b[0] + 1;
}
#ifndef FIX_DEEP_SLOT
#define FIX_DEEP_SLOT 99
#endif
#define PICK(i, f) ((FIX_DEEP_SLOT) == (i) ? fix_deep : (f))
#if defined(FIX_BIND_RW)
#define FIX_BIND_ATTR __attribute__((section(".data.fix_bind")))
#elif defined(FIX_BIND_WEAK)
#define FIX_BIND_ATTR __attribute__((weak))
#else
#define FIX_BIND_ATTR
#endif
#ifndef FIX_BIND_NONE
FIX_BIND_ATTR const struct plugin_bindings plugin_board_bindings = {
    .base = {
        .version  = PLUGIN_ABI_VERSION,
        .size     = (uint32_t)sizeof(struct plugin_base_api),
        .log      = (void (*)(void *, const char *, size_t))
                    PICK(0, nn_plugin_log),
        .to_frame = (int (*)(void *, float, float, float, float,
                             struct plugin_rect *))PICK(1, nn_active_to_frame),
    },
    .rect      = (void (*)(void *, const struct plugin_rect *, uint16_t,
                           uint16_t))PICK(2, paint_rect),
    .fill_rect = (void (*)(void *, const struct plugin_rect *, uint16_t))
                 PICK(3, paint_fill_rect),
    .blit      = (void (*)(void *, const struct plugin_rect *,
                           const uint16_t *, uint32_t, int32_t))
                 PICK(4, paint_blit),
    .write     = (int (*)(void *, const char *, size_t))
                 PICK(5, nn_report_write),
};
#endif
""",
    # (issue #165) The entries that took a base or handed a vtable its
    # functions, under their pre-#165 names: a firmware object that still
    # defines one is refused by check_policy_entry.py.
    "old.c": """
#if defined(FIX_OLD_EXEC)
int plugin_exec_load(void) { return 0; }
#elif defined(FIX_OLD_RUN)
int plugin_run_load(void) { return 0; }
#elif defined(FIX_OLD_REPORT)
int nn_active_report(void) { return 0; }
#elif defined(FIX_OLD_CORE_REPORT)
int nn_active_core_report(void) { return 0; }
#elif defined(FIX_OLD_PAINT)
int plugin_paint_vtable(void) { return 0; }
#elif defined(FIX_OLD_DRAW)
int nn_active_draw(void) { return 0; }
#elif defined(FIX_OLD_CORE_DRAW)
int nn_active_core_draw(void) { return 0; }
#elif defined(FIX_OLD_PAINT_BIND)
int plugin_paint_bind(void) { return 0; }
#elif defined(FIX_SECOND_PARSE)
/* A second body under the firmware entry's name, local to this file: the
 * entry that reads the policy by name must be defined exactly once. */
__attribute__((used)) static int plugin_parse(void) { return 0; }
#endif
int fix_old_unit(void) { return 0; }
""",
    # (issue #165 review) The object a -D on the reader's own file renames
    # the policy or the table to: the read-back finds the real one, the reader
    # reads this.
    "runtime.c": """
#include "plugin_load.h"
#include "plugin_bindings.h"
#if defined(FIX_RUNTIME_POLICY)
const struct plugin_policy runtime_policy = { 0 };
#endif
#if defined(FIX_RUNTIME_BINDINGS)
const struct plugin_bindings runtime_bindings = { { 0 }, 0, 0, 0, 0 };
#endif
int fix_runtime_unit(void) { return 0; }
""",
    # (issue #165 review) The host entry's name and type, without the body,
    # for a static-library member (FIX_HOST_TU=LIB).
    "hostlike.c": """
#include "plugin_load_host.h"
enum plugin_result plugin_parse_with_policy(const void *buf, size_t len,
                                            const struct plugin_policy *pol,
                                            struct plugin_view *out)
{
    (void)buf; (void)len; (void)out;
    return pol != NULL ? PLUGIN_OK : PLUGIN_ERR_ARG;
}
""",
    # (issue #165 review) A second firmware file that includes the parse body
    # to call it with a policy of its own: the link must refuse it.
    "body2.c": """
#if defined(FIX_BODY_TWICE)
#include "plugin_load_body.h"
static const struct plugin_policy my_policy = { 0 };
enum plugin_result fix_own_parse(const void *buf, size_t len,
                                 struct plugin_view *out)
{
    return parse_checked(buf, len, &my_policy, out);
}
#endif
int fix_body2_unit(void) { return 0; }
""",
    # What svc/plugin/plugin_load_board.c needs from a C library this
    # -nostdlib fixture does not link.  Never called: nothing runs the image.
    "libc.c": """
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n)
{
    char *dp = d;
    const char *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    char *dp = d;
    while (n--)
        *dp++ = (char)c;
    return d;
}
""",
    "record.py": """import sys
open(sys.argv[2], "w").write(sys.argv[1] + "\\n")
""",
    "objlink.c": LEAF % ("obj_link_leaf", 48),
    "lib.c": LEAF % ("lib_leaf", 56),
    "other.c": "int main(void) { volatile char b[8]; b[0] = 1; "
               "return b[0]; }\n",
    # The delivery's own refusal (see the module docstring).
    # The blind spot of the delivery check: it finds the image on its own.
    # The OpenOCD/Tcl shape: the path inside braces (issue #126 review).
    "brace.py": """import os, re, shutil, sys
shutil.copy(re.search(r"[{]([^}]*)[}]", sys.argv[1]).group(1), sys.argv[2])
""",
    "hidden.py": """import os, shutil, sys
shutil.copy(os.path.join(sys.argv[1], "shell" + "." + "elf"),
            os.path.join(sys.argv[1], sys.argv[2]))
""",
    "deliver.py": """import os, sys
stamp, image, out = sys.argv[1:4]
if not os.path.exists(stamp):
    sys.exit("deliver: no stamp -- delivered without a passed check")
if os.path.getmtime(stamp) < os.path.getmtime(image):
    sys.exit("deliver: the stamp is older than the image")
open(out, "w").write("delivered\\n")
""",
}

CMAKELISTS = r"""
cmake_minimum_required(VERSION 3.20)
project(veneer_gate_build C)
set(Python3_EXECUTABLE "@PYTHON@")
set(DECLARED 4096 CACHE STRING "")
set(FIX_LTO OFF CACHE STRING "")
set(FIX_A 300 CACHE STRING "")
set(FIX_DELIVERY flash CACHE STRING "")
set(FIX_EXTRA "" CACHE STRING "")
set(FIX_B 500 CACHE STRING "")
set(FIX_PLUGIN OFF CACHE BOOL "")

# (issue #165) FIX_HOST_TU puts the host's policy-taking entry where it must
# never be: OBJ in the firmware's object library, LIB in its static library.
set(FIX_HOST_TU "" CACHE STRING "")
add_library(fw_objs OBJECT objsrc.c policy.c twin.c libc.c bind.c old.c
            runtime.c body2.c
            "@SVC@/plugin/plugin_load_board.c" "@SVC@/crc32.c"
            "@SVC@/plugin/plugin_bindings.c" "@SVC@/plugin/plugin_exec.c")
# (issue #165 review) FIX_RENAME renames, on the reader's own file only, the
# object it reads by name -- the shape check_policy_entry.py must refuse.
set(FIX_RENAME "" CACHE STRING "")
if(FIX_RENAME STREQUAL "policy")
    set_source_files_properties("@SVC@/plugin/plugin_load_board.c" PROPERTIES
        COMPILE_DEFINITIONS "plugin_board_policy=runtime_policy")
elseif(FIX_RENAME STREQUAL "bindings")
    set_source_files_properties("@SVC@/plugin/plugin_bindings.c" PROPERTIES
        COMPILE_DEFINITIONS "plugin_board_bindings=runtime_bindings")
endif()
if(FIX_HOST_TU STREQUAL "OBJ")
    target_sources(fw_objs PRIVATE "@SVC@/plugin/plugin_load_host.c")
endif()
target_include_directories(fw_objs PRIVATE "@SVC@" "@SVC@/nn" "@SVC@/plugin" "@SVC@/camera" "@SVC@/blob")
add_library(fw_objlink OBJECT objlink.c)
add_library(fw_lib STATIC lib.c)
if(FIX_HOST_TU STREQUAL "LIB")
    target_sources(fw_lib PRIVATE hostlike.c)
    target_include_directories(fw_lib PRIVATE "@SVC@" "@SVC@/plugin")
endif()
add_library(fw_iface INTERFACE)
target_link_libraries(fw_iface INTERFACE fw_lib)

add_executable(shell root.c $<TARGET_OBJECTS:fw_objs>)
set_target_properties(shell PROPERTIES SUFFIX .elf)
target_link_libraries(shell PRIVATE fw_iface fw_objlink)
target_link_options(shell PRIVATE
    -T "${CMAKE_SOURCE_DIR}/fix.ld" "-Wl,-Map=${CMAKE_BINARY_DIR}/shell.map"
    -Wl,-e,main)
if(NOT FIX_LTO STREQUAL "OFF")
    # The static library stays a plain archive (no gcc-ar needed).
    foreach(t shell fw_objs fw_objlink)
        target_compile_options(${t} PRIVATE -flto)
    endforeach()
    target_link_options(shell PRIVATE -flto -flto-partition=${FIX_LTO})
endif()

add_executable(other other.c)
set_target_properties(other PROPERTIES SUFFIX .elf)
target_link_options(other PRIVATE -T "${CMAKE_SOURCE_DIR}/fix.ld" -Wl,-e,main)

set(STAMP "${CMAKE_BINARY_DIR}/veneer_cost/shell.checked")
set(DELIVER "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/deliver.py"
    "${STAMP}" "$<TARGET_FILE:shell>")
add_custom_target(flash
    COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/flashed"
    DEPENDS shell VERBATIM)

include("${CMAKE_SOURCE_DIR}/cmake/veneer_cost_gate.cmake")
veneer_cost_gate(
    FIRMWARE shell
    MAP      "${CMAKE_BINARY_DIR}/shell.map"
    DECLARED ${DECLARED}
    PREBUILT_ROOTS "@PREBUILT@"
    DELIVERY ${FIX_DELIVERY})

# --- issue #126 (P14): targets the delivery check must see ------------------
# FIX_EXTRA picks one; each "delivers" by writing a file, and must not get to.
set(_img "${CMAKE_BINARY_DIR}/shell.elf")
if(FIX_EXTRA STREQUAL "undeclared")
    # A new flashing target, wired to the image and not to the gate.  Its
    # command does not name the image: only the dependency says it uses it.
    add_custom_target(flash2
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/hidden.py"
                "${CMAKE_BINARY_DIR}" flashed2
        DEPENDS shell VERBATIM)
elseif(FIX_EXTRA STREQUAL "by_command")
    # No dependency at all: the image is named only in the command, relative
    # to the build directory (wio's dfu-shell shape).
    add_custom_target(flash3
        COMMAND "${CMAKE_COMMAND}" -E copy shell.elf flashed3
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" VERBATIM)
elseif(FIX_EXTRA STREQUAL "post_build_other")
    # Another executable's POST_BUILD that ships this firmware.
    add_custom_command(TARGET other POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "${_img}"
                "${CMAKE_BINARY_DIR}/flashed4" VERBATIM)
elseif(FIX_EXTRA STREQUAL "gated_undeclared")
    # Waits for the gate -- by accident, through flash -- but is not named.
    add_custom_target(flash5
        COMMAND "${CMAKE_COMMAND}" -E copy "${_img}" "${CMAKE_BINARY_DIR}/flashed5"
        DEPENDS shell VERBATIM)
    add_dependencies(flash5 flash)
elseif(FIX_EXTRA STREQUAL "declares_nothing")
    # On DELIVERY (FIX_DELIVERY), but uses no artifact of the firmware.
    add_custom_target(noop_deliver COMMAND "${CMAKE_COMMAND}" -E true)
elseif(FIX_EXTRA STREQUAL "hidden_path")
    # [!] THE DOCUMENTED BLIND SPOT: the path is computed inside a script, and
    # the only link to the firmware is a target-level (order-only) dependency.
    add_custom_target(flash6
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/hidden.py"
                "${CMAKE_BINARY_DIR}" flashed6 VERBATIM)
    add_dependencies(flash6 shell)
elseif(FIX_EXTRA STREQUAL "late")
    # A target created after the gate's own end-of-configure step, so it was
    # never given the check as a dependency.
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
                   add_custom_target late_target
                   COMMAND "${CMAKE_COMMAND}" -E copy "${_img}"
                           "${CMAKE_BINARY_DIR}/flashed_late")
elseif(FIX_EXTRA STREQUAL "amp")
    # (issue #126 review) a shell separator right after the name.
    add_custom_target(flash_amp
        COMMAND sh -c "x=shell.elf&&cp \$x flashed8"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" VERBATIM)
    add_dependencies(flash_amp shell)
elseif(FIX_EXTRA STREQUAL "brace")
    # (issue #126 review) the name in braces, as an OpenOCD script takes it.
    add_custom_target(flash_brace
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/brace.py"
                "program {shell.elf} verify" flashed9
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" VERBATIM)
    add_dependencies(flash_brace shell)
elseif(FIX_EXTRA STREQUAL "short_opt")
    # (issue #149) the name glued to a short option, as `dfu-util -Dshell.bin`
    # takes it: the only mention of the image is `-Dshell.elf`.
    add_custom_target(flash_opt
        COMMAND "${Python3_EXECUTABLE}" -c
                "import sys,shutil;shutil.copy(sys.argv[1][2:],sys.argv[2])"
                -Dshell.elf flashed11
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" VERBATIM)
elseif(FIX_EXTRA STREQUAL "cluster")
    # (issue #149 review) the name glued to the last option of a cluster, as
    # getopt parses `dfu-util -vDshell.bin`: `-v -D shell.bin`.
    add_custom_target(flash_cluster
        COMMAND "${Python3_EXECUTABLE}" -c
                "import sys,shutil;shutil.copy(sys.argv[1][3:],sys.argv[2])"
                -vDshell.elf flashed12
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" VERBATIM)
elseif(FIX_EXTRA MATCHES "^copy_alias")
    # (issue #126 review, Grove's image_gen shape) the firmware's own
    # POST_BUILD leaves a copy under another name, and a new target ships the
    # copy.  Declared as a BYPRODUCT, the copy is a firmware artifact.
    set(_alias "${CMAKE_BINARY_DIR}/image_gen/output.img")
    if(FIX_EXTRA STREQUAL "copy_alias")
        set(_by BYPRODUCTS "${_alias}")
    else()
        set(_by "")
    endif()
    add_custom_command(TARGET shell POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/image_gen"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:shell>" "${_alias}"
        ${_by} VERBATIM)
    add_custom_target(flash_alias
        COMMAND "${CMAKE_COMMAND}" -E copy "${_alias}"
                "${CMAKE_BINARY_DIR}/flashed10"
        VERBATIM)
    add_dependencies(flash_alias shell)
endif()

# The board's stack table (issue #126): the allowances reach the policy TU in
# fw_objs, and the read-back holds the image's policy to these rows.
plugin_stack_table(
    DEFINE_ON  fw_objs
    ALLOWANCES FIX_STACK_A=${FIX_A} FIX_STACK_B=${FIX_B}
    SLOTS      entry=FIX_STACK_A shapes_ok=FIX_STACK_A decode=FIX_STACK_A
               draw=FIX_STACK_B report=FIX_STACK_A param_set=FIX_STACK_A
               param_get=FIX_STACK_A)

# An asset with no dependency of its own: only the helper's name match stands
# between it and an unchecked firmware.
add_custom_target(asset-bare
    COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/bare.nnc" VERBATIM)
# An asset whose pack rule names the gate, as cmake/add_asset.cmake does.
get_property(_gate GLOBAL PROPERTY VENEER_GATE_TARGET)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/packed.nnc"
    COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/packed.nnc"
    DEPENDS ${_gate} VERBATIM)
add_custom_target(asset-packed DEPENDS "${CMAKE_BINARY_DIR}/packed.nnc")
# The c an asset hands the host container verifier, as cmake/add_asset.cmake
# takes it (issue #111).
veneer_cost_gate_declared(_cost)
add_custom_target(asset-cost
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/record.py"
            "${_cost}" "${CMAKE_BINARY_DIR}/asset_cost.txt"
    VERBATIM)

# (issue #124) A real plugin, built by the real add_plugin(): its link + gate
# is the second user of cmake/thumb_stack_analysis.py.  wio's facts, which this
# fixture's Cortex-M7 toolchain matches; the allowances are FIX_A / FIX_B.
if(FIX_PLUGIN)
    include("${CMAKE_SOURCE_DIR}/cmake/add_plugin.cmake")
    set(_parch -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)
    add_plugin(cifar10
        CFLAGS ${_parch} -Os -std=c11 -ffreestanding -fno-builtin -fno-common
               -ffunction-sections -fdata-sections -fno-stack-protector
               -fno-unwind-tables -fno-asynchronous-unwind-tables
               -fstack-usage
               -I "@SVC@" -I "@SVC@/nn" -I "@SVC@/plugin" -I "@SVC@/camera" -I "@SVC@/blob"
               -I "${CMAKE_SOURCE_DIR}/asset/common"
        ARCH_FLAGS ${_parch}
        MEMORY_LD "${CMAKE_SOURCE_DIR}/plugin_memory.ld"
        IMAGE_BASE 0x24048000
        IMAGE_END  0x24050000
        FORBIDDEN  HAL_FLASH_Program
        VENEER_BASE_COST ${DECLARED}
        TARGET_ID  0x1201
        OUT_DIR "${CMAKE_BINARY_DIR}/plugin"
        OUT_VAR FIX_PLUGIN_ELFS
        SLOTS   entry shapes_ok decode draw report
        SBUF_WRITE_MAX 64)
    add_custom_target(plugin ALL DEPENDS ${FIX_PLUGIN_ELFS})
    # The plugin's container, as cmake/add_asset.cmake wires one: it waits for
    # the plugin's image AND the firmware's check.
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/plugin.nnc"
        COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/plugin.nnc"
        DEPENDS ${FIX_PLUGIN_ELFS} ${_gate} VERBATIM)
    add_custom_target(asset-plugin DEPENDS "${CMAKE_BINARY_DIR}/plugin.nnc")
endif()
"""


class Fail(Exception):
    pass


class Project:
    def __init__(self, work, cmake_dir, cc):
        self.src = os.path.join(work, "src")
        self.bld = os.path.join(work, "b")
        os.makedirs(os.path.join(self.src, "cmake"))
        for f in COPIED:
            shutil.copy(os.path.join(cmake_dir, f),
                        os.path.join(self.src, "cmake", f))
        for name, text in SOURCES.items():
            with open(os.path.join(self.src, name), "w") as fh:
                fh.write(text)
        for sub in ASSET_COPIED:
            shutil.copytree(os.path.join(REPO, "asset", sub),
                            os.path.join(self.src, "asset", sub),
                            ignore=shutil.ignore_patterns("*.tflite",
                                                          "__pycache__"))
        with open(os.path.join(self.src, "plugin_memory.ld"), "w") as fh:
            fh.write("MEMORY { PLUGIN (rwx) : ORIGIN = 0x24048000, "
                     "LENGTH = 32K }\n")
        with open(os.path.join(self.src, "fix.ld"), "w") as fh:
            fh.write(LINKER_SCRIPT)
        prebuilt = os.path.dirname(os.path.dirname(cc))
        with open(os.path.join(self.src, "CMakeLists.txt"), "w") as fh:
            fh.write(CMAKELISTS.replace("@PYTHON@", sys.executable)
                     .replace("@PREBUILT@", prebuilt)
                     .replace("@SVC@", os.path.join(REPO, "svc")))
        with open(os.path.join(work, "toolchain.cmake"), "w") as fh:
            fh.write(
                "set(CMAKE_SYSTEM_NAME Generic)\n"
                "set(CMAKE_SYSTEM_PROCESSOR arm)\n"
                'set(CMAKE_C_COMPILER "%s")\n'
                "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
                'set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m7 -mthumb '
                "-mfpu=fpv5-d16 -mfloat-abi=hard -Os -ffreestanding "
                "-fno-builtin -ffunction-sections -fno-unwind-tables "
                '-fno-asynchronous-unwind-tables")\n'
                'set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -nostartfiles '
                '-Wl,--no-warn-rwx-segments")\n' % cc)
        self.toolchain = os.path.join(work, "toolchain.cmake")
        self.c_init = ("-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 "
                       "-mfloat-abi=hard -Os -ffreestanding -fno-builtin "
                       "-ffunction-sections -fno-unwind-tables "
                       "-fno-asynchronous-unwind-tables")
        self.cc = cc
        self.stamp = os.path.join(self.bld, "veneer_cost", "shell.checked")
        self.elf = os.path.join(self.bld, "shell.elf")

    def configure(self, **defs):
        cmd = ["cmake", "-G", "Ninja", "-S", self.src, "-B", self.bld,
               "-DCMAKE_TOOLCHAIN_FILE=" + self.toolchain]
        cmd += ["-D%s=%s" % kv for kv in defs.items()]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail("configure failed:\n" + r.stdout[-1500:]
                       + r.stderr[-1500:])

    def ninja(self, *targets):
        r = subprocess.run(["ninja", "-C", self.bld] + list(targets),
                           capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def path(self, name):
        return os.path.join(self.bld, name)

    def rm(self, name):
        p = self.path(name)
        if os.path.exists(p):
            os.remove(p)

    def records(self):
        found = []
        for root, _, files in os.walk(self.bld):
            found += [f for f in files if f.endswith(".su")]
        return found


def expect(cond, why, out=""):
    if not cond:
        raise Fail(why + ("\n" + out[-2000:] if out else ""))


def make_edit_visible(p, path):
    """Make an edit of `path` visible to ninja before the next build.

    ninja reruns an edge only when an input is NEWER than its output, and an
    equal mtime is "up to date".  File times here come from a clock that ticks
    once per jiffy (1 ms on the host this was found on), so an edit made within
    the same tick as the last thing ninja wrote -- the stamp, a few ms earlier --
    can carry the same time, and the next `ninja` then says "no work to do"
    (issue #204: script_change failed that way once).  So after the edit the
    file is re-touched with the CURRENT time until it is strictly newer than
    everything in the build directory.  Never a time in the future: a script
    dated ahead of the stamp its rerun writes would stay dirty, and the "a
    second build does nothing" cases would fail instead.
    """
    newest = 0
    for root, _, files in os.walk(p.bld):
        for f in files:
            try:
                newest = max(newest,
                             os.stat(os.path.join(root, f)).st_mtime_ns)
            except OSError:
                pass
    deadline = time.monotonic() + 5
    while os.stat(path).st_mtime_ns <= newest:
        if time.monotonic() > deadline:
            raise Fail("make_edit_visible: %s is still not newer than "
                       "the build directory after 5 s" % path)
        time.sleep(0.002)
        os.utime(path)


def ran_check(out):
    return "check_veneer_base_cost: " in out


def derived_of(out):
    m = re.search(r"VENEER_BASE_COST (\d+) B >= (\d+) B derived", out)
    if not m:
        raise Fail("no OK line to read the derived value from:\n" + out[-1500:])
    return int(m.group(2))


def main_tree(p, results):
    """The cases that run in sequence on one tree."""
    p.configure(DECLARED="4096")

    rc, out = p.ninja("shell")
    expect(rc == 0, "firmware_alone: `ninja shell` failed", out)
    expect(os.path.exists(p.elf) and not os.path.exists(p.stamp),
           "firmware_alone: the image should exist and the stamp should not",
           out)
    results.append(("firmware_alone", "the image links; no stamp yet"))

    rc, out = p.ninja("flash")
    expect(rc == 0, "flash_waits: `ninja flash` failed", out)
    expect(ran_check(out) and os.path.exists(p.stamp)
           and os.path.exists(p.path("flashed")),
           "flash_waits: flash should have run the check, then delivered",
           out)
    results.append(("flash_waits", "the check ran before the flash command"))

    rc, out = p.ninja()
    expect(rc == 0, "noop: `ninja` failed", out)
    elf_mtime = os.path.getmtime(p.elf)
    rc, out = p.ninja()
    expect(rc == 0 and not ran_check(out) and "no work to do" in out,
           "noop: a second `ninja` should do nothing", out)
    results.append(("noop", "a second `ninja` reruns nothing"))

    recs = p.records()
    for tu in ("root.c.su", "objsrc.c.su", "objlink.c.su", "lib.c.su"):
        expect(any(r.endswith(tu) for r in recs),
               "witnesses: no record for %s (records: %s)" % (tu, recs))
    expect(not any(r.endswith("other.c.su") for r in recs),
           "witnesses: other.c was instrumented (records: %s)" % recs)
    results.append(("witnesses", "all four TUs of the image have a record, "
                                 "other.c none"))

    for script in ("check_veneer_base_cost.py", "thumb_stack_analysis.py",
                   "check_plugin_image.py"):
        with open(os.path.join(p.src, "cmake", script), "a") as fh:
            fh.write("\n# touched by the build test\n")
        make_edit_visible(p, os.path.join(p.src, "cmake", script))
        rc, out = p.ninja()
        expect(rc == 0 and ran_check(out),
               "script_change: editing %s did not rerun the check" % script,
               out)
        expect(os.path.getmtime(p.elf) == elf_mtime,
               "script_change: editing %s relinked the image" % script, out)
    results.append(("script_change", "each of its scripts reruns the check, "
                                     "no relink"))

    with open(os.path.join(p.src, "root.c"), "a") as fh:
        fh.write("\nint image_change_marker(void) { return 3; }\n")
    make_edit_visible(p, os.path.join(p.src, "root.c"))
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out)
           and os.path.getmtime(p.elf) != elf_mtime,
           "image_change: a relinked image did not rerun the check", out)
    results.append(("image_change", "a relinked image reruns the check"))

    # The derived value, from a run of the check itself.
    os.remove(p.stamp)
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out), "failure: could not rerun the check",
           out)
    derived = derived_of(out)

    p.configure(DECLARED=str(derived - 1))
    for f in ("flashed", "bare.nnc", "packed.nnc"):
        p.rm(f)
    rc, out = p.ninja()
    flat = " ".join(out.split())
    expect(rc != 0 and "[budget]" in flat
           and ("needs %d B below it (" % derived) in flat
           and ("declares %d B" % (derived - 1)) in flat,
           "failure: the build should fail in [budget] naming the chain", out)
    expect(not os.path.exists(p.stamp),
           "failure: a stamp survived the failed check", out)
    for target, made in (("flash", "flashed"), ("asset-bare", "bare.nnc"),
                         ("asset-packed", "packed.nnc")):
        rc, out = p.ninja(target)
        expect(rc != 0 and not os.path.exists(p.path(made)),
               "failure: `ninja %s` proceeded past the failed check"
               % target, out)
    expect(not os.path.exists(p.stamp),
           "failure: a stamp appeared after the failed check")
    results.append(("failure", "DECLARED %d < %d: build fails naming the "
                               "chain, no stamp, flash and both assets "
                               "stopped" % (derived - 1, derived)))

    p.configure(DECLARED=str(derived))
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp),
           "recovery: the derived value should pass again", out)
    rc, out = p.ninja("flash")
    expect(rc == 0 and os.path.exists(p.path("flashed")),
           "recovery: flash should proceed", out)
    results.append(("recovery", "DECLARED %d: stamp back, flash proceeds"
                                % derived))

    # [!] A STAMP MUST NOT OUTLIVE THE IMAGE IT DESCRIBES.  Building the image
    # alone -- `ninja shell.img` on a board, anything that needs the ELF but
    # not the check -- relinks without checking, and before this the previous
    # run's passing stamp stayed behind, describing an image that no longer
    # existed.  Delivery still waited for the check, so nothing unchecked
    # shipped; what was wrong is that the stamp went on asserting a pass.
    with open(os.path.join(p.src, "root.c"), "a") as fh:
        fh.write("\nint relink_marker(void) { return 4; }\n")
    make_edit_visible(p, os.path.join(p.src, "root.c"))
    before = os.path.getmtime(p.elf)
    rc, out = p.ninja("shell")
    expect(rc == 0 and os.path.getmtime(p.elf) != before,
           "relink_retracts_stamp: the image should have been relinked", out)
    expect(not ran_check(out),
           "relink_retracts_stamp: `ninja shell` should not run the check "
           "(if it does, this case no longer tests anything)", out)
    expect(not os.path.exists(p.stamp),
           "relink_retracts_stamp: a passing stamp survived a relink that "
           "did not re-run the check", out)
    rc, out = p.ninja("flash")
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp),
           "relink_retracts_stamp: the next delivery should check and stamp "
           "again", out)
    results.append(("relink_retracts_stamp",
                    "`ninja shell` alone relinks and leaves no stamp"))


def asset_tree(p, target, made, results, name):
    p.configure(DECLARED="4096")
    rc, out = p.ninja(target)
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp)
           and os.path.exists(p.path(made)),
           "%s: `ninja %s` from a fresh tree should run the check first"
           % (name, target), out)
    results.append((name, "`ninja %s` from nothing ran the check first"
                          % target))


PLUGIN_GATE = "plugin cifar10: ld + gate"
PLUGIN_FAIL = "check_plugin_image: FAIL"
REFUSAL_MSG = "refused by the build test's edit of the shared module"
# A refusal that only the shared module makes, in the analysis of one body --
# the part BOTH gates call (step 2 of #124), so each reports it as one of its
# own refusals: the firmware's check below a veneer, the plugin gate for every
# body of the plugin.
REFUSAL = """

# --- appended by run_veneer_gate_build_tests.py (shared_analysis) ---
_fixture_analyse = Image.analyse


def _fixture_refusing_analyse(self, node):
    info = _fixture_analyse(self, node)
    msg = (node.addr, "%s")
    if msg not in info.flow_errs:
        info.flow_errs.append(msg)
    return info


Image.analyse = _fixture_refusing_analyse
""" % REFUSAL_MSG


def shared_tree(p, results):
    """(issue #124) thumb_stack_analysis.py is an input of BOTH gates."""
    p.configure(DECLARED="1024", FIX_A="2048", FIX_B="2048", FIX_PLUGIN="ON")
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out) and PLUGIN_GATE in out
           and os.path.exists(p.stamp),
           "shared_analysis: the first build should run both gates", out)
    rc, out = p.ninja()
    expect(rc == 0 and "no work to do" in out and not ran_check(out)
           and PLUGIN_GATE not in out,
           "shared_analysis: with nothing edited, nothing should rerun", out)
    elf_mtime = os.path.getmtime(p.elf)
    mod = os.path.join(p.src, "cmake", "thumb_stack_analysis.py")
    with open(mod) as fh:
        pristine = fh.read()

    with open(mod, "a") as fh:
        fh.write("\n# touched by the build test\n")
    make_edit_visible(p, mod)
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out) and PLUGIN_GATE in out
           and os.path.exists(p.stamp),
           "shared_analysis: editing the shared module alone should rerun "
           "both gates", out)
    expect(os.path.getmtime(p.elf) == elf_mtime,
           "shared_analysis: editing the shared module relinked the "
           "firmware", out)
    rc, out = p.ninja()
    expect(rc == 0 and "no work to do" in out,
           "shared_analysis: a second build after the edit should do "
           "nothing", out)
    results.append(("shared_analysis", "an edit of thumb_stack_analysis.py "
                                       "alone reruns both gates, no "
                                       "firmware relink; no edit, no rerun"))

    for f in ("flashed", "bare.nnc", "packed.nnc", "plugin.nnc"):
        p.rm(f)
    with open(mod, "w") as fh:
        fh.write(pristine + REFUSAL)
    make_edit_visible(p, mod)
    # -k 0: the two gates are independent edges, and ninja would otherwise
    # stop at whichever failed first -- this case is about BOTH.
    rc, out = p.ninja("-k", "0")
    flat = " ".join(out.split())
    expect(rc != 0 and "check_veneer_base_cost: FAIL" in flat
           and ("[walk] " in flat and REFUSAL_MSG in flat),
           "shared_refusal: a refusal in the shared module should fail the "
           "firmware's check, for that reason", out)
    expect(PLUGIN_FAIL in flat
           and re.search(r"check_plugin_image: FAIL\s+- stack: \S+: "
                         + re.escape(REFUSAL_MSG), out),
           "shared_refusal: a refusal in the shared module should fail the "
           "plugin gate too, for that reason", out)
    expect(not os.path.exists(p.stamp),
           "shared_refusal: a stamp survived the refusal", out)
    for target, made in (("flash", "flashed"), ("asset-bare", "bare.nnc"),
                         ("asset-packed", "packed.nnc"),
                         ("asset-plugin", "plugin.nnc")):
        rc, out = p.ninja(target)
        expect(rc != 0 and not os.path.exists(p.path(made)),
               "shared_refusal: `ninja %s` proceeded past the refusal"
               % target, out)
    expect(not os.path.exists(p.stamp),
           "shared_refusal: a stamp appeared after the refusal")

    with open(mod, "w") as fh:
        fh.write(pristine)
    make_edit_visible(p, mod)
    rc, out = p.ninja("asset-plugin", "flash")
    expect(rc == 0 and os.path.exists(p.stamp)
           and os.path.exists(p.path("plugin.nnc"))
           and os.path.exists(p.path("flashed")),
           "shared_refusal: the module restored, flash and the plugin's "
           "asset should proceed", out)
    results.append(("shared_refusal", "a refusal added to the shared module "
                                      "alone: both gates fail for it, no "
                                      "stamp, flash and all three assets "
                                      "stopped; restored, they proceed"))


def lto_tree(p, results):
    p.configure(DECLARED="4096", FIX_LTO="max")
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out), "ltrans_drop: the max link failed",
           out)
    many = sorted(r for r in p.records() if ".ltrans" in r)
    expect(len(many) >= 2, "ltrans_drop: -flto-partition=max wrote %d "
                           "partition records" % len(many), out)
    p.configure(DECLARED="4096", FIX_LTO="one")
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp),
           "ltrans_drop: max -> one should still pass", out)
    one = sorted(r for r in p.records() if ".ltrans" in r)
    expect(len(one) == 1, "ltrans_drop: %d partition records after the "
                          "one-partition link: %s" % (len(one), one), out)
    results.append(("ltrans_drop", "%d partition records -> 1, and the check "
                                   "passes" % len(many)))


def probe_fail(out):
    return " ".join(out.split())


def stack_tree(p, results):
    """The board's table -> the policy TU -> the read-back (issue #126)."""
    seen = []
    for b in ("500", "600"):
        p.configure(DECLARED="4096", FIX_A="300", FIX_B=b)
        p.rm("flashed")
        rc, out = p.ninja("flash")
        flat = probe_fail(out)
        expect(rc == 0 and os.path.exists(p.path("flashed"))
               and "check_policy_probe: OK" in flat
               and ("(entry 300, shapes_ok 300, decode 300, draw %s, "
                    "report 300, param_set 300, param_get 300 B)" % b) in flat,
               "stack_wiring: FIX_B=%s should pass and read back draw %s"
               % (b, b), out)
        seen.append(b)
    results.append(("stack_wiring", "the table reached the policy TU and the "
                                    "read-back saw draw %s"
                                    % " -> ".join(seen)))

    # [!] AN OVERRIDE THAT COMPILES, in both directions.  The helper's -D comes
    # first on the command line, so CMAKE_C_FLAGS wins with only a warning:
    # the image holds a limit nothing on the host side used.
    for name, flag, slot, held in (
            ("stack_override_down", "-DFIX_STACK_B=100u", "draw", 100),
            ("stack_override_up", "-DFIX_STACK_A=4000u", "entry", 4000)):
        for f in ("flashed", "packed.nnc"):
            p.rm(f)
        p.configure(DECLARED="4096", FIX_A="300", FIX_B="600",
                    CMAKE_C_FLAGS=p.c_init + " " + flag)
        rc, out = p.ninja("shell")
        expect(rc == 0, "%s: the overridden image should still link" % name,
               out)
        for target, made in (("flash", "flashed"),
                             ("asset-packed", "packed.nnc")):
            rc, out = p.ninja(target)
            flat = probe_fail(out)
            expect(rc != 0 and not os.path.exists(p.path(made))
                   and "check_policy_probe: FAIL" in flat
                   and ("slot %s holds a stack limit of %d B" % (slot, held))
                   in flat,
                   "%s: `ninja %s` should fail in check_policy_probe naming "
                   "slot %s" % (name, target, slot), out)
        expect(not os.path.exists(p.stamp),
               "%s: a stamp survived the refused probe" % name)
        results.append((name, "CMAKE_C_FLAGS %s links; flash and the asset "
                              "stop: slot %s holds %d B" % (flag, slot, held)))

    # Every slot is compared: each one, nudged by a byte on its own, is named.
    names = ("entry", "shapes_ok", "decode", "draw", "report", "param_set",
             "param_get")
    for i, slot in enumerate(names):
        p.rm("flashed")
        p.configure(DECLARED="4096", FIX_A="300", FIX_B="600",
                    CMAKE_C_FLAGS=p.c_init + " -DFIX_BUMP_SLOT=%d" % i)
        rc, out = p.ninja("flash")
        flat = probe_fail(out)
        want = 601 if slot == "draw" else 301
        expect(rc != 0 and not os.path.exists(p.path("flashed"))
               and ("slot %s holds a stack limit of %d B" % (slot, want))
               in flat
               and flat.count("holds a stack limit") == 1,
               "stack_every_slot: slot %s off by one byte should be refused, "
               "and only it named" % slot, out)
    results.append(("stack_every_slot", "each of the 7 slots, off by one "
                                        "byte on its own, is refused by name"))

    # The firmware's own mapping disagreeing with the table.
    p.rm("flashed")
    p.configure(DECLARED="4096", FIX_A="300", FIX_B="600",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_POLICY_MISMAP")
    rc, out = p.ninja("flash")
    flat = probe_fail(out)
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and "slot draw holds a stack limit of 300 B" in flat
           and "declares 600 B" in flat,
           "stack_mismap: the firmware mapping draw to A should be refused",
           out)
    results.append(("stack_mismap", "draw mapped to the other allowance: "
                                    "300 held, 600 declared, refused"))

    # [!] AND WHAT IT CANNOT SEE.  The same mismapping with equal values: the
    # device enforces exactly the table's numbers, so nothing differs to read.
    p.configure(DECLARED="4096", FIX_A="300", FIX_B="300",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_POLICY_MISMAP")
    rc, out = p.ninja("flash")
    expect(rc == 0 and "check_policy_probe: OK" in out,
           "stack_mismap_equal: expected to pass (equal values), which is the "
           "documented limit; if this now fails, update the docs", out)
    results.append(("stack_mismap_equal", "the same mismapping with equal "
                                          "allowances passes (documented "
                                          "limit)"))

    # (issue #126 review) a policy in a writable section: the image holds only
    # its initial value, so the read-back refuses it.
    p.rm("flashed")
    p.configure(DECLARED="4096", FIX_A="300", FIX_B="600",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_POLICY_RW")
    rc, out = p.ninja("flash")
    flat = probe_fail(out)
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and "a writable section" in flat,
           "stack_writable_policy: a non-const policy should be refused", out)
    results.append(("stack_writable_policy", "a policy in .data is refused: "
                    "the image holds only its initial value"))

    # The probe's own refusals of the rows it is handed, on this image.
    p.configure(DECLARED="4096", FIX_A="300", FIX_B="600",
                CMAKE_C_FLAGS=p.c_init)
    rc, out = p.ninja("flash")
    expect(rc == 0, "stack_rows: the clean tree should pass", out)
    probe = os.path.join(p.src, "cmake", "check_policy_probe.py")
    full = ["entry=300", "shapes_ok=300", "decode=300", "draw=600",
            "report=300", "param_set=300", "param_get=300"]
    for rows, says in ((full[:-1], "has no row for param_get"),
                       (full + ["preview=300"], "--stack-limit 'preview=300'"),
                       (full[:3] + ["draw=0"] + full[4:],
                        "--stack-limit 'draw=0'"),
                       (full + ["draw=600"], "names slot draw twice")):
        cmd = [sys.executable, probe, p.elf, "--declared", "4096"]
        for r in rows:
            cmd += ["--stack-limit", r]
        r = subprocess.run(cmd, capture_output=True, text=True)
        expect(r.returncode != 0 and says in r.stdout + r.stderr,
               "stack_rows: the probe should refuse (%s)" % says,
               r.stdout + r.stderr)
    cmd = [sys.executable, probe, p.elf, "--declared", "4096"]
    for r in full:
        cmd += ["--stack-limit", r]
    r = subprocess.run(cmd, capture_output=True, text=True)
    expect(r.returncode == 0, "stack_rows: the full table should pass",
           r.stdout + r.stderr)
    results.append(("stack_rows", "a missing slot, an unknown slot, a 0 and "
                                  "a duplicate are refused; the full table "
                                  "passes"))


def delivery_tree(p, results):
    """issue #126 (P14): DELIVERY is checked against the build graph."""
    def dcheck(out):
        return " ".join(out.split())

    p.configure(DECLARED="4096", FIX_EXTRA="", FIX_DELIVERY="flash")
    rc, out = p.ninja()
    expect(rc == 0 and "check_delivery_gate: OK" in out,
           "delivery_clean: the plain tree should pass the delivery check", out)
    results.append(("delivery_clean", next(
        ln.strip() for ln in out.splitlines()
        if "check_delivery_gate: OK" in ln)[:110] + "..."))

    for extra, target, made, says in (
            ("undeclared", "flash2", "flashed2",
             "depends on shell.elf and does not wait for"),
            ("by_command", "flash3", "flashed3",
             "names shell.elf in its command and does not wait for"),
            ("post_build_other", "other", "flashed4",
             "names shell.elf in its POST_BUILD and does not wait for"),
            ("gated_undeclared", "flash5", "flashed5",
             "target 'flash5' uses the firmware"),
            ("amp", "flash_amp", "flashed8",
             "names shell.elf in its command and does not wait for"),
            ("brace", "flash_brace", "flashed9",
             "names shell.elf in its command and does not wait for"),
            ("copy_alias", "flash_alias", "flashed10",
             "names output.img in its command and does not wait for"),
            ("short_opt", "flash_opt", "flashed11",
             "names shell.elf in its command and does not wait for"),
            ("cluster", "flash_cluster", "flashed12",
             "names shell.elf in its command and does not wait for"),
    ):
        p.configure(DECLARED="4096", FIX_EXTRA=extra, FIX_DELIVERY="flash")
        p.rm(made)
        rc, out = p.ninja(target)
        flat = dcheck(out)
        expect(rc != 0 and not os.path.exists(p.path(made))
               and "check_delivery_gate: FAIL" in flat and says in flat,
               "delivery_%s: `ninja %s` should stop in the delivery check, "
               "before it delivers" % (extra, target), out)
        line = next(ln.strip() for ln in out.splitlines()
                    if ("uses the firmware" in ln or "does not wait" in ln))
        results.append(("delivery_" + extra, "`ninja %s` refused before it "
                        "ran: %s" % (target, line[:90])))

    # The other direction: DELIVERY names a target that delivers nothing.
    p.configure(DECLARED="4096", FIX_EXTRA="declares_nothing",
                FIX_DELIVERY="flash;noop_deliver")
    rc, out = p.ninja()
    flat = dcheck(out)
    expect(rc != 0 and "DELIVERY names 'noop_deliver', which uses no firmware "
                       "artifact" in flat,
           "delivery_declares_nothing: should be refused", out)
    results.append(("delivery_declares_nothing", "DELIVERY flash;noop_deliver "
                    "refused: noop_deliver uses no firmware artifact"))

    # A target deferred past the wiring would not wait for the check.  The
    # gate refuses the configure, and the target -- invoked DIRECTLY, from the
    # tree the last good configure left -- cannot run (issue #126 review: the
    # first version only named it on a build that ran the check, and
    # `ninja late_target` delivered).
    try:
        p.configure(DECLARED="4096", FIX_EXTRA="late", FIX_DELIVERY="flash")
        cfg = None
    except Fail as e:
        cfg = str(e)
    expect(cfg is not None and "deferred call(s) still pending after the "
           "gate's end-of-configure wiring" in " ".join(cfg.split()),
           "delivery_late: the configure should be refused", cfg or "")
    p.rm("flashed_late")
    rc, out = p.ninja("late_target")
    expect(rc != 0 and not os.path.exists(p.path("flashed_late")),
           "delivery_late: `ninja late_target` should not run", out)
    results.append(("delivery_late", "a target deferred past the wiring: "
                    "configure refused, `ninja late_target` does not run"))
    p.configure(DECLARED="4096", FIX_EXTRA="", FIX_DELIVERY="flash")
    rc, out = p.ninja()
    expect(rc == 0, "delivery_late: the tree should recover", out)

    # Two rules that no CMake input left can reach on its own -- since the
    # deferred call above is refused -- tested on an edited copy of the graph:
    # rule 4 reads the targets from build.ninja as well as from the list, and
    # rule 1 holds for an edge no target owns (so rule 2 says nothing).
    def edited_check(extra):
        edited = p.path("edited.ninja")
        with open(p.path("build.ninja")) as fh, open(edited, "w") as ofh:
            ofh.write(fh.read() + "\n" + extra)
        r = subprocess.run(
            [sys.executable,
             os.path.join(p.src, "cmake", "check_delivery_gate.py"),
             "--ninja", edited, "--firmware-target", "shell",
             "--gate-stamp", p.stamp, "--gate-target", "veneer_cost_check",
             "--self-stamp", p.path("veneer_cost/delivery_graph.checked"),
             "--self-target", "delivery_gate_check",
             "--targets", p.path("veneer_cost/targets.txt"),
             "--delivery", "flash", "--by-name", "asset-bare",
             "asset-packed", "asset-cost"], capture_output=True, text=True)
        return r.returncode, " ".join((r.stdout + r.stderr).split())
    rc, flat = edited_check("build rogue_late: phony CMakeFiles/rogue\n"
                            "build CMakeFiles/rogue: CUSTOM_COMMAND\n"
                            "  COMMAND = true\n")
    expect(rc != 0 and "target 'rogue_late' does not wait for this check"
           in flat, "delivery_rule4_graph: an unwired target should be named",
           flat)
    results.append(("delivery_rule4_graph", "a phony target in the graph "
                    "that no wiring reached is named"))
    rc, flat = edited_check("build orphan.bin: CUSTOM_COMMAND\n"
                            "  COMMAND = cp shell.elf orphan.bin\n")
    expect(rc != 0 and "orphan.bin" in flat and "does not wait for" in flat
           and "uses the firmware" not in flat,
           "delivery_rule1_graph: an unowned edge using the image should "
           "fail rule 1 alone", flat)
    results.append(("delivery_rule1_graph", "an edge no target owns that "
                    "copies the image fails rule 1 alone"))

    # [!] AND A COPY NOBODY DECLARED IS NOT AN ARTIFACT: the same alias without
    # BYPRODUCTS gets through -- the documented limit, stated as a case.
    p.configure(DECLARED="4096", FIX_EXTRA="copy_alias_undeclared",
                FIX_DELIVERY="flash")
    p.rm("flashed10")
    rc, out = p.ninja("flash_alias")
    expect(rc == 0 and os.path.exists(p.path("flashed10")),
           "delivery_copy_undeclared: expected to get through (documented "
           "limit); if this is now refused, update the docs", out)
    results.append(("delivery_copy_undeclared", "a copy the POST_BUILD does "
                    "not declare gets through (documented limit)"))

    # [!] AND WHAT IT CANNOT SEE: a script that finds the image itself, with
    # only a target-level dependency on the firmware.  This one DELIVERS --
    # stated as a case so the limit is written down, not discovered.
    p.configure(DECLARED="4096", FIX_EXTRA="hidden_path", FIX_DELIVERY="flash")
    p.rm("flashed6")
    rc, out = p.ninja("flash6")
    expect(rc == 0 and os.path.exists(p.path("flashed6")),
           "delivery_hidden_path: expected to get through (the documented "
           "limit); if this is now refused, update the docs", out)
    results.append(("delivery_hidden_path", "a script that computes the path "
                    "itself gets through (documented limit)"))
    p.configure(DECLARED="4096", FIX_EXTRA="", FIX_DELIVERY="flash")


def _image_words(p, sym, count):
    """The first @p count 32-bit words of @p sym, as the shipped image holds
    them, and the address of every symbol (the copied analysis' ELF reader)."""
    spec = importlib.util.spec_from_file_location(
        "thumb_stack_analysis_under_test",
        os.path.join(p.src, "cmake", "thumb_stack_analysis.py"))
    tsa = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(tsa)
    elf = tsa.Elf(p.elf)
    addr = {s[0]: s[1] for s in elf.symbols if s[0]}
    a = addr[sym]
    for sec in elf.sections:
        if sec.type != 8 and sec.size and sec.addr <= a < sec.addr + sec.size:
            o = sec.offset + a - sec.addr
            return [int.from_bytes(elf.raw[o + 4 * i:o + 4 * i + 4], "little")
                    for i in range(count)], addr
    raise Fail("%s is in no section with bytes" % sym)


# The veneer behind each member of struct plugin_bindings, by FIX_DEEP_SLOT
# index -- STATED HERE, not read from the record the check reads (issue #165).
DEEP_SLOTS = ("pl_base_log", "pl_base_to_frame", "pl_paint_rect",
              "pl_paint_fill_rect", "pl_paint_blit", "pl_print_write")


def decoy_tree(p, results):
    """issue #165: the gate reads what the loader uses.

    Both decoys passed until #165 (step 1 recorded them as documented
    limits); both are refused now, each for its own reason.
    """
    # --- bind_decoy --------------------------------------------------------
    # [!] TURNED AROUND BY STEP 3 OF #165.  The table the plugin is handed
    # binds log to a function deeper than DECLARED; until then ROOTS named the
    # shallow nn_plugin_log and the check passed.  Now the roots are the
    # table's own words, so the deep function is walked, and named.
    for i, veneer in enumerate(DEEP_SLOTS):
        name = "bind_decoy" if i == 0 else "bind_deep_" + veneer
        p.rm("flashed")
        p.configure(DECLARED="4096",
                    CMAKE_C_FLAGS=p.c_init + " -DFIX_DEEP_SLOT=%d" % i)
        rc, out = p.ninja("flash")
        flat = " ".join(out.split())
        words, addr = _image_words(p, "plugin_board_bindings", 9)
        expect(words[3 + i] == (addr["fix_deep"] | 1),
               "%s: the table's word for %s should be fix_deep (0x%08x), "
               "holds 0x%08x" % (name, veneer, addr["fix_deep"] | 1,
                                 words[3 + i]))
        expect(rc != 0 and not os.path.exists(p.path("flashed"))
               and not os.path.exists(p.stamp)
               and ("[budget] %s needs" % veneer) in flat
               and flat.count("[budget]") == 1
               and re.search(r"%s fix_deep\s+>?=?\s*\d+ B" % veneer, flat)
               and "declares 4096 B" in flat,
               "%s: the table binds %s to fix_deep, and the check should "
               "refuse that veneer alone in [budget], naming fix_deep"
               % (name, veneer), out)
    results.append(("bind_decoy", "the table binds log to a function deeper "
                                  "than DECLARED: refused in [budget] for "
                                  "pl_base_log, naming it"))
    results.append(("bind_deep_each", "each of the other five members, deep "
                                      "on its own: refused for that veneer "
                                      "alone"))

    # --- policy_decoy ------------------------------------------------------
    # [!] TURNED AROUND BY STEP 2 OF #165.  Until then the probe pointed at a
    # correct policy while the parse entry was handed this one, and passed.
    # Now the firmware's parse takes no policy, and an entry that takes one in
    # any input of the link is refused before the probe runs.
    p.rm("flashed")
    p.configure(DECLARED="4096",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_POLICY_DECOY")
    rc, out = p.ninja("flash")
    flat = " ".join(out.split())
    # The decoy is in the image: the read-back's object is right, the one the
    # stand-in entry is handed holds c = 1.  struct plugin_policy_probe:
    # magic, size, off_veneer_cost, off_accounting, off_stack_limit.
    probe, addr = _image_words(p, "plugin_policy_probe", 5)
    good, _ = _image_words(p, "plugin_board_policy", probe[1] // 4)
    used, _ = _image_words(p, "fix_policy_used", probe[1] // 4)
    expect(good[probe[2] // 4] == 4096 and used[probe[2] // 4] == 1,
           "policy_decoy: the image should hold plugin_board_policy c = 4096 "
           "and fix_policy_used c = 1 (got %d and %d)"
           % (good[probe[2] // 4], used[probe[2] // 4]))
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and not os.path.exists(p.stamp)
           and "check_policy_entry: FAIL" in flat
           and re.search(r"policy\.c\.o(bj)? defines "
                         r"plugin_parse_with_policy \(T\)", flat)
           and flat.count("plugin_parse_with_policy (") == 1
           and "check_policy_probe: " not in flat,
           "policy_decoy: the link holds an entry that takes a policy, and "
           "check_policy_entry should refuse it -- naming policy.c, before "
           "the probe runs", out)
    results.append(("policy_decoy", "an entry handed another policy (c = 1) "
                                    "while the read-back's is right: refused "
                                    "by check_policy_entry, naming the "
                                    "object that defines it"))


def entry_fail(out):
    return " ".join(out.split())


def entry_tree(p, results):
    """issue #165: no input of the firmware link takes a policy."""
    entry = os.path.join(p.src, "cmake", "check_policy_entry.py")
    nm = p.cc[:-len("gcc")] + "gcc-nm"

    def run_entry(mapfile, use_nm=nm):
        r = subprocess.run([sys.executable, entry, "--map", mapfile,
                            "--link-dir", p.bld, "--nm", use_nm],
                           capture_output=True, text=True)
        return r.returncode, " ".join((r.stdout + r.stderr).split())

    p.configure(DECLARED="4096", FIX_HOST_TU="", CMAKE_C_FLAGS=p.c_init)
    rc, out = p.ninja("flash")
    flat = entry_fail(out)
    m = re.search(r"check_policy_entry: OK -- (\d+) inputs of the firmware "
                  r"link \((\d+) objects, (\d+) archive members\)", flat)
    expect(rc == 0 and m is not None and int(m.group(3)) >= 1,
           "entry_clean: the plain tree should pass, reading at least the "
           "static library's member", out)
    results.append(("entry_clean", "the plain tree: %s inputs read (%s "
                                   "objects, %s archive members), none takes "
                                   "a policy" % m.groups()))

    # The map, edited, against the same tree: what the map says and what is
    # on disk must agree.
    with open(p.path("shell.map")) as fh:
        text = fh.read()
    for name, edit, says in (
            ("missing_object",
             lambda t: t.replace("\nLOAD ", "\nLOAD CMakeFiles/gone.c.obj\nLOAD ",
                                 1),
             "the map LOADs CMakeFiles/gone.c.obj, which cannot be read"),
            ("missing_member",
             lambda t: t.replace("Archive member included to satisfy "
                                 "reference by file (symbol)\n\n",
                                 "Archive member included to satisfy "
                                 "reference by file (symbol)\n\n"
                                 "libfw_lib.a(nothere.c.obj)\n"
                                 "                              x.o (y)\n", 1),
             "libfw_lib.a(nothere.c.obj), and libfw_lib.a holds no such "
             "member"),
            ("unloaded_archive",
             lambda t: t.replace("Archive member included to satisfy "
                                 "reference by file (symbol)\n\n",
                                 "Archive member included to satisfy "
                                 "reference by file (symbol)\n\n"
                                 "libother.a(a.c.obj)\n"
                                 "                              x.o (y)\n", 1),
             "LOADs no archive libother.a")):
        edited = edit(text)
        expect(edited != text, "entry_map_%s: the edit did not apply" % name)
        mp = p.path("edited.map")
        with open(mp, "w") as fh:
            fh.write(edited)
        rc, got = run_entry(mp)
        expect(rc != 0 and "check_policy_entry: FAIL" in got and says in got,
               "entry_map_%s: should refuse (%s)" % (name, says), got)
    results.append(("entry_map", "a LOADed object that is not on disk, a "
                                 "member its archive does not hold, a member "
                                 "of an archive not LOADed: each refused"))

    # The host's entry where it must never be.  The real host file in the
    # firmware's objects is refused by the link itself since the review of
    # #165: it includes the parse body, and so does plugin_load_board.c.
    p.rm("flashed")
    p.configure(DECLARED="4096", FIX_HOST_TU="OBJ", CMAKE_C_FLAGS=p.c_init)
    rc, out = p.ninja("flash")
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and "multiple definition" in out
           and "plugin_load_body_once" in out,
           "entry_host_obj: the host file in the firmware's objects should "
           "fail the link on the parse body's guard", out)
    # An entry under the host's name in the static library, with a caller:
    # the check reads the archive member and the reference.
    p.rm("flashed")
    p.configure(DECLARED="4096", FIX_HOST_TU="LIB",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_CALL_HOST")
    rc, out = p.ninja("flash")
    flat = entry_fail(out)
    want = [r"libfw_lib\.a\(hostlike\.c\.o(bj)?\) defines "
            r"plugin_parse_with_policy \(T\)",
            r"policy\.c\.o(bj)? references plugin_parse_with_policy \(U\)"]
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and not os.path.exists(p.stamp)
           and "check_policy_entry: FAIL" in flat
           and all(re.search(w, flat) for w in want)
           and flat.count("plugin_parse_with_policy (") == len(want),
           "entry_host_lib: should be refused naming %s" % " and ".join(want),
           out)
    results.append(("entry_host_tu", "the host file in the firmware's "
                                     "objects fails the link; an entry of "
                                     "its name in the static library, with a "
                                     "caller, is refused naming the member "
                                     "and the reference"))

    # Under LTO the inputs are IR: read through gcc-nm, and refused with a
    # plain nm rather than read as their empty wrappers.
    p.rm("flashed")
    p.configure(DECLARED="4096", FIX_HOST_TU="", FIX_LTO="max",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_POLICY_DECOY")
    rc, out = p.ninja("flash")
    flat = entry_fail(out)
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and "check_policy_entry: FAIL" in flat
           and re.search(r"policy\.c\.o(bj)? defines "
                         r"plugin_parse_with_policy", flat),
           "entry_lto: the LTO link should be refused, read as IR", out)
    rc, got = run_entry(p.path("shell.map"), p.cc[:-len("gcc")] + "nm")
    expect(rc != 0 and "check_policy_entry: FAIL" in got
           and "plugin needed" in got,
           "entry_lto: a plain nm should be refused", got)
    results.append(("entry_lto", "an LTO link with the decoy: refused, read "
                                 "through gcc-nm; with a plain nm the check "
                                 "refuses rather than read wrappers"))


def bind_shape_tree(p, results):
    """issue #165: the one table the loader hands the plugin, by name."""
    for name, flag, says in (
            ("bind_writable", "-DFIX_BIND_RW",
             "plugin_board_bindings at 0x"),
            ("bind_weak", "-DFIX_BIND_WEAK",
             "plugin_board_bindings is weak, not global"),
            ("bind_twin", "-DFIX_BIND_TWIN",
             "2 symbols named plugin_board_bindings")):
        p.rm("flashed")
        p.configure(DECLARED="4096", CMAKE_C_FLAGS=p.c_init + " " + flag)
        rc, out = p.ninja("flash")
        flat = " ".join(out.split())
        extra = (name != "bind_writable"
                 or "is in .data, a writable section" in flat)
        expect(rc != 0 and not os.path.exists(p.path("flashed"))
               and not os.path.exists(p.stamp)
               and "check_veneer_base_cost: FAIL" in flat
               and ("[bindings] " + says) in flat and extra,
               "%s: the check should refuse the table (%s)" % (name, says),
               out)
    results.append(("bind_shape", "the table in a writable section, weak, "
                                  "or with a second object of its name: "
                                  "each refused"))
    p.rm("flashed")
    p.configure(DECLARED="4096", CMAKE_C_FLAGS=p.c_init + " -DFIX_BIND_NONE")
    rc, out = p.ninja("shell")
    expect(rc != 0 and "plugin_board_bindings" in out,
           "bind_none: with no plugin_board_bindings the link should fail "
           "naming it", out)
    results.append(("bind_none", "no plugin_board_bindings: the link fails "
                                 "naming it"))

    # The pre-#165 entries: a loader that took the base (the shared one and
    # both boards' had these names), the report and painter calls that took
    # the vtable's functions.
    for flag, old in (("FIX_OLD_EXEC", "plugin_exec_load"),
                      ("FIX_OLD_RUN", "plugin_run_load"),
                      ("FIX_OLD_REPORT", "nn_active_report"),
                      ("FIX_OLD_CORE_REPORT", "nn_active_core_report"),
                      ("FIX_OLD_PAINT", "plugin_paint_vtable"),
                      ("FIX_OLD_DRAW", "nn_active_draw"),
                      ("FIX_OLD_CORE_DRAW", "nn_active_core_draw"),
                      ("FIX_OLD_PAINT_BIND", "plugin_paint_bind")):
        p.rm("flashed")
        p.configure(DECLARED="4096", CMAKE_C_FLAGS=p.c_init + " -D" + flag)
        rc, out = p.ninja("flash")
        flat = " ".join(out.split())
        expect(rc != 0 and not os.path.exists(p.path("flashed"))
               and "check_policy_entry: FAIL" in flat
               and re.search(r"old\.c\.o(bj)? defines %s \(T\)" % old, flat),
               "entry_old_%s: an object defining %s should be refused"
               % (old, old), out)
    results.append(("entry_old", "an object defining plugin_exec_load, "
                                 "plugin_run_load, nn_active_report, "
                                 "nn_active_core_report, plugin_paint_vtable, "
                                 "nn_active_draw, nn_active_core_draw or "
                                 "plugin_paint_bind (the pre-#165 entries, the "
                                 "draw's since the review): each refused"))

    # (issue #165 review) A -D on the reader's own file renames what it
    # reads; the read-back still finds the real object.  Without LTO and with.
    for lto in ("OFF", "max"):
        for what, flag, says in (
                ("policy", "-DFIX_RUNTIME_POLICY",
                 [r"plugin_load_board\.c\.o(bj)? defines plugin_parse but "
                  r"does not reference plugin_board_policy"]),
                ("bindings", "-DFIX_RUNTIME_BINDINGS",
                 [r"plugin_bindings\.c\.o(bj)? defines plugin_painter_init "
                  r"but does not reference plugin_board_bindings",
                  r"plugin_bindings\.c\.o(bj)? defines plugin_printer_init "
                  r"but does not reference plugin_board_bindings"])):
            p.rm("flashed")
            p.configure(DECLARED="4096", FIX_LTO=lto, FIX_RENAME=what,
                        CMAKE_C_FLAGS=p.c_init + " " + flag)
            rc, out = p.ninja("flash")
            flat = " ".join(out.split())
            expect(rc != 0 and not os.path.exists(p.path("flashed"))
                   and not os.path.exists(p.stamp)
                   and "check_policy_entry: FAIL" in flat
                   and all(re.search(x, flat) for x in says)
                   and "check_policy_probe: " not in flat,
                   "entry_rename_%s (LTO %s): renaming what the reader reads "
                   "should be refused" % (what, lto), out)
    p.configure(DECLARED="4096", FIX_LTO="OFF", FIX_RENAME="",
                CMAKE_C_FLAGS=p.c_init + " -DFIX_SECOND_PARSE")
    p.rm("flashed")
    rc, out = p.ninja("flash")
    flat = " ".join(out.split())
    expect(rc != 0 and not os.path.exists(p.path("flashed"))
           and "check_policy_entry: FAIL" in flat
           and "2 inputs define plugin_parse" in flat,
           "entry_second_parse: a second definition of plugin_parse should "
           "be refused", out)
    p.configure(DECLARED="4096", FIX_LTO="OFF", FIX_RENAME="",
                CMAKE_C_FLAGS=p.c_init)
    results.append(("entry_second_parse", "a second (local) plugin_parse in "
                                          "another file: refused, the entry "
                                          "must be defined once"))
    results.append(("entry_rename", "a -D on the reader's own file renaming "
                                    "the policy or the table it reads, with "
                                    "and without LTO: refused, naming the "
                                    "reader"))

    # (issue #165 review) A second firmware file including the parse body.
    for lto in ("OFF", "max"):
        p.rm("flashed")
        p.configure(DECLARED="4096", FIX_LTO=lto,
                    CMAKE_C_FLAGS=p.c_init + " -DFIX_BODY_TWICE")
        rc, out = p.ninja("shell")
        expect(rc != 0 and "plugin_load_body_once" in out
               and "multiple definition" in out,
               "body_twice (LTO %s): a second file including "
               "plugin_load_body.h should fail the link" % lto, out)
    p.configure(DECLARED="4096", FIX_LTO="OFF", CMAKE_C_FLAGS=p.c_init)
    results.append(("body_twice", "a second firmware file including "
                                  "plugin_load_body.h, with and without "
                                  "LTO: the link fails on its guard"))


def policy_shape_tree(p, results):
    """issue #165: the one object the loader reads, by name."""
    for name, flag, says in (
            ("policy_twin", "-DFIX_POLICY_TWIN",
             "2 symbols named plugin_board_policy"),
            ("policy_weak", "-DFIX_POLICY_WEAK",
             "plugin_board_policy is weak, not global"),
            ("policy_size", "-DFIX_POLICY_ARRAY",
             "plugin_board_policy is 80 B, but sizeof(struct plugin_policy) "
             "is 56 B"),
            ("policy_func", "-DFIX_POLICY_FUNC",
             "plugin_board_policy is symbol type 2, not a data object")):
        p.rm("flashed")
        p.configure(DECLARED="4096", CMAKE_C_FLAGS=p.c_init + " " + flag)
        rc, out = p.ninja("flash")
        flat = probe_fail(out)
        expect(rc != 0 and not os.path.exists(p.path("flashed"))
               and not os.path.exists(p.stamp)
               and "check_policy_probe: FAIL" in flat and says in flat,
               "%s: the probe should refuse (%s)" % (name, says), out)
    results.append(("policy_shape", "a second object of the name, a weak "
                                    "one, one of another size, a function "
                                    "of the name: each refused "
                                    "by the read-back (a writable one is "
                                    "stack_writable_policy)"))
    p.rm("flashed")
    p.configure(DECLARED="4096", CMAKE_C_FLAGS=p.c_init + " -DFIX_POLICY_NONE")
    rc, out = p.ninja("shell")
    expect(rc != 0 and "plugin_board_policy" in out,
           "policy_none: with no plugin_board_policy the link should fail "
           "naming it", out)
    results.append(("policy_none", "no plugin_board_policy: the link fails "
                                   "naming it"))


def cost_tree(p, results):
    """DECLARED -> the policy TU's compile and the asset's command line."""
    nm = p.cc[:-len("gcc")] + "nm"
    seen = []
    for declared in (4096, 5000):
        p.configure(DECLARED=str(declared))
        rc, out = p.ninja("shell", "asset-cost")
        expect(rc == 0, "cost_wiring: build at DECLARED %d failed" % declared,
               out)
        objs = [os.path.join(r, f) for r, _, fs in os.walk(p.bld)
                for f in fs if f.startswith("policy.c.") and
                f.endswith((".o", ".obj"))]
        expect(len(objs) == 1, "cost_wiring: policy.c object not found: %r"
               % objs)
        r = subprocess.run([nm, "-S", objs[0]], capture_output=True,
                           text=True)
        m = re.search(r"^[0-9a-f]+ ([0-9a-f]+) \S+ policy_cost_probe$",
                      r.stdout, re.M)
        expect(m is not None, "cost_wiring: no probe in policy.c.o", r.stdout)
        fw = int(m.group(1), 16)
        with open(p.path("asset_cost.txt")) as fh:
            asset = int(fh.read().strip())
        expect(fw == declared and asset == declared,
               "cost_wiring: DECLARED %d, but the policy TU was compiled with "
               "%d and the asset was handed %d" % (declared, fw, asset))
        seen.append(declared)
    results.append(("cost_wiring", "DECLARED %s reached the policy TU and "
                                   "the asset, and followed the change"
                                   % " -> ".join(map(str, seen))))

    # [!] AN OVERRIDE THAT COMPILES.  A -D later on the command line than the
    # helper's wins with only a warning, so the image carries c = 1 while the
    # build checked 5000.  Nothing may be delivered from it: the check reads the
    # policy back from shell.elf and refuses, and neither flash nor an asset
    # proceeds.
    for f in ("flashed", "packed.nnc"):
        p.rm(f)
    p.configure(DECLARED="5000",
                CMAKE_C_FLAGS=p.c_init + " -DPLUGIN_VENEER_BASE_COST=1u")
    rc, out = p.ninja("shell")
    expect(rc == 0, "cost_override: the overridden image should still link",
           out)
    for target, made in (("flash", "flashed"), ("asset-packed", "packed.nnc")):
        rc, out = p.ninja(target)
        flat = " ".join(out.split())
        expect(rc != 0 and not os.path.exists(p.path(made))
               and "check_policy_probe: FAIL" in flat
               and "its veneer_cost is 1 B" in flat
               and "DECLARED 5000 B" in flat,
               "cost_override: `ninja %s` should fail in check_policy_probe"
               % target, out)
    expect(not os.path.exists(p.stamp),
           "cost_override: a stamp survived the refused probe")
    results.append(("cost_override", "CMAKE_C_FLAGS -DPLUGIN_VENEER_BASE_COST="
                                     "1u links, and the probe stops flash and "
                                     "the asset: " + next(
                                         ln.strip() for ln in out.splitlines()
                                         if "check_policy_probe: FAIL" in ln)
                                     [:110] + "..."))

    # And back: the same tree without the override passes again.
    p.configure(DECLARED="5000", CMAKE_C_FLAGS=p.c_init)
    rc, out = p.ninja("flash")
    expect(rc == 0 and "check_policy_probe: OK" in out,
           "cost_override: removing the override should pass again", out)


def delivery_glued_tree(p, results):
    """issue #149: a path component that ENDS in the firmware's file name.

    This tree is built under a directory called `xshell.elf`, so every command
    of the graph mentions `.../xshell.elf/b/...`.  Before #149 the gate matched
    the name with nothing required before it, saw `shell.elf` in all of them,
    and refused the plain tree (in run_add_plugin_arg_tests.py this happened
    whenever tempfile's random directory name ended in `fw`).  The name must
    not be preceded by a name character: `-` is one, so `x-shell.elf` would
    not reproduce it -- the letter is the point.
    """
    assert os.path.basename(os.path.dirname(p.src)) == "xshell.elf"
    p.configure(DECLARED="4096", FIX_EXTRA="", FIX_DELIVERY="flash")
    rc, out = p.ninja()
    expect(rc == 0 and "check_delivery_gate: OK" in out,
           "delivery_glued: a work path ending in `xshell.elf` must not count "
           "as naming shell.elf", out)
    # And the gate still sees a real use in the same tree.
    p.configure(DECLARED="4096", FIX_EXTRA="by_command", FIX_DELIVERY="flash")
    p.rm("flashed3")
    rc, out = p.ninja("flash3")
    flat = " ".join(out.split())
    expect(rc != 0 and not os.path.exists(p.path("flashed3"))
           and "names shell.elf in its command and does not wait for" in flat,
           "delivery_glued: `ninja flash3` should still be refused under a "
           "work path ending in `xshell.elf`", out)
    results.append(("delivery_glued", "built under .../xshell.elf/: the plain "
                    "tree passes, `ninja flash3` is still refused"))


def name_re_case(cmake_dir, results):
    """issue #149: the name matcher over the whole character class.

    The build cases put only a space, `=` or `{` in front of the name, so a
    matcher that lost `;` (or `(`, `&`, a backtick...) from its "not a name
    character" class would pass all of them.  This imports the gate under test
    -- the one in --cmake-dir, so a mutated copy is tested as the file it is --
    and tries every printable ASCII character on both sides of the name.
    """
    path = os.path.join(cmake_dir, "check_delivery_gate.py")
    spec = importlib.util.spec_from_file_location("delivery_gate_under_test",
                                                  path)
    gate = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(gate)
    if not hasattr(gate, "firmware_name_re"):
        raise Fail("delivery_name_re: %s has no firmware_name_re()" % path)
    rx = gate.firmware_name_re(["shell.elf", "shell.bin", "output.img"])
    namech = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                 "0123456789_.+~-")
    printable = [chr(i) for i in range(32, 127)]
    other = [c for c in printable if c not in namech] + ["\t"]
    bad = []

    def want(text, hit):
        m = rx.search(text)
        if hit and (m is None or m.group(1) != "shell.bin"):
            bad.append("%r should name shell.bin" % text)
        elif not hit and m is not None:
            bad.append("%r should not match (got %r)" % (text, m.group(1)))

    # The #149 shapes, and the documented over-match (first, so a failure
    # names them before the per-character list).
    if gate.firmware_name_re(["fw"]).search("/tmp/tmpab12fw/b"):
        bad.append("'/tmp/tmpab12fw/b' should not name fw")
    if rx.search("/tmp/xshell.elf/b"):
        bad.append("'/tmp/xshell.elf/b' should not name shell.elf")
    if not rx.search("/tmp/-xshell.elf/b"):
        bad.append("'/tmp/-xshell.elf/b' should name shell.elf "
                   "(documented over-match)")
    # Before the name: start of string, or any character that is not a name
    # character, alone or followed by a short-option cluster.
    for c in [""] + other:
        want(c + "shell.bin", True)
        want(c + "-vDshell.bin", True)
    # A name character in front makes it another name -- except `-`, which is
    # the cluster `-` with no letters: `-shell.bin` is not a real option
    # syntax, but counting it refuses a delivery, which is the safe side.
    for c in sorted(namech):
        want(c + "shell.bin", c == "-")
    # After the name: any name character continues it, anything else ends it.
    for c in sorted(namech):
        want("shell.bin" + c, False)
    for c in other:
        want("shell.bin" + c, True)
    if bad:
        raise Fail("delivery_name_re: %d case(s) wrong: %s"
                   % (len(bad), "; ".join(bad[:12])))
    results.append(("delivery_name_re", "%d characters before and after the "
                    "name, the cluster, and the #149 paths"
                    % (len(printable) + 1)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cc", required=True,
                    help="the pinned arm-none-eabi-gcc")
    ap.add_argument("--cmake-dir", default=os.path.join(REPO, "cmake"),
                    help="where the helper and scripts under test are "
                         "(a mutated copy, to test these tests)")
    args = ap.parse_args()
    args.cc = os.path.abspath(args.cc)
    if not shutil.which("ninja"):
        print("run_veneer_gate_build_tests: FAILED -- ninja not found",
              file=sys.stderr)
        return 1

    print("run_veneer_gate_build_tests:")
    results, bad = [], 0
    runs = [
        ("main", lambda p: main_tree(p, results)),
        ("asset_alone", lambda p: asset_tree(p, "asset-bare", "bare.nnc",
                                             results, "asset_alone")),
        ("asset_packed", lambda p: asset_tree(p, "asset-packed",
                                              "packed.nnc", results,
                                              "asset_packed")),
        ("shared_analysis", lambda p: shared_tree(p, results)),
        ("ltrans_drop", lambda p: lto_tree(p, results)),
        ("cost_wiring", lambda p: cost_tree(p, results)),
        ("stack_wiring", lambda p: stack_tree(p, results)),
        ("decoy", lambda p: decoy_tree(p, results)),
        ("entry", lambda p: entry_tree(p, results)),
        ("policy_shape", lambda p: policy_shape_tree(p, results)),
        ("bind_shape", lambda p: bind_shape_tree(p, results)),
        ("delivery", lambda p: delivery_tree(p, results)),
        ("delivery_glued", lambda p: delivery_glued_tree(p, results)),
    ]
    # The directory each tree is built under, inside its temporary directory.
    under = {"delivery_glued": "xshell.elf"}
    try:
        name_re_case(args.cmake_dir, results)
    except Fail as e:
        bad += 1
        print("  FAIL %-15s %s" % ("delivery_name_re", e))
    for name, fn in runs:
        with tempfile.TemporaryDirectory() as work:
            try:
                fn(Project(os.path.join(work, under.get(name, "")),
                           args.cmake_dir, args.cc))
            except Fail as e:
                bad += 1
                print("  FAIL %-15s %s" % (name, e))
    for name, why in results:
        print("  ok   %-15s %s" % (name, why))
    if bad:
        print("run_veneer_gate_build_tests: FAILED", file=sys.stderr)
        return 1
    print("run_veneer_gate_build_tests: the gate cannot be built around")
    return 0


if __name__ == "__main__":
    sys.exit(main())
