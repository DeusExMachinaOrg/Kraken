"""docs §140.17: fleet steady-turn comparison using absolute yaw-rate error.

The old fleet sweep used one hard-steer run and a travel ratio.  This runner uses the settled
steady-turn scenario from §140.13 and compares ``abs(jolt_yaw - ode_yaw)`` for wm4 friction modes
0 and 2.  It deliberately keeps one relaunch per vehicle/mode/run: the physics project has
already demonstrated that chaining vehicle rebuilds leaks constraints and eventually crashes.

Usage:
    python fleet_steady_turn_modes.py [output.json]
        [--vehicles A,B,C] [--n 1] [--steer 0.35]
"""
import json
import math
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import baseline1c
import fleet_calibration_sweep as fcs
from harness import Sample, Scenario
from session import Session

STEER = 0.35
N = 1
TURN_T = 6.05
END_T = 15.0
WINDOW = (11.0, 15.0)
DIV = re.compile(r"angle=[-\d.]+deg yaw=\(([-\d.]+)/([-\d.]+)\)deg/s \(jolt com=\[([-\d. ]+)\] ode com=\[([-\d. ]+)\]\)")
TS = re.compile(r"(\d\d):(\d\d):(\d\d\.\d\d\d)")


def scenario(steer):
    return Scenario(samples=[
        Sample(0.0, throttle=0.0, steer=0.0),
        Sample(2.0, throttle=0.0, steer=0.0),
        Sample(2.05, throttle=1.0, steer=0.0),
        Sample(6.0, throttle=1.0, steer=0.0),
        Sample(TURN_T, throttle=0.5, steer=steer),
        Sample(END_T, throttle=0.5, steer=steer),
    ])


def parse_divergence(lines):
    out = []
    for line in lines:
        tm = TS.search(line)
        m = DIV.search(line)
        if not tm or not m:
            continue
        t = int(tm.group(1)) * 3600 + int(tm.group(2)) * 60 + float(tm.group(3))
        out.append((t, [float(x) for x in m.group(3).split()],
                    [float(x) for x in m.group(4).split()], float(m.group(1)), float(m.group(2))))
    return out


def yaw_rate(rows, index, t0, t1):
    sub = [r for r in rows if t0 <= r[0] <= t1]
    rates = []
    for i in range(2, len(sub)):
        a, b, c = sub[i - 2][index], sub[i - 1][index], sub[i][index]
        h1 = math.atan2(b[0] - a[0], b[2] - a[2])
        h2 = math.atan2(c[0] - b[0], c[2] - b[2])
        if (math.hypot(b[0] - a[0], b[2] - a[2]) < 0.05 or
                math.hypot(c[0] - b[0], c[2] - b[2]) < 0.05):
            continue
        d = h2 - h1
        while d > math.pi:
            d -= 2.0 * math.pi
        while d < -math.pi:
            d += 2.0 * math.pi
        dt = sub[i][0] - sub[i - 1][0]
        if dt > 0:
            rates.append(abs(d) / dt * 180.0 / math.pi)
    return statistics.median(rates) if len(rates) >= 5 else None


def logged_yaw_rate(rows, index, t0, t1):
    """Median native world-up angular rate; works even when COM travel is zero."""
    values = [r[index] for r in rows if t0 <= r[0] <= t1]
    return statistics.median(values) if len(values) >= 5 else None


def run_one(vehicle, mode, index, steer):
    ini = dict(fcs.RUN)
    ini[("jolt_harness", "wm4_friction_constraint")] = mode
    ini[("jolt_harness", "wm4_diag_interval")] = 10
    baseline1c.set_ini(ini)
    session = Session()
    try:
        session.start(vehicle=vehicle, save=fcs.SAVE)
        before = session.log_len()
        _, status = session.run(scenario(steer), "fleet_turn_%s_m%d_%d" % (vehicle, mode, index))
        rows = parse_divergence(session.log_since(before))
        if status != "ok" or len(rows) < 20:
            result = {"status": status, "samples": len(rows)}
            if status == "crashed":
                result["exit_code"] = session.last_exit_code
                result["exception"] = session.last_exception
            return result
        t0 = rows[0][0]
        jolt = logged_yaw_rate(rows, 3, t0 + WINDOW[0], t0 + WINDOW[1])
        ode = logged_yaw_rate(rows, 4, t0 + WINDOW[0], t0 + WINDOW[1])
        if jolt is None or ode is None:
            return {"status": status, "samples": len(rows), "jolt_yaw_dps": jolt,
                    "ode_yaw_dps": ode}
        return {"status": status, "samples": len(rows), "jolt_yaw_dps": jolt,
                "ode_yaw_dps": ode, "abs_error_dps": abs(jolt - ode)}
    finally:
        session.stop()
        baseline1c.set_ini(fcs.SAFE)


def parse_args():
    out = "fleet_steady_turn_modes.json"
    vehicles = list(fcs.ROSTER)
    n = N
    steer = STEER
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--vehicles":
            vehicles = args[i + 1].split(",")
            i += 2
        elif args[i] == "--n":
            n = int(args[i + 1])
            i += 2
        elif args[i] == "--steer":
            steer = float(args[i + 1])
            i += 2
        else:
            out = args[i]
            i += 1
    return out, vehicles, n, steer


def main():
    out_path, vehicles, n, steer = parse_args()
    results = {}
    try:
        total = len(vehicles) * n * 2
        count = 0
        for vehicle in vehicles:
            results[vehicle] = {"mode0": [], "mode2": []}
            for index in range(n):
                for mode in (0, 2):
                    count += 1
                    print("=== %s mode%d (%d/%d) ===" % (vehicle, mode, count, total), flush=True)
                    try:
                        result = run_one(vehicle, mode, index, steer)
                        results[vehicle]["mode%d" % mode].append(result)
                        print("  %s" % result, flush=True)
                    except Exception as exc:
                        result = {"status": "exception", "error": str(exc)}
                        results[vehicle]["mode%d" % mode].append(result)
                        print("  ABORTED: %s" % exc, flush=True)
    finally:
        baseline1c.set_ini(fcs.SAFE)
        with open(out_path, "w") as handle:
            json.dump({"steer": steer, "n": n, "results": results}, handle, indent=2)
        print("wrote %s" % out_path)

    print("\nvehicle                 mode0 MAE   mode2 MAE   delta (mode2-mode0)")
    for vehicle in vehicles:
        vals = {}
        for mode in (0, 2):
            vals[mode] = [x["abs_error_dps"] for x in results[vehicle]["mode%d" % mode]
                          if "abs_error_dps" in x]
        a = statistics.mean(vals[0]) if vals[0] else None
        b = statistics.mean(vals[2]) if vals[2] else None
        delta = b - a if a is not None and b is not None else None
        fmt = lambda x: "-" if x is None else "%.3f" % x
        print("%-23s %10s %10s %18s" % (vehicle, fmt(a), fmt(b), fmt(delta)))


if __name__ == "__main__":
    main()
