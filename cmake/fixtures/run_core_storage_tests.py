#!/usr/bin/env python3
"""Tests of rule (d) of cmake/check_shared_storage_build.py on single objects (#246).

Rule (d) refuses a shell core object (a source under shell/, outside shell/cmds/
and shell/test/) that lets another file name anything but a function in
read-only code or a constant in .rodata(.<...>), and any COMMON symbol (since
the #246 review the passing side is closed: a `const` in a section the board
places in RAM is refused although it carries no write flag). A file `static`
(or a function-local one) is the core's own and passes. This compiles small
sources and holds the verdict of core_storage_offenders() on each object to what
it must be, both ways, and feeds it objects it cannot read. The layout cases
hold core_links() and core_build_dir() to theirs on a small directory tree.

Each compiler given is used in turn: the host one (`--cc`, with -fno-pie so a
constant pointer table lands in .rodata as it does on the boards, where nothing
is position-independent) and, when the pinned toolchain is there, the board
compiler (`--cross-cc`). The audit's own build-time run is
run_shared_storage_build_tests.py's; this is the one that shows each shape.

The last two "accept" cases are not wishes: they are ways around the rule
that it cannot see (a constant table that holds a static's address, and a
function that returns it), kept here so the README's list of what the rule
does not stop stays true of the code.
"""
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

