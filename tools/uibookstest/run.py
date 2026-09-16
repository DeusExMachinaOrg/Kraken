"""End-to-end smoke test for the uibooks feature (Ex Machina Books tab).

Launches the game, loads a save, opens the journal's Books tab through the
patched path (JournalWnd::AddBook -> BooksWnd selection notification ->
patched TextBoxWnd::SetText), a synthetic 3-page book with bold/italic spans,
and verifies the game does not crash by watching the exceptions/ folder plus
the drop-box result the in-game module writes.

Usage:
    python run.py                     # newest save
    python run.py --save 00000032     # a specific save name
    python run.py --list-saves
    python run.py --deploy            # copy the fresh build over the deployed DLL
    python run.py --trigger openpages # run the live per-book pages-mode variant
"""
import argparse
import ctypes
import io
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import datetime, timedelta

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedir

_TS_RE = re.compile(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)")


def _line_ts(line):
    m = _TS_RE.search(line)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return None


def read_log_lines():
    try:
        with io.open(gamedir.LOG, encoding="utf-8", errors="replace") as f:
            return [l.rstrip() for l in f.readlines()]
    except OSError:
        return []


_user32 = None


def _u32():
    global _user32
    if _user32 is None:
        _user32 = ctypes.WinDLL("user32", use_last_error=True)
    return _user32


def find_game_hwnd(pid):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _cb(hwnd, _):
        wp = wintypes.DWORD()
        _u32().GetWindowThreadProcessId(hwnd, ctypes.byref(wp))
        if wp.value == pid:
            found.append(hwnd)
        return True

    _u32().EnumWindows(_cb, 0)
    return found[0] if found else None


def wake_game(hwnd):
    """Keep the game's message-pumped main loop ticking while we wait.

    The game's main loop only spins while its thread queue holds a message
    (observed: an idle in-world game did not tick the ProcessAllEvents hook
    for 20+ minutes; the first user click after that woke it). A posted
    WM_NULL is an inert message whose only job is to wake the queue so the
    hook - and with it the load_save / trigger drop-box checks - runs in a
    timely manner.
    """
    if hwnd:
        _u32().PostMessageW(hwnd, 0x0000, 0, 0)


def kill_game():
    subprocess.run(["taskkill", "/F", "/IM", "hta.exe"], capture_output=True)
    time.sleep(6.0)


def clear_dropbox():
    os.makedirs(gamedir.BASE_DIR, exist_ok=True)
    for name in ("load_save.txt", "trigger.txt"):
        with open(os.path.join(gamedir.BASE_DIR, name), "w") as f:
            f.write("")
    for name in os.listdir(gamedir.BASE_DIR):
        if name.startswith("output_"):
            try:
                os.remove(os.path.join(gamedir.BASE_DIR, name))
            except OSError:
                pass


def cleanup_dropbox():
    """Remove the drop-box files so the next boot starts clean.

    A stale trigger.txt is consumed on the very first hook tick of the next
    launch - before any world exists - and would turn the next run into a
    false failure. Called from main's finally, so every exit path cleans up.
    """
    removed = []
    try:
        names = os.listdir(gamedir.BASE_DIR)
    except OSError:
        names = []
    for name in names:
        if name in ("load_save.txt", "trigger.txt") or name.startswith("output_"):
            try:
                os.remove(os.path.join(gamedir.BASE_DIR, name))
                removed.append(name)
            except OSError:
                pass
    return removed


