"""Stage 1 precondition -1C: cold re-baseline of the divergence metric.

Why this exists: the 'angle' metric is yaw-conflated and noisy. docs §51 measured a
151-167deg spread across repeats of COMPLETELY UNCHANGED code, and that noise already
caused one false "regression" alarm this project. Before Stage 1 lands, we need the
noise floor per save, or the first run of the new topology will be read as signal.

Save selection is not configurable (testharness.cpp:548-574 hardcodes "newest mtime
across all profiles"), so the established zero-code technique is used: temporarily make
the target save directory the newest, then restore every original mtime afterwards.
Only filesystem metadata is touched - save contents are never modified.

Usage: python baseline1c.py <save_number> <repeats> [token]
"""
import os
import re
import subprocess
import sys
import time
import statistics as st

PROFILES = r"F:\HTA_Kraken\data\profiles"
INI = r"F:\HTA_Kraken\data\kraken.ini"
WM_CFG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wm_cfg.py")

# The reference config this baseline is defined against. Set explicitly on every invocation:
# wm_cfg.py only edits [wheelmodel] params and ASSUMES the rest is already right, so if the ini
# happens to be left with [testharness] enabled=0 the game never autoloads a save, silently runs
# the main-menu demo car instead, and every run fails the vehicle-fingerprint check. That happened
# once for real - the runner must not depend on ambient ini state.
REQUIRED_INI = {
    ("jolt", "enabled"): 1,
    ("jolt_harness", "shadow"): 1,
    ("jolt_harness", "apply"): 1,
    ("jolt_harness", "player_only"): 1,
    ("jolt_harness", "ai_count"): 0,
    ("jolt_harness", "wheelmodel"): 2,
    ("testharness", "enabled"): 1,
    ("testharness", "autoload_save"): 1,
    ("testharness", "perfmon"): 0,
    ("testharness", "god_mode"): 0,
}


def set_ini(pairs):
    with open(INI, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    section, done = None, set()
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            section = s[1:-1]
            continue
        if "=" not in s or section is None:
            continue
        key = s.split("=", 1)[0].strip()
        if (section, key) in pairs and (section, key) not in done:
            lines[i] = "%s=%s" % (key, pairs[(section, key)])
            done.add((section, key))
    missing = set(pairs) - done
    if missing:
        raise RuntimeError("ini keys not found: %r" % (missing,))
    with open(INI, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def saves_root():
    """The profile folder name is Cyrillic; discover it rather than hardcode an encoding."""
    for d in os.listdir(PROFILES):
        cand = os.path.join(PROFILES, d, "saves")
        if os.path.isdir(cand):
            return cand
    raise RuntimeError("no profile with a saves/ dir under %s" % PROFILES)


SAVES = saves_root()


def save_dirs():
    return [os.path.join(SAVES, d) for d in os.listdir(SAVES)
            if os.path.isdir(os.path.join(SAVES, d))]


def select_save(target):
    """Make `target` the newest save dir. Returns {path: (atime, mtime)} to restore."""
    dirs = save_dirs()
    original = {p: (os.stat(p).st_atime, os.stat(p).st_mtime) for p in dirs}
    tpath = os.path.join(SAVES, target)
    if tpath not in original:
        raise RuntimeError("save %s not found; have: %s"
                           % (target, sorted(os.path.basename(p) for p in dirs)))
    newest = max(m for (_, m) in original.values())
    os.utime(tpath, (original[tpath][0], newest + 120.0))
    return original


def restore(original):
    for p, (a, m) in original.items():
        try:
            os.utime(p, (a, m))
        except OSError:
            pass


ANG_RE = re.compile(r"pos-div=([\d.]+) angle=([\d.]+)")
RATIO_RE = re.compile(r"ratio=([\d.]+)")
MAXANG_RE = re.compile(r"max-angle=([\d.]+)")
CONF_RE = re.compile(r"confirmed: (.*)")
# docs §67.6: a ratio of 0.000 is ambiguous between "Jolt still" and "ODE huge" -
# always carry the raw distances too.
TRAVEL_RE = re.compile(r"travelXZ ODE=([\-\d.]+) Jolt=([\-\d.]+)")


def one_run(token, drive=8.0):
    proc = subprocess.run([sys.executable, WM_CFG, token, str(drive)],
                          capture_output=True, text=True, timeout=400)
    out = proc.stdout
    ang = ANG_RE.search(out)
    ratio = RATIO_RE.search(out)
    maxang = MAXANG_RE.search(out)
    if not (ang and ratio and maxang):
        print("    RUN FAILED / unparseable:")
        print("    " + "\n    ".join(out.strip().split("\n")[-6:]))
        return None
    tr = TRAVEL_RE.search(out)
    return {
        "ode_travel": float(tr.group(1)) if tr else float("nan"),
        "jolt_travel": float(tr.group(2)) if tr else float("nan"),
        "pos_div": float(ang.group(1)),
        "angle": float(ang.group(2)),
        "ratio": float(ratio.group(1)),
        "max_angle": float(maxang.group(1)),
        "vehicle": (CONF_RE.search(out).group(1)[:60] if CONF_RE.search(out) else "?"),
    }


def summarize(name, rows, key):
    vals = [r[key] for r in rows]
    if not vals:
        return "%-10s no data" % key
    mean = st.mean(vals)
    sd = st.stdev(vals) if len(vals) > 1 else 0.0
    return ("%-10s n=%d mean=%7.2f sd=%6.2f min=%7.2f max=%7.2f spread=%7.2f"
            % (key, len(vals), mean, sd, min(vals), max(vals), max(vals) - min(vals)))


if __name__ == "__main__":
    target = sys.argv[1]
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    token = sys.argv[3] if len(sys.argv) > 3 else "b1c"

    set_ini(REQUIRED_INI)
    original = select_save(target)
    rows = []
    try:
        for i in range(repeats):
            print("  [%s] run %d/%d ..." % (target, i + 1, repeats), flush=True)
            r = one_run("%s_%s_%d" % (token, target, i))
            if r:
                rows.append(r)
                print("    ratio=%.3f angle=%.1f max-angle=%.1f pos-div=%.2f  (%s)"
                      % (r["ratio"], r["angle"], r["max_angle"], r["pos_div"], r["vehicle"]),
                      flush=True)
    finally:
        restore(original)
        print("  (save mtimes restored)")

    print("\n=== BASELINE save %s, %d/%d runs parsed ===" % (target, len(rows), repeats))
    for k in ("ratio", "angle", "max_angle", "pos_div"):
        print("  " + summarize(target, rows, k))
