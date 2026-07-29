"""One game launch, many scenarios. docs §115.

Every measurement script so far relaunched hta.exe per run, because wm_cfg.py is built that way -
launch, pin, one scenario, kill. That costs ~90 s of process startup and save load per data point,
and it is why an n=5 comparison took half an hour and why several tests were run at n=1.

None of it was necessary. The harness protocol is already a drop box: trigger.txt starts a scenario
in the LIVE process and output_<token>.done reports it finished. So a session can launch once, then
run scenario after scenario. Combined with variant shadows (several parameter arms inside one run)
and in-session save loading, a whole comparison fits in one or two launches.

What this module keeps from wm_cfg.py, deliberately, is the STRICT part: the vehicle-fingerprint
check that aborts rather than measure the wrong vehicle, and the DLL-staleness check. Those exist
because both failure modes have silently contaminated real measurements here.

Usage:
    s = Session()
    s.start(vehicle="Molokovoz01")
    s.load_save("00000006")            # mid-session, no relaunch
    rows, status = s.run(scenario, "hb0")
    s.stop()
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedir
import harness

BUILD_RE = re.compile(r"built \(player\): (\d+) wheels \((\d+) driven axle\(s\)\), "
                      r"mass=([\d.]+), chassis=([\d.x]+)")
FINGERPRINTS = {
    "Molokovoz01": ("mass=167.0", "chassis=3.50x2.50x9.00"),
    "Scout01":     ("mass=100.0", "chassis=4.00x1.00x2.00"),
    "Ural01":      ("mass=303.0", "chassis=5.00x3.00x12.00"),
    "Bug01":       ("mass=132.0", "chassis=3.00x2.00x8.00"),
}


def kraken_autoload_enabled():
    """Read [testharness] autoload_save straight from the ini the game will read."""
    try:
        with open(gamedir.INI, encoding="utf-8", errors="replace") as f:
            section = None
            for line in f:
                s = line.strip()
                if s.startswith("[") and s.endswith("]"):
                    section = s[1:-1]
                elif section == "testharness" and s.startswith("autoload_save"):
                    return s.split("=", 1)[1].strip() not in ("0", "")
    except OSError:
        pass
    return False


class Session:
    def __init__(self, base_dir=None):
        self.base_dir = base_dir or gamedir.BASE_DIR
        self.started = False
        self._pin_nonce = 0

    # -- lifecycle ---------------------------------------------------------------------------
    def _kill(self):
        subprocess.run(["taskkill", "/F", "/IM", "hta.exe"], capture_output=True)
        time.sleep(2.0)

    def _log(self):
        try:
            with open(gamedir.LOG, encoding="utf-8", errors="replace") as f:
                return f.readlines()
        except OSError:
            return []

    def _clear_dropbox(self):
        for name in ("trigger.txt", "switch_vehicle.txt", "load_save.txt"):
            p = os.path.join(self.base_dir, name)
            if os.path.exists(p):
                with open(p, "w") as f:
                    f.write("")

    def start(self, vehicle=None, save=None, wait=120.0):
        """Launch the game, reach the menu, then load `save` and pin `vehicle` explicitly."""
        gamedir.check_dll_current()      # docs §109.1 - never measure a stale build
        self._kill()
        os.makedirs(self.base_dir, exist_ok=True)
        self._clear_dropbox()
        subprocess.Popen([gamedir.EXE], cwd=gamedir.WORKDIR, close_fds=True)
        # Wait only for the game to reach the menu - the placeholder shadow (mass=100, chassis
        # 6.00x1.00x3.00) is the signal that it is alive and ticking.
        deadline = time.time() + wait
        while time.time() < deadline:
            time.sleep(3.0)
            if any("built (player)" in l for l in self._log()):
                break
        else:
            self._kill()
            raise RuntimeError("game did not reach the menu within %.0fs" % wait)
        time.sleep(6.0)
        self.started = True
        # The level is loaded EXPLICITLY rather than by waiting on autoload. Autoload fires once
        # per process, after a frame-count delay, and picks by directory mtime - so it is both
        # timing-dependent (one launch here sat at the menu for 90 s and never fired) and unable to
        # choose a save. Driving it through load_save.txt makes which-save-and-when deterministic,
        # and it is the same call either way.
        if save:
            self.load_save(save, vehicle=vehicle)
        elif vehicle:
            self.pin_vehicle(vehicle)

    def stop(self):
        self._kill()
        self.started = False

    # -- in-session control ------------------------------------------------------------------
    def pin_vehicle(self, name, attempts=3):
        """Switch the player to a named prototype and CONFIRM it by build fingerprint.

        The confirmation is the point. A pin that silently does not take leaves the test measuring
        whatever the save happened to contain, which has contaminated a real run here before.
        """
        fp = FINGERPRINTS.get(name)
        for i in range(attempts):
            # docs §115: '#<nonce>' keeps every request distinct. The game debounces on the token
            # changing, so re-pinning the SAME vehicle - which is what a re-pin after a save load
            # is - would otherwise be dropped silently and leave the save's own vehicle in place.
            self._pin_nonce += 1
            with open(os.path.join(self.base_dir, "switch_vehicle.txt"), "w") as f:
                f.write("%s#%d\n" % (name, self._pin_nonce))
            time.sleep(8.0)
            last = None
            for line in self._log():
                if "built (player)" in line:
                    last = line
            if last and (fp is None or all(t in last for t in fp)):
                return last
            time.sleep(4.0)   # a switch right after a save load can take a moment to rebuild
        raise RuntimeError("could not confirm vehicle %r (fingerprint %s) after %d attempts; "
                           "last build line: %r" % (name, fp, attempts, last))

    def load_save(self, save, vehicle=None, wait=40.0):
        """Load a save WITHOUT relaunching (docs §115). Re-pins the vehicle afterwards if asked."""
        before = len(self._log())
        with open(os.path.join(self.base_dir, "load_save.txt"), "w") as f:
            f.write(save + "\n")
        deadline = time.time() + wait
        while time.time() < deadline:
            time.sleep(2.0)
            tail = self._log()[before:]
            if any("load_save: LoadSavedGame returned" in l for l in tail):
                break
        else:
            raise RuntimeError("save %s did not load within %.0fs" % (save, wait))
        time.sleep(6.0)
        if vehicle:
            self.pin_vehicle(vehicle)

    def run(self, scenario, token, extra_timeout=10.0):
        """Write, trigger and wait for one scenario in the LIVE process. Returns (rows, status)."""
        harness.write_scenario(scenario, base_dir=self.base_dir)
        for ext in (".done", ".csv"):
            p = os.path.join(self.base_dir, "output_%s%s" % (token, ext))
            if os.path.exists(p):
                os.remove(p)
        end = max(s.t for s in scenario.samples)
        harness.trigger_run(base_dir=self.base_dir, token=token)
        try:
            status = harness.wait_for_done(token, base_dir=self.base_dir, timeout=end + extra_timeout)
        except TimeoutError:
            # A scenario that never reports back means the process is wedged or gone. Returning a
            # status instead of raising is what lets a long session lose ONE data point rather than
            # all of them - a whole 13-scenario run died here once because run 4 hung.
            return [], "timeout"
        rows = harness.load_telemetry(token, base_dir=self.base_dir) if status == "ok" else []
        return rows, status

    def alive(self):
        """Is the process there AND still answering? A wedged game keeps its PID, so a liveness
        check on the process table alone reports healthy while every scenario times out - which is
        how one run lost eight consecutive data points to a 'still alive' hang."""
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq hta.exe"],
                             capture_output=True, text=True)
        if "hta.exe" not in (out.stdout or ""):
            return False
        # It answers if the log is still growing.
        n0 = len(self._log())
        time.sleep(3.0)
        return len(self._log()) > n0

    def restart(self, vehicle=None, save=None):
        """Bring the session back after a hang, so the remaining work still gets measured."""
        self.start(vehicle=vehicle, save=save)

    def log_since(self, n):
        return self._log()[n:]

    def log_len(self):
        return len(self._log())
