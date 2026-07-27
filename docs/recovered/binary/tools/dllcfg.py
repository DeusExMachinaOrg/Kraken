"""Reconstruct the whole config table out of the symbol-less Stage-1 kraken.dll.

Config::Config() builds each ConfigValue as a fixed instruction pattern - two string
pointers, a default, a dumpable byte, then a min/max pair - and stores it at a known
member offset. That pattern is regular enough to parse straight off the disassembly,
and it was calibrated against a key whose values are known from the source that wrote
it (wm4_chassis_mass_excl_wheels = {1, true, 0, 1}), so the reading is checked rather
than assumed.

Floats and ints share the same 32-bit immediate, and the DLL does not say which is
which, so BOTH readings are printed; the key name and the min/max pair disambiguate.

Usage: python dllcfg.py <dll> <strings.tsv> <start-va> <end-va>
"""
import re
import struct
import sys

import capstone
import pefile

dll, tsv, start_s, end_s = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
START, END = int(start_s, 16), int(end_s, 16)

pe = pefile.PE(dll)
BASE = pe.OPTIONAL_HEADER.ImageBase
TEXT = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
CODE = TEXT.get_data()
TVA = BASE + TEXT.VirtualAddress

STRINGS = {}
for line in open(tsv, encoding="utf-8"):
    a, sec, s = line.rstrip("\n").split("\t")
    STRINGS[int(a, 16)] = s

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
ins = list(md.disasm(CODE[START - TVA:END - TVA], START))

MOVD = re.compile(r"dword ptr \[(ebp - 0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+)$")
MOVB = re.compile(r"byte ptr \[(ebp - 0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+)$")
STORE = re.compile(r"xmmword ptr \[edi \+ (0x[0-9a-f]+)\]|qword ptr \[edi \+ (0x[0-9a-f]+)\]")


def num(t):
    return int(t, 16) if t.startswith("0x") else int(t)


def asfloat(v):
    return struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]


slots, entries, cur = {}, [], {}
for i in ins:
    op = i.op_str
    m = MOVD.search(op)
    if m and i.mnemonic == "mov":
        slots[m.group(1)] = num(m.group(2))
        continue
    m = MOVB.search(op)
    if m and i.mnemonic == "mov":
        slots[m.group(1)] = num(m.group(2))
        continue
    m = STORE.search(op)
    if m and i.mnemonic.startswith("vmov"):
        off = int((m.group(1) or m.group(2)), 16)
        if m.group(1):                                   # 16-byte store = the head half
            cur = {
                "member": off,
                "section": STRINGS.get(slots.get("ebp - 0x24"), hex(slots.get("ebp - 0x24", 0))),
                "key": STRINGS.get(slots.get("ebp - 0x20"), hex(slots.get("ebp - 0x20", 0))),
                "default": slots.get("ebp - 0x1c", 0),
                "dumpable": slots.get("ebp - 0x18", 0),
            }
        else:                                            # 8-byte store = the min/max half
            if cur:
                cur["min"] = slots.get("ebp - 0x14", 0)
                cur["max"] = slots.get("ebp - 0x10", 0)
                entries.append(cur)
                cur = {}

print("recovered %d config entries\n" % len(entries))
print("%-14s %-32s %-22s %-22s %-22s %s" % ("section", "key", "default", "min", "max", "member"))
for e in entries:
    def both(v):
        f = asfloat(v)
        return "%d / %g" % (v, f) if 0 < abs(f) < 1e9 or v == 0 else str(v)
    print("%-14s %-32s %-22s %-22s %-22s +0x%x%s" % (
        e["section"], e["key"], both(e["default"]), both(e.get("min", 0)),
        both(e.get("max", 0)), e["member"], "" if e["dumpable"] else "  [not dumped]"))
