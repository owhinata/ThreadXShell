#!/usr/bin/env python3
"""Integration/negative tests of derived board-context storage auditing (#117).

Uses a small real CMake/Ninja project and the supplied ARM compiler. Optional
--board-build replays copies of the three shared regression TUs using the
selected consumer's actual compile context (Grove or Wio), without editing or
building any firmware tree.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

CMAKE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("storage_build", CMAKE / "check_shared_storage_build.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def run(command, expected=True, contains=None, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    text = result.stdout + result.stderr
    if (result.returncode == 0) != expected or (contains and contains not in text):
        raise AssertionError(f"unexpected result {result.returncode}: {shlex.join(command)}\n{text}")
    return text


def write(root, file, text):
    path = root / file
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def project(root, args):
    (root / "cmake").mkdir()
    for name in ("shared_storage_gate.cmake", "check_shared_storage_build.py", "check_no_mutable_storage.py"):
        shutil.copy2(CMAKE / name, root / "cmake" / name)
    write(root, "shell/cmds/cmd_nn.c", "int cmd_nn(void) { return 0; }\n")
    write(root, "shell/cmds/nn_cmd_core.c", "int nn_cmd_core(void) { return 0; }\n")
    write(root, "svc/pure.c", "int pure(void) { return 1; }\n")
    write(root, "svc/ymodem.c", "static volatile int state; int transfer(void) { return ++state; }\n")
    write(root, "svc/header_only.c", "#error not compiled\n")
    write(root, "include dir/context.h", "#define CONTEXT 1\n")
    write(root, "svc/conditional.c", "#if !defined(CONTEXT) || !defined(__OPTIMIZE__)\n#error lost context\n#endif\nint conditional(void) { return 0; }\n")
    write(root, "CMakeLists.txt", '''cmake_minimum_required(VERSION 3.20)
project(storage_fixture C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_library(services OBJECT svc/pure.c svc/ymodem.c svc/header_only.c
    "$<$<BOOL:1>:${CMAKE_CURRENT_SOURCE_DIR}/svc/conditional.c>")
set_source_files_properties(svc/header_only.c PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(svc/conditional.c PROPERTIES COMPILE_OPTIONS "-O3;-include;${CMAKE_CURRENT_SOURCE_DIR}/include dir/context.h")
add_executable(shell shell/cmds/cmd_nn.c shell/cmds/nn_cmd_core.c $<TARGET_OBJECTS:services>)
target_link_options(shell PRIVATE -nostdlib -Wl,-e,cmd_nn)
if(DISABLE_EXPORT)
    set_property(TARGET services PROPERTY EXPORT_COMPILE_COMMANDS OFF)
endif()
if(ENABLE_UNITY)
    set_property(TARGET services PROPERTY UNITY_BUILD ON)
endif()
if(EXTRA_SOURCE)
    target_sources(services PRIVATE "${EXTRA_SOURCE}")
endif()
if(SECOND_CONTEXT)
    add_library(second OBJECT svc/pure.c)
    target_compile_definitions(second PRIVATE SECOND_CONTEXT=1)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/shared_storage_gate.cmake")
add_shared_storage_build_gate()
''')


def configure(root, build, args, *extra, expected=True, contains=None):
    return run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_SYSTEM_NAME=Generic", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
                "-DCMAKE_C_COMPILER=" + args.cc, "-DCMAKE_OBJDUMP=" + args.objdump,
                "-DCMAKE_NM=" + args.nm, "-DCMAKE_C_FLAGS=-mcpu=cortex-m7 -mthumb -O2",
                *extra], expected, contains)


def replay_mutations(args):
    database = json.loads((Path(args.board_build) / "compile_commands.json").read_text())
    root = CMAKE.parent.resolve()
    names = ("rect_geom.c", "plugin_exec.c", "plugin_paint_budget.c")
    guards = ("defined(__arm__)", "defined(__OPTIMIZE__)", "__INCLUDE_LEVEL__ == 0")
    for name, guard in zip(names, guards):
        source = root / "svc" / name
        entries = [e for e in database if Path(e["file"]).resolve() == source]
        assert entries, "board does not compile " + name
        for entry in entries:
            with tempfile.TemporaryDirectory(prefix="storage-regression-") as tmp:
                tmp = Path(tmp)
                copy = tmp / name
                copy.write_text(source.read_text() + f"\n#if {guard}\nstatic volatile int injected;\nint storage_regression_probe(void) {{ return ++injected; }}\n#endif\n")
                argv = gate.entry_argv(entry)
                # The copy remains a directly compiled TU. The extra quote path
                # reproduces original source-relative headers after copying.
                argv = [str(copy) if a == str(source) else a for a in argv]
                argv += ["-iquote", str(source.parent)]
                command = gate.replay_argv(argv, copy, entry["directory"], Path(args.cc).resolve(), tmp / "bad.o")
                run(command, cwd=entry["directory"])
                run([sys.executable, str(CMAKE / "check_no_mutable_storage.py"),
                     "--objdump", args.objdump, "--nm", args.nm, str(tmp / "bad.o")], False,
                    "owns mutable storage")
            print("ok board source mutation:", name, guard)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cc", "objdump", "nm"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--board-build")
    args = parser.parse_args()
    args.cc, args.objdump, args.nm = map(lambda p: str(Path(p).resolve()), (args.cc, args.objdump, args.nm))
    with tempfile.TemporaryDirectory(prefix="derived-storage-fixture-") as tmp:
        root, build = Path(tmp) / "source", Path(tmp) / "build"
        root.mkdir()
        project(root, args)
        configure(root, build, args)
        shell = ["cmake", "--build", str(build), "--target", "shell"]
        run(shell, contains="coverage verified")
        report = json.loads((build / "shared-storage/last-audit.json").read_text())
        assert report["status"] == "passed"
        assert any(e.get("exception") for e in report["entries"])
        for entry in report["entries"]:
            if "original" not in entry:
                continue
            original = list(entry["original"])
            output_index = original.index("-o")
            del original[output_index:output_index + 2]
            assert original == entry["audit"][:-3]
            assert entry["audit"][-3:-1] == ["-fno-lto", "-o"]
        print("ok real CMake, conditional/relative sources, header-only, source flags, argv report")
        # Every shell invocation must re-run: the old shipping ELF remains after
        # failure, and must not grant a second invocation permission to pass.
        bad = "\n#if defined(__arm__) && defined(__OPTIMIZE__) && __INCLUDE_LEVEL__ == 0\nstatic volatile int counter; int bump(void) { return ++counter; }\n#endif\n"
        write(root, "svc/pure.c", "int pure(void) { return 1; }\n" + bad)
        run(shell, False, "owns mutable storage")
        run(shell, False, "owns mutable storage")
        write(root, "svc/pure.c", "int pure(void) { return 1; }\n")
        run(shell, contains="coverage verified")
        print("ok direct ARM/optimized source mutation, repeated failure, recovery")
        configure(root, Path(tmp) / "no-export", args, "-DDISABLE_EXPORT=ON", expected=False, contains="must export compile commands")
        configure(root, Path(tmp) / "unity", args, "-DENABLE_UNITY=ON", expected=False, contains="use unity builds")
        print("ok per-target export OFF and unity rejected")
        base = (build / "compile_commands.json").read_text()
        audit = [sys.executable, str(CMAKE / "check_shared_storage_build.py"), "--root", str(root),
                 "--database", str(build / "compile_commands.json"), "--inventory", str(build / "shared-storage/inventory"),
                 "--cc", args.cc, "--objdump", args.objdump, "--nm", args.nm]
        entries = json.loads(base)
        mutations = [
            ("omitted service", [e for e in entries if not e["file"].endswith("/svc/pure.c")], "coverage mismatch"),
            ("empty DB", [], "empty/invalid"),
        ]
        extra = json.loads(base)
        extra.append(dict(extra[0], output="CMakeFiles/ghost.dir/x.o", command=extra[0]["command"].replace(gate.output_of(gate.entry_argv(extra[0])), "CMakeFiles/ghost.dir/x.o")))
        mutations.append(("unknown producer", extra, "unrecognized/ambiguous"))
        # A stale entry with a known producer is still extra membership, even
        # though the source and its output both look otherwise plausible.
        write(root, "svc/stale.c", "int stale(void) { return 0; }\n")
        stale = dict(entries[0])
        old_source = stale["file"]
        stale["file"] = str(root / "svc/stale.c")
        old_output = gate.output_of(gate.entry_argv(stale))
        stale["output"] = "CMakeFiles/services.dir/svc/stale.c.obj"
        stale["command"] = stale["command"].replace(old_source, stale["file"]).replace(old_output, stale["output"])
        mutations.append(("stale known-producer entry", entries + [stale], "coverage mismatch"))
        malformed = json.loads(base)
        malformed[0]["command"] = "'unterminated"
        mutations.append(("malformed argv", malformed, "malformed compile command"))
        for label, data, message in mutations:
            (build / "compile_commands.json").write_text(json.dumps(data))
            run(audit, False, message)
            print("ok", label)
        for text in ("not json", "{}"):
            (build / "compile_commands.json").write_text(text)
            run(audit, False)
        (build / "compile_commands.json").unlink()
        run(audit, False)
        (build / "compile_commands.json").write_text(base)
        print("ok malformed and missing DB")
        for tool in ("--objdump", "--nm", "--cc"):
            command = list(audit)
            command[command.index(tool) + 1] = "/no/such/tool"
            run(command, False)
        print("ok missing compiler/objdump/nm fail closed")
        # A path alias does not escape source selection. Conversely a lexical
        # service whose symlink points out of the shared tree is unsupported.
        write(root, "svc/real_alias.c", "int alias_fn(void) { return 0; }\n")
        (root / "svc/alias.c").symlink_to(root / "svc/real_alias.c")
        configure(root, build, args, "-DEXTRA_SOURCE=svc/alias.c")
        run(shell, contains="coverage verified")
        write(root, "outside.c", "int outside(void) { return 0; }\n")
        (root / "svc/escape.c").symlink_to(root / "outside.c")
        configure(root, build, args, "-DEXTRA_SOURCE=svc/escape.c")
        run(shell, False, "symlink escapes scope")
        configure(root, build, args, "-DEXTRA_SOURCE=")
        run(shell, contains="coverage verified")
        print("ok canonical alias and escaping symlink")
        # New TUs are discovered with no audit declaration; exact exception only.
        write(root, "svc/nested/ymodem_extra.c", bad)
        configure(root, build, args, "-DEXTRA_SOURCE=svc/nested/ymodem_extra.c")
        run(shell, False, "owns mutable storage")
        configure(root, build, args, "-DEXTRA_SOURCE=")
        run(shell, contains="coverage verified")
        print("ok added nested source audited, removed source not audited")
        write(root, "svc/pure.c", "#ifdef SECOND_CONTEXT\n" + bad + "\n#endif\nint pure(void) { return 1; }\n")
        configure(root, build, args, "-DSECOND_CONTEXT=ON")
        run(shell, False, "owns mutable storage")
        print("ok same source, second target context rejected")
        # Direct parser checks ensure unsupported syntax fails rather than being
        # approximated, and output/depfile rewrites never hit production paths.
        source = root / "svc/pure.c"
        base_argv = [args.cc, "-O3", "-std=c11", "-D", "X=1", "-I", str(root), "-MMD", "-MFkeep.d", "-MT", "old", "-c", str(source), "-okeep.o"]
        out = Path(tmp) / "private.o"
        translated = gate.replay_argv(base_argv, source, root, Path(args.cc), out)
        assert "-MFkeep.d" not in translated and "-okeep.o" not in translated
        assert translated[-3:] == ["-fno-lto", "-o", str(out)]
        for flag in ("@args.rsp", "-save-temps", "-Wp,-MMD,keep.d", "-fplugin=evil.so"):
            try:
                gate.replay_argv(base_argv + [flag], source, root, Path(args.cc), out)
                raise AssertionError("accepted unsupported flag " + flag)
            except gate.AuditError:
                pass
        for compiler in ("ccache", "/no/such/compiler"):
            try:
                gate.replay_argv([compiler] + base_argv[1:], source, root, Path(args.cc), out)
                raise AssertionError("accepted unsupported compiler")
            except gate.AuditError:
                pass
        print("ok quoted/paired/joined options and forbidden side effects/launchers")
    if args.board_build:
        replay_mutations(args)
    print("run_shared_storage_build_tests: all fixtures passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
