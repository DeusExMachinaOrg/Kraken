# Steps 2 and 3 verified live — and several values match the lost build exactly

Run on 2026-07-28 against the restored build, `wheelmodel=4`, `apply=0` (observation only, so
the shadow cannot write back into ODE), autoload save, ~55 s.

## Step 2 — wheel bodies and the §96 mass fix

```
docs §58 (player): mode 4 - built 4 wheel bodies + SixDOF constraints (chassisMass=100.0, massFloor=2.50) | docs §95.4 wheelMass=10.0 chassisMassFinal=90.0 total=100.0
docs §58 (player): mode 4 - built 4 wheel bodies + SixDOF constraints (chassisMass=167.0, massFloor=4.18) | docs §95.4 wheelMass=44.0 chassisMassFinal=123.0 total=167.0
```

The engagement check built into the line holds on both: `chassisMassFinal + wheelMass == chassisMass`
(90 + 10 = 100, 123 + 44 = 167). The mass fix is doing what §96 measured it doing, from the first
build rather than after months of shipping the double count.

**These are not merely plausible numbers — they match the lost build's own output.** The recovered
spec quotes live `massFloor` values of 2.50 at chassisMass 100.0 and 4.18 at 167.0; both reproduce
exactly. So does the 100 kg vehicle's whole trailing group: the verification pass that examined
`mass-data-96` quoted `wheelMass=10.0 chassisMassFinal=90.0 total=100.0` from the lost logs, and
that is character-for-character what the restored code now prints.

## Step 2 — constraint frame

```
docs §75: constraint frame (player) w=2 axle.fwd=+0.0000 (0.00 deg off perpendicular) | anchorOffsetFromWheelCentre=0.0500 m (restLen)
```

`0.00 deg off perpendicular` on every wheel — the Gram-Schmidt orthonormalisation is doing its job,
and it matches the recovered spec's note that the live log read 0.00 deg on every vehicle.
`anchorOffsetFromWheelCentre=0.0500` equals `minLen`, which is also what the spec records for this
vehicle.

The §60 static-sag guard stayed silent: no wheel is parked on its bump stop.

## Step 3 — harvest

```
docs §59/63: harvest (player) wheelsWithContact=4 contactPoints=8 overflow=0 (of 4) | survived=4 maxNormalF=0N bound(k_t*tau)=0N ratio=0.00
```

Exactly the expected shape, and each field is a separate assertion:

- `wheelsWithContact=4 (of 4)` — every wheel is finding ground.
- `contactPoints=8` — two per wheel, which is the TYRE and RIM sub-shapes both reporting. If the
  per-pair sub-shape decode were broken this would be 4, or 8 with `survived=8`.
- `survived=4` — one per wheel. Only TYRE records feed the band; the RIM records are harvested and
  counted but correctly excluded, which is the thing that would otherwise apply the normal load
  twice.
- `overflow=0` — the 16-record buffer is not close to full.
- `maxNormalF=0N bound=0N` — **by design at step 3**, which applies no force. A zero here is the
  pass condition, not a missing feature.

## Shadow behaviour

```
Shadow divergence (player): pos=1.613m vel=0.012m/s angle=0.2deg
```

Stable across the whole run: 0.2° of attitude error and a positional offset that does not grow.
The vehicle did **not** sink when the wheel bodies appeared, which is the outcome the step-2 code
comment predicts and which one draft of the spec got backwards - the old `StepWheelModel` path is
still carrying the chassis, exactly as plan §7 step 2 requires.

The only warnings in the run are `Jolt: static box shape creation failed: Invalid half extent` from
the static-obstacle exporter, which is pre-existing and unrelated to Stage 1.

## What this does not yet show

No force has been applied through the new path, so nothing here says the tyre model is right - only
that the plumbing carries real contacts to the place step 4 will consume them. Step 4 is where
`maxNormalF` and the `k_t*tau` bound become meaningful, and where the load-carrying acceptance check
(`carried` ≈ 1.0 against vehicle weight) first has anything to check.
