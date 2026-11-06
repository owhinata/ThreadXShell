#!/usr/bin/env python3
"""Post-link guard: every blob slot's payload fits the model staging buffer
(issue #193; shared by wio-lite-ai and f746g-disco since issue #194).

WHY THIS EXISTS
---------------
`nn model load <name>` copies a blob's whole payload out of the NOR into the
tflm backend's one staging buffer before anything parses it, and refuses a blob
longer than the buffer.  So a slot whose payload area is larger than the buffer
is space the store will happily fill -- `blob write` takes the file -- with a
model this firmware can never load, and the operator finds out only after the
transfer.  The two numbers live in different places (the slot table is the
board's src/blob_slots.c, the buffer is NN_TFLM_MODEL_KB in the board's
cmake/tflite-micro.cmake, private to the tflm library), so no C _Static_assert
can see both without wiring one into the other.  This reads BOTH out of the
SHIPPED IMAGE and requires

    staging capacity >= the largest slot's size - the header unit

[!] READ BACK, NOT RESTATED.  The table is the const `blob_map_slots[]` and its
`blob_map_slots_count` (the fixed names svc/blob/blob_map.h declares, kept in
the image by -Wl,--require-defined in board.cmake: LTO folds the count into
code, and without a reader --gc-sections drops both); the capacity is the const
`nn_tflm_stage_cap` the backend hands out (svc/nn/tflm/nn_tflm_mem.h), and the
buffer behind it is `nn_tflm_model_buf`, which must be at least that long.  The
words are read from the image's loaded bytes, like check_policy_probe.py.

THE BOARD PASSES ITS OWN FACTS, AND ONLY THOSE (issue #194).
  --header-bytes      the header unit at the top of every slot (the board's
                      src/blob.h BLOB_ERASE_UNIT)
  --backend           what board.cmake compiled (CONFIG_NN_BACKEND)
  --staged-backend    each backend of the board that loads a model through the
                      tflm staging buffer (and so must hold it)
  --unstaged-backend  each backend that does not (and so must not)
A backend in neither list could not be checked, which stops the build.

[!] TWO NUMBERS ARE RESTATED, AND THE HOST TEST TIES THEM TO THE FIRMWARE.
--header-bytes is a literal in the board's board.cmake and ENTRY_BYTES (here)
is sizeof(struct blob_slot), read as <base, size>.  The host test
(cmake/fixtures/run_blob_staging_tests.py, from each board's test/host_tests.sh)
compiles the board's blob.h and blob_map.h, reads the --header-bytes the
board.cmake passes, and refuses when any of them differs, so a change to one
side alone fails the suite rather than quietly loosening this gate.
(ENTRY_BYTES is also checked against the image: the table's size must be
exactly count x ENTRY_BYTES.)

[!] "NOT APPLICABLE" IS DECIDED BY THE BUILD AND CHECKED AGAINST THE IMAGE.
An unstaged backend (`null`; on f746g-disco also `stedgeai`, whose model is
linked in, and `stedgeai_reloc`, which loads through its own loader into its
own 448 KB slots and refuses at load what does not fit) has nothing to fit --
but this does not take that on trust: an unstaged image that holds the staging
symbols, or a staged one that does not, is a refusal.  The table is read either
way.  Reading nothing is never a pass.

Exit status 0 = pass (or not applicable, said so); 1 = a slot does not fit or
the image contradicts itself; 2 = the check could not be performed.  The build
treats 1 and 2 alike: either stops the link edge, so flash and the assets
(which wait on the firmware) do not run.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from thumb_stack_analysis import Elf, InputError  # noqa: E402

CANNOT_CHECK = 2

# sizeof(struct blob_slot) -- { uint32_t base; uint32_t size; }, svc/blob/blob_map.h.
ENTRY_BYTES = 8

TABLE = "blob_map_slots"
COUNT = "blob_map_slots_count"
CAP = "nn_tflm_stage_cap"
BUF = "nn_tflm_model_buf"

SHT_NOBITS = 8
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
STT_OBJECT = 1


class Refuse(Exception):
    pass


def named(elf, name):
    """Every symbol called exactly @name (LTO may make it local; it may not
    rename it -- a renamed one is not found, which is a refusal)."""
    return [s for s in elf.symbols if s[0] == name]


def one_object(elf, name):
    hits = named(elf, name)
    if len(hits) != 1:
        raise Refuse("%d symbols named %s; the image must hold it exactly "
                     "once" % (len(hits), name))
    _, addr, size, typ, _ = hits[0]
    if typ != STT_OBJECT:
        raise Refuse("%s is not a data object (type %d)" % (name, typ))
    if size == 0:
        raise Refuse("%s has no size in the symbol table" % name)
    return addr, size


def const_bytes(elf, name):
    """The loaded bytes of a const object, refused anywhere else."""
    addr, size = one_object(elf, name)
    home = [s for s in elf.sections
            if s.flags & SHF_ALLOC and s.type != SHT_NOBITS and s.size
            and s.addr <= addr and addr + size <= s.addr + s.size]
    if len(home) != 1:
        raise Refuse("%s at 0x%08x+%d is not in loadable bytes"
                     % (name, addr, size))
    if home[0].flags & SHF_WRITE:
        raise Refuse("%s is in %s, a writable section: what the image holds "
                     "there is only an initial value" % (name, home[0].name))
    off = home[0].offset + addr - home[0].addr
    return elf.raw[off:off + size]


def read_table(elf, header):
    raw = const_bytes(elf, TABLE)
    if len(raw) % ENTRY_BYTES:
        raise Refuse("%s is %d B, not a whole number of %d-byte entries"
                     % (TABLE, len(raw), ENTRY_BYTES))
    craw = const_bytes(elf, COUNT)
    if len(craw) != 4:
        raise Refuse("%s is %d B, not an unsigned int" % (COUNT, len(craw)))
    count, = struct.unpack("<I", craw)
    if count == 0:
        raise Refuse("%s is 0: a table with no slots" % COUNT)
    if count * ENTRY_BYTES != len(raw):
        raise Refuse("%s says %d slots but %s is %d B, which is %d entries"
                     % (COUNT, count, TABLE, len(raw), len(raw) // ENTRY_BYTES))
    slots = [struct.unpack_from("<II", raw, i * ENTRY_BYTES)
             for i in range(count)]
    for i, (base, size) in enumerate(slots):
        if size <= header:
            raise Refuse("slot %d (0x%08x) is %d B, no payload after its "
                         "%d B header" % (i, base, size, header))
    return slots


def stage(elf):
    raw = const_bytes(elf, CAP)
    if len(raw) != 4:
        raise Refuse("%s is %d B, not a uint32_t" % (CAP, len(raw)))
    cap, = struct.unpack("<I", raw)
    if cap == 0:
        raise Refuse("%s holds 0, not a capacity" % CAP)
    _, buf = one_object(elf, BUF)
    if buf < cap:
        raise Refuse("%s says %d B but %s is only %d B" % (CAP, cap, BUF, buf))
    return cap, buf


def check(elf, backend, staged, header):
    """(status, message) -- 0 pass / n.a., 1 refusal."""
    slots = read_table(elf, header)
    i, (base, size) = max(enumerate(slots), key=lambda e: e[1][1])
    payload = size - header
    present = [n for n in (CAP, BUF) if named(elf, n)]
    if not staged:
        if present:
            raise Refuse("built as backend %s, but the image holds %s -- "
                         "the build and the image disagree about whether "
                         "there is a staging buffer"
                         % (backend, ", ".join(present)))
        return 0, ("NOT APPLICABLE -- backend %s does not load through the "
                   "tflm staging buffer; the table was read (%d slots, largest "
                   "payload %d B in slot %d) and nothing here is loaded from "
                   "it through that buffer" % (backend, len(slots), payload, i))
    cap, buf = stage(elf)
    if payload > cap:
        raise Refuse("slot %d (0x%08x, %d B) holds %d B of payload, and the "
                     "staging buffer takes %d B: a blob stored there could "
                     "never be loaded" % (i, base, size, payload, cap))
    return 0, ("OK -- %d slots; the largest, slot %d (0x%08x, %d B), holds %d B "
               "of payload; staging %d B (%s %d B), %d B to spare"
               % (len(slots), i, base, size, payload, cap, BUF, buf,
                  cap - payload))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf")
    ap.add_argument("--backend", required=True,
                    help="CONFIG_NN_BACKEND the image was built with")
    ap.add_argument("--staged-backend", action="append", default=[],
                    help="a backend that loads through the staging buffer")
    ap.add_argument("--unstaged-backend", action="append", default=[],
                    help="a backend that does not")
    ap.add_argument("--header-bytes", type=int, required=True,
                    help="the board's header unit (src/blob.h BLOB_ERASE_UNIT)")
    try:
        args = ap.parse_args(argv)
    except SystemExit:
        print("check_blob_staging: CANNOT CHECK -- bad arguments",
              file=sys.stderr)
        return CANNOT_CHECK
    if not args.staged_backend:
        print("check_blob_staging: CANNOT CHECK -- the board names no staged "
              "backend", file=sys.stderr)
        return CANNOT_CHECK
    both = set(args.staged_backend) & set(args.unstaged_backend)
    if both:
        print("check_blob_staging: CANNOT CHECK -- %s is both staged and "
              "unstaged" % ", ".join(sorted(both)), file=sys.stderr)
        return CANNOT_CHECK
    if args.backend in args.staged_backend:
        staged = True
    elif args.backend in args.unstaged_backend:
        staged = False
    else:
        print("check_blob_staging: CANNOT CHECK -- unknown backend %r"
              % args.backend, file=sys.stderr)
        return CANNOT_CHECK
    if args.header_bytes <= 0:
        print("check_blob_staging: CANNOT CHECK -- --header-bytes %d is not a "
              "size" % args.header_bytes, file=sys.stderr)
        return CANNOT_CHECK
    try:
        elf = Elf(args.elf)
    except InputError as exc:
        print("check_blob_staging: CANNOT CHECK -- %s" % exc, file=sys.stderr)
        return CANNOT_CHECK
    try:
        rc, msg = check(elf, args.backend, staged, args.header_bytes)
    except Refuse as exc:
        print("check_blob_staging: FAIL -- %s" % exc, file=sys.stderr)
        return 1
    print("check_blob_staging: %s" % msg)
    return rc


if __name__ == "__main__":
    sys.exit(main())
