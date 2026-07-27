# Environment after the 2026-07-27 loss — what moved and what had to be reinstalled

The `F:` drive is gone. Everything that lived on it had to be relocated or reinstalled. This file
records the working state so it is not rediscovered one failure at a time.

## Paths that changed

| what | was | now |
|---|---|---|
| repository | `F:\Kraken` | `F:\Kraken` (fresh clone of this remote — the drive exists again, but empty of history) |
| game install | `F:\HTA_Kraken` | **`C:\Users\etozh\code\HTA_Kraken`** — intact: `hta.exe`, `game.pdb` (40 MB), full `data/`, `data/kraken_testharness/` |
| Python | `F:\python310` (gone, with every installed package) | `C:\Users\etozh\AppData\Local\Programs\Python\Python312\python.exe` (3.12.10) |

Every harness script and helper in the lost tree hard-coded `F:\HTA_Kraken` and `F:\Kraken`. Anything
recovered or rewritten has to be repointed at the new game path.

## Dependencies that had to be reinstalled

`lora` imports `pefile`, `capstone`, `pydia2`, `mcp` (plus stdlib). Reinstall with:

```bash
python -m pip install pefile capstone pydia2 mcp
```

Verified working versions: pefile 2024.8.26, capstone 5.0.7, pydia2 0.2.1, mcp (current).

## `lora` did NOT need restoring

`lora_v3.zip` was fetched and compared file by file against `tools/lora` on this branch: **all 43
source files are byte-identical once CRLF/LF is normalised**. The branch already carries v3. What was
actually missing was only the Python environment above.

Smoke test that proves the whole chain (PE + PDB + DIA):

```bash
python tools/lora_query.py '[{"tool":"load","args":{"exe_path":"C:/Users/etozh/code/HTA_Kraken/hta.exe","pdb_path":"C:/Users/etozh/code/HTA_Kraken/game.pdb"}},{"tool":"pdb_function_detail","args":{"func_name":"ai::Vehicle::_ApplyStabilizingForces"}}]'
```

## Two tools that were available all along and were not used

Recorded because §94 and §95 were done the hard way, and the next pass should not repeat that.

- **`xrefs_to`** — cross-references to an address. §95 hand-rolled a byte-level `E8`/`E9` rel32 scanner
  and a second `call [reg+disp32]` scanner to close the virtual-dispatch hole. Check what `xrefs_to`
  already covers before writing either again.
- **`disasm_detailed`** — returns far more than instructions: the original **source file and line
  range**, **local variable names and types** from the PDB, and the disassembly **grouped by source
  line**. On `ai::Vehicle::_ApplyStabilizingForces` it gives `vehicle.cpp:6717-6769` with locals
  `vel: CVector`, `horizVel: float`, `curAngle: float`.

  That last point independently corroborates the §94 decode from the other direction: the downforce is
  computed from a *horizontal* velocity and the yaw torque from a wheel's *current steering angle*,
  which is exactly what the instruction-level reading concluded before these names were available.

The full tool list is 60 entries; `python tools/lora_query.py` with a `load` step and any tool name
works, and the registry can be listed by importing the `tools_*` modules with a stub registrar.

## Still true

`docs/recovered/binary/kraken-stage1-20260727-1522.dll` is the only surviving artefact of the Stage 1
implementation. Do not rebuild and deploy from this branch until everything wanted has been taken out
of it — see `README.md`.
