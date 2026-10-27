#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""The pinned fetches ignore the git environment they inherit (issue #158).

cmake/fetch_model.cmake (the model pin, run by ninja) and the Grove SDK fetch
(boards/grove-vision-ai-v2/cmake/himax_sdk_fetch.cmake, run at configure) start
git through FETCH_UPSTREAM_GIT (cmake/fetch_upstream.cmake).  This runs both
against small file:// upstreams, from inside a throwaway superproject (itself a
git repository, as this one is), with no network.  Needs git and cmake only.

  - GIT_DIR + GIT_WORK_TREE, GIT_INDEX_FILE, GIT_OBJECT_DIRECTORY and
    GIT_COMMON_DIR, each pointed at the superproject: the model is fetched and
    verified, the SDK tree is checked when it exists and fetched when it does
    not, and the superproject's HEAD, config, refs, index, objects, shallow file
    and working tree are byte for byte what they were; with
    GIT_ALTERNATE_OBJECT_DIRECTORIES naming a store that holds the pin, the
    fetched SDK tree still owns every object it checked out;
  - GIT_CONFIG_PARAMETERS (what `git -c` exports) and GIT_CONFIG_COUNT /
    GIT_CONFIG_KEY_<n> / GIT_CONFIG_VALUE_<n>, each carrying a core.hooksPath
    with a post-checkout hook and a url.<x>.insteadOf that retargets the
    upstream at a mirror: the hook does not run and the mirror is never read
    (GIT_TRACE names every upload-pack git starts).

[!] EVERY CASE IS PAIRED WITH THE SAME FETCH PASSING WITH A CLEAN ENVIRONMENT,
and the mirror holds the same commit, so a retargeted fetch would succeed --
only the trace can tell the two apart, which is the point: the hash cannot.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FETCH_MODEL = os.path.join(REPO, "cmake", "fetch_model.cmake")
SDK_FETCH = os.path.join(REPO, "boards", "grove-vision-ai-v2", "cmake",
                         "himax_sdk_fetch.cmake")
SDK_PROBES = ["EPII_CM55M_APP_S/device/WE2_core.c",
              "EPII_CM55M_APP_S/prebuilt_libs/gnu/libdriver.a",
              "we2_image_gen_local/we2_local_image_gen",
              "xmodem/xmodem_send.py"]

bad = 0

# Nothing git-shaped from the caller's shell reaches the harness's own git
# calls or the baseline runs; each case adds exactly what it tests.
CLEAN = {k: v for k, v in os.environ.items()
         if not k.startswith("GIT_")}
