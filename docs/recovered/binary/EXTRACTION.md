# What was taken out of the symbol-less Stage-1 DLL

`kraken-stage1-20260727-1522.dll` has **no PDB**: its debug directory carries only a VC_FEATURE entry,
there is no CodeView record, and no `kraken.pdb` exists anywhere on the machine. (`library.pdb` in the
game directory is unrelated — it contains none of `joltshadow`, `StepWheelModel`, `wm4_`, `SixDOF`.)

So there are no function names and no boundaries. Everything below was recovered from what a stripped
binary still carries: string literals at known addresses, and the code that references them.

## Artefacts

| file | what |
|---|---|
| `mined/strings.tsv` | 4509 strings with virtual address and section |
| `mined/formats.tsv` | 187 printf-style format strings — 56 of them tagged `docs §` |
| `mined/inikeys.txt` | 103 identifier-shaped strings (config key candidates) |
| `mined/config-members.txt` | 40 `ConfigValue` entries parsed out of `Config::Config()`, with member offsets |
| `kraken.ini` | the config as dumped by the game — **every key with its value** |
| `tools/dllmine.py` | string/format/key miner |
| `tools/dllx.py` | string-anchored cross-referencer and disassembler |
| `tools/dllcfg.py` | `Config::Config()` table parser |

## The config table is fully recovered — from two sources that check each other

`kraken.ini` gives every key and its value across all sections. `Config::Config()` gives the
declaration order (member offsets `+0x350` … `+0x6f8`), the section each key belongs to, and the
integer defaults and bounds. Together they reconstruct `config.cpp` and the `config.hpp` member list.

The parse was **calibrated against ground truth** rather than assumed: `wm4_chassis_mass_excl_wheels`
reads back as `{ "jolt_harness", "wm4_chassis_mass_excl_wheels", 1, true, 0, 1 }`, which is exactly the
line written into `config.cpp` on the last day before the loss.

The 13 `wm4_*` keys, in declaration order:
`wm4_hardcore` `wm4_spin` `wm4_steer` `wm4_joint_at_mount` `wm4_contact_torque_cap`
`wm4_compress_fraction` `wm4_resync_period` `wm4_resync_reseat` `wm4_resync_keep_gear`
`wm4_inertia_from_box` `wm4_chassis_mass_excl_wheels` `wm4_max_dt` `wm4_max_substeps`.

### What the table does NOT give

Float defaults and float min/max bounds. MSVC does not emit those as immediates: the value is loaded
into an xmm register from `.rdata` and the min/max pair is packed with `vunpcklps`, so recovering them
statically needs register dataflow across the whole constructor, not pattern matching. It was not
worth doing — `kraken.ini` already carries every float **value**, and only the guard-rail bounds are
missing, which are not behaviour.

`mined/config-members.txt` therefore prints integer readings that are correct for integer keys and
**stale carry-over for float keys** (five consecutive `wheelmodel` entries showing the same 4/1/16 is
the tell). Read it for section, key, member offset and dumpable flag; read `kraken.ini` for values.

## String-anchored function recovery — proven, and deliberately not pursued far

`tools/dllx.py xref <addr>` finds the code referencing a string. Demonstrated on the §58 build line
(`docs §58 (%s): mode 4 - built %zu wheel bodies + SixDOF constraints …`, address `0x101dc3e8`): one
reference, at `0x100347a5`, inside the function beginning near `0x10034708` — i.e. `BuildWheelBodies`.
The recovered string is the **final** version including the `| docs §95.4 wheelMass=… total=…` suffix
added hours before the loss, which confirms this DLL is the post-fix build.

Each of the 56 `docs §`-tagged format strings is an anchor of the same kind, so all 56 log sites can be
located and their functions read.

**That was not carried further, on purpose.** Reading optimised x86 back into C++ for ~56 functions
would cost far more than re-implementing them, and would produce worse code: the output would be a
transcription of what the compiler made of the lost source, not the source. The DLL's real value is as
a **specification and a constant table**, and both have now been extracted.

## What the format strings are worth

`mined/formats.tsv` is the closest thing to a specification of the lost implementation. Each
`docs §`-tagged line states what a feature measured and in what units — for §22 through §95.4,
covering every Stage 1 section whose source is gone. Re-implementation should start there and from
`docs/recovered/sec94..sec96`, not from this binary's instructions.
