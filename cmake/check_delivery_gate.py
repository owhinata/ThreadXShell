#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Every target that uses the shipped firmware waits for its gate (#126, P14).

cmake/veneer_cost_gate.cmake makes the board's delivery targets -- the ones it
is TOLD about, DELIVERY -- wait for the veneer-cost check and policy read-back.
A list is only as good as whoever keeps it: a new target that flashes the image
would be outside the gate, and nothing would say so.  CMake cannot answer the
question at configure time (a custom target's COMMAND and its DEPENDS are not
queryable properties), so this answers it from the build graph CMake wrote:
build.ninja.

A FIRMWARE ARTIFACT is every output of the edge that links the firmware: the
ELF and whatever its link step declares beside it (the map, the .bin/.hex/.img
its POST_BUILD writes as BYPRODUCTS).

An edge USES the firmware when, other than the link edge itself, the gate's
own check edge and this check's edge:
  - one of its explicit or implicit inputs, resolved through phony nodes, is a
    firmware artifact -- not its order-only inputs, which CMake fills with
    every target the rule's target transitively waits for; or
  - its COMMAND, or any other variable of the edge but its description (a
    link edge keeps an executable's POST_BUILD and PRE_LINK steps in variables
    of their own), names one by file name (`dfu-util -D shell.bin`, run from
    the build directory, has no input that says so).

Then, in both directions:
  1. every edge that uses the firmware reaches the gate's stamp through its
     inputs -- the property itself, whatever the target is called;
  2. every CMake target that owns such an edge is declared in DELIVERY (or is
     an asset-* target, which the gate wires by name) -- a new delivery target
     is named, not merely gated by accident;
  3. every DELIVERY target actually reaches an edge that uses the firmware --
     a declaration that delivers nothing is a stale one;
  4. every CMake target waits for THIS check, so `ninja <new-target>` cannot
     run before it has been looked at.