CMAKE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("storage_build", CMAKE / "check_shared_storage_build.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

# issue #205 -- the last line every negative-test runner prints when it passes.
decided = 0


def negative_tests_ok():
    me = Path(__file__).resolve()
    print("negative-tests: %s OK (%d cases)" % (me.relative_to(me.parents[2]).as_posix(), decided))


USE = "int use_it(void);\n"
# (name, source, expected offenders as {(symbol, section)}; empty = accepted)
FIXTURES = [
    ("global_bss", "int core_state;\nint get(void) { return ++core_state; }\n",
     {("core_state", ".bss.core_state")}),
    ("global_data", "int core_state = 1;\nint get(void) { return ++core_state; }\n",
     {("core_state", ".data.core_state")}),
    ("weak_variable", "__attribute__((weak)) int core_state = 1;\n",
     {("core_state", ".data.core_state")}),
    ("explicit_common", "int core_state __attribute__((common));\n",
     {("core_state", "COMMON")}),
    ("thread_local", "__thread int core_state;\nint get(void) { return ++core_state; }\n",
     {("core_state", ".tbss.core_state")}),
    ("asm_globl", '__asm__(".pushsection .data.asm_state,\\"aw\\"\\n.globl asm_state\\n'
     'asm_state: .word 0\\n.popsection");\n',
     {("asm_state", ".data.asm_state")}),
    ("static_alias", "static int core_state;\n"
     "extern int core_alias __attribute__((alias(\"core_state\")));\n"
     "int get(void) { return ++core_state; }\n",
     {("core_alias", ".bss.core_state")}),
    ("own_section", '__attribute__((section(".core_state"))) int core_state;\n'
     "int get(void) { return ++core_state; }\n",
     {("core_state", ".core_state")}),
    ("file_static", "static int core_state;\nint get(void) { return ++core_state; }\n", set()),
    ("function_static", "int get(void) { static int n; return ++n; }\n", set()),
    ("const_table", "const int core_table[4] = { 1, 2, 3, 4 };\n"
     "int get(int i) { return core_table[i & 3]; }\n", set()),
    ("const_pointer_to_static", "static int core_state;\nint *const core_ref = &core_state;\n"
     "int get(void) { return ++core_state; }\n", set()),
    ("function_returns_static", "static int core_state;\n"
     "int *core_state_of(void) { return &core_state; }\n", set()),
    # issue #246 review: no write flag is not "not writable". The board's linker
    # script decides where a section lands, so a `const` in a section named for
    # a RAM area (f746 .sdram / .RamFunc / .log_noinit, wio .sram1_dma) is
    # refused, as is a data label in executable RAM (wio's ITCM). Only a
    # function in read-only code and anything else in .rodata(.<...>) passes.
    *[(f"const_in_{area.strip('.').lower()}",
       f'__attribute__((section("{area}"))) const int core_state = 1;\n',
       {("core_state", area)})
      for area in (".sdram", ".RamFunc", ".log_noinit", ".sram1_dma")],
    ("const_in_rodata_lookalike", '__attribute__((section(".rodataX"))) const int core_state = 1;\n',
     {("core_state", ".rodataX")}),
    ("asm_data_in_code", '__asm__(".pushsection .itcm_text.evil,\\"ax\\"\\n.globl evil\\n'
     'evil: .word 0\\n.popsection");\n', {("evil", ".itcm_text.evil")}),
    ("asm_function_in_rodata", '__asm__(".pushsection .rodata.fn,\\"a\\"\\n.globl fn\\n'
     '.type fn, %function\\nfn: .word 0\\n.popsection");\n', {("fn", ".rodata.fn")}),
    ("asm_function_in_writable_code", '__asm__(".pushsection .ramcode,\\"awx\\"\\n.globl fn\\n'
     '.type fn, %function\\nfn: .word 0\\n.popsection");\n', {("fn", ".ramcode")}),
    ("asm_label_not_allocated", '__asm__(".pushsection .note.core,\\"\\"\\n.globl note\\n'
     'note: .word 0\\n.popsection");\n', {("note", ".note.core")}),
    ("asm_label_in_rodata", '__asm__(".pushsection .rodata.lbl,\\"a\\"\\n.globl lbl\\n'
     'lbl: .word 0\\n.popsection");\n', set()),
]


def compile_one(cc, flags, tmp, name, source):
    src, obj = tmp / (name + ".c"), tmp / (name + ".o")
    src.write_text(source)
    subprocess.run([cc, "-std=gnu11", "-O2", "-fno-lto", "-fdata-sections", "-ffunction-sections",
                    *flags, "-c", str(src), "-o", str(obj)], check=True, capture_output=True, text=True)
    return obj


def expect_refused(path, message, label):
    """core_storage_offenders() must raise with this message: it could not read."""
    global decided
    try:
        found = gate.core_storage_offenders(path)
    except gate.AuditError as exc:
        assert message in str(exc), f"{label}: {exc}"
        decided += 1
        print(f"  ok   {label}: cannot check ({exc})")
        return
    raise AssertionError(f"{label}: read as {found!r}; it must not be checked")


def unreadable(obj, tmp):
    """Objects the rule cannot read, made from a good one."""
    raw = obj.read_bytes()
    wide = raw[4] == 2
    shoff = struct.unpack_from("<Q" if wide else "<I", raw, 0x28 if wide else 0x20)[0]
    shentsize, shnum, _ = struct.unpack_from("<HHH", raw, 0x3A if wide else 0x2E)
    heads = [shoff + i * shentsize for i in range(shnum)]
    symtab = [h for h in heads if struct.unpack_from("<I", raw, h + 4)[0] == gate.SHT_SYMTAB]
    assert len(symtab) == 1

    def variant(name, data):
        path = tmp / name
        path.write_bytes(bytes(data))
        return path

    yield variant("text.o", b"not an object\n"), "is not an ELF file", "not an ELF file"
    yield variant("cut.o", raw[:len(raw) // 2]), "rule (d): ", "a truncated object"
    yield tmp / "missing.o", "cannot read", "a missing object"
    data = bytearray(raw)
    struct.pack_into("<I", data, symtab[0] + 4, 1)          # SHT_SYMTAB -> SHT_PROGBITS
    yield variant("no-symtab.o", data), "0 symbol tables", "no symbol table"
    # A global symbol whose section index is not in the table.
    h = symtab[0]
    if wide:
        offset, size = struct.unpack_from("<QQ", raw, h + 24)
        entsize, = struct.unpack_from("<Q", raw, h + 56)
        info_at, shndx_at = 4, 6
    else:
        offset, size = struct.unpack_from("<II", raw, h + 16)
        entsize, = struct.unpack_from("<I", raw, h + 36)
        info_at, shndx_at = 12, 14
    for index, value in (("outside", shnum + 5), ("extended", 0xFFFF)):
        data = bytearray(raw)
        for n in range(1, size // entsize):
            at = offset + n * entsize
            if data[at + info_at] >> 4 != gate.STB_LOCAL:
                struct.pack_into("<H", data, at + shndx_at, value)
                break
        else:
            raise AssertionError("no global symbol to move")
        yield (variant(f"index-{index}.o", data), "not in its section table",
               f"a symbol's section index {value:#x} ({index})")


def run_compiler(label, cc, flags):
    global decided
    print(f"run_core_storage_tests: {label} ({Path(cc).name})")
    with tempfile.TemporaryDirectory(prefix="core-storage-") as tmp:
        tmp = Path(tmp)
        for name, source, want in FIXTURES:
            obj = compile_one(cc, flags, tmp, name, source)
            got = {(s, sec) for s, sec, _ in gate.core_storage_offenders(obj)}
            assert got == want, f"{label} {name}: offenders {sorted(got)}, expected {sorted(want)}"
            decided += 1
            print(f"  ok   {name}: " + ("refused " + ", ".join(f"{s} in {sec}" for s, sec in sorted(got))
                                       if got else "accepted"))
        good = compile_one(cc, flags, tmp, "probe", "int core_state;\n" + USE)
        for path, message, case in unreadable(good, tmp):
            expect_refused(path, message, case)
        # An object that is still LTO bytecode is refused for what it is.
        lto = compile_one(cc, flags + ["-flto"], tmp, "lto", "static int s;\nint get(void) { return ++s; }\n")
        expect_refused(lto, "holds LTO bytecode", "an LTO bytecode object")


def expect(label, call, message=None):
    """call() must pass (message None) or raise AuditError with message."""
    global decided
    try:
        call()
    except gate.AuditError as exc:
        assert message and message in str(exc), f"{label}: {exc}"
        decided += 1
        print(f"  ok   {label}: refused ({exc})")
        return
    assert message is None, f"{label}: passed; it must be refused with {message!r}"
    decided += 1
    print(f"  ok   {label}: passes")


def layout_cases():
    """issue #246 review: which files rule (d) sees cannot be moved out of it --
    by a link under shell/ to elsewhere, or by a build directory in shell/."""
    print("run_core_storage_tests: layout")
    with tempfile.TemporaryDirectory(prefix="core-layout-") as tmp:
        root = Path(tmp).resolve()
        for name in ("shell/core/a.c", "shell/test/t.c", "shell/cmds/c.c", "boards/x/state.c"):
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text("int x;\n")
        links = gate.core_links
        expect("no links under shell/", lambda: links(root))
        (root / "shell/core/inner.c").symlink_to(root / "shell/core/a.c")
        expect("a link inside the core", lambda: links(root))
        (root / "shell/cmds/out.c").symlink_to(root / "boards/x/state.c")
        expect("a link under shell/cmds/ (refused by the command walk, not here)", lambda: links(root))
        (root / "shell/cmds/out.c").unlink()
        for name, target in (("shell/core/state.c", "boards/x/state.c"),
                             ("shell/backend", "boards/x"),
                             ("shell/core/to_test.c", "shell/test/t.c"),
                             ("shell/core/to_cmds.c", "shell/cmds/c.c")):
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).symlink_to(root / target, target_is_directory=(root / target).is_dir())
            expect(f"link {name} -> {target}", lambda: links(root), "core symlink escapes scope: " + name)
            (root / name).unlink()
        shell = root / "shell"
        shell.rename(root / "real_shell")
        shell.symlink_to(root / "real_shell", target_is_directory=True)
        expect("shell/ itself a link", lambda: links(root), "must be a directory of this repository")
        builds = gate.core_build_dir
        for build, message in ((root / "build", None), (root / "shell/core", "inside shell/ or holds it"),
                               (root / "shell", "inside shell/ or holds it"),
                               (root, "inside shell/ or holds it")):
            expect(f"build directory {build.relative_to(root.parent)}",
                   lambda: builds(build, root), message)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", required=True, help="the host C compiler")
    ap.add_argument("--cross-cc", help="the pinned arm-none-eabi-gcc, when present")
    args = ap.parse_args()
    run_compiler("host", args.cc, ["-fno-pie", "-fno-pic"])
    if args.cross_cc:
        run_compiler("cross", args.cross_cc, ["-mcpu=cortex-m7", "-mthumb"])
    layout_cases()
    negative_tests_ok()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, subprocess.CalledProcessError) as exc:
        print(f"run_core_storage_tests: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
