"""One-off sweep for docs §122.16/122.17: pin several vehicles in one session and collect the
chassis-inertia build-time diagnostic line for each. No new instrumentation - the LOG_INFO already
exists in joltshadow.cpp; this just drives the existing harness (Session) to trigger a shadow
rebuild per vehicle and greps the resulting kraken.log lines.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedir
from session import Session

VEHICLES = ["Scout01", "Ural01", "Bug01"]

INERTIA_RE = re.compile(r"122\.16: chassis inertia \(player\).*")


def main():
    s = Session()
    s.start(save="00000009", vehicle="Molokovoz01")
    print("=== session started, save 00000009 loaded, Molokovoz01 pinned ===", flush=True)

    results = []
    for veh in VEHICLES:
        before = s.log_len()
        try:
            s.pin_vehicle(veh)
        except RuntimeError as e:
            print("%-14s PIN FAILED: %s" % (veh, e), flush=True)
            continue
        tail = s.log_since(before)
        lines = [l.strip() for l in tail if "122.16: chassis inertia (player)" in l]
        if not lines:
            print("%-14s NO §122.16 LINE CAPTURED" % veh, flush=True)
            continue
        line = lines[-1]
        print("%-14s %s" % (veh, line), flush=True)
        results.append((veh, line))

    s.stop()
    print("\n=== done, %d/%d vehicles captured ===" % (len(results), len(VEHICLES)))


if __name__ == "__main__":
    main()
