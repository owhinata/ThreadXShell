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
    untracked file, a file upstream's .gitignore hides and an empty directory
    standing in for the checkout are each FATAL, and are not repaired or
    re-fetched;
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

    def configure(commit=sha_a, probe="Inc/a.h", sparse="/Inc/;/Src/", u=None):
        runs[0] += 1
        b = os.path.join(tmp, "b%d" % runs[0])
        r = subprocess.run(
            ["cmake", "-S", sup, "-B", b, "-G", "Ninja",
             "-DFX_URL=" + (u or url), "-DFX_COMMIT=" + commit,
             "-DFX_PROBE=" + probe, "-DFX_SPARSE=" + sparse],
            capture_output=True, text=True)
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

    # --- a deleted tree is fetched again --------------------------------------
    shutil.rmtree(dest)
    os.makedirs(dest)
    refuses("empty dir inside the superproject", "not a git checkout of its own")
    os.rmdir(dest)
    write(os.path.join(dest + ".partial", "junk"), "an interrupted fetch\n")
    passes("deleted tree, leftover staging", True)
    check("  ... staging gone", not os.path.exists(dest + ".partial"), dest + ".partial")

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
