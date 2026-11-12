#!/usr/bin/env python3
"""Audit stateless services using their actual board compile commands (#117).

Coverage is derived twice: CMake's evaluated target sources, and its compile DB.
Their (producer, source) sets must agree before any TU is audited. No file is
exempt (#230 removed the last exception, svc/ymodem.c). We only
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

shell/cmds/ (#206): every source the build compiles under shell/cmds/ is audited,
as under svc/, with no list of files. Whether a build must ship one is said by
the file itself, in its first line: `/* shared-command: required */`, or
`/* shared-command: optional BOARD_HAS_<X> */` where BOARD_HAS_<X> is a property
every board.cmake sets to ON or OFF. A required file (or an optional one that is
ON) must be compiled as an object the `shell` link edge of build.ninja takes
directly: compiled only for a target that does not ship, or reaching the link
only through an archive, is refused. An optional file that is OFF must not be
compiled by any target at all.

Three more rules (#206) close the ways around that. They look at every TU the
shell link takes (directly or from an archive) whose source is this
repository's own: not under lib/, boards/<board>/lib/ or boards/<board>/sdk/,
and not generated in the build tree. Each is replayed with the same argv rules
as the storage audit (an argv that cannot be replayed fails).
  (a) its dependencies (-M) may not include a .c/.cc/.cpp/.cxx other than
      itself: a board TU that #includes a shared command would ship that
      command outside the audit.
  (b) only TUs under shell/cmds/ and boards/<board>/cmds/ may own a section
      whose name starts with .shell_root_cmds, and none may own the bare
      .shell_root_cmds (CLI_CMD_REGISTER always names the root command).
  (c) the root names collected from those sections are unique across the link.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_delivery_gate as ninja_graph  # noqa: E402  (one reading of build.ninja)

# The audited trees. A source is in scope when its real path is under one; a
# path spelled under one must also resolve under that same one (#206).
SCOPES = ("svc", "shell/cmds")
COMMANDS = "shell/cmds"
# The first line of every source under shell/cmds/ (#206). The token is looked
# for anywhere in the file, so a second mark is found wherever it is.
MARK_TOKEN = "shared-command:"
MARK_RE = re.compile(r"/\* shared-command: (required|optional (BOARD_HAS_[A-Z0-9_]+)) \*/")
CAPABILITY_RE = re.compile(r"(BOARD_HAS_[A-Z0-9_]+)=(.*)")
# Rules (a)(b)(c) (#206).
REGISTRATION = ".shell_root_cmds"
INCLUDED_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
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


def scope_of(path, root):
    """The audited tree a path is under (no resolution here), or None."""
    for scope in SCOPES:
        if Path(path).is_relative_to(root / scope):
            return scope
    return None


def policy_source(path, root):
    """Return whether the canonical source is in an audited scope."""
    return scope_of(Path(path).resolve(), root) is not None


def lexical_ok(lexical, physical, root):
    """A path spelled under an audited tree must resolve under that same tree."""
    scope = scope_of(lexical, root)
    return scope is None or scope_of(physical, root) == scope


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
            # symlink under svc/ or shell/cmds/ may not silently exempt a file
            # outside it (#206: nor move a command out of reach of its mark).
            lexical = Path(os.path.abspath(os.path.join(source_dir, spelling)))
            need(lexical_ok(lexical, src, root), f"service symlink escapes scope: {spelling}")
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
    return selected


def read_mark(path, root):
    """The mark of one shell/cmds/ source: ("required", None) or ("optional", var)."""
    name = path.relative_to(root).as_posix()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise AuditError(f"cannot read the mark of {name}: {exc}") from exc
    count = sum(MARK_TOKEN in line for line in lines)
    need(count, f"no mark in {name}: its first line must be "
         "/* shared-command: required */ or /* shared-command: optional BOARD_HAS_<X> */")
    need(count == 1, f"{count} marks in {name}: exactly one is allowed")
    match = MARK_RE.fullmatch(lines[0])
    need(match, f"unreadable mark in {name}: its first line must be exactly "
         "/* shared-command: required */ or /* shared-command: optional BOARD_HAS_<X> */")
    return ("required", None) if match.group(1) == "required" else ("optional", match.group(2))


def command_marks(root):
    """Every source under shell/cmds/ on disk, with its mark.

    The walk does not follow links; a link (to a file or a directory) that
    resolves outside shell/cmds/ is refused, the same rule the inventory uses.
    """
    base = root / COMMANDS
    need(base.is_dir(), f"missing {COMMANDS}/")
    marks = {}
    for directory, dirs, files in os.walk(base):
        for name in dirs + files:
            entry = Path(directory) / name
            if entry.is_symlink():
                need(scope_of(entry.resolve(), root) == COMMANDS,
                     f"command symlink escapes scope: {entry.relative_to(root).as_posix()}")
        for name in files:
            entry = Path(directory) / name
            if entry.suffix not in SOURCE_SUFFIXES:
                continue
            marks[entry.resolve()] = read_mark(entry, root)
    return marks


def capabilities(path):
    """BOARD_HAS_<X> -> True/False, as board.cmake set them (written at configure)."""
    values = {}
    for line in Path(path).read_text().splitlines():
        if not line:
            continue
        match = CAPABILITY_RE.fullmatch(line)
        need(match, f"malformed capability line: {line!r}")
        need(match.group(1) not in values, f"duplicate capability: {match.group(1)}")
        need(match.group(2) in ("ON", "OFF"),
             f"{match.group(1)} must be ON or OFF, not {match.group(2)!r}")
        values[match.group(1)] = match.group(2) == "ON"
    return values


def shell_link(ninja_path, link_target):
    """(objects the link takes directly, objects of the archives it takes,
    and for each of those objects the source its compile edge reads, or None).

    Read with check_delivery_gate.py's parser. Only the inputs before `|` are
    linked as written; an archive's members are listed separately so that a
    command reaching the link only through one is named as such.
    """
    ninja_path = Path(ninja_path)
    try:
        top, edges = ninja_graph.parse(ninja_path)
    except (OSError, ValueError) as exc:
        raise AuditError(f"cannot read {ninja_path}: {exc}") from exc
    incs = [i for i in top.get("__includes__", []) if not i.endswith("rules.ninja")]
    need(not incs, f"{ninja_path} includes {incs}; only a single-config build.ninja is read")
    bdir = ninja_path.parent.resolve()
    workdir = top.get("cmake_ninja_workdir", str(bdir) + "/")

    def norm(p):
        p = (p.replace("${cmake_ninja_workdir}", workdir).replace("$ ", " ")
             .replace("$:", ":").replace("$$", "$"))
        # CMake writes no other variable into a path; one left unexpanded would
        # be a path this reading does not know, so it is refused (#206 review).
        need("${" not in p, f"unexpanded variable in a build.ninja path: {p}")
        return (bdir / p).resolve()

    producer = {}
    for e in edges:
        for o in e.outs:
            producer[norm(o)] = e
    node = producer.get(norm(link_target))
    need(node is not None, f"no edge in {ninja_path} produces '{link_target}'")
    links = [node] if node.rule != "phony" else [
        producer[norm(i)] for i in node.ins
        if norm(i) in producer and producer[norm(i)].rule != "phony"]
    need(len(links) == 1,
         f"'{link_target}' resolves to {len(links)} link edge(s) in {ninja_path}, not one")
    link = links[0]
    direct = {norm(i) for i in link.explicit}
    archived = set()
    for i in link.data:
        e = producer.get(norm(i))
        if e is not None and "STATIC_LIBRARY" in e.rule:
            archived |= {norm(j) for j in e.explicit}
    # #206 review: which source each linked object is compiled from, read from
    # the graph itself (the first explicit input of a CMake compile edge), so
    # the compile DB can be held to it. An object this build does not make (a
    # prebuilt) has none. One it makes by any other rule (a custom command) is
    # refused: nothing could replay it, so rules (a)(b)(c) would not see it.
    origin = {}
    for obj in direct | archived:
        e = producer.get(obj)
        if e is None:
            origin[obj] = None
        elif "STATIC_LIBRARY" in e.rule:
            continue           # an archive taken directly: its members are above
        else:
            need("_COMPILER__" in e.rule and e.explicit,
                 f"the shell link takes {obj}, which build.ninja makes with rule "
                 f"'{e.rule}', not a CMake compile rule; such an object cannot be "
                 "replayed by this audit")
            origin[obj] = norm(e.explicit[0])
    return direct, archived, origin


def own_source(src, root, build):
    """A source of this repository that rules (a)(b)(c) look at."""
    if not src.is_relative_to(root) or src.is_relative_to(build):
        return False
    parts = src.relative_to(root).parts
    if parts[0] == "lib":
        return False
    return not (len(parts) > 2 and parts[0] == "boards" and parts[2] in ("lib", "sdk"))


def may_register(src, root):
    """Rule (b): the places a root command may be registered from."""
    parts = src.relative_to(root).parts
    return (parts[:2] == ("shell", "cmds") and len(parts) > 2) or (
        len(parts) > 3 and parts[0] == "boards" and parts[2] == "cmds")


def linked_units(database, root, build, origin):
    """Every compile of an own source whose object the shell link takes.

    Both ways (#206 review): a DB entry whose output the link takes is a unit,
    and every linked object build.ninja compiles from an own source must have
    exactly one DB entry with that output and that source -- a stale or cut
    compile DB would otherwise hide a TU from rules (a)(b)(c).
    """
    by_output = {}
    for entry in json.loads(Path(database).read_text()):
        cwd = Path(entry["directory"])
        output = canonical(output_of(entry_argv(entry)), cwd)
        by_output.setdefault(output, []).append(entry)
    units, checked = [], 0
    for obj, src in sorted(origin.items()):
        if src is None or not own_source(src, root, build):
            continue
        checked += 1
        entries = by_output.get(obj, [])
        need(len(entries) == 1, f"the shell link takes {obj}, compiled from "
             f"{src.relative_to(root).as_posix()}, which has {len(entries)} entries "
             "in compile_commands.json (exactly one is required)")
        entry = entries[0]
        cwd = Path(entry["directory"])
        need(canonical(entry["file"], cwd) == src,
             f"compile_commands.json compiles {obj} from {entry['file']}, build.ninja from {src}")
        units.append((src, entry, entry_argv(entry)))
    need(units, "no own source reaches the shell link")
    return units


def make_deps(text):
    """The prerequisites of a `gcc -M` rule, unescaped, without its target."""
    tokens = re.findall(r"(?:\\.|[^\s\\])+", text.replace("\\\n", " "))
    for i, token in enumerate(tokens):
        if token.endswith(":"):
            return [t.replace("\\ ", " ").replace("$$", "$") for t in tokens[i + 1:]]
    raise AuditError("malformed -M output: " + text[:200])


def section_names(objdump_text):
    return re.findall(r"^\s*\d+\s+(\S+)\s", objdump_text, re.M)


def check_units(units, root, cc, cxx, objdump, scratch):
    """Rules (a)(b)(c) over the linked own sources (#206)."""
    jobs = []
    for index, (src, entry, argv) in enumerate(units):
        name = src.relative_to(root).as_posix()
        obj = Path(scratch) / f"rule-{index}.o"
        try:
            command = replay_argv(argv, src, entry["directory"], cc, obj, cxx)
        except AuditError as exc:
            raise AuditError(f"cannot replay {name} for the include and registration "
                             f"rules: {exc}") from exc
        jobs.append((src, name, entry["directory"], command, obj))

    def one(job):
        src, name, cwd, command, obj = job
        # [!] -M, not -MM: a header (or .c) found through -isystem is listed too.
        deps = make_deps(run(command[:-2] + ["-M"], cwd))
        run(command, cwd)
        return deps, section_names(run([objdump, "-h", "-w", str(obj)], cwd))

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(one, jobs))
    roots, registering = {}, 0
    for (src, name, cwd, _, _), (deps, sections) in zip(jobs, results):
        included = sorted({canonical(d, cwd) for d in deps
                           if Path(d).suffix in INCLUDED_SOURCE_SUFFIXES} - {src})
        need(not included, f"rule (a): {name} includes a source file: "
             + ", ".join(str(p) for p in included))
        registrations = [n for n in sections if n.startswith(REGISTRATION)]
        need(REGISTRATION not in registrations,
             f"rule (b): {name} owns the bare {REGISTRATION} section; a root command is "
             "registered only through CLI_CMD_REGISTER, which names it")
        if not registrations:
            continue
        need(may_register(src, root),
             f"rule (b): {name} registers root commands ({', '.join(registrations)}) "
             "outside shell/cmds/ and boards/<board>/cmds/")
        registering += 1
        for section in registrations:
            match = re.fullmatch(re.escape(REGISTRATION) + r"\.(.+)", section)
            need(match, f"rule (b): {name} owns {section}, which names no root command")
            roots.setdefault(match.group(1), []).append(name)
    twice = {r: f for r, f in roots.items() if len(f) > 1}
    need(not twice, "rule (c): root command registered more than once: " + "; ".join(
        f"{r} in {' and '.join(sorted(f))}" for r, f in sorted(twice.items())))
    return {"units": len(units), "registering": registering, "roots": len(roots)}


def check_commands(selected, root, marks, caps, direct, archived):
    """Every compiled shell/cmds/ source is marked, and each mark holds (#206)."""
    compiled = {}
    for (target, src), entry, argv in selected:
        if scope_of(src, root) != COMMANDS:
            continue
        need(src in marks, f"compiled command has no mark on disk: {src}")
        output = canonical(output_of(argv), entry["directory"])
        compiled.setdefault(src, []).append((target, output))
    records = []
    for src, (kind, var) in sorted(marks.items()):
        name = src.relative_to(root).as_posix()
        compiles = compiled.get(src, [])
        record = {"source": name, "mark": kind if var is None else f"{kind} {var}"}
        records.append(record)
        if kind == "optional":
            need(var in caps, f"{name} is optional on {var}, which this board does not "
                 "set (board.cmake must set it to ON or OFF)")
            record["capability"] = "ON" if caps[var] else "OFF"
            if not caps[var]:
                need(not compiles, f"{name} is compiled although {var} is OFF: "
                     + ", ".join(sorted({t for t, _ in compiles})))
                record["shipped_by"] = []
                continue
        why = "required" if kind == "required" else f"optional and {var} is ON"
        record["shipped_by"] = sorted(t for t, output in compiles if output in direct)
        if record["shipped_by"]:
            continue
        need(not any(output in archived for _, output in compiles),
             f"{name} ({why}) reaches the shell link only through an archive; "
             "it must be an object the link takes directly")
        need(not compiles, f"{name} ({why}) is compiled only for targets the shell link "
             "does not take directly: " + ", ".join(sorted({t for t, _ in compiles})))
        raise AuditError(f"{name} ({why}) is not compiled by this build")
    return records


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
    marks = command_marks(root)
    direct, archived, origin = shell_link(args.ninja, args.link_target)
    report["commands"] = check_commands(selected, root, marks, capabilities(args.capabilities),
                                        direct, archived)
    shipped = sum(bool(r["shipped_by"]) for r in report["commands"])
    build = Path(args.ninja).parent.resolve()
    with tempfile.TemporaryDirectory(prefix="shared-storage-") as scratch:
        rules = check_units(linked_units(args.database, root, build, origin),
                            root, cc, cxx, args.objdump, scratch)
        report["rules"] = rules
        print(f"shared_storage: rules: {rules['units']} own TUs in the {args.link_target} link "
              f"checked for included sources, {rules['registering']} register "
              f"{rules['roots']} distinct root commands")
        for index, ((target, source), entry, argv) in enumerate(selected):
            relative = source.relative_to(root).as_posix()
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
    print(f"shared_storage: commands: {len(marks)} marked under {COMMANDS}/, "
          f"{shipped} shipped directly by the {args.link_target} link")
    print(f"shared_storage: OK ({len(selected)} compile contexts, coverage verified)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "database", "inventory", "cc", "objdump", "nm",
                 "ninja", "capabilities"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--link-target", default="shell")
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
