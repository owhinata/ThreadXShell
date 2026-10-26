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

[!] THE BOARD'S FACT IS WHICH FIRMWARE FUNCTION SITS BEHIND EACH VENEER, and it
is the one thing here that is stated rather than derived (--root).  The SET of
veneers is not the board's: it is the plugin gate's VENEERS, imported, and the
roots must cover it exactly -- none missing, none extra.  The result holds for
the bindings as declared; binding a different callback without updating the
declaration is invisible here, the same layer of "board states a fact" as the
FORBIDDEN table.

[!] AND NOTHING HERE HAS A BOARD NAME OR A -mcpu IN IT.  The decoder covers the
Thumb-2 of Armv7E-M and Armv8.1-M Mainline as far as this needs it, which is
the same for every board; an encoding it does not know is a refusal, never a
guess.

How a bound is derived (every step fails closed; since issue #124 the code
is in thumb_stack_analysis.py, and the roots, records and budget here):

  functions  are ELF FUNC symbols, identified by ADDRESS.  A name is only ever
             used to resolve a root, and a root that names two bodies (two
             statics in two files) is refused.  Extents come from st_size, or
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
             return that is not proven to use the return address: `bx lr` needs
             lr to hold it on every path, a pop into pc needs lr to have been
             saved on every path.  Recursion is refused.
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

