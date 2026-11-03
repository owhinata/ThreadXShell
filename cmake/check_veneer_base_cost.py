#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Check a board's VENEER_BASE_COST against the firmware it ships (issue #112).

The plugin image gate (check_plugin_image.py) cannot see across a veneer, so at
every crossing into the base it charges a flat allowance: VENEER_BASE_COST, the
stack the FIRMWARE may spend below one veneer.  Until #112 that number was a
claim (Grove) or a hand-summed comment (wio), and nothing derived it from the
code it describes.  This is the derivation, run over the SHIPPED firmware image:
for every veneer of the ABI it walks the firmware function the board says sits
behind it, finds the deepest chain of stack frames below it, and fails unless

    declared VENEER_BASE_COST >= the deepest chain.

[!] IT CHECKS; IT DOES NOT GENERATE.  A generated number would follow the
firmware silently, and the containers already on a device -- whose stack
declarations were computed with the OLD number -- would go on being loaded by a
firmware that can no longer tell (the loader compares a manifest with a policy,
not with this build).  Raising the declaration is a diff someone has to write,
and that diff is the signal that containers must be re-packed and re-sent.

[!] THE ROOTS ARE READ OUT OF THE IMAGE, NOT DECLARED (issue #165).  Which
firmware function sits behind each veneer is the board's table
plugin_board_bindings (svc/plugin/plugin_bindings.h): its head is the base the
loader hands entry(), and the painter and printer are filled from its other
members, so its words in the shipped ELF ARE the six bindings.  Which word is
which veneer is the record plugin_bind_probe (offsets taken from the struct,
veneer names beside them).  So the roots are derived:

    plugin_bind_probe   one symbol, magic, size of the table, count, and per
                        field an offset and a veneer name; the names must be
                        the plugin gate's VENEERS exactly (imported -- the one
                        declaration of the set), each once, the offsets
                        distinct, word-aligned and inside the table
    plugin_board_bindings
                        one symbol, global (not weak, not local), a data
                        object of exactly that size, inside one loadable
                        read-only section
    each word           the Thumb entry of a function, by the same rule a
                        linker veneer's literal is read with
                        (thumb_stack_analysis.Image.thumb_entry): NULL, a
                        clear Thumb bit, an address inside a body or in no
                        body at all are refused.  Aliases at one address are
                        one body; an ARM-state symbol or sizes that disagree
                        are refused when the body is analysed.

Until #165 the board named the six by hand (--root), and a board whose table
bound a deep function while the list named a shallow one passed.

What this does not see: trusted firmware code that hands a callback a vtable
it built itself, or that writes the const table through a cast (Grove's
.rodata is SRAM) -- going around the shared code on purpose.

[!] AND NOTHING HERE HAS A BOARD NAME OR A -mcpu IN IT.  The decoder covers the
Thumb-2 of Armv7E-M and Armv8.1-M Mainline as far as this needs it, which is
the same for every board; an encoding it does not know is a refusal, never a
guess.

