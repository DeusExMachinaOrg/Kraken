"""
Reliable per-config calibration measurement for the Jolt wheelmodel (docs §40).

Relaunches hta.exe -> fresh autoload places the player at the identical pristine spawn ->
settle -> drive straight (NO teleport, so it's reproducible; the spawn-line teleport proved
unreliable for large displacements, wedging the ODE vehicle). Then parses kraken.log for
this run's window and reports Jolt-vs-ODE divergence + suspension health.

Because every run starts from the identical autoload state, differences between configs are
attributable to the [wheelmodel] params (read live from kraken.ini each frame) - edit the ini
between calls, no rebuild.

Usage: python wm_cfg.py <token> [drive_seconds]
"""
import os
import re
import subprocess
import sys
import time
import statistics as st

sys.path.insert(0, r"F:\Kraken\tools\testharness")
import harness  # noqa: E402

# Paths come from gamedir.py - the single place that knows where the game is installed.
# The recovered copy of this file hard-coded F:\HTA_Kraken, which no longer exists.
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import gamedir


BASE_DIR = gamedir.BASE_DIR
LOG = gamedir.LOG
EXE = gamedir.EXE
WORKDIR = gamedir.WORKDIR

token = sys.argv[1] if len(sys.argv) > 1 else "wmcfg"
DRIVE = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
SETTLE = 3.0
# docs §42: autoload silently rebuilds the shadow 2-3x in the first ~9s after launch (a
# placeholder/menu-default vehicle first, then the save's real one) - caught live when a
# rolling-resistance test's "built:" line said mass=100 but the LAST build in the log was
# actually mass=167, meaning prior tests may have been comparing different vehicles across
# runs without knowing it. Pin an explicit, known vehicle via switch_vehicle.txt (same
# mechanism run_vehicle_cycle.py uses) after the initial dust settles, so every test - past
# comparisons aside - is unambiguously against the SAME prototype from here on.
PIN_VEHICLE = os.environ.get("WM_VEHICLE", "Molokovoz01")


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "hta.exe"], capture_output=True)
    time.sleep(2.0)


def clear_trigger():
    with open(os.path.join(BASE_DIR, "trigger.txt"), "w") as f:
        f.write("")
    # docs §47: switch_vehicle.txt has the same stale-leftover-file bug as trigger.txt - a
    # leftover token from an earlier test's last write auto-fires on the very first tick of a
    # fresh process. Harmless here in practice (every test this session wanted Molokovoz01
    # anyway, so a stale leftover always matched), but caught live corrupting a different
    # script's Ural01 test - clearing it defensively so this script never relies on that luck.
    with open(os.path.join(BASE_DIR, "switch_vehicle.txt"), "w") as f:
        f.write("")


def log_line_count():
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return sum(1 for _ in f)


