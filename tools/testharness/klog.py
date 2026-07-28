"""Safe reader for F:\\HTA_Kraken\\kraken.log. Use this INSTEAD of hand-written grep.

WHY IT EXISTS: autoload rebuilds the Jolt shadow 2-3 times per launch - a menu/placeholder
vehicle first, then the save's real one, then any pinned prototype - so the first matching line
in the file is almost never the vehicle under test. That cost three false investigations in one
session (docs §67.5, §71.1): a wheel mass of 1.0 kg and a disc inertia of 0.86 were both read off
the MENU car and chased as bugs that did not exist.

The rule ("anchor at the last `built (player)` line") is easy to state and easy to forget under
time pressure, so it is mechanised here instead:
  * every extraction is anchored at the last build automatically;
  * every extraction PRINTS the vehicle it belongs to first, so the output is self-labelling and
    a wrong-vehicle read is visible in the output rather than silent;
  * it REFUSES to print anything if no build line exists, instead of returning an empty result
    that reads like "the feature produced nothing".

Usage:
    python klog.py <regex> [more regexes ...]     # anchored extraction, with provenance header
    python klog.py --builds                       # list every build in the log, in order
    python klog.py --check                        # just print which vehicle is active
Options:
    --all      search the WHOLE file (prints a loud warning; use only to inspect earlier builds)
    --count    print match counts instead of the lines

NOTE on patterns: the log's section sign is written by the game in a non-UTF-8 codepage, so a
pattern containing a literal § round-trips only by luck. Prefer a plain-ASCII substring that
appears next to it - "tyre band", "drivetrain inertia", "slipdense" - which cannot be affected.
"""
import io
import os
import re
import sys

# Paths come from gamedir.py - the single place that knows where the game is installed.
# The recovered copy of this file hard-coded F:\HTA_Kraken, which no longer exists.
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import gamedir


LOG = os.environ.get("KRAKEN_LOG", gamedir.LOG)

BUILD_RE = re.compile(
    r"Shadow vehicle #(\d+) built \((\w+)\): (\d+) wheels .*?mass=([\d.]+), chassis=([\d.x]+)")


def read_lines():
    try:
        with io.open(LOG, encoding="utf-8", errors="replace") as f:
            return f.readlines()
    except OSError as e:
        sys.exit("cannot read %s: %s" % (LOG, e))


def builds(lines):
    """Every shadow build in the log, as (line_index, serial, label, wheels, mass, chassis)."""
    out = []
    for i, line in enumerate(lines):
        m = BUILD_RE.search(line)
        if m:
            out.append((i,) + m.groups())
    return out


def anchor(lines):
    """Index of the last build line, and its description. Refuses rather than guessing."""
    bs = builds(lines)
    if not bs:
        sys.exit("REFUSING: no 'Shadow vehicle #N built' line in the log - there is no vehicle to "
                 "attribute anything to. Did the run reach a loaded save?")
    return bs[-1]


def main():
    args = [a for a in sys.argv[1:]]
    whole = "--all" in args
    counting = "--count" in args
    args = [a for a in args if not a.startswith("--")]
    lines = read_lines()

    if "--builds" in sys.argv[1:]:
        for i, serial, label, wheels, mass, chassis in builds(lines):
            print("  line %-6d build #%-3s (%s) %s wheels mass=%s chassis=%s"
                  % (i + 1, serial, label, wheels, mass, chassis))
        return

    idx, serial, label, wheels, mass, chassis = anchor(lines)
    print("== active vehicle: build #%s (%s) %s wheels mass=%s chassis=%s  [log line %d] =="
          % (serial, label, wheels, mass, chassis, idx + 1))
    if whole:
        print("!! --all: searching the WHOLE file, so matches may belong to EARLIER vehicles !!")
    tail = lines if whole else lines[idx:]
    if "--check" in sys.argv[1:] or not args:
        print("   (%d lines after the last build)" % len(tail))
        return

    for pattern in args:
        rx = re.compile(pattern)
        hits = [l.rstrip() for l in tail if rx.search(l)]
        print("\n-- %s : %d match(es) --" % (pattern, len(hits)))
        if counting:
            continue
        for h in hits:
            print("   " + re.sub(r"^.*?joltshadow - ", "", h))


if __name__ == "__main__":
    main()
