#!/usr/bin/env python3
"""Dump the COM/WinRT interfaces described by a MIDL proxy/stub DLL (e.g.
EmbeddedXvd/.../gameosproxy.dll): interface name, IID, method count, and each
method's parameters decoded from the /Oicf procedure format strings.

The Xbox WinRT classes run outside the title (brokered), so this proxy data is
the best description of their interfaces we have: slot order = proc number,
parameter directions and types (base types, interface pointers with IIDs,
HSTRINGs...). Method NAMES are not in the data.

Usage: python3 -P tools/midl_proxies.py <proxy.dll> [name-filter]
Output describes Microsoft binaries: keep it in research/private/ (Rule 1).
"""
import struct
import sys
import uuid

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pe_re import PE  # noqa: E402

BASE = {0x1: "byte", 0x2: "char", 0x3: "small", 0x4: "usmall", 0x5: "wchar", 0x6: "short", 0x7: "ushort",
        0x8: "long", 0x9: "ulong", 0xa: "float", 0xb: "hyper", 0xc: "double", 0xd: "enum16", 0xe: "enum32",
        0x10: "error_status", 0xb8: "int3264", 0xb9: "uint3264"}


def cstr(p, rva):
    end = p.img.find(b"\0", rva)
    return p.img[rva:end].decode("latin-1")


def is_ptr(p, v):
    return p.base <= v < p.base + len(p.img)


def find_proxy_file_infos(p):
    """ProxyFileInfo: {pProxyVtblList, pStubVtblList, pNamesArray, pDelegatedIIDs, pIIDLookupRtn,
    u16 TableSize, u16 TableVersion, ...}. Find structs whose pNamesArray points at an array of
    pointers to '__x_'/'I...' strings."""
    out = []
    img = p.img
    for off in range(0, len(img) - 0x30, 8):
        a, b, c = struct.unpack_from("<QQQ", img, off)
        if not (is_ptr(p, a) and is_ptr(p, b) and is_ptr(p, c)):
            continue
        size = struct.unpack_from("<H", img, off + 0x28)[0]
        if not (0 < size < 4096):
            continue
        names = []
        ok = True
        for i in range(size):
            v = struct.unpack_from("<Q", img, c - p.base + 8 * i)[0]
            if not is_ptr(p, v):
                ok = False
                break
            s = cstr(p, v - p.base)
            if not s or not (s.startswith("__x_") or s.startswith("I") or s.startswith("__F")):
                ok = False
                break
            names.append(s)
        if ok and names:
            out.append((off, size, a - p.base, b - p.base, names))
    return out


def decode_type(p, fmt_types, off, depth=0):
    """Describe a type at offset `off` in the type format string."""
    if depth > 4:
        return "..."
    t = p.img[fmt_types + off]
    if t == 0x2f:  # FC_IP
        kind = p.img[fmt_types + off + 1]
        if kind == 0x5a:  # FC_CONSTANT_IID
            g = str(uuid.UUID(bytes_le=bytes(p.img[fmt_types + off + 2: fmt_types + off + 18]))).upper()
            return "IFACE{%s}" % g
        return "IFACE(iid_is)"
    if t in (0x11, 0x12, 0x13, 0x14):  # FC_RP, FC_UP, FC_OP, FC_FP
        flags = p.img[fmt_types + off + 1]
        if flags & 0x08:  # FC_SIMPLE_POINTER
            return "%s*" % BASE.get(p.img[fmt_types + off + 2], "0x%x" % p.img[fmt_types + off + 2])
        rel = struct.unpack_from("<h", p.img, fmt_types + off + 2)[0]
        return decode_type(p, fmt_types, off + 2 + rel, depth + 1) + "*"
    if t == 0xb4:  # FC_USER_MARSHAL
        return "user_marshal(HSTRING?)"
    if t in BASE:
        return BASE[t]
    if t in (0x15, 0x1a):  # FC_STRUCT / FC_BOGUS_STRUCT: list the member types
        align, size = p.img[fmt_types + off + 1], struct.unpack_from("<H", p.img, fmt_types + off + 2)[0]
        i = fmt_types + off + (4 if t == 0x15 else 8)
        members = []
        for _ in range(64):
            m = p.img[i]
            if m == 0x5b:  # FC_END
                break
            if m in BASE:
                members.append(BASE[m])
            elif m == 0x4c:  # FC_EMBEDDED_COMPLEX
                rel = struct.unpack_from("<h", p.img, i + 2)[0]
                members.append(decode_type(p, fmt_types, (i + 2 - fmt_types) + rel, depth + 1))
                i += 3
            elif m in (0x39, 0x3a, 0x3b, 0x3c, 0x3d):  # FC_ALIGNM*/FC_STRUCTPAD*
                pass
            else:
                members.append("0x%02x" % m)
            i += 1
        return "%s{%d bytes: %s}" % ("struct" if t == 0x15 else "bogus_struct", size, " ".join(members))
    names = {0x15: "struct", 0x16: "pstruct", 0x17: "cstruct", 0x1a: "bogus_struct", 0x1b: "carray",
             0x1c: "cvarray", 0x1d: "smfarray", 0x21: "bogus_array", 0x22: "c_cstring", 0x25: "c_wstring",
             0x2a: "encap_union", 0x2b: "non_encap_union"}
    return names.get(t, "type0x%02x" % t)


