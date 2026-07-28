"""
Driver/analysis tool for kraken's fix::testharness module.

Protocol (see Kraken/source/fix/testharness.cpp for the authoritative side):
  <base>/scenario.csv     - written by us before a run.
  <base>/trigger.txt      - single line token; changing it starts a new scenario.
  <base>/output_<tok>.csv - telemetry, one row per physics tick.
  <base>/output_<tok>.done- written by the game when the scenario finishes.

This file has no external dependencies (stdlib only) so it runs with any
Python 3 on the box the harness is driven from.
"""

import csv
import math
import os
import time
from dataclasses import dataclass, field

# Paths come from gamedir.py - the single place that knows where the game is installed.
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import gamedir
GAME_DIR = gamedir.GAME_DIR
BASE_DIR = os.path.join(GAME_DIR, "data", "kraken_testharness")

TELEMETRY_COLUMNS = [
    "t", "px", "py", "pz", "qx", "qy", "qz", "qw",
    "comx", "comy", "comz", "vx", "vy", "vz", "avx", "avy", "avz",
    "throttle", "steer", "brake", "handbrake",
    "gear", "engineRpm", "realThrottle", "wheelsTouchingGround",
    "numWheels", "drivenWheels", "drivenWheelsJointed",
]


@dataclass
class Sample:
    t: float
    throttle: float = 0.0
    steer: float = 0.0
    brake: float = 0.0
    handbrake: bool = False


@dataclass
class Scenario:
    samples: list
    spawn: tuple = None  # (x, y, z, qx, qy, qz, qw) or None to keep current position


def write_scenario(scenario: Scenario, base_dir: str = BASE_DIR) -> str:
    os.makedirs(base_dir, exist_ok=True)
    path = os.path.join(base_dir, "scenario.csv")
    with open(path, "w", newline="") as f:
        if scenario.spawn is not None:
            f.write("spawn," + ",".join(str(v) for v in scenario.spawn) + "\n")
        f.write("t,throttle,steer,brake,handbrake\n")
        for s in sorted(scenario.samples, key=lambda s: s.t):
            f.write(f"{s.t},{s.throttle},{s.steer},{s.brake},{1 if s.handbrake else 0}\n")
    return path


def trigger_run(base_dir: str = BASE_DIR, token: str = None) -> str:
    """Bump trigger.txt to a fresh token, which the game picks up on its next tick."""
    if token is None:
        token = str(int(time.time() * 1000))
    os.makedirs(base_dir, exist_ok=True)
    with open(os.path.join(base_dir, "trigger.txt"), "w") as f:
        f.write(token + "\n")
    return token


def wait_for_done(token: str, base_dir: str = BASE_DIR, timeout: float = 60.0, poll: float = 0.5) -> str:
    """Blocks until output_<token>.done appears (or timeout). Returns its content ('ok'/'no_vehicle')."""
    done_path = os.path.join(base_dir, f"output_{token}.done")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(done_path):
            with open(done_path) as f:
                return f.read().strip()
        time.sleep(poll)
    raise TimeoutError(f"scenario {token} did not finish within {timeout}s (is hta.exe running with [testharness] enabled=1?)")


def load_telemetry(token: str, base_dir: str = BASE_DIR):
    path = os.path.join(base_dir, f"output_{token}.csv")
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append({k: float(v) if k != "handbrake" else int(float(v)) for k, v in row.items()})
    return rows


def run_scenario(scenario: Scenario, base_dir: str = BASE_DIR, timeout: float = 60.0):
    """Full round trip: write scenario, trigger, wait, load telemetry. Returns (token, rows, status)."""
    write_scenario(scenario, base_dir)
    token = trigger_run(base_dir)
    status = wait_for_done(token, base_dir, timeout=timeout)
    rows = load_telemetry(token, base_dir) if status == "ok" else []
    return token, rows, status


def metrics(rows):
    """Basic single-run sanity metrics: distance traveled, max speed, final position/duration."""
    if not rows:
        return {}
    dist = 0.0
    max_speed = 0.0
    for a, b in zip(rows, rows[1:]):
        dx, dy, dz = b["px"] - a["px"], b["py"] - a["py"], b["pz"] - a["pz"]
        dist += math.sqrt(dx * dx + dy * dy + dz * dz)
    for r in rows:
        speed = math.sqrt(r["vx"] ** 2 + r["vy"] ** 2 + r["vz"] ** 2)
        max_speed = max(max_speed, speed)
    first, last = rows[0], rows[-1]
    return {
        "duration_s": last["t"] - first["t"],
        "samples": len(rows),
        "distance_m": dist,
        "max_speed_mps": max_speed,
        "start_pos": (first["px"], first["py"], first["pz"]),
        "end_pos": (last["px"], last["py"], last["pz"]),
        "net_displacement_m": math.sqrt(
            (last["px"] - first["px"]) ** 2 +
            (last["py"] - first["py"]) ** 2 +
            (last["pz"] - first["pz"]) ** 2
        ),
    }


def compare(rows_a, rows_b):
    """Aligns two runs by nearest timestamp and reports position/orientation RMSE.
    Intended for ODE-baseline vs Jolt-shadow comparisons once Jolt telemetry exists,
    but works today for ODE-vs-ODE determinism sanity checks too."""
    if not rows_a or not rows_b:
        return {}

    def pos_at(rows, t):
        best = min(rows, key=lambda r: abs(r["t"] - t))
        return best

    pos_sq_err = 0.0
    quat_angle_err = 0.0
    n = 0
    for a in rows_a:
        b = pos_at(rows_b, a["t"])
        dx, dy, dz = a["px"] - b["px"], a["py"] - b["py"], a["pz"] - b["pz"]
        pos_sq_err += dx * dx + dy * dy + dz * dz
        dot = abs(a["qx"] * b["qx"] + a["qy"] * b["qy"] + a["qz"] * b["qz"] + a["qw"] * b["qw"])
        dot = max(-1.0, min(1.0, dot))
        quat_angle_err += 2 * math.acos(dot)
        n += 1

    return {
        "samples_compared": n,
        "position_rmse_m": math.sqrt(pos_sq_err / n) if n else 0.0,
        "mean_orientation_error_rad": (quat_angle_err / n) if n else 0.0,
    }


def smoke_scenario() -> Scenario:
    """throttle full for 4s, coast 1s, brake to a stop for 3s - straight line sanity check."""
    samples = [
        Sample(t=0.0, throttle=0.0),
        Sample(t=0.1, throttle=1.0),
        Sample(t=4.0, throttle=1.0),
        Sample(t=4.1, throttle=0.0),
        Sample(t=5.0, throttle=0.0),
        Sample(t=5.1, throttle=0.0, brake=1.0),
        Sample(t=8.0, throttle=0.0, brake=1.0),
    ]
    return Scenario(samples=samples, spawn=None)


if __name__ == "__main__":
    import sys
    import json

    if len(sys.argv) > 1 and sys.argv[1] == "smoke":
        print(f"[harness] writing scenario + trigger under {BASE_DIR}")
        token, rows, status = run_scenario(smoke_scenario(), timeout=30.0)
        print(f"[harness] run {token} finished: {status}")
        if status == "ok":
            print(json.dumps(metrics(rows), indent=2))
        else:
            print("[harness] scenario did not complete cleanly (no vehicle found?) - check kraken.log")
    else:
        print("usage: python harness.py smoke")
