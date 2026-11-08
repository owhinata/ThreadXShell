# Shared service storage audit

`shared_storage_check` runs on every shipping `shell` build (#117), including
when the ELF is already current. It covers every compiled translation unit in
the repository's `svc/` and `shell/cmds/` trees (#206; before that, a list of
four `shell/cmds/` files). There is no list of files: a source is in scope when
its real path is under one of the two trees, and a path written under one must
resolve under that same tree -- a symlink out of it is refused, both in the
build's inventory and in a walk of `shell/cmds/` on disk. The exact
`svc/ymodem.c` path is the sole existing exception: it owns non-reentrant send
and receive staging buffers and last-run diagnostics. This is not a stateless
rule for the entire shell; instances and job pools intentionally own state.

`shared_storage_gate.cmake` recursively discovers normal CMake targets and
emits their generator-evaluated sources. `check_shared_storage_build.py` requires
that inventory and `compile_commands.json` agree in both directions by producer
target and canonical source path. A new service therefore needs no second audit
declaration. Separate compilations of a source are separate audit contexts.

The supported build model is single-config Ninja with ordinary GCC compile
commands. Export disabled on a target, unity builds, unresolved source
expressions, unrecognized producer paths, compiler launchers, response files,
and unsupported compile options fail closed. Generated build-time headers must
have their producer dependencies attached before their sources use this gate;
current service inputs are source/configure-time files.

Each selected command compiles the original TU directly, in its original working
directory. Architecture, includes, definitions, language/optimization and source
options are preserved. Only output/depfile side effects are redirected or removed,
and a final `-fno-lto` makes native sections inspectable. Audits live in private
temporary directories and are never linked. `check_no_mutable_storage.py` rejects
allocated writable sections and COMMON symbols. Missing tools and failed commands
fail the build.

C++ services (#161) are replayed with the board's C++ driver, which the gate
passes as `--cxx` only when the build enabled C++. A compile command must name
exactly the configured C or C++ compiler, and the source suffix must match that
driver (`.c`/`.s`/`.S` for C, `.cc`/`.cpp`/`.cxx` for C++); an unknown driver or
a mismatch fails the audit. An explicit `-x` must name the same language (`c`,
or `assembler`/`assembler-with-cpp` for `.s`/`.S`, under the C driver; `c++`
under the C++ driver); any other value, and the joined `-x<lang>` form, is
refused. `-fno-use-cxa-atexit` is the one C++ flag added to
the accepted set. A global constructor emits `.init_array`, a writable section,
so the unchanged section rule refuses dynamic initialisation as well. This checks ordinary storage additions, not deliberately evasive
source code or memory safety.

`build/<board>/shared-storage/last-audit.json` is a diagnostic record of original
and replayed argv, not an acceptance stamp. The gate always repeats the work;
there is no cached success. Direct object/probe targets are outside the shipping
invocation guarantee. Plugin custom commands remain a separate artifact graph,
with actual-object storage checks and ownership validation in `add_plugin.cmake`.

## Which `shell/cmds/` files a build must ship: the file's mark

Every source under `shell/cmds/` says so in its first line, exactly one of:

```
/* shared-command: required */
/* shared-command: optional BOARD_HAS_<X> */
```

No mark, a second `shared-command:` anywhere in the file, a first line that is
not exactly one of these, or an unreadable file fails the audit.
`BOARD_HAS_<X>` names a property (what the board has, never which board it is);
today the only optional file is `fs_cmd_core.c`, on `BOARD_HAS_FILEX`. Every
board's `board.cmake` must set each such variable to `ON` or `OFF`
(f746g-disco ON, wio-lite-ai from `BSP_ENABLE_SD`, grove-vision-ai-v2 OFF);
`shared_storage_gate.cmake` writes every `BOARD_HAS_*` it sees to
`shared-storage/capabilities.txt`, and a variable a mark names that is unset or
not exactly `ON`/`OFF` fails.

- A `required` file, or an `optional` one whose property is `ON`, must be
  compiled as an object the `shell` link takes directly: one of the inputs
  before `|` in that link's `build.ninja` statement, read with
  `check_delivery_gate.py`'s parser (an `OBJECT` library's objects are such
  inputs). Compiled only for a target the link does not take, or reaching the
  link only through a static library, fails -- even though an archive member
  can end up linked.
- An `optional` file whose property is `OFF` must be compiled by no target at
  all.

A board that stops compiling a shared command is therefore refused, not audited
less, and the only way to leave one out is its property, checked both ways.

## Included sources, stray and duplicate registrations (#206)

Three more rules look at every translation unit the `shell` link takes (its
direct objects and the members of the static libraries it takes) whose source
is this repository's own: not under `lib/`, `boards/<board>/lib/` or
`boards/<board>/sdk/`, and not generated in the build tree. Each is replayed
with the same argv rules as the storage audit (in parallel); one that cannot be
replayed fails rather than being skipped. The set is taken from `build.ninja`,
not from the compile DB: every such object a compile edge produces must have
exactly one `compile_commands.json` entry with that output and that source, so
a stale or cut DB cannot hide a translation unit from these rules.

- (a) Its dependencies (`gcc -M`, which also lists files found through
  `-isystem`) may not include a `.c`, `.cc`, `.cpp` or `.cxx` other than
  itself. A board file that `#include`s a shared command would otherwise ship
  that command's code outside the audit.
- (b) Only translation units under `shell/cmds/` and `boards/<board>/cmds/` may
  own a section whose name starts with `.shell_root_cmds`, and there it must be
  `.shell_root_cmds.<root name>`, as `CLI_CMD_REGISTER` writes it. The bare
  `.shell_root_cmds` (which the f746 and Grove linker scripts also keep) is
  refused wherever it is.
- (c) The root names collected from those section names are unique across the
  link; a duplicate names both files. The shell would otherwise run whichever
  comes first in name order and the other would never be reached.

The audit prints how many translation units it checked, how many register and
how many distinct root names it found (on 2026-11-08: f746g-disco null backend
113 / 25 / 30, wio-lite-ai 140 / 23 / 28, grove-vision-ai-v2 140 / 20 / 25).

**Documented limits.** Like the rest of this audit it is a build-integrity check,
not a defence against a hostile change; these are left to diff review:

- a registration written by hand without `CLI_CMD_REGISTER`, whose section's
  root name differs from the record's `.name` (rule (c) reads section names);
- a change of a mark from `required` to `optional`;
- a copy of an optional shared command placed in `boards/<board>/cmds/` on a
  board where its property is `OFF`;
- an object produced by a custom command rather than a compile in
  `compile_commands.json` (nothing replays it);
