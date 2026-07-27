"""Task #95 / docs §66: where in the tyre band does a mode-4 wheel actually sit?

§65 concluded "the band is bottomed out at rest" from the MEAN of maxNormalF - which is a
MAXIMUM over 4 wheels x 3 contact slots, sampled once per 60 frames. A max-statistic cannot
support that claim. This measures the thing that can: per-wheel ground normal force, the
penetration that produced it, and whether the surviving contacts came from the TYRE sphere
(radius R) or the RIM sphere (radius R-tau).

THE PREDICTION UNDER TEST (from reading Jolt's own headers, not from theory):
HarvestWheelManifold stores GetWorldSpaceContactPointOn1(0) - the contact point on SHAPE 1's
surface - and PhysicsSystem.cpp:1072 guarantees shape 1 is the WHEEL for wheel-vs-terrain
(dynamic trumps static). So the stored anchor sits on the wheel's own sub-shape surface, and
ApplyWheelForces' reprojection
    penRaw = R - Dot(centre - anchor, n)
degenerates to a constant per sub-shape:
    TYRE record (radius R):     penRaw = R - R       = 0      -> discarded (penRaw <= 0)
    RIM  record (radius R-tau): penRaw = R - (R-tau) = tau    -> clamped to tau = FULL BAND
i.e. the band has no proportional region at all. It is bang-bang: zero force when the rim is
clear, maximum force the instant it touches.

  CONFIRMED  <=>  survived == recs_rim (tyre records all discarded) AND pen/tau == 1.00
  REFUTED    <=>  tyre records survive and pen/tau varies continuously below 1

Usage: python band95.py [save] [repeats] [drive_seconds]
"""
import importlib.util
import os
import re
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("b1c", os.path.join(HERE, "baseline1c.py"))
b1c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b1c)

LOG = r"F:\HTA_Kraken\kraken.log"

BASE_INI = {
    ("jolt", "enabled"): 1,
    # docs §69.5: pinned via WM_THREADS so the Jolt job system can be taken out of the
    # determinism question without editing the file. 0 = auto (the shipping default).
    ("jolt", "threads"): int(os.environ.get("WM_THREADS", "0")),
    ("jolt_harness", "shadow"): 1,
    ("jolt_harness", "apply"): 1,
    ("jolt_harness", "player_only"): 1,
    ("jolt_harness", "ai_count"): 0,
    ("jolt_harness", "wheelmodel"): 4,
    ("jolt_harness", "wm4_hardcore"): 1,
    ("jolt_harness", "wm4_max_dt"): 0,
    ("jolt_harness", "wm4_spin"): 1,
    ("jolt_harness", "wm4_steer"): 1,
    ("jolt_harness", "wm4_max_substeps"): 8,
    ("testharness", "enabled"): 1,
    ("testharness", "autoload_save"): 1,
    ("testharness", "perfmon"): 1,
    ("testharness", "god_mode"): 1,
}

BAND_RE = re.compile(
    r"docs §66: tyre band \(player\) w=(\d+) fnGround=([\-\d.]+)N fnTotal=([\-\d.]+)N pen=([\-\d.]+) "
    r"penRaw=([\-\d.]+) tau=([\-\d.]+) pen/tau=([\-\d.]+) \| "
    r"recs tyre=(\d+) rim=(\d+) survived=(\d+) stale=(\d+) distMax=([\-\d.]+)R")
SPIN_RE = re.compile(
    r"docs §67: spin \(player\) w=(\d+) driven=(\d+) omega=([\-\d.]+) rad/s "
    r"surfaceSpeed=([\-\d.]+) m/s")
DRIVE_RE = re.compile(
    r"docs §67: drivetrain \(player\) gear=(\d+) rpm=([\-\d.]+) driveTorque=([\-\d.]+)Nm")
TILT_RE = re.compile(r"docs §69: tilt \(player\) jolt=([\-\d.]+) deg ode=([\-\d.]+) deg")
SUM_RE = re.compile(
    r"docs §66: tyre band SUM \(player\) fnGroundSum=([\-\d.]+)N fnTotalSum=([\-\d.]+)N weight=([\-\d.]+)N "
    r"carried=([\-\d.]+) \| penSum=([\-\d.]+) recs tyre=(\d+) rim=(\d+) stale=(\d+)")


