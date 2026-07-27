"""String-anchored recovery from the symbol-less Stage-1 kraken.dll.

There is no PDB, so no function names and no boundaries. What there is: every log
format string sits at a known address, and x86 references it as a 4-byte absolute
immediate. Scanning .text for that immediate finds the log site; walking backwards to
the nearest function prologue recovers the emitting function's body.

Usage:
  python dllx.py <dll> xref <hex-va> [...]        - find code referencing these addresses
  python dllx.py <dll> func <hex-va-in-text>      - disassemble the containing function
  python dllx.py <dll> at <hex-va> <n>            - disassemble n instructions from here
"""
import re
import struct
import sys

import capstone
import pefile

dll = sys.argv[1]
cmd = sys.argv[2]

pe = pefile.PE(dll)
BASE = pe.OPTIONAL_HEADER.ImageBase
TEXT = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
CODE = TEXT.get_data()
TVA = BASE + TEXT.VirtualAddress
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = False

# A function prologue in this build is `push ebp; mov ebp,esp` (55 8B EC) or a plain
# `sub esp, imm` frame (83 EC / 81 EC) preceded by int3 padding. Scanning back for the
# nearest of those, bounded, is enough to bracket a body for reading.
PROLOGUES = (b"\x55\x8b\xec", b"\x83\xec", b"\x81\xec", b"\x53\x56\x57", b"\x56\x57")


def refs(target):
    """Every offset in .text holding target as a 4-byte little-endian immediate."""
    pat = struct.pack("<I", target)
    return [m.start() for m in re.finditer(re.escape(pat), CODE)]


def func_start(off):
    """Walk back to a plausible prologue: the first byte after an int3 run, or a
    recognised frame setup. Bounded at 4 KB - past that the guess is worthless."""
    lo = max(0, off - 4096)
    best = None
    for i in range(off, lo, -1):
        if CODE[i - 1] == 0xCC and CODE[i] != 0xCC:      # first byte after padding
            return i
        for p in PROLOGUES:
            if CODE[i:i + len(p)] == p and best is None:
                best = i
    return best if best is not None else lo


def show(off, count):
    for ins in MD.disasm(CODE[off:off + count * 16], TVA + off):
        print("0x%08x  %-9s %s" % (ins.address, ins.mnemonic, ins.op_str))
        count -= 1
        if count <= 0 or ins.mnemonic == "ret":
            break


if cmd == "xref":
    for a in sys.argv[3:]:
        t = int(a, 16)
        hits = refs(t)
        print("=== 0x%08x : %d ref(s)" % (t, len(hits)))
        for h in hits:
            print("    at 0x%08x   (function starts ~0x%08x)" % (TVA + h, TVA + func_start(h)))
elif cmd == "func":
    off = int(sys.argv[3], 16) - TVA
    st = func_start(off)
    print("=== function ~0x%08x (anchor 0x%08x)" % (TVA + st, TVA + off))
    show(st, 400)
elif cmd == "at":
    show(int(sys.argv[3], 16) - TVA, int(sys.argv[4]))
