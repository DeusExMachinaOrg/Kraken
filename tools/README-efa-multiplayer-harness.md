# EFA multiplayer two-machine harness

`efa_multiplayer_harness.ps1` deploys the built Kraken DLL and the EFA
multiplayer overlay to a local and an SSH-connected Windows machine. It then
runs the real ENet peer executable across the LAN and collects the relevant
logs. It never modifies a target unless `-Action Deploy` or `-Action All` is
specified.

## Preconditions

- The same ComMod-managed EFA installation already exists at both targets.
  The harness verifies `data/scripts/efa.lua` and the EFA r1m1 map files before
  it writes anything.
- Local target: `E:\HTA_EFA` by default. This is the isolated EFA copy used
  for multiplayer work; the original `E:\HTA-Kraken` is left untouched.
- Remote SSH target: `etozh@192.168.2.80`, game copy:
  `E:\HTA_EFA` by default.
- OpenSSH client, `scp`, and Windows PowerShell 5+ are present on the local
  machine; the remote account must accept key-based SSH authentication.

ComMod is used for the initial Community Remaster/EFA installation because EFA
declares that prerequisite in its `manifest.yaml`. The harness does not try to
reimplement ComMod's dependency/patch handling. `-Action Package` creates an
updated ComMod-compatible EFA archive with `manifest.yaml` at its root.

`-Action ProvisionRemote` is intentionally a byte-copy of the already working
local EFA runtime, not a second implementation of ComMod. It excludes logs,
screenshots and backups, refuses to overwrite an existing remote target, and
removes its newly-created target if unpacking or validation fails. The overlay
recognises the flat `r1m1\triggers.xml` layout used by `HTA_EFA` and writes the
EFA Community Remaster source hooks to the files that this installed game loads.
It preserves each target's own `server.lua` (which can differ across EFA
releases) and appends the adapter include only when it is absent.
The harness preserves map XML too, except for one targeted CP1251-safe insertion
into EFA's existing `StartMatchmaking` trigger: it calls `EFA_MP.BeginRaid()`
without replacing the rest of the map or its seasonal logic.

## Commands

```powershell
# Creates the EFA archive; no game copy is changed.
.\tools\efa_multiplayer_harness.ps1 -Action Package

# One-time: creates E:\HTA_EFA on the remote PC from the local, already
# ComMod-installed EFA runtime. It refuses to overwrite an existing directory.
.\tools\efa_multiplayer_harness.ps1 -Action ProvisionRemote

# Verifies both game copies are already EFA installs; no write occurs.
.\tools\efa_multiplayer_harness.ps1 -Action Preflight

# Closes local and remote hta.exe, then copies the DLL and only the EFA files
# affected by multiplayer. Existing files are backed up under
# backups\efa-mp-harness\<timestamp> on each machine.
.\tools\efa_multiplayer_harness.ps1 -Action Deploy

# Runs host locally and client remotely over the actual LAN. It verifies
# EntityAssign, snapshots/input/weapon traffic, and EntityDespawn.
.\tools\efa_multiplayer_harness.ps1 -Action Smoke

# Deploy, run the smoke test, then collect the final 2,000 lines of both logs.
.\tools\efa_multiplayer_harness.ps1 -Action All
```

Artifacts and logs are saved in
`artifacts\efa-multiplayer-harness\<timestamp>`. The smoke test does not start
two graphical game instances; it exercises the same ENet protocol with
`kraken_net_peer_test.exe`, avoiding Ex Machina's known second-instance 3D
initialisation failure. It is a two-PC test: the host binds the LAN address
selected for the SSH peer and the remote executable connects to it, so it also
catches firewall, routing and protocol-compatibility regressions.
