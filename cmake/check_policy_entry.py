#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""No firmware link holds an entry that takes a policy or a binding (#165).

The firmware's plugin_parse() reads the board's policy by name
(plugin_board_policy, svc/plugin/plugin_load.h), and cmake/check_policy_probe.py
reads that same symbol back out of shell.elf.  That is a statement about the
policy containers are admitted by only while no other entry to the same parse
is in the image: plugin_parse_with_policy() (svc/plugin/plugin_load_host.c) is
the same body with a policy ARGUMENT, there for the host container verifier and
the host tests, and a firmware that linked it could hand the parse any policy
while the probe went on reading the right one.  So this reads every input the
firmware link loaded and refuses one that defines or references that name.

    the link's map
      -> every object it LOADed, and every archive member it says it included
      -> the symbols of each, through the toolchain's plugin-aware nm (so an
         LTO object is read as the IR the link compiled, not as the empty
         wrapper plain nm sees)
      -> none may name an entry in WATCHED, defined or undefined

[!] THE LINK INPUTS, NOT THE IMAGE.  A name absent from shell.elf proves
nothing under LTO: the old entry, inlined into its caller, leaves no symbol, yet
it was called with whatever policy the caller passed.  Before the LTO link every
call is still a reference to the name, so the inputs are where it is read.

[!] DERIVED FROM THE MAP, NOT LISTED.  The inputs are what the map says the
link loaded -- the objects of every target, the in-tree static libraries' and
the vendor's / toolchain's archive members alike -- so a TU added anywhere is
read without anyone naming it.  The LTO partitions the link wrote and removed
(<tmp>.ltransN.ltrans.o) are not read: each is compiled from the IR objects
read here, and is gone by the time this runs.

[!] FAIL CLOSED.  An input that cannot be read, a member the map names that its
archive does not hold, an archive member from an archive the map did not load,
an nm without the LTO plugin: each is a refusal, and the stamp is not written.

