#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Tests for cmake/fetch_upstream.cmake (issue #136), with no network.

A throwaway "upstream" repository with two commits is served over file://, and
a throwaway superproject (itself a git repository, as this one is) configures a
tiny CMake project that calls fetch_upstream() on it.  Needs git, cmake and
ninja -- no toolchain, no board.

  - a fresh fetch publishes <repo>-<sha> with only the sparse paths, and a
    second configure is OFFLINE (the upstream is moved away first);
  - the checkout's HEAD file is a configure dependency;
  - a checkout moved to another commit, a deleted probe, an edited file, an
    untracked file, a file upstream's .gitignore hides, an empty directory
    standing in for the checkout, a branch checked out at the pin, an edit hidden
    by assume-unchanged, a deletion hidden by skip-worktree, a narrowed sparse
    set, a sparse tree asked for without SPARSE (and the reverse) and a `git
    replace` of the pin are each FATAL, and are not repaired or re-fetched;
  - GIT_DIR pointing at the superproject neither breaks the check of an existing
    tree nor lets a fetch land in the superproject;
  - a staging directory that is a symlink is refused and its target kept;
  - a deleted checkout is fetched again, even over a leftover staging directory;
  - a commit the upstream does not have, an abbreviated commit, a missing PROBE
    and a probe the sparse patterns exclude are refused, and the first publishes
    nothing;
  - a second configure waits for the lock a first one holds.

[!] EVERY REFUSAL IS PAIRED WITH THE SAME CONFIGURE PASSING BY ONE CHANGE, so a
harness that could not run the helper at all would not look like a working gate.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HELPER = os.path.join(REPO, "cmake", "fetch_upstream.cmake")

bad = 0


def check(name, ok, why):
    global bad
    print("  %s %-34s %s" % ("ok  " if ok else "FAIL", name, why))
    if not ok:
        bad += 1


