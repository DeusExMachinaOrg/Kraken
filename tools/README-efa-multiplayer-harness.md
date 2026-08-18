# EFA multiplayer two-machine acceptance harness

`efa_multiplayer_harness.ps1` packages and deploys Kraken-owned binaries, runs
headless protocol checks, and orchestrates the real two-machine raid, JIP, and
combat acceptance matrix. It never overwrites EFA Lua/XML or any other mod
resource. Preflight hashes the installed EFA contract on both machines and
fails if the copies differ.

The graphical actions use no UI/input emulation. They do not synthesize input,
move either vehicle, inject an NPC, rewrite a map, or call an EFA-specific raid
function. The runtime loads the newest native saved game, opens the generic
Kraken session, and starts `StartMatchmaking` only on the elected host. Clients
discover the host through automatic LAN discovery and only follow host-authored
match/map state.

## Prerequisites

- Matching, working EFA installations at `E:\HTA_EFA` on both Windows PCs.
- Key-based OpenSSH access to the remote PC and an active interactive desktop
  session there. The harness uses a scheduled task only to enter that desktop
  session; it does not automate the game UI.
- A fresh unique Win32 Release build produced by
  `E:/KrakenWorkspace/scripts3/mcp_cmake.py`.
- Pass that exact artifact directory with `-KrakenBuildRoot`, or set
  `KRAKEN_MP_BUILD_ROOT`. Shared `build` and `build-ninja` directories are
  rejected.
- The same multiplayer configuration and EFA resources on both machines. JIP
  requires `joinPolicy=join_in_progress` and sufficient `maxPlayers`.

Deployment always closes both games first. It copies only `kraken.dll`,
`kraken_net_peer_test.exe`, and a Kraken manifest. The remote scheduled launcher
writes unique per-run stdout/stderr files. All collected evidence is placed in
`artifacts/efa-multiplayer-harness/<run-id>`.

Every standalone graphical action hashes the local and remote deployed
`kraken.dll` and requires both hashes to equal the exact
`-KrakenBuildRoot/kraken.dll`. It does not deploy automatically and fails with a
stale-deployment diagnostic; run `Deploy` explicitly. `All` deploys before its
graphical actions.

## Actions

```powershell
$build = 'E:\KrakenWorkspace\Kraken\build\mp_verify2_release'

# Parser, deterministic fatal-parser, split-line, and role-gate tests. No game.
.\tools\efa_multiplayer_harness.ps1 -Action SelfCheck

# Verify artifacts and matching EFA installations without writing either game.
.\tools\efa_multiplayer_harness.ps1 -Action Preflight -KrakenBuildRoot $build

# Build a Kraken-only archive, or close both games and deploy it.
.\tools\efa_multiplayer_harness.ps1 -Action Package -KrakenBuildRoot $build
.\tools\efa_multiplayer_harness.ps1 -Action Deploy -KrakenBuildRoot $build

# Run the bounded 64-participant executable or CTest from the supplied build.
.\tools\efa_multiplayer_harness.ps1 -Action Headless -KrakenBuildRoot $build
.\tools\efa_multiplayer_harness.ps1 -Action Unit -KrakenBuildRoot $build

# Real LAN protocol executable smoke.
.\tools\efa_multiplayer_harness.ps1 -Action Smoke -KrakenBuildRoot $build

# Real graphical forming, JIP, and death/exit scenarios.
.\tools\efa_multiplayer_harness.ps1 -Action RaidCrashSmoke -KrakenBuildRoot $build
.\tools\efa_multiplayer_harness.ps1 -Action JipSmoke -KrakenBuildRoot $build
.\tools\efa_multiplayer_harness.ps1 -Action CombatHostKillsClient -KrakenBuildRoot $build
.\tools\efa_multiplayer_harness.ps1 -Action CombatClientKillsHost -KrakenBuildRoot $build
```

