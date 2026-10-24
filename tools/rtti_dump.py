#!/usr/bin/env python3
"""Dump MSVC x64 RTTI: every vtable with its class name and method count.

Usage: python3 -P tools/rtti_dump.py <exe> > research/private/vtables.txt

Layout (MSVC x64):
  vtable[-1] -> RTTICompleteObjectLocator {u32 signature=1, u32 offset, u32 cdOffset,
                 u32 typeDescRva, u32 classDescRva, u32 selfRva}
  TypeDescriptor { u64 pVFTable, u64 spare, char name[] }   name like ".?AVFoo@ns@@"
Output columns: vtable_rva  this_offset  n_methods  class_name
"""
import struct
import sys
import pefile


def pretty(name):
    # ".?AVFoo@Bar@@" -> "Bar::Foo"; templates/complex names are left mangled.
    if name.startswith(".?A") and name.endswith("@@") and "?" not in name[3:] and "$" not in name:
        parts = name[4:-2].split("@")
        return "::".join(reversed(parts))
    return name


def main():
    pe = pefile.PE(sys.argv[1], fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    size = len(img)

    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    t_lo, t_hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
    ranges = []
    for s in pe.sections:
        if s.Name.rstrip(b"\0") in (b".rdata", b".data", b"_RDATA"):
            ranges.append((s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize))

    def u32(rva):
        return struct.unpack_from("<I", img, rva)[0]

    def u64(rva):
        return struct.unpack_from("<Q", img, rva)[0]

    # 1. find all complete object locators (self-referencing)
    cols = {}
    for lo, hi in ranges:
        for rva in range(lo, hi - 24, 4):
            if img[rva] == 1 and img[rva + 1:rva + 4] == b"\0\0\0" and u32(rva + 20) == rva:
                td = u32(rva + 12)
                if 0 < td < size - 16:
                    end = img.find(b"\0", td + 16, td + 16 + 4096)
                    name = img[td + 16:end].decode(errors="replace")
                    if name.startswith(".?A"):
                        cols[rva] = (u32(rva + 4), name)

    # 2. find vtables: a qword pointing at a COL, followed by code pointers
    targets = {base + c: c for c in cols}
    out = []
    for lo, hi in ranges:
        for rva in range(lo, hi - 8, 8):
            q = u64(rva)
            c = targets.get(q)
            if c is None:
                continue
            vt = rva + 8
            n = 0
            while vt + 8 * n + 8 <= size:
                p = u64(vt + 8 * n) - base
                if not (t_lo <= p < t_hi):
                    break
                n += 1
            off, name = cols[c]
            out.append((vt, off, n, name))

    out.sort()
    print(f"# {len(cols)} type locators, {len(out)} vtables")
    print("# vtable_rva  this_off  methods  class")
    for vt, off, n, name in out:
        print(f"0x{vt:08x}  {off:6d}  {n:5d}  {pretty(name)}")


if __name__ == "__main__":
    main()
