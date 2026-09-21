#!/usr/bin/env python3
"""Negative tests for cmake/check_plugin_reservation.py (issue #108).

[!] SYNTHETIC IMAGES, ON PURPOSE.  The firmware's linker script ASSERTs the same
rules at link time, so an image built from it with the reservation broken never
LINKS -- and a fixture that is refused by the linker proves nothing about the
gate, which is the check that still holds when an ASSERT is edited, weakened or
defeated.  So each fixture links a small image against a script that carries the
relevant shape of the firmware's AXI-SRAM layout and none of its ASSERTs, then
breaks exactly one rule.

The control case PASSES.  Every refusal differs from it in one line of the
script, and each is asserted on its own diagnostic, so a fixture that starts
failing for some other reason is a test failure rather than a pass.
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(os.path.dirname(HERE), "check_plugin_reservation.py")

# `end` is PROVIDEd, so it exists only if something references it -- in the
# firmware that is src/retarget.c.  The fixture references it the same way, or
# every case would fail on "no heap floor" instead of on what it was built for.
#
# The image also carries an `_sbrk` shaped like src/retarget.c's, bounded by
# @LIMIT@, because the gate reads the heap bound where it is ENFORCED (check 5):
# a correct __heap_end symbol next to an _sbrk that ignores it must still fail.
SRC = """
extern char end;
extern char @LIMIT@;
char *plugin_fixture_floor = &end;
int plugin_fixture_data = 1;
int plugin_fixture_bss;
__attribute__((section(".intruder"))) int plugin_fixture_intruder = 2;
@SBRK@
void Reset_Handler(void) { for (;;) { plugin_fixture_bss++; @CALL@ } }
"""
SBRK = """
static char *plugin_fixture_brk;
void *_sbrk(int incr)
{
    char *prev = plugin_fixture_brk ? plugin_fixture_brk : &end;
    if ((unsigned long)(&@LIMIT@ - prev) < (unsigned long)incr)
        return (void *)-1;
    plugin_fixture_brk = prev + incr;
    return prev;
}
"""

# The firmware's AXI-SRAM tail, reduced to what the gate reasons about.
SCRIPT = """
ENTRY(Reset_Handler)
MEMORY { RAM (xrw) : ORIGIN = 0x24000000, LENGTH = 320K }
SECTIONS
{
  .text : { *(.text*) } >RAM
  .data : { *(.data*) } >RAM
  .bss  : { *(.bss*) *(COMMON) } >RAM
  ._user_heap_stack : { . = ALIGN(8); PROVIDE(end = .); . = . + 0x600; } >RAM
  @INTRUDER@
  .plugin @BASE@ @NOLOAD@ :
  {
    __plugin_start = .;
    @FILL@
    __plugin_end = .;
  } >RAM
  __heap_end = @CEILING@;
  __ram_end = ORIGIN(RAM) + LENGTH(RAM);
  @STRAY@
  /DISCARD/ : { *(.intruder) }
}
"""

CONTROL = {
    "@BASE@": "0x24048000",
    "@NOLOAD@": "(NOLOAD)",
    "@FILL@": ". += 0x8000;",
    "@CEILING@": "__plugin_start",
    "@INTRUDER@": "",
    "@STRAY@": "",
    "@LIMIT@": "__heap_end",
    "@SBRK@": SBRK,
    "@CALL@": "(void)_sbrk(0);",
}

# (name, overrides, extra link flags, who refuses: None | "gate" | "link",
#  diagnostic substring)
#
# [!] "link" IS RECORDED, NOT SKIPPED.  ld refuses overlapping sections by
# itself -- loadable or NOLOAD alike, measured -- so in a normal link an
# intruding section never reaches the gate.  Saying so here keeps anyone from
# reading the gate's pass on the firmware as evidence that its overlap check
# ran.  What the gate adds is the same answer when that check is switched off
# (--no-check-sections), and the checks ld does not make at all: the address,
# the size, NOLOAD, a stray symbol and the heap ceiling.
INTRUDER = ".intruder 0x2404A000 (NOLOAD) : { KEEP(*(.intruder)) } >RAM"
CASES = [
    ("control", {}, [], None, None),
    ("moved", {"@BASE@": "0x24040000"}, [], "gate",
     "a prelinked address may not move"),
    ("short", {"@FILL@": ". += 0x4000;"}, [], "gate",
     "a prelinked address may not move"),
    # (NOLOAD) wins over a BYTE() inside it, so the keyword itself has to go.
    ("loaded", {"@NOLOAD@": "", "@FILL@": "BYTE(0x5A) . += 0x7FFF;"}, [],
     "gate", "it must be NOLOAD"),
    ("heap_above", {"@CEILING@": "__plugin_end"}, [], "gate",
     "the heap could grow into a plugin"),
    ("section_inside", {"@INTRUDER@": INTRUDER}, [], "link",
     "overlaps section .plugin"),
    ("unchecked_inside", {"@INTRUDER@": INTRUDER},
     ["-Wl,--no-check-sections"], "gate",
     "overlaps the .plugin reservation"),
    ("symbol_inside", {"@STRAY@": "plugin_fixture_stray = 0x24049000;"}, [],
     "gate", "lies inside the .plugin reservation"),
    # [!] THE SYMBOL IS RIGHT AND THE CODE IGNORES IT.  __heap_end is defined at
    # the base, exactly as in the control -- only _sbrk changed, back to the
    # pre-#108 __ram_end.  A gate that checked the symbol alone passed this.
    ("sbrk_ram_end", {"@LIMIT@": "__ram_end"}, [], "gate",
     "does not name __heap_end"),
    ("no_sbrk", {"@SBRK@": "", "@CALL@": ""}, [], "gate",
     "_sbrk: not in the image"),
]


def build(cc, work, overrides, ldflags):
    subs = dict(CONTROL, **overrides)
    script = SCRIPT
    src = SRC
    # @SBRK@ first: its text carries @LIMIT@ too.
    for k in ["@SBRK@"] + [k for k in subs if k != "@SBRK@"]:
        script = script.replace(k, subs[k])
        src = src.replace(k, subs[k])
    # The intruder is discarded unless a fixture places it: input sections go
    # to the FIRST output section that names them, so a placed .intruder wins
    # over the /DISCARD/ that follows it.
    ld = os.path.join(work, "t.ld")
    with open(ld, "w") as fh:
        fh.write(script)
    c = os.path.join(work, "t.c")
    with open(c, "w") as fh:
        fh.write(src)
    elf = os.path.join(work, "t.elf")
    r = subprocess.run([cc, "-mcpu=cortex-m7", "-mthumb", "-Os", "-nostdlib",
                        "-nostartfiles", "-Wl,--no-warn-rwx-segments"]
                       + ldflags + ["-T", ld, c, "-o", elf],
                       capture_output=True, text=True)
    return r.returncode, r.stderr, elf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    args = ap.parse_args()

    print("run_reservation_tests (check_plugin_reservation.py):")
    bad = 0
    for name, overrides, ldflags, who, expect in CASES:
        with tempfile.TemporaryDirectory() as work:
            rc, err, elf = build(args.cc, work, overrides, ldflags)
            if rc != 0:
                ok = who == "link" and expect in err
                why = "linker: " + (expect or "")
                if not ok:
                    print(f"  FAIL {name:17s} the fixture did not link -- it "
                          f"tests nothing:\n{err.strip()[:400]}")
                    bad += 1
                else:
                    print(f"  ok   {name:17s} {why} (never reaches the gate)")
                continue
            r = subprocess.run([sys.executable, GATE, elf, "--nm", args.nm,
                                "--objdump", args.objdump],
                               capture_output=True, text=True)
        out = r.stdout + r.stderr
        if who is None:
            ok = r.returncode == 0
            why = "accepted (every refusal below differs from this in one line)"
        else:
            ok = who == "gate" and r.returncode == 1 and expect in out
            why = "gate: " + expect
        if ok:
            print(f"  ok   {name:17s} {why}")
        else:
            bad += 1
            print(f"  FAIL {name:17s} rc={r.returncode}, expected "
                  f"{expect or 'a pass'}\n{out.strip()[:400]}")
    if bad:
        print("run_reservation_tests: FAILED", file=sys.stderr)
        return 1
    print("run_reservation_tests: all fixtures behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