[!] WHAT THIS CANNOT SEE.  A command that reaches the image without naming it
or depending on it -- a script that hard-codes or computes the path, a glob, an
environment variable -- does not use the firmware as far as the graph says, and
neither does anything run outside the build (a shell alias, picocom by hand).
And a POST_BUILD or PRE_LINK step on the FIRMWARE target is part of its link
edge, which runs before any gate can: nothing here, or anywhere in a build
graph, can order a gate between a link and its own post-build step.
"""
import argparse
import os
import re
import sys


def fail(msg):
    print(f"check_delivery_gate: FAIL -- {msg}", file=sys.stderr)
    return 1


class Edge:
    __slots__ = ("outs", "rule", "ins", "data", "vars", "line")

    def __init__(self, outs, rule, ins, data, line):
        # ins: every input, order-only included (what ninja waits for).
        # data: explicit and implicit inputs only (what the edge reads).
        self.outs, self.rule, self.ins, self.data, self.vars, self.line = (
            outs, rule, ins, data, {}, line)


def split_paths(text):
    """Ninja path list: whitespace-separated, `$ ` `$:` `$$` escapes."""
    out, cur, i = [], [], 0
    while i < len(text):
        c = text[i]
        if c == "$" and i + 1 < len(text):
            cur.append(text[i:i + 2])
            i += 2
            continue
        if c in " \t":
            if cur:
                out.append("".join(cur))
                cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        out.append("".join(cur))
    return out


def find_unescaped(text, ch):
    i = 0
    while i < len(text):
        if text[i] == "$":
            i += 2
            continue
        if text[i] == ch:
            return i
        i += 1
    return -1


def parse(path):
    """build.ninja -> (top-level variables, edges)."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        raw = fh.read().split("\n")
    lines, buf = [], ""
    for ln in raw:                     # join `$` continuations
        if ln.endswith("$") and not ln.endswith("$$"):
            buf += ln[:-1]
            continue
        lines.append(buf + ln)
        buf = ""
    top, edges, cur, n = {}, [], None, 0
    for ln in lines:
        n += 1
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        if ln[0] in " \t":
            if cur is not None and "=" in ln:
                k, _, v = ln.strip().partition("=")
                cur.vars[k.strip()] = v.strip()
            continue
        cur = None
        if ln.startswith("build "):
            body = ln[len("build "):]
            colon = find_unescaped(body, ":")
            if colon < 0:
                raise ValueError(f"line {n}: no ':' in a build statement")
            outs = split_paths(body[:colon])
            rest = split_paths(body[colon + 1:])
            if not rest:
                raise ValueError(f"line {n}: no rule")
            rule, ins, data, order_only = rest[0], [], [], False
            for tok in rest[1:]:
                if tok in ("||", "|@"):
                    order_only = True
                    continue
                if tok == "|":
                    continue
                ins.append(tok)
                if not order_only:
                    data.append(tok)
            cur = Edge([o for o in outs if o != "|"], rule, ins, data, n)
            edges.append(cur)
        elif ln.startswith(("include ", "subninja ")):
            top.setdefault("__includes__", []).append(ln.split(None, 1)[1])
        elif re.match(r"^[A-Za-z0-9_.-]+\s*=", ln):
            k, _, v = ln.partition("=")
            top[k.strip()] = v.strip()
    return top, edges


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ninja", required=True, help="build.ninja")
    ap.add_argument("--firmware-target", required=True,
                    help="the firmware's CMake target; its link edge is found "
                         "through the target's phony node")
    ap.add_argument("--gate-stamp", required=True,
                    help="the stamp the gate writes when it passes")
    ap.add_argument("--gate-target", required=True)
    ap.add_argument("--self-stamp", required=True,
                    help="this check's own stamp, which every target waits for")
    ap.add_argument("--self-target", required=True)
    ap.add_argument("--targets", required=True,
                    help="file listing every buildsystem target, one a line")
    ap.add_argument("--delivery", nargs="+", required=True)
    ap.add_argument("--by-name", nargs="*", default=[],
                    help="targets the gate wired by name (asset-*): they may "
                         "use the firmware without being in DELIVERY")
    args = ap.parse_args()

    try:
        top, edges = parse(args.ninja)
    except (OSError, ValueError) as exc:
        return fail(f"cannot read {args.ninja}: {exc}")
    # [!] ONE FILE OR NOTHING.  A multi-config generator spreads the edges over
    # impl files this does not read; reading part of the graph would pass what
    # it never saw.
    incs = [i for i in top.get("__includes__", [])
            if not i.endswith("rules.ninja")]
    if incs:
        return fail(f"{args.ninja} includes {incs}; only a single-config "
                    "Ninja build.ninja is understood, and a graph read in part "
                    "is not checked")
    bdir = os.path.dirname(os.path.abspath(args.ninja))
    wd = top.get("cmake_ninja_workdir", bdir + "/")

    def norm(p):
        p = p.replace("${cmake_ninja_workdir}", wd).replace("$ ", " ") \
             .replace("$:", ":").replace("$$", "$")
        if os.path.isabs(p):
            ap_ = os.path.normpath(p)
            if ap_.startswith(bdir + os.sep):
                return os.path.relpath(ap_, bdir)
            return ap_
        return os.path.normpath(p)

    for e in edges:
        e.outs = [norm(o) for o in e.outs]
        e.ins = [norm(i) for i in e.ins]
        e.data = [norm(i) for i in e.data]
    producer = {}
    for e in edges:
        for o in e.outs:
            producer[o] = e
    stamp, selfstamp = norm(args.gate_stamp), norm(args.self_stamp)
    for what, p in (("gate stamp", stamp), ("this check's stamp", selfstamp)):
        if p not in producer:
            return fail(f"no edge in {args.ninja} produces the {what} {p}")
    fe = producer.get(args.firmware_target)
    if fe is not None and fe.rule != "phony":
        links = [fe]                 # an executable with no suffix: the file
    else:                            # is the target's name
        links = [producer[i] for i in (fe.ins if fe is not None else [])
                 if i in producer and producer[i].rule != "phony"]
    if len(links) != 1:
        return fail(f"the firmware target '{args.firmware_target}' resolves to "
                    f"{len(links)} link edge(s) in {args.ninja}, not one")
    link, gate = links[0], producer[stamp]
    artifacts = set(link.outs)
    names = sorted({os.path.basename(a) for a in artifacts})
    # The file name as a whole path component, wherever the command puts it:
    # after a space, a quote, '=', ':' or '/', and not continued by anything
    # that would make it another name (shell.elf.ltrans0.ltrans.su is not it).
    name_re = re.compile(r"(?:^|[\s'\"=:/])(" + "|".join(map(re.escape, names))
                         + r")(?=$|[\s'\";,)])")
    with open(args.targets) as fh:
        listed = [t.strip() for t in fh if t.strip()]
    for t in [args.gate_target, args.self_target] + args.delivery:
        if t not in listed:
            return fail(f"'{t}' is not a target of this build")
    # [!] THE TARGETS ARE ALSO READ FROM THE GRAPH, not only from the list the
    # gate wrote at the end of configure: a target created after that (a later
    # deferred call) is in build.ninja and in no list.  A top-level phony node
    # is one CMake target's name (or its output's alias); the exceptions are
    # CMake's own helpers, which build nothing of this project.
    builtin = {"edit_cache", "rebuild_cache", "install", "install/local",
               "install/strip", "list_install_components", "test", "package",
               "package_source"}
    targets = list(listed)
    for e in edges:
        # A phony with no inputs is CMake marking a regeneration input that may
        # be missing (CMakeCache.txt, a list file) -- not a target.
        if e.rule != "phony" or not e.ins:
            continue
        for o in e.outs:
            if ("/" in o or o in builtin or o in targets
                    or o.startswith("cmake_object_order_depends_target_")):
                continue
            targets.append(o)

    def through_phony(node, seen):
        """Node, plus what a phony edge producing it stands for."""
        if node in seen:
            return
        seen.add(node)
        e = producer.get(node)
        if e is not None and e.rule == "phony":
            for i in e.ins:
                through_phony(i, seen)

    me = producer[selfstamp]

    def uses(e):
        if e is link or e is gate or e is me or e.rule == "phony":
            return None
        # [!] EXPLICIT AND IMPLICIT INPUTS ONLY.  CMake writes every target a
        # custom command's target transitively depends on into its order-only
        # list -- every rule of a target that waits for the gate lists the
        # firmware there -- so order-only says "after", not "reads".
        seen = set()
        for i in e.data:
            through_phony(i, seen)
        hit = sorted(seen & artifacts)
        if hit:
            return "depends on " + ", ".join(hit)
        # Every variable of the edge but its description: a link edge carries
        # an executable's POST_BUILD and PRE_LINK steps in variables of their
        # own, not in COMMAND -- found by a fixture, not by reading.
        for k, v in sorted(e.vars.items()):
            if k == "DESC":
                continue
            m = name_re.search(v)
            if m:
                where = "its command" if k == "COMMAND" else f"its {k}"
                return f"names {m.group(1)} in {where}"
        return None

    def reaches(start_ins, goal):
        stack, seen = list(start_ins), set()
        while stack:
            n = stack.pop()
            if n == goal:
                return True
            if n in seen:
                continue
            seen.add(n)
            e = producer.get(n)
            if e is not None:
                stack.extend(e.ins)
        return False

    users = [(e, why) for e in edges for why in [uses(e)] if why]
    errors = []
    # 1. the property
    for e, why in users:
        if not reaches(e.ins, stamp):
            errors.append(f"{e.outs[0]} (build.ninja line {e.line}) {why} "
                          f"and does not wait for {stamp}")

    # Which CMake target owns each user: the targets whose phony closure
    # contains one of its outputs (a target that depends on a delivery target
    # owns that target's edges too, and is named like it -- wio's `flash`).
    closure = {}
    for t in targets:
        seen = set()
        through_phony(t, seen)
        closure[t] = seen
    user_outs = {o: e for e, _ in users for o in e.outs}
    owners = {}
    for t in targets:
        if t in (args.gate_target, args.self_target, "all"):
            continue
        for o in closure[t] & set(user_outs):
            owners.setdefault(t, set()).add(user_outs[o].outs[0])
    # 2. every owner declared
    for t in sorted(owners):
        if t not in args.delivery and t not in args.by_name:
            errors.append(f"target '{t}' uses the firmware ({', '.join(sorted(owners[t]))}) "
                          "but is not in veneer_cost_gate()'s DELIVERY")
    # 3. every declaration delivers something
    for t in args.delivery:
        if t not in owners:
            errors.append(f"DELIVERY names '{t}', which uses no firmware "
                          f"artifact ({', '.join(names)}) -- a stale or "
                          "mistaken declaration")
    # 4. every target waits for this check
    for t in targets:
        if t == args.self_target:
            continue
        if not reaches([t], selfstamp):
            errors.append(f"target '{t}' does not wait for this check, so "
                          f"`ninja {t}` could run before it")
    if errors:
        return fail(f"{len(errors)} problem(s) in {args.ninja}:\n  "
                    + "\n  ".join(errors))
    print(f"check_delivery_gate: OK -- {len(users)} edge(s) use the firmware "
          f"({', '.join(names)}); each waits for {stamp}; DELIVERY = "
          f"{' '.join(sorted(args.delivery))} = the targets that own them; "
          f"{len(targets)} target(s) wait for this check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
