# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""The stack analysis of a linked Thumb image, shared by the stack gates (#124).

What is here is the part of a stack bound that does not depend on WHERE the
walk stops: a reader for just enough of a 32-bit ARM ELF, a Thumb-2 decoder
that works on the instruction bytes, the analysis of one function body (its
frame, its edges, whether its returns are proven) and the walk down the call
tree, and the lookup of each body's -fstack-usage record (the witness).  It
came out of check_veneer_base_cost.py unchanged (issue #124, step 1); that
script's docstring is still where the rules are stated -- how a frame, an edge
and a bound are derived, what is refused and what is not proven.

Since step 2 of #124 it has two callers, and they differ only in where the
walk stops: check_veneer_base_cost.py walks everything below a firmware root
(Walk.total), check_plugin_image.py stops at the register branch of a named
veneer and takes the two halves the plugin manifest declares (Walk.parts).
Which bodies are veneers, which may have no record and what the budget is
are the callers' facts.

[!] ONE IMPLEMENTATION, NOT TWO.  The point of the move is that a second stack
gate calls this rather than growing its own decoder: two analyses of the same
code would answer differently and nothing would say which one is right.

[!] NO BOARD NAME AND NO -mcpu.  Like the script it came from, this names no
core: an encoding the decoder does not know is a refusal, never a guess.  And
it imports neither gate -- the veneer set, which records may be absent
(--prebuilt-root) and the budget are the callers' -- so it is a dependency of
both and owns no policy.

[!] IT IS A BUILD INPUT OF BOTH GATES.  veneer_cost_gate() and add_plugin()
list this file in the DEPENDS of the commands that run them, so editing it
alone re-runs both checks; cmake/fixtures/run_veneer_gate_build_tests.py
builds that both ways (re-runs after an edit, nothing re-runs without one).
"""
import bisect
import os
import re
import struct

SP, LR, PC = 13, 14, 15
REG = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
       "r11", "r12", "sp", "lr", "pc"]


class InputError(Exception):
    """An input that cannot be read as what it claims to be."""


# ============================================================================
#  ELF
# ============================================================================
SHT_PROGBITS, SHT_SYMTAB = 1, 2
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4
STT_FUNC = 2


class Section:
    __slots__ = ("index", "name", "type", "flags", "addr", "offset", "size",
                 "link")

    def __init__(self, index, name, typ, flags, addr, offset, size, link):
        self.index, self.name, self.type, self.flags = index, name, typ, flags
        self.addr, self.offset, self.size, self.link = addr, offset, size, link

    @property
    def end(self):
        return self.addr + self.size


class Elf:
    """Just enough of a 32-bit little-endian ARM ELF: sections and symbols."""

    def __init__(self, path):
        try:
            raw = open(path, "rb").read()
        except OSError as exc:
            raise InputError(f"cannot read {path}: {exc}")
        if raw[:4] != b"\x7fELF":
            raise InputError(f"{path} is not an ELF file")
        if raw[4] != 1 or raw[5] != 1:
            raise InputError(f"{path} is not a 32-bit little-endian ELF")
        if struct.unpack_from("<H", raw, 0x12)[0] != 40:
            raise InputError(f"{path} is not an ARM ELF")
        self.raw = raw
        shoff, = struct.unpack_from("<I", raw, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 0x2E)
        hdrs = [struct.unpack_from("<10I", raw, shoff + i * shentsize)
                for i in range(shnum)]
        stro = hdrs[shstrndx][4]
        self.sections = []
        for i, h in enumerate(hdrs):
            name = raw[stro + h[0]:raw.index(b"\0", stro + h[0])].decode(
                "ascii", "replace")
            self.sections.append(Section(i, name, h[1], h[2], h[3], h[4],
                                         h[5], h[6]))
        self.code = sorted((s for s in self.sections
                            if s.type == SHT_PROGBITS and s.size
                            and s.flags & SHF_ALLOC
                            and s.flags & SHF_EXECINSTR),
                           key=lambda s: s.addr)
        self._code_starts = [s.addr for s in self.code]
        self.symbols = []
        symtab = [s for s in self.sections if s.type == SHT_SYMTAB]
        if not symtab:
            raise InputError(f"{path} has no symbol table (stripped?) -- "
                             "functions cannot be found without one")
        st = symtab[0]
        strtab = self.sections[st.link]
        for off in range(st.offset, st.offset + st.size, 16):
            name_off, value, size, info, _other, shndx = struct.unpack_from(
                "<IIIBBH", raw, off)
            no = strtab.offset + name_off
            name = raw[no:raw.index(b"\0", no)].decode("ascii", "replace")
            self.symbols.append((name, value, size, info & 0xF, shndx))

    def code_section(self, addr):
        i = bisect.bisect_right(self._code_starts, addr) - 1
        if i >= 0 and addr < self.code[i].end:
            return self.code[i]
        return None

    def u16(self, addr):
        s = self.code_section(addr)
        if s is None or addr + 2 > s.end:
            return None
        return struct.unpack_from("<H", self.raw, s.offset + addr - s.addr)[0]

    def u32(self, addr):
        s = self.code_section(addr)
        if s is None or addr + 4 > s.end:
            return None
        return struct.unpack_from("<I", self.raw, s.offset + addr - s.addr)[0]


# ============================================================================
#  The Thumb decoder
#
#  Not a disassembler: for each instruction it answers only what the walk
#  needs -- its size, which core registers it writes, what it does to sp and
#  to control flow.  It works on the instruction BYTES, not on a disassembler's
#  text: a check that parses a tool's output depends on that tool's vocabulary
#  (the MVE predication scan of issues #42/#66 passed everything because
#  objdump never decoded what it was grepping for).  Every encoding it does not
#  positively know is "undecoded", and an undecoded instruction below a veneer
#  is a refusal.  Encodings: Armv7-M ARM (DDI0403) A5 and Armv8-M ARM (DDI0553)
#  C2, including the Armv8.1-M additions a compiler emits for the Cortex-M55
#  (low-overhead loops, CSEL, the MVE scalar shifts); the vector space itself is
#  not decoded, so MVE vector code below a veneer is refused, not analysed.
# ============================================================================
class Insn:
    __slots__ = ("addr", "size", "raw", "flow", "target", "wr", "sp",
                 "sp_known", "lrk", "why", "itlen", "forever", "lit")

    def __init__(self, addr, size, raw):
        self.addr, self.size, self.raw = addr, size, raw
        self.flow = "seq"       # see Walk for the vocabulary
        self.target = None      # branch target / literal address
        self.wr = set()         # core registers written
        self.sp = 0             # constant change to sp, when sp_known
        self.sp_known = False
        self.lrk = None         # "call" | "save" | "restore" | "arb"
        self.why = None         # what the instruction is, for a diagnostic
        self.itlen = 0
        self.forever = False    # LE without a counter: never falls through
        self.lit = False

    def hexraw(self):
        return f"{self.raw:08x}" if self.size == 4 else f"{self.raw:04x}"


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def _regs(mask):
    return {r for r in range(16) if mask >> r & 1}


def _expand_imm(imm12):
    """ThumbExpandImm (Armv7-M ARM A5.3.2)."""
    if imm12 >> 10 == 0:
        b = imm12 & 0xFF
        return [b, b << 16 | b, b << 24 | b << 8,
                b * 0x01010101][(imm12 >> 8) & 3]
    unrot = 0x80 | (imm12 & 0x7F)
    rot = imm12 >> 7
    return ((unrot >> rot) | (unrot << (32 - rot))) & 0xFFFFFFFF


def is32(hw):
    return (hw >> 11) in (0b11101, 0b11110, 0b11111)


def _undef(i, why):
    i.flow = "undef"
    i.why = why
    return i


def _finish(i):
    """What follows from the written set, applied the same way for every
    encoding so that no path can forget it."""
    if SP in i.wr and not i.sp_known:
        i.sp_known = False
        if i.why is None:
            i.why = "writes sp"
    if PC in i.wr and i.flow == "seq":
        i.flow = "ind_jump"
        i.why = i.why or "writes pc"
    if LR in i.wr and i.lrk is None:
        i.lrk = "arb"
    return i


def _spdelta(i, delta, why):
    i.wr.add(SP)
    i.sp = delta
    i.sp_known = True
    i.why = why


# --- 16-bit ------------------------------------------------------------------
def dec16(hw, addr):
    i = Insn(addr, 2, hw)
    lo3 = hw & 7
    if hw >> 14 == 0:                        # shift, add, sub, mov, compare
        if hw >> 11 == 0b00101:
            pass                             # CMP imm8
        elif hw >> 13 == 0b001:
            i.wr.add((hw >> 8) & 7)
        else:
            i.wr.add(lo3)
    elif hw >> 10 == 0b010000:               # data processing
        if (hw >> 6) & 0xF not in (8, 10, 11):   # TST, CMP, CMN write nothing
            i.wr.add(lo3)
    elif hw >> 10 == 0b010001:               # special data, branch/exchange
        op = (hw >> 8) & 3
        rm = (hw >> 3) & 0xF
        rdn = (hw >> 4) & 8 | lo3
        if op == 0:                          # ADD (register) T2
            i.wr.add(rdn)
            i.why = f"add {REG[rdn]}, {REG[rm]}"
            if rdn == PC:
                i.flow = "ind_jump"
        elif op == 2:                        # MOV (register) T1
            i.wr.add(rdn)
            i.why = f"mov {REG[rdn]}, {REG[rm]}"
            if rdn == PC:
                i.flow = "ind_jump"
        elif op == 3:
            if hw & 0x80:                    # BLX / BLXNS
                if lo3 not in (0, 4):
                    return _undef(i, "BLX with nonzero low bits")
                i.flow = "ind_call"
                i.why = f"blx{'ns' if lo3 else ''} {REG[rm]}"
                i.wr.add(LR)
                i.lrk = "call"
            else:                            # BX / BXNS
                if lo3 == 0 and rm == LR:
                    i.flow = "ret_lr"
                    i.why = "bx lr"
                elif lo3 in (0, 4):
                    i.flow = "ind_jump"
                    i.why = f"bx{'ns' if lo3 else ''} {REG[rm]}"
                else:
                    return _undef(i, "BX with nonzero low bits")
    elif hw >> 11 == 0b01001:                # LDR (literal)
        i.wr.add((hw >> 8) & 7)
    elif hw >> 12 == 0b0101:                 # load/store, register offset
        if (hw >> 9) & 7 >= 3:
            i.wr.add(lo3)
    elif hw >> 13 == 0b011 or hw >> 12 == 0b1000:   # imm5 word/byte/half
        if hw & 0x800:
            i.wr.add(lo3)
    elif hw >> 12 == 0b1001:                 # load/store sp-relative (no wb)
        if hw & 0x800:
            i.wr.add((hw >> 8) & 7)
    elif hw >> 12 == 0b1010:                 # ADR / ADD Rd, sp, #imm
        i.wr.add((hw >> 8) & 7)
    elif hw >> 12 == 0b1011:
        _misc16(i, hw)
    elif hw >> 12 == 0b1100:                 # STM / LDM T1 (low registers)
        rn = (hw >> 8) & 7
        lst = hw & 0xFF
        if not lst:
            return _undef(i, "LDM/STM with an empty list")
        if hw & 0x800:
            i.wr |= _regs(lst)
            if not lst >> rn & 1:
                i.wr.add(rn)
        else:
            i.wr.add(rn)
    elif hw >> 12 == 0b1101:
        cond = (hw >> 8) & 0xF
        if cond == 0xE:
            i.flow, i.why = "trap", "udf"
        elif cond == 0xF:
            i.flow, i.why = "trap", "svc"
        else:
            i.flow = "bcc"
            i.target = addr + 4 + _sext((hw & 0xFF) << 1, 9)
    elif hw >> 11 == 0b11100:
        i.flow = "b"
        i.target = addr + 4 + _sext((hw & 0x7FF) << 1, 12)
    else:
        return _undef(i, "not a 16-bit encoding")
    return _finish(i)


def _misc16(i, hw):
    b = (hw >> 8) & 0xF
    if b == 0:                               # ADD/SUB sp, sp, #imm7
        imm = (hw & 0x7F) << 2
        neg = bool(hw & 0x80)
        _spdelta(i, -imm if neg else imm,
                 f"{'sub' if neg else 'add'} sp, #{imm}")
    elif b in (1, 3, 9, 11):                 # CBZ / CBNZ
        i.flow = "cbz"
        i.target = i.addr + 4 + (((hw >> 9) & 1) << 6 | ((hw >> 3) & 0x1F) << 1)
    elif b == 2:                             # SXTH/SXTB/UXTH/UXTB
        i.wr.add(hw & 7)
    elif b in (4, 5):                        # PUSH
        lst = hw & 0xFF
        n = bin(lst).count("1") + (b & 1)
        if not n:
            _undef(i, "PUSH with an empty list")
            return
        _spdelta(i, -4 * n, f"push ({n} regs)")
        if b & 1:
            i.lrk = "save"
    elif b == 6:
        if hw & 0xFFEF != 0xB662:            # CPS; nothing else is allocated
            _undef(i, "unallocated 1011 0110")
    elif b == 0xA:                           # REV / REV16 / REVSH
        if (hw >> 6) & 3 == 2:
            _undef(i, "HLT is not an M-profile instruction")
            return
        i.wr.add(hw & 7)
    elif b in (0xC, 0xD):                    # POP
        lst = hw & 0xFF
        n = bin(lst).count("1") + (b & 1)
        if not n:
            _undef(i, "POP with an empty list")
            return
        i.wr |= _regs(lst)
        _spdelta(i, 4 * n, f"pop ({n} regs)")
        if b & 1:
            i.wr.add(PC)
            i.flow = "ret_pop"
            i.why = "pop {..., pc}"
    elif b == 0xE:
        i.flow, i.why = "trap", "bkpt"
    elif b == 0xF:
        mask = hw & 0xF
        if mask:
            if (hw >> 4) & 0xF == 0xF:
                _undef(i, "IT with condition 0b1111")
                return
            i.flow = "it"
            i.itlen = 4 - ((mask & -mask).bit_length() - 1)
        # mask == 0: NOP-compatible hints (NOP, YIELD, WFE, WFI, SEV, ...)
    else:
        _undef(i, "unallocated 1011 0111 / 1011 1000")


# --- 32-bit ------------------------------------------------------------------
def dec32(hw1, hw2, addr):
    i = Insn(addr, 4, hw1 << 16 | hw2)
    op1 = (hw1 >> 11) & 3
    op2 = (hw1 >> 4) & 0x7F
    if op1 == 1:
        if op2 & 0b1100100 == 0b0000000:
            _ldst_multiple(i, hw1, hw2)
        elif op2 & 0b1100100 == 0b0000100:
            _ldst_dual(i, hw1, hw2)
        elif op2 & 0b1100000 == 0b0100000:
            _dp_shifted(i, hw1, hw2)
        else:
            _coproc(i, hw1, hw2)
    elif op1 == 2:
        if hw2 & 0x8000:
            _branch_misc(i, hw1, hw2)
        elif op2 & 0b0100000 == 0:
            _dp_modimm(i, hw1, hw2)
        else:
            _dp_plainimm(i, hw1, hw2)
    elif op1 == 3:
        if op2 & 0b1110001 == 0b0000000:
            _store_single(i, hw1, hw2)
        elif op2 & 0b1100111 in (0b0000001, 0b0000011, 0b0000101):
            _load_single(i, hw1, hw2)
        elif op2 & 0b1110000 == 0b0100000:
            _dp_register(i, hw1, hw2)
        elif op2 & 0b1111000 == 0b0110000:
            if hw2 & 0xC0:
                _undef(i, "multiply with bits [7:6] set")
            else:                            # MUL/MLA/MLS/SMLAxy/... -> Rd
                i.wr.add((hw2 >> 8) & 0xF)
        elif op2 & 0b1111000 == 0b0111000:
            _long_multiply(i, hw1, hw2)
        elif op2 & 0b1000000:
            _coproc(i, hw1, hw2)
        else:
            _undef(i, "unallocated load/store space")
    else:
        _undef(i, "not a 32-bit encoding")
    if i.flow == "undef":
        return i
    rd_pc = PC in i.wr and i.flow == "seq"
    if rd_pc:
        i.flow = "ind_jump"
        i.why = i.why or "writes pc"
    return _finish(i)


def _ldst_multiple(i, hw1, hw2):
    op = (hw1 >> 7) & 3
    w = hw1 >> 5 & 1
    load = hw1 >> 4 & 1
    rn = hw1 & 0xF
    lst = _regs(hw2)
    if op in (0, 3):
        _undef(i, "SRS/RFE are not M-profile instructions")
        return
    if rn == PC:
        if op == 1 and load and not w and SP not in lst:     # CLRM
            i.wr |= lst - {PC}               # bit 15 is APSR, not pc
            i.why = "clrm"
        else:
            _undef(i, "LDM/STM based on pc")
        return
    if not lst:
        _undef(i, "LDM/STM with an empty list")
        return
    n = len(lst)
    name = ("ldm" if load else "stm") + ("ia" if op == 1 else "db")
    if w:
        i.wr.add(rn)
        if rn == SP:                         # IA adds, DB subtracts
            _spdelta(i, 4 * n if op == 1 else -4 * n, f"{name} sp! ({n} regs)")
    if not load:
        if SP in lst or PC in lst:
            _undef(i, "STM storing sp or pc")
            return
        if rn == SP and LR in lst:
            i.lrk = "save"
        return
    if SP in lst:
        i.wr.add(SP)
        i.sp_known = False
        i.why = f"{name} loading sp"
        return
    i.wr |= lst
    if LR in lst:
        i.lrk = "restore" if rn == SP else "arb"
    if PC in lst:
        if rn == SP and w and op == 1:
            i.flow = "ret_pop"
            i.why = "ldmia sp!, {..., pc}"
        else:
            i.flow = "ind_jump"
            i.why = f"{name} loading pc"


def _ldst_dual(i, hw1, hw2):
    p = hw1 >> 8 & 1
    u = hw1 >> 7 & 1
    w = hw1 >> 5 & 1
    load = hw1 >> 4 & 1
    rn = hw1 & 0xF
    rt = hw2 >> 12
    rt2 = (hw2 >> 8) & 0xF
    op3 = (hw2 >> 4) & 0xF
    if p == 0 and w == 0:                    # exclusive / acquire-release / TB
        if u == 0:
            if load:                         # LDREX
                i.wr.add(rt)
            else:                            # STREX, or TT/TTT/TTA/TTAT
                i.wr.add(rt2)
            return
        if not load:
            if op3 in (4, 5, 12, 13, 14):    # STREXB/H, STLEXB/H/STLEX
                i.wr.add(hw2 & 0xF)
            elif op3 not in (8, 9, 10):      # STLB/STLH/STL write nothing
                _undef(i, "unallocated store-exclusive form")
            return
        if op3 in (0, 1):
            i.flow = "ind_jump"
            i.why = "tbb" if op3 == 0 else "tbh"
            return
        if op3 in (4, 5, 8, 9, 10, 12, 13, 14):  # LDREXB/H, LDA*, LDAEX*
            i.wr.add(rt)
        else:
            _undef(i, "unallocated load-exclusive form")
        return
    if hw1 == 0xE97F and hw2 == 0xE97F:      # SG
        i.why = "sg"
        return
    off = (hw2 & 0xFF) << 2
    off = off if u else -off
    if w and rn == PC:
        _undef(i, "LDRD/STRD writing back pc")
        return
    if w:
        i.wr.add(rn)
        if rn == SP:
            _spdelta(i, off, f"{'ldrd' if load else 'strd'} [sp] writeback "
                             f"{off:+d}")
    if load:
        if SP in (rt, rt2) or PC in (rt, rt2):
            i.wr |= {rt, rt2}
            i.why = "ldrd into sp or pc"
            if PC in (rt, rt2):
                i.flow = "ind_jump"
            i.sp_known = False if SP in (rt, rt2) else i.sp_known
            return
        i.wr |= {rt, rt2}
        if LR in (rt, rt2):
            i.lrk = "restore" if rn == SP else "arb"
    else:
        if rn == SP and LR in (rt, rt2):
            i.lrk = "save"


def _dp_shifted(i, hw1, hw2):
    op = (hw1 >> 5) & 0xF
    s = hw1 >> 4 & 1
    rn = hw1 & 0xF
    rd = (hw2 >> 8) & 0xF
    rm = hw2 & 0xF
    if hw2 & 0x8000:
        # Armv8.1-M: CSEL/CSINC/CSINV/CSNEG (and their aliases) take the
        # ORRS encoding with bit 15 set.
        if op == 2 and s and (hw2 >> 12) in (8, 9, 10, 11):
            i.wr.add(rd)
            i.why = "csel"
        else:
            _undef(i, "data-processing (shifted register) with bit 15 set")
        return
    if op == 2 and s and rn != PC and rm in (SP, PC):
        # Armv8.1-M MVE scalar shifts (ASRL, LSLL, LSRL, SQRSHR, UQSHL, ...):
        # they write the register in the Rn field, and the long ones the Rd
        # field too.  Rm = sp/pc is UNPREDICTABLE for a real ORRS.
        i.wr.add(rn)
        if rd != PC:
            i.wr.add(rd)
        i.why = "MVE scalar shift"
        return
    if op not in (0, 1, 2, 3, 4, 6, 8, 10, 11, 13, 14):
        _undef(i, "unallocated data-processing (shifted register)")
        return
    if s and rd == PC and op in (0, 4, 8, 13):
        return                               # TST / TEQ / CMN / CMP
    i.wr.add(rd)
    if rd == SP:
        i.why = "register arithmetic into sp"


def _dp_modimm(i, hw1, hw2):
    op = (hw1 >> 5) & 0xF
    s = hw1 >> 4 & 1
    rn = hw1 & 0xF
    rd = (hw2 >> 8) & 0xF
    imm = _expand_imm((hw1 >> 10 & 1) << 11 | ((hw2 >> 12) & 7) << 8
                      | (hw2 & 0xFF))
    if op not in (0, 1, 2, 3, 4, 8, 10, 11, 13, 14):
        _undef(i, "unallocated data-processing (modified immediate)")
        return
    if s and rd == PC and op in (0, 4, 8, 13):
        return                               # TST / TEQ / CMN / CMP
    if rd == SP and rn == SP and op in (8, 13):
        _spdelta(i, imm if op == 8 else -imm,
                 f"{'add' if op == 8 else 'sub'}.w sp, sp, #{imm}")
        return
    i.wr.add(rd)
    if rd == SP:
        i.why = "immediate arithmetic into sp not relative to sp"


def _dp_plainimm(i, hw1, hw2):
    op = (hw1 >> 4) & 0x1F
    rn = hw1 & 0xF
    rd = (hw2 >> 8) & 0xF
    imm12 = (hw1 >> 10 & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF)
    if op in (0b00000, 0b01010):             # ADDW / SUBW (ADR when Rn = pc)
        if rd == SP and rn == SP:
            _spdelta(i, imm12 if op == 0 else -imm12,
                     f"{'addw' if op == 0 else 'subw'} sp, sp, #{imm12}")
            return
    elif op not in (0b00100, 0b01100, 0b10000, 0b10010, 0b10100, 0b10110,
                    0b11000, 0b11010, 0b11100):
        _undef(i, "unallocated data-processing (plain binary immediate)")
        return
    i.wr.add(rd)
    if rd == SP:
        i.why = "immediate write to sp"


def _branch_misc(i, hw1, hw2):
    op1 = (hw2 >> 12) & 7
    op = (hw1 >> 4) & 0x7F
    s = hw1 >> 10 & 1
    if op1 & 0b101 == 0b000:
        if op & 0b0111000 != 0b0111000:      # B<cond> T3
            j1 = hw2 >> 13 & 1
            j2 = hw2 >> 11 & 1
            off = _sext(s << 20 | j2 << 19 | j1 << 18 | (hw1 & 0x3F) << 12
                        | (hw2 & 0x7FF) << 1, 21)
            i.flow = "bcc"
            i.target = i.addr + 4 + off
            return
        if op in (0b0111000, 0b0111001):     # MSR
            sysm = hw2 & 0xFF
            if sysm in (0x08, 0x09, 0x88, 0x89):
                i.wr.add(SP)
                i.why = "msr to a stack pointer"
            elif sysm in (0x14, 0x94):
                i.wr.add(SP)
                i.why = "msr CONTROL (may switch the active stack)"
            return
        if op == 0b0111010:                  # hints
            if hw2 & 0x0700:
                _undef(i, "CPS.W is not an M-profile encoding")
            return
        if op == 0b0111011:                  # CLREX, DSB, DMB, ISB, SB, ...
            if (hw2 >> 4) & 0xF not in (2, 4, 5, 6, 7):
                _undef(i, "unallocated miscellaneous control")
            return
        if op in (0b0111110, 0b0111111):     # MRS
            i.wr.add((hw2 >> 8) & 0xF)
            return
        if op == 0b1111111 and op1 == 0b010:
            i.flow, i.why = "trap", "udf.w"
            return
        _undef(i, "unallocated branch/miscellaneous control")
        return
    if op1 & 0b101 == 0b001 or op1 & 0b101 == 0b101:   # B T4 / BL
        j1 = hw2 >> 13 & 1
        j2 = hw2 >> 11 & 1
        i1 = 1 ^ (j1 ^ s)
        i2 = 1 ^ (j2 ^ s)
        off = _sext(s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12
                    | (hw2 & 0x7FF) << 1, 25)
        i.target = i.addr + 4 + off
        if op1 & 0b100:
            i.flow = "bl"
            i.wr.add(LR)
            i.lrk = "call"
        else:
            i.flow = "b"
        return
    # op1 = 1x0: BLX (immediate) in Armv7, which switches to ARM state and is
    # UNDEFINED on M-profile.  Armv8.1-M puts the low-overhead-branch
    # instructions here, with bit 0 set.
    if not hw2 & 1:
        _undef(i, "BLX (immediate) switches to ARM state")
        return
    imm = ((hw2 >> 1) & 0x3FF) << 2 | (hw2 >> 11 & 1) << 1
    if not hw2 & 0x2000:                     # 1100 ....: LE / LETP / WLS(TP)
        if hw1 in (0xF00F, 0xF01F, 0xF02F):
            i.flow = "le"
            i.target = i.addr + 4 - imm
            i.why = "le" if hw1 != 0xF01F else "letp"
            if hw1 == 0xF02F:
                i.forever = True             # no counter: always loops
            else:
                i.wr.add(LR)
            return
        if hw1 & 0xFFF0 == 0xF040 or hw1 & 0xFFC0 == 0xF000:
            i.flow = "wls"
            i.target = i.addr + 4 + imm
            i.wr.add(LR)
            i.why = "wls"
            return
        _undef(i, "branch-future instruction (BFL): control flow this "
                  "decoder does not model")
        return
    if hw1 == 0xF00F and hw2 == 0xE001:      # LCTP
        return
    if hw2 == 0xE001 and (hw1 & 0xFFF0 == 0xF040 or hw1 & 0xFFC0 == 0xF000):
        i.wr.add(LR)                         # DLS / DLSTP
        i.why = "dls"
        return
    if hw2 == 0xE801 and hw1 & 0xFFC0 == 0xF000:
        return                               # VCTP (writes VPR only)
    _undef(i, "branch-future instruction (BF/BFX/BFLX/BFCSEL): control flow "
              "this decoder does not model")


def _index_fields(hw1, hw2, i, what):
    """The imm8 addressing forms of the single loads/stores (T4): returns
    (writeback, delta) or None for an unallocated combination."""
    pw = (hw2 >> 8) & 0xF
    if pw == 0b1110:                         # LDRT/STRT: offset, no writeback
        return False, 0
    p, u, w = hw2 >> 10 & 1, hw2 >> 9 & 1, hw2 >> 8 & 1
    if not p and not w:
        _undef(i, f"unallocated {what} addressing mode")
        return None
    off = hw2 & 0xFF
    return bool(w), off if u else -off


def _store_single(i, hw1, hw2):
    size = (hw1 >> 5) & 3
    rn = hw1 & 0xF
    rt = hw2 >> 12
    if size == 3:
        _undef(i, "unallocated store size")
        return
    if rn == PC:
        _undef(i, "store based on pc")
        return
    wb, delta = False, 0
    if not hw1 & 0x80:                       # not the imm12 form
        if hw2 & 0x800:
            got = _index_fields(hw1, hw2, i, "store")
            if got is None:
                return
            wb, delta = got
        elif (hw2 >> 6) & 0x3F:
            _undef(i, "unallocated store (register) form")
            return
    if rt == PC:
        _undef(i, "store of pc")
        return
    if wb:
        i.wr.add(rn)
        if rn == SP:
            _spdelta(i, delta, f"str [sp] writeback {delta:+d}")
    if rn == SP and rt == LR and size == 2:
        i.lrk = "save"


def _load_single(i, hw1, hw2):
    size = (hw1 >> 5) & 3
    rn = hw1 & 0xF
    rt = hw2 >> 12
    wb, delta = False, 0
    post = False
    if rn == PC:                             # literal
        if rt == PC and size == 2:
            imm = hw2 & 0xFFF
            base = (i.addr + 4) & ~3
            i.flow = "lit_pc"
            i.target = base + imm if hw1 & 0x80 else base - imm
            i.lit = True
            i.wr.add(PC)
            i.why = "ldr pc, [pc, #imm]"
            return
    elif not hw1 & 0x80:                     # not the imm12 form
        if hw2 & 0x800:
            got = _index_fields(hw1, hw2, i, "load")
            if got is None:
                return
            wb, delta = got
            post = not hw2 >> 10 & 1
        elif (hw2 >> 6) & 0x3F:
            _undef(i, "unallocated load (register) form")
            return
    if rt == PC and size != 2:
        if wb:
            _undef(i, "preload with writeback")
            return
        return                               # PLD / PLI / hints
    if wb:
        i.wr.add(rn)
        if rn == SP:
            _spdelta(i, delta, f"ldr [sp] writeback {delta:+d}")
    if rt == SP:
        i.wr.add(SP)
        i.sp_known = False
        i.why = "load into sp"
        return
    i.wr.add(rt)
    if rt == PC:
        if rn == SP and wb and post and delta > 0:
            i.flow = "ret_pop"
            i.why = "ldr pc, [sp], #imm"
        else:
            i.flow = "ind_jump"
            i.why = "load into pc"
        return
    if rt == LR:
        i.lrk = "restore" if rn == SP and size == 2 else "arb"


def _dp_register(i, hw1, hw2):
    if hw2 >> 12 != 0xF:
        _undef(i, "data-processing (register) without 1111 in [15:12]")
        return
    op1 = (hw1 >> 4) & 0xF
    op2 = (hw2 >> 4) & 0xF
    ok = ((op2 == 0 and op1 < 8)                         # LSL/LSR/ASR/ROR
          or (op2 & 8 and op1 < 6)                       # extend (+add)
          or (op1 & 8 and op2 < 8)                       # parallel add/sub
          or (op1 & 0xC == 8 and op2 & 0xC == 8
              and (op1 & 3 < 2 or op2 == 8)))            # QADD..CLZ
    if not ok:
        _undef(i, "unallocated data-processing (register)")
        return
    rd = (hw2 >> 8) & 0xF
    i.wr.add(rd)
    if rd == SP:
        i.why = "register arithmetic into sp"


def _long_multiply(i, hw1, hw2):
    op1 = (hw1 >> 4) & 7
    op2 = (hw2 >> 4) & 0xF
    if op1 in (1, 3) and op2 == 0xF:         # SDIV / UDIV
        i.wr.add((hw2 >> 8) & 0xF)
    elif op1 in (0, 2, 4, 5, 6):             # (S|U)MULL, (S|U)MLAL, UMAAL, ...
        i.wr |= {hw2 >> 12, (hw2 >> 8) & 0xF}
    else:
        _undef(i, "unallocated long multiply/divide")


# MVE predication (Armv8.1-M): VPST, VPT (all forms), VPNOT.  They set VPR and
# write no core register.  (value, mask) over the whole 32-bit word.
MVE_PREDICATION = (
    (0xFE310F4D, 0xFFBF1FFF),                # VPST (and VPNOT)
    (0xEE310F00, 0xEFB10F50), (0xEE310F40, 0xEFB10F50),   # VPT (FP)
    (0xFE010F00, 0xFF811F51), (0xFE010F01, 0xFF811F51),   # VPT (vector)
    (0xFE011F00, 0xFF811F50), (0xFE010F40, 0xFF811F70),
    (0xFE010F60, 0xFF811F70), (0xFE011F40, 0xFF811F50),
)


def _mve_contiguous(i, hw1, hw2):
    """MVE VLDRB/VLDRH/VLDRW and VSTRB/VSTRH/VSTRW, contiguous (T1, T2, T5,
    T6, T7).  They write Q registers; the one core-register effect is the
    base writeback."""
    p, w = hw1 >> 8 & 1, hw1 >> 5 & 1
    if not p and not w:
        return False                 # gather/scatter: not decoded here
    if hw2 & 0x1E00 == 0x0E00:       # T1/T2: widening, Rn is r0-r7
        rn = hw1 & 7
    elif (hw2 >> 7) & 0x3F in (0b111100, 0b111101, 0b111110):
        rn = hw1 & 0xF               # T5/T6/T7
    else:
        return False
    if rn == PC:
        return False
    if w and rn == SP:
        # UNPREDICTABLE (the assembler says so too): not a constant change,
        # whatever the offset field holds.
        _undef(i, "MVE load/store writing back sp is UNPREDICTABLE")
        return True
    i.why = "MVE vector load/store"
    if w:
        i.wr.add(rn)
    return True


def _coproc(i, hw1, hw2):
    """The coprocessor space: the floating-point classes (cp9-cp11), and of
    MVE only the contiguous loads/stores and the predication instructions.
    Everything else -- vector arithmetic, CDE, generic coprocessors -- is
    'undecoded': refused below a veneer, never assumed harmless."""
    coproc = (hw2 >> 8) & 0xF
    top = (hw1 >> 8) & 0xF
    word = hw1 << 16 | hw2
    if any(word & m == v for v, m in MVE_PREDICATION):
        i.why = "MVE predication"
        return
    if coproc in (14, 15) and top in (0xC, 0xD) and not hw1 >> 12 & 1:
        if _mve_contiguous(i, hw1, hw2):
            return
    if top == 0xF:
        _undef(i, "vector instruction (MVE / Advanced SIMD space)")
        return
    if coproc not in (9, 10, 11):
        _undef(i, f"coprocessor/vector instruction (coproc {coproc})")
        return
    if top in (0xC, 0xD):                    # FP load/store, 64-bit transfer
        if hw1 >> 12 & 1:
            _undef(i, "LDC2/STC2 space")
            return
        p, u, d, w, load = (hw1 >> 8 & 1, hw1 >> 7 & 1, hw1 >> 6 & 1,
                            hw1 >> 5 & 1, hw1 >> 4 & 1)
        rn = hw1 & 0xF
        if not p and not u:
            if d and not w:                  # VMOV two core <-> ext
                if load:
                    i.wr |= {hw2 >> 12, hw1 & 0xF}
                return
            if not d and w and hw2 == 0x0A00:    # VLSTM / VLLDM
                return
            _undef(i, "unallocated FP load/store")
            return
        if p and not w:                      # VLDR / VSTR
            return
        if p and u and w:
            _undef(i, "unallocated FP load/store")
            return
        if rn == PC:
            if not p and u and w and not load:
                return                       # VSCCLRM
            _undef(i, "FP load/store multiple based on pc")
            return
        if w:                                # VLDM/VSTM IA!, VLDMDB/VSTMDB!
            imm = (hw2 & 0xFF) << 2
            i.wr.add(rn)
            if rn == SP:
                _spdelta(i, imm if u else -imm,
                         f"{'vldm' if load else 'vstm'} sp! "
                         f"{imm if u else -imm:+d}")
        return
    if top == 0xE:
        if not hw2 & 0x10:                   # FP data processing
            return
        if hw1 >> 4 & 1:                     # transfer to a core register
            rt = hw2 >> 12
            if rt == PC:
                if hw1 & 0xEFFF == 0xEEF1 and hw2 & 0x0FFF == 0x0A10:
                    return                   # VMRS APSR_nzcv, FPSCR
                _undef(i, "FP/vector transfer into pc")
                return
            i.wr.add(rt)
        return
    _undef(i, "unallocated coprocessor space")


def decode(elf, addr):
    """The instruction at addr, or None when its bytes are not in the image."""
    hw1 = elf.u16(addr)
    if hw1 is None:
        return None
    if is32(hw1):
        hw2 = elf.u16(addr + 2)
        if hw2 is None:
            return None
        return dec32(hw1, hw2, addr)
    return dec16(hw1, addr)


# ============================================================================
#  The map: which input file each piece of code came from
# ============================================================================
MAP_SEC1 = re.compile(
    r"^ (\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
MAP_SEC2 = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
MAP_NAME = re.compile(r"^ (\S+)$")


def parse_map(path, code_names):
    """([(addr, size, input)], {loaded input}): every non-empty input section
    the map places in an executable output section, and every file the link
    LOADed.  Only executable sections: the debug sections list their inputs at
    offsets that overlap real code addresses."""
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError as exc:
        raise InputError(f"cannot read map {path}: {exc}")
    out = []
    loads = set()
    in_map = False
    cur = None
    pending = None
    for line in lines:
        if not in_map:
            in_map = line.startswith("Linker script and memory map")
            continue
        if line.startswith("Cross Reference Table"):
            break
        if not line.strip():
            continue
        if line.startswith("LOAD "):
            loads.add(line[5:].strip())
        if not line[0].isspace():
            tok = line.split()
            cur = tok[0] if tok[0] in code_names else None
            pending = None
            continue
        if cur is None:
            continue
        m = MAP_SEC1.match(line)
        if m and not m.group(1).startswith("*"):
            if int(m.group(3), 16):
                out.append((int(m.group(2), 16), int(m.group(3), 16),
                            m.group(4).strip()))
            pending = None
            continue
        m = MAP_NAME.match(line)
        if m and not m.group(1).startswith("*"):
            pending = m.group(1)
            continue
        m = MAP_SEC2.match(line)
        if m and pending is not None:
            if int(m.group(2), 16):
                out.append((int(m.group(1), 16), int(m.group(2), 16),
                            m.group(3).strip()))
        pending = None
    if not in_map:
        raise InputError(f"{path} has no 'Linker script and memory map' "
                         "section -- not a GNU ld map")
    out.sort()
    return out, loads


# ============================================================================
#  Functions and their analysis
# ============================================================================
class Node:
    __slots__ = ("addr", "end", "names", "section", "problems", "inferred",
                 "origin", "info")

    def __init__(self, addr, section):
        self.addr = addr
        self.end = None
        self.names = []
        self.section = section
        self.problems = []
        self.inferred = False
        self.origin = None
        self.info = None

    @property
    def name(self):
        return sorted(self.names, key=lambda n: ("." in n, len(n), n))[0]


class Info:
    """What one body's analysis found."""
    __slots__ = ("frame", "frame_errs", "flow_errs", "edges", "veneer",
                 "sp_ops", "indirect")

    def __init__(self):
        self.frame = 0
        self.frame_errs = []   # the frame is not a bound
        self.flow_errs = []    # the body's control flow is not closed
        self.edges = []        # (callee addr, kind, at)
        self.veneer = None     # target addr, when the body is a linker veneer
        self.sp_ops = 0
        # (at, why, the flow_errs entry) for every indirect transfer the body
        # reaches.  Each is ALSO in flow_errs: this only names them, so that a
        # caller that may exempt one (a plugin veneer, issue #124) removes that
        # exact entry and nothing else.
        self.indirect = []


class Image:
    def __init__(self, elf, map_inputs, loads=()):
        self.elf = elf
        self.map_inputs = map_inputs
        self.loads = set(loads)
        self._map_starts = [a for a, _, _ in map_inputs]
        # mapping symbols per code section
        self.maps = {}
        for name, value, _size, _typ, shndx in elf.symbols:
            if name[:2] in ("$t", "$d", "$a") and (len(name) == 2
                                                   or name[2] == "."):
                self.maps.setdefault(shndx, []).append((value, name[1]))
        for v in self.maps.values():
            v.sort()
        self.nodes = {}
        code_idx = {s.index: s for s in elf.code}
        sizes = {}
        for name, value, size, typ, shndx in elf.symbols:
            if typ != STT_FUNC or shndx not in code_idx:
                continue
            addr = value & ~1
            node = self.nodes.get(addr)
            if node is None:
                node = self.nodes[addr] = Node(addr, code_idx[shndx])
            node.names.append(name)
            if not value & 1:
                node.problems.append(f"{name} is an ARM-state symbol")
            sizes.setdefault(addr, set()).add(size)
        self.starts = sorted(self.nodes)
        for idx, addr in enumerate(self.starts):
            node = self.nodes[addr]
            known = sizes[addr] - {0}
            nxt = self.starts[idx + 1] if idx + 1 < len(self.starts) else None
            if len(known) > 1:
                node.problems.append(
                    "its symbols disagree about its size ("
                    + ", ".join(str(s) for s in sorted(known)) + ")")
            if known:
                node.end = addr + max(known)
            else:
                # Assembler that declared no .size: up to the next function or
                # the end of its input section, whichever is first.
                ends = [node.section.end]
                if nxt is not None:
                    ends.append(nxt)
                inp = self.input_at(addr)
                if inp is not None:
                    ends.append(inp[0] + inp[1])
                node.end = min(ends)
                node.inferred = True
            if node.end > node.section.end:
                node.problems.append("its extent runs past its section")
            if nxt is not None and nxt < node.end:
                node.problems.append(
                    f"its extent overlaps {self.nodes[nxt].name} at "
                    f"0x{nxt:08x}")

    def input_at(self, addr):
        i = bisect.bisect_right(self._map_starts, addr) - 1
        if i >= 0:
            a, size, what = self.map_inputs[i]
            if addr < a + size:
                return a, size, what
        return None

    def node_containing(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            n = self.nodes[self.starts[i]]
            if addr < n.end:
                return n
        return None

    def regions(self, node):
        """[(start, end, kind)] covering the node's extent, from its section's
        mapping symbols.  kind is 't', 'd', 'a' or None (no symbol yet)."""
        maps = self.maps.get(node.section.index, [])
        keys = [a for a, _ in maps]
        i = bisect.bisect_right(keys, node.addr) - 1
        kind = maps[i][1] if i >= 0 else None
        out = []
        cur = node.addr
        j = i + 1
        while cur < node.end:
            nxt = node.end
            while j < len(maps) and maps[j][0] <= cur:
                kind = maps[j][1]
                j += 1
            if j < len(maps) and maps[j][0] < node.end:
                nxt = maps[j][0]
            out.append((cur, nxt, kind))
            cur = nxt
        return out

    # --- one body ------------------------------------------------------------
    def analyse(self, node):
        if node.info is not None:
            return node.info
        info = node.info = Info()
        for p in node.problems:
            info.frame_errs.append((node.addr, p))
        insns = {}
        datas = []
        for start, end, kind in self.regions(node):
            if kind == "d":
                datas.append((start, end))
                continue
            if kind != "t":
                info.frame_errs.append(
                    (start, "code at 0x%08x is %s" % (
                        start, "ARM state" if kind == "a"
                        else "covered by no mapping symbol")))
                continue
            a = start
            while a < end:
                ins = decode(self.elf, a)
                if ins is None or a + ins.size > end:
                    info.frame_errs.append(
                        (a, f"an instruction at 0x{a:08x} runs past its code "
                            "region"))
                    break
                insns[a] = ins
                a += ins.size
        # IT blocks, in linear order
        order = sorted(insns)
        in_it = {}
        k = 0
        while k < len(order):
            ins = insns[order[k]]
            if ins.flow == "it":
                for m in range(1, ins.itlen + 1):
                    if k + m >= len(order) or \
                            order[k + m] != order[k + m - 1] + \
                            insns[order[k + m - 1]].size:
                        info.flow_errs.append(
                            (ins.addr, f"the IT block at 0x{ins.addr:08x} "
                                       "runs out of code"))
                        break
                    inner = insns[order[k + m]]
                    in_it[inner.addr] = (m == ins.itlen)
                    if inner.flow in ("it", "cbz", "bcc", "le", "wls") or (
                            inner.flow not in ("seq", "undef", "trap")
                            and m != ins.itlen):
                        info.flow_errs.append(
                            (inner.addr, f"0x{inner.addr:08x} is a branch "
                                         "that may not appear there in an "
                                         "IT block"))
            k += 1
        # frame, over every instruction of the body
        for ins in insns.values():
            if ins.flow == "undef":
                info.frame_errs.append(
                    (ins.addr, f"cannot decode {ins.hexraw()} at "
                               f"0x{ins.addr:08x} ({ins.why})"))
                continue
            if SP in ins.wr:
                if ins.sp_known:
                    info.sp_ops += 1
                    if ins.sp < 0:
                        info.frame -= ins.sp
                else:
                    info.frame_errs.append(
                        (ins.addr, f"{ins.why} at 0x{ins.addr:08x} "
                                   f"({ins.hexraw()}): the stack pointer's "
                                   "change is not a known constant"))

        # A linker long-branch veneer: the body is exactly `ldr.w pc, [pc,#imm]`
        # and a literal word, and the word is the Thumb entry of a function.
        #
        # [!] THE LITERAL MUST LIE IN THIS BODY'S OWN DATA.  `datas` holds only
        # this node's data regions (regions() never leaves the body), so that
        # one test is the whole "own body" guard -- there is no separate extent
        # check to keep in step with it.  Reading the word from ANYWHERE else --
        # another function's constant pool, a jump table, a word in RAM -- would
        # let an ordinary `ldr pc` masquerade as a veneer: the gate would follow
        # whatever those four bytes happen to spell as a callee and charge that
        # instead of the real one, undercounting the stack below the real edge.
        # An `ldr pc` whose literal is not in its own body is not a veneer; the
        # reachability walk below refuses it as an indirect branch.
        entry = insns.get(node.addr)
        if entry is None:
            info.flow_errs.append((node.addr, "the entry is not an instruction "
                                              "in a Thumb code region"))
            return info
        if entry.flow == "lit_pc":
            lit = entry.target
            in_own_data = any(s <= lit and lit + 4 <= e for s, e in datas)
            if in_own_data:
                val = self.elf.u32(lit)
                tgt = val & ~1 if val is not None else None
                if val is None or not val & 1:
                    info.flow_errs.append(
                        (node.addr, "a long-branch veneer whose literal is "
                                    "not a Thumb address"))
                elif tgt not in self.nodes:
                    info.flow_errs.append(
                        (node.addr, f"a long-branch veneer to 0x{tgt:08x}, "
                                    "which is no function's entry"))
                else:
                    info.veneer = tgt
                    info.edges.append((tgt, "veneer", node.addr))
                return info
        # reachability over the body
        seen = set()
        work = [node.addr]
        succ = {}
        while work:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            ins = insns[a]
            cond = a in in_it
            nxt = []
            fall = ins.addr + ins.size
            f = ins.flow
            falls = True
            if f in ("b", "ret_lr", "ret_pop", "ind_jump", "lit_pc"):
                falls = cond
            elif f == "le":
                falls = not ins.forever or cond
            elif f in ("trap", "undef"):
                falls = False
            if f == "trap":
                info.flow_errs.append(
                    (a, f"{ins.why} at 0x{a:08x} (an exception, not a call)"))
            elif f == "ind_call":
                err = (a, f"indirect call `{ins.why}` at 0x{a:08x} -- the "
                          "walk cannot know what it reaches")
                info.flow_errs.append(err)
                info.indirect.append((a, ins.why, err))
            elif f == "ind_jump":
                err = (a, f"indirect branch `{ins.why}` at 0x{a:08x} -- the "
                          "walk cannot know where it goes")
                info.flow_errs.append(err)
                info.indirect.append((a, ins.why, err))
            elif f == "lit_pc":
                err = (a, f"`{ins.why}` at 0x{a:08x} outside a linker "
                          "long-branch veneer")
                info.flow_errs.append(err)
                info.indirect.append((a, ins.why, err))
            if f in ("b", "bcc", "cbz", "le", "wls", "bl"):
                t = ins.target
                if f == "bl":
                    if t in self.nodes:
                        info.edges.append((t, "call", a))
                    else:
                        self._bad_target(info, node, a, t, "call")
                elif node.addr < t < node.end:
                    if t in insns:
                        if t in in_it:
                            info.flow_errs.append(
                                (a, f"branch at 0x{a:08x} into an IT block"))
                        else:
                            nxt.append(t)
                    else:
                        info.flow_errs.append(
                            (a, f"branch at 0x{a:08x} to 0x{t:08x}, which is "
                                "not an instruction of its own body"))
                elif f in ("le", "wls"):
                    info.flow_errs.append(
                        (a, f"loop branch at 0x{a:08x} leaves the body"))
                elif t in self.nodes:
                    info.edges.append((t, "tail", a))
                else:
                    self._bad_target(info, node, a, t, "branch")
            if falls:
                if fall in insns and fall not in in_it or \
                        (fall in in_it and ins.flow == "it") or \
                        (fall in in_it and a in in_it):
                    nxt.append(fall)
                elif fall >= node.end:
                    info.flow_errs.append(
                        (a, f"the instruction at 0x{a:08x} falls off the end "
                            "of the body"))
                else:
                    info.flow_errs.append(
                        (a, f"the instruction at 0x{a:08x} falls through into "
                            "data or into the middle of an instruction"))
            succ[a] = nxt
            work.extend(nxt)
        # stack changes on a cycle of the body
        for comp in _sccs(seen, succ):
            if len(comp) == 1:
                a = next(iter(comp))
                if a not in succ.get(a, []):
                    continue
            for a in sorted(comp):
                ins = insns[a]
                if SP in ins.wr:
                    info.frame_errs.append(
                        (a, f"{ins.why} at 0x{a:08x} is inside a loop -- a "
                            "change counted once would be made per "
                            "iteration"))
                    break
        # returns: lr must hold the return address (bx lr), or have been
        # saved while it did (pop into pc), on EVERY path to the return -- a
        # must-analysis over (lr intact, lr saved), iterated to its greatest
        # fixpoint.  A reload of lr from the stack is trusted to reload what was
        # saved; slot identity is not tracked (see the module docstring).
        preds = {}
        for a, ns in succ.items():
            for b in ns:
                preds.setdefault(b, []).append(a)
        top = (True, True)
        OUT = {a: top for a in seen}
        IN = {}
        changed = True
        while changed:
            changed = False
            for a in sorted(seen):
                ps = [OUT[p] for p in preds.get(a, [])]
                if a == node.addr:
                    ps.append((True, False))
                IN[a] = st = (all(p[0] for p in ps), all(p[1] for p in ps))
                ins = insns[a]
                ok, saved = st
                cond = a in in_it
                if ins.lrk == "save":
                    saved = saved or (ok and not cond)
                if ins.lrk in ("call", "arb"):
                    ok = False
                elif ins.lrk == "restore":
                    ok = (ok and saved) if cond else saved
                if OUT[a] != (ok, saved):
                    OUT[a] = (ok, saved)
                    changed = True
        for a in sorted(seen):
            ins = insns[a]
            st = IN[a]
            if ins.flow == "ret_lr" and not st[0]:
                info.flow_errs.append(
                    (a, f"`bx lr` at 0x{a:08x} is not proven to be a return: "
                        "lr may have been overwritten on some path"))
            elif ins.flow == "ret_pop" and not st[1]:
                info.flow_errs.append(
                    (a, f"`{ins.why}` at 0x{a:08x} is not proven to be a "
                        "return: lr was not saved on every path"))
        return info

    def _bad_target(self, info, node, at, t, what):
        other = self.node_containing(t)
        if other is not None and other is not node:
            info.flow_errs.append(
                (at, f"{what} at 0x{at:08x} into the interior of "
                     f"{other.name} (0x{t:08x}), not its entry"))
        elif other is node:
            info.flow_errs.append(
                (at, f"{what} at 0x{at:08x} back into its own body "
                     f"(0x{t:08x})"))
        else:
            info.flow_errs.append(
                (at, f"{what} at 0x{at:08x} to 0x{t:08x}, which is no "
                     "function's entry"))


def _sccs(nodes, succ):
    """Tarjan, iterative."""
    index = {}
    low = {}
    onstack = set()
    stack = []
    out = []
    counter = [0]
    for v in sorted(nodes):
        if v in index:
            continue
        work = [(v, 0)]
        while work:
            u, pi = work.pop()
            if pi == 0:
                index[u] = low[u] = counter[0]
                counter[0] += 1
                stack.append(u)
                onstack.add(u)
            ns = succ.get(u, [])
            if pi < len(ns):
                work.append((u, pi + 1))
                w = ns[pi]
                if w not in index:
                    work.append((w, 0))
                elif w in onstack:
                    low[u] = min(low[u], index[w])
                continue
            if low[u] == index[u]:
                comp = set()
                while True:
                    w = stack.pop()
                    onstack.discard(w)
                    comp.add(w)
                    if w == u:
                        break
                out.append(comp)
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[u])
    return out


# ============================================================================
#  Provenance and witnesses (-fstack-usage records)
#
#  Moved here from check_veneer_base_cost.py in step 2 of issue #124, so that
#  both gates find a body's record by one rule: by the map's input file of
#  THAT body, never by a name looked up across the build.  Which bodies may
#  have no record (--prebuilt-root) is the caller's policy, not this module's.
# ============================================================================
ARCHIVE_RE = re.compile(r"^(.*)\(([^()]+)\)$")
LTRANS_O_RE = re.compile(r"\.ltrans(\d+)\.ltrans\.o$")
LTRANS_SU_RE = re.compile(r"\.ltrans(\d+)\.ltrans\.su$")
SU_LINE = re.compile(r"^(.*):(\d+):(\d+):(.+)\t(\d+)\t(\S+)$")


def read_su(path):
    """{name: [(frame, qualifier, location)]} from one -fstack-usage file."""
    recs = {}
    try:
        text = open(path, errors="replace").read()
    except OSError as exc:
        raise InputError(f"cannot read {path}: {exc}")
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        m = SU_LINE.match(line)
        if not m:
            raise InputError(f"{path}:{n}: not a -fstack-usage record: "
                             f"{line[:80]!r}")
        recs.setdefault(m.group(4), []).append(
            (int(m.group(5)), m.group(6), f"{m.group(1)}:{m.group(2)}"))
    return recs


LTO_PRIV = re.compile(r"^(.*?)((?:\.lto_priv\.\d+)+)$")


def record_names(name):
    """The spellings -fstack-usage may use for one ELF symbol, most exact
    first.  Measured with the pinned GCC: the record drops the NUMBER of an
    IPA clone but not the clone's tag -- `f.constprop.0.isra.0` is recorded as
    `f.constprop.isra` -- while a partial-inlining part keeps its own
    (`f.part.0.constprop`); and LTO's `f.lto_priv.0` is a rename in the symbol
    table only, recorded as `f`.  Every combination of dropped numbers is
    tried, fewest first; a spelling can only ever match a record in the body's
    OWN record file, and two records that disagree are refused as ambiguous."""
    out = [name]
    m = LTO_PRIV.match(name)
    base = m.group(1) if m else name
    parts = base.split(".")
    nums = [k for k in range(1, len(parts)) if parts[k].isdigit()]
    for drop in range(len(nums) + 1):
        for combo in _combos(nums[::-1], drop):
            cand = ".".join(p for k, p in enumerate(parts) if k not in combo)
            if cand not in out:
                out.append(cand)
    return out


def _combos(items, k):
    if k == 0:
        yield set()
        return
    for idx, it in enumerate(items):
        for rest in _combos(items[idx + 1:], k - 1):
            yield {it} | rest


class Witnesses:
    def __init__(self, image, link_dir, prebuilt_roots, ltrans_su):
        self.image = image
        self.link_dir = link_dir
        self.roots = [os.path.realpath(r) for r in prebuilt_roots]
        self.errors = []
        self._su_cache = {}
        self.ltrans = {}
        for path in ltrans_su:
            m = LTRANS_SU_RE.search(os.path.basename(path))
            if not m:
                raise InputError(f"--ltrans-su {path}: not named like an LTO "
                                 "partition's record (*.ltransN.ltrans.su)")
            n = int(m.group(1))
            if n in self.ltrans:
                raise InputError(f"two records for LTO partition {n}: "
                                 f"{self.ltrans[n]} and {path}")
            self.ltrans[n] = path
        seen_parts = set()
        for what in image.loads:
            m = LTRANS_O_RE.search(what)
            if m and not ARCHIVE_RE.match(what):
                seen_parts.add(int(m.group(1)))
        for n in sorted(set(self.ltrans) - seen_parts):
            self.errors.append(
                f"{self.ltrans[n]} is the record of LTO partition {n}, which "
                "this image's map does not contain -- a record left by an "
                "earlier link is not a witness of this one")

    def origin(self, node):
        """('prebuilt' | 'stub' | 'object' | 'ltrans' | 'unknown', detail)"""
        if node.origin is not None:
            return node.origin
        inp = self.image.input_at(node.addr)
        if inp is None:
            o = ("unknown", "the map places no input section there")
        else:
            a, size, what = inp
            if node.end > a + size:
                o = ("unknown", f"its extent runs past its input section "
                                f"({what})")
            elif what == "linker stubs":
                o = ("stub", what)
            else:
                m = ARCHIVE_RE.match(what)
                if m:
                    arc = os.path.realpath(os.path.join(self.link_dir,
                                                        m.group(1)))
                    if any(arc.startswith(r + os.sep) for r in self.roots):
                        o = ("prebuilt", what)
                    else:
                        o = ("unknown", f"archive member {what} is not under "
                                        "a --prebuilt-root, and nothing says "
                                        "where its record is")
                else:
                    m = LTRANS_O_RE.search(what)
                    if m:
                        o = ("ltrans", int(m.group(1)))
                    else:
                        o = ("object", os.path.join(self.link_dir, what))
        node.origin = o
        return o

    def records(self, node):
        """(state, detail): state is 'none' (no witness is required), 'missing',
        'ambiguous', 'dynamic' or 'ok' (detail = (frame, where))."""
        kind, detail = self.origin(node)
        if kind in ("prebuilt", "stub"):
            return "none", kind
        if kind == "unknown":
            return "missing", detail
        if kind == "ltrans":
            path = self.ltrans.get(detail)
            if path is None:
                return "missing", (f"LTO partition {detail} has no record "
                                   "file")
        else:
            path = os.path.splitext(detail)[0] + ".su"
            if not os.path.exists(path):
                return "missing", f"no record file {path}"
        recs = self._su(path)
        # [!] EVERY SPELLING OF EVERY NAME, NOT THE FIRST THAT HITS.  Picking
        # one would make the answer depend on the order the spellings are
        # tried in; taking them all makes two records that could belong to
        # this body disagree out loud (ambiguous) instead.
        cands = []
        tried = set()
        for name in node.names:
            for spelling in record_names(name):
                if spelling in recs and spelling not in tried:
                    tried.add(spelling)
                    cands.extend(recs[spelling])
        if not cands:
            return "missing", f"no record for {node.name} in {path}"
        if any(q != "static" for _f, q, _w in cands):
            q = [q for _f, q, _w in cands if q != "static"][0]
            return "dynamic", f"{path} records {node.name} as '{q}'"
        if len({f for f, _q, _w in cands}) > 1:
            return "ambiguous", (f"{path} has {len(cands)} different records "
                                 f"named like {node.name}")
        return "ok", (cands[0][0], path)

    def _su(self, path):
        if path not in self._su_cache:
            self._su_cache[path] = read_su(path)
        return self._su_cache[path]


# ============================================================================
#  The walk
# ============================================================================
# [!] THE ONE INDIRECT TRANSFER A CROSSING MAY EXEMPT (issue #124): a branch
# through a general-purpose register, `bx rN` / `blx rN` with N in r0-r12 --
# what the plugin's veneers compile to (`bx r3`, `bx r12`, `blx r3`).  Not a
# load or move into pc, not tbb/tbh, not `bx lr` that is not a proven return,
# and not BXNS/BLXNS: each of those is still refused inside a veneer too.
CROSSING_WHY = re.compile(r"^bl?x r(?:[0-9]|1[0-2])$")


def crossing_sites(info):
    """The entries of info.flow_errs that a crossing body may exempt."""
    return [err for _at, why, err in info.indirect if CROSSING_WHY.match(why)]


class Walk:
    """The walk down the call tree, by ADDRESS.

    total() walks everything below a root (the firmware's check).  parts()
    is the same walk for a caller that names CROSSINGS -- bodies whose
    register branches lead somewhere this image does not contain (the plugin
    gate's veneers, issue #124): it stops there and returns the two halves
    the plugin manifest declares.  Which body is a crossing is the caller's
    fact; how frames, edges and recursion are found is this module's, for both.
    """

    def __init__(self, image, crossings=()):
        self.image = image
        self.memo = {}
        self.pmemo = {}
        self.crossings = frozenset(crossings)
        self.errors = []
        self.recursion = set()
        self.closure = set()

    def _recursion(self, addr, path):
        cyc = list(path[path.index(addr):]) + [addr]
        key = frozenset(cyc)
        if key not in self.recursion:
            self.recursion.add(key)
            names = " -> ".join(self.image.nodes[a].name for a in cyc)
            self.errors.append(("walk", f"recursion: {names} -- no bound "
                                        "exists"))

    def body_errors(self, addr):
        """Every refusal of one body, less the register branches of a crossing.
        (errors, crosses)."""
        info = self.image.analyse(self.image.nodes[addr])
        exempt = crossing_sites(info) if addr in self.crossings else []
        errs = [e for e in info.frame_errs + info.flow_errs
                if not any(e is x for x in exempt)]
        return errs, bool(exempt)

    def parts(self, addr, path=()):
        """(A0, A1, complete) below addr.

          A0  the deepest the image's own frames go, over every path -- a path
              that reaches a crossing counts up to and including the crossing
              body's frame;
          A1  the deepest they go on a path that REACHES a crossing, or None
              when no path does.  A1 <= A0.

        The crossing body's direct edges are walked like any other's; only its
        register branches stop the walk.  `complete` is False when a body on
        the way was refused (body_errors) or recursion was found."""
        if addr in path:
            self._recursion(addr, path)
            return 0, None, False
        if addr in self.pmemo:
            return self.pmemo[addr]
        info = self.image.analyse(self.image.nodes[addr])
        self.closure.add(addr)
        errs, crosses = self.body_errors(addr)
        complete = not errs
        own, cross = 0, (0 if crosses else None)
        for callee, _kind, _at in info.edges:
            o, x, ok = self.parts(callee, path + (addr,))
            complete = complete and ok
            own = max(own, o)
            if x is not None:
                cross = x if cross is None else max(cross, x)
        res = (info.frame + own,
               None if cross is None else info.frame + cross, complete)
        self.pmemo[addr] = res
        return res

    def total(self, addr, path=()):
        if addr in path:
            self._recursion(addr, path)
            return 0, [], False
        if addr in self.memo:
            return self.memo[addr]
        node = self.image.nodes[addr]
        self.closure.add(addr)
        info = self.image.analyse(node)
        complete = not info.frame_errs and not info.flow_errs
        best, chain = 0, []
        for callee, _kind, _at in info.edges:
            t, ch, ok = self.total(callee, path + (addr,))
            complete = complete and ok
            if not chain or t > best:
                best, chain = t, ch
        res = (info.frame + best, [addr] + chain, complete)
        self.memo[addr] = res
        return res
