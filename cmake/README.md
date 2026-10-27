# Shared service storage audit

`shared_storage_check` runs on every shipping `shell` build (#117), including
when the ELF is already current. It covers every compiled translation unit in
the repository's `svc/` tree and the shared commands outside it,
`SHARED_COMMANDS` in `check_shared_storage_build.py`:
`shell/cmds/{cmd_nn,nn_cmd_core,cmd_camera}.c` (`cmd_camera.c` since #143).
Each of those must also be compiled by the board -- a build that stops
compiling one is refused ("mandatory command missing"), not audited less. The exact
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
fail the build. This checks ordinary storage additions, not deliberately evasive
source code or memory safety.

`build/<board>/shared-storage/last-audit.json` is a diagnostic record of original
and replayed argv, not an acceptance stamp. The gate always repeats the work;
there is no cached success. Direct object/probe targets are outside the shipping
invocation guarantee. Plugin custom commands remain a separate artifact graph,
with actual-object storage checks and ownership validation in `add_plugin.cmake`.

`fixtures/run_shared_storage_build_tests.py --cc <arm-gcc> --objdump <arm-objdump>
--nm <arm-nm>` builds a temporary real CMake project and tests coverage omissions,
target export/unity, source additions/removal, differing target contexts, direct
ARM-only/optimized storage, and failure persistence. Optional `--board-build
<configured-consumer-build>` mutates temporary copies of the three shared
regression sources using the selected Grove or Wio consumer's actual compile
context. It never edits firmware sources or
writes firmware. The existing `run_storage_gate_tests.py` independently tests
section/COMMON detection, including TLS, anonymous assembly and broken tools.
Grove runs both fixture suites on every `shell` build, including the integration
suite's real consumer mutations. The generic section-checker fixtures are not
duplicated in Wio builds; Grove's cross-compiler invocation and the host runner
already exercise them, while Wio still audits its own compiled services.

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
or a `#define` overrides it with only a warning. So the board exports its policy
with `PLUGIN_POLICY_PROBE()` (`svc/plugin/plugin_load.h`) -- a constant record holding a
pointer to the policy `plugin_parse()` gets and the offsets of its fields -- and
`check_policy_probe.py` reads, from the shipped ELF, the `veneer_cost` and
`stack_accounting` that policy actually holds. Anything but DECLARED and the
ABI's accounting version refuses the stamp. The helper links with
`--require-defined=plugin_policy_probe`, which keeps the record past
`--gc-sections` (and LTO) and fails the link of a board that exports none.

The derivation walks the linked ELF, not the sources: for each veneer it starts
at the firmware function the board binds behind it, decodes Thumb-2 instructions
to sum each body's stack decrements, follows direct calls and branches, and
takes frame + max(callee). Every step fails closed -- an encoding it does not
model, an SP change it cannot evaluate as a constant, an SP change on a cycle,
an indirect transfer, a return not proven to use the return address, a body
that leaves with sp somewhere other than where it found it (see **Returns and
the stack pointer** below), recursion, and a root that resolves to two bodies
are each a refusal rather than a guess.
The set of veneers is imported from `check_plugin_image.py`, and the board's
roots must cover it exactly, so adding a veneer there fails every board that has
not bound a function to it.

The decoder, the analysis of one body, the walk and the lookup of each body's
`-fstack-usage` record live in `thumb_stack_analysis.py` (#124), and the plugin
image gate calls the same code (see **Plugin image gate: the stack bound**
below); the roots, which bodies may have no record and the budget stay in
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

What the board states is only what the board knows: the function behind each
veneer, the declaration (the same variable every `add_plugin()` is given), where
its vendor's prebuilt archives live, and what flashes it. `add_plugin()` refuses
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
It also covers the c the firmware compiles in: `cost_wiring` changes DECLARED and
reads the value back from a policy TU in a `$<TARGET_OBJECTS:>` library and from
an asset's command line, and `cost_override` adds `-DPLUGIN_VENEER_BASE_COST=1u`
through CMAKE_C_FLAGS -- the image links, and flash and the asset stop in
`check_policy_probe.py`.
`fixtures/run_add_plugin_arg_tests.py` covers the configure-time refusals and
that the declared value, the printer bounds and the stack table reach the
check's command line.

What this does NOT prove: that the declared bindings are the real ones (binding
a different callback without updating the declaration is invisible here);
anything about exception entry, whose stacking is a separate reserve; anything
about the plugin side, which is `check_plugin_image.py`'s; and anything about a
container already on a device, which no build can reach.

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
match is by file name, so it errs towards flagging. Other firmware (wio's `blink` and `dfu-blink`, f746) is not this
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
  (`--map`, which replaced `--su`). A scan below the record, a body with no
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
shared analysis gives the same A0 / A1 / S as the analysis it replaced, so the
change re-packed nothing: every `plugin.stacks.json` and container is byte for
byte what it was. `fixtures/run_plugin_gate_tests.py` keeps that older
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
  (a conditional store, which may not run, is not a save). It stops counting
  when a store at a known position overlaps it -- any width: byte, word,
  doubleword, store multiple, FP, MVE -- or when sp rises above it: a freed
  word may be written by an exception's stacking or by a callee, so it stays
  dead even if sp comes back down. Since a callee may write only below the sp
  it is called with, a word above the call's sp lives across the call.
- `bx lr` needs lr to hold the return address and sp at the entry. A pop into
  pc must load a live saved copy and leave sp at the entry. lr reloaded from
  a live saved copy holds the return address again; from any other word it
  does not.
- A tail call (a direct branch to another function's entry) and a plugin
  veneer's `bx rN` need sp at the entry and lr holding the return address.
- A store's position is known when it is sp plus a constant: `[sp, #k]`, or a
  register that holds sp plus a constant -- followed through register moves
  and add/sub of an immediate inside the body, and through base writeback --
  plus a constant offset.

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
`tail_after_lr_change` / `tail_after_reload`, `lr_reload_other` /
`lr_reload_slot`, the IT-block, width and indexing pairs, `sg`, `bxns`,
`blxns`), and `run_plugin_gate_tests.py` the plugin side (`lr_slot` on both
cores, `veneer_sp`, `veneer_sg`, `veneer_bxns`, `veneer_blxns`).

# S: every plugin function the base may be handed

The printer veneer (`pl_print_write`) calls whatever function the printer it
is handed carries, so the far side of a crossing is sometimes the plugin
itself: its own string sink `pl_sbuf_write`, put in a printer by
`asset/common/plugin_text.c`. The manifest carries that bound as S, and the
loader charges `max(c, S)` at a crossing. Until #124 S was the bound of that
one function, named on the gate's `--entry`. Any other plugin function whose
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
`pl_sbuf_write` alone, so S and every container are byte for byte what they
were.

Refused: a relocation type not listed above; a reference to code that is not
a function's first instruction; any reference to the slot table from outside
it, by name, by alias or by address (`plugin_slot_table`, `__plugin_slots_start`
and `__plugin_slots_end`, the last by name because it points one past the
table) -- code that reads a slot out of the table gets a function pointer no
relocation shows, so the table is not readable at all; an object that cannot
be read as an ARM relocatable; an object the link loaded that the gate was not
given, or one it was given that the link did not load; an input section the
map places from any other file (an archive member, for instance); an allocated
section the map neither places nor discards.

[!] The line the list does not cross: a function address made from a number.
Code that writes a function's address as an integer constant, or computes it
from one, carries no relocation, and nothing here can find it. That is on the
same side as memory safety: a plugin is reviewed, trusted native code, and the
gate proves a stack bound for the code as the compiler and linker describe it,
not against a plugin that hides a pointer from them.

`fixtures/run_plugin_gate_tests.py` (`handed_*`) builds each form and each
refusal: the shipped plugin on both cores lists `pl_sbuf_write` only (and the
same with `-g`, and with an address taken only in code the link removed); the
plugin's own painter function, a slot function put in a printer, a section
symbol plus an addend, MOVW/MOVT on both cores, a literal pool and an address
taken in another object each raise S to that function; refused are one over
the printer limit, one that reaches a veneer, `R_ARM_REL32`, the slot table
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
and compares it with the seven numbers the shipped policy holds, through the
same `PLUGIN_POLICY_PROBE` record. The probe already carried the offset of
`stack_limit`, so no firmware byte changed to add this.

What it CATCHES: a later `-D` or `#define` of an allowance (CMAKE_C_FLAGS wins
over the definition the table makes, with only a warning), lowering or raising
it; and the firmware mapping a slot to a different allowance than the table --
both boards' firmware still states its own slot -> allowance mapping, next to
the asserts that hold each allowance under the threads its slot runs on
(Grove `port/nn/nn_plugin_stack.h`, wio `port/nn/nn_svc_wio.c`).

What it does NOT catch: a mapping disagreement between two allowances of EQUAL
value -- today every allowance on both boards is 1,024 B, so the device enforces
the table's numbers either way and the disagreement stays invisible (and
harmless) until the values differ; that the object the probe points at is the
one the board passes to `plugin_parse()` -- a copy, or a second policy, passed
at run time is invisible (the same root as parity P13, unchanged since #111);
and whether the table itself is right. The read-back also refuses a policy in a
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
