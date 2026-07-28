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
import time

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

# docs §109.1: the built DLL and the deployed one. `install` is NOT the deploy step - CMake's
# install() target writes to C:\Program Files\kraken and fails without admin; deployment has always
# been a manual copy, which is exactly why it can be forgotten.
BUILD_DLL = os.path.join(r"F:\Kraken", "build", "Release", "kraken.dll")


def deploy_dll():
    """Copy the built DLL over the deployed one if it is newer. Returns a status string."""
    import shutil
    if not os.path.isfile(BUILD_DLL):
        return "no build at %s" % BUILD_DLL
    if os.path.isfile(DLL) and os.path.getmtime(DLL) >= os.path.getmtime(BUILD_DLL):
        return "already current"
    shutil.copy2(BUILD_DLL, DLL)
    return "deployed %s" % time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(DLL)))


def check_dll_current():
    """Raise if the deployed DLL is older than the build. See docs §109.1.

    A stale DLL does not fail loudly - it runs the PREVIOUS code, silently ignores any config key
    added since, and reports every arm as identical. That is a false null, and this project has
    already published one of those (§107.7). Cheap check, expensive failure.
    """
    if not os.path.isfile(BUILD_DLL):
        return  # nothing built here; not our business to judge
    if not os.path.isfile(DLL):
        raise RuntimeError("no kraken.dll deployed at %s" % DLL)
    build_t, live_t = os.path.getmtime(BUILD_DLL), os.path.getmtime(DLL)
    if live_t < build_t - 1.0:
        raise RuntimeError(
            "ABORT: deployed kraken.dll is STALE (%s) against the build (%s). The run would "
            "execute the previous code and report every arm as identical - a false null. Run "
            "`python -c \"import gamedir; print(gamedir.deploy_dll())\"` or copy it by hand."
            % (time.strftime("%H:%M:%S", time.localtime(live_t)),
               time.strftime("%H:%M:%S", time.localtime(build_t))))


if __name__ == "__main__":
    for name in ("GAME_DIR", "EXE", "PDB", "DLL", "LOG", "INI", "PROFILES", "BASE_DIR"):
        path = globals()[name]
        mark = "ok " if os.path.exists(path) else "MISSING"
        print("%-8s %-8s %s" % (name, mark, path))
