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
    # docs §139: rest of the established 23-vehicle "wheeled prototypes" roster
    # (stage2-plan.md §2.5), same values as tools/testharness/wm_cfg.py's own FINGERPRINTS
    # dict (kept duplicated, matching that file's existing precedent rather than introducing
    # a new shared-import dependency between the two).
    "Belaz01":       ("mass=392.0", "chassis=4.00x4.50x12.00"),
    "Mirotvorec01":  ("mass=411.0", "chassis=5.00x2.50x12.00"),
    "Cruiser01":     ("mass=104.0", "chassis=10.00x1.00x5.00"),
    "Scout02":       ("mass=100.0", "chassis=4.00x1.00x2.00"),
    "Scout03":       ("mass=100.0", "chassis=4.00x1.00x2.00"),
    "ArcadeScout01": ("mass=100.0", "chassis=3.00x2.00x2.00"),
    "Hunter01":      ("mass=100.0", "chassis=10.00x1.00x5.00"),
    "Hunter02":      ("mass=100.0", "chassis=10.00x1.00x5.00"),
    "Dozer01":       ("mass=127.0", "chassis=18.00x1.00x13.00"),
    "Traktor01":     ("mass=125.0", "chassis=18.00x1.00x13.00"),
    "Fighter01":     ("mass=100.0", "chassis=6.00x1.00x3.00"),
    "Fighter02":     ("mass=100.0", "chassis=6.00x1.00x3.00"),
    "Fighter03":     ("mass=100.0", "chassis=6.00x1.00x3.00"),
    "Formula01":     ("mass=351.0", "chassis=8.00x8.00x8.00"),
    "Sml101":        ("mass=100.0", "chassis=2.00x1.00x4.00"),
    "Sml201":        ("mass=100.0", "chassis=4.00x1.00x2.00"),
    "Sml301":        ("mass=100.0", "chassis=4.00x1.00x1.00"),
    "Sml401":        ("mass=100.0", "chassis=4.00x1.00x1.00"),
    "Tank01":        ("mass=187.0", "chassis=18.00x1.00x13.00"),
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
        self._process = None
        self.last_exit_code = None
        self.last_exception = None
        self._exception_baseline = {}

    # -- lifecycle ---------------------------------------------------------------------------
    def _kill(self):
        subprocess.run(["taskkill", "/F", "/IM", "hta.exe"], capture_output=True)
        # Longer than it looks like it needs to be. A force-killed game - and a wedged one always
        # is - takes a while to release its files, and relaunching too soon produced a process that
        # printed its init lines and then died without ever building a shadow, which reads as
        # "game did not reach the menu" 120 s later. That failure ended a 12-launch measurement on
        # its second point.
        time.sleep(6.0)

    def _terminate_owned_process(self):
        """Stop only the process started by this Session after an immediate failure."""
        process = self._process
        self._process = None
        if process is None or process.poll() is not None:
            return
        process.kill()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            pass

    def _raise_if_failed(self, context):
        exception_path = gamedir.new_exception(self._exception_baseline)
        if exception_path is not None:
            self.last_exception = exception_path
            self._terminate_owned_process()
            raise RuntimeError("new exception report appeared during %s: %s" %
                               (context, exception_path))
        if self._process is not None:
            exit_code = self._process.poll()
            if exit_code is not None:
                self.last_exit_code = exit_code
                self._process = None
                raise RuntimeError("hta.exe exited during %s with code %s" %
                                   (context, exit_code))

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

    def start(self, vehicle=None, save=None, wait=120.0, attempts=3):
        """Launch the game, reach the menu, then load `save` and pin `vehicle` explicitly.

        Retries the launch. The game fails to come up often enough under automation that a single
        attempt makes any multi-launch measurement a lottery - one dead launch used to abort a
        whole 12-point sweep at its second point.
        """
        gamedir.check_dll_current()      # docs §109.1 - never measure a stale build
        os.makedirs(self.base_dir, exist_ok=True)
        for attempt in range(attempts):
            self._kill()
            self._clear_dropbox()
            self.last_exit_code = None
            self.last_exception = None
            self._exception_baseline = gamedir.exception_snapshot()
            self._process = subprocess.Popen([gamedir.EXE], cwd=gamedir.WORKDIR, close_fds=True)
            # Wait only for the game to reach the menu - the placeholder shadow (mass=100, chassis
            # 6.00x1.00x3.00) is the signal that it is alive and ticking.
            deadline = time.time() + wait
            ok = False
            while time.time() < deadline:
                time.sleep(3.0)
                exception_path = gamedir.new_exception(self._exception_baseline)
                if exception_path is not None:
                    self.last_exception = exception_path
                    self._terminate_owned_process()
                    raise RuntimeError("new exception report appeared: %s" % exception_path)
                exit_code = self._process.poll()
                if exit_code is not None:
                    self.last_exit_code = exit_code
                    self._process = None
                    print("  (hta.exe exited during launch with code %s)" % exit_code, flush=True)
                    break
                if any("built (player)" in l for l in self._log()):
                    ok = True
                    break
            if ok:
                break
            print("  (launch attempt %d/%d did not reach the menu - retrying)"
                  % (attempt + 1, attempts), flush=True)
        else:
            self._kill()
            raise RuntimeError("game did not reach the menu within %.0fs x %d attempts"
                               % (wait, attempts))
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
        self._process = None
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
            deadline = time.time() + 8.0
            last = None
            while time.time() < deadline:
                self._raise_if_failed("vehicle switch")
                for line in self._log():
                    if "built (player)" in line:
                        last = line
                if last and (fp is None or all(t in last for t in fp)):
                    return last
                time.sleep(0.5)
            if last and (fp is None or all(t in last for t in fp)):
                return last
            time.sleep(4.0)   # a switch right after a save load can take a moment to rebuild
        raise RuntimeError("could not confirm vehicle %r (fingerprint %s) after %d attempts; "
                           "last build line: %r" % (name, fp, attempts, last))

    def load_save(self, save, vehicle=None, wait=40.0):
        """Load a save WITHOUT relaunching (docs §115). Re-pins the vehicle afterwards if asked."""
        before = len(self._log())
        # docs §139: '#<nonce>' keeps every request distinct, same reasoning and mechanism as
        # pin_vehicle's own nonce just below - the game debounces load_save.txt on the token
        # CONTENT (CheckSaveLoad, testharness.cpp), so reloading the SAME save twice in a row
        # (e.g. to reset a vehicle to its spawn pose between fleet-sweep pins) used to be silently
        # dropped and then reported as a 40s timeout for a request that was never even attempted.
        self._pin_nonce += 1
        with open(os.path.join(self.base_dir, "load_save.txt"), "w") as f:
            f.write("%s#%d\n" % (save, self._pin_nonce))
        deadline = time.time() + wait
        while time.time() < deadline:
            self._raise_if_failed("save load")
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
        # Wait on PROGRESS, not on a fixed deadline. The scenario clock advances with the game's
        # physics step, not with wall time, so under load it runs slower than real time - at 11.7
        # fps a 17 s scenario takes far longer than 17 s to play out. A fixed `end + slack` timeout
        # then reports "timeout" for a scenario that is running perfectly well, which is exactly
        # what made twelve consecutive measurement points fail and what probably explains the
        # "hangs" recorded earlier in the session.
        done_path = os.path.join(self.base_dir, "output_%s.done" % token)
        csv_path = os.path.join(self.base_dir, "output_%s.csv" % token)
        hard_deadline = time.time() + end + extra_timeout + 300.0
        last_size, last_change = -1, time.time()
        status = None
        while time.time() < hard_deadline:
            exception_path = gamedir.new_exception(self._exception_baseline)
            if exception_path is not None:
                self.last_exception = exception_path
                self._terminate_owned_process()
                return [], "crashed"
            if self._process is not None:
                exit_code = self._process.poll()
                if exit_code is not None:
                    self.last_exit_code = exit_code
                    return [], "crashed"
            if os.path.exists(done_path):
                with open(done_path) as f:
                    status = f.read().strip()
                break
            size = os.path.getsize(csv_path) if os.path.exists(csv_path) else -1
            if size != last_size:
                last_size, last_change = size, time.time()
            elif time.time() - last_change > 45.0:
                # Genuinely stuck: nothing written for 45 s and no completion marker.
                return [], "timeout"
            time.sleep(1.0)
        if status is None:
            return [], "timeout"
        rows = harness.load_telemetry(token, base_dir=self.base_dir) if status == "ok" else []
        return rows, status

    def alive(self):
        """Is the process there AND still answering? A wedged game keeps its PID, so a liveness
        check on the process table alone reports healthy while every scenario times out - which is
        how one run lost eight consecutive data points to a 'still alive' hang."""
        if self._process is not None:
            if self._process.poll() is not None:
                return False
            return True
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
