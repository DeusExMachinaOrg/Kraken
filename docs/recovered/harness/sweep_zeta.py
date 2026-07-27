"""docs §69: plan risk #8's recalibration pass - sweep the tyre damping ratio zeta_t.

WHY THIS AXIS FIRST, and why the range is not arbitrary: risk #8 states the basis change
invalidates §42-46's calibration because c_t = 2*zeta_t*sqrt(k_t*m) and m went from the SPRUNG
per-corner mass (~42 kg) to the wheel body's own (11 kg). That scales c_t by sqrt(11/42) = 0.51,
i.e. the damping is now about HALF what §42-46 tuned. Restoring the old c_t would need
zeta_t = 0.5 / 0.51 = 0.98. So the sweep brackets that: the current value, the compensated value,
and one beyond it - a prediction to test, not a search.

OBJECTIVE: RMS(jolt_tilt - ode_tilt), not max_angle. This scenario throws BOTH sides around
(ODE alone reaches 46 deg), so the angle BETWEEN orientations cannot separate "the model is wrong"
from "the terrain is violent". Tilt RMS asks the question a calibration should: does the shadow
follow ODE's attitude?
"""
import importlib.util, os, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("b95", os.path.join(HERE, "band95.py"))
b95 = importlib.util.module_from_spec(spec); spec.loader.exec_module(b95)
b1c = b95.b1c

REPEATS = int(sys.argv[1]) if len(sys.argv) > 1 else 2
VALUES  = [float(x) for x in sys.argv[2].split(",")] if len(sys.argv) > 2 else [0.5, 1.0, 2.0]
# docs §69.6: the axis is a parameter now, and the OBJECTIVE is travel Jolt/ODE - the only
# metric measured with a spread (2%) small against the effect being chased. tilt and pos-div
# have spreads comparable to their own signal and cannot resolve a parameter change.
KEY     = sys.argv[3] if len(sys.argv) > 3 else "tyre_damping"

ini = dict(b95.BASE_INI)
ini[("jolt_harness", "wm4_max_dt")] = 0
ini[("jolt_harness", "wheelmodel")] = 4
original = b1c.select_save("00000009")
results = {}
try:
    for z in VALUES:
        cell = dict(ini); cell[("wheelmodel", KEY)] = z
        b1c.set_ini(cell)
        print("=== %s = %s ===" % (KEY, z), flush=True)
        rows = []
        for i in range(REPEATS):
            r = b1c.one_run("sw_%s_%d" % (str(z).replace(".", ""), i), drive=4.0)
            if r is None:
                print("  run failed"); continue
            r.update(b95.scan_log()); rows.append(r)
            print("  travel=%.3f  (ODE %.1f m, Jolt %.1f m)  tiltRMS=%6.2f pos-div=%6.2f"
                  % (r.get("ratio", 0), r.get("ode_travel", 0), r.get("jolt_travel", 0),
                     r.get("tilt_rms", 0), r.get("pos_div", 0)), flush=True)
        results[z] = rows
finally:
    b1c.restore(original)

print("\n=== zeta_t sweep (objective = tilt RMS, lower is better) ===")
print("  %-8s %-4s %10s %10s %10s %10s" % (KEY, "n", "travel", "sd", "tiltRMS", "pos_div"))
for z in VALUES:
    rows = results.get(z, [])
    if not rows:
        print("  %-8s  no data" % z); continue
    tr = [r.get("ratio", 0) for r in rows]
    print("  %-8s %-4d %10.3f %10.3f %10.2f %10.2f"
          % (z, len(rows), st.mean(tr), (st.stdev(tr) if len(tr) > 1 else 0.0),
             st.mean([r.get("tilt_rms", 0) for r in rows]),
             st.mean([r.get("pos_div", 0) for r in rows])))
