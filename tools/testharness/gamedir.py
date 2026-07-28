"""The one place that knows where the game is installed.

Every recovered harness script hard-coded `F:\\HTA_Kraken`, in twelve separate string
literals across five files. That drive is gone and the install now lives on C:, so all
twelve were wrong at once - which is the argument for this module existing rather than
for another round of find-and-replace.

Override with the KRAKEN_GAME_DIR environment variable; otherwise the first of the
known locations that actually contains hta.exe wins, so a machine that still has the
old layout keeps working without edits.
"""
import os

_CANDIDATES = (
    os.environ.get("KRAKEN_GAME_DIR"),
    r"C:\Users\etozh\code\HTA_Kraken",
    r"F:\HTA_Kraken",
    r"E:\HTA-Kraken",
)


def _resolve():
    tried = []
    for c in _CANDIDATES:
        if not c:
            continue
        tried.append(c)
        if os.path.isfile(os.path.join(c, "hta.exe")):
            return c
    raise RuntimeError(
        "no game directory found (looked for hta.exe in: %s). "
        "Set KRAKEN_GAME_DIR to the install root." % ", ".join(tried))


GAME_DIR = _resolve()

EXE      = os.path.join(GAME_DIR, "hta.exe")
PDB      = os.path.join(GAME_DIR, "game.pdb")
DLL      = os.path.join(GAME_DIR, "kraken.dll")
LOG      = os.path.join(GAME_DIR, "kraken.log")
DATA     = os.path.join(GAME_DIR, "data")
INI      = os.path.join(DATA, "kraken.ini")
PROFILES = os.path.join(DATA, "profiles")
BASE_DIR = os.path.join(DATA, "kraken_testharness")   # the scenario/trigger drop box
WORKDIR  = GAME_DIR

if __name__ == "__main__":
    for name in ("GAME_DIR", "EXE", "PDB", "DLL", "LOG", "INI", "PROFILES", "BASE_DIR"):
        path = globals()[name]
        mark = "ok " if os.path.exists(path) else "MISSING"
        print("%-8s %-8s %s" % (name, mark, path))
