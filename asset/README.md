# asset/ -- models, plugin containers and the blob store

An **asset** is a model together with the code that reads its output, built on
the PC and stored on the board without reflashing the firmware.  This document
follows one asset from start to finish: the file format, how the build makes
and checks it, where it is stored, how you confirm that what the board stored
is what was built, and what the firmware checks before it runs any of it.  It
covers what all three boards share.

Where the rest of the facts live:

| What | Where |
|---|---|
| The rules that must not be broken | `CLAUDE.md` (its plugin container and asset section) and `AGENTS.md` |
| One line per file under `svc/blob/` and `svc/plugin/`, and the `blob` command's behaviour | [`svc/README.md`](../svc/README.md#the-asset-stores-shared-parts-issue-192) |
| How each build-time check works, and its negative tests | [`cmake/README.md`](../cmake/README.md) |
| Each board's slot table, reservation address, stack numbers, measurements and commands | that board's README (links [at the end](#where-each-boards-details-are)) |

## The pieces

```
  on the PC (cmake --build ... --target asset-<name>)
  ---------------------------------------------------
  asset/plugins/<name>/ --- add_plugin(), image gate ---> plugin.elf --+
  pinned model --- the board's own preparation ---------> model ------+
                                                                       |
                  add_asset(): pack ONCE  <----------------------------+
                  check the packed file
                  check it fits the slot
                  publish  <name>.nnc + receipt (CRC-32)
                         |
                         |                     on the board
                         |                     ------------
             picocom: sb -k <path>  -------->  blob write <name> <slot>
                                               (YMODEM into one slot)
                                                    |
             compare with the receipt  <-------  blob list   (crc32 column)
                                                    |
                                               nn model load <name>
                                               validate, copy the plugin into
                                               its reservation, check the MPU,
                                               call its entry point
                                                    |
                                               nn run / nn stream:
                                               decode, draw, report,
                                               each call under the plugin lease
```

The parts in this tree:

| Path | What it is |
|---|---|
| `asset/plugins/blazeface/`, `asset/plugins/cifar10/` | The two plugins.  `blazeface` wraps the shared face decoder `svc/nn/blazeface.c`; `cifar10` reads a ten-class output and prints the class names, which only the container can carry. |
| `asset/common/` | What every plugin links: the calls into the firmware (`plugin_base.c`, each checking the firmware's table version and size first), its own text formatting and font (`plugin_fmt.c`, `plugin_text.c`), a few C library functions (`plugin_libc.c`), and the section layout (`plugin.ld`, joined with the board's address by `plugin_link.ld.in`). |
| `asset/tools/pack_container.py` | The packer.  It reads the field offsets from `abi_layout.json`, which `asset/tools/abi_layout.c` prints after being compiled against `svc/plugin/plugin_abi.h`, so the layout is never written out a second time. |
| `asset/tools/verify_container.c` | Runs the firmware's own container parse on the PC (see [Building an asset](#building-an-asset)). |
| `asset/tools/slot_table.c` | Writes a board's slot table as `plugin/slot_table.json` for the size check (see [The blob store](#the-blob-store-and-its-slot-tables)). |

## The container format

`svc/plugin/plugin_abi.h` is the only declaration of the format; the packer
reads its offsets from the compiled header, and the `_Static_assert`s at the
end of the file pin every size and offset.  Every multi-byte field is an
unsigned little-endian integer, and every reserved field and padding byte must
be zero -- a non-zero one is refused, not ignored.

```
  offset 0    container header, 80 B
                "NNC1" "PLGC"  hdr_size  abi_version  total_size
                section_count  plugin_digest  reserved
                3 x section entry { type, offset, length, reserved }
              PLUGIN section
                manifest ("PMAN"): target word, link address, capabilities,
                  image and segment offsets, 7 entry-point offsets,
                  stack declaration, name, build id
                load image: [code + read-only data][data initialiser]
                  (bss and scratch exist only in memory)
              zero padding up to a multiple of 16
              MODEL section: the model file, offset a multiple of 16
              [DATA section]
```

- **Sections.**  Exactly one MODEL, at most one PLUGIN, at most one DATA; none
  may be empty, overlap another or reach back into the header.  A container
  with no PLUGIN is valid (the model still loads).  The packer can add a DATA
  section, but no board's asset rule passes one today.  The order above is
  the packer's; the parser checks offsets, not order.
- **The model starts on a multiple of 16 bytes** (`PLUGIN_MODEL_ALIGN`).  The
  flatbuffer itself only needs 4; 16 is what the Ethos-U driver demands of the
  command stream it reads in place.  Checking the offset is enough because a
  slot's payload starts a whole erase unit after the slot's base.  Grove's
  README tells how 4 was mistaken for the answer:
  [The model section is 16-byte aligned](../boards/grove-vision-ai-v2/README.md#-the-model-section-is-16-byte-aligned-and-4-is-not-enough).
- **A container cannot be mistaken for a bare model.**  A `.tflite` carries
  `TFL3` at bytes 4..7, where a container carries `PLGC`; `plugin_probe()`
  tests for `TFL3` first and reads nothing else.  A bare model still loads
  as it did before containers existed.
- **The plugin image is prelinked** at the board's reservation address, and
  its start address and size are multiples of 32 (`PLUGIN_IMAGE_ALIGN`), so
  cache maintenance, which rounds outward to whole cache lines, never reaches
  past the reservation.  The loader applies no relocations.
- **Entry points are offsets** with the Thumb bit set.  ENTRY, SHAPES_OK and
  DECODE are required; DRAW, REPORT and the parameter pair are optional, and
  each is present exactly when its capability bit is set.
- **The target word** packs CPU, FPU, float ABI, byte order and (on Grove) the
  Secure-state bit into one 32-bit value compared for equality, so a
  container built for another board is refused with its own reason.  It is
  checked at both ends: the firmware asserts its own word at compile time
  (`svc/plugin/plugin_target.h`), and the image gate reads what the plugin
  was compiled for from its ELF.
- **Versions are compared for equality, never as a range**: `abi_version`
  (`PLUGIN_ABI_VERSION`) in the header and the manifest, and
  `stack_accounting` (`PLUGIN_STACK_ACCOUNTING`, see [Stack](#stack-what-a-plugin-declares-and-what-the-firmware-adds)).
- **`plugin_digest` is a CRC-32 of the PLUGIN section, and it is not a
  signature.**  It shows that the plugin bytes that arrived are the bytes
  that were packed.  It says nothing about where they came from: anyone can
  compute it for a hand-made container.  What makes a container trustworthy
  is that the official build ran the checks and assembled it in one step.
  The MODEL section has no digest of its own; the store's CRC over the whole
  payload covers it in transit.

## Building an asset

```bash
cmake --build build/<board> --target asset-<name>
```

`asset-<name>` is not part of the default build: a tree with no network access
to the model's host still builds the firmware.  Each board declares its assets
in its `board.cmake` with the shared `add_asset()` (`cmake/add_asset.cmake`),
which runs `cmake/build_asset.py`.  The plugin it packs comes from
`add_plugin()` (`cmake/add_plugin.cmake`), which links the plugin and runs the
image gate, `cmake/check_plugin_image.py`, as part of that link.

The order inside `build_asset.py` is the point of it:

1. **Pack once** (`asset/tools/pack_container.py`), at a private path inside
   the output directory.  The model it packs is the pinned file, checked
   against its SHA-256 a second time by its first reader; between the fetch
   and the pack each board does its own preparation (Grove: the boundary
   strip and Vela, where the model needs them; wio-lite-ai and f746g-disco:
   none).
2. **Read the model back out of the packed file** and run the board's model
   check on those bytes (`verify_vela_model` on Grove, `verify_tflite` on the
   other two).  With no host C++ compiler there is no model check, and the
   asset is refused rather than built unchecked.
3. **Run the firmware's container parse over the whole packed file**
   (`verify_container`), with the board's target word, reservation, stack
   limits, the firmware's veneer cost and the accounting version as
   arguments.
4. **Check the size against the declared slot** in the board's slot table.
   `blob write` erases the whole slot before the transfer tells it the file's
   size, so a container that does not fit would cost the slot's old contents
   to find out.
5. **Publish** only when all of that passed: the `.nnc` is renamed into
   `build/<board>/asset/` (read-only), then its receipt.

Checking the parts and then assembling them is not the same thing: the
assembler could read its inputs again and nothing after it would notice.  So
every check after the pack reads only the packed file, and the file that is
published is the one that was checked.

**The PC and the board run the same parse.**  The container and manifest
checks are one body, `svc/plugin/plugin_load_body.h`, with two entries in two
files: the firmware's `plugin_parse()` (`svc/plugin/plugin_load_board.c`),
which reads the board's policy object `plugin_board_policy` by name, and the
PC's `plugin_parse_with_policy()` (`svc/plugin/plugin_load_host.c`), which takes
the policy as an argument; `verify_container` links the second.
`svc/plugin/plugin_load.c` holds what both share besides the parse
(`plugin_probe()` and the refusal names).  `cmake/check_policy_entry.py`
refuses a firmware link that contains the PC's entry.  So a container cannot
pass on the PC and be refused by the board's parse for a structural reason --
issue #93 is what that cost before.

Before anything is packed, the firmware itself must have passed its own build
checks: every `asset-*` target, like every target that sends the firmware to a
board (`DELIVERY`), waits for the veneer-cost gate's stamp.  Which targets
use the firmware is not trusted to the list: `cmake/check_delivery_gate.py`
derives the users from `build.ninja` and compares them with the declaration in
both directions ([Delivery targets](../cmake/README.md#delivery-targets-declared-and-checked-against-the-build-graph-p14)).

The receipt is printed by `cmake/asset_receipt.py` every time the target runs,
with the same text on every board; only the name, slot and CRC differ:

```
  asset blazeface    /.../build/<board>/asset/blazeface.nnc
  slot <n>           <bytes> B   crc32 <CRC>

  on the board:  blob write blazeface <n>   (erases the slot, then waits for the file)
                 ...
  afterwards:    `blob list` must show crc32 <CRC> -- that is how you know
                 the bytes that were built are the bytes that were stored.
  then:          nn model load blazeface
```

The CRC is CRC-32/ISO-HDLC over the whole file (Python's `zlib.crc32`), the
same function as `svc/crc32.c`, recomputed from the file each time the receipt
is printed.  It changes whenever the build id changes (the commit the tree was
configured at, stamped into the manifest), even if no plugin source changed.

## The blob store and its slot tables

Each board has a flash region divided into fixed **slots** -- the external NOR
on Grove and wio-lite-ai, the QSPI flash on f746g-disco.  One blob fills one
slot.  The shared code is `svc/blob/` and the one `blob` command,
`shell/cmds/cmd_blob.c`; what each file does and how each `blob` subcommand
behaves is in
[`svc/README.md`](../svc/README.md#the-asset-stores-shared-parts-issue-192).

```
  slot base          header: the first erase unit
                       page 0 (256 B): magic, written last
                       page 1 (256 B): version, slot base, length,
                                       payload CRC-32, name, body CRC-32
  base + erase unit  payload: the container (or a bare model)
  base + size        next slot
```

- **A slot table is a list of (base, size)**, each board's
  `src/blob_slots.c`, under the fixed names `blob_map_slots[]` and
  `blob_map_slots_count`.  `blob_map_check()` checks it against the board's
  writable interval and erase unit before the store is used.
- **The base address is the blob's identity.**  The header records the base of
  its own slot, so a header read at another base is not accepted.  That is
  why **a table may only be appended to once it has shipped**: moving a slot
  strands what is in it.  Each board has had its table rebuilt from scratch
  once (Grove at #94, wio-lite-ai at #193; f746g-disco started from
  wio-lite-ai's at #194), and each board's own test pins every entry
  (`test/test_blob_slots.c` on wio-lite-ai and f746g-disco,
  `test/test_blob_map.c` on Grove).
- **A slot's state is decided by its header alone** (`svc/blob/blob_state.h`):
  `empty` (both header pages erased), `valid` (magic present, body decodes),
  `incomplete` (no magic, body decodes -- an interrupted or failed write) and
  `invalid` (anything else, including flash this firmware never wrote).
  **`empty` means "no header", not "blank flash"**: the payload area may still
  hold an old model or, on Grove, factory data.  `blob write` erases the whole
  slot, so read `blob list` (and, on Grove, `nor scan`) first.
- **The magic page is written last**, only after the payload has been read
  back whole and matched the CRC of what arrived (`svc/blob/blob_write.h`), so
  a slot is `valid` only when its bytes are known good.
- **The build sees the table too.**  `asset/tools/slot_table.c` is compiled
  with the board's `src/blob_slots.c`, the shared lookups and the firmware's
  own interval and erase unit, refuses a table the store would refuse, and
  writes `plugin/slot_table.json`; `build_asset.py` checks every row of it and
  the declared slot's payload size ([Asset destination](../cmake/README.md#asset-destination-a-store-slot-required-159-195)).
  On wio-lite-ai and f746g-disco, whose model is copied whole into one
  staging buffer, `cmake/check_blob_staging.py` also refuses after the link
  any slot whose payload is larger than that buffer
  ([Blob slots against the model staging buffer](../cmake/README.md#blob-slots-against-the-model-staging-buffer-check_blob_stagingpy-193)).

The three tables, and why each looks the way it does:

| Board | Table | Described in |
|---|---|---|
| Grove Vision AI V2 | `boards/grove-vision-ai-v2/src/blob_slots.c` | [`blob` -- the asset store](../boards/grove-vision-ai-v2/README.md#blob----the-asset-store-issue-92-49-step-2), and [the blob area is not empty flash](../boards/grove-vision-ai-v2/README.md#-the-blob-area-is-not-empty-flash-and-the-first-write-destroys-what-is-there) |
| Wio Lite AI | `boards/wio-lite-ai/src/blob_slots.c` | [The NOR asset store](../boards/wio-lite-ai/README.md#the-nor-asset-store-blob-issue-193) |
| STM32F746G-DISCO | `boards/f746g-disco/src/blob_slots.c` | [The QSPI asset store](../boards/f746g-disco/README.md#the-qspi-asset-store-blob-issue-194) |

## Sending, and checking what arrived

The build checks the file it published; it cannot see what you send.
picocom's `sb -k` sends whatever path is typed, and `blob write` receives into
whatever slot is typed.  So the step that closes the loop is yours:

1. `blob list` first, to see what the slot holds.
2. `blob write <name> <slot>` on the board, then in picocom `C-a C-s` and the
   path from the receipt.  The command refuses before erasing anything if the
   slot holds another blob or the name is already in another slot; clearing
   either is a deliberate `blob erase <slot>`.
3. **`blob list` must show the receipt's CRC** for that slot.  This is the only
   end-to-end check that the bytes that were built are the bytes that were
   stored.
4. `nn model load <name>`.

The `.nnc` under `build/` is the last one **packed**, not the last one
**sent**.  To say what a board holds, start from `blob list`'s CRC and the
receipt of a transfer you actually made, never from the build tree.

What still gets through, because the board checks structure and target and not
identity, is listed in Grove's
[What moving the gate to build time gives up](../boards/grove-vision-ai-v2/README.md#-what-moving-the-gate-to-build-time-gives-up):
a stale file from another build, a container that pairs the wrong model with
the wrong plugin, and damage to the model section that the backend's own model
checks still accept.

## On the board: from `nn model load` to the first call

1. **Find and read.**  The name is looked up among the `valid` slots (exactly
   one must carry it).  wio-lite-ai and f746g-disco copy the payload into the
   model staging buffer; Grove reads it in place through the memory-mapped
   flash window, holding the window's lease while it does.
2. **Classify** with `plugin_probe()`: container, bare `.tflite`, or neither.
3. **Validate the whole container** with `plugin_parse()` against the board's
   `plugin_board_policy`: header, sections, model alignment, then -- if there
   is a plugin -- manifest, target word, link address, capabilities, sizes,
   segments, entry points, stack, and the digest last.  Every refusal has its
   own name (`plugin_result_name()`).  The result, `struct plugin_view`, is
   plain data: offsets and copied bytes with **no function pointer in it**,
   so validating cannot run anything.
4. **Load the plugin**, after the backend has accepted the model
   (`svc/nn/nn_core_model.c` holds that order), through the board's
   `plugin_run_start()` into the shared `plugin_exec_start()`
   (`svc/plugin/plugin_exec.c`):
   - the caller must hold the plugin lease, or nothing is touched;
   - the previous plugin is unpublished first, even when the new container
     has no plugin;
   - the board confirms the bytes are still where it said (`source_ok`: the
     staging buffer it was handed, or Grove's window lease);
   - the image is copied into the reservation and bss is zeroed;
   - the board's cache maintenance runs over the whole reservation, not just
     the image;
   - **the MPU is read back and judged** (`exec_ok`): the reservation must be
     executable, readable and writable ordinary memory right now.  The judges
     are pure, host-tested functions -- `plugin_mpu_judge()` for Grove's
     Armv8-M MPU (`svc/plugin/plugin_mpu.c`) and `pl_mpu7_judge()` for the
     Armv7-M boards (`svc/plugin/plugin_mpu_v7m.c`) -- and anything they cannot
     decide, such as an incomplete register snapshot, is a refusal;
   - the lease is asked again at that point, and entry's stack depth is
     recorded;
   - the plugin is published, and only then is its ENTRY called with the
     board's table of calls (`plugin_board_bindings`).  A non-zero answer
     unloads it.
5. **Calls after that** go through `plugin_run_slot()`, which every board's
   decoder layer uses.  The one place an offset becomes an address is
   `plugin_exec_slot()` in `svc/plugin/plugin_exec.c` (the loader's own ENTRY
   call uses the same helper), and it returns NULL for an absent slot or when
   no plugin has finished its ENTRY.  `plugin_run_slot()` itself is a one-line
   wrapper: `svc/plugin/plugin_run_core.c` for wio-lite-ai and f746g-disco,
   Grove's own `port/plugin/plugin_run.c`.

A fault inside the plugin is reported with the plugin's name and the offset
into its image (`plugin_exec_attribute()`).  `nn info` shows the loaded
plugin's name, build id and digest.

### What a plugin gets, and what it does not

The plugin calls into the firmware only through the table it is given: a log
line and the conversion of a box from model coordinates to frame pixels
(`struct plugin_base_api`).  Its draw callback gets a painter whose rectangles
and copies the firmware clips and charges against a per-call budget
(`svc/plugin/plugin_paint_budget.c`, `svc/plugin/plugin_paint_span.c`); there
is no framebuffer pointer.  Its report callback gets a writer.  The output
tensors it is handed are valid for that one call only.

**The checks do not make a plugin safe.**  The image gate proves a stack bound
and the absence of a list of things (forbidden symbols, unknown sections,
relocations, indirect branches outside the named veneers), not memory safety
and not which addresses it touches.  A plugin is reviewed, trusted native code
with the same standing as board code.

## Stack: what a plugin declares and what the firmware adds

Each callback runs on another thread's stack -- decode on the inference
worker, draw on the panel or display thread, the rest on a console -- and a
plugin does not exist when the firmware is built.  So the limit is set at
three points:

- **The image gate measures** each exported entry point and writes the result
  into the manifest as the plugin's own numbers, with no firmware cost in
  them: A0 (`stack_own`, the deepest its own frames go), A1 (`stack_cross`,
  the deepest on a path that calls into the firmware), a bit per slot saying
  whether it calls into the firmware at all, and S (`stack_sink`, the bound
  on the plugin's own text output function, which the firmware may call
  back).  The gate's stack analysis is `cmake/thumb_stack_analysis.py`, the
  same code that bounds the firmware's side.
- **The firmware adds its own cost at load time.**  c is the stack the
  firmware spends below one call from a plugin, checked on every build
  against the shipped image by the veneer-cost gate and compiled in as
  `PLUGIN_VENEER_BASE_COST`
  ([Plugin veneer cost gate](../cmake/README.md#plugin-veneer-cost-gate)).
  The requirement of a slot is

  ```
  need = max(A0, A1 + max(c, S))   when the slot calls into the firmware
  need = A0                        otherwise
  ```

  and it must not exceed that slot's `stack_limit`.  Because c is not in the
  container, changing c re-packs nothing; a container that no longer fits is
  refused on the board.  A limit of 0 refuses the slot outright, and a
  measured need of 0 is a real measurement, not "absent" (absence is the
  entry-point offset 0).
- **The limits are one table per board**, `plugin_stack_table()` in its
  `board.cmake` (`cmake/plugin_stack_table.cmake`).  The firmware's policy,
  the image gate and `verify_container` all take their numbers from it, and
  `cmake/check_policy_probe.py` reads the seven numbers back out of the
  shipped firmware and compares them
  ([Plugin stack allowances](../cmake/README.md#plugin-stack-allowances-one-table-read-back-from-the-image)).
  Each allowance is declared against the shallowest thread that can reach
  its slot and asserted at compile time to be below every such thread's
  stack; the depths it is derived from are measured at the callback's entry
  and are in each board README.

`stack_accounting` names the version of the stack analysis.  If what the
analysis charges, where it stops, or what it accepts changes -- even to make
the bound more precise -- the version goes up, and the loader refuses older
containers as "declared under another analysis" so they are re-packed rather
than read under new rules.

## The plugin lease: one callback at a time

No two callbacks of one plugin run at once, on any board.  Every path into a
plugin -- ENTRY, the shape check, decode, draw, report, the parameters, and
loading or unloading one -- holds the board's plugin lease
(`svc/plugin/plugin_lease_api.h`, implemented by each board's
`port/plugin/plugin_lease.c`).  So what decode leaves for draw and report needs
no lock of the plugin's own.

- The inference worker and the consoles wait for it, up to one fixed bound per
  board (`PLUGIN_LEASE_WAIT_MS` in the board's `port/plugin/plugin_lease.h`).
  A wait that runs out is an answer (busy), never permission to go in.
- The panel only tries; when it fails the frame is shown without the overlay
  and the miss is counted.
- Each entry point checks that the calling thread holds the lease and refuses
  (and counts) a caller that does not.  Which entry points exist is derived
  from the compiled objects by `cmake/check_lease_entries.py`, not listed by
  hand ([Plugin lease entries](../cmake/README.md#plugin-lease-entries-derived-from-the-objects-check_lease_entriespy)).

## Firmware and container are separate products

Flashing the firmware does not update the containers on the board, and
sending a container does not need a reflash.  Mistakes go both ways:

| You changed | The board still has | What happens |
|---|---|---|
| a plugin's source | the old container | Nothing changes until you build `asset-<name>`, send it and `nn model load` it again. |
| the firmware, not what containers depend on | the old containers | They keep loading; no re-send is needed. |
| the firmware's target word, plugin reservation address or size, a stack limit, c, `PLUGIN_ABI_VERSION` or `PLUGIN_STACK_ACCOUNTING` | containers packed for the old values | `nn model load` refuses each one that no longer matches or no longer fits, with a named reason; re-pack and re-send it. Moving the reservation invalidates every container, because plugins are prelinked. |
| a slot table entry that has shipped | blobs at the old bases | They can no longer be found by name; this is why tables are only appended to. |

## Where each board's details are

| | Grove Vision AI V2 | Wio Lite AI | STM32F746G-DISCO |
|---|---|---|---|
| Store and slot table | [`blob` -- the asset store](../boards/grove-vision-ai-v2/README.md#blob----the-asset-store-issue-92-49-step-2) | [The NOR asset store](../boards/wio-lite-ai/README.md#the-nor-asset-store-blob-issue-193) | [The QSPI asset store](../boards/f746g-disco/README.md#the-qspi-asset-store-blob-issue-194) |
| Containers on this board | [Model containers](../boards/grove-vision-ai-v2/README.md#model-containers-issue-101), [The plugin runs](../boards/grove-vision-ai-v2/README.md#the-plugin-runs-issue-103--78-step-1b) | [Plugin containers](../boards/wio-lite-ai/README.md#plugin-containers-issues-108-110-116--78-steps-3a-3b-and-3c) | [Plugin containers](../boards/f746g-disco/README.md#plugin-containers-issue-159--78-step-4) |
| Plugin reservation | [The reservation](../boards/grove-vision-ai-v2/README.md#the-reservation) | [The top 32 KB of AXI-SRAM](../boards/wio-lite-ai/README.md#-the-top-32-kb-of-axi-sram-is-the-plugin-reservation-issue-108) | [The top 32 KB of internal SRAM](../boards/f746g-disco/README.md#-the-top-32-kb-of-internal-sram-is-the-plugin-reservation-issue-159) |
| MPU read-back | [The MPU is read back before the branch](../boards/grove-vision-ai-v2/README.md#the-mpu-is-read-back-before-the-branch) | `port/plugin/plugin_run.c` | `port/plugin/plugin_run.c`, [MPU regions](../boards/f746g-disco/README.md#mpu-regions) |
| Stack numbers | [What a plugin may spend](../boards/grove-vision-ai-v2/README.md#what-a-plugin-may-spend-and-where-the-numbers-come-from) | [The stack budget](../boards/wio-lite-ai/README.md#the-stack-budget-and-what-is-still-owed-to-it-issue-110) | [Plugin containers](../boards/f746g-disco/README.md#plugin-containers-issue-159--78-step-4) |
| Lease | [One lock into the plugin](../boards/grove-vision-ai-v2/README.md#one-lock-into-the-plugin-issue-127) | [The lease that separates them](../boards/wio-lite-ai/README.md#-what-decodes-draws-and-reports-and-the-lease-that-separates-them) | `port/plugin/plugin_lease.h` |
| Sending | [Sending a container](../boards/grove-vision-ai-v2/README.md#sending-a-container) | [Sending an asset](../boards/wio-lite-ai/README.md#sending-an-asset) | [Building this board's containers](../boards/f746g-disco/README.md#building-this-boards-containers) |
