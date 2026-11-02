#!/usr/bin/env python3
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
"""The deepest nesting of TABLES a flatbuffers verifier can reach from a
schema's root, and whether the schema has a cycle among its tables (issue #175,
svc/nn/tflm/nn_tflm_model_check.h).

The generated table verifiers are type-directed: Tensor::Verify() calls the
verifier of each table-typed field because the schema says so, whatever the
bytes are, and only a table start counts a level of flatbuffers' max_depth.  So
for an acyclic schema the reachable depth is the longest chain of table-typed
fields from the root -- a property of the schema, not of any buffer -- and the
verifier's max_depth is not what bounds its recursion.  A union field counts as
each of its member tables; struct, enum and scalar fields do not count.

    schema_table_depth.py <schema.fbs> [--root Model]

prints the depth, one longest chain and any cycle, and exits 1 on a cycle.
`--self-test` checks the parser on two small schemas (one with a cycle through a
union) so a parser that stopped seeing fields cannot pass silently.
"""
import argparse
import re
import sys

ROOT = "Model"


def parse(src):
    """{table: [table-typed field targets]} with unions expanded."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    tables, unions = {}, {}
    for kind, name, body in re.findall(r"\b(table|struct|union)\s+(\w+)[^{]*\{(.*?)\}",
                                       src, re.S):
        if kind == "table":
            body = re.sub(r"\([^)]*\)", "", body)        # field attributes
            tables[name] = [t.split(".")[-1] for t in
                            re.findall(r"\w+\s*:\s*\[?\s*([\w.]+)", body)]
        elif kind == "union":
            members = []
            for m in re.findall(r"\w+(?:\s*:\s*[\w.]+)?", body):
                members.append(m.split(":")[-1].strip().split(".")[-1])
            unions[name] = members
    graph = {}
    for t, fields in tables.items():
        out = []
        for f in fields:
            if f in tables:
                out.append(f)
            elif f in unions:
                out += [u for u in unions[f] if u in tables]
        graph[t] = sorted(set(out))
    return graph


def analyse(graph, root):
    """(depth, chain, cycle or None) from @p root; depth counts the root as 1."""
    WHITE, GREY, BLACK = 0, 1, 2
    colour = {t: WHITE for t in graph}
    best = {}
    cycle = []
    sys.setrecursionlimit(max(1000, 4 * len(graph) + 100))

    def visit(t, path):
        colour[t] = GREY
        path.append(t)
        chain = [t]
        for r in graph[t]:
            if colour[r] == GREY:
                if not cycle:
                    cycle.extend(path[path.index(r):] + [r])
                continue
            if colour[r] == WHITE:
                visit(r, path)
            if r in best and 1 + len(best[r]) > len(chain):
                chain = [t] + best[r]
        path.pop()
        colour[t] = BLACK
        best[t] = chain

    visit(root, [])
    return len(best[root]), best[root], (cycle or None)


SELF_TEST = [
    # (schema, expected depth (None: not judged), expect a cycle)
    # A cycle reached only through a union member under an alias, behind an
    # attribute -- the forms schema.fbs uses.
    ("table A { b:B; v:[C]; s:S; }  table B { u:U (id: 1); }  table C { x:int = 0; }"
     " table D { a:A; }  struct S { x:int; }  union U { C, Alias:D }", None, True),
    ("table A { b:B; } table B { c:[ns.C]; } table C { x:int; } union U { A }", 3, False),
]


def self_test():
    ok = True
    for src, want_depth, want_cycle in SELF_TEST:
        g = parse(src)
        d, chain, cyc = analyse(g, "A")
        good = (cyc is not None) == want_cycle and want_depth in (None, d)
        print(f"  {'ok  ' if good else 'FAIL'} self-test: depth {d}, chain "
              f"{' > '.join(chain)}, cycle {cyc}")
        ok = ok and good
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("schema", nargs="?")
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(0 if self_test() else 1)
    if not a.schema:
        ap.error("a schema.fbs path is required")
    graph = parse(open(a.schema).read())
    if a.root not in graph:
        sys.exit(f"schema_table_depth: no table {a.root} in {a.schema}")
    d, chain, cyc = analyse(graph, a.root)
    print(f"tables {len(graph)}, max table nesting from {a.root}: {d}")
    print("chain: " + " > ".join(chain))
    if cyc:
        print("CYCLE: " + " > ".join(cyc))
        sys.exit(1)


if __name__ == "__main__":
    main()
