"""The one place that knows where the uibookstest game is installed."""
import os
import shutil
import time

_CANDIDATES = (
    os.environ.get("KRAKEN_GAME_DIR"),
    r"C:\Users\etozh\code\HTA_Kraken",
    r"E:\HTA-Kraken",
    r"E:\KrakenWorkspace\target",
    r"E:\KrakenWorkspace",
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

EXE           = os.path.join(GAME_DIR, "hta.exe")
DLL           = os.path.join(GAME_DIR, "kraken.dll")
LOG           = os.path.join(GAME_DIR, "kraken.log")
EXM_LOG       = os.path.join(GAME_DIR, "exmachina.log")  # engine log; dies before "Log ends" on a hard crash
EXCEPTIONS    = os.path.join(GAME_DIR, "exceptions")
INI           = os.path.join(GAME_DIR, "data", "kraken.ini")
PROFILES      = os.path.join(GAME_DIR, "data", "profiles")
BASE_DIR      = os.path.join(GAME_DIR, "data", "uibookstest")  # drop box
WORKDIR       = GAME_DIR
BUILD_DLL     = os.path.join(r"F:\Kraken", "build", "Release", "kraken.dll")


def deploy_dll():
    """Copy the built DLL over the deployed one if it is newer. Returns a status string."""
    if not os.path.isfile(BUILD_DLL):
        return "no build at %s" % BUILD_DLL
    deployed = []
    if not os.path.isfile(DLL) or os.path.getmtime(DLL) < os.path.getmtime(BUILD_DLL):
        shutil.copy2(BUILD_DLL, DLL)
        deployed.append("dll")
    if not deployed:
        return "already current"
    return "deployed %s" % ", ".join(deployed)


def check_dll_current():
    """Raise if the deployed DLL is older than the build - a stale DLL runs the
    previous code and reports a false pass. Cheap check, expensive failure."""
    if not os.path.isfile(BUILD_DLL):
        return
    if not os.path.isfile(DLL):
        raise RuntimeError("no kraken.dll deployed at %s" % DLL)
    build_t, live_t = os.path.getmtime(BUILD_DLL), os.path.getmtime(DLL)
    if live_t < build_t - 1.0:
        raise RuntimeError(
            "ABORT: deployed kraken.dll is STALE (%s) against the build (%s). "
            "Run with --deploy or copy it by hand."
            % (time.strftime("%H:%M:%S", time.localtime(live_t)),
               time.strftime("%H:%M:%S", time.localtime(build_t))))


def exception_snapshot():
    """Current exception-report names + metadata, for crash detection."""
    try:
        return {
            entry.name: (entry.stat().st_mtime_ns, entry.stat().st_size)
            for entry in os.scandir(EXCEPTIONS)
            if entry.is_file()
        }
    except OSError:
        return {}


def new_exception(snapshot):
    """A newly created/updated exception report, or None."""
    current = exception_snapshot()
    changed = [name for name, m in current.items()
               if name not in snapshot or m != snapshot[name]]
    if not changed:
        return None
    changed.sort(key=lambda name: current[name], reverse=True)
    return os.path.join(EXCEPTIONS, changed[0])


def list_saves():
    """All save directory names (by directory name), sorted by mtime, newest first."""
    out = []
    try:
        for profile in os.scandir(PROFILES):
            if not profile.is_dir():
                continue
            saves = os.path.join(profile.path, "saves")
            try:
                entries = list(os.scandir(saves))
            except OSError:
                continue
            for e in entries:
                if e.is_dir():
                    out.append((e.stat().st_mtime, e.name))
    except OSError:
        pass
    out.sort(reverse=True)
    return out


def combined_log_size():
    """Total size of the two logs the game keeps writing while it is alive.

    A hard crash (and some hangs) leave NO file in exceptions/ - the game's
    own report writer doesn't cover every fault type - so log growth is the
    other half of the crash signal. exmachina.log's last line on a clean exit
    is "Log ends on ..."; when it ends abruptly mid "Game loading ...", the
    process is the one that died.
    """
    total = 0
    for p in (LOG, EXM_LOG):
        try:
            total += os.path.getsize(p)
        except OSError:
            pass
    return total


def tail_lines(path, n=8, encodings=("cp1251", "utf-8", "utf-16")):
    try:
        raw = open(path, "rb").read()
    except OSError:
        return []
    for enc in encodings:
        try:
            return raw.decode(enc).splitlines()[-n:]
        except (UnicodeDecodeError, ValueError):
            continue
    return raw.decode("utf-8", "replace").splitlines()[-n:]


if __name__ == "__main__":
    for name in ("GAME_DIR", "EXE", "DLL", "LOG", "EXM_LOG", "INI", "PROFILES", "BASE_DIR", "BUILD_DLL"):
        path = globals()[name]
        mark = "ok " if os.path.exists(path) else "MISSING"
        print("%-10s %-8s %s" % (name, mark, path))
    print("\nsaves (newest first):")
    for m, name in list_saves()[:10]:
        print("  %s  %s" % (time.strftime("%Y-%m-%d %H:%M", time.localtime(m)), name))
