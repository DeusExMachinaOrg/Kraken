"""docs §139: fleet-wide accel/brake/turn baseline against ODE - the table stage2-plan.md
§2.5 and §124.11 never produced (those checked build/crash safety at default params, not
Jolt-vs-ODE magnitude). One launch, one save (00000009, already proven safe fleet-wide),
all 23 established "wheeled prototypes" (stage2-plan.md §2.5), two scenarios each:
straight accel/brake, and accel+hard-steer+brake (loads the lateral contact slot, same as
§124.11's own fleet sweep). [jolt_harness] apply=0 for the whole run - apply=1 would let
Jolt overwrite the ODE body every frame, corrupting the very ground truth this measures
(the autotuner's own rule, joltshadow.cpp:7452-7453).

Usage: python fleet_calibration_sweep.py [output.json] [--vehicles Bug01,Fighter01,...]
       [--tuning susp_damping=1.15,friction_long=0.95,...]

--tuning docs §139.5: overrides [jolt_harness] susp_frequency/susp_damping/friction_long/
friction_lat on top of RUN's defaults, for A/B-verifying an autotune-found candidate (or any
manual guess) against this same baseline measurement before touching config.cpp's real
defaults.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import baseline1c
import gamedir
from session import Session
from harness import Scenario, Sample

SAVE = "00000009"
SETTLE = 3.0
DRIVE = 8.0
BRAKE = 3.0

ROSTER = [
    "Molokovoz01", "Scout01", "Ural01", "Bug01", "Belaz01", "Cruiser01", "Mirotvorec01",
    "Hunter01", "Dozer01", "Traktor01", "Fighter01", "Fighter02", "Fighter03", "Hunter02",
    "Scout02", "Scout03", "ArcadeScout01", "Formula01", "Sml101", "Sml201", "Sml301",
    "Sml401", "Tank01",
]

RUN = {
    ("jolt", "enabled"): 1, ("jolt_harness", "shadow"): 1, ("jolt_harness", "apply"): 0,
    ("jolt_harness", "player_only"): 1, ("jolt_harness", "ai"): 0,
    ("jolt_harness", "wheelmodel"): 4, ("jolt_harness", "wm4_contact_constraint"): 1,
    ("jolt_harness", "wm4_per_wheel_mu"): 0, ("jolt_harness", "wm4_diag_interval"): 60,
    ("jolt_harness", "deferred_destroy"): 0,
    ("testharness", "enabled"): 1, ("testharness", "autoload_save"): 0,
    ("testharness", "god_mode"): 0, ("testharness", "perfmon"): 0,
}
SAFE = {
    ("jolt", "enabled"): 1, ("jolt_harness", "shadow"): 1, ("jolt_harness", "apply"): 1,
    ("jolt_harness", "player_only"): 0, ("jolt_harness", "ai"): 1,
    ("jolt_harness", "wheelmodel"): 4, ("jolt_harness", "wm4_contact_constraint"): 1,
    ("jolt_harness", "wm4_per_wheel_mu"): 0, ("jolt_harness", "wm4_diag_interval"): 60,
    ("jolt_harness", "deferred_destroy"): 0,
    # docs §139.6: --tuning can override any of TUNING_KEYS for the RUN - without an explicit
    # reset here, a --tuning run left the ini contaminated with its candidate values after exit
    # (found live: had to restore these by hand more than once). SAFE always resets every
    # TUNING_KEYS-capable knob back to the project's own established baseline, whether or not
    # this particular invocation happened to touch it.
    ("jolt_harness", "susp_frequency"): 1.0, ("jolt_harness", "susp_damping"): 1.0,
    ("jolt_harness", "friction_long"): 0.93, ("jolt_harness", "friction_lat"): 1.0,
    ("jolt_harness", "chassis_inertia_ode_box"): 0,
    ("testharness", "enabled"): 0, ("testharness", "autoload_save"): 0,
    ("testharness", "perfmon"): 0,
}


def scenario_straight():
    return Scenario(samples=[
        Sample(0.0, throttle=0.0, steer=0.0, brake=0.0, handbrake=False),
        Sample(SETTLE, throttle=0.0, steer=0.0, brake=0.0, handbrake=False),
        Sample(SETTLE + 0.05, throttle=1.0, steer=0.0, brake=0.0, handbrake=False),
        Sample(SETTLE + DRIVE, throttle=1.0, steer=0.0, brake=0.0, handbrake=False),
        Sample(SETTLE + DRIVE + 0.05, throttle=0.0, steer=0.0, brake=1.0, handbrake=False),
        Sample(SETTLE + DRIVE + BRAKE, throttle=0.0, steer=0.0, brake=1.0, handbrake=False),
    ])


def scenario_steer():
    # docs §124.11: WM_STEER=1.0-equivalent - loads the lateral (kSide) contact slot, which
    # a straight line barely touches.
    return Scenario(samples=[
        Sample(0.0, throttle=0.0, steer=0.0, brake=0.0, handbrake=False),
        Sample(SETTLE, throttle=0.0, steer=0.0, brake=0.0, handbrake=False),
        Sample(SETTLE + 0.05, throttle=1.0, steer=1.0, brake=0.0, handbrake=False),
        Sample(SETTLE + DRIVE, throttle=1.0, steer=1.0, brake=0.0, handbrake=False),
        Sample(SETTLE + DRIVE + 0.05, throttle=0.0, steer=0.0, brake=1.0, handbrake=False),
        Sample(SETTLE + DRIVE + BRAKE, throttle=0.0, steer=0.0, brake=1.0, handbrake=False),
    ])


SCENARIOS = {"accel_brake": scenario_straight, "accel_steer_brake": scenario_steer}

ts_re = re.compile(r"(\d\d:\d\d:\d\d\.\d\d\d)")
div_re = re.compile(
    r"Shadow divergence \(player\): pos=([\-\d.]+)m vel=([\-\d.]+)m/s angle=([\-\d.]+)deg "
    r"\(jolt com=\[([\-\d. ]+)\] ode com=\[([\-\d. ]+)\]\)")


def secs(ts):
    h, m, s = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def epoch_to_secs(epoch):
    lt = time.localtime(epoch)
    return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + (epoch - int(epoch))


def horiz(a, b):
    return ((a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def analyze(lines, trigger_epoch):
    """Same metric as wm_cfg.py's analyze(), adapted to work off an already-sliced line list
    (Session.log_since()) instead of re-scanning the whole file by wall-clock cutoff."""
    div = []
    for line in lines:
        tm = ts_re.search(line)
        d = div_re.search(line)
        if not tm or not d:
            continue
        jc = [float(x) for x in d.group(4).split()]
        oc = [float(x) for x in d.group(5).split()]
        div.append((secs(tm.group(1)), float(d.group(1)), float(d.group(3)), jc, oc))
    if not div:
        return {"error": "no Shadow divergence lines in this window"}
    throttle_end_t = epoch_to_secs(trigger_epoch) + SETTLE + DRIVE
    at_end = [d for d in div if d[0] <= throttle_end_t] or [div[-1]]
    end = at_end[-1]
    ode_tr = horiz(end[4], div[0][4])
    jolt_tr = horiz(end[3], div[0][3])
    ratio = jolt_tr / ode_tr if ode_tr > 0.5 else None
    pos = [d[1] for d in div]
    ang = [d[2] for d in div]
    return {
        "samples": len(div),
        "pos_div_at_throttle_end": end[1],
        "angle_div_at_throttle_end": end[2],
        "ode_travel_m": ode_tr,
        "jolt_travel_m": jolt_tr,
        "ratio": ratio,
        "pos_div_last": pos[-1],
        "angle_div_last": ang[-1],
        "max_pos_div": max(pos),
        "max_angle_div": max(ang),
    }


TUNING_KEYS = {"susp_frequency", "susp_damping", "friction_long", "friction_lat",
               "chassis_inertia_ode_box"}


def main():
    args = sys.argv[1:]
    roster = ROSTER
    out_path = None
    run_ini = dict(RUN)
    i = 0
    while i < len(args):
        if args[i] == "--vehicles":
            roster = args[i + 1].split(",")
            i += 2
        elif args[i] == "--tuning":
            for pair in args[i + 1].split(","):
                key, val = pair.split("=", 1)
                if key not in TUNING_KEYS:
                    raise SystemExit("--tuning: unknown key %r (must be one of %r)" % (key, TUNING_KEYS))
                run_ini[("jolt_harness", key)] = float(val)
            i += 2
        else:
            out_path = args[i]
            i += 1
    if out_path is None:
        out_path = "fleet_calibration_baseline.json"
    baseline1c.set_ini(run_ini)
    results = {}
    try:
        for i, veh in enumerate(roster):
            print("=== %s (%d/%d) ===" % (veh, i + 1, len(roster)), flush=True)
            # docs §139 (found live, TWO sweep attempts): (1) a bare pin_vehicle() leaves the new
            # vehicle in the PREVIOUS vehicle's pose (wm_cfg.py's own §67.7 warning) - across a
            # 23-vehicle roster spanning 2m to 18m chassis lengths that meant every switch
            # dropped/embedded the new vehicle, producing meaningless divergence. (2) reloading the
            # SAVE repeatedly inside ONE launch instead - fixed the pose problem but chained ~20
            # rebuilds' worth of the leak-forever wheel-contact-constraint convention (docs §124.1)
            # into one process, and it crashed for real (ACCESS VIOLATION, total=124 leaked
            # constraints in the log right before the crash) around vehicle #7. A FULL RELAUNCH per
            # vehicle - its own Session, stopped before the next one starts - is slower but matches
            # this project's own established practice for behavioral (not just crash-safety)
            # measurement (wm_cfg.py's "relaunch-per-run" discipline, used throughout §122-124) and
            # never accumulates more than one vehicle's worth of rebuilt bodies/constraints.
            s = Session()
            try:
                s.start(vehicle=veh, save=SAVE)
                veh_result = {}
                for label, build in SCENARIOS.items():
                    before = s.log_len()
                    trigger_epoch = time.time()
                    rows, status = s.run(build(), "fleet_%s_%s" % (veh, label))
                    lines = s.log_since(before)
                    if status != "ok":
                        veh_result[label] = {"error": "scenario status=%s" % status}
                        print("  %s: FAILED status=%s" % (label, status))
                        continue
                    metrics = analyze(lines, trigger_epoch)
                    veh_result[label] = metrics
                    print("  %s: %s" % (label, metrics))
                results[veh] = veh_result
            except Exception as e:
                print("  ABORTED for %s: %s" % (veh, e), flush=True)
                results[veh] = {"error": str(e)}
            finally:
                s.stop()
    finally:
        baseline1c.set_ini(SAFE)
        with open(out_path, "w") as f:
            json.dump(results, f, indent=2)
        print("\nwrote %s" % out_path)


if __name__ == "__main__":
    main()
