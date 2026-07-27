# Recovered fragments — read this before trusting anything here

On 2026-07-27 the working copy at `F:\Kraken` was lost. It is not recoverable: `F:\$RECYCLE.BIN` is
empty, and a deep search of `C:\Users\etozh` for `joltshadow.cpp`, `jolt-integration-techanalysis.md`,
`wheelmodel_core.hpp` and `divergence_rate.py` returned nothing. The only surviving source of truth is
this GitHub remote, whose `jolt` head was `09af3ff` ("Plan the ODE→Jolt replacement, measure the real
cost, and close Stage 1 preconditions").

**The remote is roughly a dozen sessions behind the lost tree**, because the standing working rule was
"commit locally, never push". Concretely, this branch does NOT contain:

- `docs/jolt-integration-techanalysis.md` §55 through §96 — the file here stops at §54 (5324 lines
  against roughly 9130 lost).
- The whole of Stage 1 "wheel as a real body": wheel bodies, `JPH::SixDOFConstraint`, wheelmodel
  mode 4, the parallel harvest listener, sub-stepping, the §82 resync divergence instrument, §87 wheel
  re-seating, §88 gearbox gating, §90 per-frame extremes, §93 inertia lever, §96 wheel-mass fix.
  `source/fix/joltshadow.cpp` here is 4413 lines and defines **zero** `wm4_*` config keys.
- The test-harness toolkit: this branch has `harness.py` and a README; the lost tree had ~18 scripts
  plus versioned per-experiment data directories.

## What still WORKS — the first assessment was too gloomy

The game install was never on `F:`. It lives at `C:\Users\etozh\code\HTA_Kraken` and is intact:
`hta.exe`, `game.pdb` (40 MB), the full `data/` tree, and `data/kraken_testharness/` with the last
runs' outputs. `tools/lora`, the PDB+PE disassembly toolkit, **is** on this branch.

So both capabilities the loss appeared to remove are in fact available: **disassembly of the original
game** works right now, and **live measurement** works right now — see the binary below.

## `binary/` — the single most valuable artefact here

`binary/kraken-stage1-20260727-1522.dll` (md5 `a4a65cae9f8ae5373541f3d463c2c00b`) is the last build of
the lost tree. It contains the entire Stage 1 implementation in compiled form, including the §96
wheel-mass fix, and it is what the game directory was still running.

**Do not rebuild and deploy from this branch until everything wanted from that DLL has been taken out
of it.** A rebuild from the current source produces a binary with no mode 4 at all and overwrites the
only surviving copy of a dozen sessions of work. It has been copied here for exactly that reason.

Already extracted from it:

- `binary/strings.txt` — 6283 ASCII strings.
- `binary/docs_strings.txt` — the 58 `docs §`-tagged log format strings. These encode what each
  instrumented feature does and what it measures, for §22 … §95.4 — including every Stage 1 section
  whose source is gone (§58, §59, §60, §61, §66, §67, §68, §69, §70, §70.1, §71, §75, §78, §82, §87,
  §93, §95.4). This is the best available specification for re-implementation.
- The complete `wm4_*` config key list, recovered from the DLL:
  `wm4_chassis_mass_excl_wheels`, `wm4_compress_fraction`, `wm4_contact_torque_cap`, `wm4_hardcore`,
  `wm4_inertia_from_box`, `wm4_joint_at_mount`, `wm4_max_dt`, `wm4_max_substeps`,
  `wm4_resync_keep_gear`, `wm4_resync_period`, `wm4_resync_reseat`, `wm4_spin`, `wm4_steer`.
- `binary/kraken-20260727-1527.log` — the last session's live log.
- `binary/kraken.ini` — the config as restored to its safe state at the end of that session.

## The rest of this folder

Salvaged from the session scratchpad, which survived.

| path | what it is | trust |
|---|---|---|
| `sec94-arcade-assists.md` | docs §94 — `_ApplyStabilizingForces`: the game applies a speed-proportional downforce and a steering-proportional yaw torque to every vehicle's ODE chassis, and the Jolt shadow reproduces neither | verbatim as written and committed |
| `sec94-6-why-missed.md` | docs §94.6 — why it was missed: the port is scoped to the physics step and hooks `StepScene`, while the assists live in `Vehicle::Update` | verbatim |
| `sec95-force-site-sweep.md` | docs §95 — the reverse pass: every site where the game touches a vehicle body, enumerated and closed, plus the mode-4 wheel-mass double count | verbatim |
| `sec96-wheel-mass-fix.md` | docs §96 — the wheel-mass fix, its exact binding check, and the null measurement | verbatim |
| `harness/*.py` | six of the ~18 harness scripts | **STALE, DO NOT TRUST** — scratchpad copies of unknown vintage, not the final versions. `band95.py`'s `BASE_INI` pins every key it lists and its defaults are known to have drifted behind the code's (see the §87/§89 incident described in §95) |
| `mass-data-96/` | the six live runs behind §96's measurement, with the `docs §58` build lines that prove the mass binding | raw logs, trustworthy |

## What cannot be reconstructed

The mode-4 source itself. It was built across many sessions as hundreds of incremental edits; only the
last session's conversation survives, and it holds edits rather than whole files. Reaching mode 4 again
means re-implementing Stage 1 steps 1–7 — now with the DLL's log strings as a specification and
§94–§96 as findings that should shape the design from the start rather than be discovered again.

## The rule that should change

The remote sat 40+ doc sections behind because nothing was ever pushed. Whatever is rebuilt should be
pushed as it lands.
