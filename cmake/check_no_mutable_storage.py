#!/usr/bin/env python3
"""Refuse a translation unit that owns mutable storage (issue #97).

WHAT THIS PROTECTS.  svc/blazeface.c is one decoder shared by three boards, and
each board keeps its candidate scratch where its own memory map wants it (wio
.psram_ai, f746 .sdram.ai, Grove plain .bss) by passing it in.  That only works
while the shared translation unit owns nothing itself.  If someone adds a static
to it, the buffer silently becomes shared state that no board placed, no board's
residency gate names, and nothing else would ever report -- the f746 build in
particular has no gate that would notice.

WHY A SECTION RULE RATHER THAN A SYMBOL RULE.  Looking for defined OBJECT symbols
misses two real shapes: thread-local storage is STT_TLS rather than STT_OBJECT,
and inline asm can place anonymous writable bytes with no symbol at all.  A
section that is both allocated and writable is the storage itself, whatever put
it there, so that is what is measured.  Empty sections are ignored because the
compiler emits .data and .bss unconditionally.

[!] WHY THIS RUNS PER BOARD AND NOT ONCE ON THE HOST.  The property is about the
object each TARGET build produces.  The host and the boards differ in predefined
macros, ABI and compile options, so code that generates storage only under
__arm__ -- or only under one board's definitions -- passes a host check and fails
on the device.  Each board therefore compiles the shared TU with its own real
definitions, includes and architecture flags (optimisation, LTO and common are
overridden for the audit, so this is an audit compile and not the build's own).

[!] WHAT IT DOES NOT PROVE.  That the object owns no storage expressed as
writable.  NOT that the source contains no `static` -- a static the compiler
optimises away entirely leaves nothing to see, and proving its absence would
take a source-level check this is not.
"""
import argparse
import re
import subprocess
import sys

# objdump -h prints each section's flags on the line after its geometry.
_SECTION = re.compile(
    r"^\s*(?P<idx>\d+)\s+(?P<name>\S+)\s+(?P<size>[0-9a-fA-F]+)\s")


def sections(objdump, obj):
    """Yield (name, size, flags) for every section in an object file."""
    out = subprocess.run([objdump, "-h", obj], check=True,
                         capture_output=True, text=True).stdout
    lines = out.splitlines()
    for i, line in enumerate(lines):
        m = _SECTION.match(line)
        if not m:
            continue
        flags = set()
        if i + 1 < len(lines):
            flags = {f.strip() for f in lines[i + 1].split(",")}
        yield m.group("name"), int(m.group("size"), 16), flags


def offenders(objdump, obj):
    """Allocated, writable, non-empty sections -- i.e. owned storage.

    objdump spells "writable" as the ABSENCE of READONLY, so a section that is
    ALLOC without READONLY is writable: .data, .bss, .tdata, .tbss all land here
    and .text/.rodata do not.
    """
    bad = []
    for name, size, flags in sections(objdump, obj):
        if "ALLOC" not in flags:
            continue
        if "READONLY" in flags:
            continue
        if size == 0:
            continue
        bad.append((name, size, flags))
    return bad


def symbols(nm, obj):
    """Defined data-ish symbols, for a message that names the culprit."""
    try:
        out = subprocess.run([nm, "--defined-only", obj], check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    found = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        cls = parts[-2] if len(parts) >= 3 else parts[0]
        name = parts[-1]
        if cls in "BbCcDdGgSs":
            found.append((cls, name))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--nm", help="only used to name symbols in a failure")
    ap.add_argument("--label", default="",
                    help="what to call this object in messages")
    ap.add_argument("object")
    args = ap.parse_args()

    what = args.label or args.object

    try:
        bad = offenders(args.objdump, args.object)
    except (OSError, subprocess.CalledProcessError) as exc:
        print("check_no_mutable_storage: cannot read %s: %s" %
              (args.object, exc), file=sys.stderr)
        return 2

    if not bad:
        print("check_no_mutable_storage: OK (%s owns no mutable storage)" % what)
        return 0

    print("check_no_mutable_storage: FAIL -- %s owns mutable storage" % what,
          file=sys.stderr)
    for name, size, flags in bad:
        print("  %-20s %6d B   [%s]" %
              (name, size, ", ".join(sorted(flags))), file=sys.stderr)
    if args.nm:
        syms = symbols(args.nm, args.object)
        if syms:
            print("  symbols:", file=sys.stderr)
            for cls, name in syms:
                print("    %s %s" % (cls, name), file=sys.stderr)
        else:
            print("  no data symbols -- anonymous or thread-local storage,",
                  "which is why this gate measures sections", file=sys.stderr)
    print("", file=sys.stderr)
    print("  This translation unit is shared by every board and must own no",
          file=sys.stderr)
    print("  state: each board passes in its own scratch so that it keeps its",
          file=sys.stderr)
    print("  own placement and its own residency gate.  Take the storage out,",
          file=sys.stderr)
    print("  or hand it in through the init call -- do not relax this.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
