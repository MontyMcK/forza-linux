#!/usr/bin/env python3
"""Small reverse-engineering helpers for PE files without RTTI (e.g. umd.dll).

Usage (run with python3 -P; needs pefile + capstone):
  tools/pe_re.py dis     <pe> <rva> [count]          disassemble from rva (follows nothing)
  tools/pe_re.py slots   <pe> <vtable_rva> <lo-hi>    each slot's target (thunks followed) + first insns
  tools/pe_re.py vtables <pe> [min_slots]             find vtables: runs of code pointers in data
                                                      sections, split where code LEAs a start
  tools/pe_re.py guid    <pe> <rva>...                print the GUID stored at each rva
  tools/pe_re.py findguid <pe> <GUID>...              rvas where a GUID is stored, and code comparing it

Output contains addresses and short disassembly of Microsoft/game binaries:
keep it in research/private/ (Rule 0), never in tracked files.
"""
import struct
import sys
import uuid

import capstone
import pefile
from capstone import x86


class PE:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = bytes(self.pe.get_memory_mapped_image())
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.code = [s for s in self.pe.sections if s.Characteristics & 0x20000000]

    def q(self, rva):
        return struct.unpack_from("<Q", self.img, rva)[0]

    def is_code_va(self, va):
        r = va - self.base
        return any(s.VirtualAddress <= r < s.VirtualAddress + s.Misc_VirtualSize for s in self.code)

    def insns(self, rva, n):
        out = []
        for ins in self.md.disasm(self.img[rva: rva + n * 15], self.base + rva):
            out.append(ins)
            if len(out) >= n:
                break
        return out

    def follow(self, va):
        """Follow `jmp imm` thunks."""
        for _ in range(5):
            if not self.is_code_va(va):
                break
            ins = next(self.md.disasm(self.img[va - self.base: va - self.base + 16], va), None)
            if ins and ins.mnemonic == "jmp" and ins.operands[0].type == x86.X86_OP_IMM:
                va = ins.operands[0].imm
            else:
                break
        return va

    @staticmethod
    def riprefs(ins):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                yield ins.address + ins.size + op.mem.disp

    def guid(self, rva):
        return str(uuid.UUID(bytes_le=self.img[rva: rva + 16])).upper()


def fmt(p, ins):
    extra = "".join("  ; rva 0x%x" % (t - p.base) for t in p.riprefs(ins))
    return "%x: %s %s%s" % (ins.address - p.base, ins.mnemonic, ins.op_str, extra)


def cmd_dis(p, rva, n="60"):
    for ins in p.insns(int(rva, 16), int(n)):
        print(fmt(p, ins))
        if ins.mnemonic == "int3":
            break


def cmd_slots(p, vt, rng, n="6"):
    vt = int(vt, 16)
    lo, hi = (int(x) for x in rng.split("-"))
    for s in range(lo, hi + 1):
        raw = p.q(vt + 8 * s)
        if not p.is_code_va(raw):
            print("slot %3d  (not code: 0x%x)" % (s, raw))
            continue
        f = p.follow(raw)
        body = []
        for ins in p.insns(f - p.base, int(n)):
            body.append("%s %s" % (ins.mnemonic, ins.op_str))
            if ins.mnemonic in ("ret", "int3"):
                break
        print("slot %3d  0x%06x%s | %s" % (s, f - p.base, "*" if f != raw else " ", " ; ".join(body)))


def code_leas(p):
    """rva -> list of code rvas that LEA it (linear sweep with resync on bad bytes)."""
    refs = {}
    for s in p.code:
        off, end = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
        while off < end:
            last = off
            for ins in p.md.disasm(p.img[off:end], p.base + off):
                last = ins.address - p.base + ins.size
                if ins.mnemonic == "lea":
                    for t in p.riprefs(ins):
                        refs.setdefault(t - p.base, []).append(ins.address - p.base)
            off = last + 1 if last == off else last
            if last < end:
                off = max(off, last + 1)
    return refs


def cmd_vtables(p, min_slots="20"):
    min_slots = int(min_slots)
    refs = code_leas(p)
    for s in p.pe.sections:
        if s.Characteristics & 0x20000000:
            continue
        a, end = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
        i = a
        while i + 8 <= end:
            j = i
            while j + 8 <= end and p.is_code_va(p.q(j)):
                j += 8
            if (j - i) // 8 >= min_slots:
                starts = [t for t in range(i, j, 8) if t in refs] or [i]
                bounds = starts + [j]
                for k, st in enumerate(starts):
                    print("vtable rva 0x%x  %3d slots  (%s)  lea'd from %s" % (
                        st, (bounds[k + 1] - st) // 8, s.Name.rstrip(b"\0").decode(),
                        ",".join(hex(x) for x in refs.get(st, [])[:4]) or "-"))
            i = j + 8 if j > i else i + 8


def cmd_guid(p, *rvas):
    for r in rvas:
        print(r, "{%s}" % p.guid(int(r, 16)))


def cmd_findguid(p, *guids):
    refs = None
    for g in guids:
        b = uuid.UUID(g.strip("{}")).bytes_le
        locs, i = [], p.img.find(b)
        while i >= 0:
            locs.append(i)
            i = p.img.find(b, i + 1)
        print(g, "stored at", [hex(x) for x in locs])
        if refs is None:
            refs = {}
            for s in p.code:
                for ins in p.md.disasm(p.img[s.VirtualAddress: s.VirtualAddress + s.Misc_VirtualSize],
                                       p.base + s.VirtualAddress):
                    for t in p.riprefs(ins):
                        refs.setdefault(t - p.base, []).append(ins.address - p.base)
        for l in locs:
            print("  rva 0x%x referenced by code at %s" % (l, [hex(x) for x in refs.get(l, [])[:8]]))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, path, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    fn = globals().get("cmd_" + cmd)
    if not fn:
        sys.exit(__doc__)
    fn(PE(path), *args)


if __name__ == "__main__":
    main()