[!] AND WHAT EACH ENTRY READS BY NAME IS THAT NAME (issue #165 review).  The
one input that defines plugin_parse() must reference plugin_board_policy, and
the ones that define plugin_exec_start() / plugin_painter_init() /
plugin_printer_init() plugin_board_bindings: a -D on that one file renames what
it reads, and the read-back would go on finding the real object.

[!] AND THE SAME FOR THE BINDINGS (issue #165).  The base a plugin is handed,
and the functions its painter and printer carry, come from the board's
plugin_board_bindings, which cmake/check_veneer_base_cost.py derives the stack
roots from.  The entries that used to take them as arguments -- the loaders'
base, the report's write, the painter's three and the draw's painter the board
built -- are watched by their old names too: an object still defining or calling one is a firmware that can hand a
plugin something the build did not read.

What this does not see: a board that copies the parse body into a TU of its
own under another name and calls it with another policy, or that builds a
vtable of its own and calls a plugin's slot with it -- trusted native code
going around the rule on purpose (CLAUDE.md: the gate proves a stack bound, not
the absence of such code).  It watches the shared entries' names.
"""
import argparse
import os
import re
import subprocess
import sys

# Entries that take what the firmware must read by name instead:
WATCHED = (
    # the container parse with a policy argument (svc/plugin/plugin_load_host.c)
    "plugin_parse_with_policy",
    # (issue #165) the loaders that took the base to hand entry() -- the shared
    # one and both boards' -- before the base became the head of
    # plugin_board_bindings (plugin_exec_start() / plugin_run_start() now)
    "plugin_exec_load",
    "plugin_run_load",
    # ...and the calls that handed a callback's vtable its functions: the
    # printer's write (nn_active_report_to() now) and the painter's three
    # (plugin_painter_init())
    "nn_active_report",
    "nn_active_core_report",
    "plugin_paint_vtable",
    # ...and the draw entries that took a painter the board had built (and so
    # could edit after it was filled), and the board call that built it:
    # nn_active_draw_on() / nn_active_core_draw_on() / plugin_paint_target()
    # now hand a context only (issue #165 review)
    "nn_active_draw",
    "nn_active_core_draw",
    "plugin_paint_bind",
)

# [!] AND EACH ENTRY THAT READS BY NAME MUST NAME THE OBJECT (issue #165
# review).  A -D on the one file that defines an entry renames what it reads
# (`-Dplugin_board_policy=runtime_policy`), while the check that reads the
# object back still finds the real one, kept by --require-defined.  So the one
# input that defines each entry below must reference the object by its name
# (nm's U -- in an LTO object too).
READS_BY_NAME = (
    ("plugin_parse", "plugin_board_policy"),
    ("plugin_exec_start", "plugin_board_bindings"),
    ("plugin_painter_init", "plugin_board_bindings"),
    ("plugin_printer_init", "plugin_board_bindings"),
)

# What an LTO link writes for itself and removes: its partitions and its early
# debug objects (gcc's /tmp/ccXXXXXX.{ltransN.ltrans,debug.temp}.o).
LTO_TEMP = re.compile(r"(\.ltrans\d+\.ltrans|\.debug\.temp)\.o$")
MEMBER = re.compile(r"^(\S+?\.a)\(([^()]+)\)")
NO_SYMBOLS = re.compile(r"^.*?: (.+): no symbols$")


class InputError(Exception):
    pass


def read_map(path):
    """(loaded objects, loaded archives, [(archive, member)]) in map order."""
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError as exc:
        raise InputError(f"cannot read map {path}: {exc}")
    objs, archives, members = [], [], []
    in_members = False
    saw_map = False
    for line in lines:
        if line.startswith("Archive member included"):
            in_members = True
            continue
        if in_members:
            if line and not line[0].isspace():
                m = MEMBER.match(line)
                if m:
                    members.append((m.group(1), m.group(2)))
                    continue
                in_members = False     # the next heading
            else:
                continue
        if line.startswith("Linker script and memory map"):
            saw_map = True
            continue
        if line.startswith("Cross Reference Table"):
            break
        if saw_map and line.startswith("LOAD "):
            what = line[5:].strip()
            if what == "linker stubs":
                continue
            if what.endswith(".a"):
                if what not in archives:
                    archives.append(what)
            elif what not in objs:
                objs.append(what)
    if not saw_map:
        raise InputError(f"{path} has no 'Linker script and memory map' "
                         "section -- not a GNU ld map")
    return objs, archives, members


def run_nm(nm, files, cwd):
    """{file key: [(name, type)]} and stderr lines, from nm -A -P."""
    r = subprocess.run([nm, "-A", "-P"] + files, cwd=cwd,
                       capture_output=True, text=True)
    out = {}
    for line in r.stdout.splitlines():
        key, sep, rest = line.partition(": ")
        if not sep:
            raise InputError(f"unparsed nm line: {line!r}")
        tok = rest.split()
        if len(tok) < 2:
            raise InputError(f"unparsed nm line: {line!r}")
        out.setdefault(key, []).append((tok[0], tok[1]))
    errs = [ln for ln in r.stderr.splitlines() if ln.strip()]
    return r.returncode, out, errs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--map", required=True, help="the firmware link's map")
    ap.add_argument("--link-dir", required=True,
                    help="the directory the link ran in (map paths are "
                         "relative to it)")
    ap.add_argument("--nm", required=True,
                    help="the toolchain's plugin-aware nm (<triple>-gcc-nm)")
    args = ap.parse_args()

    def fail(msg):
        print(f"check_policy_entry: FAIL -- {msg}", file=sys.stderr)
        return 1

    try:
        objs, archives, members = read_map(args.map)
    except InputError as exc:
        return fail(str(exc))
    if not objs:
        return fail(f"{args.map} LOADs no object -- not the firmware's map")

    def path(p):
        return p if os.path.isabs(p) else os.path.join(args.link_dir, p)

    readable = []
    for o in objs:
        if os.path.isfile(path(o)):
            readable.append(o)
        elif LTO_TEMP.search(o):
            continue                  # written and removed by the LTO link
        else:
            return fail(f"the map LOADs {o}, which cannot be read "
                        f"(looked in {args.link_dir})")
    by_archive = {}
    for a, m in members:
        if a not in archives:
            return fail(f"the map includes {a}({m}), but LOADs no archive "
                        f"{a}")
        by_archive.setdefault(a, set()).add(m)
    for a in by_archive:
        if not os.path.isfile(path(a)):
            return fail(f"the map LOADs {a}, which cannot be read")

    hits, read = [], 0
    try:
        rc, syms, errs = run_nm(args.nm, readable, args.link_dir)
    except (OSError, InputError) as exc:
        return fail(f"{args.nm}: {exc}")
    quiet = set()
    for e in errs:
        m = NO_SYMBOLS.match(e)
        if m:
            quiet.add(m.group(1))
            continue
        return fail(f"{args.nm} on the firmware's objects: {e}")
    if rc != 0:
        return fail(f"{args.nm} on the firmware's objects exited {rc}")
    # An object nm prints nothing for is one whose symbol table holds only
    # section and file symbols (a TU its configuration empties): nm exits 0
    # and says nothing, while one it cannot read is an error above.
    read += len(readable)
    tables = [(o, syms.get(o, [])) for o in readable]
    # gcc-nm's archiver beside it: arm-none-eabi-gcc-nm -> -gcc-ar,
    # gcc-nm-13 -> gcc-ar-13.
    head, sep, tail = args.nm.rpartition("nm")
    if not sep:
        return fail(f"--nm {args.nm}: cannot name its archiver")
    ar = head + "ar" + tail
    for a, want in sorted(by_archive.items()):
        r = subprocess.run([ar, "t", a], cwd=args.link_dir,
                           capture_output=True, text=True)
        if r.returncode != 0:
            return fail(f"{ar} t {a}: {r.stderr.strip()}")
        held = set(r.stdout.split())
        for m in sorted(want - held):
            return fail(f"the map includes {a}({m}), and {a} holds no such "
                        "member")
        try:
            rc, asyms, errs = run_nm(args.nm, [a], args.link_dir)
        except (OSError, InputError) as exc:
            return fail(f"{args.nm} {a}: {exc}")
        aquiet = set()
        for e in errs:
            m = NO_SYMBOLS.match(e)
            if m:
                aquiet.add(m.group(1))
                continue
            return fail(f"{args.nm} on {a}: {e}")
        if rc != 0:
            return fail(f"{args.nm} on {a} exited {rc}")
        for m in sorted(want):
            key = f"{a}[{m}]"
            tables.append((f"{a}({m})", asyms.get(key, [])))
            read += 1

    for where, table in tables:
        for name, typ in table:
            # [!] PLAIN nm READS AN LTO OBJECT AS ITS WRAPPER (one marker
            # symbol), which would let every IR input pass unread.
            if name == "__gnu_lto_slim":
                return fail(f"{args.nm} read {where} without the LTO plugin "
                            "(it shows __gnu_lto_slim, the wrapper of an IR "
                            "object): pass the toolchain's gcc-nm")
            if name in WATCHED:
                how = "references" if typ in ("U", "u") else "defines"
                hits.append(f"{where} {how} {name} ({typ})")
    for entry, obj in READS_BY_NAME:
        defs = [where for where, table in tables
                if any(n == entry and t not in ("U", "u", "w", "v")
                       for n, t in table)]
        if len(defs) != 1:
            hits.append(f"{len(defs)} inputs define {entry} "
                        f"({', '.join(defs) or 'none'}), where exactly one "
                        f"must, and read {obj} by name")
            continue
        table = dict(tables)[defs[0]]
        if not any(n == obj and t in ("U", "u") for n, t in table):
            hits.append(f"{defs[0]} defines {entry} but does not reference "
                        f"{obj} -- what it reads is not the object the build "
                        "reads back (a -D renaming it, or a definition of its "
                        "own)")
    if hits:
        return fail("the firmware link does not read by name what it "
                    "must: "
                    + "; ".join(hits) + ".  The loader reads the board's "
                    "policy (plugin_board_policy) and its bindings "
                    "(plugin_board_bindings) by name; an entry that takes "
                    "either could be handed something other than what the "
                    "build reads back.  plugin_parse_with_policy() is the "
                    "host's (svc/plugin/plugin_load_host.c); the others are "
                    "the pre-#165 forms of plugin_exec_start(), "
                    "plugin_run_start(), nn_active_report_to(), "
                    "nn_active_draw_on() and plugin_paint_target()")
    print(f"check_policy_entry: OK -- {read} inputs of the firmware link "
          f"({len(readable)} objects, {read - len(readable)} archive "
          f"members) read through {os.path.basename(args.nm)}; none defines "
          f"or references {', '.join(WATCHED)}; "
          + ", ".join(f"{e} is defined once and reads {o}"
                      for e, o in READS_BY_NAME))
    return 0


if __name__ == "__main__":
    sys.exit(main())