How a bound is derived (every step fails closed; since issue #124 the code
is in thumb_stack_analysis.py -- shared with the plugin image gate, which
stops at its veneers instead -- and the roots, which bodies may have no
record, and the budget here):

  functions  are ELF FUNC symbols, identified by ADDRESS, and so are the
             roots (above).  Extents come from st_size, or
             -- for assembler that declared none -- from the next function or
             the end of the map's input section, whichever is first.  Code and
             data inside an extent come from the mapping symbols ($t / $d).
  frame      is the sum of every stack DECREMENT in the body, read out of the
             instruction bytes by the decoder (thumb_stack_analysis.py).
             Increments are ignored, which can only overstate.  An SP write
             whose effect is not a constant (register arithmetic, a move or a
             load into sp, an msr to a stack pointer) is refused, and so is
             any SP change on a cycle of the body's control flow -- a
             decrement counted once but executed per iteration is exactly the
             undercount this exists to rule out.
  edges      are direct calls and direct branches that leave the body (tail
             calls, conditional ones included).  A branch may land on a
             function's entry or inside its own body, nowhere else.  A linker
             long-branch veneer is an edge: its literal is read out of the ELF
             and must be a Thumb function entry.  Indirect transfers (blx/bx
             <reg>, tbb/tbh, a load or move into pc) are refused, and so is a
             return that is not proven to go back to the caller with sp where
             it was: `bx lr` needs lr to hold the return address on every
             path, a pop into pc must load the word lr was saved to (still
             live: nothing stored over it at a known position, sp never risen
             above it), and both, like a tail call, must leave sp at the
             entry; paths that meet with sp at two places are refused
             (thumb_stack_analysis.py, _ReturnAnalysis; issue #124).  So are
             security state transitions (sg, bxns, blxns).  Recursion is
             refused.
  bound      is frame(f) + max(bound(callee)) -- the caller's whole frame is
             charged even below a tail call, which can only overstate.

[!] THE COMPILER'S OWN -fstack-usage RECORD IS A WITNESS, NOT THE ANSWER.  It
under-reports variadic functions (the register save area of `push {r2, r3}` is
not in it), so a bound built from it would err on the unsafe side.  Each body is
compared with the record of ITS OWN object file -- or its own LTO partition --
never with a name looked up across the build (the firmware has two static
`out_str`).  A record larger than the scan means the decoder missed something,
and that fails the check wherever it happens in the image, not only below a
veneer: the decoder's vocabulary is tested against every function the compiler
measured -- and so is a 'dynamic' record for a body the scan found constant.
A body below a veneer must have a record unless the map places it in an
archive under one of the --prebuilt-root directories (the toolchain's and a
vendor's libraries) or it is a linker long-branch veneer; anything of unknown
provenance fails, and so does an LTO partition record the map does not load
(one left behind by an earlier link).

[!] SOME REFUSAL BRANCHES ARE DEFENSIVE, AND HAVE NO FIXTURE FOR A REASON.
cmake/fixtures/run_veneer_cost_tests.py fires every reason it can build; the
rest are pinned as CLASSES, not one bit pattern at a time, and each is grounded
(measured, not assumed -- a search that fails to build a case is not proof one
cannot exist):
  - The decoder's per-encoding "unallocated / based on pc / empty list"
    refusals are one class: an encoding it does not model.  The `undecoded`
    fixture fires that path and proves it is fatal; the `decoder_vs_objdump`
    fixtures compare every instruction of the fixture images with objdump --
    a few hundred per core, the vocabulary those cases themselves use, and
    each run prints how many -- so none of these refusals misfires on the
    shapes tested here.  That nothing REAL is mis-decoded rests on a wider
    run, made once over the two shipped firmware images (~180,000
    instructions, no disagreement); it is not what the fixtures repeat.  No
    instruction count is written here on purpose: adding a case moves it, and
    nothing would keep this sentence in step.  A fixture per unallocated bit
    pattern is neither tractable nor emitted by any compiler.
  - "ARM-state symbol", "entry is not an instruction in a Thumb region",
    "code covered by no mapping symbol": on M-profile the assembler forces the
    Thumb bit on every function symbol (measured), so these need a corrupt or
    hand-built image; the target this repo builds cannot reach them.
  - "the IT block runs out of code", "a branch that may not appear in an IT
    block", "a branch to an offset that is not an instruction", "a loop branch
    that leaves the body": the assembler enforces IT-block and low-overhead-
    loop well-formedness (measured -- gas rejects a branch that is not last in
    an IT block, and a LE/WLS out of range), so these are reachable only from
    hand-assembled bytes.
  - "back into its own body" and "extent runs past its section" are the
    reachable siblings of the branch-into-interior and extent-overlap fixtures;
    the first is observed on the f746 soft-float runtime (__divdf3), which this
    part does not wire.
  - "linker-generated code that is not a long-branch veneer" needs a closure
    that reaches a glue/veneer stub, which only a real image produces.
  - "cannot read <path>" is I/O on an unreadable file: the same class as the
    missing-map fixture.

What this does NOT prove: that the declared bindings are the real ones (above);
anything about exception entry, whose stacking is a separate reserve; anything
about the plugin side, which is check_plugin_image.py's.  And the image-wide
comparison reaches only bodies whose record can be named: a C++ record is
spelled demangled, so C++ bodies are counted "without a record" -- which below
a veneer is a refusal.

[!] AND WHAT IS NOT COMPARED WITH THE SAVED LR.  Since issue #124 a return is
proven by WHERE lr was saved, not only by whether it was: the shapes #112 had
to accept -- `push {lr}` then a store over that word, or `push {lr}` then
`sub sp, #4` so the pop takes another word -- are refused, and so is a body
that returns with sp elsewhere than where it found it
(cmake/fixtures/run_veneer_cost_tests.py: `lr_slot_*`, `tail_sp`,
`sp_unbalanced`, `sp_join`, ...).  A store kills a saved copy when its
position is sp plus a known constant.  A store whose position is NOT -- a
pointer from anywhere else, or one made from sp at a position no constant
gives (a variable index into a local array, a pointer stepped through a
buffer) -- is not compared: it reaches the saved lr only by writing outside
its object, which is memory safety, and that is not what this proves
(cmake/README.md, "Returns and the stack pointer").
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# The veneer set's single source.  A second list here would be a second
# declaration of the same fact, free to drift from the one the plugin gate uses.
import check_plugin_image  # noqa: E402
# [!] THE DECODER, THE ANALYSIS OF ONE BODY AND THE WALK ARE SHARED (issue
# #124): they live in thumb_stack_analysis.py, a build input of this check and
# of the plugin gate both.  What stays here is what is THIS check's -- the
# roots, which bodies need no record (--prebuilt-root), the budget and the
# report.
# SP and decode are not used here: the fixtures reach them through this
# module (run_veneer_cost_tests.py's decoder comparison).
# The records' provenance and the witness lookup are shared too (step 2 of
# #124): the plugin gate compares every body with its own object's record by
# the same rule.
from thumb_stack_analysis import (  # noqa: E402,F401
    InputError, SP, Elf, Image, Walk, decode, parse_map,
    LTO_PRIV, LTRANS_SU_RE, Witnesses, read_su, record_names)


def fmt_chain(image, chain):
    parts = []
    for a in chain:
        n = image.nodes[a]
        info = image.analyse(n)
        tag = " (veneer)" if info.veneer is not None else ""
        parts.append(f"{n.name} {info.frame}{tag}")
    return " > ".join(parts)


# ============================================================================
#  The roots, from the board's table (issue #165)
# ============================================================================
TABLE = "plugin_board_bindings"
RECORD = "plugin_bind_probe"
RECORD_MAGIC = 0x444E4250        # svc/plugin/plugin_bindings.h "PBND"
NAME_MAX = 24                    # PLUGIN_BIND_NAME_MAX
SHT_NOBITS = 8
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
STB_GLOBAL = 1
STT_OBJECT = 1


def _home(elf, addr, size):
    """The loadable, non-empty sections that hold [addr, addr + size)."""
    return [s for s in elf.sections
            if s.flags & SHF_ALLOC and s.type != SHT_NOBITS and s.size
            and s.addr <= addr and addr + size <= s.addr + s.size]


def _bytes(elf, addr, size):
    home = _home(elf, addr, size)
    if len(home) != 1:
        return None
    o = home[0].offset + addr - home[0].addr
    return elf.raw[o:o + size]


def _word(elf, addr):
    b = _bytes(elf, addr, 4)
    return None if b is None else int.from_bytes(b, "little")


def display_name(node):
    """The name a root is shown by: an LTO-localised static without its
    .lto_priv suffix, so that a row reads the same with and without LTO."""
    n = node.name
    m = LTO_PRIV.match(n)
    return m.group(1) if m else n


def derive_roots(elf, image, abi):
    """({veneer: addr}, {veneer: name}, [error]) from the board's table."""
    errs = []
    recs = [x for x in elf.symbols if x[0] == RECORD]
    if len(recs) != 1:
        return {}, {}, [f"{len(recs)} symbols named {RECORD}; the firmware "
                        "links svc/plugin/plugin_bindings.c, which defines it "
                        "once"]
    _, raddr, rsize, _, _ = recs[0]
    head = _bytes(elf, raddr, 12) if rsize >= 12 else None
    if head is None:
        return {}, {}, [f"{RECORD} at 0x{raddr:08x} ({rsize} B) is not in "
                        "loadable bytes"]
    magic, tsize, count = (int.from_bytes(head[i:i + 4], "little")
                           for i in (0, 4, 8))
    if magic != RECORD_MAGIC:
        return {}, {}, [f"{RECORD} magic 0x{magic:08x}, not "
                        f"0x{RECORD_MAGIC:08x}"]
    if rsize != 12 + count * (4 + NAME_MAX):
        return {}, {}, [f"{RECORD} is {rsize} B, not the {count} fields it "
                        "says it holds"]
    body = _bytes(elf, raddr, rsize)
    fields = []
    for k in range(count):
        o = 12 + k * (4 + NAME_MAX)
        off = int.from_bytes(body[o:o + 4], "little")
        raw = body[o + 4:o + 4 + NAME_MAX]
        name = raw.split(b"\0", 1)[0].decode("ascii", "replace")
        if b"\0" not in raw:
            errs.append(f"{RECORD} field {k}: the veneer name is not "
                        "terminated")
        fields.append((off, name))
    names = [n for _, n in fields]
    for n in sorted(set(names)):
        if names.count(n) > 1:
            errs.append(f"{RECORD} names veneer {n} {names.count(n)} times")
    for n in sorted(set(names) - abi):
        errs.append(f"{RECORD} names {n}, which is not a veneer of the plugin "
                    f"ABI ({', '.join(sorted(abi))})")
    for n in sorted(abi - set(names)):
        errs.append(f"{RECORD} has no field for veneer {n} -- every veneer "
                    "the ABI has must be bound by the table")
    offs = [o for o, _ in fields]
    for o, n in fields:
        if o % 4 or o + 4 > tsize:
            errs.append(f"{RECORD}: the {n} field at offset {o} is not a "
                        f"word inside a {tsize} B table")
        elif offs.count(o) > 1:
            errs.append(f"{RECORD}: offset {o} is given to more than one "
                        "veneer")

    tabs = [(x, b) for x, b in zip(elf.symbols, elf.binds) if x[0] == TABLE]
    if len(tabs) != 1:
        errs.append(f"{len(tabs)} symbols named {TABLE}; the board defines "
                    "exactly one (svc/plugin/plugin_bindings.h)")
        return {}, {}, errs
    (_, taddr, size, stype, _), bind = tabs[0]
    if bind != STB_GLOBAL:
        how = {0: "local", 2: "weak"}.get(bind, f"binding {bind}")
        errs.append(f"{TABLE} is {how}, not global: a weak one can be "
                    "replaced at link by a table this did not read, a local "
                    "one is not the table the loader names")
    if stype != STT_OBJECT:
        errs.append(f"{TABLE} is symbol type {stype}, not a data object")
    if size != tsize:
        errs.append(f"{TABLE} is {size} B, but sizeof(struct plugin_bindings) "
                    f"is {tsize} B ({RECORD})")
    home = _home(elf, taddr, tsize)
    if len(home) != 1:
        errs.append(f"{TABLE} at 0x{taddr:08x} is not inside one loadable "
                    "section")
    elif home[0].flags & SHF_WRITE:
        errs.append(f"{TABLE} at 0x{taddr:08x} is in {home[0].name}, a "
                    "writable section: the image holds only its initial "
                    "words.  Define the board's table const.")
    if errs:
        return {}, {}, errs

    resolved, shown = {}, {}
    for off, v in sorted(fields, key=lambda f: f[1]):
        w = _word(elf, taddr + off)
        where = f"{TABLE}+{off} ({v})"
        if w is None:
            errs.append(f"{where} is not in loadable bytes")
            continue
        if w == 0:
            errs.append(f"{where} is NULL -- the plugin would call address 0 "
                        "through that veneer")
            continue
        tgt, why = image.thumb_entry(w)
        if why == "not_thumb":
            errs.append(f"{where} holds 0x{w:08x}, which is not a Thumb code "
                        "address (bit 0 clear)")
            continue
        if why is not None:
            inside = image.node_containing(tgt)
            if inside is not None:
                errs.append(f"{where} holds 0x{w:08x}, inside "
                            f"{inside.name} (0x{inside.addr:08x}), not its "
                            "entry")
            else:
                errs.append(f"{where} holds 0x{w:08x}, which is no "
                            "function's entry")
            continue
        resolved[v] = tgt
        shown[v] = display_name(image.nodes[tgt])
    return resolved, shown, errs


def positive(v):
    try:
        n = int(v, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {v!r}")
    if n <= 0:
        raise argparse.ArgumentTypeError(
            f"{v!r}: zero is not a cost -- it is the analysis assuming the "
            "base spends nothing behind a veneer")
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Check VENEER_BASE_COST against the shipped firmware.",
        fromfile_prefix_chars="@")
    ap.add_argument("elf", help="the firmware image as it ships")
    ap.add_argument("--map", required=True, help="the GNU ld map of that link")
    ap.add_argument("--link-dir",
                    help="the directory the link ran in (relative map paths "
                         "resolve against it); default: the map's directory")
    ap.add_argument("--declared", required=True, type=positive,
                    help="the board's VENEER_BASE_COST, in bytes")
    ap.add_argument("--printer-limit", action="append", default=[],
                    metavar="WHO=BYTES",
                    help="a plugin's printer limit -- the bound on every "
                         "plugin function the base may be handed, which runs "
                         "below the printer veneer too")
    ap.add_argument("--prebuilt-root", action="append", default=[],
                    metavar="DIR",
                    help="archives under DIR are the toolchain's or a "
                         "vendor's, and their members need no record")
    ap.add_argument("--ltrans-su", action="append", default=[],
                    metavar="FILE",
                    help="an LTO partition's -fstack-usage record")
    ap.add_argument("--ltrans-prefix", action="append", default=[],
                    metavar="PREFIX",
                    help="take every PREFIX.ltransN.ltrans.su that exists as "
                         "--ltrans-su (the build cannot name them: they only "
                         "exist once the link has run).  None found is not an "
                         "error -- a non-LTO link writes none -- but then a "
                         "body in an LTO partition has no record, and fails")
    args = ap.parse_args(argv)
    # [!] ONLY THE RECORDS OF THE LINK THAT PRODUCED THIS IMAGE.  The build
    # deletes PREFIX's partition records before each link (veneer_cost_gate
    # .cmake); one that is still here but names a partition the map does not
    # load is refused by Witnesses as a record left by an earlier link.
    for prefix in args.ltrans_prefix:
        d = os.path.dirname(os.path.abspath(prefix)) or "."
        base = os.path.basename(prefix) + ".ltrans"
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.startswith(base) and LTRANS_SU_RE.search(f):
                    args.ltrans_su.append(os.path.join(d, f))
    try:
        return run(args)
    except InputError as exc:
        print(f"check_veneer_base_cost: FAIL\n  - [input] {exc}",
              file=sys.stderr)
        return 1


def run(args):
    errors = []            # (stage, message)
    elf = Elf(args.elf)
    code_names = {s.name for s in elf.code}
    image = Image(elf, *parse_map(args.map, code_names))
    link_dir = args.link_dir or os.path.dirname(os.path.abspath(args.map))
    wit = Witnesses(image, link_dir, args.prebuilt_root, args.ltrans_su)
    errors += [("witness", e) for e in wit.errors]

    # --- roots: read out of the image (issue #165) ---------------------------
    abi = set(check_plugin_image.VENEERS)
    if not abi:
        errors.append(("roots", "the ABI's veneer set is empty -- there is "
                                "nothing to derive a cost for, which is not "
                                "the same as a cost of zero"))
    resolved, roots = {}, {}
    if abi:
        resolved, roots, errs = derive_roots(elf, image, abi)
        errors += [("bindings", e) for e in errs]

    # --- walk ----------------------------------------------------------------
    walk = Walk(image)
    results = {}
    for v, a in sorted(resolved.items()):
        results[v] = walk.total(a)
    errors += walk.errors
    for a in sorted(walk.closure):
        node = image.nodes[a]
        info = image.analyse(node)
        for _at, msg in info.frame_errs + info.flow_errs:
            errors.append(("walk", f"{node.name}: {msg}"))
        kind, detail = wit.origin(node)
        if kind == "stub" and info.veneer is None:
            errors.append(("walk", f"{node.name}: linker-generated code that "
                                   "is not a long-branch veneer"))

    # --- witnesses -----------------------------------------------------------
    stats = {"equal": 0, "above": [], "unscannable": 0, "norecord": 0,
             "none": 0, "closure": 0}
    for a in sorted(image.nodes):
        node = image.nodes[a]
        state, detail = wit.records(node)
        in_closure = a in walk.closure
        info = image.analyse(node)
        if in_closure and state != "none":
            if state != "ok":
                errors.append(("witness", f"{node.name}: {detail} -- a "
                                          "compiled body below a veneer must "
                                          "have its own object's record"
                               if state == "missing" else
                               f"{node.name}: {detail}"))
                continue
            stats["closure"] += 1
        if state == "none":
            stats["none"] += 1
            continue
        if state == "missing":
            stats["norecord"] += 1
            continue
        if state == "ambiguous":
            continue
        scannable = not info.frame_errs
        if state == "dynamic":
            if scannable and not in_closure:
                errors.append(("witness", f"{node.name}: {detail}, but the "
                                          f"scan found a constant frame of "
                                          f"{info.frame} B -- the decoder "
                                          "missed a stack adjustment"))
            continue
        frame, where = detail
        if not scannable:
            stats["unscannable"] += 1
            continue
        if info.frame < frame:
            errors.append(("witness", f"{node.name}: the scan found "
                                      f"{info.frame} B, the compiler's record "
                                      f"({where}) says {frame} B -- the "
                                      "decoder missed a stack adjustment"))
        elif info.frame > frame:
            stats["above"].append((node.name, info.frame, frame))
        else:
            stats["equal"] += 1

    # --- budget --------------------------------------------------------------
    deepest = max(sorted(results), key=lambda v: results[v][0]) \
        if results else None
    derived = results[deepest][0] if deepest is not None else 0
    for v, (t, chain, complete) in sorted(results.items()):
        # [!] A LOWER BOUND CAN STILL PROVE A DEFICIT.  A walk that was refused
        # somewhere reports what it could see; if even that exceeds the
        # declaration, the declaration is wrong whatever the refusal hid.
        if t > args.declared:
            least = "" if complete else "at least "
            errors.append(("budget", f"{v} needs {least}{t} B below it "
                                     f"({fmt_chain(image, chain)}), but "
                                     f"VENEER_BASE_COST declares "
                                     f"{args.declared} B"))
    for spec in args.printer_limit:
        who, sep, n = spec.partition("=")
        try:
            lim = int(n, 0) if sep else None
        except ValueError:
            lim = None
        if lim is None or lim <= 0:
            errors.append(("budget", f"--printer-limit {spec!r} is not "
                                     "WHO=BYTES"))
            continue
        if lim > args.declared:
            errors.append(("budget", f"{who}: its own printer is bounded at "
                                     f"{lim} B, above VENEER_BASE_COST "
                                     f"{args.declared} B -- the printer veneer "
                                     "charges the declared cost, so the "
                                     "plugin's printer must fit under it"))

    # --- report --------------------------------------------------------------
    elfname = os.path.basename(args.elf)
    width = max([len(v) for v in results] + [8])
    fwidth = max([len(roots[v]) for v in results] + [8])
    table = []
    for v, (t, chain, complete) in sorted(results.items()):
        table.append(f"  {v:{width}s} {roots[v]:{fwidth}s} "
                     f"{'>=' if not complete else '  '}{t:5d} B  "
                     f"{fmt_chain(image, chain)}")
    above = stats["above"]
    summary = [
        f"  witnesses: {stats['closure']} bodies below a veneer matched their "
        f"own object's record; image-wide {stats['equal'] + len(above)} "
        f"checked ({stats['equal']} equal, {len(above)} above the record), "
        f"{stats['unscannable']} not scannable, {stats['norecord']} without "
        f"a record, {stats['none']} prebuilt or linker-generated"]
    if above:
        summary.append(
            "  above the record (the compiler leaves out e.g. a variadic save "
            "area): " + ", ".join(f"{n} {s}/{r}" for n, s, r in above[:12])
            + (" ..." if len(above) > 12 else ""))
    if errors:
        print("check_veneer_base_cost: FAIL", file=sys.stderr)
        for stage, msg in errors:
            print(f"  - [{stage}] {msg}", file=sys.stderr)
        if table:
            print("  what could be walked (>= marks a lower bound):",
                  file=sys.stderr)
            for line in table:
                print("  " + line, file=sys.stderr)
        for line in summary:
            print(line, file=sys.stderr)
        return 1
    print(f"check_veneer_base_cost: OK -- VENEER_BASE_COST {args.declared} B "
          f">= {derived} B derived from {elfname} (headroom "
          f"{args.declared - derived} B)")
    for line in table:
        print(line)
    if deepest is not None:
        print(f"  deepest: {deepest}: "
              f"{fmt_chain(image, results[deepest][1])}")
    for spec in args.printer_limit:
        print(f"  printer: {spec.replace('=', ' ')} B <= {args.declared} B")
    for line in summary:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