[!] NOR THAT A RETURN RETURNS.  A `pop {..., pc}` is accepted as a return when
lr was SAVED on every path; which word the pop then takes is not checked, and
slot identity is not tracked at all.  So a body that saves lr and then leaves
by some other word is still read as a return -- `push {lr}` then `str` over
that slot, or `push {lr}` then `sub sp, #4` so the pop takes a different word
with no store at all (both measured) -- and only that body's own frame is
charged, with nothing charged for wherever it goes.  What IS held is the
condition itself: with lr never saved, the same `pop {..., pc}` is refused
(measured), so this is not a way past the walk for ordinary code.  No compiler
emits any of it; the witness rule stops the hand-written spellings that could
reach a veneer from this tree (no -fstack-usage record, so no walk), but C with
__attribute__((naked)) and inline asm has a record and does reach it.
cmake/fixtures/run_veneer_cost_tests.py pins all four shapes
(`lr_slot_asm_in_tree`, `lr_slot_naked_c`, `lr_slot_moved_sp`,
`lr_never_saved`) so the limit cannot move in silence; closing it needs slot
identity, in the shared walk (issue #111).
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
# roots, the records and their provenance, the budget and the report.
# SP and decode are not used here: the fixtures reach them through this
# module (run_veneer_cost_tests.py's decoder comparison).
from thumb_stack_analysis import (  # noqa: E402,F401
    InputError, SP, Elf, Image, Walk, decode, parse_map)


# ============================================================================
#  Provenance and witnesses
# ============================================================================
ARCHIVE_RE = re.compile(r"^(.*)\(([^()]+)\)$")
LTRANS_O_RE = re.compile(r"\.ltrans(\d+)\.ltrans\.o$")
LTRANS_SU_RE = re.compile(r"\.ltrans(\d+)\.ltrans\.su$")
SU_LINE = re.compile(r"^(.*):(\d+):(\d+):(.+)\t(\d+)\t(\S+)$")


def read_su(path):
    """{name: [(frame, qualifier, location)]} from one -fstack-usage file."""
    recs = {}
    try:
        text = open(path, errors="replace").read()
    except OSError as exc:
        raise InputError(f"cannot read {path}: {exc}")
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        m = SU_LINE.match(line)
        if not m:
            raise InputError(f"{path}:{n}: not a -fstack-usage record: "
                             f"{line[:80]!r}")
        recs.setdefault(m.group(4), []).append(
            (int(m.group(5)), m.group(6), f"{m.group(1)}:{m.group(2)}"))
    return recs


LTO_PRIV = re.compile(r"^(.*?)((?:\.lto_priv\.\d+)+)$")


def record_names(name):
    """The spellings -fstack-usage may use for one ELF symbol, most exact
    first.  Measured with the pinned GCC: the record drops the NUMBER of an
    IPA clone but not the clone's tag -- `f.constprop.0.isra.0` is recorded as
    `f.constprop.isra` -- while a partial-inlining part keeps its own
    (`f.part.0.constprop`); and LTO's `f.lto_priv.0` is a rename in the symbol
    table only, recorded as `f`.  Every combination of dropped numbers is
    tried, fewest first; a spelling can only ever match a record in the body's
    OWN record file, and two records that disagree are refused as ambiguous."""
    out = [name]
    m = LTO_PRIV.match(name)
    base = m.group(1) if m else name
    parts = base.split(".")
    nums = [k for k in range(1, len(parts)) if parts[k].isdigit()]
    for drop in range(len(nums) + 1):
        for combo in _combos(nums[::-1], drop):
            cand = ".".join(p for k, p in enumerate(parts) if k not in combo)
            if cand not in out:
                out.append(cand)
    return out


def _combos(items, k):
    if k == 0:
        yield set()
        return
    for idx, it in enumerate(items):
        for rest in _combos(items[idx + 1:], k - 1):
            yield {it} | rest


class Witnesses:
    def __init__(self, image, link_dir, prebuilt_roots, ltrans_su):
        self.image = image
        self.link_dir = link_dir
        self.roots = [os.path.realpath(r) for r in prebuilt_roots]
        self.errors = []
        self._su_cache = {}
        self.ltrans = {}
        for path in ltrans_su:
            m = LTRANS_SU_RE.search(os.path.basename(path))
            if not m:
                raise InputError(f"--ltrans-su {path}: not named like an LTO "
                                 "partition's record (*.ltransN.ltrans.su)")
            n = int(m.group(1))
            if n in self.ltrans:
                raise InputError(f"two records for LTO partition {n}: "
                                 f"{self.ltrans[n]} and {path}")
            self.ltrans[n] = path
        seen_parts = set()
        for what in image.loads:
            m = LTRANS_O_RE.search(what)
            if m and not ARCHIVE_RE.match(what):
                seen_parts.add(int(m.group(1)))
        for n in sorted(set(self.ltrans) - seen_parts):
            self.errors.append(
                f"{self.ltrans[n]} is the record of LTO partition {n}, which "
                "this image's map does not contain -- a record left by an "
                "earlier link is not a witness of this one")

    def origin(self, node):
        """('prebuilt' | 'stub' | 'object' | 'ltrans' | 'unknown', detail)"""
        if node.origin is not None:
            return node.origin
        inp = self.image.input_at(node.addr)
        if inp is None:
            o = ("unknown", "the map places no input section there")
        else:
            a, size, what = inp
            if node.end > a + size:
                o = ("unknown", f"its extent runs past its input section "
                                f"({what})")
            elif what == "linker stubs":
                o = ("stub", what)
            else:
                m = ARCHIVE_RE.match(what)
                if m:
                    arc = os.path.realpath(os.path.join(self.link_dir,
                                                        m.group(1)))
                    if any(arc.startswith(r + os.sep) for r in self.roots):
                        o = ("prebuilt", what)
                    else:
                        o = ("unknown", f"archive member {what} is not under "
                                        "a --prebuilt-root, and nothing says "
                                        "where its record is")
                else:
                    m = LTRANS_O_RE.search(what)
                    if m:
                        o = ("ltrans", int(m.group(1)))
                    else:
                        o = ("object", os.path.join(self.link_dir, what))
        node.origin = o
        return o

    def records(self, node):
        """(state, detail): state is 'none' (no witness is required), 'missing',
        'ambiguous', 'dynamic' or 'ok' (detail = (frame, where))."""
        kind, detail = self.origin(node)
        if kind in ("prebuilt", "stub"):
            return "none", kind
        if kind == "unknown":
            return "missing", detail
        if kind == "ltrans":
            path = self.ltrans.get(detail)
            if path is None:
                return "missing", (f"LTO partition {detail} has no record "
                                   "file")
        else:
            path = os.path.splitext(detail)[0] + ".su"
            if not os.path.exists(path):
                return "missing", f"no record file {path}"
        recs = self._su(path)
        # [!] EVERY SPELLING OF EVERY NAME, NOT THE FIRST THAT HITS.  Picking
        # one would make the answer depend on the order the spellings are
        # tried in; taking them all makes two records that could belong to
        # this body disagree out loud (ambiguous) instead.
        cands = []
        tried = set()
        for name in node.names:
            for spelling in record_names(name):
                if spelling in recs and spelling not in tried:
                    tried.add(spelling)
                    cands.extend(recs[spelling])
        if not cands:
            return "missing", f"no record for {node.name} in {path}"
        if any(q != "static" for _f, q, _w in cands):
            q = [q for _f, q, _w in cands if q != "static"][0]
            return "dynamic", f"{path} records {node.name} as '{q}'"
        if len({f for f, _q, _w in cands}) > 1:
            return "ambiguous", (f"{path} has {len(cands)} different records "
                                 f"named like {node.name}")
        return "ok", (cands[0][0], path)

    def _su(self, path):
        if path not in self._su_cache:
            self._su_cache[path] = read_su(path)
        return self._su_cache[path]


def fmt_chain(image, chain):
    parts = []
    for a in chain:
        n = image.nodes[a]
        info = image.analyse(n)
        tag = " (veneer)" if info.veneer is not None else ""
        parts.append(f"{n.name} {info.frame}{tag}")
    return " > ".join(parts)


# ============================================================================
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
    ap.add_argument("--root", action="append", default=[], metavar="V=F",
                    help="veneer V of the plugin ABI reaches firmware "
                         "function F; one per veneer, no more, no fewer")
    ap.add_argument("--declared", required=True, type=positive,
                    help="the board's VENEER_BASE_COST, in bytes")
    ap.add_argument("--printer-limit", action="append", default=[],
                    metavar="WHO=BYTES",
                    help="a plugin's own pl_sbuf_write bound, which runs below "
                         "the printer veneer too")
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

    # --- roots ---------------------------------------------------------------
    abi = set(check_plugin_image.VENEERS)
    roots = {}
    if not abi:
        errors.append(("roots", "the ABI's veneer set is empty -- there is "
                                "nothing to derive a cost for, which is not "
                                "the same as a cost of zero"))
    for spec in args.root:
        v, sep, f = spec.partition("=")
        if not sep or not v or not f:
            errors.append(("roots", f"--root {spec!r} is not VENEER=FUNCTION"))
            continue
        if v in roots:
            errors.append(("roots", f"veneer {v} is declared twice"))
            continue
        roots[v] = f
    for v in sorted(set(roots) - abi):
        errors.append(("roots", f"{v} is not a veneer of the plugin ABI "
                                f"({', '.join(sorted(abi))})"))
    for v in sorted(abi - set(roots)):
        errors.append(("roots", f"no firmware function is declared behind "
                                f"veneer {v} -- every veneer the ABI has must "
                                "be accounted for"))
    resolved = {}
    for v, f in sorted(roots.items()):
        if v not in abi:
            continue
        exact = [a for a, n in image.nodes.items() if f in n.names]
        if not exact:
            exact = [a for a, n in image.nodes.items()
                     if any(LTO_PRIV.match(x) and LTO_PRIV.match(x).group(1)
                            == f for x in n.names)]
        if not exact:
            errors.append(("roots", f"{v}: {f} is not a function in "
                                    f"{os.path.basename(args.elf)}"))
        elif len(exact) > 1:
            where = ", ".join(f"0x{a:08x}" for a in sorted(exact))
            errors.append(("roots", f"{v}: {f} names {len(exact)} functions "
                                    f"({where}) -- a root must be exactly "
                                    "one body"))
        else:
            resolved[v] = exact[0]

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
