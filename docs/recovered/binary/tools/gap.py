"""Inventory what the lost DLL emitted that the current branch cannot.

Every printf format string in the last built DLL corresponds to a log site in the lost
source. If the branch's source does not contain that string, the code that emitted it
is gone. That turns 187 strings into an exact gap list, rather than discovering the
missing subsystems one compile error at a time.

Matching is on a distinctive literal slice rather than the whole string, because the
branch may carry an older wording of the same line - and a near-match is exactly the
interesting case, so it is reported separately from a clean miss.

Usage: python gap.py <formats.tsv> <source-root...>
"""
import os
import re
import sys

fmt_path, roots = sys.argv[1], sys.argv[2:]

src = []
for root in roots:
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ("build", ".git", "extern", "__pycache__")]
        for f in files:
            if f.endswith((".cpp", ".hpp", ".h", ".c")):
                p = os.path.join(dirpath, f)
                try:
                    src.append((p, open(p, encoding="utf-8", errors="replace").read()))
                except OSError:
                    pass
blob = "\n".join(t for _, t in src)

rows = [l.rstrip("\n").split("\t") for l in open(fmt_path, encoding="utf-8")]

def slice_of(s):
    """Longest run of literal text without a format placeholder - the part that would
    survive a reworded line, and the part worth matching on."""
    parts = re.split(r"%[-+ #0]*[\d.*]*(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGaAcsp]", s)
    parts = [p.strip() for p in parts if len(p.strip()) >= 12]
    return max(parts, key=len) if parts else None

present, missing, weak = [], [], []
for addr, sec, s in rows:
    key = slice_of(s)
    if key is None:
        weak.append((addr, s))
        continue
    (present if key in blob else missing).append((addr, s, key))

print("format strings      : %d" % len(rows))
print("still in the branch : %d" % len(present))
print("GONE from the branch: %d" % len(missing))
print("unmatchable (short) : %d\n" % len(weak))

def tag(s):
    m = re.search(r"docs §([0-9]+(?:\.[0-9]+)?)", s)
    return "docs §" + m.group(1) if m else "(untagged)"

groups = {}
for addr, s, key in missing:
    groups.setdefault(tag(s), []).append(s)
for g in sorted(groups, key=lambda t: (t == "(untagged)", t)):
    print("== %s  (%d line%s)" % (g, len(groups[g]), "" if len(groups[g]) == 1 else "s"))
    for s in groups[g]:
        print("   " + (s[:150] + ("..." if len(s) > 150 else "")))