def decode_proc(p, proc, fmt_types):
    """/Oicf procedure header + params. Returns (proc_num, [param descriptions])."""
    i = proc
    handle_type = p.img[i]
    oi_flags = p.img[i + 1]
    i += 2
    if oi_flags & 0x08:  # Oi_HAS_RPCFLAGS
        i += 4
    proc_num, stack = struct.unpack_from("<HH", p.img, i)
    i += 4
    if handle_type == 0:  # explicit handle descriptor
        i += 4 if p.img[i] != 0x3c else 6
    i += 4  # client/server buffer sizes
    oi2_flags = p.img[i]
    nparams = p.img[i + 1]
    i += 2
    if oi2_flags & 0x40:  # has extensions
        i += p.img[i]
    params = []
    for _ in range(nparams):
        attr, stack_off = struct.unpack_from("<HH", p.img, i)
        if attr & 0x40:  # IsBasetype
            ty = BASE.get(p.img[i + 4], "base0x%x" % p.img[i + 4])
        else:
            ty = decode_type(p, fmt_types, struct.unpack_from("<H", p.img, i + 4)[0])
        d = ("ret " if attr & 0x20 else "") + ("in" if attr & 0x8 else "") + ("out" if attr & 0x10 else "")
        params.append("%s %s" % (d or "?", ty))
        i += 6
    return proc_num, params


def main():
    p = PE(sys.argv[1])
    filt = sys.argv[2] if len(sys.argv) > 2 else ""
    for (off, size, proxy_list, stub_list, names) in find_proxy_file_infos(p):
        for idx, name in enumerate(names):
            if filt and filt not in name:
                continue
            stub = struct.unpack_from("<Q", p.img, stub_list + 8 * idx)[0] - p.base
            piid, server_info, count = struct.unpack_from("<QQI", p.img, stub)
            iid = str(uuid.UUID(bytes_le=bytes(p.img[piid - p.base: piid - p.base + 16]))).upper()
            print("%s {%s} %d methods" % (name, iid, count))
            # MIDL_SERVER_INFO: pStubDesc, DispatchTable, ProcString, FmtStringOffset, ...
            stub_desc, _, proc_string, fmt_offsets = struct.unpack_from("<QQQQ", p.img, server_info - p.base)
            # MIDL_STUB_DESC: ... pFormatTypes at +0x28 on x64 (RpcInterfaceInformation, pfnAllocate,
            # pfnFree, IMPLICIT_HANDLE_INFO, apfnNdrRundownRoutines, aGenericBindingRoutinePairs,
            # apfnExprEval, aXmitQuintuple, pFormatTypes...)
            fmt_types = struct.unpack_from("<Q", p.img, stub_desc - p.base + 0x40)[0] - p.base
            for m in range(3, count):
                fo = struct.unpack_from("<H", p.img, fmt_offsets - p.base + 2 * m)[0]
                if fo == 0xffff:
                    print("    %2d  (inherited / not remoted)" % m)
                    continue
                try:
                    num, params = decode_proc(p, proc_string - p.base + fo, fmt_types)
                    print("    %2d  (%s)" % (num, ", ".join(params)))
                except Exception as e:  # keep going on odd procs
                    print("    %2d  decode error: %s" % (m, e))


if __name__ == "__main__":
    main()