def ensure_config(uibooks_enabled):
    """Make sure [uibookstest] enabled=1 (and optionally [uibooks]) in kraken.ini."""
    changed = []
    try:
        with open(gamedir.INI, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        text = ""
    lines = text.splitlines() if text else []

    def set_flag(section, key, value, lines):
        in_section = False
        for i, line in enumerate(lines):
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                in_section = (s[1:-1].lower() == section.lower())
                continue
            if in_section and s.lower().startswith(key + "="):
                if s.split("=", 1)[1].strip() != value:
                    lines[i] = "%s=%s" % (key, value)
                    return True
                return False
        if not in_section:
            # section itself missing: append it
            lines.append("[" + section + "]")
            lines.append("%s=%s" % (key, value))
            return True
        # section present but key missing: find its end and insert after it
        in_section = False
        insert_at = len(lines)
        for i, line in enumerate(lines):
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                if in_section:
                    insert_at = i
                    break
                in_section = (s[1:-1].lower() == section.lower())
        lines.insert(insert_at, "%s=%s" % (key, value))
        return True

    if set_flag("uibookstest", "enabled", "1", lines):
        changed.append("[uibookstest] enabled=1")
    if set_flag("uibooks", "enabled", uibooks_enabled, lines):
        changed.append("[uibooks] enabled=%s" % uibooks_enabled)

    if changed:
        with open(gamedir.INI, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print("  ini updated: %s" % ", ".join(changed), flush=True)
    else:
        print("  ini already has [uibookstest] enabled=1 and [uibooks] enabled=1", flush=True)


def log_since(since):
    """Log lines stamped at (or after) `since`. Whole-file, timestamp-anchored."""
    floor = since - timedelta(seconds=2)
    out = []
    for line in read_log_lines():
        ts = _line_ts(line)
        if ts is None or ts >= floor:
            out.append(line)
    return out


def wait_for(process, marker, timeout, exception_baseline, label, since, wedged_sec, waker=None):
    """Wait for a log line containing `marker` stamped at/after `since`.

    The game truncates kraken.log at startup, so a line-count anchor taken
    before the launch silently goes wrong (every line ends up 'before' the
    anchor). Anchoring on each line's own timestamp is safe for both
    truncate-and-rewrite and append behavior.

    The game can also DIE without the exceptions/ writer ever producing a
    report (observed: hard fault mid "Game loading ...", no dmp, no log).
    A live process whose two logs stop growing for `wedged_sec` is a wedged
    (i.e. crashed) game, so that is reported as a failure too.

    If `waker` is given it is called once per poll (the message keep-alive,
    see wake_game) so the message-driven main loop keeps ticking while we wait.
    """
    deadline = time.time() + timeout
    start = deadline - timeout
    last_beat = start
    floor = since - timedelta(seconds=2)
    last_size = gamedir.combined_log_size()
    last_change = time.time()
    while time.time() < deadline:
        exc = gamedir.new_exception(exception_baseline)
        if exc is not None:
            return False, exc
        code = process.poll()
        if code is not None:
            return False, "exited:%s" % code
        for line in read_log_lines():
            if marker not in line:
                continue
            ts = _line_ts(line)
            if ts is None or ts >= floor:
                return True, None
        size = gamedir.combined_log_size()
        if size != last_size:
            last_size, last_change = size, time.time()
        elif time.time() - last_change >= wedged_sec:
            return False, "game wedged/crashed (logs silent %.0fs)" % (time.time() - last_change)
        if time.time() - last_beat >= 5.0:
            last_beat = time.time()
            print("  [%s] waiting for %r (%.0fs/%.0fs) - game is up, do not close it"
                  % (label, marker[:44], time.time() - start, timeout), flush=True)
        if waker:
            waker()
        time.sleep(0.5)
    return False, "timeout"


def wait_for_world(process, marker_bytes, timeout, exception_baseline, anchor_size, wedged_sec, waker=None):
    """Wait for the engine's load-complete line to appear in exmachina.log.

    exmachina.log is NOT truncated at game startup - one file spans several
    process lifetimes (each run appends under a fresh "Log begins" banner) -
    so a plain substring match would light up on a line from a previous run.
    The driver therefore anchors on the file size recorded before the load
    was requested and only searches bytes appended after that; if the file
    is ever truncated while we wait, the anchor resets to the whole file.

    The marker is b"Game loaded:" (view32_loadsave.cpp) - the engine's
    load-complete line for BOTH menu loads and in-world saves-to-saves
    reloads (observed: 3.4 s and 1 s after "LoadSavedGame returned 1").
    It is the world-ready signal the Books tab needs before the trigger.
    """
    deadline = time.time() + timeout
    start = deadline - timeout
    pos = anchor_size
    last_beat = start
    last_size = gamedir.combined_log_size()
    last_change = time.time()
    while time.time() < deadline:
        exc = gamedir.new_exception(exception_baseline)
        if exc is not None:
            return False, exc
        if process.poll() is not None:
            return False, "exited:%s" % process.poll()
        try:
            with open(gamedir.EXM_LOG, "rb") as f:
                f.seek(0, 2)
                fsize = f.tell()
                if fsize < pos:
                    pos = 0  # truncated under us - re-anchor at the start
                f.seek(pos)
                chunk = f.read()
        except OSError:
            chunk = b""
        if marker_bytes in chunk:
            return True, None
        pos += len(chunk)
        size = gamedir.combined_log_size()
        if size != last_size:
            last_size, last_change = size, time.time()
        elif time.time() - last_change >= wedged_sec:
            return False, "game wedged/crashed (logs silent %.0fs)" % (time.time() - last_change)
        if time.time() - last_beat >= 10.0:
            last_beat = time.time()
            print("  [world] waiting for the engine's load-complete line (%.0fs/%.0fs) - game is up, do not close it"
                  % (time.time() - start, timeout), flush=True)
        if waker:
            waker()
        time.sleep(0.5)
    return False, "timeout"


def crash_evidence(context):
    """Print where the game actually died: process state, exceptions, log tails."""
    print("  --- %s: evidence ---" % context, flush=True)
    try:
        import glob as _g
        candidates = sorted(_g.glob(os.path.join(gamedir.EXCEPTIONS, "hta.exe*")), key=os.path.getmtime)
        if candidates:
            p = candidates[-1]
            print("   newest exception report: %s (%s)"
                  % (os.path.basename(p),
                     time.strftime("%m-%d %H:%M:%S", time.localtime(os.path.getmtime(p)))))
        else:
            print("   no exception reports at all in %s" % gamedir.EXCEPTIONS)
    except OSError:
        print("   cannot scan %s" % gamedir.EXCEPTIONS)
    print("   --- kraken.log tail ---")
    for l in gamedir.tail_lines(gamedir.LOG, 4):
        print("    %s" % l[-150:])
    print("   --- exmachina.log tail (engine) ---")
    for l in gamedir.tail_lines(gamedir.EXM_LOG, 6):
        print("    %s" % l[-150:])


def main():
    # the game's log carries non-UTF8 codepage chars; keep the driver alive on any console
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", default="latest", help="save directory name, or 'latest'")
    ap.add_argument("--list-saves", action="store_true")
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds to wait for the open-books trigger")
    ap.add_argument("--load-timeout", type=float, default=20.0)
    ap.add_argument("--world-timeout", type=float, default=150.0,
                    help="seconds to wait for the engine's world-ready ('Game loaded:') line after the load is accepted")
    ap.add_argument("--launch-timeout", type=float, default=60.0)
    ap.add_argument("--deploy", action="store_true", help="copy the built DLL before the run")
    ap.add_argument("--uibooks", choices=("0", "1"), default="1",
                    help="A/B switch: 0 = run the load-without-features control (no vtable patching)")
    ap.add_argument("--attempts", type=int, default=1, help="game launch retries")
    ap.add_argument("--wedged", type=float, default=30.0,
                    help="seconds of total log silence before a live process counts as crashed (the exceptions/ writer misses some faults)")
    ap.add_argument("--keep", action="store_true",
                    help="leave the game running after a PASS so the results can be inspected manually")
    ap.add_argument("--trigger", choices=("open1", "openpages", "openauto", "openpages2"), default="open1",
                    help="book test variant: open1 = scroll mode, openpages = per-book pages mode, openauto = one long scroll page, openpages2 = second auto-pages book")
    args = ap.parse_args()

    saves = gamedir.list_saves()
    if args.list_saves:
        if not saves:
            print("no saves under %s" % gamedir.PROFILES)
            return 1
        for m, name in saves:
            print("%s  %s" % (time.strftime("%Y-%m-%d %H:%M", time.localtime(m)), name))
        return 0
    if not saves:
        print("FATAL: no saves under %s" % gamedir.PROFILES)
        return 1
    save = saves[0][1] if args.save == "latest" else args.save
    if not any(n == save for _, n in saves):
        print("FATAL: save '%s' not found (see --list-saves)" % save)
        return 1

    print("game dir : %s" % gamedir.GAME_DIR, flush=True)
    print("save     : %s" % save, flush=True)

    if args.deploy:
        print("deploy   : %s" % gamedir.deploy_dll(), flush=True)
    else:
        gamedir.check_dll_current()

    ensure_config(args.uibooks)
    clear_dropbox()

    exception_baseline = gamedir.exception_snapshot()

    try:
        return _execute(args, save, exception_baseline)
    finally:
        removed = cleanup_dropbox()
        if removed:
            print("drop box : cleaned %s after the run (next boot starts clean)"
                  % ", ".join(removed), flush=True)


def _execute(args, save, exception_baseline):
    # ---- launch --------------------------------------------------------------------------
    process = None
    run_since = None
    launch_ok = False
    for attempt in range(1, args.attempts + 1):
        kill_game()
        exception_baseline = gamedir.exception_snapshot()
        run_since = datetime.now()
        process = subprocess.Popen([gamedir.EXE], cwd=gamedir.WORKDIR, close_fds=True)
        print("launch %d/%d: starting hta.exe, waiting for the ProcessAllEvents hook line"
              % (attempt, args.attempts), flush=True)
        found, bad = wait_for(process, "hook installed", args.launch_timeout,
                              exception_baseline, "launch", run_since, args.wedged)
        if found:
            launch_ok = True
            break
        print("  launch attempt %d failed: %s" % (attempt, bad), flush=True)
        crash_evidence("launch failure")
        try:
            process.kill()
            process.wait(timeout=5)
        except Exception:
            pass
        process = None
    if not launch_ok:
        kill_game()
        print("RESULT: FAIL (game did not reach the hook within %ds x %d attempts)"
              % (args.launch_timeout, args.attempts))
        return 1

    hwnd = find_game_hwnd(process.pid)
    if hwnd:
        print("keepalive: game window 0x%08X found - posting WM_NULL ~2/s to keep the"
              " message-driven main loop ticking while we wait" % hwnd, flush=True)
    else:
        print("keepalive: WARNING - no top-level window found for pid %d; while the game"
              " idles the hook may not tick until you click in the game once"
              % process.pid, flush=True)
    waker = (lambda: wake_game(hwnd))

    # ---- load the save ---------------------------------------------------------------------
    print("load     : loading save '%s'" % save, flush=True)
    load_since = datetime.now()
    try:
        exm_anchor = os.path.getsize(gamedir.EXM_LOG)
    except OSError:
        exm_anchor = 0
    with open(os.path.join(gamedir.BASE_DIR, "load_save.txt"), "w") as f:
        f.write("%s#1\n" % save)
    found, bad = wait_for(process, "load_save: LoadSavedGame returned",
                          args.load_timeout, exception_baseline, "load", load_since,
                          args.wedged, waker)
    if not found:
        crash_evidence("save load failure")
        kill_game()
        print("RESULT: FAIL (%s while loading save '%s')" % (bad, save))
        return 1
    tail = [l for l in log_since(load_since) if "load_save: LoadSavedGame returned" in l]
    last = tail[-1] if tail else ""
    if "returned 0" in last or "returned 1" not in last:
        crash_evidence("save load returned non-1")
        kill_game()
        print("RESULT: FAIL (LoadSavedGame did not return 1; last line: %s)" % last)
        return 1
    print("world    : waiting for the engine's 'Game loaded:' line (up to %.0fs)"
          % args.world_timeout, flush=True)
    wfound, wbad = wait_for_world(process, b"Game loaded:", args.world_timeout,
                                  exception_baseline, exm_anchor, args.wedged, waker)
    if wfound:
        print("  world : engine reports the save loaded - settling 4s", flush=True)
    else:
        print("  world : WARNING - no engine load-complete line seen (%s); proceeding anyway,"
              " the in-game guards will report the actual state" % wbad, flush=True)
        crash_evidence("world gate miss")
    for _ in range(8):
        waker()
        time.sleep(0.5)

    # ---- trigger the open-books sequence ----------------------------------------------------
    print("trigger  : writing trigger.txt (%s), waiting up to %.0fs for output_%s.done"
          % (args.trigger, args.timeout, args.trigger), flush=True)
    with open(os.path.join(gamedir.BASE_DIR, "trigger.txt"), "w") as f:
        f.write(args.trigger + "\n")
    done_path = os.path.join(gamedir.BASE_DIR, "output_%s.done" % args.trigger)
    deadline = time.time() + args.timeout
    last_beat = deadline - args.timeout
    status = None
    bad = None
    last_size = gamedir.combined_log_size()
    last_change = time.time()
    while time.time() < deadline:
        exc = gamedir.new_exception(exception_baseline)
        if exc is not None:
            bad = "new exception report: %s" % exc
            break
        if process.poll() is not None:
            bad = "hta.exe exited with code %s" % process.poll()
            break
        if os.path.exists(done_path):
            try:
                with open(done_path, encoding="utf-8", errors="replace") as f:
                    status = f.read().strip()
            except OSError:
                status = "unreadable"
            break
        size = gamedir.combined_log_size()
        if size != last_size:
            last_size, last_change = size, time.time()
        elif time.time() - last_change >= args.wedged:
            bad = "game wedged/crashed (logs silent %.0fs)" % (time.time() - last_change)
            break
        if time.time() - last_beat >= 5.0:
            last_beat = time.time()
            print("  [trigger] waiting for output_%s.done (%.0fs/%.0fs) - game is up, do not close it"
                  % (args.trigger, time.time() - (deadline - args.timeout), args.timeout), flush=True)
        waker()
        time.sleep(0.5)

    if bad:
        crash_evidence("trigger failure")
        kill_game()
        print("RESULT: FAIL (%s)" % bad)
        return 1

    # let a couple of paint frames land after the notify - the patched SetText's
    # book rendering happens in UI draw frames, which only run while messages
    # arrive, so keep waking the loop through this window too
    for _ in range(10):
        waker()
        time.sleep(0.5)
    exc = gamedir.new_exception(exception_baseline)
    if exc is not None:
        bad = "new exception report after trigger: %s" % exc
    alive = process.poll() is None

    # ---- evidence ---------------------------------------------------------------------------
    evidence = [l for l in log_since(run_since)
                if ("book parsed:" in l or "book layout ready:" in l
                    or "open_books:" in l or "hook installed" in l
                    or "load_save:" in l)]

    print("")
    print("  status     : %s%s" % (status, "" if status else " (no done file)"))
    print("  alive      : %s" % alive)
    print("  exceptions : %s" % (exc if exc else "none new"))
    print("  log evidence (%d lines this run):" % len(evidence))
    for l in evidence[-25:]:
        print("    %s" % l)

    ok = (status == "ok" and alive and exc is None)
    if not ok:
        crash_evidence("final state")
    if ok and args.keep:
        print("keep     : --keep set, leaving the game running (pid %d) - close it yourself"
              % process.pid, flush=True)
    else:
        kill_game()
    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    if not ok and status == "ok" and not alive:
        print("  (status was ok but the process died shortly after - treat as crash)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