CLEAN.update(GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
             GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")


def check(name, ok, why):
    global bad
    print("  %s %-44s %s" % ("ok  " if ok else "FAIL", name, why))
    if not ok:
        bad += 1


def git(cwd, *args):
    return subprocess.run(["git", "--no-optional-locks", "-C", cwd, *args],
                          check=True, capture_output=True, text=True,
                          env=CLEAN).stdout.strip()


def ask(cwd, *args):
    """git, where a failure is an answer (a superproject a broken case left
    unreadable must still compare unequal, not stop the run)."""
    r = subprocess.run(["git", "--no-optional-locks", "-C", cwd, *args],
                       capture_output=True, text=True, env=CLEAN)
    return (r.returncode, r.stdout.strip())


def write(path, text, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    if mode:
        os.chmod(path, mode)


def tree_digest(root, skip=()):
    """Every file under root (paths, bytes), minus the top-level names in skip."""
    h = hashlib.sha256()
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == ".":
            dirs[:] = [x for x in dirs if x not in skip]
            files = [x for x in files if x not in skip]
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            h.update(os.path.relpath(p, root).encode() + b"\0")
            if os.path.islink(p):
                h.update(b"L" + os.readlink(p).encode())
            else:
                with open(p, "rb") as fh:
                    h.update(fh.read())
    return h.hexdigest()


tmp = tempfile.mkdtemp(prefix="git_env_")
try:
    def make_upstream(name, files):
        up = os.path.join(tmp, "remote", name)
        os.makedirs(up)
        git(up, "init", "-q")
        git(up, "config", "uploadpack.allowFilter", "true")
        git(up, "config", "uploadpack.allowAnySHA1InWant", "true")
        for rel, text in files.items():
            write(os.path.join(up, rel), text)
        git(up, "add", "-A")
        git(up, "commit", "-q", "-m", "pin")
        sha = git(up, "rev-parse", "HEAD")
        # The mirror holds the SAME commit, so a fetch retargeted at it
        # succeeds and only the trace shows where it went.
        mirror = up + "-mirror"
        subprocess.run(["git", "clone", "-q", "--bare", up, mirror], check=True,
                       env=CLEAN)
        git(mirror, "config", "uploadpack.allowFilter", "true")
        git(mirror, "config", "uploadpack.allowAnySHA1InWant", "true")
        return up, mirror, sha

    model_bytes = "not really a flatbuffer\n"
    m_up, m_mirror, m_sha = make_upstream(
        "models", {"vision/m.tflite": model_bytes, "other/big.bin": "x\n"})
    model_sha256 = hashlib.sha256(model_bytes.encode()).hexdigest()
    s_up, s_mirror, s_sha = make_upstream(
        "sdk", {p: "probe\n" for p in SDK_PROBES})

    # --- the superproject: a repository with a commit, like this one ----------
    sup = os.path.join(tmp, "super")
    os.makedirs(sup)
    git(sup, "init", "-q")
    write(os.path.join(sup, ".gitignore"), "build/\nsdk/\n")
    write(os.path.join(sup, "README"), "superproject\n")
    git(sup, "add", "-A")
    git(sup, "commit", "-q", "-m", "super")
    pristine = os.path.join(tmp, "super-pristine")
    shutil.copytree(sup, pristine, symlinks=True)
    sdk_root = os.path.join(sup, "sdk")

    def restore():
        keep = os.path.join(tmp, "sdk-kept")
        if os.path.exists(sdk_root):
            os.rename(sdk_root, keep)
        shutil.rmtree(sup)
        shutil.copytree(pristine, sup, symlinks=True)
        if os.path.exists(keep):
            os.rename(keep, sdk_root)
    sdk_script = os.path.join(tmp, "sdk_fetch.cmake")
    write(sdk_script,
          'include("%s")\n'
          'grove_sdk_fetch("${ROOT}" "${URL}" "${PIN}")\n'
          'grove_sdk_probe("${ROOT}")\n'
          'message(STATUS "SDK_OK")\n' % SDK_FETCH)
    hooks = os.path.join(tmp, "hooks")
    hook_ran = os.path.join(tmp, "hook-ran")
    write(os.path.join(hooks, "post-checkout"),
          "#!/bin/sh\necho ran >> '%s'\n" % hook_ran, 0o755)
    trace = os.path.join(tmp, "trace")

    def super_state():
        g = os.path.join(sup, ".git")
        # The working tree outside the ignored build/ and sdk/, and all of .git
        # but the files git itself rewrites when it merely reads (none here:
        # every harness call is --no-optional-locks).
        return (tree_digest(sup, skip=(".git", "build", "sdk")),
                tree_digest(g),
                ask(sup, "rev-parse", "HEAD"),
                ask(sup, "status", "--porcelain", "--ignored=no"))

    def run(args, extra):
        env = dict(CLEAN, GIT_TRACE=trace, **extra)
        if os.path.exists(trace):
            os.remove(trace)
        r = subprocess.run(["cmake", *args], capture_output=True, text=True,
                           env=env, cwd=sup)
        return r.returncode, r.stdout + r.stderr

    def fetch_model(extra):
        out = os.path.join(sup, "build", "model", "fetched.tflite")
        if os.path.exists(out):
            os.remove(out)
        rc, log = run(["-DURL=file://" + m_up, "-DCOMMIT=" + m_sha,
                       "-DPATH_IN=vision/m.tflite", "-DSHA256=" + model_sha256,
                       "-DOUT=" + out,
                       "-DWORK=" + os.path.join(sup, "build", "model", "work"),
                       "-DOVERRIDE=FX_MODEL_OVERRIDE", "-P", FETCH_MODEL], extra)
        ok = (rc == 0 and os.path.isfile(out)
              and open(out).read() == model_bytes)
        return ok, rc, log

    def fetch_sdk(extra, fresh):
        if fresh and os.path.exists(sdk_root):
            shutil.rmtree(sdk_root)
        rc, log = run(["-DROOT=" + sdk_root, "-DURL=file://" + s_up,
                       "-DPIN=" + s_sha, "-P", sdk_script], extra)
        fetched = "Fetching Himax WiseEye2 SDK" in log
        ok = (rc == 0 and "SDK_OK" in log and fetched == fresh
              and ask(sdk_root, "rev-parse", "HEAD") == (0, s_sha)
              and ask(sdk_root, "rev-parse", "--show-toplevel")
              == (0, os.path.realpath(sdk_root))
              # every object it checked out is its own, not borrowed from an
              # alternate the environment named for one run
              and ask(sdk_root, "fsck", "--connectivity-only")[0] == 0)
        return ok, rc, log

    def traced():
        return open(trace).read() if os.path.exists(trace) else ""

    # Each fetch: model, SDK fetched, SDK existing.
    fetches = [
        ("model fetch", lambda e: fetch_model(e)),
        ("SDK fresh fetch", lambda e: fetch_sdk(e, True)),
        ("SDK existing tree", lambda e: fetch_sdk(e, False)),
    ]

    # --- baseline: a clean environment ----------------------------------------
    for name, f in fetches:
        ok, rc, log = f({})
        check("clean env: " + name, ok, "rc=%d" % rc + ("" if ok else "\n" + log))
    ok, rc, log = fetch_sdk({}, True)
    check("  ... trace names the real upstream",
          "'%s'" % s_up in traced(), "GIT_TRACE works")

    # --- the repository-redirecting variables ---------------------------------
    gdir = os.path.join(sup, ".git")
    redirects = [
        ("GIT_DIR+GIT_WORK_TREE", {"GIT_DIR": gdir, "GIT_WORK_TREE": sup}),
        ("GIT_INDEX_FILE", {"GIT_INDEX_FILE": os.path.join(gdir, "index")}),
        ("GIT_OBJECT_DIRECTORY",
         {"GIT_OBJECT_DIRECTORY": os.path.join(gdir, "objects")}),
        ("GIT_COMMON_DIR", {"GIT_COMMON_DIR": gdir}),
        # Borrowing, not writing: a fetch that finds the pin in an alternate
        # takes nothing over the wire, and the checkout is left without its
        # objects once the variable is gone.  The model's staging is deleted
        # after the hash, so only the SDK tree can show it.
        ("GIT_ALTERNATE_OBJECT_DIRECTORIES",
         {"GIT_ALTERNATE_OBJECT_DIRECTORIES":
          os.path.join(tmp, "remote", "sdk", ".git", "objects")}),
    ]
    for vname, env in redirects:
        for name, f in fetches:
            before = super_state()
            ok, rc, log = f(env)
            after = super_state()
            check("%s: %s" % (vname, name), ok,
                  "rc=%d" % rc + ("" if ok else "\n" + log[-1500:]))
            check("  ... superproject untouched", after == before,
                  "HEAD/config/refs/index/objects/worktree")
            if after != before:
                restore()   # so the next case starts from the same superproject

    # --- configuration from the environment -----------------------------------
    def cfg_cases():
        pairs = [("core.hooksPath", hooks),
                 ("url.file://%s.insteadOf" % m_mirror, "file://" + m_up),
                 ("url.file://%s.insteadOf" % s_mirror, "file://" + s_up)]
        params = " ".join("'%s'='%s'" % kv for kv in pairs)
        count = {"GIT_CONFIG_COUNT": str(len(pairs))}
        for i, (k, v) in enumerate(pairs):
            count["GIT_CONFIG_KEY_%d" % i] = k
            count["GIT_CONFIG_VALUE_%d" % i] = v
        return [("GIT_CONFIG_PARAMETERS", {"GIT_CONFIG_PARAMETERS": params}),
                ("GIT_CONFIG_COUNT", count)]

    # The injection itself works: a bare git with the same environment does
    # retarget and does run the hook (otherwise the cases below prove nothing).
    for vname, env in cfg_cases():
        probe = os.path.join(tmp, "probe-" + vname)
        if os.path.exists(hook_ran):
            os.remove(hook_ran)
        e = dict(CLEAN, GIT_TRACE=trace, **env)
        if os.path.exists(trace):
            os.remove(trace)
        subprocess.run(["git", "clone", "-q", "file://" + m_up, probe],
                       check=True, capture_output=True, env=e)
        check("(bare git obeys %s)" % vname,
              "'%s'" % m_mirror in traced() and os.path.exists(hook_ran),
              "mirror read, hook ran")

    for vname, env in cfg_cases():
        for name, f in fetches:
            if os.path.exists(hook_ran):
                os.remove(hook_ran)
            ok, rc, log = f(env)
            t = traced()
            check("%s: %s" % (vname, name), ok,
                  "rc=%d" % rc + ("" if ok else "\n" + log[-1500:]))
            if name != "SDK existing tree":
                check("  ... fetched from the pinned URL",
                      "'%s'" % (m_up if name == "model fetch" else s_up) in t,
                      "trace")
            check("  ... the mirror was not read",
                  "'%s'" % m_mirror not in t and "'%s'" % s_mirror not in t,
                  "trace")
            check("  ... the injected hook did not run",
                  not os.path.exists(hook_ran), hook_ran)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if bad:
    print("git environment tests: %d FAILED" % bad)
    sys.exit(1)
print("git environment tests passed")
