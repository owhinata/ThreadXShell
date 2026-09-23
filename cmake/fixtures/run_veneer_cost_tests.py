#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative (and exact positive) tests for check_veneer_base_cost.py (#112).

[!] EVERY REFUSAL IS ASSERTED FOR ITS OWN REASON: the stage, a substring of
the diagnostic, and the NUMBER of refusals.  A case that failed for some other
reason -- a fixture that no longer links, a witness the harness forgot, a root
that did not resolve -- is a failure of the case, not a pass.  That is how the
plugin gate's recursion fixture went on "passing" for the wrong reason until a
second bug was fixed (issue #103).

[!] AND EVERY ACCEPT IS ASSERTED TO THE BYTE.  "It accepted" says nothing about
whether the number is right; a walk that dropped an edge or trusted -fstack-
usage for a variadic frame would accept too, with a smaller number.  The
expected values never come from the gate itself: the assembler fixtures are
exact by construction, the C ones are summed from the compiler's own records,
and the variadic one from objdump's rendering of the prologue -- three
sources independent of the decoder under test.

[!] WHERE THE SCRIPT CHOOSES ONE OF SEVERAL, THE RIGHT ONE IS IN THE MIDDLE.
The deepest callee, the deepest root, the printer bound that is too large, the
record that says 'dynamic', the prebuilt root that matches, the end of a
symbol with no .size, the stack change inside a loop: in each, the answer is
neither the first nor the last nor the smallest, by address, by call order or
by the order the walk meets it -- so "take the first" or "take the minimum"
fails a case instead of passing every one (both happened: the first version
of these cases always put the deepest callee first).

Everything runs twice, once per core a board here ships: Cortex-M55 (Grove,
Armv8.1-M, with the low-overhead loops and MVE) and Cortex-M7 (wio and f746,
Armv7E-M).  The script under test has no -mcpu in it; these are what make that
claim checkable.

  --gate lets a mutated copy of the script be run against the same cases, which
  is how the cases themselves are tested (see the #112 report).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
GATE = os.path.join(REPO, "cmake", "check_veneer_base_cost.py")

ARCHES = {
    "m55": ["-mcpu=cortex-m55", "-mthumb", "-mfloat-abi=hard"],
    "m7": ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard"],
}
CFLAGS = ["-Os", "-std=gnu11", "-ffreestanding", "-fno-builtin",
          "-fno-common", "-ffunction-sections", "-fno-unwind-tables",
          "-fno-asynchronous-unwind-tables", "-fstack-usage"]
VENEERS = ["pl_base_log", "pl_base_to_frame", "pl_paint_rect",
           "pl_paint_fill_rect", "pl_paint_blit", "pl_print_write"]

LINKER_SCRIPT = """
MEMORY { ITCM (rx) : ORIGIN = 0x00000000, LENGTH = 64K
         FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1M
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
SECTIONS {
  .itcm : { *(.itcm*) } > ITCM
  .text : { *(.text*) } > FLASH
  .rodata : { *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(COMMON) } > RAM
}
"""

# --- the assembler half: shapes that must be exact ---------------------------
# In an archive under a --prebuilt-root, so that none of them needs a witness
# and every refusal below is about the shape, never about provenance.
ASM_COMMON = r"""
    .syntax unified
    .thumb
    .macro FUNC name
    .section .text.\name,"ax",%progbits
    .global \name
    .type \name, %function
    .thumb_func
\name:
    .endm
    .macro END name
    .size \name, . - \name
    .endm

FUNC leaf
    bx lr
END leaf

@ root_chain 8 > ch_a 24 > ch_b 16 > max(ch_c 8, ch_d 40) = 88.  ch_d is only
@ reached through a CONDITIONAL tail call and ch_c through an unconditional
@ one: a walk that drops either edge gets 56.
FUNC root_chain
    push {r3, lr}
    bl ch_a
    pop {r3, pc}
END root_chain
FUNC ch_a
    push {r4, lr}
    sub sp, #16
    bl ch_b
    add sp, #16
    pop {r4, pc}
END ch_a
FUNC ch_b
    push {r4, r5, r6, lr}
    cmp r0, #0
    beq.w ch_d
    pop {r4, r5, r6, lr}
    b.w ch_c
END ch_b
FUNC ch_c
    sub sp, #8
    add sp, #8
    bx lr
END ch_c
FUNC ch_d
    push {r4-r11, lr}
    sub sp, #4
    add sp, #4
    pop {r4-r11, pc}
END ch_d

@ A conditional return in an IT block: 8 + 8 = 16.
FUNC it_ret
    push {r4, lr}
    cmp r0, #0
    it eq
    popeq {r4, pc}
    sub sp, #8
    add sp, #8
    pop {r4, pc}
END it_ret

@ The vocabulary both cores share: 8 + 16 + 1024 + 12 = 1060.
FUNC fp_vocab
    strd r4, lr, [sp, #-8]!
    vpush {d8-d9}
    sub.w sp, sp, #1024
    subw sp, sp, #12
    vmov r0, s0
    vcmp.f32 s0, s1
    vmrs APSR_nzcv, fpscr
    addw sp, sp, #12
    add.w sp, sp, #1024
    vpop {d8-d9}
    ldrd r4, lr, [sp], #8
    bx lr
END fp_vocab

@ vcall 8 > (linker long-branch veneer 0) > far_deep 64 = 72.  far_deep sits
@ 128 MB away, so the linker has to put a veneer between them.
FUNC vcall
    push {r3, lr}
    bl far_deep
    pop {r3, pc}
END vcall
    .section .itcm.far_deep,"ax",%progbits
    .global far_deep
    .type far_deep, %function
    .thumb_func
far_deep:
    push {r4-r7, lr}
    sub sp, #44
    add sp, #44
    pop {r4-r7, pc}
    .size far_deep, . - far_deep

@ Veneer-shaped, but the literal word is EVEN -- no Thumb bit, so it is not a
@ callable Thumb entry.  Only the Thumb-bit check refuses it.
    .section .text.ven_even,"ax",%progbits
    .balign 4
    .global ven_even
    .type ven_even, %function
    .thumb_func
ven_even:
    ldr.w pc, 1f
    .balign 4
1:  .word 0x20000000
    .size ven_even, . - ven_even

@ Veneer-shaped, but the literal word sits in a following blob, OUTSIDE this
@ body's own data -- the word there is far_deep's real (odd) address, so
@ dropping the own-body guard would follow it as a veneer.
    .section .text.ven_out,"ax",%progbits
    .balign 4
    .global ven_out
    .type ven_out, %function
    .thumb_func
ven_out:
    ldr.w pc, [pc, #0]
    .size ven_out, . - ven_out
ven_out_blob:
    .word far_deep

@ Veneer-shaped, literal in its own data, but the word is not any function's
@ entry.
    .section .text.ven_nonode,"ax",%progbits
    .balign 4
    .global ven_nonode
    .type ven_nonode, %function
    .thumb_func
ven_nonode:
    ldr.w pc, 1f
    .balign 4
1:  .word 0x08ffff01
    .size ven_nonode, . - ven_nonode

FUNC tbb_fn
    tbb [pc, r0]
1:  .byte (2f - 1b) / 2, (3f - 1b) / 2
    .align 1
2:  bx lr
3:  bx lr
END tbb_fn

FUNC sp_reg_fn
    sub.w sp, sp, r0
    bx lr
END sp_reg_fn

FUNC loop_fn
    push {r4, lr}
1:  sub sp, #8
    subs r0, #1
    bne 1b
    pop {r4, pc}
END loop_fn

FUNC other_fn
    push {r4, lr}
    nop
    nop
    pop {r4, pc}
END other_fn
FUNC interior_fn
    b.w other_fn + 4
END interior_fn

FUNC stray_fn
    b.w stray_label
END stray_fn
    .section .text.stray_blob,"ax",%progbits
stray_label:
    bx lr

FUNC undec_fn
    .inst.w 0xee000000
    bx lr
END undec_fn

FUNC falloff_fn
    push {r4, lr}
    bl leaf
END falloff_fn

@ A 32-bit instruction whose second halfword is past the body's code region:
@ a lone f000-prefix halfword at the .size boundary.
    .section .text.trunc_fn,"ax",%progbits
    .global trunc_fn
    .type trunc_fn, %function
    .thumb_func
trunc_fn:
    push {r4, lr}
    .inst.n 0xf000
    .size trunc_fn, . - trunc_fn
    .word 0x12345678

@ A trap in the middle of a body: an exception, not a call.
    .section .text.udf_fn,"ax",%progbits
    .global udf_fn
    .type udf_fn, %function
    .thumb_func
udf_fn:
    push {r4, lr}
    udf #0
    pop {r4, pc}
    .size udf_fn, . - udf_fn

@ A body that falls through into its own trailing data.
    .section .text.data_fall,"ax",%progbits
    .global data_fall
    .type data_fall, %function
    .thumb_func
data_fall:
    push {r4, lr}
    nop
    .word 0xffffffff
    .size data_fall, . - data_fall

@ A branch whose target is inside an IT block.
    .section .text.into_it,"ax",%progbits
    .global into_it
    .type into_it, %function
    .thumb_func
into_it:
    cmp r0, #0
    b 1f
    it eq
1:  moveq r1, #2
    bx lr
    .size into_it, . - into_it

FUNC badret_fn
    mov lr, r3
    bx lr
END badret_fn

FUNC badpop_fn
    pop {r4, pc}
END badpop_fn

@ The other ways to write sp or pc, one refusal each.
FUNC msr_sp_fn
    msr psp, r0
    bx lr
END msr_sp_fn
FUNC ldr_sp_fn
    ldr.w sp, [r0, #4]
    bx lr
END ldr_sp_fn
FUNC bx_reg_fn
    bx r3
END bx_reg_fn
FUNC ldr_pc_fn
    ldr.w pc, [r0]
END ldr_pc_fn
FUNC mov_pc_fn
    mov pc, r3
END mov_pc_fn
@ The veneer's own instruction, but not in the veneer's shape -- and placed at
@ 2 mod 4, so that the literal's address depends on Align(PC, 4).
    .section .text.litpc_fn,"ax",%progbits
    .balign 4
    .global litpc_fn
    .type litpc_fn, %function
    .thumb_func
litpc_fn:
    push {r4, lr}
    ldr.w pc, 1f
    .align 2
1:  .word leaf
END litpc_fn

@ [!] SIBLINGS: the deepest callee is the MIDDLE one by address (low < deep
@ < high), by call order, and by the order the walk meets the calls -- so a
@ walk that keeps the first, the last or the shallowest callee is wrong here.
@ sib_root = 8 + sib_deep (20 + 100) = 128.  sib_branchy meets its calls in
@ the order high, deep, low (the walk goes fall-through first): 8 + 120 = 128.
FUNC sib_low
    push {r4, lr}
    pop {r4, pc}
END sib_low
FUNC sib_deep
    push {r4-r7, lr}
    sub sp, #100
    add sp, #100
    pop {r4-r7, pc}
END sib_deep
FUNC sib_high
    sub sp, #16
    add sp, #16
    bx lr
END sib_high
FUNC sib_root
    push {r3, lr}
    bl sib_low
    bl sib_deep
    bl sib_high
    pop {r3, pc}
END sib_root
FUNC sib_branchy
    push {r3, lr}
    cmp r0, #0
    beq 1f
    bl sib_high
    pop {r3, pc}
1:  bl sib_deep
    cmp r1, #0
    beq 2f
    pop {r3, pc}
2:  bl sib_low
    pop {r3, pc}
END sib_branchy

@ A body that is complete itself, below which one is not: inc_root must be
@ reported as a LOWER bound (8 + 40 = at least 48).
FUNC inc_root
    push {r3, lr}
    bl inc_mid
    pop {r3, pc}
END inc_root
FUNC inc_mid
    push {r4, lr}
    sub sp, #32
    blx r3
    add sp, #32
    pop {r4, pc}
END inc_mid

@ The incomplete callee is NOT the last one (8 + 40 = at least 48), and a
@ root that is incomplete ITSELF above a complete callee (8 + 8 = at least 16).
FUNC inc_first
    push {r3, lr}
    bl inc_mid
    bl sib_low
    pop {r3, pc}
END inc_first
FUNC inc_self
    push {r3, lr}
    blx r3
    bl sib_low
    pop {r3, pc}
END inc_self

@ The veneer's shape with its literal in the SECOND of two data regions:
@ 0 + far_deep 64 = 64.
    .section .text.vlike,"ax",%progbits
    .global vlike
    .type vlike, %function
    .balign 4
    .thumb_func
vlike:
    ldr.w pc, 2f
    .word 0
    nop
    nop
2:  .word far_deep
    .size vlike, . - vlike

@ [!] NO .size: sz0_a ends where sz0_b begins (the next function, before the
@ end of its input section): 8.  sz0_c ends at the end of its input section,
@ before an unlabelled blob that precedes the next function: 8.
    .section .text.sz0_pair,"ax",%progbits
    .global sz0_a
    .type sz0_a, %function
    .thumb_func
sz0_a:
    push {r4, lr}
    pop {r4, pc}
    .global sz0_b
    .type sz0_b, %function
    .thumb_func
sz0_b:
    push {r4-r11, lr}
    pop {r4-r11, pc}
    .size sz0_b, . - sz0_b
    .section .text.sz0_alone,"ax",%progbits
    .global sz0_c
    .type sz0_c, %function
    .thumb_func
sz0_c:
    push {r4, lr}
    pop {r4, pc}
    .section .text.sz0_blob,"ax",%progbits
    push {r4-r11, lr}
    pop {r4-r11, pc}
FUNC sz0_after
    bx lr
END sz0_after

@ One symbol's .size runs over the next symbol: overlapping extents.
    .section .text.ovl,"ax",%progbits
    .global ovl_a
    .type ovl_a, %function
    .thumb_func
ovl_a:
    push {r4, lr}
    pop {r4, pc}
    .size ovl_a, 8
    .global ovl_b
    .type ovl_b, %function
    .thumb_func
ovl_b:
    bx lr
    .size ovl_b, . - ovl_b

@ Two symbols for one body that disagree about its size.
    .section .text.alias_fn,"ax",%progbits
    .global alias_a
    .type alias_a, %function
    .global alias_b
    .type alias_b, %function
    .thumb_func
alias_a:
    .thumb_func
alias_b:
    push {r4, lr}
    nop
    pop {r4, pc}
    .size alias_a, . - alias_a
    .size alias_b, 4

@ Joins: lr is overwritten on ONE of two paths into the bx lr; lr is saved on
@ one of two paths into the pop.  Neither return is proven.
FUNC lr_join_fn
    cmp r0, #0
    beq 1f
    mov lr, r3
1:  bx lr
END lr_join_fn
FUNC saved_join_fn
    cmp r0, #0
    beq 1f
    push {r4, lr}
1:  pop {r4, pc}
END saved_join_fn

@ The stack change is neither the head nor the tail of its loop.
FUNC loop_mid_fn
    push {r4, lr}
1:  subs r0, #1
    sub sp, #8
    nop
    bne 1b
    pop {r4, pc}
END loop_mid_fn

@ The code after a CONDITIONAL return is reachable; a walk that took `popeq`
@ for an unconditional return would never see the blx.
FUNC it_ind
    push {r4, lr}
    cmp r0, #0
    it eq
    popeq {r4, pc}
    blx r3
    pop {r4, pc}
END it_ind
"""

ASM_M55 = r"""
@ Armv8.1-M: CSEL, the MVE scalar shifts and a low-overhead loop, none of which
@ moves sp: 8.
FUNC m55_vocab
    push {r4, lr}
    csel r0, r1, r2, eq
    asrl r0, r1, #3
    lsll r2, r3, r4
    wls lr, r0, 2f
1:  adds r1, #1
    le lr, 1b
2:  vstrw.32 q0, [r1]
    vldrh.u16 q1, [r2], #16
    pop {r4, pc}
END m55_vocab

FUNC le_loop_fn
    push {r4, lr}
    dls lr, r0
1:  sub sp, #8
    le lr, 1b
    pop {r4, pc}
END le_loop_fn

FUNC mve_vec_fn
    vadd.i32 q0, q1, q2
    bx lr
END mve_vec_fn

FUNC mve_sp_fn
    vstrw.32 q0, [sp, #-16]!
    bx lr
END mve_sp_fn
"""

# --- the C half: what the compiler's own records say ------------------------
C_SOURCES = {
    "c_chain.c": r"""
__attribute__((noinline, noclone)) int c_leaf(int x)
{ volatile int a[4]; a[x & 3] = x; return a[1]; }
__attribute__((noinline, noclone)) int c_mid(int x)
{ volatile int b[6]; b[x & 3] = x; return c_leaf(b[2]) + 1; }
int c_root(int x) { volatile int c[2]; c[x & 1] = x; return c_mid(c[0]) + 2; }
""",
    "varargs.c": r"""
#include <stdarg.h>
__attribute__((noinline, noclone)) int vf(int n, ...)
{ va_list ap; int s = 0; va_start(ap, n); while (n-- > 0) s += va_arg(ap, int);
  va_end(ap); return s; }
int va_root(int x) { return vf(3, x, x + 1, x + 2) + 1; }
""",
    "indirect.c": r"""
int (*volatile ind_fp)(int);
int ind_root(int x) { return ind_fp(x) + 1; }
""",
    "recur.c": r"""
volatile int recur_sink;
__attribute__((noinline, noclone)) int recur(int n)
{ if (!n) return 0; recur_sink = n; return recur_sink + recur(n - 1); }
int recur_root(int x) { return recur(x) + 1; }
""",
    "alloca.c": r"""
__attribute__((noinline, noclone)) int dyn(int n)
{ volatile char *p = __builtin_alloca(n); p[0] = 1; return p[0]; }
int alloca_root(int x) { return dyn(x) + 1; }
""",
    # Two statics named helper1 in two files, deep one first in the link, and
    # two named helper2 with the SHALLOW one first -- so that a walk or a
    # witness lookup by name picks the wrong body in one pair or the other,
    # whichever of the two it happens to take.
    "tu_a1.c": r"""
static __attribute__((noinline, noclone)) int helper1(int x)
{ volatile int big[24]; big[x & 7] = x; return big[3]; }
int use_a1(int x) { return helper1(x) + 1; }
""",
    "tu_b1.c": r"""
static __attribute__((noinline, noclone)) int helper1(int x)
{ volatile int s = x; return s; }
int use_b1(int x) { return helper1(x) + 2; }
""",
    "tu_b2.c": r"""
static __attribute__((noinline, noclone)) int helper2(int x)
{ volatile int s = x; return s; }
int use_b2(int x) { return helper2(x) + 3; }
""",
    "tu_a2.c": r"""
static __attribute__((noinline, noclone)) int helper2(int x)
{ volatile int big[20]; big[x & 7] = x; return big[5]; }
int use_a2(int x) { return helper2(x) + 4; }
""",
    # GCC names this body sr.constprop.0.isra.0 and records it as
    # sr.constprop.isra (measured with the pinned 15.2).
    "clone.c": r"""
static __attribute__((noinline)) int sr(const int *p, int unused)
{ volatile int v[5]; v[*p & 3] = *p; return v[1]; }
int sr_root(int x) { return sr(&x, 0) + sr(&x, 0) + 1; }
""",
    # [!] A LIMIT, RECORDED -- NOT A SHAPE THAT OUGHT TO PASS.  A return is
    # proven here by lr's STATE (was it saved?), never by the IDENTITY of the
    # slot it comes back from, so a body that saves lr and then stores over
    # that slot transfers to an arbitrary address and is still read as a
    # return: only its own frame is charged, and whatever it reaches is
    # charged nothing.  The cases below pin what happens today so that a
    # change to it cannot be silent; slot identity belongs to the shared walk
    # (issue #111).  In C, in the tree, so it has a -fstack-usage record and
    # reaches the walk -- which is exactly why the witness rule is not a
    # defence against this.
    "lr_slot.c": r"""
__attribute__((noinline, used)) int lr_slot_target(void)
{ volatile char b[64]; b[0] = 1; return b[0]; }
__attribute__((naked, used)) void lr_slot_naked(void)
{
    __asm__ volatile("push {r4, lr}\n\t"
                     "movw r0, #:lower16:lr_slot_target\n\t"
                     "movt r0, #:upper16:lr_slot_target\n\t"
                     "str  r0, [sp, #4]\n\t"
                     "pop  {r4, pc}\n\t");
}
__attribute__((naked, used)) void lr_slot_moved(void)
{
    /* lr saved, then sp moved so the pop takes a DIFFERENT word: no store
     * needed to leave the slot behind. */
    __asm__ volatile("push {lr}\n\t"
                     "sub  sp, #4\n\t"
                     "pop  {pc}\n\t");
}
__attribute__((naked, used)) void lr_never_saved(void)
{
    /* The other side of the line, and a real defence: lr is never saved, so
     * the pop is not a return at all. */
    __asm__ volatile("sub  sp, #4\n\t"
                     "movw r0, #:lower16:lr_slot_target\n\t"
                     "movt r0, #:upper16:lr_slot_target\n\t"
                     "str  r0, [sp, #0]\n\t"
                     "pop  {pc}\n\t");
}
""",
}
# in link order
C_ORDER = ["c_chain.c", "varargs.c", "indirect.c", "recur.c", "alloca.c",
           "tu_a1.c", "tu_b1.c", "tu_b2.c", "tu_a2.c", "clone.c", "lr_slot.c"]

# The same shape in hand-written assembler, in an object of the tree's own --
# NOT in the prebuilt archive.  It has no -fstack-usage record, which is what
# stops it: the witness rule refuses a compiled body below a veneer whose
# record cannot be named.  That is a real defence, and it is the one this pins.
INTREE_ASM = r"""
    .syntax unified
    .thumb
    .section .text.lr_slot_asm,"ax",%progbits
    .global lr_slot_asm
    .type lr_slot_asm, %function
    .thumb_func
lr_slot_asm:
    push {r4, lr}
    movw r0, #:lower16:lr_slot_target
    movt r0, #:upper16:lr_slot_target
    str  r0, [sp, #4]
    pop  {r4, pc}
    .size lr_slot_asm, . - lr_slot_asm
"""
# An archive the build made itself, NOT under the prebuilt root.
TREE_SOURCE = r"""
int tree_fn(int x) { volatile int t[3]; t[x & 1] = x; return t[0]; }
"""
LTO_SOURCES = {
    "lto_a.c": r"""
extern int lto_mid(int);
__attribute__((noinline, noclone, used)) int lto_root(int x)
{ volatile int a[3]; a[x & 1] = x; return lto_mid(a[1]) + 1; }
""",
    "lto_b.c": r"""
__attribute__((noinline, noclone, used)) int lto_mid(int x)
{ volatile int b[9]; b[x & 7] = x; return b[4]; }
__attribute__((noinline, noclone, used)) int lto_leaf(void) { return 0; }
""",
}


class Tools:
    def __init__(self, cc):
        self.cc = cc
        base = cc[:-len("gcc")]
        self.ar = base + "ar"
        self.objdump = base + "objdump"
        self.objcopy = base + "objcopy"


def sh(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("command failed: " + " ".join(cmd) + "\n"
                           + r.stdout + r.stderr)
    return r.stdout


def build_main(tools, arch, work):
    """The non-LTO image: C objects with their records, the assembler shapes
    in a prebuilt archive, and one in-tree archive."""
    flags = ARCHES[arch]
    os.makedirs(os.path.join(work, "obj"))
    os.makedirs(os.path.join(work, "prebuilt"))
    os.makedirs(os.path.join(work, "prebuilt_tree"))
    with open(os.path.join(work, "fix.ld"), "w") as fh:
        fh.write(LINKER_SCRIPT)
    asm = ASM_COMMON + (ASM_M55 if arch == "m55" else "")
    with open(os.path.join(work, "fix.S"), "w") as fh:
        fh.write(asm)
    sh([tools.cc] + flags + ["-c", "fix.S", "-o", "fix.o"], work)
    sh([tools.ar, "rcs", "prebuilt/libfix.a", "fix.o"], work)
    objs = []
    for name in C_ORDER:
        with open(os.path.join(work, name), "w") as fh:
            fh.write(C_SOURCES[name])
        obj = f"obj/{name}.obj"
        sh([tools.cc] + flags + CFLAGS + ["-c", name, "-o", obj], work)
        objs.append(obj)
    with open(os.path.join(work, "intree.S"), "w") as fh:
        fh.write(INTREE_ASM)
    sh([tools.cc] + flags + ["-c", "intree.S", "-o", "obj/intree.obj"], work)
    objs.append("obj/intree.obj")
    with open(os.path.join(work, "tree.c"), "w") as fh:
        fh.write(TREE_SOURCE)
    # [!] prebuilt_tree/, NOT lib/: a sibling whose name begins with the
    # prebuilt root's, so that a prefix test missing its separator accepts it.
    sh([tools.cc] + flags + CFLAGS + ["-c", "tree.c", "-o",
                                      "prebuilt_tree/tree.o"], work)
    sh([tools.ar, "rcs", "prebuilt_tree/libtree.a", "prebuilt_tree/tree.o"],
       work)
    sh([tools.cc] + flags + ["-nostdlib", "-nostartfiles", "-T", "fix.ld",
                             "-Wl,-Map=fix.map", "-Wl,--no-warn-rwx-segments",
                             "-Wl,-e,leaf"] + objs
       + ["-Wl,--whole-archive", "prebuilt/libfix.a",
          "prebuilt_tree/libtree.a",
          "-Wl,--no-whole-archive", "-o", "fix.elf"], work)
    return {"elf": "fix.elf", "map": "fix.map", "ltrans": []}


def build_lto(tools, arch, work):
    flags = ARCHES[arch]
    with open(os.path.join(work, "fix.ld"), "w") as fh:
        fh.write(LINKER_SCRIPT)
    objs = []
    for name in sorted(LTO_SOURCES):
        with open(os.path.join(work, name), "w") as fh:
            fh.write(LTO_SOURCES[name])
        obj = name + ".obj"
        sh([tools.cc] + flags + CFLAGS + ["-flto", "-c", name, "-o", obj],
           work)
        objs.append(obj)
    sh([tools.cc] + flags + ["-Os", "-flto", "-flto-partition=max",
                             "-fstack-usage", "-nostdlib", "-nostartfiles",
                             "-T", "fix.ld", "-Wl,-Map=lto.map",
                             "-Wl,--no-warn-rwx-segments", "-Wl,-e,lto_leaf"]
       + objs + ["-o", "lto.elf"], work)
    ltrans = sorted(f for f in os.listdir(work)
                    if re.search(r"\.ltrans\d+\.ltrans\.su$", f))
    if not ltrans:
        raise RuntimeError("the LTO link wrote no ltrans records")
    return {"elf": "lto.elf", "map": "lto.map", "ltrans": ltrans}


# --- independent expectations ------------------------------------------------
def su_frame(path, name):
    for line in open(path):
        m = re.match(r"^.*:\d+:\d+:(.+)\t(\d+)\t(\S+)$", line.rstrip("\n"))
        if m and m.group(1) == name:
            return int(m.group(2))
    raise KeyError(f"{name} not in {path}")


def ltrans_frame(work, ltrans, name):
    for f in ltrans:
        try:
            return su_frame(os.path.join(work, f), name)
        except KeyError:
            continue
    raise KeyError(name)


def objdump_frame(tools, work, elf, name):
    """The prologue's stack use as objdump renders it: an oracle that shares
    nothing with the decoder under test."""
    out = subprocess.run([tools.objdump, "-d", "--no-show-raw-insn",
                          os.path.join(work, elf)],
                         capture_output=True, text=True).stdout
    total, inside = 0, False
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            inside = m.group(1) == name
            continue
        if not inside or "\t" not in line:
            continue
        ins = re.sub(r"\s*@.*$", "", line.split("\t", 1)[1]).strip()
        m = re.match(r"^push(?:\.w)?\s+\{([^}]*)\}", ins) or \
            re.match(r"^stmdb(?:\.w)?\s+sp!,\s*\{([^}]*)\}", ins)
        if m:
            n = 0
            for part in m.group(1).split(","):
                r = re.match(r"\s*r(\d+)-r(\d+)", part)
                n += int(r.group(2)) - int(r.group(1)) + 1 if r else 1
            total += 4 * n
            continue
        m = re.match(r"^sub(?:\.w|w)?\s+sp,\s*(?:sp,\s*)?#(\d+)", ins)
        if m:
            total += int(m.group(1))
    return total


# --- running the gate --------------------------------------------------------
def run_gate(gate, work, image, roots, declared, extra=()):
    args = [sys.executable, gate, os.path.join(work, image["elf"]),
            "--map", os.path.join(work, image["map"]),
            "--declared", str(declared),
            # [!] THE REAL ROOT IS THE MIDDLE OF THREE, so that a check that
            # consults only the first or the last root refuses every
            # assembler fixture.
            "--prebuilt-root", os.path.join(work, "no_such_root_a"),
            "--prebuilt-root", os.path.join(work, "prebuilt"),
            "--prebuilt-root", os.path.join(work, "no_such_root_b")]
    for v in VENEERS:
        if v in roots and roots[v] is None:
            continue
        args.append(f"--root={v}={roots.get(v, roots.get('*', 'leaf'))}")
    for v, f in roots.items():
        if v not in VENEERS and v != "*":
            args.append(f"--root={v}={f}")
    for f in image["ltrans"]:
        args += ["--ltrans-su", os.path.join(work, f)]
    args += list(extra)
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def row_of(out, veneer):
    """(lower-bound mark, bytes) of one row of the per-veneer table."""
    m = re.search(r"^\s+%s\s+\S+\s+(>=)?\s*(\d+) B" % re.escape(veneer), out,
                  re.M)
    return (m.group(1) == ">=", int(m.group(2))) if m else (None, None)


def derived_of(out, veneer):
    return row_of(out, veneer)[1]


def headline_of(out):
    """(derived, deepest veneer) from an accept's first lines."""
    m = re.search(r"^check_veneer_base_cost: OK -- VENEER_BASE_COST \d+ B >= "
                  r"(\d+) B derived", out, re.M)
    d = re.search(r"^\s+deepest: (\S+):", out, re.M)
    return (int(m.group(1)) if m else None, d.group(1) if d else None)


def errors_of(out):
    return re.findall(r"^\s+- \[(\w+)\] (.*)$", out, re.M)


# --- .su edits: witnesses that disagree with the image ----------------------
def su_bump(work, path, name, delta):
    p = os.path.join(work, path)
    lines = open(p).read().splitlines()
    hit = False
    for k, line in enumerate(lines):
        m = re.match(r"^(.*:\d+:\d+:)(.+)\t(\d+)\t(\S+)$", line)
        if m and m.group(2) == name:
            lines[k] = f"{m.group(1)}{name}\t{int(m.group(3)) + delta}\t" \
                       f"{m.group(4)}"
            hit = True
    assert hit, f"{name} not in {path}"
    open(p, "w").write("\n".join(lines) + "\n")


def su_add(work, path, record):
    with open(os.path.join(work, path), "a") as fh:
        fh.write(record + "\n")


def su_drop(work, path, name):
    p = os.path.join(work, path)
    lines = open(p).read().splitlines()
    keep = [ln for ln in lines
            if not re.match(r"^.*:\d+:\d+:%s\t" % re.escape(name), ln)]
    assert len(keep) < len(lines), f"{name} not in {path}"
    open(p, "w").write("\n".join(keep) + "\n")


def ltrans_holding(work, image, name):
    for f in image["ltrans"]:
        if re.search(r":%s\t" % re.escape(name), open(os.path.join(work, f))
                     .read()):
            return f
    raise KeyError(name)


# --- the cases ---------------------------------------------------------------
# (name, image, arches, roots, declared, prepare, expect, why)
#   roots:   {veneer: function}; '*' is the default for the rest (leaf); a
#            veneer mapped to None is left out.
#   prepare: f(tools, work, image) -> extra gate arguments, run on a private
#            copy of the built image (it may edit the witnesses).
#   expect:  ("accept", {veneer: bytes})  -- every value exact
#            ("reject", [(stage, substring), ...])  -- exactly these refusals
def exp_c_chain(tools, work, image):
    su = os.path.join(work, "obj", "c_chain.c.su")
    return (su_frame(su, "c_root") + su_frame(su, "c_mid")
            + su_frame(su, "c_leaf"))


def exp_same_name(obj, use, helper):
    def f(tools, work, image):
        su = os.path.join(work, "obj", obj + ".su")
        return su_frame(su, use) + su_frame(su, helper)
    return f


def exp_clone(tools, work, image):
    su = os.path.join(work, "obj", "clone.c.su")
    return su_frame(su, "sr_root") + su_frame(su, "sr.constprop.isra")


def exp_varargs(tools, work, image):
    su = os.path.join(work, "obj", "varargs.c.su")
    by_su = su_frame(su, "va_root") + su_frame(su, "vf")
    by_code = (objdump_frame(tools, work, image["elf"], "va_root")
               + objdump_frame(tools, work, image["elf"], "vf"))
    assert by_code > by_su, (
        f"the fixture no longer shows -fstack-usage under-reporting a variadic "
        f"frame (code {by_code} B, records {by_su} B)")
    return by_code


def exp_lto(tools, work, image):
    return (ltrans_frame(work, image["ltrans"], "lto_root")
            + ltrans_frame(work, image["ltrans"], "lto_mid"))


def p_none(tools, work, image):
    return []


def p_bump(path, name, delta):
    return lambda tools, work, image: (su_bump(work, path, name, delta), [])[1]


def p_add(path, record):
    return lambda tools, work, image: (su_add(work, path, record), [])[1]


def p_drop(path, name):
    return lambda tools, work, image: (su_drop(work, path, name), [])[1]


def p_delete(path):
    return lambda tools, work, image: (os.remove(os.path.join(work, path)),
                                       [])[1]


def p_extra(*args):
    return lambda tools, work, image: list(args)


def p_dynamic_middle(path, name):
    """The body's real record, then a 'dynamic' one and a 'static' one with
    the SAME frame: only the qualifier differs, and it is the middle record."""
    def f(tools, work, image):
        frame = su_frame(os.path.join(work, path), name)
        su_add(work, path, f"x.c:1:1:{name}\t{frame}\tdynamic")
        su_add(work, path, f"x.c:1:1:{name}\t{frame}\tstatic")
        return []
    return f


def p_ltrans_corrupt(name):
    def f(tools, work, image):
        victim = ltrans_holding(work, image, name)
        with open(os.path.join(work, victim), "w") as fh:
            fh.write("this is not a -fstack-usage record line\n")
        return []
    return f


def p_ltrans_delete(name):
    def f(tools, work, image):
        victim = ltrans_holding(work, image, name)
        os.remove(os.path.join(work, victim))
        image["ltrans"] = [x for x in image["ltrans"] if x != victim]
        return []
    return f


def p_ltrans_drop(name):
    def f(tools, work, image):
        su_drop(work, ltrans_holding(work, image, name), name)
        return []
    return f


def p_ltrans_stale(tools, work, image):
    src = os.path.join(work, image["ltrans"][0])
    stale = os.path.join(work, "lto.elf.ltrans97.ltrans.su")
    shutil.copy(src, stale)
    image["ltrans"] = image["ltrans"] + [os.path.basename(stale)]
    return []


BOTH = ("m55", "m7")
CASES = [
    # --- exact derivations -------------------------------------------------
    ("chain_exact", "main", BOTH, {"pl_base_log": "root_chain"}, 88, p_none,
     ("accept", {"pl_base_log": 88}),
     "tail call + conditional tail call, summed to the byte"),
    ("declared_equal", "main", BOTH, {"pl_base_log": "root_chain"}, 88,
     p_none, ("accept", {"pl_base_log": 88}),
     "declared == derived passes"),
    ("declared_minus_one", "main", BOTH, {"pl_base_log": "root_chain"}, 87,
     p_none, ("reject", [("budget", "pl_base_log needs 88 B below it "
                                    "(root_chain 8 > ch_a 24 > ch_b 16 > "
                                    "ch_d 40)")]),
     "declared == derived - 1 fails, naming the chain"),
    ("it_return", "main", BOTH, {"pl_paint_rect": "it_ret"}, 64, p_none,
     ("accept", {"pl_paint_rect": 16}),
     "a conditional return inside an IT block"),
    ("fp_vocabulary", "main", BOTH, {"pl_paint_blit": "fp_vocab"}, 2048,
     p_none, ("accept", {"pl_paint_blit": 1060}),
     "strd/ldrd writeback, vpush/vpop, sub.w/subw/addw sp"),
    ("veneer_deep", "main", BOTH, {"pl_base_to_frame": "vcall"}, 128, p_none,
     ("accept", {"pl_base_to_frame": 72}),
     "a linker long-branch veneer is an edge, read out of its literal"),
    ("veneer_second_data_region", "main", BOTH,
     {"pl_base_to_frame": "vlike"}, 128, p_none,
     ("accept", {"pl_base_to_frame": 64}),
     "the veneer's literal is found in whichever of its data regions holds "
     "it"),
    ("veneer_literal_even", "main", BOTH, {"pl_base_to_frame": "ven_even"},
     1024, p_none,
     ("reject", [("walk", "whose literal is not a Thumb address")]),
     "an even veneer literal (the Thumb bit clear) is refused"),
    ("veneer_literal_outside_body", "main", BOTH,
     {"pl_base_to_frame": "ven_out"}, 1024, p_none,
     ("reject", [("walk", "outside a linker long-branch veneer")]),
     "an ldr pc whose literal is in another blob is not a veneer"),
    ("veneer_literal_not_a_node", "main", BOTH,
     {"pl_base_to_frame": "ven_nonode"}, 1024, p_none,
     ("reject", [("walk", "which is no function's entry")]),
     "a veneer literal that is no function's entry"),
    ("m55_vocabulary", "main", ("m55",), {"pl_paint_fill_rect": "m55_vocab"},
     64, p_none, ("accept", {"pl_paint_fill_rect": 8}),
     "CSEL, MVE scalar shifts, WLS/LE and MVE load/store move no sp"),
    ("c_chain", "main", BOTH, {"pl_print_write": "c_root"}, 1024, p_none,
     ("accept", {"pl_print_write": exp_c_chain}),
     "compiled chain, equal to the compiler's own records"),
    ("varargs", "main", BOTH, {"pl_base_log": "va_root"}, 1024, p_none,
     ("accept", {"pl_base_log": exp_varargs}),
     "the variadic save area -fstack-usage leaves out is counted"),
    ("same_name_deep_first", "main", BOTH, {"pl_base_log": "use_a1"}, 1024,
     p_none, ("accept", {"pl_base_log": exp_same_name("tu_a1.c", "use_a1",
                                                      "helper1")}),
     "two statics called helper1, the deep one linked first"),
    ("same_name_deep_last", "main", BOTH, {"pl_base_log": "use_a2"}, 1024,
     p_none, ("accept", {"pl_base_log": exp_same_name("tu_a2.c", "use_a2",
                                                      "helper2")}),
     "two statics called helper2, the deep one linked last"),
    ("lto_chain", "lto", BOTH, {"*": "lto_leaf", "pl_base_log": "lto_root"},
     1024, p_none, ("accept", {"pl_base_log": exp_lto}),
     "an LTO image, each body checked against its own partition's record"),
    ("sibling_middle", "main", BOTH, {"pl_paint_blit": "sib_root"}, 128,
     p_none, ("accept", {"pl_paint_blit": 128}),
     "the deepest of three callees is neither the first nor the last"),
    ("sibling_branchy", "main", BOTH, {"pl_paint_blit": "sib_branchy"}, 128,
     p_none, ("accept", {"pl_paint_blit": 128}),
     "...nor when the walk meets them in another order than the addresses"),
    ("deepest_in_middle", "main", BOTH,
     {"pl_base_log": "root_chain", "pl_base_to_frame": "it_ret",
      "pl_paint_blit": "vcall", "pl_paint_fill_rect": "sib_root",
      "pl_paint_rect": "ch_d", "pl_print_write": "leaf"}, 200, p_none,
     ("accept", {"pl_base_log": 88, "pl_base_to_frame": 16,
                 "pl_paint_blit": 72, "pl_paint_fill_rect": 128,
                 "pl_paint_rect": 40, "pl_print_write": 0,
                 "headline": (128, "pl_paint_fill_rect")}),
     "the deepest ROOT is the middle one: the headline says which"),
    ("size0_next_function", "main", BOTH, {"pl_paint_rect": "sz0_a"}, 64,
     p_none, ("accept", {"pl_paint_rect": 8}),
     "no .size: the body ends at the next function"),
    ("size0_input_end", "main", BOTH, {"pl_paint_rect": "sz0_c"}, 64,
     p_none, ("accept", {"pl_paint_rect": 8}),
     "no .size: the body ends at the end of its input section"),
    ("clone_name", "main", BOTH, {"pl_print_write": "sr_root"}, 1024,
     p_none, ("accept", {"pl_print_write": exp_clone}),
     "sr.constprop.0.isra.0 is checked against sr.constprop.isra"),
    # --- the walk refuses ----------------------------------------------------
    ("indirect_call", "main", BOTH, {"pl_base_log": "ind_root"}, 1024, p_none,
     ("reject", [("walk", "indirect call `blx")]),
     "a call through a pointer"),
    ("tbb", "main", BOTH, {"pl_base_log": "tbb_fn"}, 1024, p_none,
     ("reject", [("walk", "indirect branch `tbb`")]),
     "a table branch"),
    ("recursion", "main", BOTH, {"pl_base_log": "recur_root"}, 1024, p_none,
     ("reject", [("walk", "recursion: recur -> recur")]),
     "recursion"),
    ("alloca", "main", BOTH, {"pl_base_log": "alloca_root"}, 1024, p_none,
     ("reject", [("walk", "dyn: register arithmetic into sp"),
                 ("walk", "dyn: mov sp, r7"),
                 ("witness", "records dyn as 'dynamic'")]),
     "alloca: both non-constant sp writes, and a record that says so"),
    ("sp_register", "main", BOTH, {"pl_base_log": "sp_reg_fn"}, 1024, p_none,
     ("reject", [("walk", "register arithmetic into sp at")]),
     "an sp write outside the vocabulary"),
    ("sp_in_loop", "main", BOTH, {"pl_base_log": "loop_fn"}, 1024, p_none,
     ("reject", [("walk", "is inside a loop")]),
     "a decrement on a backward branch's cycle"),
    ("sp_in_le_loop", "main", ("m55",), {"pl_base_log": "le_loop_fn"}, 1024,
     p_none, ("reject", [("walk", "is inside a loop")]),
     "a decrement inside a DLS/LE low-overhead loop"),
    ("branch_interior", "main", BOTH, {"pl_base_log": "interior_fn"}, 1024,
     p_none, ("reject", [("walk", "into the interior of other_fn")]),
     "a branch into another function's middle"),
    ("branch_unresolved", "main", BOTH, {"pl_base_log": "stray_fn"}, 1024,
     p_none, ("reject", [("walk", "which is no function's entry")]),
     "a branch to code no function symbol covers"),
    ("undecoded", "main", BOTH, {"pl_base_log": "undec_fn"}, 1024, p_none,
     ("reject", [("walk", "cannot decode ee000000")]),
     "an encoding the decoder does not know"),
    ("mve_vector", "main", ("m55",), {"pl_base_log": "mve_vec_fn"}, 1024,
     p_none, ("reject", [("walk", "vector instruction")]),
     "MVE vector arithmetic is not assumed harmless"),
    ("mve_sp_writeback", "main", ("m55",), {"pl_base_log": "mve_sp_fn"},
     1024, p_none, ("reject", [("walk", "UNPREDICTABLE")]),
     "an MVE store writing back sp is UNPREDICTABLE, not a constant"),
    ("fall_off_end", "main", BOTH, {"pl_base_log": "falloff_fn"}, 1024,
     p_none, ("reject", [("walk", "falls off the end of the body")]),
     "a body whose last instruction falls through"),
    ("unproven_bx_lr", "main", BOTH, {"pl_base_log": "badret_fn"}, 1024,
     p_none, ("reject", [("walk", "`bx lr` at")]),
     "bx lr after lr was overwritten"),
    ("unproven_pop_pc", "main", BOTH, {"pl_base_log": "badpop_fn"}, 1024,
     p_none, ("reject", [("walk", "lr was not saved on every path")]),
     "pop into pc with nothing saved"),
    ("msr_stack_pointer", "main", BOTH, {"pl_base_log": "msr_sp_fn"}, 1024,
     p_none, ("reject", [("walk", "msr to a stack pointer")]),
     "an msr to a stack pointer"),
    ("load_into_sp", "main", BOTH, {"pl_base_log": "ldr_sp_fn"}, 1024,
     p_none, ("reject", [("walk", "load into sp")]),
     "a load into sp"),
    ("bx_register", "main", BOTH, {"pl_base_log": "bx_reg_fn"}, 1024, p_none,
     ("reject", [("walk", "indirect branch `bx r3`")]),
     "bx to a register other than lr"),
    ("load_into_pc", "main", BOTH, {"pl_base_log": "ldr_pc_fn"}, 1024,
     p_none, ("reject", [("walk", "indirect branch `load into pc`")]),
     "a load into pc from anywhere but the stack"),
    ("mov_into_pc", "main", BOTH, {"pl_base_log": "mov_pc_fn"}, 1024, p_none,
     ("reject", [("walk", "indirect branch `mov pc, r3`")]),
     "a move into pc"),
    ("literal_pc_not_veneer", "main", BOTH, {"pl_base_log": "litpc_fn"},
     1024, p_none,
     ("reject", [("walk", "outside a linker long-branch veneer")]),
     "ldr pc, [pc, #imm] inside an ordinary body"),
    ("sp_in_loop_middle", "main", BOTH, {"pl_base_log": "loop_mid_fn"},
     1024, p_none, ("reject", [("walk", "is inside a loop")]),
     "the stack change is in the middle of its loop"),
    ("lr_clobbered_on_one_path", "main", BOTH, {"pl_base_log": "lr_join_fn"},
     1024, p_none, ("reject", [("walk", "`bx lr` at")]),
     "bx lr where only one of two incoming paths overwrote lr"),
    ("lr_saved_on_one_path", "main", BOTH,
     {"pl_base_log": "saved_join_fn"}, 1024, p_none,
     ("reject", [("walk", "lr was not saved on every path")]),
     "pop into pc where only one of two incoming paths saved lr"),
    ("extent_overlap", "main", BOTH, {"pl_base_log": "ovl_a"}, 1024, p_none,
     ("reject", [("walk", "its extent overlaps ovl_b"),
                 ("witness", "its extent runs past its input section")]),
     "one symbol's .size runs over the next (and so past its input section)"),
    ("alias_size_conflict", "main", BOTH, {"pl_base_log": "alias_a"}, 1024,
     p_none, ("reject", [("walk", "its symbols disagree about its size")]),
     "two symbols for one body with two sizes"),
    ("incomplete_below", "main", BOTH, {"pl_paint_blit": "inc_root"}, 40,
     p_none,
     ("reject", [("walk", "inc_mid: indirect call `blx r3`"),
                 ("budget", "pl_paint_blit needs at least 48 B below it")],
      {"pl_paint_blit": (True, 48), "pl_paint_rect": (False, 0)}),
     "a refusal BELOW the root makes the root's number a lower bound"),
    ("incomplete_not_last", "main", BOTH, {"pl_paint_blit": "inc_first"},
     40, p_none,
     ("reject", [("walk", "inc_mid: indirect call `blx r3`"),
                 ("budget", "pl_paint_blit needs at least 48 B below it")],
      {"pl_paint_blit": (True, 48)}),
     "...when the incomplete callee is not the last one"),
    ("incomplete_self", "main", BOTH, {"pl_paint_blit": "inc_self"}, 8,
     p_none,
     ("reject", [("walk", "inc_self: indirect call `blx r3`"),
                 ("budget", "pl_paint_blit needs at least 16 B below it")],
      {"pl_paint_blit": (True, 16)}),
     "...and when the root itself is the incomplete one"),
    ("budget_two_roots", "main", BOTH,
     {"pl_base_log": "root_chain", "pl_base_to_frame": "it_ret",
      "pl_paint_blit": "vcall", "pl_paint_fill_rect": "sib_root",
      "pl_paint_rect": "ch_d", "pl_print_write": "leaf"}, 80, p_none,
     ("reject", [("budget", "pl_base_log needs 88 B below it"),
                 ("budget", "pl_paint_fill_rect needs 128 B below it")],
      {"pl_paint_blit": (False, 72)}),
     "every root over the declaration is named, not only the deepest"),
    ("truncated_instruction", "main", BOTH, {"pl_base_log": "trunc_fn"},
     1024, p_none,
     ("reject", [("walk", "runs past its code region"),
                 ("walk", "falls through into data")]),
     "a 32-bit instruction whose second halfword is past the body"),
    ("trap_not_call", "main", BOTH, {"pl_base_log": "udf_fn"}, 1024,
     p_none, ("reject", [("walk", "udf at 0x")]),
     "a trap instruction is not a call (bkpt/svc take the same branch)"),
    ("fall_into_data", "main", BOTH, {"pl_base_log": "data_fall"}, 1024,
     p_none, ("reject", [("walk", "falls through into data")]),
     "a body that runs off its code into its own trailing data"),
    ("branch_into_it", "main", BOTH, {"pl_base_log": "into_it"}, 1024,
     p_none, ("reject", [("walk", "into an IT block")]),
     "a branch whose target is inside an IT block"),
    ("it_return_then_indirect", "main", BOTH, {"pl_base_log": "it_ind"}, 1024,
     p_none, ("reject", [("walk", "indirect call `blx r3`")]),
     "what follows a conditional return is still walked"),
    # --- witnesses -----------------------------------------------------------
    ("witness_mismatch", "main", BOTH, {"pl_print_write": "c_root"}, 1024,
     p_bump("obj/c_chain.c.su", "c_mid", 100),
     ("reject", [("witness", "c_mid: the scan found")]),
     "a record larger than the scan: the decoder missed something"),
    ("witness_mismatch_outside", "main", BOTH, {}, 1024,
     p_bump("obj/tu_b2.c.su", "use_b2", 64),
     ("reject", [("witness", "use_b2: the scan found")]),
     "the same check on a body no veneer reaches (image-wide)"),
    ("witness_same_name_bumped", "main", BOTH, {"pl_base_log": "use_a1"},
     1024, p_bump("obj/tu_a1.c.su", "helper1", 100),
     ("reject", [("witness", "tu_a1.c.su) says")]),
     "the deep helper1's own record, not the other file's"),
    ("witness_dynamic_twin", "main", BOTH, {"pl_print_write": "c_root"}, 1024,
     p_add("obj/c_chain.c.su", "c_chain.c:9:9:c_mid\t8\tdynamic"),
     ("reject", [("witness", "records c_mid as 'dynamic'")]),
     "a second record of the same name with no static bound"),
    ("witness_dynamic_outside", "main", BOTH, {}, 1024,
     p_add("obj/tu_b1.c.su", "tu_b1.c:4:5:use_b1\t16\tdynamic"),
     ("reject", [("witness", "but the scan found a constant frame")]),
     "a 'dynamic' record for a body the scan found constant, anywhere"),
    ("witness_ambiguous", "main", BOTH, {"pl_print_write": "c_root"}, 1024,
     p_add("obj/c_chain.c.su", "c_chain.c:9:9:c_mid\t1\tstatic"),
     ("reject", [("witness", "different records named like c_mid")]),
     "two static records of one name that disagree"),
    ("witness_dynamic_middle", "main", BOTH, {"pl_print_write": "c_root"},
     1024, p_dynamic_middle("obj/c_chain.c.su", "c_mid"),
     ("reject", [("witness", "records c_mid as 'dynamic'")]),
     "three equal records, the middle one 'dynamic'"),
    ("witness_spelling_disagrees", "main", BOTH,
     {"pl_print_write": "sr_root"}, 1024,
     p_add("obj/clone.c.su", "clone.c:1:1:sr.constprop.0.isra.0\t999\tstatic"),
     ("reject", [("witness", "different records named like "
                             "sr.constprop.0.isra.0")]),
     "two spellings of one body with records that disagree"),
    ("witness_file_missing", "main", BOTH, {"pl_print_write": "c_root"}, 1024,
     p_delete("obj/c_chain.c.su"),
     ("reject", [("witness", "c_root: no record file"),
                 ("witness", "c_mid: no record file"),
                 ("witness", "c_leaf: no record file")]),
     "a compiled body below a veneer with no record file"),
    ("witness_record_missing", "main", BOTH, {"pl_print_write": "c_root"},
     1024, p_drop("obj/c_chain.c.su", "c_leaf"),
     ("reject", [("witness", "no record for c_leaf")]),
     "a compiled body whose file has no line for it"),
    ("in_tree_archive", "main", BOTH, {"pl_base_log": "tree_fn"}, 1024,
     p_none, ("reject", [("witness", "is not under a --prebuilt-root")]),
     "an archive this build made is not a vendor's"),
    # --- the lr slot: a limit, pinned so that changing it cannot be silent --
    # [!] THE SECOND OF THESE RECORDS SOMETHING UNDESIRABLE.  A return is
    # proven by lr's STATE, not by the IDENTITY of the slot it is reloaded
    # from, so a body that saves lr and then stores over that slot is read as
    # a return: the walk charges ITS frame (8 B) and charges NOTHING for the
    # address it actually transfers to -- lr_slot_target, whose own frame is
    # 64 B and never appears.  Closing that needs slot identity, which belongs
    # to the shared walk (issue #111).  Until then: if somebody adds slot
    # identity, this case turns into a refusal and MUST be rewritten as one --
    # that is what it is here to force.  If instead somebody widens what
    # counts as a return, the number moves and the case fails too.
    ("lr_slot_asm_in_tree", "main", BOTH, {"pl_base_log": "lr_slot_asm"},
     1024, p_none, ("reject", [("witness", "no record file")]),
     "the same shape in the tree's own assembler is stopped -- by the "
     "WITNESS rule, not by the walk: it has no -fstack-usage record"),
    ("lr_slot_naked_c", "main", BOTH, {"pl_base_log": "lr_slot_naked"}, 1024,
     p_none, ("accept", {"pl_base_log": 8}),
     "[!] KNOWN LIMIT, NOT DESIRED BEHAVIOUR: storing over the saved lr slot "
     "passes as a return, charging the frame alone and nothing for the "
     "64 B body it reaches"),
    ("lr_slot_moved_sp", "main", BOTH, {"pl_base_log": "lr_slot_moved"}, 1024,
     p_none, ("accept", {"pl_base_log": 8}),
     "[!] KNOWN LIMIT: and no store is needed -- moving sp after saving lr "
     "pops a different word, which is the same missing slot identity"),
    ("lr_never_saved", "main", BOTH, {"pl_base_log": "lr_never_saved"}, 1024,
     p_none,
     ("reject", [("walk", "is not proven to be a return: lr was not saved "
                          "on every path")]),
     "the line that IS held: with lr never saved, `pop {..., pc}` is refused "
     "-- so the two accepts above turn on lr's state, not on the slot"),
    ("ltrans_partition_missing", "lto", BOTH,
     {"*": "lto_leaf", "pl_base_log": "lto_root"}, 1024,
     p_ltrans_delete("lto_mid"),
     ("reject", [("witness", "has no record file")]),
     "one LTO partition's record removed"),
    ("ltrans_record_missing", "lto", BOTH,
     {"*": "lto_leaf", "pl_base_log": "lto_root"}, 1024,
     p_ltrans_drop("lto_mid"),
     ("reject", [("witness", "no record for lto_mid")]),
     "a body missing from its partition's record"),
    ("ltrans_corrupt", "lto", BOTH,
     {"*": "lto_leaf", "pl_base_log": "lto_root"}, 1024,
     p_ltrans_corrupt("lto_mid"),
     ("reject", [("input", "not a -fstack-usage record")]),
     "a partition record with a malformed line"),
    ("ltrans_stale", "lto", BOTH, {"*": "lto_leaf"}, 1024, p_ltrans_stale,
     ("reject", [("witness", "which this image's map does not contain")]),
     "a partition record left by an earlier link"),
    # --- roots and budget ----------------------------------------------------
    ("root_missing", "main", BOTH, {"pl_print_write": None}, 1024, p_none,
     ("reject", [("roots", "declared behind veneer pl_print_write")]),
     "a veneer with no root"),
    ("root_extra", "main", BOTH, {"pl_not_a_veneer": "leaf"}, 1024, p_none,
     ("reject", [("roots", "pl_not_a_veneer is not a veneer")]),
     "a root for something that is not a veneer"),
    ("root_ambiguous", "main", BOTH, {"pl_base_log": "helper1"}, 1024, p_none,
     ("reject", [("roots", "helper1 names 2 functions")]),
     "a root that names two bodies"),
    ("root_no_equals", "main", BOTH, {}, 1024,
     p_extra("--root=just_a_veneer_name"),
     ("reject", [("roots", "is not VENEER=FUNCTION")]),
     "a --root with no =FUNCTION"),
    ("root_duplicate", "main", BOTH, {}, 1024,
     p_extra("--root=pl_base_log=root_chain"),
     ("reject", [("roots", "veneer pl_base_log is declared twice")]),
     "one veneer declared twice"),
    ("root_unresolved", "main", BOTH, {"pl_base_log": "no_such_fn"}, 1024,
     p_none, ("reject", [("roots", "no_such_fn is not a function")]),
     "a root that names none"),
    ("printer_limit", "main", BOTH, {}, 256, p_extra("--printer-limit",
                                                     "fixture=300"),
     ("reject", [("budget", "bounded at 300 B, above VENEER_BASE_COST 256")]),
     "a plugin printer bound above the declared cost"),
    ("printer_limit_several", "main", BOTH, {}, 256,
     p_extra("--printer-limit", "a=10", "--printer-limit", "b=300",
             "--printer-limit", "c=400", "--printer-limit", "d=20"),
     ("reject", [("budget", "b: its own printer is bounded at 300 B"),
                 ("budget", "c: its own printer is bounded at 400 B")]),
     "every printer bound is checked: the two too large ones are in the "
     "middle"),
    ("printer_limit_malformed", "main", BOTH, {}, 256,
     p_extra("--printer-limit", "no_equals_sign"),
     ("reject", [("budget", "is not WHO=BYTES")]),
     "a --printer-limit with no bytes is rejected"),
    ("printer_limit_equal", "main", BOTH, {}, 256,
     p_extra("--printer-limit", "fixture=256"), ("accept", {}),
     "a plugin printer bound equal to the declared cost"),
]


def check_case(tools, gate, arch, base_work, image_proto, case):
    name, _img, _arches, roots, declared, prepare, expect, _why = case
    with tempfile.TemporaryDirectory() as work:
        shutil.rmtree(work)
        shutil.copytree(base_work, work)
        image = dict(image_proto, ltrans=list(image_proto["ltrans"]))
        extra = prepare(tools, work, image)
        rc, out = run_gate(gate, work, image, roots, declared, extra)
        errs = errors_of(out)
        if expect[0] == "accept":
            if rc != 0:
                return f"expected accept, got rc {rc}:\n{out}"
            for v in VENEERS:
                mark, _n = row_of(out, v)
                if mark:
                    return (f"{v} is marked as a lower bound in an "
                            f"accept:\n{out}")
            for v, want in expect[1].items():
                if v == "headline":
                    got = headline_of(out)
                    if got != want:
                        return f"headline {got}, expected {want}:\n{out}"
                    continue
                if callable(want):
                    want = want(tools, work, image)
                got = derived_of(out, v)
                if got != want:
                    return f"{v}: derived {got}, expected {want}:\n{out}"
            return None
        if rc == 0:
            return f"expected a refusal, got an accept:\n{out}"
        if rc != 1:
            return f"expected rc 1 (refused), got {rc}:\n{out}"
        want = expect[1]
        for stage, text in want:
            if not any(s == stage and text in m for s, m in errs):
                return (f"no [{stage}] refusal saying {text!r}; the gate "
                        f"said:\n{out}")
        if len(errs) != len(want):
            return (f"{len(errs)} refusals where exactly {len(want)} were "
                    f"expected -- it failed for another reason too:\n{out}")
        # A refusal may also pin rows of the table: (mark, bytes) per veneer,
        # the mark being whether the row is reported as a lower bound.
        for v, (mark, n) in (expect[2] if len(expect) > 2 else {}).items():
            got = row_of(out, v)
            if got != (mark, n):
                return f"{v}: row {got}, expected {(mark, n)}:\n{out}"
        return None


def empty_abi_case(gate, work, image):
    """The ABI's veneer set is imported, so an empty one can only be made by
    patching the import -- which is what this does, in-process, on a real
    image with no roots at all."""
    import contextlib
    import io
    mod = _load_gate(gate, "gate_under_test")
    saved = mod.check_plugin_image.VENEERS
    mod.check_plugin_image.VENEERS = set()
    buf = io.StringIO()
    try:
        with contextlib.redirect_stderr(buf), contextlib.redirect_stdout(buf):
            rc = mod.main([os.path.join(work, image["elf"]),
                           "--map", os.path.join(work, image["map"]),
                           "--declared", "8",
                           "--prebuilt-root", os.path.join(work, "prebuilt")])
    finally:
        mod.check_plugin_image.VENEERS = saved
    return rc, buf.getvalue()


def _load_gate(gate, name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, gate)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _od_writes_sp(text):
    """Whether objdump's rendering of an instruction writes sp -- the oracle's
    heuristic, deliberately written from the TEXT so that it shares nothing
    with the decoder it checks."""
    mn, _, ops = text.partition("\t")
    mn = re.sub(r"(\.w|\.n)$", "", mn)
    ops = re.sub(r"\s*@.*$", "", ops)
    if re.match(r"^(push|pop|vpush|vpop)", mn):
        return True
    if "sp!" in ops or re.search(r"\[sp, #-?\d+\]!", ops) or \
            re.search(r"\[sp\], #", ops):
        return True
    first = ops.split(",")[0].strip()
    if first == "sp" and not re.match(
            r"^(str|stm|ldm|cmp|cmn|tst|teq|vst|b|bx|blx|cbz|cbnz)", mn):
        return True
    return bool(re.match(r"^msr", mn)
                and re.search(r"\b(msp|psp|control)\b", ops, re.I))


def decoder_vs_objdump(tools, gate, work, image):
    """Every instruction the decoder finds in every function of a fixture
    image, compared with objdump: the same boundaries, the same direct-branch
    targets, and the same answer to 'does it write sp'.  On the two shipped
    firmware images (#112) the same comparison covered ~180,000 instructions
    with no disagreement; this keeps the encodings the fixtures use pinned on
    both cores."""
    mod = _load_gate(gate, "gate_decoder")
    path = os.path.join(work, image["elf"])
    elf = mod.Elf(path)
    img = mod.Image(elf, [])
    od = {}
    out = subprocess.run([tools.objdump, "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\t(.*)$", line)
        if m:
            od[int(m.group(1), 16)] = m.group(2).strip()
    problems, n = [], 0
    for node in img.nodes.values():
        for start, end, kind in img.regions(node):
            if kind != "t":
                continue
            a = start
            while a < end:
                ins = mod.decode(elf, a)
                if ins is None:
                    problems.append(f"0x{a:08x}: no instruction decoded")
                    break
                n += 1
                text = od.get(a)
                if text is None:
                    problems.append(f"0x{a:08x} ({node.name}): objdump has no "
                                    "instruction boundary here")
                elif ins.flow != "undef":
                    if ins.flow in ("b", "bcc", "cbz", "bl", "le", "wls"):
                        m = re.search(r"\b([0-9a-f]+) <", text)
                        if not m or int(m.group(1), 16) != ins.target:
                            problems.append(f"0x{a:08x} {text!r}: target "
                                            f"0x{ins.target:08x}")
                    if ins.lit:
                        m = re.search(r"@ ([0-9a-f]+)", text)
                        if not m or int(m.group(1), 16) != ins.target:
                            problems.append(f"0x{a:08x} {text!r}: literal at "
                                            f"0x{ins.target:08x}")
                    if (mod.SP in ins.wr) != _od_writes_sp(text):
                        problems.append(f"0x{a:08x} {text!r}: writes sp = "
                                        f"{mod.SP in ins.wr}")
                a += ins.size
    if n == 0:
        problems.append("no instructions compared")
    return problems, n


def input_error_cases(gate, work, image):
    """The [input] guards: malformed inputs, each refused for its own reason,
    run on the real built image with one input replaced at a time."""
    elf = os.path.join(work, image["elf"])
    mp = os.path.join(work, image["map"])
    prebuilt = os.path.join(work, "prebuilt")
    base = [sys.executable, gate, elf, "--map", mp, "--declared", "1024",
            "--prebuilt-root", prebuilt] + [f"--root={v}=leaf" for v in VENEERS]
    cases = []
    notelf = os.path.join(work, "notelf.bin")
    open(notelf, "w").write("this is not an ELF\n")
    cases.append(("not_an_elf", [x if x != elf else notelf for x in base],
                  "is not an ELF file"))
    # stripped: iargs None, built with objcopy in main() (it is next to cc)
    cases.append(("stripped", None, "has no symbol table"))
    cases.append(("missing_map",
                  [x if x != mp else os.path.join(work, "nope.map")
                   for x in base],
                  "cannot read map"))
    notmap = os.path.join(work, "notmap.txt")
    open(notmap, "w").write("just some text, no map here\n")
    cases.append(("not_a_map", [x if x != mp else notmap for x in base],
                  "no 'Linker script and memory map'"))
    badname = os.path.join(work, "not_a_partition.su")
    open(badname, "w").write("x.c:1:1:f\t8\tstatic\n")
    cases.append(("ltrans_misnamed", base + ["--ltrans-su", badname],
                  "not named like an LTO partition"))
    a1 = os.path.join(work, "one.elf.ltrans0.ltrans.su")
    a2 = os.path.join(work, "two.elf.ltrans0.ltrans.su")
    open(a1, "w").write("")
    open(a2, "w").write("")
    cases.append(("ltrans_dup", base + ["--ltrans-su", a1, "--ltrans-su", a2],
                  "two records for LTO partition 0"))
    # --declared must be a positive number
    cases.append(("declared_zero",
                  [x if x != "1024" else "0" for x in base],
                  "zero is not a cost"))
    cases.append(("declared_nan",
                  [x if x != "1024" else "not_a_number" for x in base],
                  "not a number"))
    # crafted ELF headers isolating the class/endianness and machine checks
    elf64 = os.path.join(work, "elf64.bin")
    open(elf64, "wb").write(b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 12)
    cases.append(("elf_64bit", [x if x != elf else elf64 for x in base],
                  "not a 32-bit little-endian ELF"))
    elf32x86 = os.path.join(work, "elf32x86.bin")
    hdr = bytearray(b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 12)
    hdr[0x12:0x14] = (3).to_bytes(2, "little")     # EM_386, not EM_ARM (40)
    open(elf32x86, "wb").write(bytes(hdr))
    cases.append(("elf_not_arm", [x if x != elf else elf32x86 for x in base],
                  "is not an ARM ELF"))
    return cases


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--gate", default=GATE,
                    help="the script under test (a mutated copy, to test the "
                         "cases themselves)")
    ap.add_argument("--only", help="run only cases whose name contains this")
    args = ap.parse_args()
    tools = Tools(args.cc)
    gate = os.path.abspath(args.gate)
    print(f"run_veneer_cost_tests: {os.path.basename(args.cc)} -> "
          f"{os.path.relpath(gate, REPO) if gate.startswith(REPO) else gate}")
    bad = 0
    ran = 0
    with tempfile.TemporaryDirectory() as root:
        for arch in ARCHES:
            images = {}
            for kind, build in (("main", build_main), ("lto", build_lto)):
                work = os.path.join(root, f"{arch}-{kind}")
                os.makedirs(work)
                try:
                    images[kind] = (work, build(tools, arch, work))
                except RuntimeError as exc:
                    print(f"  FAIL {arch}:{kind} image did not build:\n{exc}")
                    bad += 1
            for case in CASES:
                name, img, arches = case[0], case[1], case[2]
                if arch not in arches or img not in images:
                    continue
                if args.only and args.only not in name:
                    continue
                ran += 1
                work, image = images[img]
                try:
                    msg = check_case(tools, gate, arch, work, image, case)
                except (AssertionError, KeyError, RuntimeError) as exc:
                    msg = f"the fixture itself broke: {exc}"
                label = f"{arch}:{name}"
                if msg:
                    bad += 1
                    print(f"  FAIL {label:34s} {msg[:2000]}")
                else:
                    print(f"  ok   {label:34s} {case[7]}")
            for kind in ("main", "lto"):
                if kind not in images or (args.only and args.only not in
                                          "decoder_vs_objdump"):
                    continue
                ran += 1
                label = f"{arch}:decoder_vs_objdump_{kind}"
                problems, n = decoder_vs_objdump(tools, gate, *images[kind])
                if problems:
                    bad += 1
                    print(f"  FAIL {label:34s} " + "\n        ".join(
                        problems[:20]))
                else:
                    print(f"  ok   {label:34s} {n} instructions agree with "
                          "objdump")
            if "main" in images and (not args.only
                                     or args.only in "empty_abi"):
                ran += 1
                rc, out = empty_abi_case(gate, *images["main"])
                errs = errors_of(out)
                label = f"{arch}:empty_abi"
                if rc == 1 and errs == [("roots", "the ABI's veneer set is "
                                         "empty -- there is nothing to derive "
                                         "a cost for, which is not the same "
                                         "as a cost of zero")]:
                    print(f"  ok   {label:34s} an empty veneer set is "
                          "refused, not read as a cost of zero")
                else:
                    bad += 1
                    print(f"  FAIL {label:34s} rc {rc}:\n{out}")
            if "main" in images and (not args.only
                                     or args.only in "input_error"):
                iwork, iimg = images["main"]
                for iname, iargs, isay in input_error_cases(gate, iwork,
                                                            iimg):
                    if iargs is None:      # the stripped-ELF case needs objcopy
                        strp = os.path.join(iwork, "stripped.elf")
                        r0 = subprocess.run([tools.objcopy, "--strip-all",
                                             os.path.join(iwork, iimg["elf"]),
                                             strp], capture_output=True)
                        if r0.returncode != 0:
                            continue
                        iargs = [sys.executable, gate, strp, "--map",
                                 os.path.join(iwork, iimg["map"]),
                                 "--declared", "1024", "--prebuilt-root",
                                 os.path.join(iwork, "prebuilt")] + [
                                     f"--root={v}=leaf" for v in VENEERS]
                    ran += 1
                    label = f"{arch}:input_{iname}"
                    r = subprocess.run(iargs, capture_output=True, text=True)
                    out = r.stdout + r.stderr
                    errs = errors_of(out)
                    ok_input = (r.returncode == 1 and len(errs) == 1
                                and errs[0][0] == "input"
                                and isay in errs[0][1])
                    ok_argparse = (r.returncode == 2 and isay in out)
                    if ok_input or ok_argparse:
                        print(f"  ok   {label:34s} refused: {isay}")
                    else:
                        bad += 1
                        print(f"  FAIL {label:34s} rc {r.returncode}, "
                              f"expected one [input] refusal saying "
                              f"{isay!r}:\n{out}")
    if bad:
        print(f"run_veneer_cost_tests: FAILED ({bad} of {ran})",
              file=sys.stderr)
        return 1
    print(f"run_veneer_cost_tests: all {ran} cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
