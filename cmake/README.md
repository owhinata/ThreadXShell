# Shared service storage audit

`shared_storage_check` runs on every shipping `shell` build (#117), including
when the ELF is already current. It covers every compiled translation unit in
the repository's `svc/` tree and `shell/cmds/{cmd_nn,nn_cmd_core}.c`. The exact
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
with `PLUGIN_POLICY_PROBE()` (`svc/plugin_load.h`) -- a constant record holding a
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
an indirect transfer, a return not proven to use the return address, recursion,
and a root that resolves to two bodies are each a refusal rather than a guess.
The set of veneers is imported from `check_plugin_image.py`, and the board's
roots must cover it exactly, so adding a veneer there fails every board that has
not bound a function to it.

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
and each plugin's own `pl_sbuf_write` bound goes to the check as well, because
the printer veneer lands in the plugin too.

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
link that writes fewer partitions than the last.
It also covers the c the firmware compiles in: `cost_wiring` changes DECLARED and
reads the value back from a policy TU in a `$<TARGET_OBJECTS:>` library and from
an asset's command line, and `cost_override` adds `-DPLUGIN_VENEER_BASE_COST=1u`
through CMAKE_C_FLAGS -- the image links, and flash and the asset stop in
`check_policy_probe.py`.
`fixtures/run_add_plugin_arg_tests.py` covers the configure-time refusals and
that the declared value and printer bounds reach the check's command line.

What this does NOT prove: that the declared bindings are the real ones (binding
a different callback without updating the declaration is invisible here);
anything about exception entry, whose stacking is a separate reserve; anything
about the plugin side, which is `check_plugin_image.py`'s; and anything about a
container already on a device, which no build can reach.

# Plugin objects and the ABI they were compiled against

Two rules in `add_plugin.cmake` and `check_plugin_image.py` exist because of one
defect found on hardware in #111: the plugin compile depended on its `.c` alone,
so bumping `PLUGIN_ABI_VERSION` in `svc/plugin_abi.h` rebuilt only the TUs whose
source had also changed. The shipped images compared the base against ABI 1
while the packer, which reads the header, stamped ABI 2 -- the loader accepted
them and each plugin refused its own entry point. Every gate passed, because
every gate reads the linked image and it was self-consistent.

- Each plugin compile writes a depfile (`-MMD -MF <stem>.d -MT <stamp>`, CMake
  `DEPFILE`), so a header change recompiles -- and re-audits -- every TU that
  read it. `run_add_plugin_arg_tests.py`'s `header_rebuild` builds one plugin's
  objects on the host, touches `svc/plugin_abi.h`, and requires them rebuilt.
- `add_plugin()` compiles with `-DPLUGIN_IMAGE_BUILD`, under which every TU that
  includes `plugin_abi.h` leaves one word, `pl_abi_mark`, in `.plugin_abi_mark`
  (kept by `asset/common/plugin.ld`). The image gate refuses an image with no
  record, or with any record that is not the header's ABI (`stale_tu` and
  `no_marks` in `run_plugin_gate_tests.py`). This is the backstop, not the fix:
  a TU that does not include the header states no ABI.
