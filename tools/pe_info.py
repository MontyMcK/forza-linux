#!/usr/bin/env python3
"""Print PE facts the loader cares about: sections, TLS, load config, imports.

Usage: python3 -P tools/pe_info.py <exe> [--imports]
Output may contain game-derived data: redirect to research/private/ if saving.
"""
import sys
import pefile


def main():
    path = sys.argv[1]
    show_imports = "--imports" in sys.argv
    pe = pefile.PE(path, fast_load=False)
    oh = pe.OPTIONAL_HEADER
    base = oh.ImageBase
    print(f"ImageBase        0x{base:x}")
    print(f"SizeOfImage      0x{oh.SizeOfImage:x}")
    print(f"EntryPoint RVA   0x{oh.AddressOfEntryPoint:x}")
    print(f"StackReserve     0x{oh.SizeOfStackReserve:x}  commit 0x{oh.SizeOfStackCommit:x}")
    print(f"HeapReserve      0x{oh.SizeOfHeapReserve:x}")
    print(f"DllCharacteristics 0x{oh.DllCharacteristics:x}")
    print()
    print("Sections:")
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        print(f"  {name:10s} rva=0x{s.VirtualAddress:08x} vsize=0x{s.Misc_VirtualSize:08x} "
              f"raw=0x{s.PointerToRawData:08x} rawsize=0x{s.SizeOfRawData:08x} chr=0x{s.Characteristics:08x}")
    print()
    for i, d in enumerate(oh.DATA_DIRECTORY):
        if d.Size:
            print(f"  DataDir[{i:2d}] {d.name:40s} rva=0x{d.VirtualAddress:08x} size=0x{d.Size:x}")
    print()
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        t = pe.DIRECTORY_ENTRY_TLS.struct
        print("TLS directory:")
        print(f"  StartAddressOfRawData 0x{t.StartAddressOfRawData:x} (rva 0x{t.StartAddressOfRawData - base:x})")
        print(f"  EndAddressOfRawData   0x{t.EndAddressOfRawData:x}  (template size 0x{t.EndAddressOfRawData - t.StartAddressOfRawData:x})")
        print(f"  AddressOfIndex        0x{t.AddressOfIndex:x} (rva 0x{t.AddressOfIndex - base:x})")
        print(f"  AddressOfCallBacks    0x{t.AddressOfCallBacks:x}")
        print(f"  SizeOfZeroFill        0x{t.SizeOfZeroFill:x}")
        if t.AddressOfCallBacks:
            rva = t.AddressOfCallBacks - base
            n = 0
            while True:
                cb = pe.get_qword_at_rva(rva + 8 * n)
                if not cb:
                    break
                print(f"    callback[{n}] rva 0x{cb - base:x}")
                n += 1
        print()
    if hasattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG"):
        lc = pe.DIRECTORY_ENTRY_LOAD_CONFIG.struct
        print("Load config:")
        for f in ("Size", "SecurityCookie", "GuardCFCheckFunctionPointer", "GuardFlags"):
            if hasattr(lc, f):
                v = getattr(lc, f)
                extra = f" (rva 0x{v - base:x})" if f == "SecurityCookie" and v else ""
                print(f"  {f:28s} 0x{v:x}{extra}")
        print()
    if hasattr(pe, "DIRECTORY_ENTRY_EXCEPTION"):
        print(f"RUNTIME_FUNCTION entries: {len(pe.DIRECTORY_ENTRY_EXCEPTION)}")
    if show_imports and hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            print(f"\n[{imp.dll.decode()}]")
            for s in imp.imports:
                name = s.name.decode() if s.name else f"#{s.ordinal}"
                print(f"  iat_rva=0x{s.address - base:08x} {name}")


if __name__ == "__main__":
    main()
