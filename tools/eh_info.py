# Print the exception handler of the functions containing the given RVAs: for
# __CxxFrameHandler3 the FuncInfo, the state at that RVA and every try block's
# catches; for __C_specific_handler the scope table. Answers "would anything
# catch an exception thrown through here?" for a backtrace.
#   python3 -P tools/eh_info.py <pe file> <rva> [<rva>...]
import sys, struct, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
data = pe.get_memory_mapped_image()
u32 = lambda r: struct.unpack_from('<I', data, r)[0]
i32 = lambda r: struct.unpack_from('<i', data, r)[0]
imports = {}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports:
        imports[i.address - pe.OPTIONAL_HEADER.ImageBase] = (i.name or b'?').decode()
fns = [(x.struct.BeginAddress, x.struct.EndAddress, x.struct.UnwindData) for x in pe.DIRECTORY_ENTRY_EXCEPTION]
def handler_name(h):
    # handler is a thunk: jmp [rip+x]
    if data[h] == 0xff and data[h+1] == 0x25:
        t = h + 6 + i32(h+2); return imports.get(t, hex(t))
    if data[h] == 0xe9:
        return handler_name(h + 5 + i32(h+1))
    return hex(h)
for a in sys.argv[2:]:
    rva = int(a, 16)
    f = [x for x in fns if x[0] <= rva < x[1]]
    if not f: print(a, 'no pdata'); continue
    b, e, uw = f[0]
    # follow chained unwind
    while True:
        ver = data[uw]; flags = ver >> 3; cnt = data[uw+2]
        tail = uw + 4 + 2 * ((cnt + 1) & ~1)
        if flags & 4:  # chained
            b, e, uw = struct.unpack_from('<III', data, tail); continue
        break
    print(f'{a}: func 0x{b:x}-0x{e:x} flags {flags}', end='')
    if not flags & 3: print(' no handler'); continue
    h = u32(tail); hd = tail + 4; hn = handler_name(h)
    print(f' handler {hn}')
    if '__CxxFrameHandler' in hn:
        fi = u32(hd)
        magic, maxstate, unwmap, ntry, trymap = struct.unpack_from('<IiIII', data, fi)
        print(f'   FuncInfo 0x{fi:x} magic {magic:x} maxState {maxstate} tryBlocks {ntry}')
        # ip2state
        nip, ipmap = struct.unpack_from('<II', data, fi + 20)
        st = None
        for k in range(nip):
            ip, s = struct.unpack_from('<Ii', data, ipmap + 8*k)
            if ip <= rva: st = s
        print(f'   state at 0x{rva:x}: {st}')
        for t in range(ntry):
            lo, hi, catchhigh, ncatch, hmap = struct.unpack_from('<iiiiI', data, trymap + 20*t)
            cs = []
            for c in range(ncatch):
                adj, tdesc, disp, hand, frame = struct.unpack_from('<IIiII', data, hmap + 20*c)
                name = data[tdesc+16:tdesc+80].split(b'\0')[0].decode(errors='replace') if tdesc else '...'
                cs.append(f'{name}@0x{hand:x}')
            print(f'   try [{lo},{hi}] catches {cs}')
    elif 'C_specific' in hn:
        n = u32(hd)
        for k in range(n):
            s, en, hf, tg = struct.unpack_from('<IIII', data, hd + 4 + 16*k)
            print(f'   scope 0x{s:x}-0x{en:x} filter 0x{hf:x} target 0x{tg:x}')
