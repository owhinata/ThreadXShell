#!/usr/bin/env python3
"""Audit stateless services using their actual board compile commands (#117).

Coverage is derived twice: CMake's evaluated target sources, and its compile DB.
Their (producer, source) sets must agree before any exception is applied. We only
support ordinary single-config Ninja/GCC compiles; unknown command forms fail
closed. This is a build-integrity check, not a defense against malicious CMake.

C++ (#161): a TU compiled by the build's C++ driver is replayed with that driver.
The driver must be exactly the C or the C++ compiler CMake was configured with,
and the source suffix must belong to that driver's language (a .c by the C++
driver, or a .cc by the C driver, is refused rather than guessed at). An explicit
`-x <lang>` must name that same language too, so `gcc -x c++ x.c` cannot compile
C++ under the C pairing; any other -x value is refused. A C++
global constructor lands in .init_array, a writable section, so the unchanged
section rule in check_no_mutable_storage.py refuses it like any other storage.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
# The shared commands outside svc/: in scope like svc/, and each must be compiled
# by every board (a board that lost one would otherwise pass with less audited).
# `camera` joined `nn` in issue #143.
SHARED_COMMANDS = ("shell/cmds/cmd_nn.c", "shell/cmds/nn_cmd_core.c",
                   "shell/cmds/cmd_camera.c")
# Existing protocol staging buffers and last-run diagnostics; moving these out
# would change ownership/reentrancy and is not part of the stateless nn contract.
EXCEPTIONS = {"svc/ymodem.c": "existing s_block, s_rdata and s_diag (non-reentrant protocol)"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".s", ".S"}
# Which suffixes each driver may compile. Together these are SOURCE_SUFFIXES.
# The C driver keeps the assembly suffixes it always accepted (CMake's ASM
# compiler is the same gcc); C++ sources are accepted only from the C++ driver.
DRIVER_SUFFIXES = {"C": {".c", ".s", ".S"}, "C++": {".cc", ".cpp", ".cxx"}}
ASM_SUFFIXES = {".s", ".S"}


def x_language_ok(language, suffix, value):
    """Whether `-x value` keeps the driver/suffix pairing true (#161 review).

    The C driver may say `c`, and `assembler`/`assembler-with-cpp` only for an
    assembly suffix; the C++ driver may say `c++`. Nothing else (c-header,
    none, objective-c, a C++ value under the C driver, ...) is accepted.
    """
    if language == "C++":
        return value == "c++"
    if value == "c":
        return suffix == ".c"
    if value in ("assembler", "assembler-with-cpp"):
        return suffix in ASM_SUFFIXES
    return False
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc"}
SAFE_F = {
    "-fdata-sections", "-ffunction-sections", "-fno-common", "-fcommon",
    "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    "-fno-tree-vectorize", "-funroll-loops", "-fno-unroll-loops",
    "-fomit-frame-pointer", "-fno-omit-frame-pointer", "-fno-builtin",
    "-ffreestanding", "-fno-strict-aliasing", "-fstrict-aliasing",
    "-fno-lto", "-flto", "-fshort-enums", "-fno-short-enums",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-fsigned-char", "-funsigned-char", "-fwrapv", "-fno-wrapv",
    "-fno-inline", "-fstack-usage", "-fno-delete-null-pointer-checks",
    # C++ only (#161): static destructors are not registered through
    # __cxa_atexit. Storage that would need one is still refused by section.
    "-fno-use-cxa-atexit",
}


class AuditError(Exception):
    pass


def need(condition, message):
    if not condition:
        raise AuditError(message)


def canonical(path, directory):
    p = Path(path)
    return (p if p.is_absolute() else Path(directory) / p).resolve()


def policy_source(path, root):
    """Return the in-scope canonical source, without applying exceptions."""
    path = Path(path).resolve()
    if path in {root / n for n in SHARED_COMMANDS}:
        return True
    return path.is_relative_to(root / "svc")


def inventory(directory, root):
    directory = Path(directory)
    names = (directory / "index.txt").read_text().splitlines()
    need(names and len(names) == len(set(names)), "empty or duplicate target inventory")
    targets, wanted = {}, set()
    for name in names:
        need(re.fullmatch(r"[A-Za-z0-9_.+-]+\.txt", name), "invalid inventory filename")
        rows = (directory / name).read_text().splitlines()
        need(len(rows) >= 5 and rows[3] == "[sources]" and "[header_only]" in rows,
             f"malformed inventory {name}")
        target, source_dir, binary_dir = rows[:3]
        need(name == target + ".txt" and target not in targets, "duplicate/invalid target identity")
        need(Path(source_dir).is_absolute() and Path(binary_dir).is_absolute(), "relative target directory")
        split = rows.index("[header_only]", 4)
        sources, headers = rows[4:split], rows[split + 1:]
        need(not any("$<" in s or ";" in s for s in sources + headers),
             f"unsupported unresolved source expression in {target}")
        targets[target] = Path(binary_dir).resolve() / "CMakeFiles" / (target + ".dir")
        excluded = {canonical(h, source_dir) for h in headers if h}
        for spelling in sources:
            if not spelling:
                continue
            src = canonical(spelling, source_dir)
            # Both lexical and physical service paths must be classified. A
            # symlink under svc/ may not silently exempt a file outside svc/.
            lexical = Path(os.path.abspath(os.path.join(source_dir, spelling)))
            if lexical.is_relative_to(root / "svc"):
                need(policy_source(src, root), f"service symlink escapes scope: {spelling}")
            if not policy_source(src, root):
                continue
            if src in excluded or src.suffix in HEADER_SUFFIXES:
                continue
            need(src.suffix in SOURCE_SUFFIXES, f"unsupported service source type: {src}")
            need(src.is_file(), f"missing service source: {src}")
            wanted.add((target, src))
    need(wanted, "no stateless-service inventory entries")
    return targets, wanted


def entry_argv(entry):
    if "arguments" in entry:
        argv = entry["arguments"]
        need(isinstance(argv, list) and argv and all(isinstance(a, str) for a in argv),
             "invalid compile arguments")
        return argv
    need(isinstance(entry.get("command"), str), "missing compile command")
    try:
        argv = shlex.split(entry["command"])
    except ValueError as exc:
        raise AuditError(f"malformed compile command: {exc}") from exc
    need(argv, "empty compile command")
    return argv


def output_of(argv):
    result = []
    for i, a in enumerate(argv):
        if a == "-o":
            need(i + 1 < len(argv), "missing -o argument")
            result.append(argv[i + 1])
        elif a.startswith("-o") and len(a) > 2:
            result.append(a[2:])
    need(len(result) == 1, "compile command needs exactly one output")
    return result[0]


def select(database, targets, wanted, root):
    entries = json.loads(Path(database).read_text())
    need(isinstance(entries, list) and entries, "empty/invalid compile database")
    selected, seen, outputs = [], set(), set()
    for entry in entries:
        need(isinstance(entry, dict) and isinstance(entry.get("file"), str)
             and isinstance(entry.get("directory"), str), "malformed compile database record")
        cwd = Path(entry["directory"])
        need(cwd.is_absolute() and cwd.is_dir(), "invalid compile working directory")
        src = canonical(entry["file"], cwd)
        if not policy_source(src, root):
            continue
        argv = entry_argv(entry)
        output = canonical(output_of(argv), cwd)
        if "output" in entry:
            need(isinstance(entry["output"], str) and canonical(entry["output"], cwd) == output,
                 f"compile output disagrees with command: {src}")
        owners = [t for t, d in targets.items() if output.is_relative_to(d)]
        need(len(owners) == 1, f"unrecognized/ambiguous compile producer: {output}")
        key = (owners[0], src)
        need(output not in outputs, f"duplicate compile output: {output}")
        outputs.add(output)
        seen.add(key)
        selected.append((key, entry, argv))
    missing, extra = wanted - seen, seen - wanted
    need(not missing and not extra,
         "inventory/database coverage mismatch; missing=" + repr(sorted(map(str, missing)))
         + "; unexpected=" + repr(sorted(map(str, extra))))
    for cmd in SHARED_COMMANDS:
        need(any(src == root / cmd for _, src in seen), f"mandatory command missing: {cmd}")
    return selected


def driver_language(argv0, source, cwd, cc, cxx=None):
    """Name the driver's language, refusing unknown drivers and mismatches.

    [!] Only the two configured compilers are drivers. Anything else (a
    launcher, another compiler, the C++ driver when none was configured) is
    refused, and so is a source whose suffix belongs to the other language.
    """
    driver = canonical(argv0, cwd)
    if driver == cc:
        language = "C"
    elif cxx is not None and driver == cxx:
        language = "C++"
    else:
        raise AuditError("unsupported compiler/launcher (neither the configured C "
                         "nor C++ compiler): " + argv0)
    need(Path(source).suffix in DRIVER_SUFFIXES[language],
         f"source/driver mismatch: {Path(source).name} compiled by the {language} driver")
    return language


def replay_argv(argv, source, cwd, cc, destination, cxx=None):
    language = driver_language(argv[0], source, cwd, cc, cxx)
    result, inputs, compile_count = [argv[0]], [], 0
    i = 1
    while i < len(argv):
        a = argv[i]
        need(a and not a.startswith("@"), "response files/empty arguments are unsupported")
        if a in ("-o", "-MF", "-MT", "-MQ"):
            need(i + 1 < len(argv), f"missing argument for {a}")
            i += 2
            continue
        if any(a.startswith(p) and a != p for p in ("-o", "-MF", "-MT", "-MQ")):
            i += 1
            continue
        if a in ("-MD", "-MMD", "-MP", "-MG"):
            i += 1
            continue
        if a == "-c":
            compile_count += 1
            result.append(a)
        elif a in ("-D", "-U", "-I", "-isystem", "-iquote", "-include", "-imacros", "-x", "-isysroot"):
            need(i + 1 < len(argv) and argv[i + 1] and not argv[i + 1].startswith("@"),
                 f"missing/unsupported argument for {a}")
            # Joined `-xc++` never reaches here: it is an unsupported option.
            need(a != "-x" or x_language_ok(language, Path(source).suffix, argv[i + 1]),
                 f"-x {argv[i + 1]} does not match the {language} driver for {Path(source).name}")
            result.extend(argv[i:i + 2])
            i += 2
            continue
        elif not a.startswith("-"):
            need(canonical(a, cwd) == source, f"unexpected compiler input: {a}")
            inputs.append(a)
            result.append(a)
        elif (a in SAFE_F or re.fullmatch(r"-flto(?:=[0-9]+|=auto)?", a)
              or re.fullmatch(r"-O(?:[0-3gsz]|fast)", a)
              or re.fullmatch(r"-g(?:[0-3]|gdb[0-3]?|dwarf-[2-5])?", a)
              or re.fullmatch(r"-m(?:cpu=[A-Za-z0-9_.+-]+|arch=[A-Za-z0-9_.+-]+|fpu=[A-Za-z0-9_.+-]+|float-abi=(?:soft|softfp|hard)|thumb|arm|cmse|no-unaligned-access|unaligned-access)", a)
              or a.startswith(("-D", "-U", "-I", "-isystem", "-iquote", "-std="))
              or re.fullmatch(r"-W(?:all|extra|error(?:=[A-Za-z0-9_-]+)?|no-[A-Za-z0-9_-]+|[A-Za-z0-9_-]+)", a)
              or a in ("-pedantic", "-pedantic-errors", "-nostdinc", "-nostdinc++", "-pthread")):
            result.append(a)
        else:
            raise AuditError(f"unsupported compile option: {a}")
        i += 1
    need(compile_count == 1 and len(inputs) == 1, "expected one direct source compile")
    # Last option wins even over per-source LTO flags. Auxiliary .su output from
    # -fstack-usage follows this private output; never the shipping object.
    result += ["-fno-lto", "-o", str(destination)]
    return result


def run(cmd, cwd, input_text=None):
    done = subprocess.run(cmd, cwd=cwd, input=input_text, capture_output=True, text=True)
    need(done.returncode == 0,
         f"command failed ({done.returncode}): {shlex.join(cmd)}\n{done.stdout}{done.stderr}")
    return done.stdout


def audit(args):
    root, cc = Path(args.root).resolve(), Path(args.cc).resolve()
    cxx = Path(args.cxx).resolve() if args.cxx else None
    report = {"status": "running", "entries": []}
    report_path = Path(args.report) if args.report else None
    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        # A diagnostic report is never an acceptance token. Discard the previous
        # result before compilation so even consumers of the report see failure.
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    need(cc.is_file(), f"missing compiler: {cc}")
    if cxx is not None:
        need(cxx.is_file(), f"missing C++ compiler: {cxx}")
        need(cxx != cc, "the C and C++ compilers must be distinct drivers")
    targets, wanted = inventory(args.inventory, root)
    selected = select(args.database, targets, wanted, root)
    with tempfile.TemporaryDirectory(prefix="shared-storage-") as scratch:
        for index, ((target, source), entry, argv) in enumerate(selected):
            relative = source.relative_to(root).as_posix()
            if relative in EXCEPTIONS:
                print(f"shared_storage: EXEMPT {target}: {relative}: {EXCEPTIONS[relative]}")
                report["entries"].append({"target": target, "source": str(source),
                                          "exception": EXCEPTIONS[relative]})
                continue
            obj = Path(scratch) / f"{index}.o"
            cwd = entry["directory"]
            command = replay_argv(argv, source, cwd, cc, obj, cxx)
            # Check the same architecture/definition context, including forced
            # includes. -dM/-E suppress assembly/output; the original TU stays direct.
            macros = run(command[:-2] + ["-dM", "-E", "-o", "-"], cwd)
            need(re.search(r"^#define __arm__\s", macros, re.M),
                 f"not an ARM compile context: {target}: {relative}")
            run(command, cwd)
            sections = run([args.objdump, "-h", str(obj)], cwd)
            need(".gnu.lto_" not in sections and "file format elf32-" in sections,
                 f"audit did not produce native ELF: {relative}")
            message = run([sys.executable, str(HERE / "check_no_mutable_storage.py"),
                           "--objdump", args.objdump, "--nm", args.nm,
                           "--label", f"{target}: {relative}", str(obj)], cwd)
            print(message.rstrip())
            report["entries"].append({"target": target, "source": str(source),
                                      "directory": cwd, "original": argv, "audit": command})
    report["status"] = "passed"
    if report_path:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"shared_storage: OK ({len(selected)} compile contexts, coverage verified)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "database", "inventory", "cc", "objdump", "nm"):
        parser.add_argument("--" + name, required=True)
    # Optional: a build that never enabled C++ has no C++ driver, and then every
    # C++ compile command is refused as an unknown compiler.
    parser.add_argument("--cxx")
    parser.add_argument("--report")
    args = parser.parse_args()
    try:
        audit(args)
    except (AuditError, OSError, ValueError, TypeError) as exc:
        print(f"shared_storage: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
