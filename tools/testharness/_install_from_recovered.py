"""Install the recovered harness scripts, repointed at the new game directory.

The recovered copies hard-code F:\\HTA_Kraken in twelve literals. Rather than editing
twelve strings, each becomes a reference to tools/testharness/gamedir.py, so the next
move of the install is one edit rather than twelve.

Deliberately a script rather than a manual pass: it prints exactly which literals it
rewrote per file, so the repoint is auditable instead of being taken on trust.
"""
import os
import re
import shutil

SRC = r"F:\Kraken\docs\recovered\harness"
DST = r"F:\Kraken\tools\testharness"

# assignment-target -> the gamedir attribute it becomes
SUBS = {
    "LOG": "gamedir.LOG",
    "EXE": "gamedir.EXE",
    "INI": "gamedir.INI",
    "WORKDIR": "gamedir.WORKDIR",
    "BASE_DIR": "gamedir.BASE_DIR",
    "PROFILES": "gamedir.PROFILES",
}

LOADER = (
    "# Paths come from gamedir.py - the single place that knows where the game is installed.\n"
    "# The recovered copy of this file hard-coded F:\\HTA_Kraken, which no longer exists.\n"
    "import os as _os, sys as _sys\n"
    "_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))\n"
    "import gamedir\n"
)

for name in sorted(os.listdir(SRC)):
    if not name.endswith(".py"):
        continue
    text = open(os.path.join(SRC, name), encoding="utf-8").read()
    changed = []
    for var, repl in SUBS.items():
        # `VAR = r"F:\..."` or `VAR = os.environ.get("X", r"F:\...")`
        pat = re.compile(r'^(%s\s*=\s*)(r?"[^"]*HTA[-_]Kraken[^"]*")' % var, re.M)
        if pat.search(text):
            text = pat.sub(lambda m: m.group(1) + repl, text)
            changed.append(var)
        pat2 = re.compile(r'^(%s\s*=\s*os\.environ\.get\("[^"]+",\s*)(r?"[^"]*HTA[-_]Kraken[^"]*")(\))' % var, re.M)
        if pat2.search(text):
            text = pat2.sub(lambda m: m.group(1) + repl + m.group(3), text)
            changed.append(var + "(env)")

    if changed:
        # Insert the loader after the last top-level import so gamedir is available.
        lines = text.split("\n")
        last_import = 0
        for i, l in enumerate(lines[:80]):
            if re.match(r"^(import |from )\w", l):
                last_import = i
        lines.insert(last_import + 1, "\n" + LOADER)
        text = "\n".join(lines)

    open(os.path.join(DST, name), "w", encoding="utf-8", newline="\n").write(text)
    print("%-24s %s" % (name, ", ".join(changed) if changed else "(no paths)"))
