"""One game launch; pin each vehicle in turn and record its real build fingerprint + wheel data.

Exists so the step-5 cross-vehicle test can keep wm_cfg.py's STRICT fingerprint check (docs §44:
a relaxed check silently contaminated a whole run once) instead of trusting the pin to have worked.
Also captures each vehicle's wheel radius and mass from the axis-audit line, which is what the
inertia prediction needs and which is NOT in vehicleparts.xml.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, r"F:\Kraken\tools\testharness")

BASE_DIR = r"F:\HTA_Kraken\data\kraken_testharness"
LOG = r"F:\HTA_Kraken\kraken.log"
EXE = r"F:\HTA_Kraken\hta.exe"
WORKDIR = r"F:\HTA_Kraken"

VEHICLES = sys.argv[1:] or ["Molokovoz01", "Scout01", "Ural01"]

AUDIT_RE = re.compile(r"axis-audit \(player\) w=(\d+):.*?R=([\d.]+) mWheel=([\d.]+) driven=(\d) steer=(\d)")
BUILD_RE = re.compile(r"built \(player\): (\d+) wheels \((\d+) driven axle\(s\)\), mass=([\d.]+), chassis=([\d.x]+)")


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "hta.exe"], capture_output=True)
    time.sleep(2.0)


def read_log():
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            return f.readlines()
    except OSError:
        return []


def main():
    kill()
    for name in ("trigger.txt", "switch_vehicle.txt"):
        with open(os.path.join(BASE_DIR, name), "w") as f:
            f.write("")

    subprocess.Popen([EXE], cwd=WORKDIR, close_fds=True)
    deadline = time.time() + 75.0
    while time.time() < deadline:
        time.sleep(3.0)
        if any("built (player)" in l for l in read_log()):
            break
    time.sleep(6.0)

    results = {}
    for veh in VEHICLES:
        before = len(read_log())
        with open(os.path.join(BASE_DIR, "switch_vehicle.txt"), "w") as f:
            f.write(veh + "\n")
        time.sleep(6.0)
        tail = read_log()[before:]
        build = None
        wheels = []
        for line in tail:
            m = BUILD_RE.search(line)
            if m:
                build = m.groups()
                wheels = []          # audit lines precede their own build line; reset per build
            a = AUDIT_RE.search(line)
            if a:
                wheels.append((int(a.group(1)), float(a.group(2)), float(a.group(3)),
                               int(a.group(4)), int(a.group(5))))
        if build is None:
            print("%-14s NO REBUILD (already this vehicle, or the name is wrong)" % veh)
            continue
        nw, axles, mass, chassis = build
        results[veh] = {"n": int(nw), "mass": float(mass), "chassis": chassis, "wheels": wheels}
        print("%-14s wheels=%s driven_axles=%s mass=%s chassis=%s" % (veh, nw, axles, mass, chassis))
        print("%-14s fingerprint: mass=%s|chassis=%s" % ("", mass, chassis))
        for w, R, mW, drv, st in wheels:
            print("%-14s   w=%d R=%.3f mWheel=%.1f driven=%d steer=%d" % ("", w, R, mW, drv, st))
    kill()

    print("\n=== inertia prediction (effective inertial mass ratio mode2/mode4) ===")
    for veh, d in results.items():
        if not d["wheels"]:
            print("%-14s no audit lines captured" % veh)
            continue
        M = d["mass"]
        # mode 4: chassis + wheels' translational mass + their rotational I/R^2 (= 0.5*m for a disc)
        m4 = M + sum(w[2] for w in d["wheels"]) + sum(0.5 * w[2] for w in d["wheels"])
        # mode 2: chassis + the flat config inertia reflected at each wheel, 30/R^2
        m2 = M + sum(30.0 / (w[1] * w[1]) for w in d["wheels"])
        print("%-14s mode2=%7.0f kg  mode4=%7.0f kg  predicted travel-ratio gap = %.2fx"
              % (veh, m2, m4, m2 / m4))


if __name__ == "__main__":
    main()