def scan_log():
    """Only lines AFTER the last 'built (player)' - autoload rebuilds the shadow 2-3x and the
    earlier blocks belong to a different vehicle entirely (wm_cfg.py's own §42 note)."""
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return {}
    last_build, build_line = 0, ""
    for i, line in enumerate(lines):
        if "built (player)" in line:
            last_build, build_line = i, line
    tail = lines[last_build:]
    # docs §72: record WHICH vehicle these numbers came from, per run. wm_cfg.py already refuses
    # to run on a fingerprint mismatch, but that check lives in a different process - carrying the
    # fingerprint into the results means the summary can assert every cell measured one vehicle.
    fp = re.search(r"mass=([\d.]+), chassis=([\d.x]+)", build_line)
    fingerprint = ("%s|%s" % fp.groups()) if fp else "UNKNOWN"

    bands, sums, spins, drives, tilts = [], [], [], [], []
    for line in tail:
        m = TILT_RE.search(line)
        if m:
            tilts.append((float(m.group(1)), float(m.group(2))))
            continue
        m = SPIN_RE.search(line)
        if m:
            spins.append({"driven": int(m.group(2)), "omega": float(m.group(3)),
                          "surf": float(m.group(4))})
            continue
        m = DRIVE_RE.search(line)
        if m:
            drives.append({"gear": int(m.group(1)), "rpm": float(m.group(2)),
                           "tq": float(m.group(3))})
            continue
        m = BAND_RE.search(line)
        if m:
            bands.append({
                "w": int(m.group(1)), "fn": float(m.group(2)), "fnTot": float(m.group(3)),
                "pen": float(m.group(4)), "penRaw": float(m.group(5)), "tau": float(m.group(6)),
                "frac": float(m.group(7)), "tyre": int(m.group(8)),
                "rim": int(m.group(9)), "surv": int(m.group(10)),
                "stale": int(m.group(11)), "distMax": float(m.group(12)),
            })
            continue
        m = SUM_RE.search(line)
        if m:
            sums.append({
                "fnSum": float(m.group(2)), "weight": float(m.group(3)),
                "carried": float(m.group(4)), "tyre": int(m.group(6)), "rim": int(m.group(7)),
            })
    # docs §69.5: the tilt/spin/drivetrain readings do NOT depend on the tyre-band lines, and
    # mode 2 emits no band lines at all - so an early return here silently reported zero tilt for
    # every mode-2 cell, which is exactly the reference the comparison needed. Return what was
    # actually parsed instead of nothing.
    # docs §72: absent data is None, NEVER 0.0. A metric that silently returns zero is
    # indistinguishable from a real measurement of zero, and this session lost two hours to
    # exactly that: mode 2 emits no §66 band lines, the parser returned early, and every tilt
    # field read "0.000 sd 0.000" - which looks like a perfectly reproducible measurement rather
    # than "nothing was parsed". The summary below prints "no data" for None.
    tilt_fields = {
        "tilt_rms": (sum((j - o) ** 2 for j, o in tilts) / len(tilts)) ** 0.5 if tilts else None,
        "tilt_jolt_max": max([j for j, _ in tilts]) if tilts else None,
        "tilt_ode_max": max([o for _, o in tilts]) if tilts else None,
    }
    if not bands:
        return dict({"band_samples": 0, "vehicle": fingerprint}, **tilt_fields)

    live = [b for b in bands if b["surv"] > 0]
    # The decisive test: of the records that SURVIVED reprojection, are they all rim records?
    rim_only = sum(1 for b in live if b["surv"] == b["rim"] and b["tyre"] > 0)
    pinned = sum(1 for b in live if b["frac"] >= 0.995)
    return {
        "vehicle": fingerprint,
        "band_samples": len(bands),
        "live_samples": len(live),
        "rim_only_pct": 100.0 * rim_only / len(live) if live else 0.0,
        "pinned_pct": 100.0 * pinned / len(live) if live else 0.0,
        "frac_mean": st.mean([b["frac"] for b in live]) if live else 0.0,
        "frac_min": min([b["frac"] for b in live]) if live else 0.0,
        "fn_mean": st.mean([b["fn"] for b in live]) if live else 0.0,
        "fntot_mean": st.mean([b["fnTot"] for b in live]) if live else 0.0,
        "tyre_recs": st.mean([b["tyre"] for b in bands]),
        "rim_recs": st.mean([b["rim"] for b in bands]),
        "carried": st.mean([s["carried"] for s in sums]) if sums else 0.0,
        "stale_recs": st.mean([b["stale"] for b in bands]),
        "dist_max": max([b["distMax"] for b in bands]),
        "weight": sums[0]["weight"] if sums else 0.0,
        "surf_max": max([abs(x["surf"]) for x in spins]) if spins else 0.0,
        "omega_max": max([abs(x["omega"]) for x in spins]) if spins else 0.0,
        "gear_max": max([d["gear"] for d in drives]) if drives else 0,
        "rpm_max": max([d["rpm"] for d in drives]) if drives else 0.0,
        "tq_max": max([d["tq"] for d in drives]) if drives else 0.0,
        # docs §69: the calibration objective. RMS of |jolt tilt - ode tilt| measures whether the
        # SHADOW FOLLOWS ODE's attitude, which is what a recalibration should optimise. max_angle
        # (the angle BETWEEN orientations) is hopeless here: this scenario throws BOTH sides around
        # (ode reaches 46 deg on its own), so it mixes "the model is wrong" with "the terrain is
        # violent" and cannot separate them.
        "tilt_rms": (sum((j - o) ** 2 for j, o in tilts) / len(tilts)) ** 0.5 if tilts else 0.0,
        "tilt_jolt_max": max([j for j, _ in tilts]) if tilts else 0.0,
        "tilt_ode_max": max([o for _, o in tilts]) if tilts else 0.0,
    }


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "00000009"
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    drive = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
    max_dt = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    wmode  = int(sys.argv[5]) if len(sys.argv) > 5 else 4

    ini = dict(BASE_INI)
    ini[("jolt_harness", "wm4_max_dt")] = max_dt
    ini[("jolt_harness", "wheelmodel")] = wmode
    print("  [wheelmodel=%d wm4_max_dt=%s]" % (wmode, max_dt), flush=True)
    b1c.set_ini(ini)
    original = b1c.select_save(target)
    rows = []
    try:
        for i in range(repeats):
            print("  run %d/%d (drive=%.1fs) ..." % (i + 1, repeats, drive), flush=True)
            r = b1c.one_run("band95_%d" % i, drive=drive)
            if r is None:
                continue
            r.update(scan_log())
            rows.append(r)
            print("    pos-div=%8.2f max-ang=%6.1f | band: %d samples, %d live | "
                  "rim-only=%5.1f%% pinned(pen/tau>=1)=%5.1f%% | pen/tau mean=%.3f min=%.3f | "
                  "Fn=%6.0fN carried=%.2f | recs tyre=%.1f rim=%.1f"
                  % (r["pos_div"], r["max_angle"], r.get("band_samples", 0),
                     r.get("live_samples", 0), r.get("rim_only_pct", 0.0),
                     r.get("pinned_pct", 0.0), r.get("frac_mean", 0.0), r.get("frac_min", 0.0),
                     r.get("fn_mean", 0.0), r.get("carried", 0.0),
                     r.get("tyre_recs", 0.0), r.get("rim_recs", 0.0)), flush=True)
    finally:
        b1c.restore(original)
        print("  (save mtimes restored)")

    if not rows:
        print("\nNO DATA")
        return
    print("\n=== TASK #95 / docs §66 (save %s, n=%d, drive=%.1fs) ===" % (target, len(rows), drive))

    seen = set(r.get("vehicle", "UNKNOWN") for r in rows)
    if len(seen) > 1:
        print("  !! WARNING: this batch measured MORE THAN ONE vehicle: %s" % sorted(seen))
        print("  !! The aggregate below mixes them and is meaningless. Do not use it.")
    else:
        print("  vehicle (mass|chassis): %s" % sorted(seen)[0])

    def agg(key):
        vals = [r.get(key) for r in rows]
        vals = [v for v in vals if v is not None]
        if not vals:
            return None
        return (st.mean(vals), st.stdev(vals) if len(vals) > 1 else 0.0, min(vals), max(vals))

    for key, label in (("rim_only_pct", "rim-only survivors %"), ("pinned_pct", "pen/tau>=1.00 %"),
                       ("frac_mean", "pen/tau mean"), ("frac_min", "pen/tau min"),
                       ("fn_mean", "Fn ground (N)"), ("fntot_mean", "Fn total/wheel (N)"),
                       ("carried", "fnTotalSum/weight"),
                       ("tyre_recs", "tyre recs/wheel"), ("rim_recs", "rim recs/wheel"),
                       ("stale_recs", "STALE recs/wheel"),
                       ("surf_max", "max wheel surf spd"), ("omega_max", "max omega rad/s"),
                       ("gear_max", "max gear"), ("rpm_max", "max rpm"), ("tq_max", "max torque Nm"),
                       ("tilt_rms", "TILT RMS jolt-ode"), ("tilt_jolt_max", "tilt max jolt"),
                       ("tilt_ode_max", "tilt max ode"),
                       ("ode_travel", "travelXZ ODE (m)"), ("jolt_travel", "travelXZ Jolt (m)"),
                       ("ratio", "travel Jolt/ODE"),
                       ("pos_div", "pos divergence (m)"), ("max_angle", "max angle (deg)")):
        a = agg(key)
        if a is None:
            print("  %-22s NO DATA (not 0 - nothing was parsed)" % label)
            continue
        m, sd, lo, hi = a
        print("  %-22s mean=%9.3f sd=%8.3f  [%9.3f .. %9.3f]" % (label, m, sd, lo, hi))

    print("\n  weight (chassis) = %.0f N -> per-corner static load = %.0f N"
          % (rows[0].get("weight", 0.0), rows[0].get("weight", 0.0) / 4.0))
    rim_only = agg("rim_only_pct")[0]
    pinned = agg("pinned_pct")[0]
    print("  VERDICT: %s"
          % ("CONFIRMED - the band is bang-bang, only rim records survive reprojection"
             if rim_only > 80.0 and pinned > 80.0 else
             "NOT confirmed - see the numbers above"))


if __name__ == "__main__":
    main()
