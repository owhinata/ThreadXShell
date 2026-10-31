#!/usr/bin/env python3
"""Integration/negative tests of derived board-context storage auditing (#117).

Uses a small real CMake/Ninja project and the supplied ARM C and C++ compilers
(C++ services since #161). Optional
--board-build replays copies of the three shared regression TUs using the
selected consumer's actual compile context (Grove or Wio), without editing or
building any firmware tree.  When that build compiles the shared TFLM backend
(svc/nn/tflm/nn_tflm.cc: wio-lite-ai and f746g-disco with CONFIG_NN_BACKEND=tflm,
issue #161) a copy of it is replayed the same way through the C++ driver;
--require-tflm makes its absence a failure instead of a skip.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
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
    write(root, "shell/cmds/cmd_camera.c", "int cmd_camera(void) { return 0; }\n")
    write(root, "svc/pure.c", "int pure(void) { return 1; }\n")
    write(root, "svc/ymodem.c", "static volatile int state; int transfer(void) { return ++state; }\n")
    write(root, "svc/header_only.c", "#error not compiled\n")
    write(root, "include dir/context.h", "#define CONTEXT 1\n")
    write(root, "svc/conditional.c", "#if !defined(CONTEXT) || !defined(__OPTIMIZE__)\n#error lost context\n#endif\nint conditional(void) { return 0; }\n")
    write(root, "CMakeLists.txt", '''cmake_minimum_required(VERSION 3.20)
project(storage_fixture C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_library(services OBJECT svc/pure.c svc/ymodem.c svc/header_only.c
    "$<$<BOOL:1>:${CMAKE_CURRENT_SOURCE_DIR}/svc/conditional.c>")
set_source_files_properties(svc/header_only.c PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(svc/conditional.c PROPERTIES COMPILE_OPTIONS "-O3;-include;${CMAKE_CURRENT_SOURCE_DIR}/include dir/context.h")
if(OMIT_CAMERA)
    add_executable(shell shell/cmds/cmd_nn.c shell/cmds/nn_cmd_core.c $<TARGET_OBJECTS:services>)
else()
    add_executable(shell shell/cmds/cmd_nn.c shell/cmds/nn_cmd_core.c shell/cmds/cmd_camera.c $<TARGET_OBJECTS:services>)
endif()
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
# issue #161: compile a source with the other language's driver. CMake adds the
# matching -x, which the audit accepts; the suffix/driver pairing is refused.
if(C_AS_CXX)
    set_source_files_properties(svc/pure.c PROPERTIES LANGUAGE CXX)
endif()
if(CXX_AS_C)
    set_source_files_properties(svc/clean.cc PROPERTIES LANGUAGE C)
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
                "-DCMAKE_C_COMPILER=" + args.cc, "-DCMAKE_CXX_COMPILER=" + args.cxx,
                "-DCMAKE_OBJDUMP=" + args.objdump,
                "-DCMAKE_NM=" + args.nm, "-DCMAKE_C_FLAGS=-mcpu=cortex-m7 -mthumb -O2",
                "-DCMAKE_CXX_FLAGS=-mcpu=cortex-m7 -mthumb -O2 -std=c++17 -fno-exceptions"
                " -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit",
                *extra], expected, contains)


def replay_mutations(args):
    database = json.loads((Path(args.board_build) / "compile_commands.json").read_text())
    root = CMAKE.parent.resolve()
    names = ("rect_geom.c", "plugin/plugin_exec.c", "plugin/plugin_paint_budget.c")
    guards = ("defined(__arm__)", "defined(__OPTIMIZE__)", "__INCLUDE_LEVEL__ == 0")
    for name, guard in zip(names, guards):
        source = root / "svc" / name
        entries = [e for e in database if Path(e["file"]).resolve() == source]
        assert entries, "board does not compile " + name
        for entry in entries:
            with tempfile.TemporaryDirectory(prefix="storage-regression-") as tmp:
                tmp = Path(tmp)
                copy = tmp / Path(name).name
                copy.write_text(source.read_text() + f"\n#if {guard}\nstatic volatile int injected;\nint storage_regression_probe(void) {{ return ++injected; }}\n#endif\n")
                argv = gate.entry_argv(entry)
                # The copy remains a directly compiled TU. The extra quote path
                # reproduces original source-relative headers after copying.
                argv = [str(copy) if a == str(source) else a for a in argv]
                argv += ["-iquote", str(source.parent)]
                command = gate.replay_argv(argv, copy, entry["directory"], Path(args.cc).resolve(),
                                           tmp / "bad.o", Path(args.cxx).resolve())
                run(command, cwd=entry["directory"])
                run([sys.executable, str(CMAKE / "check_no_mutable_storage.py"),
                     "--objdump", args.objdump, "--nm", args.nm, str(tmp / "bad.o")], False,
                    "owns mutable storage")
            print("ok board source mutation:", name, guard)

    # [!] The C++ path of the audit in a REAL board context (#161): a copy of the
    # shared TFLM backend with one `static` injected, compiled with the tflm
    # target's own argv by its C++ driver, must be refused like the C TUs above.
    # This is what shows the backend is audited where it ships, not only that a
    # toy .cc is.
    source = root / "svc" / "nn" / "tflm" / "nn_tflm.cc"
    entries = [e for e in database if Path(e["file"]).resolve() == source]
    if not entries:
        assert not args.require_tflm, "board does not compile svc/nn/tflm/nn_tflm.cc"
        print("skip board source mutation: nn/tflm/nn_tflm.cc (not compiled by this board)")
        return
    for entry in entries:
        with tempfile.TemporaryDirectory(prefix="storage-regression-") as tmp:
            tmp = Path(tmp)
            copy = tmp / source.name
            copy.write_text(source.read_text() + "\n#if defined(__arm__)\nstatic volatile int injected;\n"
                            "extern \"C\" int storage_regression_probe(void) { return ++injected; }\n#endif\n")
            argv = gate.entry_argv(entry)
            assert gate.driver_language(argv[0], source, entry["directory"], Path(args.cc).resolve(),
                                        Path(args.cxx).resolve()) == "C++"
            argv = [str(copy) if a == str(source) else a for a in argv]
            argv += ["-iquote", str(source.parent)]
            command = gate.replay_argv(argv, copy, entry["directory"], Path(args.cc).resolve(),
                                       tmp / "bad.o", Path(args.cxx).resolve())
            run(command, cwd=entry["directory"])
            run([sys.executable, str(CMAKE / "check_no_mutable_storage.py"),
                 "--objdump", args.objdump, "--nm", args.nm, str(tmp / "bad.o")], False,
                "owns mutable storage")
            # The unmodified source in the same context passes: the refusal is the
            # injection, not something else about the C++ context.
            clean = gate.replay_argv(gate.entry_argv(entry), source, entry["directory"],
                                     Path(args.cc).resolve(), tmp / "good.o",
                                     Path(args.cxx).resolve())
            run(clean, cwd=entry["directory"])
            run([sys.executable, str(CMAKE / "check_no_mutable_storage.py"),
                 "--objdump", args.objdump, "--nm", args.nm, str(tmp / "good.o")], True,
                "owns no mutable storage")
        print("ok board source mutation: nn/tflm/nn_tflm.cc defined(__arm__) (C++ driver)")


CLEAN_CC = """\
// A C++ service with no mutable storage: constant-initialised objects only.
#ifdef __cplusplus
namespace {
struct Pair { int a, b; constexpr Pair(int x, int y) : a(x), b(y) {} };
constexpr Pair table[2] = {{1, 2}, {3, 4}};
}
extern "C" int clean_cc(int i) { return table[i & 1].a + table[i & 1].b; }
#else
int clean_cc(int i) { return i; }
#endif
"""
STATIC_CC = """
#ifdef __cplusplus
namespace { volatile int counter; }
extern "C" int bump_cc(void) { return ++counter; }
#endif
"""
# An empty class whose constructor has a side effect: the object needs no bytes,
# so the only writable section is the .init_array entry that runs it.
CTOR_CC = """
#ifdef __cplusplus
namespace { struct Registrar { Registrar() { __asm__ volatile("" ::: "memory"); } }; const Registrar registrar; }
#endif
"""


def failure_sections(text):
    """The writable sections a FAIL listed, as names."""
    return [line.split()[0] for line in text.splitlines()
            if re.match(r"^  \.\S+\s+\d+ B\s+\[", line)]


def cxx_services(root, build, args, shell):
    """issue #161: C++ services are audited with the C++ driver, never skipped."""
    write(root, "svc/clean.cc", CLEAN_CC)
    configure(root, build, args, "-DEXTRA_SOURCE=svc/clean.cc")
    run(shell, contains="coverage verified")
    report = json.loads((build / "shared-storage/last-audit.json").read_text())
    entries = [e for e in report["entries"] if e["source"].endswith("/svc/clean.cc")]
    assert len(entries) == 1, entries
    assert Path(entries[0]["audit"][0]).resolve() == Path(args.cxx)
    assert "-fno-use-cxa-atexit" in entries[0]["audit"]
    print("ok (a) C++ service without storage audited by the C++ driver and passed")
    write(root, "svc/clean.cc", CLEAN_CC + STATIC_CC)
    text = run(shell, False, "svc/clean.cc owns mutable storage")
    assert failure_sections(text) == [".bss"], failure_sections(text)
    print("ok (b) C++ service with a static object refused (.bss)")
    write(root, "svc/clean.cc", CLEAN_CC + CTOR_CC)
    text = run(shell, False, "svc/clean.cc owns mutable storage")
    assert failure_sections(text) == [".init_array"], failure_sections(text)
    assert "global constructor" in text
    print("ok (c) C++ service with a global constructor refused (.init_array only)")
    write(root, "svc/clean.cc", CLEAN_CC)
    run(shell, contains="coverage verified")
    base = (build / "compile_commands.json").read_text()
    audit = [sys.executable, str(CMAKE / "check_shared_storage_build.py"), "--root", str(root),
             "--database", str(build / "compile_commands.json"),
             "--inventory", str(build / "shared-storage/inventory"),
             "--cc", args.cc, "--objdump", args.objdump, "--nm", args.nm]
    # Without a configured C++ driver, a C++ compile command is an unknown driver.
    run(audit, False, "unsupported compiler/launcher (neither the configured C nor C++ compiler): " + args.cxx)
    entries = json.loads(base)
    for entry in entries:
        if entry["file"].endswith("/svc/clean.cc"):
            argv = gate.entry_argv(entry)
            entry["command"] = shlex.join(["/opt/other/bin/arm-none-eabi-g++"] + argv[1:])
    (build / "compile_commands.json").write_text(json.dumps(entries))
    run(audit + ["--cxx", args.cxx], False,
        "unsupported compiler/launcher (neither the configured C nor C++ compiler): /opt/other/bin/arm-none-eabi-g++")
    (build / "compile_commands.json").write_text(base)
    run(audit + ["--cxx", args.cc], False, "the C and C++ compilers must be distinct drivers")
    run(audit + ["--cxx", "/no/such/g++"], False, "missing C++ compiler")
    run(audit + ["--cxx", args.cxx], contains="coverage verified")
    print("ok (d) unknown, unconfigured, missing and aliased C++ drivers refused")
    configure(root, build, args, "-DC_AS_CXX=ON")
    run(shell, False, "source/driver mismatch: pure.c compiled by the C++ driver")
    configure(root, build, args, "-DC_AS_CXX=OFF", "-DCXX_AS_C=ON")
    run(shell, False, "source/driver mismatch: clean.cc compiled by the C driver")
    configure(root, build, args, "-DCXX_AS_C=OFF", "-DEXTRA_SOURCE=")
    run(shell, contains="coverage verified")
    print("ok (e) source suffix and driver language must agree")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cc", "cxx", "objdump", "nm"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--board-build")
    parser.add_argument("--require-tflm", action="store_true",
                        help="with --board-build: fail unless that build compiles "
                             "svc/nn/tflm/nn_tflm.cc (the C++ replay case)")
    args = parser.parse_args()
    args.cc, args.cxx, args.objdump, args.nm = map(lambda p: str(Path(p).resolve()),
                                                   (args.cc, args.cxx, args.objdump, args.nm))
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
        # issue #143: the shared `camera` command is audited like `nn`, and a
        # build that stops compiling it is refused rather than audited less.
        write(root, "shell/cmds/cmd_camera.c", "int cmd_camera(void) { return 0; }\n" + bad)
        run(shell, False, "owns mutable storage")
        write(root, "shell/cmds/cmd_camera.c", "int cmd_camera(void) { return 0; }\n")
        run(shell, contains="coverage verified")
        configure(root, build, args, "-DOMIT_CAMERA=ON")
        run(shell, False, "mandatory command missing: shell/cmds/cmd_camera.c")
        configure(root, build, args, "-DOMIT_CAMERA=OFF")
        run(shell, contains="coverage verified")
        print("ok shared camera command audited, and required")
        configure(root, Path(tmp) / "no-export", args, "-DDISABLE_EXPORT=ON", expected=False, contains="must export compile commands")
        configure(root, Path(tmp) / "unity", args, "-DENABLE_UNITY=ON", expected=False, contains="use unity builds")
        print("ok per-target export OFF and unity rejected")
        base = (build / "compile_commands.json").read_text()
        audit = [sys.executable, str(CMAKE / "check_shared_storage_build.py"), "--root", str(root),
                 "--database", str(build / "compile_commands.json"), "--inventory", str(build / "shared-storage/inventory"),
                 "--cc", args.cc, "--cxx", args.cxx, "--objdump", args.objdump, "--nm", args.nm]
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
        for tool in ("--objdump", "--nm", "--cc", "--cxx"):
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
        cxx_services(root, build, args, shell)
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
        for compiler in ("ccache", "/no/such/compiler", args.cxx):
            try:
                gate.replay_argv([compiler] + base_argv[1:], source, root, Path(args.cc), out)
                raise AssertionError("accepted unsupported compiler")
            except gate.AuditError:
                pass
        # issue #161: each driver only with its own suffixes, also at parse level.
        cc_source = root / "svc/clean.cc"
        for driver, src, word in ((args.cxx, source, "C++"), (args.cc, cc_source, "C")):
            argv = [driver] + [str(src) if a == str(source) else a for a in base_argv[1:]]
            try:
                gate.replay_argv(argv, src, root, Path(args.cc), out, Path(args.cxx))
                raise AssertionError("accepted source/driver mismatch " + src.name)
            except gate.AuditError as exc:
                assert f"compiled by the {word} driver" in str(exc), exc
        argv = [args.cxx] + [str(cc_source) if a == str(source) else a for a in base_argv[1:]]
        assert gate.replay_argv(argv, cc_source, root, Path(args.cc), out, Path(args.cxx))[0] == args.cxx
        # An explicit -x must name the driver's own language (#161 review): the
        # pairing above is not allowed to be overridden from inside the argv.
        def x_case(driver, src, extra):
            argv = [driver] + extra + [str(src) if a == str(source) else a for a in base_argv[1:]]
            return gate.replay_argv(argv, src, root, Path(args.cc), out, Path(args.cxx))
        for driver, src, extra, message in (
                (args.cc, source, ["-x", "c++"], "-x c++ does not match the C driver for pure.c"),
                (args.cxx, cc_source, ["-x", "c"], "-x c does not match the C++ driver for clean.cc"),
                (args.cc, source, ["-x", "c-header"], "-x c-header does not match the C driver"),
                (args.cc, source, ["-x", "assembler"], "-x assembler does not match the C driver"),
                (args.cc, source, ["-xc++"], "unsupported compile option: -xc++"),
                (args.cxx, cc_source, ["-xc"], "unsupported compile option: -xc")):
            try:
                x_case(driver, src, extra)
                raise AssertionError("accepted " + " ".join(extra) + " for " + src.name)
            except gate.AuditError as exc:
                assert message in str(exc), exc
        assert x_case(args.cc, source, ["-x", "c"])[1:3] == ["-x", "c"]
        assert x_case(args.cxx, cc_source, ["-x", "c++"])[1:3] == ["-x", "c++"]
        print("ok -x must name the driver's language; joined -x<lang> is unsupported")
        print("ok quoted/paired/joined options and forbidden side effects/launchers")
    if args.board_build:
        replay_mutations(args)
    print("run_shared_storage_build_tests: all fixtures passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
