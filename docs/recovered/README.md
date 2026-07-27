# Recovered fragments — read this before trusting anything here

On 2026-07-27 the working copies at `F:\Kraken` and the game install at `F:\HTA_Kraken` were lost.
Neither was recoverable: `F:\$RECYCLE.BIN` is empty, and a sweep of C:/D:/E:/F: found no other copy of
the repository. The only surviving source of truth is this GitHub remote, whose `jolt` head is
`09af3ff` ("Plan the ODE→Jolt replacement, measure the real cost, and close Stage 1 preconditions").

**The remote is roughly a dozen sessions behind what was lost**, because the standing working rule was
"commit locally, never push". Concretely, the remote does NOT contain:

- `docs/jolt-integration-techanalysis.md` §55 through §96 — the file here stops at §54 (5324 lines
  against roughly 9130 lost).
- The whole of Stage 1 "wheel as a real body": wheel bodies, `JPH::SixDOFConstraint`, wheelmodel
  mode 4, the parallel harvest listener, sub-stepping, the §82 resync divergence instrument, §87 wheel
  re-seating, §88 gearbox gating, §90 per-frame extremes, §93 inertia lever, §96 wheel-mass fix.
  `source/fix/joltshadow.cpp` here is 4413 lines and defines **zero** `wm4_*` config keys.
- The test-harness toolkit: this branch has `harness.py` and a README; the lost tree had ~18 scripts
  plus versioned per-experiment data directories.

## What is in this folder

Salvaged from the session scratchpad, which survived. These are the ONLY artefacts of the final
session that exist.

| path | what it is | trust |
|---|---|---|
| `sec94-arcade-assists.md` | docs §94 — `_ApplyStabilizingForces`: the game applies a speed-proportional downforce and a steering-proportional yaw torque to every vehicle's ODE chassis, and the Jolt shadow reproduces neither | verbatim as written and committed |
| `sec94-6-why-missed.md` | docs §94.6 — why it was missed: the port is scoped to the physics step and hooks `StepScene`, while the assists live in `Vehicle::Update` | verbatim |
| `sec95-force-site-sweep.md` | docs §95 — the reverse pass: every site where the game touches a vehicle body, enumerated and closed, plus the mode-4 wheel-mass double count | verbatim |
| `sec96-wheel-mass-fix.md` | docs §96 — the wheel-mass fix, its exact binding check, and the null measurement | verbatim |
| `harness/*.py` | six of the ~18 harness scripts | **STALE, DO NOT TRUST** — these are scratchpad copies taken at unknown points, not the final versions. `band95.py`'s `BASE_INI` in particular pins every key it lists and its defaults are known to have drifted behind the code's (see the §87/§89 incident described in §95) |
| `mass-data-96/` | the six live runs behind §96's measurement, with the `docs §58` build lines that prove the mass binding | raw logs, trustworthy |

## What cannot be reconstructed from here

The mode-4 implementation itself. It was built across many sessions as hundreds of incremental edits
to `joltshadow.cpp`, `config.cpp/hpp` and the harness; only the last session's conversation survives,
and it contains edits rather than whole files. Reaching mode 4 again means re-implementing Stage 1
steps 1–7, not restoring them.

## Blockers on continuing at all

`F:\HTA_Kraken` is gone, including `hta.exe` and `game.pdb`. That removes **both** live testing and
disassembly. Every measurement in §94–§96 and every next step planned there depends on one or both.
`E:\HTA-Kraken` survives but holds only `data/kraken_testharness/{scenario.csv,trigger.txt}` — no
executable, no symbols.

## The rule that should change

The remote sat 40+ doc sections behind because nothing was ever pushed. Whatever is rebuilt should be
pushed as it lands.
