"""Mine the lost Stage-1 kraken.dll for everything a symbol-less binary still tells us.

The DLL has no CodeView entry, so there is no PDB and no function names. What it does
have is every log format string and every ini key, each at a known address - and those
are anchors: a cross-reference to a format string names the function that emitted it.

Usage: python dllmine.py <dll> <outdir>
Writes strings.tsv (addr, section, text), inikeys.txt, formats.tsv (printf-style only).
"""
import os
import re
import sys

import pefile

dll, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)

pe = pefile.PE(dll)
base = pe.OPTIONAL_HEADER.ImageBase

rows = []
for sec in pe.sections:
    data = sec.get_data()
    va = base + sec.VirtualAddress
    name = sec.Name.rstrip(b"\x00").decode("latin1", "replace")
    for m in re.finditer(rb"[\x20-\x7e\xc2\xa7]{8,}", data):
        try:
            s = m.group().decode("utf-8")
        except UnicodeDecodeError:
            continue
        rows.append((va + m.start(), name, s))

with open(os.path.join(outdir, "strings.tsv"), "w", encoding="utf-8") as f:
    for a, n, s in rows:
        f.write("0x%08x\t%s\t%s\n" % (a, n, s))

# printf-style format strings are the ones worth cross-referencing: each marks a log
# site, and the placeholders reveal the shape of the data the lost code carried.
fmt = [(a, n, s) for a, n, s in rows if re.search(r"%[-+ #0]*[\d.*]*(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGaAcspn%]", s)]
with open(os.path.join(outdir, "formats.tsv"), "w", encoding="utf-8") as f:
    for a, n, s in fmt:
        f.write("0x%08x\t%s\t%s\n" % (a, n, s))

# ini keys: short lowercase identifiers that appear alongside the section names the
# config layer uses. Kept loose on purpose - a false positive costs nothing here.
SECTIONS = ("jolt", "jolt_harness", "wheelmodel", "testharness", "diag", "log", "general")
keys = sorted({s for _, _, s in rows if re.fullmatch(r"[a-z][a-z0-9_]{2,40}", s)})
with open(os.path.join(outdir, "inikeys.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(keys))

print("strings      : %d" % len(rows))
print("format strs  : %d" % len(fmt))
print("ini-key cands: %d" % len(keys))
print("sections     : %s" % ", ".join(
    s.Name.rstrip(b"\x00").decode("latin1", "replace") for s in pe.sections))