- the `svc/ymodem.c` exception covers the whole translation unit, so a new
  static added to it passes as well (#210 removes the exception itself).

## Negative tests

`fixtures/run_shared_storage_build_tests.py --cc <arm-gcc> --cxx <arm-g++>
--objdump <arm-objdump> --nm <arm-nm>` builds a temporary real CMake project and
tests coverage omissions, target export/unity, source additions/removal,
differing target contexts, direct ARM-only/optimized storage, failure
persistence, C++ services (a clean `.cc` passes; a static object, a global
constructor, an unknown or unconfigured driver, a suffix/driver mismatch and a
contradicting `-x` fail), the `shell/cmds/` marks and link rules, and rules
(a) to (c), each refused on the first build of a clean tree. Optional `--board-build
<configured-consumer-build>` mutates temporary copies of the three shared
regression sources and of `shell/cmds/cmd_builtin.c` using the selected Grove or
Wio consumer's actual compile context. When that consumer compiles the shared TFLM backend
(`svc/nn/tflm/nn_tflm.cc`, Wio Lite AI or STM32F746G-DISCO with the `tflm`
backend), a copy of it with one injected `static` is replayed through the C++
driver in that real context and must be refused, and the unmodified file must
pass; `--require-tflm` turns "this build does not compile it" from a skip into
a failure (run by hand against a Wio or F746 build; Grove's automatic run skips
it). It never edits firmware sources or
writes firmware. The existing `run_storage_gate_tests.py` independently tests
section/COMMON detection, including TLS, anonymous assembly and broken tools.
Grove runs both fixture suites on every `shell` build, including the integration
suite's real consumer mutations; there the integration suite is called with
`--no-verdict`, so it prints no `negative-tests:` line. The run whose line counts
is Grove's `test/host_tests.sh`, when both its build tree and the pinned
toolchain are present; `shell/test/run_host_tests.sh` holds that line against
its negative-test table (row `tree:grove-vision-ai-v2+toolchain`). The generic
section-checker fixtures are not duplicated in Wio builds; Grove's
cross-compiler invocation and the host runner already exercise them, while Wio
still audits its own compiled services. By hand:

```
python3 cmake/fixtures/run_shared_storage_build_tests.py \
    --cc tools/<toolchain>/bin/arm-none-eabi-gcc --cxx .../arm-none-eabi-g++ \
    --objdump .../arm-none-eabi-objdump --nm .../arm-none-eabi-nm \
    --board-build build/grove-vision-ai-v2
```

It ends a passing run with `negative-tests: cmake/fixtures/run_shared_storage_build_tests.py
OK (N cases)`, N being the commands and argv checks it held to their expected
result; `--no-verdict` leaves that line out. A commit that adds or removes a
case changes the table's row with it.

# Plugin veneer cost gate

`veneer_cost_check` runs on every shipping `shell` build of a board that
registers it (#112: Grove Vision AI V2 and Wio Lite AI). `check_plugin_image.py`
cannot see across a plugin veneer, so a crossing into the base is charged a flat
allowance c, `VENEER_BASE_COST`. This is the other side of that charge:
`check_veneer_base_cost.py` derives the stack the FIRMWARE spends below each
veneer from the image that ships, and the build fails unless the declaration
covers it.

It CHECKS; it does not generate. Since plugin ABI 2 (#111) a container no
longer carries c: its manifest declares the plugin's own frames (A0, A1 at a
crossing, and the sink bound S), and the device's loader adds its own c at load
time as `max(A0, A1 + max(c, S))`. So changing the declaration re-packs nothing
-- a container whose crossing no longer fits under a raised c is refused on the
device -- but the c the loader adds must be the one checked here, which the
next two paragraphs make sure of.

The helper hands the declaration to the firmware itself, as the compile
definition `PLUGIN_VENEER_BASE_COST` on every compile of the image (the set
that gets `-fstack-usage`), and to the asset rules through
`veneer_cost_gate_declared()`, which the host container verifier's
`--veneer-cost` comes from. A board restates it nowhere.

A `-D` is not a guarantee: a later `-D` (CMAKE_C_FLAGS, add_compile_definitions)
or a `#define` overrides it with only a warning. So `check_policy_probe.py`
reads, from the shipped ELF, the `veneer_cost` and `stack_accounting` the
board's policy actually holds. Anything but DECLARED and the ABI's accounting
version refuses the stamp.

The policy it reads is the one the loader reads, by name (#165). Each board
defines exactly one `const struct plugin_policy plugin_board_policy`, and the
firmware's `plugin_parse()` (`svc/plugin/plugin_load_board.c`) takes no policy
argument: it reads that object. The check reads the same symbol, and requires
it to be one symbol, global (not weak, not local), a data object of exactly
`sizeof(struct plugin_policy)`, inside one loadable read-only section. The
offsets of its fields come from the constant record `plugin_policy_probe`,
defined once by `plugin_load_board.c`, so the check transcribes no layout.
Until #165 that record held a pointer "to the policy `plugin_parse()` gets",
and nothing tied the two: a board could point it at a correct copy and hand the
parse another. The helper links with `--require-defined` for the record and
for `plugin_board_policy`, which keeps both past `--gc-sections` and LTO and
fails the link of a board that defines neither.

The parse has one body, `svc/plugin/plugin_load_body.h` (every function in it
`static`), and two entries over it in two files with no switch between them:
the firmware's `plugin_parse()` above, and the host's
`plugin_parse_with_policy()` (`plugin_load_host.c`), which takes a policy for
the container verifier and the host tests. A firmware must not link the second,
and `check_policy_entry.py`, also before the stamp, reads every input the
link's map loaded -- the objects, and the archive members the map says it
included -- and refuses one that defines or references it. LTO objects are read
as IR through the toolchain's `gcc-nm` (beside the compiler's `gcc-ar`), since
an inlined entry leaves no symbol in the image; an input that cannot be read, a
member its archive does not hold, a member of an archive the map did not load,
and an `nm` without the LTO plugin are each a refusal. It also refuses the
pre-#165 names of the entries that took a base or a vtable's functions
(`plugin_exec_load`, `plugin_run_load`, `nn_active_report`,
`nn_active_core_report`, `plugin_paint_vtable`, and the draw's
`nn_active_draw`, `nn_active_core_draw`, `plugin_paint_bind`, which took a
painter the board had built and so could edit; see the next paragraph). And it
requires each entry that reads by name to name the object: the one input that
defines `plugin_parse` must reference `plugin_board_policy`, and the ones that
define `plugin_exec_start`, `plugin_painter_init` and `plugin_printer_init`
must reference `plugin_board_bindings` -- a `-D` on that one file would rename
what it reads while the read-back went on finding the real object (the review
of #165). The parse body carries one definition with external linkage
(`plugin_load_body_once`), so a second firmware file that includes it -- to
call the static body with a policy of its own -- fails the link.

The function behind each veneer is read out of the image too (#165), not
stated. Each board defines one `const struct plugin_bindings
plugin_board_bindings` (`svc/plugin/plugin_bindings.h`): its head is the base
the loader hands `entry()` (`plugin_exec_start()` / `plugin_run_start()` take
no base argument), and the painter and the printer are filled from its other
members by `plugin_painter_init()` / `plugin_printer_init()`, which take a
context from the board and no function -- and which only the shared code calls,
inside `nn_active_core_draw_on()` / `nn_active_core_report_to()`, so no vtable
the board holds reaches a plugin. So its six words in the shipped ELF are
the six functions a plugin can reach. Which word is which veneer is the
constant record `plugin_bind_probe` (`svc/plugin/plugin_bindings.c`): an offset
taken from the struct and a veneer name per member. The check refuses a record
that is missing, doubled, of another magic or size, with an unterminated,
unknown, repeated or missing veneer name, or with an offset that is not a word
inside the table or is shared; a table that is not exactly one global,
non-weak data object of the record's size in a loadable read-only section; and
a word that is NULL, has the Thumb bit clear, or is not a function's entry
(inside a body, or no body at all) -- by the same rule a linker veneer's literal
is read with (`thumb_stack_analysis.Image.thumb_entry()`). Aliases at one
address are one body; symbols at one address that disagree about its size are
refused when the body is analysed. `veneer_cost_gate()` refuses a `ROOTS`
argument: until #165 the board named the six by hand, and a table binding a deep
function passed while the list named a shallow one.

An offset taken from the struct is right whatever name is written beside it, so
the record's pairs are held against a second source:
`shell/test/test_plugin_bindings.c` calls the plugin's own veneers
(`asset/common/plugin_base.c`) through the vtables the firmware hands a plugin,
with a distinct marker in each member, and requires each veneer to reach
exactly one member, the one at the record's offset for its name. The fixtures
state the veneer of each member themselves rather than reading the record.

The derivation walks the linked ELF, not the sources: for each veneer it starts
at the firmware function the table binds behind it, decodes Thumb-2 instructions
to sum each body's stack decrements, follows direct calls and branches, and
takes frame + max(callee). Every step fails closed -- an encoding it does not
model, an SP change it cannot evaluate as a constant, an SP change on a cycle,
an indirect transfer, a return not proven to use the return address, a body
that leaves with sp somewhere other than where it found it (see **Returns and
the stack pointer** below) and recursion are each a refusal rather than a
guess. The set of veneers is imported from `check_plugin_image.py`, and the
record's names must be it exactly, so adding a veneer there fails every board
until the table binds a function to it.

The decoder, the analysis of one body, the walk and the lookup of each body's
`-fstack-usage` record live in `thumb_stack_analysis.py` (#124), and the plugin
image gate calls the same code (see **Plugin image gate: the stack bound**
below); reading the roots from the table, which bodies may have no record and
the budget stay in
`check_veneer_base_cost.py`. The module is an input of both gate commands --
this check and `add_plugin()`'s link + gate -- so editing it alone re-runs both.

The compiler's own `-fstack-usage` record is a WITNESS, not the answer: it
under-reports variadic frames, so a bound built from it would err unsafely.
Every body below a veneer is compared against the record of its own object file
or its own LTO partition, never a name looked up across the build, and a record
larger than the scan fails the build wherever it occurs. A body with no record
is refused unless the map places it in an archive under a declared prebuilt
root. `veneer_cost_gate.cmake` therefore adds `-fstack-usage` to every compile
that feeds the image -- discovered by walking the firmware target's link inputs,
including object libraries and interface libraries, never a board-maintained
list -- and deletes that target's stale LTO partition records before each link.

What the board states is only what the board knows: the declaration (the same
variable every `add_plugin()` is given), where its vendor's prebuilt archives
live, and what flashes it. The functions behind the veneers are its
`plugin_board_bindings`, read out of the image. `add_plugin()` refuses
to configure on a board that has not registered the gate, and the registration
is a target and a stamp rather than a flag; a plugin charged a cost that nothing
checks is the state this exists to end. At the end of configure the value being
checked is compared with the `VENEER_BASE_COST` every `add_plugin()` received,
and each plugin's printer limit (`SBUF_WRITE_MAX`, the bound on every plugin
function the base may be handed -- see **S** below) goes to the check as well,
because the printer veneer lands in the plugin too.

Success is a stamp, `build/<board>/veneer_cost/<firmware>.checked`. It is
deleted before the check runs and written only when the check passes, it is
built by `all`, and the board's delivery targets (`flash`, and `dfu-shell` on
wio) and every `asset-*` target depend on it. So a failed check leaves nothing
for a later build to trust, and stops both the flash and the container pack. The
check re-runs when the image, any of its scripts or the command line changes; the
witness records are not listed as dependencies because each is written by the
same compile or link that rewrites the image.

`fixtures/run_veneer_cost_tests.py --cc <arm-gcc>` tests the derivation over
small Cortex-M55 and Cortex-M7 images, asserting an exact value for an accept
and the stage, diagnostic and number of refusals for a refusal.
`fixtures/run_veneer_gate_build_tests.py --cc <arm-gcc>` tests the wiring by
building a real project whose fake flash and assets refuse unless a passed
check's stamp is newer than the image: the stamp's lifetime, the dependencies,
reruns on a script or image change, the witnesses of every compile, and an LTO
link that writes fewer partitions than the last. `shared_analysis` /
`shared_refusal` add a real `add_plugin()`: an edit of
`thumb_stack_analysis.py` alone re-runs both gates and nothing re-runs without
one, and a refusal added to that module alone fails both gates for that
reason, leaves no stamp and stops flash and every asset.
Every edit a case expects ninja to notice is followed by re-touching the file
with the current time until it is strictly newer than everything in the build
directory: ninja treats an equal mtime as up to date, file times advance once
per clock tick (1 ms here), and an edit made in the same tick as the last stamp
was once missed (`script_change`, #204). The time is never set ahead, so the
"nothing re-runs" cases still hold.
It also covers the c the firmware compiles in: `cost_wiring` changes DECLARED and
reads the value back from a policy TU in a `$<TARGET_OBJECTS:>` library and from
an asset's command line, and `cost_override` adds `-DPLUGIN_VENEER_BASE_COST=1u`
through CMAKE_C_FLAGS -- the image links, and flash and the asset stop in
`check_policy_probe.py`. Since #165: `bind_decoy` / `bind_deep_each` (a member
of the table bound to a function deeper than DECLARED is refused for that
veneer alone), `bind_shape` / `bind_none`, `policy_decoy` (an entry handed a
policy other than the one read back), `policy_shape` / `policy_none`,
`entry_clean` / `entry_map` / `entry_host_tu` / `entry_lto` / `entry_old` (the
inputs of the link, read, and the host entry or a pre-#165 entry in any of
them refused). `run_veneer_cost_tests.py` binds each case's functions into a
copy of its image's table and covers the word, record and table refusals.
`fixtures/run_add_plugin_arg_tests.py` covers the configure-time refusals and
that the declared value, the printer bounds and the stack table reach the
check's command line.

What this does NOT prove: trusted firmware code going around the shared code on
purpose -- writing the const table or policy through a cast (Grove's `.rodata`
is SRAM), calling a plugin slot with a vtable it built itself instead of
through `plugin_painter_init()` / `plugin_printer_init()`, or copying the parse
body into an entry of its own under another name (the checks watch the shared
entries' names); anything about exception entry, whose stacking is a separate
reserve; anything about the plugin side, which is `check_plugin_image.py`'s; and
anything about a container already on a device, which no build can reach.

# Delivery targets: declared, and checked against the build graph (P14)

`veneer_cost_gate()` makes the targets named in `DELIVERY` (and every
`asset-*`, found by name) wait for its stamp. A list is only as good as whoever
keeps it, and which targets use the firmware cannot be asked of CMake at
configure time: a custom target's `COMMAND` and `DEPENDS` are not properties.
So the declaration stays, and `check_delivery_gate.py` compares it, in both
directions, with what it derives from the `build.ninja` CMake wrote.

| board | firmware artifacts (outputs of the link edge) | edges that use them | owning targets = DELIVERY |
|---|---|---|---|
| Grove | `shell.elf`, `shell.map`, `shell.img`, and the image tool's byte-identical copies `image_gen/output_case1_sec_wlcsp/output.img` and `image_gen/input_case1_secboot/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf` (declared as BYPRODUCTS) | `flash` (layout check + `xmodem_send.py --file=.../shell.img`) | `flash` |
| wio | `shell.elf`, `shell.bin`, `shell.hex` | `dfu-shell` (`dfu-util -D shell.bin`) | `dfu-shell`, `flash` (depends on `dfu-shell`) |

An edge USES the firmware when, other than the link edge, the gate's check and
this check, an explicit or implicit input (resolved through phony nodes) is a
firmware artifact, or any of its variables but the description names one by file
name (`COMMAND`, and a link edge's `POST_BUILD` / `PRE_LINK`). The name is matched
as a run of name characters (`[A-Za-z0-9_.+~-]`) and no separator is listed on
either side (a list always misses one): a name character after it makes it
another name (`shell.elf.ltrans0.ltrans.su` is not the ELF), and the run of
name characters right before it must be empty or one short-option cluster, `-`
and letters/digits, because getopt takes a glued argument out of a cluster.
So `shell.bin&&true`, `{shell.elf}`, `shell.elf:0x08000000`,
`--file=shell.bin`, `/x/shell.elf`, `-Dshell.bin`, `-vDshell.bin`,
`x;-Dshell.bin` and `FW=-Dshell.bin` all count, while `myshell.bin`,
`x-Dshell.bin` and a work directory `.../tmpab12fw/` or `.../xshell.elf/` that
every command of the tree mentions do not (issue #149: before, nothing was
required in front of the name, and such a build was refused whole). The one
over-match accepted is a path component that starts with `-` and ends in the
name (`/-ab12fw/`; not `tmp-ab12fw`, where the cluster does not start the run),
which is refused -- the safe side; tempfile's directory names are `[a-z0-9_]`
only. Order-only inputs
do not count: CMake writes every target a rule's target transitively waits for
there. Then: every using edge must reach the gate's stamp; every target whose
phony closure owns a using edge must be in `DELIVERY` (or be an `asset-*`);
every `DELIVERY` target must own one; and every target -- the list the gate
writes at the end of configure plus every phony name in `build.ninja` -- must
wait for this check. That last rule is enforced by construction: at the end of
configure the gate gives every non-interface target of the tree this check as a
dependency, so `ninja <new-target>` runs it first. A call deferred past that
point would create a target the wiring never saw, so the gate refuses the
configure if any deferred call of the top-level directory is still pending when
it runs (a subdirectory's own deferred calls run when that directory ends,
before it; one queued on the top level from anywhere is seen). The graph scan of
rule 4 stays as a backstop and names any target that missed the wiring. The stamp depends on `build.ninja`, so the check
reruns after every regeneration and costs nothing otherwise. Only single-config
Ninja is supported, and any other generator is refused at configure.

What it CANNOT see: a command that reaches the image without naming it or
depending on it as a file -- a script that computes or hard-codes the path, a
glob or a name assembled by shell quoting (`shel''l.bin`, `$'\x73hell.bin'`), an
environment variable, a response file written at configure time, a copy of the
whole build directory, with at most a target-level dependency; another name for
the same file -- a symlink or hard link made at configure time, or a copy a
`POST_BUILD` writes WITHOUT declaring it as a BYPRODUCT (declared, it is an
artifact; undeclared, it is not, which is why Grove declares the image tool's
two copies); the image tool's derived intermediates on Grove
(`cm55m_s_application.img`, `inter_files/...`), which are not the same bytes and
are not declared; anything run outside the build (a shell alias, picocom by
hand); and a `POST_BUILD` / `PRE_LINK` step on the FIRMWARE target itself, which
is part of its link edge and runs before any gate can. A target named `asset-*`
is exempt from the naming rule (rule 2) -- it still waits for the gate. The
match is by file name, so it errs towards flagging. Other firmware (f746) is not this
gate's: only the registered firmware's artifacts are looked for.

`fixtures/run_veneer_gate_build_tests.py`'s `delivery_*` cases: the plain tree
passes; a new target that uses the image by file dependency
(`delivery_undeclared`), by name in its command alone (`delivery_by_command`),
from another executable's `POST_BUILD` (`delivery_post_build_other`), gated only
through `flash` but not declared (`delivery_gated_undeclared`), with a shell
separator or braces right after the name (`delivery_amp`, `delivery_brace`), or
through a `POST_BUILD` copy declared as a BYPRODUCT (`delivery_copy_alias`), or
glued to a short option or to the end of a cluster (`-Dshell.elf`,
`-vDshell.elf`: `delivery_short_opt`, `delivery_cluster`) is
stopped BEFORE it runs; `DELIVERY` naming a target that uses nothing is refused
(`delivery_declares_nothing`); a target deferred past the wiring refuses the
configure and `ninja <it>` does not run (`delivery_late`); on an edited copy of
the graph, an unwired target is named (`delivery_rule4_graph`) and an edge no
target owns fails the "waits for the gate" rule alone (`delivery_rule1_graph`);
and a script that computes the path itself, or a copy nobody declared, gets
through (`delivery_hidden_path`, `delivery_copy_undeclared`: the documented
limits, as passing cases). A tree built under a directory named `xshell.elf`
passes, and `delivery_by_command` is still refused there (`delivery_glued`). The
matcher itself (`firmware_name_re()`) is also tested over every printable ASCII
character and tab on both sides of the name, alone and before a cluster, plus
the #149 paths and the over-match (`delivery_name_re`). `run_add_plugin_arg_tests.py`'s `not_ninja` covers
the generator refusal.

# Asset models: the pin is checked again by its first consumer (#132)

`asset_model_source()` takes a pinned model (`URL`/`COMMIT`/`PATH_IN`/`SHA256`)
and `fetch_model.cmake` checks the hash when it publishes `fetched.tflite`. Once
the file exists, ninja reruns only its consumer if it changes, so the FIRST
CONSUMER checks the same `SHA256` again, inside its own command, on the bytes it
reads: `pack_container.py --model-sha256` when the model is packed as fetched
(wio), or `run_with_pinned_input.py` in front of the board's first ingest tool
(Grove's `tflite_strip_boundary`). A mismatch fails the step and writes
nothing. The value reaches the consumer only as `<OUT_VAR>_SHA256`, never as a
second literal; `add_asset()` requires `MODEL_SHA256` (possibly empty), refuses
`""` for the pinned file itself, and an empty hash given to either checker is a
refusal, not "no check". An override (`OVERRIDE_VAR`) is not hash-checked and
configure says so; `FILE` is a path, not a hash pin, and is guarded by whatever
pins the tree it lives in (Grove's cifar10 is inside the pinned Himax SDK).
`fixtures/run_pinned_input_tests.py` covers both checkers.

# Asset destination: a store slot, required (#159, #195)

Every asset goes to a store slot: `add_asset()` requires `SLOT` with
`SLOT_TABLE`, and `build_asset.py` requires `--slot` with `--slot-table` and
checks the packed container against that slot's `payload_max` before anything
is published, on every call (the check has no "if there is a destination"
branch). None, half, an empty value, a slot that is not a number, or a slot
table that is missing, not JSON, or not a non-empty list of rows whose `index`
and `payload_max` are whole numbers of at least 0 with no index twice (every
row is checked, not only the declared one: NaN, `true` or a second row of the
same index would otherwise slip past the comparison) is refused in a sentence
and publishes nothing. Together with the post-link `check_blob_staging.py` (every slot fits
the staging buffer, below), a published container also fits the buffer the
board loads it into. The table is each board's `plugin/slot_table.json`, written
by the one shared tool `asset/tools/slot_table.c` (#195): it is compiled with the
board's `src/blob_slots.c`, the shared `svc/blob/blob_map.c` lookups and the
interval and erase unit of the header the firmware's store reads them from
(forced in with `-include`, so no number is restated), refuses a table the
store would refuse, and writes no JSON then.

#159 had added a second destination, a file copied to f746's card and checked
against the staging capacity read out of the firmware ELF. f746 moved to a slot
in #194, and #195 removed that route from `add_asset.cmake`, `build_asset.py`
and `asset_receipt.py`. Its words are refused by name rather than forgotten:
`add_asset()` still parses `FILE_NAME` / `STAGE_SYMBOL` only to refuse them
(an unknown word after a multi-value keyword such as `VERIFY_ARGS` would be
taken as one of its values and configure would pass), and argparse refuses
`--stage-elf` / `--stage-symbol` / `--file`. (`run_with_pinned_input.py` was
never part of that route: it re-checks a pinned model's SHA256 in front of a
board's ingest, above.)

The receipt is one text for every board, in `asset_receipt.py` (#195): every
board runs the shared `blob` command since #194, so the commands it prints
are the same, and only the name, the slot (the one `add_asset()` gave the fit
check) and the CRC differ. It starts with `blob write <name> <slot>`, which
erases the slot and receives into it when the slot is empty or holds that
name, and says when the command refuses before any erase (another blob in
the slot, or the name already in another slot), then the picocom transfer,
the `blob list` CRC comparison and `nn model load <name>`. Until #195
each board passed its own lines (`RECEIPT_STEPS` / `_THEN` / `_TRANSFER` /
`_VERIFY`), the copies had drifted, and all three began with `blob erase
<slot>`, which removed the store's refusal every time; those four words are
now refused at configure like `FILE_NAME`. `fixtures/run_add_plugin_arg_tests.py` covers the configure
refusals; `fixtures/run_asset_dest_tests.py` covers `build_asset.py`'s (run to
the end with stand-in tools, so the fit check itself and "nothing published on
a refusal" are tested) and the receipt (its whole text byte for byte, for a
one-digit and a two-digit slot).

# Blob slots against the model staging buffer (`check_blob_staging.py`, #193)

A post-link step of every board whose tflm backend copies a blob whole into
its one staging buffer (wio-lite-ai since #193, f746g-disco since #194; it was
wio's `cmake/` until then): it reads the slot table (`blob_map_slots[]`,
`blob_map_slots_count`) and the capacity (`nn_tflm_stage_cap`, with
`nn_tflm_model_buf` at least that long) out of the linked image and refuses a
slot whose payload, the slot less its header unit, is larger than the buffer
-- a slot `blob write` would fill with a model this firmware cannot load.

The board passes only its own facts, in its `board.cmake`:

| Argument | Means | wio-lite-ai | f746g-disco |
|---|---|---|---|
| `--backend` | what this build compiled (`CONFIG_NN_BACKEND`) | | |
| `--staged-backend` | a backend that loads through the tflm buffer, and so must hold it | `tflm` | `tflm` |
| `--unstaged-backend` | a backend that does not, and so must not | `null` | `null`, `stedgeai`, `stedgeai_reloc` |
| `--header-bytes` | the header unit at the top of every slot (`src/blob.h` `BLOB_ERASE_UNIT`) | 4096 | 4096 |

wio-lite-ai runs it only with `BSP_ENABLE_KV` (no KV, no store).  A backend in
neither list stops the build; an unstaged image that holds the
staging symbols, or a staged one that does not, is refused too, and the table
is read either way (NOT APPLICABLE is said, never assumed).  Both boards keep
the table's two symbols with `-Wl,--require-defined`.  The tests are
`fixtures/run_blob_staging_tests.py`, run from each board's
`test/host_tests.sh` with `--board-dir`, so the header unit `board.cmake`
passes and the table entry layout the script restates are compared with that
board's `src/blob.h` and `svc/blob/blob_map.h`; the synthetic-image cases need
the cross toolchain and SKIP loudly without it.

# Plugin image gate: the stack bound

`check_plugin_image.py` runs in `add_plugin()`'s link command and refuses the
plugin before any container is packed. Since #124 its stack bound is not an
analysis of its own: it calls `thumb_stack_analysis.py`, the code that bounds
the firmware below each veneer, and the two gates differ only in where the walk
stops. The plugin walk stops at the register branch of a named veneer
(`VENEERS` in `check_plugin_image.py`, the set's only declaration) and returns
the two halves the manifest declares, A0 and A1; the firmware-side check walks
everything below its roots.

- Functions are ELF symbols identified by address. A name is used only to find
  an `--entry` (exactly one body) and a veneer (at most one body per name);
  two `static` functions of one name in two files are two bodies.
- A frame is the sum of the stack decrements in the body's instruction bytes.
  The compiler's record is a witness, found through the map: each body is
  compared with the `.su` beside the object the map says it came from
  (`--map`). A scan below the record, a body with no
  record, an ambiguous record and a dynamic one are refused. There is no
  prebuilt exemption on the plugin side: every body is compiled by
  `add_plugin()`.
- Refused anywhere in the image, not only below an entry: an indirect transfer
  outside a veneer (a register branch, a load or move into pc, `tbb`/`tbh`), a
  return or tail call that does not pass the checks of **Returns and the stack
  pointer** below (`mov lr, rX; bx lr`, `pop {pc}` with lr never saved or its
  word overwritten, sp not back at the entry), a branch into the middle of
  another function, a non-constant or looped SP change, a security state
  transition (`sg`, `bxns`, `blxns`), an encoding the decoder does not know; and
  below an entry, recursion by address. Inside a veneer exactly one more thing
  is allowed: `bx rN` / `blx rN` with N in r0-r12, the crossing into the base
  -- and a `bx rN` is a tail call, so it too must leave with sp at the entry
  and lr holding the return address. `sg` / `bxns` / `blxns` are refused
  inside a veneer as well. A conditional branch to another function's entry
  is a tail call and is walked.
- Switch jump tables are refused by the decoder unconditionally.
  `add_plugin()` compiles every plugin TU with `-fno-jump-tables`, after the
  board's `CFLAGS`, so that a dense `switch` compiles to a compare chain; the
  flag is what lets such a plugin pass, not a replacement for the refusal.
  Adding it changed no byte of the shipped plugins.
- Returns and sp are checked as on the firmware side (same code): see
  **Returns and the stack pointer** below.

On the four shipped plugin images (Grove and wio, blazeface and cifar10) the
shared analysis gives the same A0 / A1 / S as the analysis it replaced, to the
byte. The numbers did not move, but what they mean did, so the manifest's
stack-accounting version (`PLUGIN_STACK_ACCOUNTING` in
`svc/plugin/plugin_abi.h`, copied in `check_plugin_image.py`'s ABI table and
pinned to the header by the fixtures) went from 1 to 2 once all of #124 was in.
The device's loader compares it for equality, so a container packed before
#124 is refused on load as "stack declared under another analysis" and has to
be rebuilt and sent again (each board README says how); the new containers
differ from the old in the version field and in what covers it (digest,
CRC). Raise the version again whenever what the analysis charges, where it
stops, what it accepts as a return, or which functions S covers changes --
also when the change only makes the bound more precise, since an old
declaration would otherwise be read under the new rules.
`fixtures/run_plugin_gate_tests.py` keeps that older
analysis, transcribed, as an oracle: every slot must come out at or above it at
c = 1, 256 and 2^20, with one case (`same_name_shallow`) that must come out
below it and equal to it once its merged frames are separated. The negative
cases (`cond_tail`, `naked_frame`, `same_name_deep`, `mov_lr_bx_lr`, `ldr_pc`,
`pop_pc_unsaved`, `interior`, `jump_table` on both cores, `second_veneer`,
`su_file_missing`, `su_above_scan`, `no_entry`) are each refused for the
reason named in the case, and the ones about a bound also show that the older
analysis would have passed the same image. The linker-stub refusal (a stub that
is not a long-branch veneer) has no case: a plugin reservation is far smaller
than a branch's range.

# Returns and the stack pointer

Both stack gates (#124, in `thumb_stack_analysis.py`) follow, through every
body they analyse, where sp is relative to the body's entry, whether lr still
holds the return address, and which stack words hold a saved copy of it. A
bound that charges a body's own frame is only a bound if the body really
returns to its caller with sp where it found it; until #124 that was assumed
once lr had been saved anywhere (the four shapes `#112` pinned as a limit).

- Paths that meet with sp at two different places are refused.
- A saved copy is a word lr was stored to while it held the return address
  (a conditional store, which may not run, is not a save; nor is an exclusive
  store -- STREX, STLEX and their byte and halfword forms -- which may fail
  to write, though it still counts as a store over what it covers). It stops
  counting when a store at a known position overlaps it -- any width: byte,
  word, doubleword, store multiple, FP, MVE -- or when sp rises above it: a freed
  word may be written by an exception's stacking or by a callee, so it stays
  dead even if sp comes back down. A word above the call's sp lives across
  the call, and that is checked, not assumed: every body -- the callee too --
  is refused if it stores at a known position at or above the sp it was
  called with (any byte of the store), because that is its caller's frame.
  Measured before the check was added: no body below a veneer and none of
  the four shipped plugins does it; elsewhere in the firmware 9 bodies on
  Grove and 34 on wio do (stack-passed arguments written back, ThreadX
  service wrappers, CMSIS-NN kernels), and the firmware gate does not look
  at them. A plugin function that writes back a stack-passed argument is
  therefore refused today.
- `bx lr` needs lr to hold the return address and sp at the entry. A pop into
  pc must load a live saved copy and leave sp at the entry. lr reloaded from
  a live saved copy holds the return address again; from any other word it
  does not.
- A tail call (a direct branch to another function's entry) and a plugin
  veneer's `bx rN` need sp at the entry and lr holding the return address.
- A store's position is known when it is sp plus a constant: `[sp, #k]`, or a
  register that holds sp plus a constant -- followed through register moves
  and add/sub of an immediate inside the body, and through base writeback --
  plus a constant offset. Positions are 32-bit and signed, as the machine
  computes them: `add.w r3, sp, #0xffffffff` is sp - 1 (until #124 was
  finished it read as sp + 4294967295, so a store through it was compared
  with no saved copy).

[!] The one line the check does not cross: a store whose position is not sp
plus a known constant is not compared with the saved copies. That covers a
pointer that does not come from sp in this body (an argument, a pointer loaded
from memory, a callee's result, an sp value that went through memory, a callee
or FP/vector registers), and equally a pointer that does come from sp at a
position no constant gives: a local array written at a variable index, a
pointer stepped through a local buffer in a loop, an offset held in a
register. Such a store reaches a saved lr only by writing outside the object
it is meant for -- memory safety, which these gates do not prove (plugins are
trusted native code). Comparing those stores too was measured: it refused one
function of each of the four shipped plugins and `log_append` below
`pl_base_log` on both boards, all ordinary compiler output. Every other shape
that cannot be followed is refused, not passed.

`fixtures/run_veneer_cost_tests.py` fires each of these checks on both cores,
each refusal next to a passing case that differs only in what the check
looks at (`tail_sp`, `sp_unbalanced`, `sp_join`, `reg_kill`, `reg_copy_kill`
/ `reg_nokill`, `reg_unknown_position` -- the line above, which passes on
purpose -- `released_then_relowered` / `call_keeps_slot`,
`callee_writes_caller` / `callee_writes_own`, `store_straddles_entry`,
`position_wraps_kill` / `position_wraps_ok`,
`tail_after_lr_change` / `tail_after_reload`, `lr_reload_other` /
`lr_reload_slot`, the IT-block, width and indexing pairs, `sg`, `bxns`,
`blxns`), and `run_plugin_gate_tests.py` the plugin side (`lr_slot` and
`callee_writes_caller` / `callee_writes_own` on both cores, `veneer_sp`,
`veneer_sg`, `veneer_bxns`, `veneer_blxns`).

# S: every plugin function the base may be handed

The printer veneer (`pl_print_write`) calls whatever function the printer it
is handed carries, so the far side of a crossing is sometimes the plugin
itself: its own string sink `pl_sbuf_write`, put in a printer by
`asset/common/plugin_text.c`. The manifest carries that bound as S, and the
loader charges `max(c, S)` at a crossing. Until #124 S was the bound of that
one function alone. Any other plugin function whose
address the plugin takes could be put in a printer (or a painter) just the
same, and nothing measured it.

Since #124 S is derived. `add_plugin()` hands the gate the objects of the link
(`--objects`, the same list the link command takes, so a file added to a
plugin is read without anyone listing it) and the printer limit
(`--printer-limit`, the board's `SBUF_WRITE_MAX`). The gate reads every
relocation of every allocated input section the map places, and lists every
plugin function whose address is taken for a reason other than a branch:

- A data word or a literal-pool word (`R_ARM_ABS32`), either half of a
  MOVW/MOVT pair (`R_ARM_THM_MOVW_ABS_NC` / `R_ARM_THM_MOVT_ABS`, each naming
  its own symbol and addend), a section symbol plus an addend, a symbol of
  another object or of the link script, an alias -- each resolves to the
  linked address of what it names, with the Thumb bit dropped. A function's
  first byte there puts that function on the list; data is not a function.
- A global or weak symbol the object defines itself is resolved twice, at
  the object's own definition and at the image's global of that name, and
  the two must agree. They differ when the definition is weak and another
  object's strong one wins the link: the word then holds the winner, and the
  object's own position would list the loser.
- Not read: branch relocations (`R_ARM_THM_CALL`, `JUMP24`, `JUMP19`,
  `JUMP11`, `JUMP8` -- a call is an edge, walked as one), the relocations
  inside the slot table (the loader reaches a slot through the manifest, not
  through a pointer the base is handed), sections `--gc-sections` removed, and
  unallocated sections (debug information is not loaded).

Each listed function is bounded by the same walk as a slot. One that reaches
a veneer itself is refused (S would contain c), and each is held to the printer
limit. S is the deepest of them; an image that hands the base nothing declares
S = 0, which is a measurement. The listed functions are also emitted in
`plugin.stacks.json` beside the slots, by name (the packer reads only the
slots), and the gate prints the list: on the four shipped plugin images it is
`pl_sbuf_write` alone, so S is what it was (16 B on both boards).

Refused: a relocation type not listed above; a reference to code that is not
a function's first instruction; a function symbol's address, or any address
with the Thumb bit set, outside the image's executable sections (an odd data
address such as `&bytes[1]` is refused by the same test -- none of the
shipped plugins takes one); an executable output section other than `.text`,
and a function symbol outside `.text` -- the analysis reads instructions only
there, and the wio MPU runs the whole reservation, `.data` included, as code;
a defined global whose position in its object is not where the link bound
it; any reference to the slot table from outside
it, by name, by alias or by address (`plugin_slot_table`, `__plugin_slots_start`
and `__plugin_slots_end`, the last by name because it points one past the
table) -- code that reads a slot out of the table gets a function pointer no
relocation shows, so the table is not readable at all; an object that cannot
be read as an ARM relocatable; an object the link loaded that the gate was not
given, or one it was given that the link did not load; an input section the
map places from any other file (an archive member, for instance); an allocated
section the map neither places nor discards.

[!] The line the list does not cross: a function address no relocation shows.
Code that writes a function's address as an integer constant, or computes it
from one, carries no relocation, and nothing here can find it. The same holds
for a MOVW/MOVT pair whose halves name different symbols: each half is
resolved to the whole address of its own symbol (and each of those is listed
or refused as above), but the address the two instructions build together is
a third one that no relocation names; and for an address computed pc-relative
inside one section (`adr`, `add rX, pc`, a word holding `fn - .`
resolved by the assembler), which the assembler fixes without leaving a
relocation. Both are on the same side as memory
safety: a plugin is reviewed, trusted native code, and the
gate proves a stack bound for the code as the compiler and linker describe it,
not against a plugin that hides a pointer from them.

`fixtures/run_plugin_gate_tests.py` (`handed_*`) builds each form and each
refusal: the shipped plugin on both cores lists `pl_sbuf_write` only (and the
same with `-g`, and with an address taken only in code the link removed); the
plugin's own painter function, a slot function put in a printer, a section
symbol plus an addend, MOVW/MOVT on both cores, a literal pool and an address
taken in another object each raise S to that function, and so does a weak
definition nobody overrides; refused are a weak definition another object's
strong one wins over (on both cores), a C function in `.data` whose address
is taken (on both cores), a Thumb address into a data section with no
function symbol, an executable section other than `.text` and a function
symbol outside it (`handed_weak_*`, `handed_data_*`, `exec_data_section`,
`data_fn_symbol`), one over the printer limit, one that reaches a veneer, `R_ARM_REL32`, the slot table
read by name, by alias in its own object and in another, by its end symbol,
by a link-script symbol inside it and by a data symbol plus an addend, a code address inside a function and one no
function covers, an unreadable object, an ELF that is not an object, an object
not given, one not linked, an archive member, and a section the map does not
account for. The refusals on malformed relocation sections themselves (RELA,
a bad header, an index out of range) have no case.

# Plugin stack allowances: one table, read back from the image

What each plugin slot may ask of the stack it runs on is one fact with four
consumers: the firmware's policy (`stack_limit[]`, which the device's loader
enforces), the plugin image gate (`add_plugin()`'s `--entry`), the host
container verifier (`build_asset.py`'s `--policy-stack`), and the read-back
below. Until #126 a board wrote it out four times and nothing compared them.

A board now states it once, with `plugin_stack_table()`
(`plugin_stack_table.cmake`):

```cmake
plugin_stack_table(
    DEFINE_ON  shell_objs                       # the compile holding the policy
    ALLOWANCES GROVE_PLUGIN_STACK_SHELL=1024    # defined there as <MACRO>=<n>u
               GROVE_PLUGIN_STACK_PANEL=1024
    SLOTS      entry=GROVE_PLUGIN_STACK_SHELL ... draw=GROVE_PLUGIN_STACK_PANEL ...)
```

Every slot of the ABI must have exactly one row, every allowance must be
positive (the loader reads 0 as a refused slot) and used by some slot, and the
table is registered once. `add_plugin()` takes `SLOTS <slot ...>` (which slots
the plugin exports) and derives `--entry pl_<slot>=<limit>` from the table; it
refuses `ENTRIES` by name. `add_asset()` (`add_asset.cmake`, shared by both
boards' asset rules) derives `--policy-stack <slot>=<limit>`, which
`build_asset.py` maps to the validator's slot indices through the
`abi_layout.json` the host compiler prints from `svc/plugin/plugin_abi.h`. The slot
indices the read-back uses are `check_plugin_image.ABI`'s, pinned to the same
header by `run_plugin_gate_tests.py`.

The read-back is `check_policy_probe.py`, in the veneer-cost stamp's command:
it is handed the table as `--stack-limit <slot>=<bytes>` (every slot required)
and compares it with the seven numbers the shipped policy
(`plugin_board_policy`) holds, at the offset of `stack_limit` the
`plugin_policy_probe` record carries.

What it CATCHES: a later `-D` or `#define` of an allowance (CMAKE_C_FLAGS wins
over the definition the table makes, with only a warning), lowering or raising
it; and the firmware mapping a slot to a different allowance than the table --
both boards' firmware still states its own slot -> allowance mapping, next to
the asserts that hold each allowance under the threads its slot runs on
(Grove `port/nn/nn_plugin_stack.h`, wio `port/nn/nn_svc_wio.c`).

What it does NOT catch: a mapping disagreement between two allowances of EQUAL
value -- today every allowance on both boards is 1,024 B, so the device enforces
the table's numbers either way and the disagreement stays invisible (and
harmless) until the values differ; trusted firmware code going around the
shared code on purpose (a cast write to the const policy, a parse body copied
under another name -- see the veneer gate above); and whether the table itself
is right. Which policy is read is no longer a limit (#165): the loader takes
none and reads `plugin_board_policy`, the symbol read back, and
`check_policy_entry.py` refuses a firmware link holding the host's
policy-taking entry. The read-back also refuses a policy in a
WRITABLE section (`stack_writable_policy`), since only its initial value is in
the image -- by the section's flags, so it catches a policy declared writable,
not Grove's `.rodata`, which is SRAM and writable by the CPU. The
table moves every consumer together, so no comparison among them can say a
number fits its thread: that is the firmware's `_Static_assert` (strictly below
each stack) and the measured depths in each board README.

`fixtures/run_veneer_gate_build_tests.py` builds it: `stack_wiring` (the table
reaches the policy and is read back, following a change), `stack_override_down`
/ `stack_override_up` (a `-D` through CMAKE_C_FLAGS links, and flash and the
asset stop), `stack_every_slot` (each slot one byte off, alone, is named),
`stack_mismap`, `stack_mismap_equal` (the limit above, as a passing case) and
`stack_rows` (the probe's own refusals). `fixtures/run_add_plugin_arg_tests.py`
covers the table's, `add_plugin()`'s and `add_asset()`'s configure-time
refusals, that the table reaches all three command lines, and
`build_asset.py`'s name-to-index mapping against a layout numbered in reverse.

What the read-back does NOT read either: the policy's `link_addr` and
`capacity`.  The probe carries no offset for them, and adding one would change
every board's image.  f746g-disco holds the link address to the linked
`__plugin_start` in its layout gate (`check_f746_layout.py --policy-link-addr`,
issue #159) -- but that compares board.cmake's VARIABLE, the one compiled in as
`-DF746_PLUGIN_BASE`, not the policy object's bytes, so a `#define` or a later
`-D` in the source is outside it; wio-lite-ai has neither check.  A wrong value
fails closed (every container is refused as linked for another address), which
is why it stays a limit rather than a gate.

# Plugin lease entries: derived from the objects (`check_lease_entries.py`)

Every way into a plugin is either walked without the lease by a host test or
named in the script's `EXEMPT` table with the reason it needs no lease (issue
#130; moved here from wio-lite-ai's `test/` by issue #159, so f746g-disco runs
the same script).  The list of entries is every external text symbol of the
objects a board passes -- its `port/nn/nn_active.c` and `port/plugin/plugin_run.c`
and the shared `svc/nn/nn_active_lease.c` and `svc/plugin/plugin_run_core.c`,
each compiled on its own, never read off a linked test binary -- held in both
directions against the `LEASE ENTRIES` tables of the tests the board names
(`shell/test/test_nn_active_lease.c`, `shell/test/test_plugin_run_lease.c`, and
wio-lite-ai's `test/test_nn_active.c`).  A name defined but neither walked nor
exempted fails, and so does a name walked or exempted that no object defines.
Each board's `test/host_tests.sh` runs it; wio-lite-ai's also shows it refusing
two spellings a source-text match would miss (the cases and their verdicts are
in that board's `test/test_lease_entries.py` since #205).

# Plugin objects and the ABI they were compiled against

Two rules in `add_plugin.cmake` and `check_plugin_image.py` exist because of one
defect found on hardware in #111: the plugin compile depended on its `.c` alone,
so bumping `PLUGIN_ABI_VERSION` in `svc/plugin/plugin_abi.h` rebuilt only the TUs whose
source had also changed. The shipped images compared the base against ABI 1
while the packer, which reads the header, stamped ABI 2 -- the loader accepted
them and each plugin refused its own entry point. Every gate passed, because
every gate reads the linked image and it was self-consistent.

- Each plugin compile writes a depfile (`-MMD -MF <stem>.d -MT <stamp>`, CMake
  `DEPFILE`), so a header change recompiles -- and re-audits -- every TU that
  read it. `run_add_plugin_arg_tests.py`'s `header_rebuild` builds one plugin's
  objects on the host, touches `svc/plugin/plugin_abi.h`, and requires them rebuilt.
- `add_plugin()` compiles with `-DPLUGIN_IMAGE_BUILD`, under which every TU that
  includes `plugin_abi.h` leaves one word, `pl_abi_mark`, in `.plugin_abi_mark`
  (kept by `asset/common/plugin.ld`). The image gate refuses an image with no
  record, or with any record that is not the header's ABI (`stale_tu` and
  `no_marks` in `run_plugin_gate_tests.py`). This is the backstop, not the fix:
  a TU that does not include the header states no ABI.

# tflite-micro from its submodule (#134)

`tflm_submodule.cmake` is shared by the boards whose `tflm` backend generates a
tree with upstream's `create_tflm_tree.py` (f746g-disco, wio-lite-ai).
`tflm_submodule_pin()` initialises `lib/tflite-micro` if it is not yet (the
submodule is in no board's `submodules.cmake`, so only a tflm configure fetches
it and a `null` one stays offline -- the same fetch-on-demand as
`lib/mlperf-tiny`), returns the commit the superproject's gitlink records for it
(read from the index), and stops the configure if the submodule is checked out
anywhere else; it also makes the submodule's HEAD file a configure dependency, so
a checkout inside the submodule re-runs configure.
`tflm_submodule_build_check()` covers what that dependency cannot see, a
superproject checkout that moves only the gitlink: a target in ALL runs
`tflm_pin_check.cmake` on every build, compares the gitlink (`git ls-files -s`),
the submodule's `rev-parse HEAD` and the configured `TFLM_GIT_SHA`, and fails the
build with all three values unless they agree; the `tflm` library waits for it.
The superproject's `.git/index` is deliberately not a configure dependency
instead: `git status` rewrites it, so almost every git command would re-configure.
`tflm_submodule_export()` writes that commit with `git archive` into
`build/<board>/tflm-src`, where the board runs the generator. The generator is
never run inside `lib/`: it downloads ~300 MB of third-party code into the
tflite-micro tree it runs from, and a fresh export per attempt is what lets an
interrupted download heal on the next configure. Uncommitted edits in the
submodule are not exported.

# Upstream trees pinned per board (#136)

`fetch_upstream.cmake` gives a board an upstream source tree at an exact commit:
`fetch_upstream(<out_var> ROOT <dir> URL <url> COMMIT <40-hex sha> PROBE <file>...
[SPARSE <pattern>...])` sets `<out_var>` to `<dir>/<repo>-<commit>` (`<repo>` is
the URL's last component). `ROOT` is required and absolute; the boards pass
`boards/<board>/lib`, which each board's own `.gitignore` ignores (#177). The boards use it for the ST Cube set
(HAL, CMSIS device, CMSIS core), which ST pairs per Cube package; upstreams that
several boards share at one version stay `lib/` submodules. Rules:

- The commit is the pin and must be the full SHA (a tag is only a comment next to
  it). It is fetched directly, `--depth 1 --filter=blob:none`, with a non-cone
  sparse checkout when `SPARSE` is given, so only the needed blobs are downloaded.
- One directory per repository and commit under the board that pins it, shared
  by every build directory of the clone: a pin bump fetches a new directory beside
  the old one, and every configure after the first is offline. Two boards that pin
  the same commit each fetch their own copy (none do today).
- An existing directory is only checked, never repaired or re-fetched. It must be
  its own git checkout (an empty directory would otherwise let the superproject
  answer), its HEAD must be the pin and detached (not a branch), `git status
  --porcelain --untracked-files=all --ignored` must be empty, and every `PROBE`
  must exist. Because `git status` trusts the index marks, those are checked too:
  no entry may be assume-unchanged, `git sparse-checkout list` must be exactly
  `SPARSE` (and the tree must not be sparse when `SPARSE` is not given), and the
  skip-worktree marks must be exactly the paths those patterns exclude (asked of
  `git sparse-checkout check-rules`, so git 2.42 or later is needed). Anything
  else stops the configure with the reason. Deleting the directory is how to ask
  for a fresh copy.
- Every git call clears `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`,
  `GIT_OBJECT_DIRECTORY`, `GIT_ALTERNATE_OBJECT_DIRECTORIES` and `GIT_COMMON_DIR`
  and sets `GIT_NO_REPLACE_OBJECTS=1`, so a configure started from a git hook
  cannot check or fetch into the superproject, and `git replace` cannot stand in
  for the pin. It also clears `GIT_CONFIG_PARAMETERS` (what `git -c` hands to the
  commands it starts) and `GIT_CONFIG_COUNT` (without it the numbered
  `GIT_CONFIG_KEY_<n>` / `GIT_CONFIG_VALUE_<n>` are not read; see git-config(1),
  ENVIRONMENT), so an inherited `core.hooksPath` or `url.<x>.insteadOf` cannot run
  hooks in a fetched tree or send a fetch to another remote (#158). Configuration
  files are still read: git-lfs installs its filter in the system or global one.
- That prefix, `FETCH_UPSTREAM_GIT`, is how the whole build starts git: the
  pinned model fetch (`fetch_model.cmake`), the tflite-micro submodule
  (`tflm_submodule.cmake`, `tflm_pin_check.cmake`), the Grove SDK fetch
  (`boards/grove-vision-ai-v2/cmake/himax_sdk_fetch.cmake`), the submodule
  bootstrap and `git describe` in `CMakeLists.txt`, the boards' plugin build ids
  and the MLPerf Tiny submodule. A new git call uses it too, never a bare
  `${GIT_EXECUTABLE}` or `git`.
- The fetch runs in `<dest>.partial` and is renamed into place only after passing
  the same checks, so an interrupted fetch leaves no `<dest>` behind. A `<dest>.partial`
  that is a symlink is refused rather than removed.
- Concurrent configures serialise on `<dest>.lock` (`file(LOCK)`; a configure that
  has to wait says so).
- The checkout's HEAD file is a configure dependency. HEAD is detached, so every
  checkout or commit inside the tree rewrites that file and re-runs configure and
  its checks; a file edited without git is caught by the next configure, not by a
  build.

`fixtures/run_fetch_upstream_tests.py` (run by `shell/test/run_host_tests.sh`)
covers each refusal against a local `file://` upstream, paired with the same
configure passing, plus the offline second configure and the lock.
`fixtures/run_git_env_tests.py` (also run by `run_host_tests.sh`) runs the model
fetch and the Grove SDK fetch (fresh, and the check of an existing tree) with
each repository-redirecting variable pointed at a throwaway superproject, which
must come out byte for byte unchanged, and with a hook path and an `insteadOf`
injected through `GIT_CONFIG_PARAMETERS` and `GIT_CONFIG_COUNT`, which must
neither run the hook nor reach the other remote (`GIT_TRACE` shows which
upload-pack git started).

# Negative-test runners: held to a table of runs and counts (#205)

A negative-test runner whose call is deleted, turned into a no-op or skipped
when it should not have been prints nothing and fails nothing, so the host
tests would still end in "host tests passed".  To close that, every runner the
host tests call -- the `.py` and `.sh` files under `cmake/fixtures/`,
`boards/*/cmake/fixtures/` and `boards/*/test/` -- ends a passing run with
exactly one stdout line,
`negative-tests: <path from the repository root> OK (N cases)`, where N is the
number of verdicts it actually reached (counted where each is decided, never
the length of a case list; each entry of a table of expected values and each
separately reported comparison is its own case).  The line comes from the
runner itself, never from an `echo` in a board's script.

`shell/test/run_host_tests.sh` keeps the output of its own runners and of each
board's `test/host_tests.sh`, and holds every such line against one table in
that script: the runner, the log it must appear in (the shared runner's or a
board's), the condition under which it must have run, and N.  The condition is
decided by the shared runner from facts, never from a board's SKIPPED line,
using four predicates: always, the pinned toolchain under `tools/`, a board's
configured build tree, and a named ninja target in that tree.  The comparison
is both ways and N is an equality, not a floor: a row whose condition holds
needs its line exactly once with an equal N, a runner none of whose rows holds
must print nothing, and a line no row names fails.  The table is also held
against every `.py` and `.sh` file under those three directories, at any depth:
each needs a row or a line with its reason in the script's exclusion list, so a
runner nothing calls fails the suite before any negative-test runner is
started.  Before anything runs, each row is checked too: its log is `shared` or
a board with a `test/host_tests.sh`, so are the boards its condition names, a
named target is defined in that board's or the shared CMake sources, and no
predicate is `!always`; and the text `negative-tests:` may appear only in the
runners the table names (and the shared runner's own negative-test sections),
so a script cannot print the line for a runner it did not run.  A commit that
adds or removes a case therefore changes that runner's row in the same commit,
and a new runner needs a row.  The rows,
counts, predicates and exclusions themselves are only in the script; its
comment above the table is the reference.
