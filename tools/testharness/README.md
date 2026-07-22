# Test harness — autonomous scripted-input integration testing

Lets an external tool (no human, no keyboard/gamepad) drive the player's vehicle through a
scripted control timeline and capture its world trajectory, for comparing physics behavior
(ODE baseline today; ODE-vs-Jolt once the Jolt integration exists) and for auto-tuning.
Design background: `Kraken/docs/jolt-integration-techanalysis.md` §8.

## Enabling

`data/kraken.ini`:

```ini
[testharness]
enabled=1
```

Default is `0` (disabled) — the hook is not installed at all unless opted in.

## How it works

`kraken::fix::testharness` installs a `ChangeCall` on the direct call site that invokes
`ai::DynamicScene::CollideScene(float)` from `ai::CServer::Update` (VA `0x5F438D`). The wrapper
calls the real `CollideScene` first, then runs one harness tick with that frame's `elapsedTime`.
Every tick it polls `trigger.txt`; when its content changes it starts a new scenario. While a
scenario is running it overwrites the player vehicle's `m_throttle`/`m_steerRadians`/`m_brake`/
`m_bHandBrake` fields directly (same fields `fix::cardan` already reads/writes) and appends one
telemetry row.

This only works once a save is already loaded and the player vehicle exists in the world
(`CollideScene` doesn't run at the main menu). See "Bootstrap" below.

## File protocol

All files live under `<game>/data/kraken_testharness/`.

- `scenario.csv` — written by the driver **before** triggering a run:
  ```
  spawn,x,y,z,qx,qy,qz,qw      # optional; omit to keep the vehicle's current pose
  t,throttle,steer,brake,handbrake
  0.0,0.0,0.0,0.0,0
  0.1,1.0,0.0,0.0,0
  4.0,1.0,0.0,0.0,0
  4.1,0.0,0.0,1.0,0
  ```
  Rows are held constant between samples (last-sample-at-or-before-current-time), not
  interpolated. `throttle`/`steer`/`brake` are the same units as `Vehicle::m_throttle` /
  `m_steerRadians` / `m_brake` (steer is radians).
- `trigger.txt` — one line, any token. Changing it (while no scenario is running) starts a
  fresh run: samples reload from `scenario.csv`, vehicle resets to `spawn` (if given) with zero
  linear/angular velocity, and a new telemetry file starts.
- `output_<token>.csv` — one row per tick:
  `t,px,py,pz,qx,qy,qz,qw,comx,comy,comz,vx,vy,vz,avx,avy,avz,throttle,steer,brake,handbrake,`
  `gear,engineRpm,realThrottle,wheelsTouchingGround,numWheels,drivenWheels,drivenWheelsJointed`.
  `p*` is `Vehicle::GetPosition()`, `com*` is `Vehicle::GetMassCenterPosition()` (kept as a
  cross-check — see the world-vs-local COM bug already hit once in `wheelmodel-port`). The
  trailing group is drivetrain diagnostics added while chasing the bug below: `realThrottle`
  is what `_KeepGearBox` actually turns into wheel torque (should track `throttle` closely
  unless braking); `drivenWheels`/`drivenWheelsJointed` sanity-check that the vehicle's ODE
  wheel joints are actually attached.
- `output_<token>.done` — written when the scenario ends: `ok` or `no_vehicle` (player vehicle
  not found — e.g. no save loaded, or player got out of the car).

The vehicle is left in brake+handbrake-on state when a scenario finishes, so it doesn't roll
away unattended before the next trigger.

## Driving it — `harness.py`

Stdlib-only Python 3, no install needed.

```python
from harness import Scenario, Sample, run_scenario, metrics

scenario = Scenario(samples=[
    Sample(t=0.0, throttle=0.0),
    Sample(t=0.1, throttle=1.0),
    Sample(t=4.0, throttle=1.0),
    Sample(t=4.1, throttle=0.0, brake=1.0),
])
token, rows, status = run_scenario(scenario, timeout=30.0)
print(status, metrics(rows))
```

Or just `python harness.py smoke` for a canned straight-line accelerate/brake scenario.

`compare(rows_a, rows_b)` aligns two telemetry runs by nearest timestamp and reports position
RMSE + mean orientation error — the same comparison this will run between an ODE trace and a
Jolt trace once that side exists (nothing here is Jolt-specific yet).

## Bootstrap (automatic — `autoload_save=1`)

`CollideScene` — and therefore the whole harness — only runs once a save is loaded and a
vehicle exists. `[testharness] autoload_save=1` makes the game load one itself, with zero
human input, via `CMiracle3d::LoadSavedGame` called from a one-shot hook on
`m3d::Application::ProcessAllEvents`. It auto-picks the most-recently-written save under
`data/profiles/*/saves/*` (no profile name hardcoded, so it works regardless of the profile
folder's name/language) and loads its `maps\currentmap.xml`.

Getting this working end-to-end took three real reverse-engineering bugs, in case any of this
needs revisiting:

1. **Wrong `LoadSavedGame` address.** Was `0x006202C0`; the PDB gives RVA `0x0202c0`, i.e. VA
   `0x004202C0` (off by exactly `0x200000`). The wrong address landed mid-way through an
   unrelated function (`auxScriptErrorDesc::~auxScriptErrorDesc`), corrupting the stack.
2. **Wrong save-dir path.** `LoadSavedGame` checks `<saveDir>\currentmap.xml`, which on disk
   lives under a `maps` subfolder of the save (`saves\00000032\maps\currentmap.xml`), not the
   save root — `TryAutoLoadSave` appends `\maps` before calling.
3. **Wrong calling convention.** The PDB labels `LoadSavedGame` `__thiscall` (`this`@ecx), but
   the real prologue is `mov edi, eax` — it reads `this` from **EAX**, not ECX (MSVC's
   whole-program optimizer picked a custom register once it could see every call site; the PDB
   default is just wrong here). Calling it through a normal `__thiscall` function-pointer typedef
   silently fed it the function pointer's own address as `this` (MSVC materializes an indirect
   call target into EAX right before `call eax`, and that value happened to still be sitting in
   EAX when the callee's prologue read it) — which crashed several calls later, deep inside
   `LoadMap → CurGameMode::Set`, once it finally dereferenced that bogus `this` at a large
   member offset. Fixed with a hand-written inline-asm call (`CallLoadSavedGame` in
   `testharness.cpp`) that puts `self` in EAX explicitly.
4. **Fired one frame too early.** `ProcessAllEvents` runs once per `Application::run()` loop
   iteration, *before* that same iteration's `OneFrame()` — which is what actually loads the
   main menu level. Firing on the very first call meant no level had ever been loaded yet, so
   `LoadMap → CleanLevel → DeleteAllFilesInDirectory` hit `assert(strDir.length() > 0)` on
   "previous map temp dir" state only a real level load populates. Fixed by delaying the
   auto-load until `AUTOLOAD_DELAY_FRAMES` (300) `ProcessAllEvents` calls have passed, giving the
   normal boot flow time to reach the main menu on its own first.

With all four fixed, a cold launch with `autoload_save=1` loads the most recent save and starts
driving in ~10-15s with no human input, and repeated scenarios need no further manual step
afterward (the harness resets the vehicle's pose itself between runs instead of reloading the
level).

## Known gotcha — scripted throttle silently overridden (fixed)

After bootstrap worked, the vehicle still wouldn't move: `m_throttle`/`m_brake`/`m_bHandBrake`
read back correctly right after `Tick()` wrote them, but `m_realThrottle` (what
`_KeepGearBox` actually turns into wheel torque) sat frozen at a constant `10` regardless of
the script — i.e. the values `_KeepThrottle` itself consumed were *not* what `Tick()` had just
written.

Root cause: `fix::cardan` patches the exact same call site (`ai::Vehicle::Update`'s call to
`_KeepThrottle` at `0x5EC7AD`) with its own reimplementation, for an unrelated chassis-animation
fix. `routines::ChangeCall` doesn't chain — whichever module's `Apply()` runs last simply
owns that call site, and `testharness::Apply()` wasn't touching it at all, so cardan's version
ran unopposed on every tick. Cardan's version has a one-way "auto-handbrake when stationary
with ~zero throttle" latch (matches the vehicle's state at scenario start) with no release
logic; once latched, it force-zeros throttle and force-sets brake=1 every tick from then on,
regardless of what `Tick()` had written moments earlier — which is exactly the `realThrottle=10`
signature (`0 - sign(rpm)*1.0*10`). Confirmed empirically by temporarily logging cardan's
own inputs/outputs per call (see git history on this file/`cardan.cpp` around this fix) —
`m_throttle` measured `0.0` there throughout the scripted throttle=1 phase.

Fixed by having `testharness` also patch `0x5EC7AD` (installed after cardan's `Apply()`, so it
wins), forcing the current sample's throttle/brake/handbrake — and clearing `m_bAutoBrake` —
immediately before calling straight through to the *real*, native `_KeepThrottle` (`0x5DAAE0`),
bypassing cardan's reimplementation entirely while testharness is enabled. This is an
acceptable tradeoff since testharness is a dedicated opt-in automated-testing mode, not meant
to run during normal play — losing cardan's cosmetic animation fix while it's active doesn't
matter. With this in place, a straight-line smoke test moves ~160m in 8.5s with proper gear
shifts (RPM-thresholded 2→3) and a clean brake-to-stop, instead of ~0.8m of jitter.
