#!/usr/bin/env python3
"""Names the "native self+N" frames of a PS5 thread dump. crash.cpp prints our
own code (the title is one static binary, so dladdr can't name it) as byte
offsets from fl::loader::note_present; this adds the function and source line
from the linked ELF the title was made from.

  tools/ps5/symbolize.py LOG [ELF]     ELF defaults to build-ps5/link/llvm-pie.elf
"""
import os
import re
import subprocess
import sys

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
log = sys.argv[1]
elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "build-ps5", "link", "llvm-pie.elf")

nm = subprocess.run(["llvm-nm", "-C", elf], capture_output=True, text=True, check=True).stdout
anchor = next(int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(" fl::loader::note_present()"))

lines = open(log, errors="replace").read().splitlines()
frame = re.compile(r"native self([+-]\d+)")
addrs = sorted({anchor + int(m.group(1)) - 1 for l in lines for m in [frame.search(l)] if m})  # -1: the call
names = {}
if addrs:
    out = subprocess.run(["llvm-symbolizer", "--obj=" + elf, "-C", "--functions=short", "--no-inlines",
                          "--relative-address"], input="\n".join(hex(a) for a in addrs), capture_output=True,
                         text=True, check=True).stdout
    for a, block in zip(addrs, out.strip().split("\n\n")):
        fn, src = (block.splitlines() + ["", ""])[:2]
        names[a] = "%s (%s)" % (fn, os.path.relpath(src, root) if src.startswith("/") else src)
for l in lines:
    m = frame.search(l)
    print(l + ("  <- " + names[anchor + int(m.group(1)) - 1] if m else ""))