Combat actions automatically inspect the exact newest native save selected by
the runtime bootstrap: `data/profiles/*/saves/*/maps/currentmap.xml`, ordered by
the save directory's last-write time. The harness chooses the first non-empty
`WeaponGroupManager/CurrentWeaponGroups/WeaponGroup@weaponParts` token in group
order. Host shooting uses the local save; client shooting performs the same
bounded selection over SSH on the remote installation. It fails before launch
if the selected save has no equipped weapon. An explicit override may still be
supplied with `-HostCombatWeaponPart` / `-ClientCombatWeaponPart` or
`KRAKEN_EFA_HOST_COMBAT_WEAPON_PART` or
`KRAKEN_EFA_CLIENT_COMBAT_WEAPON_PART`.

Both peers must materialize the selected attached `Gun`, the real shooter and
target, their entity generations, and a living target before arming. Native
`CanFire` readiness is required on the role that will actually fire; the
non-shooter's presentation-only replica is not required to report `CanFire`.

`All` is intentionally comprehensive: SelfCheck, deployment, headless stress,
full unit CTest, LAN peer smoke, forming raid, JIP, both combat directions, and
log collection. It is not a short alias.

## Acceptance gates

The client gate is strictly ordered:

`native_saved_game -> snapshot_committed -> quest_committed -> world_ready sent -> gameplay_open -> first_input`

The host gate is:

`native_saved_game -> peer world_ready ACK -> gameplay_open -> host_control_ready`

`host_control_ready` proves an actual peer input packet was accepted; the host
does not pretend to send a first-input marker. Combat cannot execute until its
local production gate emits `KRAKEN_MP_ACCEPT combat_armed`. The harness waits
for both roles to arm. The deadline is exactly 30 seconds and starts when the harness
observes the second role's marker. Death before that poll, in that same poll, or
after the deadline fails. The authoritative death record must also name the
requested scenario and exact shooter/target direction.

The exit matrix requires `KRAKEN_MP_ACCEPT session_exit` only after a later
frame confirms the requested native route. Main-menu confirmation uses the
engine's `GS_MAINMENU` state; map confirmation requires both `GS_GAME` and the
requested level identity. For `host-kills-client`, only the client may exit and
its reason must be `death`; the host must stay alive and Playing and emit fresh
post-death `combat_host_surviving` heartbeats throughout the stability window.
For `client-kills-host`, both peers must exit with reason `death`;
`host_terminated` is not accepted.

## Continuous supervision and JIP evidence

One supervisor remains active from process start through the stability window
at a 100 ms cadence. It checks both `kraken.log` streams, both unique stderr
streams, process state, and exception inventories. A persistent remote SSH
watchdog avoids reconnecting on every poll. Fatal parsing carries incomplete
line tails between chunks and recognizes PANIC, assertion failures, fatal and
unhandled exceptions, access violations, stack overflow, and pure-virtual
failures. A fatal aborts immediately even if `hta.exe` remains alive.

For JIP, the host must first reach Playing/gameplay-open and then naturally
register a new NPC on the typed `EntitySpawn`/`VehicleDescriptor` stream. Only a
successful registry bind, loadout capture, and typed host-entity insertion emits
`native_entity_registered`, carrying the exact entity identity and generation.
The marker's `barrierRevision` is explicitly the independent generic-world
journal revision sampled at that moment; it is not an NPC `ObjectCreated`
revision. Bound player/NPC vehicle trees remain excluded from the generic world
journal so the generic applier cannot create duplicate vehicles.

The harness launches the client only after that post-Playing marker. The client
must materialize the exact typed NPC identity/generation/kind and commit a
generic-world snapshot revision at least as new as `barrierRevision` before
completing the quest/world/input barrier. If no natural typed registration
appears within the bounded timeout, the scenario fails with evidence; nothing
is injected to make it pass.

The same exact `EntityKind::NpcVehicle` identity/generation match is required in
forming and JIP scenarios. The supervisor remains active for the configurable
stability window after synchronization or death.