def git(cwd, *args):
    return subprocess.run(["git", "-C", cwd, *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


tmp = tempfile.mkdtemp(prefix="fetch_upstream_")
try:
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    os.environ.update(env)

    # --- the upstream: two commits, served over file:// ----------------------
    up = os.path.join(tmp, "remote", "demo-repo")
    os.makedirs(up)
    git(up, "init", "-q")
    git(up, "config", "uploadpack.allowFilter", "true")
    git(up, "config", "uploadpack.allowAnySHA1InWant", "true")
    write(os.path.join(up, "Inc", "a.h"), "#define A 1\n")
    write(os.path.join(up, "Src", "a.c"), "int a;\n")
    write(os.path.join(up, "docs", "big.txt"), "not wanted\n")
    write(os.path.join(up, "Inc", ".gitignore"), "*.gen.h\n")
    git(up, "add", "-A")
    git(up, "commit", "-q", "-m", "A")
    sha_a = git(up, "rev-parse", "HEAD")
    write(os.path.join(up, "Inc", "a.h"), "#define A 2\n")
    git(up, "commit", "-q", "-am", "B")
    sha_b = git(up, "rev-parse", "HEAD")
    url = "file://" + up

    # --- the superproject ----------------------------------------------------
    sup = os.path.join(tmp, "super")
    os.makedirs(sup)
    git(sup, "init", "-q")
    write(os.path.join(sup, "CMakeLists.txt"),
          "cmake_minimum_required(VERSION 3.20)\n"
          "project(fx NONE)\n"
          'include("%s")\n'
          "set(_sparse)\n"
          "if(FX_SPARSE)\n"
          "    set(_sparse SPARSE ${FX_SPARSE})\n"
          "endif()\n"
          "set(_probe)\n"
          "if(FX_PROBE)\n"
          "    set(_probe PROBE ${FX_PROBE})\n"
          "endif()\n"
          "fetch_upstream(FX_DIR URL ${FX_URL} COMMIT ${FX_COMMIT} ${_probe} ${_sparse})\n"
          'message(STATUS "FX_DIR=${FX_DIR}")\n' % HELPER)
    dest = os.path.join(sup, "upstream", "demo-repo-" + sha_a)
    runs = [0]

    def configure(commit=sha_a, probe="Inc/a.h", sparse="/Inc/;/Src/", u=None,
                  extra_env=None):
        runs[0] += 1
        b = os.path.join(tmp, "b%d" % runs[0])
        r = subprocess.run(
            ["cmake", "-S", sup, "-B", b, "-G", "Ninja",
             "-DFX_URL=" + (u or url), "-DFX_COMMIT=" + commit,
             "-DFX_PROBE=" + probe, "-DFX_SPARSE=" + sparse],
            capture_output=True, text=True,
            env=dict(os.environ, **(extra_env or {})))
        return r.returncode, r.stdout + r.stderr, b

    def passes(name, want_fetch, **kw):
        rc, out, b = configure(**kw)
        fetched = "fetching demo-repo" in out
        check(name, rc == 0 and fetched == want_fetch,
              "rc=%d fetched=%s" % (rc, fetched) + ("" if rc == 0 else "\n" + out))
        return b

    def refuses(name, needle, **kw):
        rc, out, _ = configure(**kw)
        out = " ".join(out.split())   # CMake wraps its messages at any space
        check(name, rc != 0 and needle in out,
              "rc=%d, wants '%s'" % (rc, needle) + ("" if rc != 0 and needle in out
                                                    else "\n" + out))

    # --- arguments -------------------------------------------------------------
    refuses("abbreviated commit", "full 40-digit SHA", commit=sha_a[:12])
    refuses("tag name as commit", "is not a commit SHA", commit="v1.0")
    refuses("no PROBE", "at least one PROBE", probe="")
    refuses("commit upstream lacks", "failed", commit="0" * 40)
    check("  ... publishes nothing", not os.path.exists(
        os.path.join(sup, "upstream", "demo-repo-" + "0" * 40)), "no checkout left")

    # --- fresh fetch, then offline ----------------------------------------------
    b = passes("fresh fetch", True)
    check("  ... sparse took effect",
          os.path.isfile(os.path.join(dest, "Inc", "a.h"))
          and not os.path.exists(os.path.join(dest, "docs")), dest)
    check("  ... HEAD file is a configure dep",
          os.path.join(dest, ".git", "HEAD") in open(os.path.join(b, "build.ninja")).read(),
          "build.ninja")
    os.rename(up, up + ".away")
    passes("second configure is offline", False)
    os.rename(up + ".away", up)

    refuses("probe outside sparse set", "has no docs/big.txt", probe="docs/big.txt")
    passes("  ... same tree, probe inside", False)

    # --- a tree that was moved or edited is refused, never repaired ------------
    git(dest, "fetch", "-q", "--depth", "1", "origin", sha_b)
    git(dest, "checkout", "-q", "--detach", sha_b)
    refuses("checked out to another commit", "not at the pin")
    check("  ... not repaired", git(dest, "rev-parse", "HEAD") == sha_b, "HEAD stays")
    git(dest, "checkout", "-q", "--detach", sha_a)
    passes("  ... back at the pin", False)

    os.remove(os.path.join(dest, "Inc", "a.h"))
    refuses("probe deleted", "differs from the pinned commit")
    git(dest, "checkout", "--", ".")
    passes("  ... probe restored", False)

    write(os.path.join(dest, "Src", "a.c"), "int a = 1;\n")
    refuses("tracked file edited", "differs from the pinned commit")
    git(dest, "checkout", "--", ".")
    write(os.path.join(dest, "Inc", "shadow.h"), "\n")
    refuses("untracked file added", "differs from the pinned commit")
    os.remove(os.path.join(dest, "Inc", "shadow.h"))
    write(os.path.join(dest, "Inc", "shadow.gen.h"), "\n")
    refuses("ignored file added", "differs from the pinned commit")
    os.remove(os.path.join(dest, "Inc", "shadow.gen.h"))
    passes("  ... cleaned", False)

    # --- what git status cannot see ---------------------------------------------
    git(dest, "checkout", "-q", "-b", "local")
    refuses("branch checked out at the pin", "is the branch refs/heads/local")
    git(dest, "checkout", "-q", "--detach", sha_a)
    git(dest, "branch", "-q", "-D", "local")
    passes("  ... detached again", False)

    git(dest, "update-index", "--assume-unchanged", "Src/a.c")
    write(os.path.join(dest, "Src", "a.c"), "int a = 1;\n")
    check("  (status is blind to it)", git(dest, "status", "--porcelain") == "", "")
    refuses("assume-unchanged edit", "assume-unchanged")
    git(dest, "update-index", "--no-assume-unchanged", "Src/a.c")
    git(dest, "checkout", "--", ".")
    passes("  ... mark dropped", False)

    git(dest, "update-index", "--skip-worktree", "Src/a.c")
    os.remove(os.path.join(dest, "Src", "a.c"))
    check("  (status is blind to it)", git(dest, "status", "--porcelain") == "", "")
    refuses("skip-worktree inside the patterns", "do not match its sparse patterns")
    git(dest, "update-index", "--no-skip-worktree", "Src/a.c")
    git(dest, "checkout", "--", ".")
    passes("  ... mark dropped", False)

    git(dest, "sparse-checkout", "set", "--no-cone", "/Inc/")
    refuses("narrowed sparse set", "not the SPARSE")
    # The same narrowing with the old patterns written back: the list matches,
    # the marks do not.
    write(os.path.join(dest, ".git", "info", "sparse-checkout"), "/Inc/\n/Src/\n")
    check("  (list restored, Src still gone)",
          not os.path.exists(os.path.join(dest, "Src")), "")
    refuses("marks narrower than the list", "do not match its sparse patterns")
    git(dest, "sparse-checkout", "set", "--no-cone", "/Inc/", "/Src/")
    passes("  ... patterns restored", False)
    refuses("sparse tree, no SPARSE asked", "no SPARSE patterns were asked for",
            sparse="")

    # A replaced pin: rev-parse still says the pin, the files are the impostor's.
    tree = git(dest, "write-tree")
    blob = subprocess.run(["git", "-C", dest, "hash-object", "-w", "--stdin"],
                          input="#define A 666\n", check=True, capture_output=True,
                          text=True).stdout.strip()
    git(dest, "update-index", "--cacheinfo", "100644,%s,Inc/a.h" % blob)
    fake = git(dest, "commit-tree", git(dest, "write-tree"), "-m", "impostor")
    git(dest, "read-tree", tree)
    git(dest, "replace", sha_a, fake)
    git(dest, "checkout", "-q", "-f", "--detach", sha_a)
    check("  (impostor checked out)",
          "666" in open(os.path.join(dest, "Inc", "a.h")).read()
          and git(dest, "status", "--porcelain") == "", "")
    refuses("git replace of the pin", "differs from the pinned commit")
    git(dest, "replace", "-d", sha_a)
    git(dest, "checkout", "-q", "-f", "--detach", sha_a)
    passes("  ... replacement deleted", False)

    # Not sparse, and asked for SPARSE: refused the other way round.
    passes("unsparse fetch (no SPARSE)", True, commit=sha_b, sparse="")
    refuses("unsparse tree, SPARSE asked", "not the SPARSE", commit=sha_b)

    # --- GIT_DIR from the environment ------------------------------------------
    def super_state():
        g = os.path.join(sup, ".git")
        return (open(os.path.join(g, "config")).read(),
                open(os.path.join(g, "HEAD")).read(),
                sorted(os.listdir(os.path.join(g, "refs", "heads"))),
                os.path.exists(os.path.join(g, "shallow")))
    before = super_state()
    genv = {"GIT_DIR": os.path.join(sup, ".git"), "GIT_WORK_TREE": sup}
    passes("GIT_DIR set, existing tree", False, extra_env=genv)
    shutil.rmtree(dest)
    passes("GIT_DIR set, fresh fetch", True, extra_env=genv)
    check("  ... landed in its own checkout",
          os.path.isfile(os.path.join(dest, ".git", "HEAD"))
          and git(dest, "rev-parse", "HEAD") == sha_a, dest)
    check("  ... superproject untouched", super_state() == before, "config/HEAD/refs")

    # --- a deleted tree is fetched again --------------------------------------
    shutil.rmtree(dest)
    os.makedirs(dest)
    refuses("empty dir inside the superproject", "not a git checkout of its own")
    os.rmdir(dest)
    write(os.path.join(dest + ".partial", "junk"), "an interrupted fetch\n")
    passes("deleted tree, leftover staging", True)
    check("  ... staging gone", not os.path.exists(dest + ".partial"), dest + ".partial")

    shutil.rmtree(dest)
    keep = os.path.join(tmp, "keep")
    write(os.path.join(keep, "precious"), "\n")
    os.symlink(keep, dest + ".partial")
    refuses("staging is a symlink", "is a symlink")
    check("  ... link and target kept",
          os.path.islink(dest + ".partial")
          and os.path.isfile(os.path.join(keep, "precious")), keep)
    os.remove(dest + ".partial")
    passes("  ... link removed", True)

    # --- the lock ---------------------------------------------------------------
    hold = os.path.join(tmp, "hold.cmake")
    write(hold, 'file(LOCK "%s.lock" RESULT_VARIABLE r)\n'
                'execute_process(COMMAND "${CMAKE_COMMAND}" -E echo held)\n'
                'execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 4)\n' % dest)
    holder = subprocess.Popen(["cmake", "-P", hold], stdout=subprocess.PIPE, text=True)
    line = holder.stdout.readline().strip()
    t0 = time.monotonic()
    rc, out, _ = configure()
    took = time.monotonic() - t0
    holder.wait()
    check("second configure waits for the lock",
          line == "held" and rc == 0 and "waiting for another configure" in out
          and took >= 2.5, "holder=%r rc=%d took=%.1fs" % (line, rc, took))
    rc, out, _ = configure()
    check("  ... and does not wait without it",
          rc == 0 and "waiting" not in out, "rc=%d" % rc)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if bad:
    print("fetch_upstream tests: %d FAILED" % bad)
    sys.exit(1)
print("fetch_upstream tests passed")
