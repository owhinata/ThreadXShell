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