def pin_vehicle(name, timeout=10.0):
    """Force a known vehicle via switch_vehicle.txt, wait for its own rebuild line so every
    test run is unambiguously against the SAME prototype (see PIN_VEHICLE comment above)."""
    before = log_line_count()
    with open(os.path.join(BASE_DIR, "switch_vehicle.txt"), "w") as f:
        f.write(name + "\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.5)
        with open(LOG, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        for line in lines[before:]:
            if "built (player)" in line:
                return line.strip()
    return None


# Molokovoz01's known fingerprint (mass, chassis dims) - a "did not confirm a rebuild" warning from
# pin_vehicle is usually harmless (the vehicle was ALREADY correct, so no new build line fires) but
# NOT always: a live run (docs §44) landed on a completely different vehicle (6 wheels, mass=275,
# chassis 5x3x12 - the OLD "Ural01" 6-wheel-truck fingerprint from §37) despite a "warning, proceeding"
# from pin_vehicle, silently contaminating that run's result. Never trust the warning path blind again -
# always verify the actual LAST build line's fingerprint before running the scenario.
MOLOKOVOZ_FINGERPRINT = ("mass=167.0", "chassis=3.50x2.50x9.00")
# docs §67.5: cross-vehicle step-5 test. Fingerprints captured by discover_vehicles.py
# from real build lines - the STRICT check stays (docs §44), it is just no longer
# hardcoded to one vehicle.
FINGERPRINTS = {
    "Molokovoz01": ("mass=167.0", "chassis=3.50x2.50x9.00"),
    "Scout01":     ("mass=100.0", "chassis=4.00x1.00x2.00"),
    "Ural01":      ("mass=303.0", "chassis=5.00x3.00x12.00"),
}
PIN_FINGERPRINT = FINGERPRINTS.get(PIN_VEHICLE, MOLOKOVOZ_FINGERPRINT)


def last_build_line():
    with open(LOG, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    for line in reversed(lines):
        if "built (player)" in line:
            return line.strip()
    return None


def verify_vehicle_or_retry(name, fingerprint, max_attempts=3):
    """docs §44: pin_vehicle's own wait can time out even when the swap DID eventually happen (or
    didn't need to), so check the real, current fingerprint directly rather than trust that wait's
    pass/fail. Retries the pin (with a fresh wait) if the fingerprint doesn't match; raises rather
    than silently returning contaminated-vehicle data if it still doesn't match after retrying."""
    for attempt in range(1, max_attempts + 1):
        pin_vehicle(name)
        time.sleep(2.0)
        line = last_build_line()
        if line and all(f in line for f in fingerprint):
            return line
        print(f"  vehicle fingerprint mismatch on attempt {attempt}: {line}")
    raise RuntimeError(
        f"Could not confirm vehicle {name!r} (fingerprint {fingerprint}) after {max_attempts} "
        f"attempts - last build line: {last_build_line()!r}. Aborting rather than risk measuring "
        f"the wrong vehicle (docs §44 - this exact failure mode silently contaminated a real run)."
    )


def launch_and_wait(timeout=75.0):
    """Launch; wait for a fresh 'built (player)' line. Returns build line or None."""
    # mark current log end so we only look at THIS launch (log truncates on launch anyway)
    subprocess.Popen([EXE], cwd=WORKDIR, close_fds=True)
    deadline = time.time() + timeout
    built = None
    while time.time() < deadline:
        time.sleep(3.0)
        try:
            with open(LOG, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "built (player)" in line:
                        built = line.strip()
        except FileNotFoundError:
            pass
        if built:
            return built
    return None


def secs(ts):
    h, m, s = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def epoch_to_secs_since_midnight(epoch):
    lt = time.localtime(epoch)
    frac = epoch - int(epoch)
    return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + frac


ts_re = re.compile(r"(\d\d:\d\d:\d\d\.\d\d\d)")
div_re = re.compile(
    r"Shadow divergence \(player\): pos=([\-\d.]+)m vel=([\-\d.]+)m/s angle=([\-\d.]+)deg "
    r"\(jolt com=\[([\-\d. ]+)\] ode com=\[([\-\d. ]+)\]\)")
wm_re = re.compile(
    r"§43: wm apply \(player\) w=(\d+) drv=(\d+) n=(\d+) gSlot=([\-\d]+) pen=([\-\d.]+) "
    r"comp=([\-\d.]+) suspF=([\-\d.]+) omega=([\-\d.]+) thr=([\-\d.]+) "
    r"fwd=\([\-\d.,]+\) Fg=\([\-\d.,]+\) fpar=([\-\d.]+) gear=([\-\d]+) rpm=([\-\d.]+) maxTq=([\-\d.]+)")
# docs §43: pulled out as an INDEPENDENT key=value regex (not more positional groups tacked onto
# wm_re) specifically to avoid repeating the exact off-by-one bug §41 hit once already (forgot
# fpar was its own group before gear) - each new field is matched by its own literal key, immune
# to argument-order mistakes in the C++ format string.
extra_re = re.compile(
    r"muReal=([\-\d.]+) compMax=([\-\d.]+) mUnsprung=([\-\d.]+) soilMu=([\-\d.]+)")


def analyze(since_hms, trigger_epoch):
    """docs §40 addendum: a short DRIVE window makes the post-drive brake tail (residual
    coasting - braking doesn't stop a moving truck instantly, especially downhill) a LARGE
    fraction of the measured distance, badly biasing any end-of-scenario travel-ratio metric
    (caught live: a 5s-drive config measured a 2.5x Jolt/ODE ratio end-to-end, but only 1.5x
    during the actual throttle window - the rest was brake-phase noise). Anchor on
    trigger_epoch (wall-clock time trigger_run() was called) to find the sample AT THROTTLE
    END (trigger_epoch + SETTLE + DRIVE) specifically, not just the scenario's last sample."""
    since = secs(since_hms)
    div, wm = [], []
    for line in open(LOG, encoding="utf-8", errors="replace"):
        tm = ts_re.search(line)
        if not tm or secs(tm.group(1)) < since - 1:
            continue
        d = div_re.search(line)
        if d:
            jc = [float(x) for x in d.group(4).split()]
            oc = [float(x) for x in d.group(5).split()]
            div.append((secs(tm.group(1)), float(d.group(1)), float(d.group(3)), jc, oc))
            continue
        w = wm_re.search(line)
        if w:
            g = w.groups()
            ex = extra_re.search(line)
            extra = tuple(float(x) for x in ex.groups()) if ex else (float("nan"),) * 4
            wm.append((float(g[5]), float(g[6]), float(g[7]), int(g[10]), float(g[11]), float(g[12]), *extra))
            # comp, suspF, omega, gear, rpm, maxTq, muReal, compMax, mUnsprung, soilMu

    def horiz(a, b):
        return ((a[0]-b[0])**2 + (a[2]-b[2])**2) ** 0.5

    print(f"  div={len(div)} wm={len(wm)}")
    if div:
        throttle_end_t = epoch_to_secs_since_midnight(trigger_epoch) + SETTLE + DRIVE
        # nearest sample at/before throttle-end (falls back to last sample if drive outran logging)
        at_end = [d for d in div if d[0] <= throttle_end_t] or [div[-1]]
        end = at_end[-1]
        pos = [d[1] for d in div]; ang = [d[2] for d in div]
        ode_tr = horiz(end[4], div[0][4]); jolt_tr = horiz(end[3], div[0][3])
        yd0 = div[0][3][1]-div[0][4][1]; yd_end = end[3][1]-end[4][1]
        ode_z = end[4][2]-div[0][4][2]; jolt_z = end[3][2]-div[0][3][2]
        xsplit = end[3][0]-end[4][0]
        ratio = jolt_tr / ode_tr if ode_tr > 0.5 else float("nan")
        print(f"  [at throttle-end, t~{DRIVE:.1f}s of drive] pos-div={end[1]:.2f} angle={end[2]:.1f}")
        print(f"  travelZ  ODE={ode_z:+.1f} Jolt={jolt_z:+.1f}   travelXZ ODE={ode_tr:.1f} Jolt={jolt_tr:.1f}  ratio={ratio:.2f}")
        print(f"  X-split={xsplit:+.1f}m   Y(Jolt-ODE) rest={yd0:+.2f} @throttle-end={yd_end:+.2f}")
        # also show full-scenario (incl brake) end state, labeled separately so it's never confused
        pos_last, ang_last = pos[-1], ang[-1]
        print(f"  [full scenario incl. brake tail] pos-div last={pos_last:.2f} angle last={ang_last:.1f} max-pos={max(pos):.2f} max-angle={max(ang):.1f}")
    if wm:
        comp=[w[0] for w in wm]; susf=[w[1] for w in wm]; om=[w[2] for w in wm]
        gear=[w[3] for w in wm]; rpm=[w[4] for w in wm]; maxtq=[w[5] for w in wm]
        muR=[w[6] for w in wm]; cmax=[w[7] for w in wm]; munsp=[w[8] for w in wm]; soilmu=[w[9] for w in wm]
        print(f"  susp comp[{min(comp):.3f},{max(comp):.3f}] suspF[{min(susf):.0f},{max(susf):.0f}] omega[{min(om):.1f},{max(om):.1f}]")
        print(f"  gearbox gear[{min(gear)},{max(gear)}] rpm[{min(rpm):.0f},{max(rpm):.0f}] maxTq[{min(maxtq):.0f},{max(maxtq):.0f}]")
        print(f"  docs §43 real-data: muReal[{min(muR):.3f},{max(muR):.3f}] compMax[{min(cmax):.3f},{max(cmax):.3f}] "
              f"mUnsprung[{min(munsp):.1f},{max(munsp):.1f}] soilMu[{min(soilmu):.3f},{max(soilmu):.3f}]")


# docs §67.7: assert the SETUP is valid, not just the result. A pinned vehicle is placed in the
# PREVIOUS vehicle's pose, so anything larger than the save's own gets dropped from a height - a
# 12 m truck fell ~4.5 m and landed nose-out over a void, where mode 2's wheel rays reach nothing
# and its forces are identically zero. That run reported travel Jolt/ODE = 0.000 with sd 0 across
# three repeats: perfectly reproducible, and reproducibly measuring a vehicle that was never on
# its wheels. A tight spread says nothing about whether the setup was sane.
#
# Ground truth for "on its wheels" differs by mode, so accept either:
#   mode 2 : "docs §43: wm apply ... n=<contacts>"      -> n > 0
#   mode 4 : "docs §66: tyre band ... survived=<recs>"  -> survived > 0
# Same spirit as verify_vehicle_or_retry: refuse to measure a contaminated setup rather than
# quietly reporting its numbers.
CONTACT_RES = (
    re.compile(r"docs §43: wm apply \(player\) w=\d+ drv=\d+ n=(\d+)"),
    re.compile(r"docs §66: tyre band \(player\) w=\d+ .*? survived=(\d+)"),
)


def wheels_on_ground(since_line):
    """Best evidence of ground contact found in the log after `since_line`. (ok, detail)."""
    with open(LOG, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()[since_line:]
    best, seen = 0, 0
    for line in lines:
        for rx in CONTACT_RES:
            m = rx.search(line)
            if m:
                seen += 1
                best = max(best, int(m.group(1)))
    return best > 0, "%d contact lines, best=%d" % (seen, best)


def verify_grounded_or_abort(max_attempts=5, pause=2.0):
    before = log_line_count()
    for attempt in range(1, max_attempts + 1):
        time.sleep(pause)
        ok, detail = wheels_on_ground(before)
        if ok:
            print(f"  grounded: {detail}")
            return
        print(f"  not grounded yet (attempt {attempt}/{max_attempts}): {detail}")
    raise RuntimeError(
        "ABORT: no wheel reports ground contact after pinning - the vehicle is not resting on its "
        "wheels, so any number this run produced would describe a broken setup, not the model "
        "(docs §67.7). Most likely it was dropped into the pose of a smaller vehicle by "
        "switch_vehicle.txt; use a save where this vehicle is native.")


def main():
    kill()
    clear_trigger()
    built = launch_and_wait()
    if not built:
        print("FAILED: no build after launch (retry once)")
        kill(); built = launch_and_wait()
        if not built:
            print("FAILED again - aborting"); return
    print(f"built (autoload, may be transient): {built.split('joltshadow - ')[-1]}")
    time.sleep(6.0)  # settle after build

    confirmed = verify_vehicle_or_retry(PIN_VEHICLE, PIN_FINGERPRINT)
    print(f"confirmed: {confirmed.split('joltshadow - ')[-1]}")
    time.sleep(2.0)  # settle after the pin's own rebuild
    verify_grounded_or_abort()

    steer_amp = float(os.environ.get("WM_STEER", "0.0"))
    if steer_amp != 0.0:
        # docs §68 (шаг 6): straight for the first half, then hard lock for the second. Kept
        # OPT-IN via WM_STEER so every earlier straight-line baseline stays comparable - changing
        # the scenario for everyone would silently invalidate the numbers already recorded.
        scen = harness.Scenario(samples=[
            harness.Sample(t=0.0, throttle=0.0),
            harness.Sample(t=SETTLE, throttle=0.0),
            harness.Sample(t=SETTLE+0.05, throttle=1.0),
            harness.Sample(t=SETTLE+DRIVE*0.5, throttle=1.0),
            harness.Sample(t=SETTLE+DRIVE*0.5+0.05, throttle=1.0, steer=steer_amp),
            harness.Sample(t=SETTLE+DRIVE, throttle=1.0, steer=steer_amp),
            harness.Sample(t=SETTLE+DRIVE+0.05, throttle=0.0, brake=1.0, steer=steer_amp),
            harness.Sample(t=SETTLE+DRIVE+2.0, throttle=0.0, brake=1.0),
        ])
    else:
        scen = harness.Scenario(samples=[
            harness.Sample(t=0.0, throttle=0.0),
            harness.Sample(t=SETTLE, throttle=0.0),
            harness.Sample(t=SETTLE+0.05, throttle=1.0),
            harness.Sample(t=SETTLE+DRIVE, throttle=1.0),
            harness.Sample(t=SETTLE+DRIVE+0.05, throttle=0.0, brake=1.0),
            harness.Sample(t=SETTLE+DRIVE+2.0, throttle=0.0, brake=1.0),
        ])
    harness.write_scenario(scen, base_dir=BASE_DIR)
    for ext in (".done", ".csv"):
        p = os.path.join(BASE_DIR, f"output_{token}{ext}")
        if os.path.exists(p):
            os.remove(p)
    since = time.strftime("%H:%M:%S")
    trigger_epoch = time.time()
    harness.trigger_run(base_dir=BASE_DIR, token=token)
    status = harness.wait_for_done(token, base_dir=BASE_DIR, timeout=SETTLE+DRIVE+12.0)
    time.sleep(1.0)
    print(f"=== cfg '{token}' status={status} drive={DRIVE}s since={since} ===")
    analyze(since, trigger_epoch)
    kill()  # don't leave hta.exe idling/still-logging between invocations - bit us once already
            # (a "stale idle tail" looked like a runaway state until traced back to this)


if __name__ == "__main__":
    main()
