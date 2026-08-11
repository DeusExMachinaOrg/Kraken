#define LOGGER "multiplayer"

#include "net/runtime.hpp"

#include "config.hpp"
#include "ext/runtime.hpp"
#include "ext/logger.hpp"
#include "net/entity_protocol.hpp"
#include "net/entity_registry.hpp"
#include "net/input_command.hpp"
#include "net/impact_damage.hpp"
#include "net/lan_discovery.hpp"
#include "net/loadout_protocol.hpp"
#include "net/loot_transaction.hpp"
#include "net/pause_policy.hpp"
#include "net/player_slots.hpp"
#include "net/session.hpp"
#include "net/spawn_attempt.hpp"
#include "net/snapshot_interpolation.hpp"
#include "net/transport.hpp"
#include "net/vehicle_snapshot.hpp"
#include "net/weapon_command.hpp"
#include "net/world_loot.hpp"
#include "routines.hpp"

#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/CMiracle3d.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/Player.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/Chest.hpp"
#include "hta/ai/DamageInfo.hpp"
#include "hta/ai/GeomRepository.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Location.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/VehiclePart.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/m3d/ScriptServer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>

namespace hta::ai {
// DamageInfo is declared by HTA but its small ABI-helper constructor and
// destructor are not part of the linked HTA library.  Keep those helpers in
// the runtime module so confirmed client-side presentation can use the
// engine's existing InflictDamage ABI without adding another external symbol.
DamageInfo::DamageInfo()
    : attackerId(0),
      attackingAgentId(0),
      bDamageFriends(false),
      gunPrototypeId(0),
      damage(0.0f),
      damageType(DAMAGE_PIERCING),
      damagedPartName(),
      hitPos(),
      hitDir(),
      normal(),
      decalId(-1)
{
}

DamageInfo::~DamageInfo() = default;
} // namespace hta::ai

namespace kraken::net::runtime {
bool RequestLocalLoot(LootId loot_id, LootTransactionId transaction_id,
                      std::uint32_t amount);
LootId SpawnHostLoot(std::int32_t chest_prototype_id, std::int32_t resource_id,
                     std::uint32_t amount);
bool BeginSession();
bool EndSession();
bool IsSessionActive();
bool submit_native_weapon_intent(::hta::ai::Vehicle* vehicle, bool trigger_held,
                                 ObjId target_obj_id, std::int32_t gun_id = 0,
                                 const VehicleVector3* aim_override = nullptr,
                                 float aim_speed = 1.0f);
namespace {

constexpr uintptr_t kServerUpdateCallSite = 0x005C809D;
constexpr uintptr_t kServerUpdateAddress = 0x005F4090;
// FrameMove calls Controls after CServer::Update.  The combat harness must
// apply its held-fire intent after real controls have processed this frame.
constexpr uintptr_t kControlsCallSite = 0x0041593B;
constexpr uintptr_t kControlsAddress = 0x004016B0;
// Bootstrap hook used exclusively by KRAKEN_EFA_RAID_AUTOTEST.  It runs after
// the stock main-menu boot and calls the engine's own save loader; no UI input
// is synthesized and no map transition is reimplemented in Kraken.
constexpr uintptr_t kProcessAllEventsCallSite = 0x005A7FFF;
constexpr uintptr_t kProcessAllEventsAddress = 0x005A7BC0;
constexpr uintptr_t kLoadSavedGameAddress = 0x004202C0;
// ai::InfoCone::GetInfoObjId assumes that SceneGraph never returns a null
// collision item.  An asynchronously queued ObjContainer removal can violate
// that assumption for one frame; guard the exact virtual call in the game.
constexpr uintptr_t kInfoConeIsKindOfCallSite = 0x007EC55C;
// WeaponGroup::KeepFire invokes this site once for every selected weapon on
// every rendered frame.  Its bool is the already-resolved game impulse
// state (pressed/released); it is therefore the input boundary for LMB, not
// an observation of a Gun's internal firing state.
constexpr uintptr_t kWeaponGroupFireByPartNameCallSite = 0x00585A89;
// ai::Gun::Fire's sole call to _DoFire.  Unlike the Lua-only Custom2 bridge,
// every player weapon reaches this call after the engine has accepted a shot.
constexpr uintptr_t kGunDoFireCallSite = 0x006E0C96;
// WeaponFirer::WeaponLookAtPoint is the actual EFA weapon-aim routine.  The
// Lua Custom2 call is not on the player mouse-aim path in this mod.
constexpr uintptr_t kVehicleWeaponLookAtPointCallSite = 0x005CBF09;
constexpr uintptr_t kCustomWeaponLookAtPointCallSite = 0x005DE009;
constexpr uintptr_t kPlayerWeaponLookAtPointCallSite = 0x005EC302;
// WeaponFirer::AimAndFireFromWeapons is exposed by the game with a nonstandard
// x86 ABI; use the explicit bridge below instead of the generated declaration.
constexpr uintptr_t kWeaponAimAndFireFromWeaponsAddress = 0x007C3370;
// The sole direct call to WanderersManager::_SpawnWanderer in
// WanderersManager::Update.  This is intentionally narrower than a global
// ObjContainer creation hook: client map loading and mod-owned objects remain
// untouched.
constexpr uintptr_t kWandererSpawnCallSite = 0x00870158;
// Vehicle::InflictDamage is the generic vehicle vtable entry at byte offset
// 0x8c.  Patch only a verified Vehicle vtable entry; do not redirect the
// function body, which would remove the original call target.
constexpr std::size_t kVehicleInflictDamageVtableOffset = 0x8c;
constexpr uintptr_t kVehicleInflictDamageAddress = 0x005E68A0;
// The executable body of Vehicle::GetSeenObjId is `mov eax,[eax+0x268]; ret`.
// Its PDB calling metadata says ECX, but invoking it as an ordinary C++ member
// is unsafe: this build's code consumes the instance from EAX.  Calls therefore
// go through the explicit x86 bridge below.
constexpr uintptr_t kVehicleGetSeenObjIdAddress = 0x00550A50;
constexpr std::size_t kVehicleSeenObjIdOffset = 0x268;
// CMiracle3d keeps the third-person camera's two wrapped Euler angles directly
// after m_gameCameraRho.  These offsets are verified against game.pdb and
// CMiracle3d::ValidateCameraAngles.  n_SetCameraAngle (the stock Lua bridge)
// writes the yaw field at 0x8b278 after converting degrees to radians.  The
// `rotYPR` call sites pass these as (yaw, pitch, roll), respectively.
constexpr std::size_t kCameraYawOffset = 0x8b278;
constexpr std::size_t kCameraPitchOffset = 0x8b27c;
// `n_GetCameraPos` reads these three floats as the current rendered camera
// origin.  They are distinct from the tracked vehicle position and therefore
// must be used for crosshair-ray calculations.
constexpr std::size_t kCameraWorldOriginOffset = 0x8b26c;
// EFA's relationship data reserves the 1000..1200 range for vehicle factions
// and starts the single-player vehicle at 1100.  A network-controlled remote
// vehicle must not inherit that faction: the engine correctly treats equal
// factions as allies and rejects PvP targeting.  The offset produces a stable
// per-slot free-for-all faction without touching health or projectile rules.
constexpr std::int32_t kMultiplayerBelongMin = 1000;
constexpr std::int32_t kMultiplayerBelongMax = 1200;

using Clock = std::chrono::steady_clock;
using ServerUpdateFn = void(__fastcall*)(void*, void*, float);
using ControlsFn = int(__thiscall*)(hta::CMiracle3d*, float, float);
using ProcessAllEventsFn = void(__thiscall*)(void*);
using FireFromWeaponByPartNameFn = bool(__thiscall*)(hta::ai::Vehicle*,
                                                      const hta::CStr*, bool);
using GunDoFireFn = bool(__thiscall*)(hta::ai::Gun*);
using WeaponLookAtPointFn = void(__fastcall*)(hta::ai::ComplexPhysicObj*,
                                               const hta::CVector&, float);
using WandererSpawnFn = void(__thiscall*)(void*);
using VehicleInflictDamageFn = void(__thiscall*)(hta::ai::Vehicle*,
                                                  const hta::ai::DamageInfo&);

constexpr std::uint64_t kInterpolationDelayMs = 100;
constexpr float kWeaponAimDistance = 1'000.0f;
constexpr std::uint16_t kLanDiscoveryPort = 27016;
constexpr auto kLanDiscoveryTimeout = std::chrono::milliseconds(1500);
constexpr std::size_t kMaxRemoteEntities = 1024;
constexpr std::size_t kMaxPendingImpactFx = 256;
constexpr std::array<const char*, kPlayerSlotCount> kPlayerSpawnNames{
    "MP_SPAWN_1", "MP_SPAWN_2", "MP_SPAWN_3", "MP_SPAWN_4"};
constexpr std::array<const char*, kPlayerSlotCount> kPlayerProxyNames{
    "MP_PROXY_1", "MP_PROXY_2", "MP_PROXY_3", "MP_PROXY_4"};

struct RemoteEntity {
    NetId entity_id = kInvalidNetId;
    std::uint16_t generation = 0;
    EntityKind kind = EntityKind::PlayerVehicle;
    std::int32_t prototype_id = -1;
    std::int32_t belong = 0;
    bool has_spawn = false;
    VehicleSnapshot spawn_snapshot{};
    bool has_spawn_snapshot = false;
    SnapshotInterpolationBuffer snapshots;
    std::uint32_t last_sequence = 0;
    bool has_sequence = false;
    WeaponCommand weapon{};
    bool has_weapon = false;
    std::uint32_t presented_shot_id = 0;
    LoadoutProfile loadout{};
    bool has_loadout = false;
    std::uint32_t applied_loadout_revision = 0;
    bool retired = false;
    EntityGeneration retired_generation = kInvalidEntityGeneration;
    SpawnAttemptState spawn_attempt;
};

struct HostEntity {
    NetId entity_id = kInvalidNetId;
    EntityGeneration generation = kInitialEntityGeneration;
    EntityKind kind = EntityKind::NpcVehicle;
    ObjId object_id = kInvalidObjId;
    std::int32_t prototype_id = -1;
    bool active = true;
    LoadoutProfile loadout{};
    std::uint32_t published_loadout_revision = 0;
};

struct PeerController {
    PeerId peer = kInvalidPeer;
    NetId entity_id = kInvalidNetId;
    InputCommand input{};
    bool has_input = false;
    std::uint32_t last_sequence = 0;
    EntityGeneration generation = kInitialEntityGeneration;
    ObjId vehicle_obj_id = kInvalidObjId;
    WeaponCommand weapon{};
    bool has_weapon = false;
    std::uint32_t last_weapon_sequence = 0;
    WeaponCommand presentation_weapon{};
    bool has_presentation_weapon = false;
    Clock::time_point next_weapon_presentation{};
    bool unstuck_was_requested = false;
    LoadoutProfile loadout{};
    bool has_loadout = false;
    std::uint32_t applied_loadout_revision = 0;
    std::uint32_t deferred_loadout_revision = 0;
    SpawnAttemptState spawn_attempt;
    bool host_vehicle_active = false;
    bool shared_spawn_applied = false;
};

struct LootRecord {
    WorldLootRecord world{};
    ObjId chest_obj_id = kInvalidObjId;
    WorldLootGeneration source_generation = kInvalidEntityGeneration;
    std::uint32_t publication_amount = 0;
    bool object_backed = false;
};

struct LootSourceBinding {
    ObjId object_id = kInvalidObjId;
    hta::ai::Obj* object = nullptr;
    WorldLootGeneration generation = kInvalidEntityGeneration;
    WorldLootId container_id = 0;
    std::int32_t prototype_id = -1;
    bool lifecycle_tombstoned = false;
};

struct WorldLootReceipt {
    PeerId peer = kInvalidPeer;
    WorldLootPickupRequest request{};
    WorldLootPickupResult result{};
};

// EntitySpawn is reliable but Session events can be consumed after an
// unreliable snapshot.  Keep a per-peer publication record so every object
// receives its creation metadata before (or alongside) its first useful
// snapshot, without re-sending it at 20 Hz.
struct SpawnPublication {
    PeerId peer = kInvalidPeer;
    NetId entity_id = kInvalidNetId;
    std::uint16_t generation = 0;
};

struct LoadoutPublication {
    PeerId peer = kInvalidPeer;
    NetId entity_id = kInvalidNetId;
    EntityGeneration generation = kInvalidEntityGeneration;
    std::uint32_t revision = 0;
};

struct RuntimeState {
    EnetTransport transport;
    LanDiscovery lan_discovery;
    std::unique_ptr<Session> session;
    std::vector<PeerId> peers;
    Clock::time_point next_ping{};
    Clock::time_point next_reconnect{};
    std::chrono::seconds reconnect_backoff{1};
    float snapshot_interest_radius = 500.0f;
    Clock::time_point next_snapshot{};
    std::uint32_t next_snapshot_sequence = 1;
    NetId next_dynamic_entity_id = 1000;
    std::uint32_t server_tick = 0;
    EntityRegistry entities;
    std::vector<HostEntity> host_entities;
    std::vector<RemoteEntity> remote_entities;
    std::vector<SpawnPublication> spawn_publications;
    std::vector<LoadoutPublication> loadout_publications;
    std::vector<PeerController> controllers;
    SnapshotInterpolationBuffer local_correction;
    NetId local_entity_id = kInvalidNetId;
    Clock::time_point next_input{};
    Clock::time_point next_loadout{};
    std::uint32_t next_input_sequence = 1;
    std::uint32_t next_weapon_sequence = 1;
    std::uint32_t presented_local_shot_id = 0;
    Clock::time_point next_local_weapon_aim{};
    Clock::time_point next_local_weapon_fire{};
    VehicleVector3 local_weapon_aim{};
    float local_weapon_aim_speed = 1.0f;
    bool has_local_weapon_aim = false;
    WeaponCommand local_weapon_state{};
    bool has_local_weapon_state = false;
    bool local_weapon_trigger_held = false;
    std::int32_t local_weapon_gun_id = 0;
    ObjId local_weapon_target_obj_id = kInvalidObjId;
    // The engine's custom target field is process-local. Retain only the
    // NetId confirmed by the current native input; release/free-aim input
    // clears this field through resolve_local_native_weapon_target().
    NetId local_weapon_target_entity_id = kInvalidNetId;
    std::uint32_t last_local_loadout_revision = 0;
    LootId next_loot_id = 1;
    WorldLootId next_loot_container_id = 1;
    WorldLootGeneration next_loot_source_generation = 1;
    WorldLootSessionEpoch session_epoch = 0;
    WorldLootRevision world_loot_revision = 1;
    std::vector<LootRecord> loot_records;
    std::vector<LootSourceBinding> loot_sources;
    std::vector<WorldLootReceipt> loot_receipts;
    WorldLootReplica world_loot;
    bool invalid_lua_world_loot_object_args_logged = false;
    PlayerSlotAllocator player_slots;
    std::array<ObjId, kPlayerSlotCount> player_slot_marker_ids{};
    std::array<ObjId, kPlayerSlotCount> player_slot_proxy_ids{};
    std::array<bool, kPlayerSlotCount> player_slot_failure_logged{};
    std::array<bool, kPlayerSlotCount> player_slot_ready_logged{};
    bool player_slots_ready = false;
    Clock::time_point next_player_slot_retry{};
    Clock::time_point player_slot_deadline{};
    ObjId previous_local_vehicle_obj_id = kInvalidObjId;
    // Both host and client retain the engine-owned Player vehicle.  Map
    // proxies are presentation-only objects for *remote* players; assigning
    // Player to one with ChangeVehicleByExisting causes this EFA build to
    // delete that proxy during its next ownership-graph update.
    ObjId local_player_vehicle_obj_id = kInvalidObjId;
    ObjId host_vehicle_obj_id = kInvalidObjId;
    bool is_host = false;
    bool hook_installed = false;
    bool engine_safety_hooks_installed = false;
    bool impact_damage_hook_installed = false;
    bool impact_damage_hook_error_logged = false;
    bool combat_unavailable_logged = false;
    VehicleInflictDamageFn vehicle_inflict_damage_original = nullptr;
    std::uint32_t next_impact_event_id = 1;
    std::uint32_t impact_health_mismatch_diagnostics = 0;
    ImpactDamageDeduplicator impact_damage_deduplicator;
    std::vector<ImpactDamage> pending_impact_damage;
    std::vector<ImpactDamage> pending_impact_fx;
    bool network_pause_was_cleared = false;
    bool spawn_together = true;
    bool local_shared_spawn_applied = false;
    // Opt-in integration smoke only. The production game never activates a
    // raid or creates test NPCs unless this environment-controlled mode is on.
    bool raid_autotest_enabled = false;
    bool raid_autotest_bootstrap_installed = false;
    bool raid_autotest_bootstrap_attempted = false;
    std::uint32_t raid_autotest_bootstrap_frames = 0;
    Clock::time_point next_raid_autotest{};
    bool raid_autotest_error_logged = false;
    // Test-only, set by KRAKEN_EFA_COMBAT_AUTOTEST.  It drives the existing
    // weapon-command/host-fire pipeline after both player entities exist;
    // no UI input is synthesized.
    std::string combat_autotest_scenario;
    // The harness obtains this from the active saved WeaponGroup on each
    // machine.  It is deliberately not a prototype or vehicle-specific
    // constant: the test invokes the selected, normally equipped weapon.
    std::string combat_autotest_weapon_part;
    Clock::time_point next_combat_autotest{};
    std::uint32_t combat_autotest_sequence = 1;
    bool combat_autotest_started = false;
    bool combat_autotest_death_logged = false;
    // Set at an engine-safe boundary after the authoritative host vehicle has
    // reached zero health.  Ending the transport from inside InflictDamage
    // would mutate registries while the engine is still walking damage state.
    bool host_defeat_session_end_pending = false;
    // The client can learn that the host ended a raid while EFA is already
    // unloading its current map.  In that path Kraken must only stop network
    // ownership; touching Player slots or scene ghosts races the map unload.
    bool session_end_preserve_client_scene = false;
};

RuntimeState g_state;
std::uint32_t g_next_session_epoch = 1;

std::uint32_t allocate_session_epoch()
{
    const std::uint32_t epoch = g_next_session_epoch++;
    if (g_next_session_epoch == 0)
        g_next_session_epoch = 1;
    return epoch == 0 ? allocate_session_epoch() : epoch;
}
ServerUpdateFn g_server_update =
    reinterpret_cast<ServerUpdateFn>(kServerUpdateAddress);
ControlsFn g_controls = reinterpret_cast<ControlsFn>(kControlsAddress);
FireFromWeaponByPartNameFn g_fire_from_weapon_by_part_name =
    reinterpret_cast<FireFromWeaponByPartNameFn>(0x005CC030);
GunDoFireFn g_gun_do_fire = reinterpret_cast<GunDoFireFn>(0x006DF650);
WeaponLookAtPointFn g_weapon_look_at_point =
    reinterpret_cast<WeaponLookAtPointFn>(0x007C3210);
WandererSpawnFn g_wanderer_spawn =
    reinterpret_cast<WandererSpawnFn>(0x0086FD70);

bool relay_weapon_command(const WeaponCommand& command);
void relay_host_weapon_presentation(hta::ai::Vehicle* vehicle,
                                    bool trigger_held, ObjId target_obj_id,
                                    std::int32_t gun_id,
                                    const VehicleVector3* aim_override = nullptr,
                                    float aim_speed = 1.0f);
bool bind_local_player_vehicle();
bool bind_host_player_vehicle();
bool initialize_player_slots();
bool is_player_controlled_vehicle(const hta::ai::Vehicle& vehicle);
void run_combat_autotest_tick(float elapsed_time);
int __fastcall controls_hook(hta::CMiracle3d*, void*, float, float);
bool release_player_slot_entity(NetId entity_id,
                                EntityGeneration expected_generation,
                                const char* reason);
bool relay_impact_damage(const ImpactDamage& event);
void receive_impact_damage(const SessionEvent& event);
void apply_pending_impact_damage();
void reconcile_host_entities();

void reset_impact_damage_state()
{
    g_state.next_impact_event_id = 1;
    g_state.impact_health_mismatch_diagnostics = 0;
    g_state.impact_damage_deduplicator.clear();
    g_state.pending_impact_damage.clear();
    g_state.pending_impact_fx.clear();
}

void adopt_client_session_epoch(const WorldLootSessionEpoch epoch)
{
    if (g_state.is_host || epoch == 0)
        return;
    if (g_state.session_epoch != 0 && g_state.session_epoch != epoch) {
        const WorldLootSessionEpoch previous = g_state.session_epoch;
        reset_impact_damage_state();
        LOG_INFO("impact replay state reset for session epoch transition old=%u new=%u",
                 previous, epoch);
    }
    g_state.session_epoch = epoch;
}

hta::ai::Vehicle* vehicle_from_object(hta::ai::Obj* object)
{
    if (object == nullptr)
        return nullptr;
    if (!object->IsKindOf(hta::ai::Vehicle::p_classObject))
        return nullptr;
    // The class relationship above is the checked downcast boundary.  ObjId
    // registry entries may refer to any engine Obj and must not be cast first.
    return static_cast<hta::ai::Vehicle*>(object);
}

std::int32_t multiplayer_remote_belong(const std::int32_t local_belong,
                                       const NetId remote_entity_id)
{
    // Keep the local player's faction unchanged.  Only its replicas/hosted
    // remote entities move into a distinct EFA relationship slot.
    const std::int64_t preferred = static_cast<std::int64_t>(local_belong) +
                                   static_cast<std::int64_t>(remote_entity_id);
    if (preferred >= kMultiplayerBelongMin &&
        preferred <= kMultiplayerBelongMax)
        return static_cast<std::int32_t>(preferred);
    const std::int64_t fallback = static_cast<std::int64_t>(local_belong) -
                                  static_cast<std::int64_t>(remote_entity_id);
    if (fallback >= kMultiplayerBelongMin && fallback <= kMultiplayerBelongMax)
        return static_cast<std::int32_t>(fallback);
    // EFA's player relationship domain is fixed.  This fallback is safe for
    // the two-player MVP and keeps an invalid engine belong out of the world.
    return kMultiplayerBelongMin + static_cast<std::int32_t>(remote_entity_id);
}

void configure_free_for_all_relationship(hta::ai::ObjContainer& objects,
                                         const std::int32_t local_belong,
                                         const std::int32_t remote_belong)
{
    if (local_belong == remote_belong)
        return;
    constexpr float kEnemyTolerance = 1.0f; // ai::RS_ENEMY
    objects.SetTolerance(local_belong, remote_belong, kEnemyTolerance);
    objects.SetTolerance(remote_belong, local_belong, kEnemyTolerance);
}

hta::ai::Location* location_from_object(hta::ai::Obj* object)
{
    if (object == nullptr)
        return nullptr;
    if (!object->IsKindOf(hta::ai::Location::p_classObject))
        return nullptr;
    return static_cast<hta::ai::Location*>(object);
}

struct ResolvedPlayerSlot {
    PlayerSlotIndex index = kInvalidPlayerSlot;
    ObjId marker_obj_id = kInvalidObjId;
    ObjId proxy_obj_id = kInvalidObjId;
    hta::CVector position{};
    hta::Quaternion rotation{};
    hta::ai::Vehicle* proxy = nullptr;
};

void log_player_slot_failure(const PlayerSlotIndex index, const char* reason)
{
    if (index >= kPlayerSlotCount || g_state.player_slot_failure_logged[index])
        return;
    g_state.player_slot_failure_logged[index] = true;
    LOG_ERROR("MP player slot %u unavailable: %s", static_cast<unsigned>(index + 1),
              reason);
}

const char* object_class_name(const hta::ai::Obj* object)
{
    return object != nullptr ? object->GetClassNameA() : "<missing>";
}

bool resolve_player_slot(const PlayerSlotIndex index, ResolvedPlayerSlot& result)
{
    if (index >= kPlayerSlotCount) {
        LOG_ERROR("MP player slot index %u is invalid",
                  static_cast<unsigned>(index));
        return false;
    }
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr) {
        log_player_slot_failure(index, "native object container is unavailable");
        return false;
    }
    hta::ai::ObjContainer& objects = *server->m_pObjects;
    const ObjId marker_id = objects.GetObjIdByObjName(
        hta::CStr(kPlayerSpawnNames[index]));
    const ObjId proxy_id = objects.GetObjIdByObjName(
        hta::CStr(kPlayerProxyNames[index]));
    hta::ai::Obj* const marker_object = objects.GetEntityByObjName(
        hta::CStr(kPlayerSpawnNames[index]));
    hta::ai::Obj* const proxy_object = objects.GetEntityByObjName(
        hta::CStr(kPlayerProxyNames[index]));
    hta::ai::Location* const marker = location_from_object(marker_object);
    hta::ai::Vehicle* const proxy = vehicle_from_object(proxy_object);
    if (marker_object == nullptr || proxy_object == nullptr || marker == nullptr ||
        proxy == nullptr || marker_object->GetDeletedStatus() ||
        proxy_object->GetDeletedStatus()) {
        if (!g_state.player_slot_failure_logged[index]) {
            LOG_ERROR("MP player slot %u unresolved marker=%s id=%d class=%s proxy=%s id=%d class=%s",
                      static_cast<unsigned>(index + 1), kPlayerSpawnNames[index],
                      marker_id, object_class_name(marker_object),
                      kPlayerProxyNames[index], proxy_id,
                      object_class_name(proxy_object));
        }
        log_player_slot_failure(index, "named marker/proxy is missing or has the wrong type");
        return false;
    }
    if (!g_state.player_slot_ready_logged[index]) {
        hta::ai::VehiclePart* const visual_gun = proxy->GetPartByName(
            hta::CStr("CABIN_SMALL_GUN"));
        LOG_INFO("MP player slot %u proxy weapon graph objId=%d cabinSmallGun=%s prototype=%d",
                 static_cast<unsigned>(index + 1), proxy->GetId(),
                 visual_gun != nullptr ? "present" : "missing",
                 visual_gun != nullptr ? visual_gun->GetPrototypeId() : -1);
    }
    if (marker_object->GetId() == proxy_object->GetId()) {
        log_player_slot_failure(index, "marker and proxy resolve to the same object");
        return false;
    }
    const hta::CVector position = marker->GetPosition();
    const hta::Quaternion rotation = marker->GetRotation();
    const float rotation_norm = rotation.x * rotation.x + rotation.y * rotation.y +
        rotation.z * rotation.z + rotation.w * rotation.w;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
        !std::isfinite(rotation.w) || !std::isfinite(rotation_norm) ||
        rotation_norm <= 0.0001f) {
        log_player_slot_failure(index, "marker transform is not finite");
        return false;
    }

    result.index = index;
    result.marker_obj_id = marker_object->GetId();
    result.proxy_obj_id = proxy_object->GetId();
    result.position = position;
    result.rotation = rotation;
    result.proxy = proxy;
    g_state.player_slot_marker_ids[index] = result.marker_obj_id;
    g_state.player_slot_proxy_ids[index] = result.proxy_obj_id;
    if (!g_state.player_slot_ready_logged[index]) {
        g_state.player_slot_ready_logged[index] = true;
        g_state.player_slot_failure_logged[index] = false;
        LOG_INFO("MP player slot %u ready marker=%s markerObjId=%d proxy=%s proxyObjId=%d prototype=%d",
                 static_cast<unsigned>(index + 1), kPlayerSpawnNames[index],
                 result.marker_obj_id, kPlayerProxyNames[index], result.proxy_obj_id,
                 proxy->GetPrototypeId());
    }
    return true;
}

bool resolve_player_spawn_transform(const PlayerSlotIndex index,
                                    hta::CVector& position,
                                    hta::Quaternion& rotation)
{
    if (index >= kPlayerSlotCount)
        return false;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return false;
    hta::ai::Obj* const marker_object = server->m_pObjects->GetEntityByObjName(
        hta::CStr(kPlayerSpawnNames[index]));
    hta::ai::Location* const marker = location_from_object(marker_object);
    if (marker == nullptr || marker_object->GetDeletedStatus())
        return false;
    position = marker->GetPosition();
    rotation = marker->GetRotation();
    const float rotation_norm = rotation.x * rotation.x +
        rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;
    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) && std::isfinite(rotation.x) &&
        std::isfinite(rotation.y) && std::isfinite(rotation.z) &&
        std::isfinite(rotation.w) && std::isfinite(rotation_norm) &&
        rotation_norm > 0.0001f;
}

bool apply_loadout_to_inactive_vehicle(hta::ai::Vehicle& vehicle,
                                       const LoadoutProfile& loadout,
                                       const char* const context)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr ||
        vehicle.bIsUpdatingByODE())
        return false;
    const ObjId object_id = vehicle.GetId();
    for (const LoadoutPart& requested : loadout.parts) {
        hta::ai::Obj* const object =
            server->m_pObjects->GetEntityByObjId(object_id);
        hta::ai::Vehicle* const current = vehicle_from_object(object);
        if (current != &vehicle || current->bIsUpdatingByODE()) {
            LOG_ERROR("%s loadout ownership/ODE check failed objId=%d entity=%u",
                      context, object_id, loadout.entity_id);
            return false;
        }
        const hta::CStr part_name(requested.slot.c_str());
        const hta::CStr prototype_name(requested.prototype.c_str());
        const hta::ai::VehiclePart* const existing =
            current->GetPartByName(part_name);
        const hta::ai::VehiclePartPrototypeInfo* const existing_prototype =
            existing != nullptr ? existing->GetPrototypeInfo() : nullptr;
        // A freshly-created vehicle of the advertised base prototype already
        // owns its stock parts. Replacing an identical live part is not a
        // no-op in this engine: it tears down native part state and can leave
        // dangling pointers. Only mutate an actual mismatch.
        if (existing_prototype != nullptr &&
            existing_prototype->m_prototypeName.c_str() != nullptr &&
            existing_prototype->m_prototypeName == requested.prototype.c_str())
            continue;
        if (!current->SetNewPart(part_name, prototype_name)) {
            LOG_ERROR("%s native SetNewPart failed objId=%d entity=%u slot=%s prototype=%s",
                      context, object_id, loadout.entity_id,
                      requested.slot.c_str(), requested.prototype.c_str());
            return false;
        }
        // SetNewPart creates and attaches native objects. Re-resolve the
        // parent before every subsequent dereference so ownership changes
        // cannot turn the next loadout operation into a stale-pointer call.
        hta::ai::Obj* const rebound_object =
            server->m_pObjects->GetEntityByObjId(object_id);
        hta::ai::Vehicle* const rebound = vehicle_from_object(rebound_object);
        if (rebound != &vehicle || rebound->bIsUpdatingByODE()) {
            LOG_ERROR("%s loadout identity/ODE changed after SetNewPart objId=%d entity=%u",
                      context, object_id, loadout.entity_id);
            return false;
        }
    }

    hta::ai::Obj* const object =
        server->m_pObjects->GetEntityByObjId(object_id);
    hta::ai::Vehicle* const rebound = vehicle_from_object(object);
    if (rebound != &vehicle || rebound->bIsUpdatingByODE())
        return false;
    for (const LoadoutPart& requested : loadout.parts) {
        const hta::CStr part_name(requested.slot.c_str());
        const hta::ai::VehiclePart* const part =
            rebound->GetPartByName(part_name);
        const hta::ai::VehiclePartPrototypeInfo* const prototype =
            part != nullptr ? part->GetPrototypeInfo() : nullptr;
        if (prototype == nullptr || prototype->m_prototypeName.c_str() == nullptr ||
            !(prototype->m_prototypeName == requested.prototype.c_str())) {
            LOG_ERROR("%s loadout verification failed objId=%d entity=%u slot=%s expected=%s",
                      context, object_id, loadout.entity_id,
                      requested.slot.c_str(), requested.prototype.c_str());
            return false;
        }
    }
    return true;
}

bool vehicle_matches_loadout(const hta::ai::Vehicle& vehicle,
                            const LoadoutProfile& loadout)
{
    for (const LoadoutPart& requested : loadout.parts) {
        const hta::ai::VehiclePart* const existing =
            vehicle.GetPartByName(hta::CStr(requested.slot.c_str()));
        const hta::ai::VehiclePartPrototypeInfo* const prototype =
            existing != nullptr ? existing->GetPrototypeInfo() : nullptr;
        if (prototype == nullptr || prototype->m_prototypeName.c_str() == nullptr ||
            !(prototype->m_prototypeName == requested.prototype.c_str()))
            return false;
    }
    return true;
}

bool ensure_vehicle_suspension_nodes(hta::ai::Vehicle& vehicle,
                                     const char* const context)
{
    std::uint32_t created = 0;
    const std::uint32_t wheel_count = vehicle.GetNumWheels();
    if (wheel_count == 0) {
        LOG_ERROR("%s has no wheels objId=%d", context, vehicle.GetId());
        return false;
    }
    for (std::uint32_t index = 0; index != wheel_count; ++index) {
        hta::ai::Wheel* const wheel = vehicle.GetWheel(index);
        if (wheel == nullptr)
            continue;
        if (wheel->m_suspensionNode == nullptr) {
            // Native Wheel API: it creates the model node only when the wheel
            // is correctly attached to a Vehicle body.
            wheel->CreateSuspensionNode();
            ++created;
        }
        if (wheel->m_suspensionNode == nullptr) {
            LOG_ERROR("%s could not create a wheel suspension node objId=%d wheel=%u",
                      context, vehicle.GetId(), index);
            return false;
        }
    }
    if (created != 0) {
        LOG_INFO("%s materialised %u missing wheel suspension nodes objId=%d",
                 context, created, vehicle.GetId());
    }
    return true;
}

bool complete_suspended_vehicle_loadout(hta::ai::ObjContainer& objects,
                                        const ObjId object_id,
                                        hta::ai::Vehicle*& vehicle,
                                        const char* const context)
{
    hta::ai::Obj* const object = objects.GetEntityByObjId(object_id);
    hta::ai::Vehicle* const current = vehicle_from_object(object);
    if (current != vehicle || current == nullptr) {
        LOG_ERROR("%s post-load identity check failed objId=%d",
                  context, object_id);
        return false;
    }

    // CreateNewObjectWithSuspendedPostLoad leaves the native object graph
    // open. SetNewPart must finish before Obj::PostLoad seals the Vehicle,
    // otherwise the first native Vehicle::Update can follow stale part/physics
    // state even though the parent Vehicle pointer still resolves.
    object->PostLoad();
    hta::ai::Obj* const rebound_object = objects.GetEntityByObjId(object_id);
    hta::ai::Vehicle* const rebound = vehicle_from_object(rebound_object);
    // Vehicle::_InternalPostLoad legitimately creates and enables its ODE
    // body.  The caller immediately retires that body for a network ghost;
    // treating the transient enabled state as failure schedules a half-linked
    // object for removal and corrupts DynamicScene's collision-cell list.
    if (rebound != vehicle) {
        LOG_ERROR("%s post-load identity check failed objId=%d",
                  context, object_id);
        return false;
    }
    vehicle = rebound;
    return true;
}

void deactivate_player_slot_vehicle(const ResolvedPlayerSlot& slot);

bool activate_player_slot_vehicle(const ResolvedPlayerSlot& slot,
                                  const bool reset_transform = true)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr || slot.proxy == nullptr)
        return false;
    hta::ai::Vehicle& vehicle = *slot.proxy;
    vehicle.m_AI.m_pDM = nullptr;
    vehicle.SetNpcMotionControllerId(-1);
    if (reset_transform) {
        vehicle.SetPositionSelf(slot.position);
        vehicle.SetRotationSelf(slot.rotation);
        vehicle.SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
        vehicle.SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    }
    vehicle.SetThrottle(0.0f, false);
    vehicle.SetBrake(1.0f);
    vehicle.m_bHandBrake = true;
    if (vehicle.GetNumWheels() == 0) {
        // EnablePhysics dereferences the first wheel/collision geometry in
        // this engine build.  A map proxy with an incomplete XML Parts block
        // therefore crashes at address 0 instead of returning an error.
        LOG_ERROR("MP player slot %u proxyObjId=%d has no wheels; physics activation rejected",
                  static_cast<unsigned>(slot.index + 1), slot.proxy_obj_id);
        deactivate_player_slot_vehicle(slot);
        return false;
    }
    vehicle.SetVisible();
    vehicle.EnableSounds(true);
    vehicle.EnablePhysics();
    vehicle.SetUpdatingByODE(true);
    server->m_pObjects->AddObjToUpdate(slot.proxy);
    return true;
}

void deactivate_player_slot_vehicle(const ResolvedPlayerSlot& slot)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr || slot.proxy == nullptr)
        return;
    hta::ai::Vehicle& vehicle = *slot.proxy;
    vehicle.SetThrottle(0.0f, false);
    vehicle.m_steerRadians = 0.0f;
    vehicle.SetBrake(1.0f);
    vehicle.m_bHandBrake = true;
    vehicle.SetNpcMotionControllerId(-1);
    vehicle.m_AI.m_pDM = nullptr;
    vehicle.SetUpdatingByODE(false);
    vehicle.DisablePhysics();
    vehicle.EnableSounds(false);
    server->m_pObjects->AddObjToNotUpdate(slot.proxy);
    vehicle.SetInvisible();
}

void restore_previous_local_vehicle()
{
    if (g_state.previous_local_vehicle_obj_id == kInvalidObjId)
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (player == nullptr || server == nullptr || server->m_pObjects == nullptr)
        return;
    hta::ai::Vehicle* const current = player->GetVehicle();
    if (current != nullptr && current->GetId() == g_state.previous_local_vehicle_obj_id)
        return;
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(
        g_state.previous_local_vehicle_obj_id);
    if (vehicle_from_object(object) == nullptr) {
        LOG_ERROR("cannot restore pre-session player vehicle objId=%d",
                  g_state.previous_local_vehicle_obj_id);
        return;
    }
    player->ChangeVehicleByExisting(g_state.previous_local_vehicle_obj_id, true);
    if (player->GetVehicle() == nullptr ||
        player->GetVehicle()->GetId() != g_state.previous_local_vehicle_obj_id)
        LOG_ERROR("native player vehicle restore did not bind objId=%d",
                  g_state.previous_local_vehicle_obj_id);
    else
        g_state.previous_local_vehicle_obj_id = kInvalidObjId;
}

bool bind_player_slot_entity(const PlayerSlotIndex index, const NetId owner_entity_id,
                             const EntityGeneration expected_generation,
                             const bool local_control, const bool activate_vehicle,
                             PlayerSlotLease& lease_out,
                             hta::ai::Vehicle*& vehicle_out)
{
    vehicle_out = nullptr;
    ResolvedPlayerSlot slot{};
    if (!resolve_player_slot(index, slot))
        return false;
    if (owner_entity_id == kInvalidNetId)
        return false;

    const auto current = g_state.player_slots.current(index);
    if (current && current->owner_entity_id != owner_entity_id) {
        log_player_slot_failure(index, "slot is owned by another entity");
        return false;
    }
    if (current && expected_generation != kInvalidEntityGeneration &&
        current->generation != expected_generation) {
        log_player_slot_failure(index, "entity generation does not match the slot generation");
        return false;
    }
    if (current && g_state.player_slots.is_bound(index)) {
        ObjId object_id = kInvalidObjId;
        EntityGeneration generation = kInvalidEntityGeneration;
        if (g_state.entities.lookup_obj_id(owner_entity_id, object_id, generation) &&
            object_id == slot.proxy_obj_id && generation == current->generation) {
            lease_out = *current;
            vehicle_out = slot.proxy;
            return true;
        }
        log_player_slot_failure(index, "bound slot has no matching entity registry entry");
        return false;
    }

    const auto lease = g_state.player_slots.reserve(index, owner_entity_id);
    if (!lease) {
        log_player_slot_failure(index, "reservation was rejected");
        return false;
    }
    if (expected_generation != kInvalidEntityGeneration &&
        lease->generation != expected_generation) {
        (void)g_state.player_slots.cancel(*lease, owner_entity_id);
        log_player_slot_failure(index, "incoming entity generation is stale");
        return false;
    }
    NetId mapped_entity = kInvalidNetId;
    EntityGeneration mapped_generation = kInvalidEntityGeneration;
    if (g_state.entities.lookup_net_id(slot.proxy_obj_id, mapped_entity,
                                       mapped_generation) &&
        (mapped_entity != owner_entity_id || mapped_generation != lease->generation)) {
        (void)g_state.player_slots.cancel(*lease, owner_entity_id);
        log_player_slot_failure(index, "proxy is already bound to another network entity");
        return false;
    }

    if (activate_vehicle && !activate_player_slot_vehicle(slot)) {
        (void)g_state.player_slots.cancel(*lease, owner_entity_id);
        log_player_slot_failure(index, "proxy has no valid ODE vehicle assembly");
        return false;
    }
    if (local_control) {
        hta::ai::Player* const player = hta::ai::Player::Instance();
        if (player == nullptr) {
            (void)g_state.player_slots.cancel(*lease, owner_entity_id);
            deactivate_player_slot_vehicle(slot);
            log_player_slot_failure(index, "native player object is unavailable");
            return false;
        }
        // Keep the map's original player car so ending a session restores the
        // single-player ownership graph instead of leaving Player on a proxy.
        if (g_state.previous_local_vehicle_obj_id == kInvalidObjId &&
            player->GetVehicle() != nullptr &&
            player->GetVehicle()->GetId() != slot.proxy_obj_id)
            g_state.previous_local_vehicle_obj_id = player->GetVehicle()->GetId();
        player->ChangeVehicleByExisting(slot.proxy_obj_id, true);
        if (player->GetVehicle() == nullptr ||
            player->GetVehicle()->GetId() != slot.proxy_obj_id) {
            (void)g_state.player_slots.cancel(*lease, owner_entity_id);
            deactivate_player_slot_vehicle(slot);
            LOG_ERROR("MP player slot %u native player bind failed proxyObjId=%d",
                      static_cast<unsigned>(index + 1), slot.proxy_obj_id);
            return false;
        }
    }

    const EntityRegistryBindResult bound = g_state.entities.bind(
        owner_entity_id, slot.proxy_obj_id, lease->generation);
    if (bound != EntityRegistryBindResult::Inserted &&
        bound != EntityRegistryBindResult::AlreadyBound) {
        (void)g_state.player_slots.cancel(*lease, owner_entity_id);
        deactivate_player_slot_vehicle(slot);
        LOG_ERROR("MP player slot %u entity bind failed entity=%u proxyObjId=%d generation=%u code=%u",
                  static_cast<unsigned>(index + 1), owner_entity_id,
                  slot.proxy_obj_id, static_cast<unsigned>(lease->generation),
                  static_cast<unsigned>(bound));
        return false;
    }
    if (!g_state.player_slots.bind(*lease, owner_entity_id)) {
        LOG_ERROR("MP player slot %u state bind failed entity=%u generation=%u",
                  static_cast<unsigned>(index + 1), owner_entity_id,
                  static_cast<unsigned>(lease->generation));
        return false;
    }
    lease_out = *lease;
    vehicle_out = slot.proxy;
    LOG_INFO("MP player slot %u bound entity=%u generation=%u markerObjId=%d proxyObjId=%d local=%u",
             static_cast<unsigned>(index + 1), owner_entity_id,
             static_cast<unsigned>(lease->generation), slot.marker_obj_id,
             slot.proxy_obj_id, local_control ? 1u : 0u);
    return true;
}

bool initialize_player_slots()
{
    g_state.player_slots.clear();
    g_state.player_slot_marker_ids.fill(kInvalidObjId);
    g_state.player_slot_proxy_ids.fill(kInvalidObjId);
    std::size_t ready = 0;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    const ObjId current_player_obj_id = player != nullptr && player->GetVehicle() != nullptr
        ? player->GetVehicle()->GetId() : kInvalidObjId;
    for (PlayerSlotIndex index = 0; index < kPlayerSlotCount; ++index) {
        ResolvedPlayerSlot slot{};
        if (!resolve_player_slot(index, slot))
            continue;
        ++ready;
        if (slot.proxy_obj_id != current_player_obj_id)
            deactivate_player_slot_vehicle(slot);
    }
    LOG_INFO("MP player-slot readiness ready=%u/%u", static_cast<unsigned>(ready),
             static_cast<unsigned>(kPlayerSlotCount));
    if (ready != kPlayerSlotCount)
        LOG_ERROR("MP player-slot readiness failed; named map markers/proxies are required");
    return ready == kPlayerSlotCount;
}

bool release_player_slot_entity(const NetId entity_id,
                                const EntityGeneration expected_generation,
                                const char* reason)
{
    const PlayerSlotIndex index = player_slot_for_entity(entity_id);
    if (index == kInvalidPlayerSlot) {
        LOG_ERROR("MP player slot release rejected entity=%u: no deterministic slot",
                  entity_id);
        return false;
    }
    const auto lease = g_state.player_slots.current(index);
    if (!lease || lease->owner_entity_id != entity_id ||
        (expected_generation != kInvalidEntityGeneration &&
         lease->generation != expected_generation)) {
        LOG_DEBUG("MP player slot release ignored stale entity=%u generation=%u reason=%s",
                  entity_id, static_cast<unsigned>(expected_generation), reason);
        return false;
    }
    ResolvedPlayerSlot slot{};
    const bool resolved = resolve_player_slot(index, slot);
    if (g_state.previous_local_vehicle_obj_id != kInvalidObjId &&
        (entity_id == g_state.local_entity_id || entity_id == 1))
        restore_previous_local_vehicle();

    ObjId mapped_object = kInvalidObjId;
    EntityGeneration mapped_generation = kInvalidEntityGeneration;
    const bool mapped = g_state.entities.lookup_obj_id(entity_id, mapped_object,
                                                        mapped_generation);
    const bool is_host_native_vehicle =
        g_state.is_host && entity_id == 1 &&
        mapped_object == g_state.host_vehicle_obj_id;
    const bool is_client_native_vehicle =
        !g_state.is_host && entity_id == g_state.local_entity_id &&
        mapped_object == g_state.local_player_vehicle_obj_id;
    if (resolved && !is_host_native_vehicle && !is_client_native_vehicle)
        deactivate_player_slot_vehicle(slot);
    if (mapped) {
        // The host retains the map's native Player vehicle.  The same
        // deterministic slot is still leased for entity 1, but its registry
        // binding deliberately points at that native vehicle rather than the
        // dormant MP_PROXY_1 (which otherwise overlaps it on spawn).
        if ((mapped_object == g_state.player_slot_proxy_ids[index] ||
             is_host_native_vehicle || is_client_native_vehicle) &&
            mapped_generation == lease->generation)
            (void)g_state.entities.unbind_net_id(entity_id);
        else
            LOG_ERROR("MP player slot release found mismatched registry entity=%u objId=%d generation=%u",
                      entity_id, mapped_object, static_cast<unsigned>(mapped_generation));
    }
    const bool released = g_state.player_slots.release(*lease, entity_id);
    if (!released)
        return false;
    if (entity_id == 1 && g_state.is_host)
        g_state.host_vehicle_obj_id = kInvalidObjId;
    if (is_client_native_vehicle)
        g_state.local_player_vehicle_obj_id = kInvalidObjId;
    LOG_INFO("MP player slot %u released entity=%u generation=%u reason=%s",
             static_cast<unsigned>(index + 1), entity_id,
             static_cast<unsigned>(lease->generation), reason);
    return true;
}

void release_all_player_slots(const char* reason)
{
    for (PlayerSlotIndex index = 0; index < kPlayerSlotCount; ++index) {
        const auto lease = g_state.player_slots.current(index);
        if (lease)
            (void)release_player_slot_entity(lease->owner_entity_id,
                                              lease->generation, reason);
    }
    restore_previous_local_vehicle();
}

bool try_activate_player_slots()
{
    if (g_state.player_slots_ready)
        return true;
    if (!initialize_player_slots())
        return false;
    if (g_state.is_host && !bind_host_player_vehicle()) {
        LOG_ERROR("MP player-slot readiness found map objects but host bind failed");
        release_all_player_slots("host deferred bind failed");
        return false;
    }
    g_state.player_slots_ready = true;
    LOG_INFO("MP player-slot activation complete role=%s",
             g_state.is_host ? "host" : "client");
    return true;
}

hta::ai::Chest* chest_from_object(hta::ai::Obj* object)
{
    if (object == nullptr)
        return nullptr;
    hta::m3d::Class* const runtime_class = object->GetRtClass();
    if (runtime_class == nullptr ||
        !runtime_class->IsKindOf(hta::ai::Chest::p_classObject))
        return nullptr;
    return static_cast<hta::ai::Chest*>(object);
}

hta::ai::Vehicle* vehicle_from_gun_owner(hta::ai::Gun* gun)
{
    if (gun == nullptr)
        return nullptr;
    const auto contains_gun = [gun](hta::ai::Vehicle& vehicle) {
        const auto names = vehicle.GetAttachedPartNames();
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (vehicle.GetPartByName(names[index]) == gun)
                return true;
        }
        return false;
    };
    // Gun is a VehiclePart.  Its PhysicBody::GetOwner() returns the physical
    // aggregate that owns the part, not necessarily the Vehicle object
    // itself.  In EFA that aggregate is commonly a nested compound part, so
    // a checked cast of GetOwner() to Vehicle drops every player shot.  Match
    // the actual part graph first, then use the physical owner as a fallback
    // for incomplete graphs.
    if (hta::ai::Player* const player = hta::ai::Player::Instance()) {
        hta::ai::Vehicle* const local = player->GetVehicle();
        if (local != nullptr && contains_gun(*local))
            return local;
    }

    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return nullptr;
    hta::ai::PhysicObj* const physical_owner = gun->GetOwner();
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
        if (vehicle != nullptr && contains_gun(*vehicle))
            return vehicle;
        if (vehicle != nullptr && physical_owner != nullptr &&
            static_cast<hta::ai::PhysicObj*>(vehicle) == physical_owner)
            return vehicle;
    }
    return nullptr;
}

void __fastcall vehicle_inflict_damage_hook(
    hta::ai::Vehicle* vehicle, void*, const hta::ai::DamageInfo& info);

bool valid_weapon_aim_point(const VehicleVector3& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z) &&
           std::abs(point.x) <= kMaxNetworkAimPointComponent &&
           std::abs(point.y) <= kMaxNetworkAimPointComponent &&
           std::abs(point.z) <= kMaxNetworkAimPointComponent;
}

bool capture_weapon_aim_point(const hta::ai::Vehicle& vehicle,
                              VehicleVector3& output)
{
    const hta::CVector position = vehicle.GetPosition();
    hta::CVector point = vehicle.GetCustomControlWeaponsTarget();
    const auto finite = [](const hta::CVector& value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    const auto usable = [&position, &finite](const hta::CVector& value) noexcept {
        const float dx = value.x - position.x;
        const float dy = value.y - position.y;
        const float dz = value.z - position.z;
        return finite(value) && std::isfinite(dx) && std::isfinite(dy) &&
               std::isfinite(dz) && (dx * dx + dy * dy + dz * dz) > 0.01f;
    };

    if (!usable(point)) {
        const hta::CVector direction = vehicle.GetDirection();
        const float length_squared = direction.x * direction.x +
            direction.y * direction.y + direction.z * direction.z;
        if (!finite(position) || !finite(direction) ||
            !std::isfinite(length_squared) || length_squared <= 0.0001f)
            return false;
        const float scale = kWeaponAimDistance / std::sqrt(length_squared);
        point = position + direction * scale;
    }
    const VehicleVector3 captured{point.x, point.y, point.z};
    if (!valid_weapon_aim_point(captured))
        return false;
    output = captured;
    return true;
}

bool g_suppress_client_weapon_damage = false;
bool g_presenting_confirmed_network_fire = false;
bool g_host_network_shot_fired = false;
// A host-produced ImpactDamage is replayed through the engine only to restore
// its hit/death presentation on a client.  The hook bypass is scoped to that
// replay; ordinary client collision damage remains suppressed.
bool g_presenting_authoritative_impact = false;
// Vehicle::_EvaluateToDead deliberately fires attached weapons while it tears
// down the vehicle.  That is native death presentation, not player input and
// must never pass through the multiplayer weapon-capture/relay path.
bool g_presenting_authoritative_death = false;
// Set only around the host's replay of a client shot.  Gun::_DoFire still
// performs the original projectile work, but its hook must not emit a second
// presentation built from a process-local fallback target.
bool g_replaying_network_fire = false;
WeaponCommand* g_active_host_weapon_command = nullptr;
bool should_relay_local_weapon_aim() noexcept
{
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_local_weapon_aim)
        return false;
    // 20 Hz is enough for smooth turret movement while avoiding a reliable
    // packet per rendered frame.
    g_state.next_local_weapon_aim = now + std::chrono::milliseconds(50);
    return true;
}

bool should_submit_local_weapon_state(const bool trigger_held) noexcept
{
    // Release must never wait behind the 20 Hz sender: a delayed release is
    // indistinguishable from a stuck automatic weapon on the host.
    if (!trigger_held)
        return true;
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_local_weapon_fire)
        return false;
    g_state.next_local_weapon_fire = now + std::chrono::milliseconds(50);
    return true;
}

void clear_local_native_weapon_target()
{
    g_state.local_weapon_target_obj_id = kInvalidObjId;
    g_state.local_weapon_target_entity_id = kInvalidNetId;
}

hta::ai::Vehicle* current_local_weapon_vehicle(
    hta::ai::Vehicle* const candidate, const char* const caller)
{
    const auto reject = [&](const char* const reason) -> hta::ai::Vehicle* {
        clear_local_native_weapon_target();
        LOG_DEBUG("local weapon vehicle rejected caller=%s reason=%s candidate=%p",
                  caller, reason, static_cast<void*>(candidate));
        return nullptr;
    };
    if (candidate == nullptr)
        return reject("null candidate");

    ObjId& cached_obj_id = g_state.is_host
        ? g_state.host_vehicle_obj_id
        : g_state.local_player_vehicle_obj_id;
    // Player::GetVehicle is a native pointer-returning path. During a map
    // transition, avoid dereferencing that pointer for refresh; the Player
    // object's validated m_vehicleObjId is the stable identity instead.
    hta::ai::Player* const player = hta::ai::Player::Instance();
    if (player == nullptr || player->m_vehicleObjId == kInvalidObjId)
        return reject("Player has no current vehicle ObjId");
    const ObjId stable_obj_id = player->m_vehicleObjId;
    if (cached_obj_id != stable_obj_id) {
        LOG_DEBUG("local weapon vehicle binding changed caller=%s cached=%d player=%d",
                  caller, cached_obj_id, stable_obj_id);
    }

    const auto lookup_bound_object = [&]() -> hta::ai::Obj* {
        // Re-read CServer::m_pObjects for every capture; a map transition may
        // replace the container itself.
        hta::ai::CServer* const server = hta::ai::CServer::Instance();
        hta::ai::ObjContainer* const objects =
            server != nullptr ? server->m_pObjects : nullptr;
        return objects != nullptr
            ? objects->GetEntityByObjId(stable_obj_id) : nullptr;
    };
    hta::ai::Obj* current_object = lookup_bound_object();

    // The candidate is untrusted until it is the exact object currently
    // owned by ObjContainer under the stable Player binding. Pointer equality
    // is intentionally the only operation on candidate before this point.
    if (current_object == nullptr || current_object != candidate)
        return reject(current_object == nullptr
                          ? "bound object is unavailable"
                          : "candidate pointer is stale");
    if (current_object->GetDeletedStatus())
        return reject("bound object is deleting");

    hta::ai::Vehicle* const current_vehicle =
        vehicle_from_object(current_object);
    if (current_vehicle == nullptr || current_vehicle != candidate)
        return reject("bound object is not a live Vehicle");
    return current_vehicle;
}

ObjId vehicle_get_seen_obj_id_abi(hta::ai::Vehicle* const vehicle) noexcept
{
#if defined(_MSC_VER) && defined(_M_IX86)
    ObjId result = kInvalidObjId;
    __asm {
        mov eax, vehicle
        mov edx, 00550A50h
        call edx
        mov result, eax
    }
    return result;
#else
    // The game ABI is x86-only. Do not make an ordinary C++ call on other
    // targets, because the declaration is not a portable member ABI.
    (void)vehicle;
    return kInvalidObjId;
#endif
}

void aim_and_fire_from_weapons_abi(hta::ai::Vehicle* const vehicle,
                                   const bool trigger_held,
                                   const float aim_speed,
                                   hta::ai::Obj* const target) noexcept
{
#if defined(_MSC_VER) && defined(_M_IX86)
    // Verified game ABI at 0x007C3370: ECX=vehicle, DL=trigger,
    // [ESP+4]=aim speed, [ESP+8]=target Obj*, and the callee executes ret 8.
    // Push the Obj* first so the float is the first stack argument.
    __asm {
        mov ecx, vehicle
        mov dl, trigger_held
        push target
        mov eax, aim_speed
        push eax
        mov eax, kWeaponAimAndFireFromWeaponsAddress
        call eax
    }
#else
    // The verified bridge is specific to the x86 game binary.
    (void)vehicle;
    (void)trigger_held;
    (void)aim_speed;
    (void)target;
#endif
}

hta::ai::Vehicle* current_local_weapon_vehicle_from_player(const char* const caller)
{
    // Do not call Player::GetVehicle here.  It returns a native raw pointer
    // which can outlive the object during a map/session transition.  The ObjId
    // is the game's stable identity and is resolved in the current container.
    hta::ai::Player* const player = hta::ai::Player::Instance();
    if (player == nullptr || player->m_vehicleObjId == kInvalidObjId)
        return nullptr;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return nullptr;
    hta::ai::Obj* const object =
        server->m_pObjects->GetEntityByObjId(player->m_vehicleObjId);
    hta::ai::Vehicle* vehicle = vehicle_from_object(object);
    return current_local_weapon_vehicle(vehicle, caller);
}

NetId resolve_local_native_weapon_target(hta::ai::Vehicle* const candidate)
{
    // Both engine target fields are process-local.  The custom field is
    // preferred only when it is current and network-bound; the seen-object
    // field is the EFA mouse-target seam maintained by the engine's weapon
    // update.  Never carry a prior target across an invalid current input.
    clear_local_native_weapon_target();
    hta::ai::Vehicle* const vehicle = current_local_weapon_vehicle(
        candidate, "native target capture");
    if (vehicle == nullptr)
        return kInvalidNetId;

    const ObjId custom_target_obj_id =
        vehicle->GetCustomControlWeaponsTargetObj();
    if (custom_target_obj_id != kInvalidObjId && custom_target_obj_id != 0) {
        NetId target_entity_id = kInvalidNetId;
        if (g_state.entities.lookup_net_id(custom_target_obj_id,
                                           target_entity_id)) {
            g_state.local_weapon_target_obj_id = custom_target_obj_id;
            g_state.local_weapon_target_entity_id = target_entity_id;
            return target_entity_id;
        }
        LOG_DEBUG("native weapon custom target objId=%d is not a network entity; checking seen target",
                  custom_target_obj_id);
    }

    const ObjId seen_target_obj_id = vehicle_get_seen_obj_id_abi(vehicle);
    if (seen_target_obj_id == kInvalidObjId || seen_target_obj_id == 0)
        return kInvalidNetId;

    NetId target_entity_id = kInvalidNetId;
    if (g_state.entities.lookup_net_id(seen_target_obj_id,
                                       target_entity_id)) {
        g_state.local_weapon_target_obj_id = seen_target_obj_id;
        g_state.local_weapon_target_entity_id = target_entity_id;
        return target_entity_id;
    }

    LOG_DEBUG("native weapon seen target objId=%d is not a network entity; using aim point",
              seen_target_obj_id);
    return kInvalidNetId;
}


void __fastcall weapon_look_at_point_hook(hta::ai::ComplexPhysicObj* object,
                                          const hta::CVector& point,
                                          float speed)
{
    // Preserve the original timing: Gun::LookAtPoint mutates the part graph
    // and must run before its world-space target is sampled for networking.
    g_weapon_look_at_point(object, point, speed);
    if (g_presenting_authoritative_death)
        return;
    if (object == nullptr || !g_state.session || !g_state.session->running() ||
        !should_relay_local_weapon_aim())
        return;
    // Do not call IsKindOf/GetSeenObjId on the hook argument until the raw
    // pointer has matched the current ObjContainer object for the bound id.
    hta::ai::Vehicle* const vehicle = current_local_weapon_vehicle(
        reinterpret_cast<hta::ai::Vehicle*>(object), "weapon look-at");
    if (vehicle == nullptr)
        return;

    const VehicleVector3 exact_aim{point.x, point.y, point.z};
    g_state.local_weapon_aim = exact_aim;
    g_state.local_weapon_aim_speed = speed;
    g_state.has_local_weapon_aim = true;
    if (g_state.is_host) {
        relay_host_weapon_presentation(vehicle, g_state.local_weapon_trigger_held,
                                       g_state.local_weapon_target_obj_id,
                                       g_state.local_weapon_gun_id,
                                       &exact_aim, speed);
    } else if (!submit_native_weapon_intent(
                   vehicle, g_state.local_weapon_trigger_held,
                   g_state.local_weapon_target_obj_id,
                   g_state.local_weapon_gun_id, &exact_aim, speed)) {
        LOG_ERROR("local weapon aim state was not sent");
    }
}

bool present_engine_gun_fire(hta::ai::Gun* const gun)
{
    if (gun == nullptr)
        return false;
    // Gun::_DoFire is the shell/damage boundary. Preserve the local muzzle
    // state/recoil path while returning before that boundary on clients.
    gun->m_bWasShot = true;
    gun->m_bJustShot = true;
    gun->DoRecoil();
    gun->_UpdateNodeFiringAction();
    return false;
}

void relay_host_weapon_presentation(hta::ai::Vehicle* vehicle,
                                    bool trigger_held, ObjId target_obj_id,
                                    std::int32_t gun_id,
                                    const VehicleVector3* aim_override,
                                    float aim_speed)
{
    if (!g_state.is_host || !g_state.session || !g_state.session->running() ||
        vehicle == nullptr)
        return;
    vehicle = current_local_weapon_vehicle(vehicle,
                                           "host weapon presentation");
    if (vehicle == nullptr)
        return;
    NetId shooter = kInvalidNetId;
    NetId target = kInvalidNetId;
    if (!g_state.entities.lookup_net_id(vehicle->GetId(), shooter)) {
        LOG_ERROR("weapon presentation missing shooter entity objId=%d",
                  vehicle->GetId());
        return;
    }
    if (target_obj_id != kInvalidObjId &&
        !g_state.entities.lookup_net_id(target_obj_id, target)) {
        LOG_DEBUG("weapon presentation target objId=%d is not a network entity",
                  target_obj_id);
        target = kInvalidNetId;
    }
    WeaponCommand command{};
    command.entity_id = shooter;
    command.sequence = g_state.next_weapon_sequence++;
    command.shot_id = command.sequence;
    command.client_tick = g_state.server_tick;
    command.gun_id = gun_id >= 0 && gun_id <= kMaxNetworkGunId ? gun_id : 0;
    if (command.gun_id != gun_id)
        LOG_DEBUG("weapon presentation gun id=%d is not network-addressable; using 0",
                  gun_id);
    command.trigger_held = trigger_held;
    command.target_entity_id = target;
    command.aim_speed = aim_speed;
    if (aim_override != nullptr && valid_weapon_aim_point(*aim_override)) {
        command.aim_point = *aim_override;
        command.has_aim_point = true;
    } else if (!capture_weapon_aim_point(*vehicle, command.aim_point)) {
        LOG_ERROR("weapon presentation aim capture failed entity=%u",
                  command.entity_id);
    } else {
        command.has_aim_point = true;
    }
    relay_weapon_command(command);
}

void publish_weapon_group_input(hta::ai::Vehicle* const vehicle,
                                const bool trigger_held,
                                const std::int32_t gun_id)
{
    if (!g_state.session || !g_state.session->running())
        return;
    hta::ai::Vehicle* const current_vehicle = current_local_weapon_vehicle(
        vehicle, "weapon group");
    if (current_vehicle == nullptr)
        return;

    const bool changed = g_state.local_weapon_trigger_held != trigger_held;
    g_state.local_weapon_trigger_held = trigger_held;
    (void)resolve_local_native_weapon_target(current_vehicle);
    if (gun_id >= 0 && gun_id <= kMaxNetworkGunId)
        g_state.local_weapon_gun_id = gun_id;

    // The group loop calls once per selected gun.  State changes (especially
    // release) must be sent immediately; continued hold is rate-limited.
    if (!changed) {
        if (!trigger_held || !should_submit_local_weapon_state(true))
            return;
    } else if (trigger_held) {
        // Record the regular resend deadline after an immediate press.
        (void)should_submit_local_weapon_state(true);
    }
    const VehicleVector3* const aim = g_state.has_local_weapon_aim
        ? &g_state.local_weapon_aim : nullptr;
    if (g_state.is_host) {
        relay_host_weapon_presentation(current_vehicle, trigger_held,
                                       g_state.local_weapon_target_obj_id,
                                       g_state.local_weapon_gun_id, aim,
                                       g_state.local_weapon_aim_speed);
    } else if (!submit_native_weapon_intent(
                   current_vehicle, trigger_held,
                   g_state.local_weapon_target_obj_id,
                   g_state.local_weapon_gun_id, aim,
                   g_state.local_weapon_aim_speed)) {
        LOG_ERROR("weapon-group input was not sent");
    }
}

bool fire_local_weapon_by_part_name(hta::ai::Vehicle* const vehicle,
                                    const hta::CStr* const gun_part_name,
                                    const bool trigger_held)
{
    if (g_presenting_authoritative_death)
        return g_fire_from_weapon_by_part_name(vehicle, gun_part_name,
                                                trigger_held);
    std::int32_t gun_id = g_state.local_weapon_gun_id;
    hta::ai::Vehicle* const current_vehicle = current_local_weapon_vehicle(
        vehicle, "weapon group hook");
    if (current_vehicle != nullptr && gun_part_name != nullptr) {
        hta::ai::VehiclePart* const part =
            current_vehicle->GetPartByName(*gun_part_name);
        if (part != nullptr)
            gun_id = part->GetPrototypeId();
    }
    publish_weapon_group_input(current_vehicle, trigger_held, gun_id);

    // The client still executes its native input/presentation path.  Only
    // the projectile/damage boundary is suppressed by gun_do_fire_hook;
    // host simulation remains the sole authority.
    hta::ai::Player* const player = hta::ai::Player::Instance();
    const bool suppress = !g_state.is_host && g_state.session &&
        g_state.session->running() && player != nullptr &&
        current_vehicle != nullptr && player->GetVehicle() == current_vehicle;
    const bool previously_suppressed = g_suppress_client_weapon_damage;
    g_suppress_client_weapon_damage = suppress;
    const bool result = g_fire_from_weapon_by_part_name(
        vehicle, gun_part_name, trigger_held);
    g_suppress_client_weapon_damage = previously_suppressed;
    return result;
}

bool __fastcall weapon_group_fire_by_part_name_hook(
    hta::ai::Vehicle* const vehicle, void*, const hta::CStr* const gun_part_name,
    const bool trigger_held)
{
    return fire_local_weapon_by_part_name(vehicle, gun_part_name, trigger_held);
}

bool __fastcall gun_do_fire_hook(hta::ai::Gun* gun, void*)
{
    if (g_presenting_authoritative_death)
        return g_gun_do_fire(gun);
    if (g_presenting_confirmed_network_fire)
        return g_gun_do_fire(gun);
    if (g_suppress_client_weapon_damage)
        return present_engine_gun_fire(gun);

    // The trigger transition is sent by WeaponGroup::KeepFire.  Do not
    // turn every native automatic-fire iteration into a new network command.
    if (gun != nullptr && !g_state.is_host && g_state.session &&
        g_state.session->running()) {
        hta::ai::Player* const player = hta::ai::Player::Instance();
        hta::ai::Vehicle* const local = player ? player->GetVehicle() : nullptr;
        if (local != nullptr && vehicle_from_gun_owner(gun) == local) {
            return false;
        }
    }

    const bool fired = g_gun_do_fire(gun);
    if (g_replaying_network_fire) {
        g_host_network_shot_fired = g_host_network_shot_fired || fired;
        if (fired && gun != nullptr && g_active_host_weapon_command != nullptr) {
            WeaponCommand confirmed = *g_active_host_weapon_command;
            confirmed.sequence = g_state.next_weapon_sequence++;
            confirmed.shot_id = confirmed.sequence;
            confirmed.trigger_held = true;
            confirmed.gun_id = gun->GetPrototypeId();
            confirmed.shells_in_current_charge = gun->GetShellsInCurrentCharge();
            confirmed.shells_in_pool = gun->GetShellsInPool();
            confirmed.has_ammo_state = true;
            (void)relay_weapon_command(confirmed);
        }
        return fired;
    }
    if (fired && g_state.is_host && gun != nullptr) {
        if (hta::ai::Vehicle* const owner = vehicle_from_gun_owner(gun)) {
            if (!g_state.combat_autotest_scenario.empty() &&
                is_player_controlled_vehicle(*owner)) {
                LOG_INFO("KRAKEN_COMBAT_AUTOTEST fired scenario=%s ownerObj=%d gun=%d targetObj=%d",
                         g_state.combat_autotest_scenario.c_str(), owner->GetId(),
                         gun->GetPrototypeId(), gun->m_targetObjId);
            }
            relay_host_weapon_presentation(owner, true, gun->m_targetObjId,
                                           gun->GetPrototypeId());
        } else
            LOG_ERROR("host gun fire has a non-vehicle owner gun=%d",
                      gun->GetPrototypeId());
    }
    return fired;
}

void __fastcall wanderer_spawn_hook(void* manager, void*)
{
    // The listen-server is the only process allowed to advance the random
    // wanderer generator.  Client ghosts arrive through EntitySpawn/Snapshot;
    // generating locally here caused each peer to get a different bot set.
    if (!g_state.is_host && IsSessionActive()) {
        LOG_DEBUG("suppressed client-side wanderer generation");
        return;
    }
    g_wanderer_spawn(manager);
}

bool __fastcall info_cone_is_kind_of(hta::m3d::Object* object, void*,
                                     hta::m3d::Class* class_object)
{
    if (object == nullptr) {
        LOG_ERROR("InfoCone ignored a null scene-graph object");
        return false;
    }
    return object->IsKindOf(class_object);
}

bool install_engine_safety_hooks()
{
    if (g_state.engine_safety_hooks_installed)
        return true;
    try {
        routines::ChangeCall(reinterpret_cast<void*>(kInfoConeIsKindOfCallSite),
                             &info_cone_is_kind_of);
        if (!g_state.combat_autotest_scenario.empty()) {
            routines::ChangeCall(reinterpret_cast<void*>(kControlsCallSite),
                                 &controls_hook);
        }
        routines::ChangeCall(
            reinterpret_cast<void*>(kWeaponGroupFireByPartNameCallSite),
            &weapon_group_fire_by_part_name_hook);
        routines::ChangeCall(reinterpret_cast<void*>(kGunDoFireCallSite),
                             &gun_do_fire_hook);
        routines::ChangeCall(
            reinterpret_cast<void*>(kVehicleWeaponLookAtPointCallSite),
            &weapon_look_at_point_hook);
        routines::ChangeCall(
            reinterpret_cast<void*>(kCustomWeaponLookAtPointCallSite),
            &weapon_look_at_point_hook);
        routines::ChangeCall(
            reinterpret_cast<void*>(kPlayerWeaponLookAtPointCallSite),
            &weapon_look_at_point_hook);
        routines::ChangeCall(reinterpret_cast<void*>(kWandererSpawnCallSite),
                             &wanderer_spawn_hook);
        g_state.engine_safety_hooks_installed = true;
        return true;
    }
    catch (const std::exception& error) {
        LOG_ERROR("failed to install multiplayer safety hooks: %s", error.what());
        return false;
    }
}

std::string find_autotest_save_maps_directory()
{
    namespace fs = std::filesystem;
    fs::path newest;
    fs::file_time_type newest_time{};
    bool found = false;
    std::error_code root_error;
    for (const fs::directory_entry& profile :
         fs::directory_iterator("data\\profiles", root_error)) {
        if (root_error || !profile.is_directory())
            continue;
        std::error_code saves_error;
        for (const fs::directory_entry& save : fs::directory_iterator(
                 profile.path() / "saves", saves_error)) {
            if (saves_error || !save.is_directory())
                continue;
            const fs::path maps = save.path() / "maps";
            std::error_code exists_error;
            if (!fs::is_regular_file(maps / "currentmap.xml", exists_error))
                continue;
            std::error_code time_error;
            const fs::file_time_type modified = fs::last_write_time(save.path(), time_error);
            if (time_error)
                continue;
            if (!found || modified > newest_time) {
                newest = maps;
                newest_time = modified;
                found = true;
            }
        }
    }
    return found ? newest.string() : std::string{};
}

// LoadSavedGame is a whole-program-optimized internal routine. Its actual
// ABI takes application in EAX and save-dir pointer on stack (validated by the
// established Jolt harness against the same hta.exe/PDB pair).
bool call_load_saved_game(void* application, hta::CStr* save_maps_directory)
{
    bool result = false;
    __asm {
        mov eax, application
        mov ecx, save_maps_directory
        push ecx
        mov edx, kLoadSavedGameAddress
        call edx
        mov result, al
    }
    return result;
}

void try_raid_autotest_bootstrap(void* application)
{
    const std::string save_maps_directory = find_autotest_save_maps_directory();
    if (save_maps_directory.empty()) {
        LOG_ERROR("raid autotest bootstrap: no data/profiles/*/saves/*/maps/currentmap.xml");
        return;
    }
    hta::CStr save_directory(save_maps_directory.c_str());
    const bool loaded = call_load_saved_game(application, &save_directory);
    LOG_INFO("raid autotest bootstrap: LoadSavedGame('%s') returned %d",
             save_maps_directory.c_str(), loaded ? 1 : 0);
}

void __fastcall process_all_events_raid_autotest_hook(void* application)
{
    reinterpret_cast<ProcessAllEventsFn>(kProcessAllEventsAddress)(application);
    if (!g_state.raid_autotest_enabled || g_state.raid_autotest_bootstrap_attempted)
        return;
    // ProcessAllEvents precedes OneFrame. The stock main menu must therefore
    // have completed before invoking its native load path.
    if (++g_state.raid_autotest_bootstrap_frames < 300)
        return;
    g_state.raid_autotest_bootstrap_attempted = true;
    try_raid_autotest_bootstrap(application);
}

bool install_raid_autotest_bootstrap_hook()
{
    if (!g_state.raid_autotest_enabled || g_state.raid_autotest_bootstrap_installed)
        return true;
    try {
        routines::ChangeCall(reinterpret_cast<void*>(kProcessAllEventsCallSite),
                             &process_all_events_raid_autotest_hook);
        g_state.raid_autotest_bootstrap_installed = true;
        LOG_INFO("raid autotest bootstrap installed (native saved-game loader)");
        return true;
    }
    catch (const std::exception& error) {
        LOG_ERROR("raid autotest bootstrap installation failed: %s", error.what());
        return false;
    }
}

bool install_impact_damage_hook()
{
    if (g_state.impact_damage_hook_installed)
        return true;

    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return false; // The first server tick may precede player creation.

    if (sizeof(void*) != sizeof(std::uint32_t)) {
        if (!g_state.impact_damage_hook_error_logged) {
            LOG_ERROR("impact damage hook requires a Win32 vehicle vtable");
            g_state.impact_damage_hook_error_logged = true;
        }
        return false;
    }

    void** const vtable = *reinterpret_cast<void***>(vehicle);
    if (vtable == nullptr) {
        if (!g_state.impact_damage_hook_error_logged) {
            LOG_ERROR("impact damage hook found a null Vehicle vtable");
            g_state.impact_damage_hook_error_logged = true;
        }
        return false;
    }
    constexpr std::size_t slot =
        kVehicleInflictDamageVtableOffset / sizeof(void*);
    void* const current = vtable[slot];
    if (current == reinterpret_cast<void*>(&vehicle_inflict_damage_hook)) {
        g_state.impact_damage_hook_installed = true;
        return true;
    }
    if (current != reinterpret_cast<void*>(kVehicleInflictDamageAddress)) {
        if (!g_state.impact_damage_hook_error_logged) {
            LOG_ERROR("impact damage hook refused unexpected Vehicle vtable[0x8c]=%p",
                      current);
            g_state.impact_damage_hook_error_logged = true;
        }
        return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE,
                        &old_protection)) {
        if (!g_state.impact_damage_hook_error_logged) {
            LOG_ERROR("impact damage hook VirtualProtect failed error=%lu",
                      static_cast<unsigned long>(GetLastError()));
            g_state.impact_damage_hook_error_logged = true;
        }
        return false;
    }
    g_state.vehicle_inflict_damage_original =
        reinterpret_cast<VehicleInflictDamageFn>(current);
    vtable[slot] = reinterpret_cast<void*>(&vehicle_inflict_damage_hook);
    DWORD ignored = 0;
    (void)VirtualProtect(&vtable[slot], sizeof(void*), old_protection,
                         &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
    g_state.impact_damage_hook_installed = true;
    LOG_INFO("impact damage hook installed Vehicle::InflictDamage vtable=0x8c");
    return true;
}

bool capture_impact_damage_input(hta::ai::Vehicle& target,
                                 const hta::ai::DamageInfo& info,
                                 ImpactDamage& event)
{
    EntityGeneration target_generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_net_id(target.GetId(),
                                        event.target_entity_id,
                                        target_generation)) {
        LOG_ERROR("authoritative damage target objId=%d is not registered",
                  target.GetId());
        return false;
    }
    event.target_generation = target_generation;

    ObjId attacker_obj_id = info.attackerId;
    bool attacker_resolved = g_state.entities.lookup_net_id(
        attacker_obj_id, event.attacker_entity_id,
        event.attacker_generation);
    if (!attacker_resolved && info.attackingAgentId != attacker_obj_id) {
        // Vehicle::InflictDamage carries both the immediate damaging object
        // and its controlling agent.  Shells created from a targetless aim
        // point can leave attackerId at the engine's zero sentinel while the
        // player identity remains in attackingAgentId.
        attacker_obj_id = info.attackingAgentId;
        attacker_resolved = g_state.entities.lookup_net_id(
            attacker_obj_id, event.attacker_entity_id,
            event.attacker_generation);
    }
    if (!attacker_resolved) {
        // Collision and other environment damage has no network entity. The
        // paired-zero identity is the only nullable attacker representation.
        event.attacker_entity_id = kInvalidNetId;
        event.attacker_generation = kInvalidEntityGeneration;
        LOG_DEBUG("authoritative damage attacker objId=%d is not registered; relaying as environment",
                  attacker_obj_id);
    }

    const int part_length = info.damagedPartName.length();
    const char* const part = info.damagedPartName.c_str();
    if (part_length < 0 ||
        static_cast<std::size_t>(part_length) > kImpactDamagePartMaxBytes ||
        (part_length != 0 && part == nullptr)) {
        LOG_ERROR("authoritative damage part name is invalid length=%d",
                  part_length);
        return false;
    }
    try {
        event.damaged_part.assign(part != nullptr ? part : "",
                                  static_cast<std::size_t>(part_length));
    }
    catch (...) {
        LOG_ERROR("authoritative damage part name allocation failed");
        return false;
    }

    // Environment impacts use -1 in the engine.  Wire format reserves only
    // non-negative ids, and zero is the stable "no gun" representation.
    event.gun_id = (std::max)(0, info.gunPrototypeId);
    event.damage_type = static_cast<std::int32_t>(info.damageType);
    event.damage = info.damage;
    event.hit_position = {info.hitPos.x, info.hitPos.y, info.hitPos.z};
    event.direction = {info.hitDir.x, info.hitDir.y, info.hitDir.z};
    event.normal = {info.normal.x, info.normal.y, info.normal.z};
    return true;
}

bool impact_damage_is_noop(const ImpactDamage& event) noexcept
{
    // LoRA: Vehicle::InflictDamage returns before its hit/decal path for
    // damage below the engine's approximately 0.01 threshold. A live packet
    // in that range is therefore not an impact to relay or present; a dead
    // result remains significant even when the final damage is zero.
    constexpr float kEngineMinimumImpactDamage = 0.01f;
    return !event.target_dead && event.damage < kEngineMinimumImpactDamage;
}

void __fastcall vehicle_inflict_damage_hook(
    hta::ai::Vehicle* const vehicle, void*,
    const hta::ai::DamageInfo& info)
{
    const VehicleInflictDamageFn original =
        g_state.vehicle_inflict_damage_original;
    if (vehicle == nullptr || original == nullptr) {
        LOG_ERROR("impact damage hook has no valid original target");
        return;
    }

    if (g_presenting_authoritative_impact) {
        original(vehicle, info);
        return;
    }

    // A connected client is presentation-only for combat.  This guard covers
    // all native collision/environment damage paths, not only weapon hooks.
    if (g_state.session && g_state.session->running() && !g_state.is_host)
        return;
    if (!g_state.session || !g_state.session->running() || !g_state.is_host) {
        original(vehicle, info);
        return;
    }

    // A destroyed Vehicle may still receive native collision/environment
    // callbacks while its wreck is settling.  Preserve those original calls,
    // but only replicate the one live-to-dead transition.  Replaying every
    // later callback would repeatedly execute _EvaluateToDead on clients.
    const bool was_dead = vehicle->_GetDeadStatus();
    ImpactDamage event{};
    const bool input_captured = capture_impact_damage_input(*vehicle, info,
                                                             event);
    // The authoritative engine transition is always exactly one original
    // invocation, even when the registry cannot represent this impact.
    original(vehicle, info);
    if (!input_captured || was_dead)
        return;

    event.event_id = g_state.next_impact_event_id++;
    if (g_state.next_impact_event_id == 0)
        g_state.next_impact_event_id = 1;
    event.server_tick = g_state.server_tick;
    const float post_health = vehicle->GetHealth();
    event.post_health = std::isfinite(post_health)
        ? (std::max)(0.0f, post_health) : -1.0f;
    event.target_dead = vehicle->_GetDeadStatus();
    // Entity 1 is the host-owned player vehicle for the lifetime of a
    // session.  EFA transitions that player to its shelter as soon as health
    // reaches zero, often before _GetDeadStatus becomes observable on this
    // object.  The authoritative zero-health transition is therefore the
    // reliable terminal event.  Defer teardown until the next network pump.
    if (event.target_entity_id == 1 && event.post_health <= 0.0f)
        g_state.host_defeat_session_end_pending = true;
    // EFA may transfer the destroyed host player to its shelter before the
    // next server tick exposes _GetDeadStatus().  For the integration test,
    // the authoritative zero-health result from the stock damage call is the
    // actual kill event; it is not a client-side health prediction.
    if (event.target_entity_id == 1 && event.post_health <= 0.0f &&
        g_state.combat_autotest_scenario == "client-kills-host" &&
        !g_state.combat_autotest_death_logged &&
        !g_state.controllers.empty() &&
        event.attacker_entity_id == g_state.controllers.front().entity_id) {
        g_state.combat_autotest_death_logged = true;
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST death scenario=%s shooter=%u target=%u",
                 g_state.combat_autotest_scenario.c_str(),
                 event.attacker_entity_id, event.target_entity_id);
    }
    if (event.target_dead && !g_state.combat_autotest_scenario.empty() &&
        !g_state.combat_autotest_death_logged) {
        NetId expected_attacker = kInvalidNetId;
        NetId expected_target = kInvalidNetId;
        if (g_state.combat_autotest_scenario == "host-kills-client" &&
            !g_state.controllers.empty()) {
            expected_attacker = 1;
            expected_target = g_state.controllers.front().entity_id;
        } else if (g_state.combat_autotest_scenario == "client-kills-host" &&
                   !g_state.controllers.empty()) {
            expected_attacker = g_state.controllers.front().entity_id;
            expected_target = 1;
        }
        if (event.attacker_entity_id == expected_attacker &&
            event.target_entity_id == expected_target) {
            g_state.combat_autotest_death_logged = true;
            LOG_INFO("KRAKEN_COMBAT_AUTOTEST death scenario=%s shooter=%u target=%u",
                     g_state.combat_autotest_scenario.c_str(),
                     event.attacker_entity_id, event.target_entity_id);
        } else {
            LOG_INFO("KRAKEN_COMBAT_AUTOTEST unrelated death scenario=%s shooter=%u target=%u expectedShooter=%u expectedTarget=%u",
                     g_state.combat_autotest_scenario.c_str(),
                     event.attacker_entity_id, event.target_entity_id,
                     expected_attacker, expected_target);
        }
    }

    if (!relay_impact_damage(event))
        LOG_ERROR("impact damage relay failed event=%u target=%u",
                  event.event_id, event.target_entity_id);
}

// Lua 5.1 represents every numeric literal as a float.  ScriptServer keeps
// that tag even for parameters declared as `int`, so the native mod seam must
// explicitly accept lossless integral floats rather than treating Lua numbers
// as a different API contract.
bool read_lua_integral_arg(const hta::m3d::sArg& argument,
                           std::int32_t& value) noexcept
{
    if (argument.GetType() == hta::m3d::sArg::ARGTYPE_INT) {
        value = argument.GetI();
        return true;
    }
    if (argument.GetType() != hta::m3d::sArg::ARGTYPE_FLOAT)
        return false;
    const double number = static_cast<double>(argument.GetF());
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < static_cast<double>((std::numeric_limits<std::int32_t>::min)()) ||
        number > static_cast<double>((std::numeric_limits<std::int32_t>::max)()))
        return false;
    value = static_cast<std::int32_t>(number);
    return true;
}

int __fastcall lua_submit_local_weapon_command(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t weapon_id = 0;
    if (args.m_numInArgs == 2 &&
        read_lua_integral_arg(args.m_InArgs[0], weapon_id) &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_BOOL)
        accepted = SubmitLocalWeaponCommand(weapon_id,
                                            args.m_InArgs[1].GetB());
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(accepted);
    return 0;
}

int __fastcall lua_request_loot(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t loot_id = 0, transaction_id = 0, amount = 0;
    if (args.m_numInArgs == 3 &&
        read_lua_integral_arg(args.m_InArgs[0], loot_id) &&
        read_lua_integral_arg(args.m_InArgs[1], transaction_id) &&
        read_lua_integral_arg(args.m_InArgs[2], amount)) {
        if (loot_id > 0 && transaction_id > 0 && amount > 0)
            accepted = RequestLocalLoot(
                static_cast<LootId>(loot_id),
                static_cast<LootTransactionId>(transaction_id),
                static_cast<std::uint32_t>(amount));
        else
            LOG_ERROR("reject Lua loot request loot=%d txn=%d amount=%d",
                      loot_id, transaction_id, amount);
    }
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_spawn_host_loot(hta::m3d::sArgStack& args)
{
    LootId loot_id = 0;
    std::int32_t container_prototype = 0, item_prototype = 0, amount = 0;
    if (args.m_numInArgs == 3 &&
        read_lua_integral_arg(args.m_InArgs[0], container_prototype) &&
        read_lua_integral_arg(args.m_InArgs[1], item_prototype) &&
        read_lua_integral_arg(args.m_InArgs[2], amount)) {
        if (container_prototype >= 0 && item_prototype >= 0 && amount > 0)
            loot_id = SpawnHostLoot(container_prototype, item_prototype,
                                    static_cast<std::uint32_t>(amount));
        else
            LOG_ERROR("reject Lua host loot spawn container=%d item=%d amount=%d",
                      container_prototype, item_prototype, amount);
    }
    if (hta::m3d::sArg* const output = args.newOut()) {
        output->m_type = hta::m3d::sArg::ARGTYPE_INT;
        output->m_i = static_cast<std::int32_t>(loot_id);
    }
    return 0;
}

int __fastcall lua_publish_host_world_loot(hta::m3d::sArgStack& args)
{
    WorldLootId loot_id = 0;
    std::int32_t container_prototype = 0, item_prototype = 0, amount = 0;
    if (args.m_numInArgs == 3 &&
        read_lua_integral_arg(args.m_InArgs[0], container_prototype) &&
        read_lua_integral_arg(args.m_InArgs[1], item_prototype) &&
        read_lua_integral_arg(args.m_InArgs[2], amount)) {
        if (container_prototype >= 0 && item_prototype >= 0 && amount > 0)
            loot_id = PublishHostWorldLoot(
                container_prototype, item_prototype,
                static_cast<std::uint32_t>(amount));
        else
            LOG_ERROR("reject Lua world loot publish container=%d item=%d amount=%d",
                      container_prototype, item_prototype, amount);
    }
    if (hta::m3d::sArg* const output = args.newOut()) {
        output->m_type = hta::m3d::sArg::ARGTYPE_INT;
        output->m_i = static_cast<std::int32_t>(loot_id);
    }
    return 0;
}

int __fastcall lua_publish_host_world_loot_object(hta::m3d::sArgStack& args)
{
    WorldLootId loot_id = 0;
    std::int32_t object_id = 0, item_prototype = 0, amount = 0;
    const bool has_three_ints =
        args.m_numInArgs == 3 &&
        read_lua_integral_arg(args.m_InArgs[0], object_id) &&
        read_lua_integral_arg(args.m_InArgs[1], item_prototype) &&
        read_lua_integral_arg(args.m_InArgs[2], amount);
    if (!has_three_ints) {
        // EFA may instantiate a large loot batch in one frame.  Reject every
        // malformed call, but emit its ABI diagnostic once per session so a
        // mod-side contract mistake cannot turn into a disk-I/O lag spike.
        if (!g_state.invalid_lua_world_loot_object_args_logged) {
            g_state.invalid_lua_world_loot_object_args_logged = true;
            const unsigned first_type = args.m_numInArgs > 0
                ? static_cast<unsigned>(args.m_InArgs[0].GetType()) : 0u;
            const unsigned second_type = args.m_numInArgs > 1
                ? static_cast<unsigned>(args.m_InArgs[1].GetType()) : 0u;
            const unsigned third_type = args.m_numInArgs > 2
                ? static_cast<unsigned>(args.m_InArgs[2].GetType()) : 0u;
            LOG_ERROR("reject Lua world loot object publish argCount=%d types=%u,%u,%u; expected int objectId, int itemPrototypeId, int amount",
                      args.m_numInArgs, first_type, second_type, third_type);
        }
    } else {
        if (object_id > 0 && item_prototype >= 0 && amount > 0)
            loot_id = PublishHostWorldLootObject(
                object_id, item_prototype, static_cast<std::uint32_t>(amount));
        else
            LOG_ERROR("reject Lua world loot object publish objectId=%d itemPrototype=%d amount=%d; expected objectId>0 itemPrototype>=0 amount>0",
                      object_id, item_prototype, amount);
    }
    if (hta::m3d::sArg* const output = args.newOut()) {
        output->m_type = hta::m3d::sArg::ARGTYPE_INT;
        output->m_i = static_cast<std::int32_t>(loot_id);
    }
    return 0;
}

int __fastcall lua_request_world_loot_pickup(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t loot_id = 0, generation = 0, transaction_id = 0, amount = 0;
    if (args.m_numInArgs == 4 &&
        read_lua_integral_arg(args.m_InArgs[0], loot_id) &&
        read_lua_integral_arg(args.m_InArgs[1], generation) &&
        read_lua_integral_arg(args.m_InArgs[2], transaction_id) &&
        read_lua_integral_arg(args.m_InArgs[3], amount)) {
        if (loot_id > 0 && generation > 0 &&
            generation <= static_cast<int>((std::numeric_limits<WorldLootGeneration>::max)()) &&
            transaction_id > 0 && amount > 0)
            accepted = RequestWorldLootPickup(
                static_cast<WorldLootId>(loot_id),
                static_cast<WorldLootGeneration>(generation),
                static_cast<std::uint32_t>(transaction_id),
                static_cast<std::uint32_t>(amount));
        else
            LOG_ERROR("reject Lua world loot pickup loot=%d generation=%d txn=%d amount=%d",
                      loot_id, generation, transaction_id, amount);
    }
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_query_world_loot_authority(hta::m3d::sArgStack& args)
{
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(args.m_numInArgs == 0 && QueryWorldLootAuthority());
    return 0;
}

void notify_lua_session_state(const bool active, const bool is_host)
{
    hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
    hta::m3d::ScriptServer* const script_server =
        kernel ? kernel->m_scriptServer : nullptr;
    if (script_server == nullptr)
        return;

    // hta.lib does not export sArgStack's lifetime functions.  Use the
    // ScriptServer's public synchronous executor with a fixed program instead
    // of fabricating an ABI-sensitive argument stack.  The callback is
    // optional so integrations which do not define it remain unaffected.
    const char* const callback = active
        ? (is_host
            ? "if MP_OnKrakenSessionState ~= nil then MP_OnKrakenSessionState(1, 1) end"
            : "if MP_OnKrakenSessionState ~= nil then MP_OnKrakenSessionState(1, 0) end")
        : "if MP_OnKrakenSessionState ~= nil then MP_OnKrakenSessionState(0, 0) end";
    const hta::m3d::eScriptError error = script_server->execute(
        callback, "kraken_session_state_bridge");
    if (error != hta::m3d::eScriptError::SUCCESS &&
        error != hta::m3d::eScriptError::NOT_INITIALIZED)
        LOG_ERROR("Lua session-state callback failed code=%u",
                  static_cast<unsigned>(error));
}

int __fastcall lua_begin_session(hta::m3d::sArgStack& args)
{
    const bool accepted = args.m_numInArgs == 0 && BeginSession();
    if (!accepted)
        notify_lua_session_state(false, false);
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_configure_session(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t port = 0, max_peers = 0;
    if (args.m_numInArgs == 4 &&
        args.m_InArgs[0].GetType() == hta::m3d::sArg::ARGTYPE_BOOL &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_STRING &&
        read_lua_integral_arg(args.m_InArgs[2], port) &&
        read_lua_integral_arg(args.m_InArgs[3], max_peers)) {
        const char* const address = args.m_InArgs[1].GetS();
        if (port >= 1024 && port <= 65535 && max_peers >= 2 &&
            max_peers <= 16 && address != nullptr)
            accepted = ConfigureSession(args.m_InArgs[0].GetB(), address,
                                        static_cast<unsigned short>(port),
                                        static_cast<unsigned int>(max_peers));
    }
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_end_session(hta::m3d::sArgStack& args)
{
    const bool accepted = args.m_numInArgs == 0 && EndSession();
    if (!accepted)
        notify_lua_session_state(false, false);
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_is_session_active(hta::m3d::sArgStack& args)
{
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(args.m_numInArgs == 0 && IsSessionActive());
    return 0;
}

int __fastcall lua_is_host(hta::m3d::sArgStack& args)
{
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(args.m_numInArgs == 0 && IsHost());
    return 0;
}

int __fastcall lua_is_authority(hta::m3d::sArgStack& args)
{
    // This MVP deliberately has one authority: the listen-server host.  Keep
    // the name role-neutral so mods do not encode a "raid" or topology detail
    // into their gameplay code.
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(args.m_numInArgs == 0 && IsAuthority());
    return 0;
}

int __fastcall lua_is_authority_or_offline(hta::m3d::sArgStack& args)
{
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(args.m_numInArgs == 0 && IsAuthorityOrOffline());
    return 0;
}

int __fastcall lua_publish_host_entity(hta::m3d::sArgStack& args)
{
    NetId entity_id = kInvalidNetId;
    std::int32_t object_id = 0, entity_kind = 0;
    if (args.m_numInArgs == 2 &&
        read_lua_integral_arg(args.m_InArgs[0], object_id) &&
        read_lua_integral_arg(args.m_InArgs[1], entity_kind))
        entity_id = PublishHostEntity(object_id, entity_kind);
    if (hta::m3d::sArg* const output = args.newOut()) {
        output->m_type = hta::m3d::sArg::ARGTYPE_INT;
        output->m_i = static_cast<std::int32_t>(entity_id);
    }
    return 0;
}

int __fastcall lua_get_local_entity_id(hta::m3d::sArgStack& args)
{
    if (hta::m3d::sArg* const output = args.newOut()) {
        output->m_type = hta::m3d::sArg::ARGTYPE_INT;
        output->m_i = args.m_numInArgs == 0
            ? static_cast<std::int32_t>(LocalEntityId()) : 0;
    }
    return 0;
}

// Polls one validated client presentation event.  The engine owns and
// constructs this sArgStack; no native code fabricates Lua ABI objects.
// Returns: hasEvent, eventId, attackerId, targetId, gunId, damageType,
// damage, damagedPart, hitPosition, direction, normal, targetDead.
int __fastcall lua_poll_impact_fx(hta::m3d::sArgStack& args)
{
    hta::m3d::sArg* const has_event_output = args.newOut();
    if (has_event_output == nullptr)
        return 0;
    has_event_output->SetB(false);
    if (args.m_numInArgs != 0 || g_state.is_host ||
        g_state.pending_impact_fx.empty())
        return 0;

    std::array<hta::m3d::sArg*, 11> outputs{};
    for (hta::m3d::sArg*& output : outputs) {
        output = args.newOut();
        if (output == nullptr) {
            LOG_ERROR("MP_PollImpactFx output stack exhausted");
            return 0;
        }
    }

    const ImpactDamage& event = g_state.pending_impact_fx.front();
    const auto set_int = [](hta::m3d::sArg& output, std::int32_t value) {
        output.m_type = hta::m3d::sArg::ARGTYPE_INT;
        output.m_i = value;
    };
    set_int(*outputs[0], static_cast<std::int32_t>(event.event_id));
    set_int(*outputs[1], static_cast<std::int32_t>(event.attacker_entity_id));
    set_int(*outputs[2], static_cast<std::int32_t>(event.target_entity_id));
    set_int(*outputs[3], event.gun_id);
    set_int(*outputs[4], event.damage_type);
    outputs[5]->SetF(event.damage);
    outputs[6]->SetS(event.damaged_part.c_str());
    outputs[7]->SetV(hta::CVector(event.hit_position.x,
                                  event.hit_position.y,
                                  event.hit_position.z));
    outputs[8]->SetV(hta::CVector(event.direction.x,
                                  event.direction.y,
                                  event.direction.z));
    outputs[9]->SetV(hta::CVector(event.normal.x, event.normal.y,
                                  event.normal.z));
    outputs[10]->SetB(event.target_dead);
    has_event_output->SetB(true);
    g_state.pending_impact_fx.erase(g_state.pending_impact_fx.begin());
    return 0;
}

void register_lua_api()
{
    hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
    hta::m3d::ScriptServer* const script_server =
        kernel ? kernel->m_scriptServer : nullptr;
    if (script_server == nullptr)
        return;
    const hta::m3d::eScriptError error = script_server->registerGlobalFunction(
        &lua_submit_local_weapon_command, "MP_SubmitWeaponCommand", "bool",
        "int gunId, bool trigger", "Submit weapon intent to the session host");
    LOG_INFO("Lua API MP_SubmitWeaponCommand registered code=%u",
             static_cast<unsigned>(error));
    const hta::m3d::eScriptError request_error = script_server->registerGlobalFunction(&lua_request_loot,
        "MP_RequestLoot", "bool", "int lootId, int transactionId, int amount",
        "Request an idempotent loot pickup from the session host");
    const hta::m3d::eScriptError spawn_error = script_server->registerGlobalFunction(&lua_spawn_host_loot,
        "MP_SpawnHostLoot", "int", "int chestPrototypeId, int itemPrototypeId, int amount",
        "Host-only: spawn a chest-backed loot record");
    const hta::m3d::eScriptError publish_world_loot_error =
        script_server->registerGlobalFunction(&lua_publish_host_world_loot,
            "MP_PublishHostWorldLoot", "int",
            "int containerPrototypeId, int itemPrototypeId, int amount",
            "Host-only: publish generic authoritative world loot");
    const hta::m3d::eScriptError publish_world_loot_object_error =
        script_server->registerGlobalFunction(&lua_publish_host_world_loot_object,
            "MP_PublishHostWorldLootObject", "int",
            "int objectId, int itemPrototypeId, int amount",
            "Host-only: publish authoritative loot from an existing chest object");
    const hta::m3d::eScriptError request_world_loot_error =
        script_server->registerGlobalFunction(&lua_request_world_loot_pickup,
            "MP_RequestWorldLootPickup", "bool",
            "int lootId, int generation, int transactionId, int amount",
            "Request an idempotent pickup from the world-loot authority");
    const hta::m3d::eScriptError query_world_loot_authority_error =
        script_server->registerGlobalFunction(&lua_query_world_loot_authority,
            "MP_QueryWorldLootAuthority", "bool", "",
            "Whether this peer is authoritative for shared world loot");
    const hta::m3d::eScriptError begin_error = script_server->registerGlobalFunction(&lua_begin_session,
        "MP_BeginSession", "bool", "", "Enter the multiplayer session from a local shelter");
    const hta::m3d::eScriptError configure_error = script_server->registerGlobalFunction(&lua_configure_session,
        "MP_ConfigureSession", "bool", "bool host, string address, int port, int maxPeers",
        "Configure the LAN listen server or client before starting a session");
    const hta::m3d::eScriptError end_error = script_server->registerGlobalFunction(&lua_end_session,
        "MP_EndSession", "bool", "", "Leave the multiplayer session and return to a local shelter");
    const hta::m3d::eScriptError active_error = script_server->registerGlobalFunction(&lua_is_session_active,
        "MP_IsSessionActive", "bool", "", "Whether the multiplayer session is active");
    const hta::m3d::eScriptError host_error = script_server->registerGlobalFunction(&lua_is_host,
        "MP_IsHost", "bool", "", "Whether this peer hosts the current session");
    const hta::m3d::eScriptError authority_error = script_server->registerGlobalFunction(&lua_is_authority,
        "MP_IsAuthority", "bool", "", "Whether this peer may author shared world state");
    const hta::m3d::eScriptError authority_or_offline_error =
        script_server->registerGlobalFunction(&lua_is_authority_or_offline,
            "MP_IsAuthorityOrOffline", "bool", "",
            "Whether shared-world logic may run in authority or offline mode");
    const hta::m3d::eScriptError publish_entity_error =
        script_server->registerGlobalFunction(&lua_publish_host_entity,
            "MP_PublishHostEntity", "int", "int objectId, int kind",
            "Host-only: publish a created vehicle with stable NetId metadata");
    const hta::m3d::eScriptError entity_error = script_server->registerGlobalFunction(&lua_get_local_entity_id,
        "MP_GetLocalEntityId", "int", "", "Stable multiplayer identity, or zero before assignment");
    const hta::m3d::eScriptError impact_poll_error =
        script_server->registerGlobalFunction(&lua_poll_impact_fx,
            "MP_PollImpactFx",
            "bool, int, int, int, int, int, float, string, vector, vector, vector, bool",
            "", "Poll one validated client impact presentation event");
    LOG_INFO("Lua multiplayer API registered weapon=%u loot_request=%u loot_spawn=%u world_publish=%u world_object_publish=%u world_request=%u world_authority=%u begin=%u configure=%u end=%u active=%u host=%u authority=%u authority_or_offline=%u publish_entity=%u entity=%u impact_poll=%u",
             static_cast<unsigned>(error), static_cast<unsigned>(request_error),
             static_cast<unsigned>(spawn_error),
             static_cast<unsigned>(publish_world_loot_error),
             static_cast<unsigned>(publish_world_loot_object_error),
             static_cast<unsigned>(request_world_loot_error),
             static_cast<unsigned>(query_world_loot_authority_error),
             static_cast<unsigned>(begin_error),
             static_cast<unsigned>(configure_error), static_cast<unsigned>(end_error),
             static_cast<unsigned>(active_error), static_cast<unsigned>(host_error),
             static_cast<unsigned>(authority_error),
             static_cast<unsigned>(authority_or_offline_error),
             static_cast<unsigned>(publish_entity_error),
             static_cast<unsigned>(entity_error),
             static_cast<unsigned>(impact_poll_error));
}

struct EffectiveConfig {
    bool enabled = false;
    bool host = true;
    std::string address = "127.0.0.1";
    std::uint16_t port = kDefaultPort;
    std::uint32_t max_peers = 16;
    bool spawn_together = true;
    bool autostart = true;
    bool auto_lan = true;
};

std::optional<EffectiveConfig> g_lifecycle_config;

std::optional<std::string> environment(const char* name)
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value, length > 0 ? length - 1 : 0);
    std::free(value);
    return result;
}

std::uint32_t environment_uint(const char* name, std::uint32_t fallback,
                               std::uint32_t minimum,
                               std::uint32_t maximum)
{
    const auto value = environment(name);
    if (!value || value->empty())
        return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value->c_str(), &end, 10);
    if (end == value->c_str() || *end != '\0')
        return fallback;
    return (std::clamp)(static_cast<std::uint32_t>(parsed), minimum, maximum);
}

EffectiveConfig effective_config(const Config& config)
{
    EffectiveConfig result;
    result.enabled = environment_uint("KRAKEN_MP_ENABLED",
                                      config.multiplayer_enabled.value, 0, 1) != 0;
    result.host = environment_uint("KRAKEN_MP_HOST",
                                   config.multiplayer_host.value, 0, 1) != 0;
    result.address = environment("KRAKEN_MP_ADDRESS")
                         .value_or(config.multiplayer_address.value);
    result.port = static_cast<std::uint16_t>(environment_uint(
        "KRAKEN_MP_PORT", config.multiplayer_port.value, 1024, 65535));
    result.max_peers = environment_uint("KRAKEN_MP_MAX_PEERS",
                                        config.multiplayer_max_peers.value, 2, 16);
    result.spawn_together = environment_uint("KRAKEN_MP_SPAWN_TOGETHER",
        config.multiplayer_spawn_together.value, 0, 1) != 0;
    result.autostart = environment_uint("KRAKEN_MP_AUTOSTART", 1, 0, 1) != 0;
    result.auto_lan = environment_uint("KRAKEN_MP_AUTO_LAN", 1, 0, 1) != 0;
    return result;
}

SnapshotTimestampMs now_ms()
{
    return static_cast<SnapshotTimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

bool sequence_is_newer(std::uint32_t candidate, std::uint32_t current)
{
    return static_cast<std::int32_t>(candidate - current) > 0;
}

RemoteEntity* find_or_add_remote(NetId entity_id)
{
    const auto found = std::find_if(
        g_state.remote_entities.begin(), g_state.remote_entities.end(),
        [entity_id](const RemoteEntity& remote) {
            return remote.entity_id == entity_id;
        });
    if (found != g_state.remote_entities.end())
        return &*found;
    if (g_state.remote_entities.size() >= kMaxRemoteEntities)
        return nullptr;

    g_state.remote_entities.push_back(RemoteEntity{entity_id});
    return &g_state.remote_entities.back();
}

RemoteEntity* find_remote(NetId entity_id)
{
    const auto found = std::find_if(
        g_state.remote_entities.begin(), g_state.remote_entities.end(),
        [entity_id](const RemoteEntity& remote) {
            return remote.entity_id == entity_id;
        });
    return found == g_state.remote_entities.end() ? nullptr : &*found;
}

PeerController* find_controller(PeerId peer)
{
    const auto found = std::find_if(g_state.controllers.begin(),
                                    g_state.controllers.end(),
                                    [peer](const PeerController& c) { return c.peer == peer; });
    return found == g_state.controllers.end() ? nullptr : &*found;
}

bool uses_native_player_slot_entity(const NetId entity_id)
{
    return (g_state.is_host && entity_id == 1) ||
        (!g_state.is_host && entity_id == g_state.local_entity_id);
}

HostEntity* find_host_entity(NetId entity_id)
{
    const auto found = std::find_if(
        g_state.host_entities.begin(), g_state.host_entities.end(),
        [entity_id](const HostEntity& entity) {
            return entity.entity_id == entity_id;
        });
    return found == g_state.host_entities.end() ? nullptr : &*found;
}

HostEntity* find_host_entity_by_object(ObjId object_id)
{
    const auto found = std::find_if(
        g_state.host_entities.begin(), g_state.host_entities.end(),
        [object_id](const HostEntity& entity) {
            return entity.active && entity.object_id == object_id;
        });
    return found == g_state.host_entities.end() ? nullptr : &*found;
}

NetId allocate_dynamic_entity_id()
{
    constexpr NetId kFirstDynamicEntityId = 1000;
    constexpr NetId kLastDynamicEntityId = (std::numeric_limits<NetId>::max)();
    for (std::uint64_t attempts = 0;
         attempts <= static_cast<std::uint64_t>(kLastDynamicEntityId -
                                                kFirstDynamicEntityId);
         ++attempts) {
        NetId candidate = g_state.next_dynamic_entity_id++;
        if (g_state.next_dynamic_entity_id < kFirstDynamicEntityId)
            g_state.next_dynamic_entity_id = kFirstDynamicEntityId;
        if (candidate < kFirstDynamicEntityId)
            continue;
        ObjId ignored = kInvalidObjId;
        if (!g_state.entities.lookup_obj_id(candidate, ignored))
            return candidate;
    }
    LOG_ERROR("network entity id space exhausted at dynamic id=%u",
              g_state.next_dynamic_entity_id);
    return kInvalidNetId;
}

hta::ai::Vehicle* find_vehicle(
    NetId entity_id, EntityGeneration expected_generation = 0);
float health_fraction(const hta::ai::Vehicle& vehicle);
VehicleVector3 to_snapshot_vector(const hta::CVector& value);
hta::CVector to_engine_vector(const VehicleVector3& value);
void apply_visual_weapon_aim(hta::ai::Vehicle& vehicle,
                             const VehicleVector3& aim_point,
                             float elapsed_time);
bool drive_network_weapon_presentation(hta::ai::Vehicle& vehicle,
                                       const WeaponCommand& command);
void apply_network_weapon_ammo(hta::ai::Vehicle& vehicle,
                               const WeaponCommand& command);
hta::Quaternion to_engine_quaternion(const VehicleQuaternion& value);
VehicleQuaternion to_snapshot_quaternion(const hta::Quaternion& value);
hta::CVector shared_spawn_position(const hta::CVector& host_position,
                                   PlayerSlotIndex slot);
hta::ai::Vehicle* ensure_remote_vehicle_replica(RemoteEntity& remote,
                                                const VehicleSnapshot& snapshot);
void materialize_remote_vehicle_replicas();

bool is_player_controlled_vehicle(const hta::ai::Vehicle& vehicle)
{
    const ObjId object_id = vehicle.GetId();
    if (object_id == g_state.host_vehicle_obj_id)
        return true;
    if (hta::ai::Player* const player = hta::ai::Player::Instance();
        player != nullptr && player->GetVehicle() == &vehicle)
        return true;
    return std::any_of(g_state.controllers.begin(), g_state.controllers.end(),
        [object_id](const PeerController& controller) {
            return controller.vehicle_obj_id == object_id;
        });
}

bool is_replicable_npc_vehicle(hta::ai::Obj& object)
{
    hta::ai::Vehicle* const vehicle = vehicle_from_object(&object);
    if (vehicle == nullptr)
        return false;
    // A decision matrix is the engine's authoritative marker for AI-driven
    // traffic/wanderers.  Player vehicles are explicitly excluded above.
    return !is_player_controlled_vehicle(*vehicle) &&
        vehicle->m_AI.m_pDM != nullptr;
}

std::uint32_t loadout_revision(const LoadoutProfile& profile);

bool capture_vehicle_loadout(const hta::ai::Vehicle& vehicle,
                             NetId entity_id,
                             EntityGeneration generation,
                             LoadoutProfile& profile)
{
    if (entity_id == kInvalidNetId || generation == kInvalidEntityGeneration)
        return false;
    profile = {};
    profile.entity_id = entity_id;
    profile.generation = generation;
    const auto names = vehicle.GetAttachedPartNames();
    if (names.size() > kMaxLoadoutParts)
        return false;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const hta::CStr& slot = names[index];
        const hta::ai::VehiclePart* const part = vehicle.GetPartByName(slot);
        const hta::ai::VehiclePartPrototypeInfo* const prototype =
            part ? part->GetPrototypeInfo() : nullptr;
        if (prototype == nullptr || slot.c_str() == nullptr ||
            prototype->m_prototypeName.c_str() == nullptr)
            continue;
        profile.parts.push_back({slot.c_str(), prototype->m_prototypeName.c_str()});
    }
    profile.revision = loadout_revision(profile);
    return profile.revision != 0;
}

void retire_network_vehicle(hta::ai::ObjContainer& objects,
                            hta::ai::Obj& object,
                            hta::ai::Vehicle& vehicle,
                            bool hide_visual,
                            bool preserve_destroyed_visual = false)
{
    vehicle.SetThrottle(0.0f, false);
    vehicle.m_steerRadians = 0.0f;
    vehicle.SetBrake(1.0f);
    vehicle.m_bHandBrake = true;
    vehicle.SetNpcMotionControllerId(-1);
    vehicle.m_AI.m_pDM = nullptr;
    vehicle.SetUpdatingByODE(false);
    vehicle.DisablePhysics();
    // A dead vehicle's native Update path owns the destroyed/exploded visual
    // transition. Keep that path alive after the network identity is retired;
    // hiding it or removing it from updates turns the death into a disappearing
    // ghost. Non-death retirement remains inert and deferred as before.
    if (preserve_destroyed_visual) {
        // Remote ghosts are intentionally moved to m_objIdsToNotUpdate while
        // alive.  Re-adding a destroyed ghost is required for Vehicle::Update
        // to execute the native dead actions/explosion presentation before
        // the map-owned container eventually cleans it up.
        objects.AddObjToUpdate(&object);
    } else {
        objects.AddObjToNotUpdate(&object);
        if (hide_visual)
            vehicle.SetInvisible();
    }
}

bool vehicle_is_destroyed(const hta::ai::Vehicle& vehicle)
{
    return vehicle._GetDeadStatus() || vehicle.GetHealth() <= 0.0f;
}

HostEntity* register_host_entity(hta::ai::Vehicle& vehicle, EntityKind kind)
{
    if (!g_state.is_host || !IsSessionActive() ||
        kind == EntityKind::PlayerVehicle || !is_valid_entity_kind(kind))
        return nullptr;

    const ObjId object_id = vehicle.GetId();
    if (HostEntity* const existing = find_host_entity_by_object(object_id)) {
        if (existing->kind != kind)
            LOG_ERROR("host entity kind collision objId=%d entity=%u oldKind=%u newKind=%u",
                      object_id, existing->entity_id,
                      static_cast<unsigned>(existing->kind),
                      static_cast<unsigned>(kind));
        return existing->kind == kind ? existing : nullptr;
    }

    NetId entity_id = kInvalidNetId;
    EntityGeneration ignored_generation = kInvalidEntityGeneration;
    if (g_state.entities.lookup_net_id(object_id, entity_id,
                                        ignored_generation)) {
        LOG_ERROR("host object already has a non-NPC network identity objId=%d entity=%u",
                  object_id, entity_id);
        return nullptr;
    }

    entity_id = allocate_dynamic_entity_id();
    if (entity_id == kInvalidNetId)
        return nullptr;
    constexpr EntityGeneration generation = kInitialEntityGeneration;
    const EntityRegistryBindResult bound =
        g_state.entities.bind(entity_id, object_id, generation);
    if (bound != EntityRegistryBindResult::Inserted) {
        LOG_ERROR("host entity registry bind failed entity=%u generation=%u objId=%d code=%u",
                  entity_id, static_cast<unsigned>(generation), object_id,
                  static_cast<unsigned>(bound));
        return nullptr;
    }

    HostEntity entity{};
    entity.entity_id = entity_id;
    entity.generation = generation;
    entity.kind = kind;
    entity.object_id = object_id;
    entity.prototype_id = vehicle.GetPrototypeId();
    if (!capture_vehicle_loadout(vehicle, entity_id, generation,
                                 entity.loadout)) {
        LOG_ERROR("host entity loadout capture failed entity=%u generation=%u objId=%d",
                  entity_id, static_cast<unsigned>(generation), object_id);
        (void)g_state.entities.unbind_net_id(entity_id);
        return nullptr;
    }
    g_state.host_entities.push_back(std::move(entity));
    HostEntity& registered = g_state.host_entities.back();
    LOG_INFO("host entity registered entity=%u generation=%u kind=%u objId=%d prototype=%d loadoutRevision=%u",
             registered.entity_id, static_cast<unsigned>(registered.generation),
             static_cast<unsigned>(registered.kind), registered.object_id,
             registered.prototype_id, registered.loadout.revision);
    return &registered;
}

LootRecord* find_loot(LootId loot_id)
{
    const auto found = std::find_if(g_state.loot_records.begin(),
                                    g_state.loot_records.end(),
        [loot_id](const LootRecord& record) {
            return record.world.loot_id == loot_id;
        });
    return found == g_state.loot_records.end() ? nullptr : &*found;
}

constexpr WorldLootId kMaxLuaWorldLootId = static_cast<WorldLootId>(
    (std::numeric_limits<std::int32_t>::max)());

LootSourceBinding* find_loot_source(const ObjId object_id)
{
    const auto found = std::find_if(
        g_state.loot_sources.begin(), g_state.loot_sources.end(),
        [object_id](const LootSourceBinding& source) {
            return source.object_id == object_id;
        });
    return found == g_state.loot_sources.end() ? nullptr : &*found;
}

LootId allocate_loot_id()
{
    constexpr std::uint64_t kMaxAttempts =
        static_cast<std::uint64_t>(kMaxLuaWorldLootId);
    for (std::uint64_t attempt = 0; attempt != kMaxAttempts; ++attempt) {
        const LootId candidate = g_state.next_loot_id++;
        if (g_state.next_loot_id == 0 ||
            g_state.next_loot_id > kMaxLuaWorldLootId)
            g_state.next_loot_id = 1;
        if (candidate != 0 && candidate <= kMaxLuaWorldLootId &&
            find_loot(candidate) == nullptr)
            return candidate;
    }
    LOG_ERROR("world loot id space exhausted");
    return 0;
}

WorldLootId allocate_loot_container_id()
{
    constexpr std::uint64_t kMaxAttempts =
        static_cast<std::uint64_t>(kMaxLuaWorldLootId);
    for (std::uint64_t attempt = 0; attempt != kMaxAttempts; ++attempt) {
        const WorldLootId candidate = g_state.next_loot_container_id++;
        if (g_state.next_loot_container_id == 0 ||
            g_state.next_loot_container_id > kMaxLuaWorldLootId)
            g_state.next_loot_container_id = 1;
        const bool already_bound = std::any_of(
            g_state.loot_sources.begin(), g_state.loot_sources.end(),
            [candidate](const LootSourceBinding& source) {
                return source.container_id == candidate;
            });
        if (candidate != 0 && candidate <= kMaxLuaWorldLootId &&
            !already_bound)
            return candidate;
    }
    LOG_ERROR("world loot container id space exhausted");
    return 0;
}

WorldLootGeneration allocate_loot_source_generation()
{
    constexpr std::uint32_t kMaxAttempts =
        (std::numeric_limits<WorldLootGeneration>::max)();
    for (std::uint32_t attempt = 0; attempt != kMaxAttempts; ++attempt) {
        const WorldLootGeneration candidate =
            g_state.next_loot_source_generation++;
        if (g_state.next_loot_source_generation == 0)
            g_state.next_loot_source_generation = 1;
        const bool already_bound = std::any_of(
            g_state.loot_sources.begin(), g_state.loot_sources.end(),
            [candidate](const LootSourceBinding& source) {
                return source.generation == candidate;
            });
        if (candidate != 0 && !already_bound)
            return candidate;
    }
    LOG_ERROR("world loot source generation space exhausted");
    return 0;
}

void tombstone_loot_source(const ObjId object_id, const char* const reason)
{
    LootSourceBinding* const source = find_loot_source(object_id);
    if (source == nullptr)
        return;
    if (!source->lifecycle_tombstoned) {
        source->lifecycle_tombstoned = true;
        LOG_ERROR("world loot source lifecycle tombstoned objId=%d generation=%u prototype=%d reason=%s",
                  source->object_id,
                  static_cast<unsigned>(source->generation),
                  source->prototype_id, reason != nullptr ? reason : "unknown");
    }
}

LootSourceBinding* bind_loot_source(ObjId object_id, hta::ai::Obj* object,
                                    std::int32_t prototype_id)
{
    LootSourceBinding* const existing = find_loot_source(object_id);
    if (existing != nullptr) {
        if (existing->lifecycle_tombstoned) {
            LOG_ERROR("reject world loot object tombstoned identity objId=%d generation=%u; no independent lifecycle token",
                      object_id, static_cast<unsigned>(existing->generation));
            return nullptr;
        }
        if (existing->object != object ||
            existing->prototype_id != prototype_id) {
            existing->lifecycle_tombstoned = true;
            LOG_ERROR("reject world loot object unsafe identity reuse objId=%d generation=%u oldPtr=%p newPtr=%p oldPrototype=%d newPrototype=%d; no independent lifecycle token",
                      object_id, static_cast<unsigned>(existing->generation),
                      static_cast<void*>(existing->object),
                      static_cast<void*>(object), existing->prototype_id,
                      prototype_id);
            return nullptr;
        }
        return existing;
    }

    const WorldLootId container_id = allocate_loot_container_id();
    if (container_id == 0)
        return nullptr;
    const WorldLootGeneration generation = allocate_loot_source_generation();
    if (generation == 0)
        return nullptr;
    g_state.loot_sources.push_back(LootSourceBinding{
        object_id, object, generation, container_id, prototype_id, false});
    LOG_INFO("world loot object source registered objId=%d generation=%u prototype=%d container=%u",
             object_id, static_cast<unsigned>(generation), prototype_id,
             container_id);
    return &g_state.loot_sources.back();
}

LootRecord* find_object_publication(
    ObjId object_id, WorldLootGeneration generation,
    std::int32_t item_prototype_id, std::uint32_t amount,
    NetId owner_entity_id, bool& conflicting)
{
    conflicting = false;
    for (LootRecord& record : g_state.loot_records) {
        if (!record.object_backed || record.chest_obj_id != object_id ||
            record.source_generation != generation ||
            record.world.item_prototype_id != item_prototype_id)
            continue;
        if (record.publication_amount == amount &&
            record.world.owner_entity_id == owner_entity_id)
            return &record;
        conflicting = true;
        return nullptr;
    }
    return nullptr;
}

bool send_world_loot_spawn(PeerId peer, const WorldLootRecord& record)
{
    if (!g_state.session) {
        LOG_ERROR("world loot spawn send rejected without session loot=%u peer=%u",
                  record.loot_id, peer);
        return false;
    }
    std::array<Byte, kWorldLootSpawnWireSize> payload{};
    const WorldLootCodecError encoded = encode_world_loot_spawn(
        WorldLootSpawn{record}, payload);
    if (!world_loot_codec_succeeded(encoded)) {
        LOG_ERROR("world loot spawn encode failed loot=%u peer=%u code=%u",
                  record.loot_id, peer, static_cast<unsigned>(encoded));
        return false;
    }
    const TransportResult sent = g_state.session->send(
        peer, MessageType::WorldLootSpawn, Channel::Reliable, payload);
    if (!sent) {
        LOG_ERROR("world loot spawn send failed loot=%u peer=%u code=%u",
                  record.loot_id, peer, static_cast<unsigned>(sent.code));
        return false;
    }
    return true;
}

void send_world_loot_delta(PeerId peer, const WorldLootDelta& delta)
{
    if (!g_state.session)
        return;
    std::array<Byte, kWorldLootDeltaWireSize> payload{};
    if (!world_loot_codec_succeeded(encode_world_loot_delta(delta, payload)))
        return;
    (void)g_state.session->send(peer, MessageType::WorldLootDelta,
                                Channel::Reliable, payload);
}

void send_world_loot_remove(PeerId peer, const WorldLootRemove& remove)
{
    if (!g_state.session)
        return;
    std::array<Byte, kWorldLootRemoveWireSize> payload{};
    if (!world_loot_codec_succeeded(encode_world_loot_remove(remove, payload)))
        return;
    (void)g_state.session->send(peer, MessageType::WorldLootRemove,
                                Channel::Reliable, payload);
}

void send_world_loot_pickup_result(
    PeerId peer, const WorldLootPickupResult& result)
{
    if (!g_state.session)
        return;
    std::array<Byte, kWorldLootPickupResultWireSize> payload{};
    if (!world_loot_codec_succeeded(encode_world_loot_pickup_result(
            result, payload)))
        return;
    (void)g_state.session->send(peer, MessageType::WorldLootPickupResult,
                                Channel::Reliable, payload);
}

bool same_world_loot_request(const WorldLootPickupRequest& lhs,
                             const WorldLootPickupRequest& rhs) noexcept
{
    return lhs.session_epoch == rhs.session_epoch &&
           lhs.entity_id == rhs.entity_id && lhs.loot_id == rhs.loot_id &&
           lhs.generation == rhs.generation &&
           lhs.transaction_id == rhs.transaction_id &&
           lhs.amount == rhs.amount;
}

void remember_world_loot_receipt(PeerId peer,
                                 const WorldLootPickupRequest& request,
                                 const WorldLootPickupResult& result)
{
    // Receipts live for the complete session epoch and are cleared only at
    // session lifecycle boundaries.  Never evict a completed transaction and
    // accidentally permit it to mutate inventory again.
    g_state.loot_receipts.push_back(WorldLootReceipt{peer, request, result});
}

void send_world_loot_baseline(PeerId peer)
{
    if (!g_state.session || !g_state.is_host)
        return;
    WorldLootBaseline baseline{};
    baseline.session_epoch = g_state.session_epoch;
    baseline.revision = g_state.world_loot_revision;
    baseline.records.reserve(g_state.loot_records.size());
    for (const LootRecord& loot : g_state.loot_records)
        if (loot.world.amount != 0)
            baseline.records.push_back(loot.world);
    std::vector<Byte> payload;
    if (!world_loot_codec_succeeded(
            encode_world_loot_baseline(baseline, payload))) {
        LOG_ERROR("world loot baseline encode failed peer=%u", peer);
        return;
    }
    (void)g_state.session->send(peer, MessageType::WorldLootBaseline,
                                Channel::Reliable, payload);
    LOG_INFO("world loot baseline peer=%u epoch=%u revision=%u records=%u",
             peer, baseline.session_epoch, baseline.revision,
             static_cast<unsigned>(baseline.records.size()));
}

void send_entity_assignment(PeerId peer, NetId entity_id)
{
    std::array<Byte, 4> payload{};
    for (int index = 0; index != 4; ++index)
        payload[index] = static_cast<Byte>(entity_id >> (8 * index));
    (void)g_state.session->send(peer, MessageType::EntityAssign,
                                Channel::Reliable, payload);
}

bool publish_entity_spawn(PeerId peer, NetId entity_id, EntityKind kind,
                          const hta::ai::Vehicle& vehicle,
                          NetId owner_entity_id = kInvalidNetId,
                          std::uint16_t generation = 1)
{
    const auto sent = std::find_if(
        g_state.spawn_publications.begin(), g_state.spawn_publications.end(),
        [peer, entity_id, generation](const SpawnPublication& publication) {
            return publication.peer == peer &&
                publication.entity_id == entity_id &&
                publication.generation == generation;
        });
    if (sent != g_state.spawn_publications.end())
        return true;

    EntitySpawn spawn{};
    spawn.entity_id = entity_id;
    spawn.generation = generation;
    spawn.kind = kind;
    spawn.prototype_id = static_cast<std::uint32_t>(vehicle.GetPrototypeId());
    spawn.owner_entity_id = owner_entity_id;
    spawn.belong = vehicle.m_belong;
    spawn.position = to_snapshot_vector(vehicle.GetPosition());
    spawn.rotation = to_snapshot_quaternion(vehicle.GetRotation());
    spawn.health_fraction = health_fraction(vehicle);
    std::array<Byte, kEntitySpawnWireSize> payload{};
    const EntityCodecError encoded = encode_entity_spawn(spawn, payload);
    if (!entity_codec_succeeded(encoded)) {
        LOG_ERROR("entity spawn encode failed entity=%u code=%u", entity_id,
                  static_cast<unsigned>(encoded));
        return false;
    }
    const TransportResult result = g_state.session->send(
        peer, MessageType::EntitySpawn, Channel::Reliable, payload);
    if (!result) {
        LOG_ERROR("entity spawn send failed entity=%u peer=%u code=%u",
                  entity_id, peer, static_cast<unsigned>(result.code));
        return false;
    }
    g_state.spawn_publications.push_back({peer, entity_id, generation});
    return true;
}

bool publish_entity_loadout(PeerId peer, const HostEntity& entity)
{
    const auto sent = std::find_if(
        g_state.loadout_publications.begin(), g_state.loadout_publications.end(),
        [&peer, &entity](const LoadoutPublication& publication) {
            return publication.peer == peer &&
                   publication.entity_id == entity.entity_id &&
                   publication.generation == entity.generation &&
                   publication.revision == entity.loadout.revision;
        });
    if (sent != g_state.loadout_publications.end())
        return true;

    std::vector<Byte> payload;
    if (!loadout_codec_succeeded(encode_loadout(entity.loadout, payload))) {
        LOG_ERROR("entity loadout encode failed entity=%u generation=%u revision=%u",
                  entity.entity_id, static_cast<unsigned>(entity.generation),
                  entity.loadout.revision);
        return false;
    }
    const TransportResult result = g_state.session->send(
        peer, MessageType::Loadout, Channel::Reliable, payload);
    if (!result) {
        LOG_ERROR("entity loadout send failed entity=%u generation=%u peer=%u code=%u",
                  entity.entity_id, static_cast<unsigned>(entity.generation), peer,
                  static_cast<unsigned>(result.code));
        return false;
    }
    g_state.loadout_publications.push_back(
        {peer, entity.entity_id, entity.generation, entity.loadout.revision});
    LOG_INFO("entity loadout published entity=%u generation=%u peer=%u revision=%u parts=%u",
             entity.entity_id, static_cast<unsigned>(entity.generation), peer,
             entity.loadout.revision,
             static_cast<unsigned>(entity.loadout.parts.size()));
    return true;
}

void send_entity_despawn(PeerId peer, NetId entity_id,
                         EntityGeneration generation,
                         std::uint8_t reason = 0)
{
    EntityDespawn despawn{entity_id, generation, reason};
    std::array<Byte, kEntityDespawnWireSize> payload{};
    const EntityCodecError encoded = encode_entity_despawn(despawn, payload);
    if (!entity_codec_succeeded(encoded)) {
        LOG_ERROR("entity despawn encode failed entity=%u generation=%u code=%u",
                  entity_id, static_cast<unsigned>(generation),
                  static_cast<unsigned>(encoded));
        return;
    }
    const TransportResult sent = g_state.session->send(
        peer, MessageType::EntityDespawn, Channel::Reliable, payload);
    if (!sent)
        LOG_ERROR("entity despawn send failed entity=%u peer=%u code=%u",
                  entity_id, peer, static_cast<unsigned>(sent.code));
    std::erase_if(g_state.spawn_publications,
                  [peer, entity_id](const SpawnPublication& publication) {
                      return publication.peer == peer &&
                          publication.entity_id == entity_id;
                  });
    std::erase_if(g_state.loadout_publications,
                  [peer, entity_id](const LoadoutPublication& publication) {
                      return publication.peer == peer &&
                          publication.entity_id == entity_id;
                  });
}

void retire_host_entity(HostEntity& entity, std::uint8_t reason)
{
    if (!entity.active)
        return;
    entity.active = false;

    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Obj* const object = server && server->m_pObjects
        ? server->m_pObjects->GetEntityByObjId(entity.object_id) : nullptr;
    if (hta::ai::Vehicle* const vehicle = vehicle_from_object(object)) {
        const bool destroyed = vehicle_is_destroyed(*vehicle);
        retire_network_vehicle(*server->m_pObjects, *object, *vehicle,
                               !destroyed, destroyed);
    }
    else {
        LOG_ERROR("host entity object became stale entity=%u generation=%u objId=%d",
                  entity.entity_id, static_cast<unsigned>(entity.generation),
                  entity.object_id);
    }

    ObjId mapped_object = kInvalidObjId;
    EntityGeneration mapped_generation = kInvalidEntityGeneration;
    if (g_state.entities.lookup_obj_id(entity.entity_id, mapped_object,
                                       mapped_generation) &&
        mapped_object == entity.object_id &&
        mapped_generation == entity.generation)
        (void)g_state.entities.unbind_net_id(entity.entity_id);

    for (const PeerId peer : g_state.peers)
        send_entity_despawn(peer, entity.entity_id, entity.generation, reason);
    LOG_INFO("host entity retired entity=%u generation=%u kind=%u reason=%u",
             entity.entity_id, static_cast<unsigned>(entity.generation),
             static_cast<unsigned>(entity.kind), static_cast<unsigned>(reason));
}

void reconcile_host_entities()
{
    if (!g_state.is_host)
        return;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr) {
        LOG_ERROR("host NPC reconciliation has no object container");
        return;
    }

    // Register live AI vehicles before the next native update.  Damage is
    // resolved inside that update, so waiting until snapshot capture would
    // lose a first-frame hit before capture_impact_damage_input can attach a
    // stable network identity to it.
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
        if (object == nullptr || vehicle == nullptr ||
            is_player_controlled_vehicle(*vehicle) ||
            object->GetDeletedStatus() || vehicle->_GetDeadStatus() ||
            vehicle->GetHealth() <= 0.0f ||
            !is_replicable_npc_vehicle(*object) ||
            find_host_entity_by_object(vehicle->GetId()) != nullptr)
            continue;
        (void)register_host_entity(*vehicle, EntityKind::NpcVehicle);
    }
    if (g_state.host_entities.empty())
        return;
    for (HostEntity& entity : g_state.host_entities) {
        if (!entity.active)
            continue;
        hta::ai::Obj* const object =
            server->m_pObjects->GetEntityByObjId(entity.object_id);
        hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
        if (object == nullptr || vehicle == nullptr || object->GetDeletedStatus() ||
            vehicle->_GetDeadStatus() || vehicle->GetHealth() <= 0.0f) {
            retire_host_entity(entity, vehicle != nullptr ? 2u : 3u);
            continue;
        }
        if (vehicle->GetPrototypeId() != entity.prototype_id) {
            LOG_ERROR("host entity prototype changed entity=%u generation=%u old=%d new=%d",
                      entity.entity_id, static_cast<unsigned>(entity.generation),
                      entity.prototype_id, vehicle->GetPrototypeId());
            retire_host_entity(entity, 4u);
            continue;
        }
        LoadoutProfile current{};
        if (!capture_vehicle_loadout(*vehicle, entity.entity_id,
                                     entity.generation, current)) {
            LOG_ERROR("host entity loadout recapture failed entity=%u generation=%u",
                      entity.entity_id, static_cast<unsigned>(entity.generation));
            continue;
        }
        if (current.revision != entity.loadout.revision)
            entity.loadout = std::move(current);
    }
}

void send_host_snapshot(PeerId peer, NetId entity_id,
                        const hta::ai::Vehicle& vehicle)
{
    VehicleSnapshot snapshot{};
    snapshot.entity_id = entity_id;
    snapshot.sequence = g_state.next_snapshot_sequence++;
    snapshot.server_tick = g_state.server_tick;
    snapshot.position = to_snapshot_vector(vehicle.GetPosition());
    snapshot.rotation = to_snapshot_quaternion(vehicle.GetRotation());
    snapshot.linear_velocity = to_snapshot_vector(vehicle.GetLinearVelocity());
    snapshot.angular_velocity = to_snapshot_vector(vehicle.GetAngularVelocity());
    snapshot.health_fraction = health_fraction(vehicle);
    std::array<Byte, kVehicleSnapshotWireSize> payload{};
    const VehicleSnapshotCodecError encoded =
        encode_vehicle_snapshot(snapshot, payload);
    if (!vehicle_snapshot_codec_succeeded(encoded)) {
        LOG_ERROR("baseline snapshot encode failed entity=%u code=%u", entity_id,
                  static_cast<unsigned>(encoded));
        return;
    }
    const TransportResult result = g_state.session->send(
        peer, MessageType::Snapshot, Channel::Unreliable, payload);
    if (!result)
        LOG_ERROR("baseline snapshot send failed entity=%u peer=%u code=%u",
                  entity_id, peer, static_cast<unsigned>(result.code));
}

void publish_host_baseline_to_peer(PeerId peer)
{
    if (!g_state.is_host || !g_state.session || !g_state.session->running())
        return;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server != nullptr && server->m_pObjects != nullptr) {
        for (auto iterator = server->m_pObjects->begin();
             iterator != server->m_pObjects->end(); ++iterator) {
            hta::ai::Obj* const object = *iterator;
            hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
            if (object == nullptr || vehicle == nullptr ||
                is_player_controlled_vehicle(*vehicle) ||
                object->GetDeletedStatus() || vehicle->_GetDeadStatus() ||
                vehicle->GetHealth() <= 0.0f ||
                !is_replicable_npc_vehicle(*object) ||
                find_host_entity_by_object(vehicle->GetId()) != nullptr)
                continue;
            (void)register_host_entity(*vehicle, EntityKind::NpcVehicle);
        }
        reconcile_host_entities();
    }
    (void)bind_host_player_vehicle();
    EntityGeneration local_generation = kInvalidEntityGeneration;
    hta::ai::Vehicle* const local = find_vehicle(1);
    (void)g_state.entities.lookup_generation(1, local_generation);
    if (local != nullptr) {
        (void)publish_entity_spawn(peer, 1, EntityKind::PlayerVehicle, *local,
                                   kInvalidNetId, local_generation);
        HostEntity loadout_entity{};
        loadout_entity.entity_id = 1;
        loadout_entity.generation = local_generation;
        loadout_entity.kind = EntityKind::PlayerVehicle;
        if (capture_vehicle_loadout(*local, 1, local_generation,
                                    loadout_entity.loadout))
            (void)publish_entity_loadout(peer, loadout_entity);
        send_host_snapshot(peer, 1, *local);
    }
    for (const PeerController& controller : g_state.controllers) {
        hta::ai::Vehicle* const vehicle = find_vehicle(controller.entity_id);
        if (vehicle == nullptr)
            continue;
        EntityGeneration generation = kInvalidEntityGeneration;
        (void)g_state.entities.lookup_generation(controller.entity_id, generation);
        (void)publish_entity_spawn(peer, controller.entity_id,
                                   EntityKind::PlayerVehicle, *vehicle,
                                   controller.entity_id, generation);
        send_host_snapshot(peer, controller.entity_id, *vehicle);
        if (controller.has_loadout) {
            HostEntity loadout_entity{};
            loadout_entity.entity_id = controller.entity_id;
            loadout_entity.generation = generation;
            loadout_entity.kind = EntityKind::PlayerVehicle;
            loadout_entity.loadout = controller.loadout;
            loadout_entity.loadout.generation = generation;
            (void)publish_entity_loadout(peer, loadout_entity);
        }
    }
    for (const HostEntity& entity : g_state.host_entities) {
        if (!entity.active)
            continue;
        hta::ai::Vehicle* const vehicle = find_vehicle(entity.entity_id,
                                                        entity.generation);
        if (vehicle == nullptr)
            continue;
        (void)publish_entity_spawn(peer, entity.entity_id, entity.kind,
                                   *vehicle, kInvalidNetId, entity.generation);
        (void)publish_entity_loadout(peer, entity);
        send_host_snapshot(peer, entity.entity_id, *vehicle);
    }
    LOG_INFO("published host entity baseline peer=%u hostEntities=%u",
             peer, static_cast<unsigned>(g_state.host_entities.size()));
}

bool decode_entity_id(ByteView payload, NetId& entity_id)
{
    if (payload.size() != 4)
        return false;
    entity_id = 0;
    for (int index = 0; index != 4; ++index)
        entity_id |= static_cast<NetId>(static_cast<std::uint8_t>(payload[index]))
                     << (8 * index);
    return entity_id != kInvalidNetId;
}

void clear_remote_generation_payload(RemoteEntity& remote)
{
    remote.snapshots.clear();
    remote.last_sequence = 0;
    remote.has_sequence = false;
    remote.weapon = WeaponCommand{};
    remote.has_weapon = false;
    remote.presented_shot_id = 0;
    remote.loadout = LoadoutProfile{};
    remote.has_loadout = false;
    remote.applied_loadout_revision = 0;
}

bool retire_bound_remote_ghost_exact(NetId entity_id,
                                     EntityGeneration expected_generation,
                                     const char* reason,
                                     bool preserve_destroyed_visual = false)
{
    if (expected_generation == kInvalidEntityGeneration) {
        LOG_ERROR("refused ghost retirement entity=%u generation=0 reason=%s",
                  entity_id, reason);
        return false;
    }

    ObjId object_id = kInvalidObjId;
    EntityGeneration bound_generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_obj_id(entity_id, object_id,
                                        bound_generation)) {
        LOG_DEBUG("no bound ghost to retire entity=%u generation=%u reason=%s",
                  entity_id, static_cast<unsigned>(expected_generation), reason);
        return false;
    }
    if (bound_generation != expected_generation) {
        LOG_ERROR("refused mismatched ghost retirement entity=%u expectedGeneration=%u boundGeneration=%u objId=%d reason=%s",
                  entity_id, static_cast<unsigned>(expected_generation),
                  static_cast<unsigned>(bound_generation), object_id, reason);
        return false;
    }

    // ObjContainer removal is asynchronous. A just-retired replica can still
    // be referenced from an InfoCone scene cell during this frame, so map
    // cleanup owns destruction; packet processing only retires presentation.
    if (hta::ai::CServer* const server = hta::ai::CServer::Instance();
        server != nullptr && server->m_pObjects != nullptr) {
        if (hta::ai::Obj* const object =
                server->m_pObjects->GetEntityByObjId(object_id)) {
            if (hta::ai::Vehicle* const vehicle = vehicle_from_object(object)) {
                const bool destroyed = preserve_destroyed_visual ||
                    vehicle_is_destroyed(*vehicle);
                retire_network_vehicle(*server->m_pObjects, *object, *vehicle,
                                       !destroyed, destroyed);
                if (destroyed)
                    LOG_INFO("preserving destroyed ghost visual entity=%u generation=%u objId=%d reason=%s",
                             entity_id, static_cast<unsigned>(expected_generation),
                             object_id, reason);
            }
            else
                LOG_ERROR("bound ghost is not a vehicle entity=%u generation=%u objId=%d reason=%s",
                          entity_id, static_cast<unsigned>(expected_generation),
                          object_id, reason);
        }
        else {
            LOG_ERROR("bound ghost object missing entity=%u generation=%u objId=%d reason=%s",
                      entity_id, static_cast<unsigned>(expected_generation),
                      object_id, reason);
        }
    }
    else {
        LOG_ERROR("cannot resolve bound ghost entity=%u generation=%u objId=%d reason=%s",
                  entity_id, static_cast<unsigned>(expected_generation),
                  object_id, reason);
    }

    (void)g_state.entities.unbind_net_id(entity_id);
    LOG_INFO("retired exact ghost entity=%u generation=%u objId=%d reason=%s",
             entity_id, static_cast<unsigned>(expected_generation), object_id,
             reason);
    return true;
}

bool retire_stale_binding_for_replacement(NetId entity_id,
                                          EntityGeneration incoming_generation)
{
    ObjId object_id = kInvalidObjId;
    EntityGeneration bound_generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_obj_id(entity_id, object_id, bound_generation))
        return true;
    if (uses_native_player_slot_entity(entity_id)) {
        if (bound_generation > incoming_generation) {
            LOG_ERROR("rejected player slot generation replacement entity=%u incomingGeneration=%u newerBoundGeneration=%u",
                      entity_id, static_cast<unsigned>(incoming_generation),
                      static_cast<unsigned>(bound_generation));
            return false;
        }
        return release_player_slot_entity(entity_id, bound_generation,
                                           "generation replacement");
    }
    if (bound_generation > incoming_generation) {
        LOG_ERROR("rejected generation replacement entity=%u incomingGeneration=%u newerBoundGeneration=%u objId=%d",
                  entity_id, static_cast<unsigned>(incoming_generation),
                  static_cast<unsigned>(bound_generation), object_id);
        return false;
    }

    if (hta::ai::CServer* const server = hta::ai::CServer::Instance();
        server != nullptr && server->m_pObjects != nullptr) {
        if (hta::ai::Obj* const object =
                server->m_pObjects->GetEntityByObjId(object_id)) {
            if (hta::ai::Vehicle* const vehicle = vehicle_from_object(object)) {
                retire_network_vehicle(*server->m_pObjects, *object, *vehicle,
                                       true);
            }
            else
                LOG_ERROR("stale replacement binding is not a vehicle entity=%u boundGeneration=%u objId=%d",
                          entity_id, static_cast<unsigned>(bound_generation),
                          object_id);
        }
        else {
            LOG_ERROR("stale replacement object missing entity=%u boundGeneration=%u objId=%d",
                      entity_id, static_cast<unsigned>(bound_generation),
                      object_id);
        }
    }
    else {
        LOG_ERROR("cannot resolve stale replacement binding entity=%u boundGeneration=%u objId=%d",
                  entity_id, static_cast<unsigned>(bound_generation), object_id);
    }

    (void)g_state.entities.unbind_net_id(entity_id);
    LOG_INFO("cleared stale replacement ghost entity=%u boundGeneration=%u incomingGeneration=%u objId=%d",
             entity_id, static_cast<unsigned>(bound_generation),
             static_cast<unsigned>(incoming_generation), object_id);
    return true;
}

void mark_remote_despawned(RemoteEntity& remote,
                           EntityGeneration generation)
{
    clear_remote_generation_payload(remote);
    remote.has_spawn = false;
    remote.retired = true;
    remote.retired_generation = generation;
}

void reset_remote_for_generation_replacement(RemoteEntity& remote)
{
    clear_remote_generation_payload(remote);
    remote.generation = kInvalidEntityGeneration;
    remote.kind = EntityKind::PlayerVehicle;
    remote.prototype_id = -1;
    remote.belong = 0;
    remote.has_spawn = false;
    remote.spawn_snapshot = VehicleSnapshot{};
    remote.has_spawn_snapshot = false;
    remote.retired = false;
    remote.retired_generation = kInvalidEntityGeneration;
    remote.spawn_attempt.reset();
}

void schedule_entity_removal(NetId entity_id)
{
    if (uses_native_player_slot_entity(entity_id)) {
        EntityGeneration generation = kInvalidEntityGeneration;
        (void)g_state.entities.lookup_generation(entity_id, generation);
        (void)release_player_slot_entity(entity_id, generation,
                                          "network entity cleanup");
        return;
    }
    if (!g_state.is_host) {
        RemoteEntity* const remote = find_remote(entity_id);
        if (remote == nullptr || !remote->has_spawn ||
            remote->generation == kInvalidEntityGeneration) {
            LOG_DEBUG("skipped unspawned remote removal entity=%u", entity_id);
            return;
        }
        const EntityGeneration generation = remote->generation;
        const bool binding_retired = retire_bound_remote_ghost_exact(
            entity_id, generation, "remote cleanup");
        mark_remote_despawned(*remote, generation);
        LOG_INFO("remote cleanup accepted entity=%u generation=%u bindingRetired=%u",
                 entity_id, static_cast<unsigned>(generation),
                 binding_retired ? 1u : 0u);
        return;
    }

    ObjId object_id = kInvalidObjId;
    EntityGeneration generation = kInvalidEntityGeneration;
    if (g_state.entities.lookup_obj_id(entity_id, object_id, generation)) {
        // ObjContainer removal is asynchronous.  Removing a live network
        // vehicle from the pre-CServerUpdate packet pump leaves a stale item
        // in InfoCone's scene-graph cell and can crash either peer.  The raid
        // map owns cleanup at session/map end; retire the vehicle in place.
        if (hta::ai::CServer* const server = hta::ai::CServer::Instance();
            server != nullptr && server->m_pObjects != nullptr) {
            if (hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id)) {
                if (hta::ai::Vehicle* const vehicle = vehicle_from_object(object))
                    retire_network_vehicle(*server->m_pObjects, *object, *vehicle,
                                           true);
            }
        }
        (void)g_state.entities.unbind_net_id(entity_id);
        LOG_INFO("retired network entity=%u objId=%d; deferred map cleanup", entity_id,
                 object_id);
    }
}

void schedule_all_remote_removals()
{
    std::vector<NetId> entity_ids;
    entity_ids.reserve(g_state.remote_entities.size());
    for (const RemoteEntity& remote : g_state.remote_entities)
        entity_ids.push_back(remote.entity_id);
    for (const NetId entity_id : entity_ids)
        schedule_entity_removal(entity_id);
}

void broadcast_entity_despawn(NetId entity_id, EntityGeneration generation,
                              PeerId excluded_peer)
{
    for (const PeerId peer : g_state.peers)
        if (peer != excluded_peer)
            send_entity_despawn(peer, entity_id, generation, 1);
}

void receive_entity_assignment(const SessionEvent& event)
{
    if (g_state.is_host || event.payload.size() != 4)
        return;
    NetId entity = 0;
    for (int index = 0; index != 4; ++index)
        entity |= static_cast<NetId>(static_cast<std::uint8_t>(event.payload[index])) << (8 * index);
    if (entity == kInvalidNetId)
        return;
    g_state.local_entity_id = entity;
    (void)bind_local_player_vehicle();
    LOG_INFO("local entity assigned id=%u", entity);
}

void receive_entity_despawn(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    EntityDespawn despawn{};
    const EntityCodecError decoded = decode_entity_despawn(event.payload,
                                                            despawn);
    if (!entity_codec_succeeded(decoded)) {
        LOG_ERROR("peer=%u invalid entity despawn code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (despawn.entity_id == g_state.local_entity_id) {
        LOG_DEBUG("ignored local entity despawn entity=%u generation=%u",
                  despawn.entity_id,
                  static_cast<unsigned>(despawn.generation));
        return;
    }
    RemoteEntity* const remote = find_remote(despawn.entity_id);
    if (remote == nullptr) {
        LOG_DEBUG("ignored entity despawn without lifecycle state entity=%u generation=%u",
                  despawn.entity_id,
                  static_cast<unsigned>(despawn.generation));
        return;
    }
    if (despawn.generation == kInvalidEntityGeneration ||
        !remote->has_spawn ||
        remote->generation == kInvalidEntityGeneration ||
        remote->generation != despawn.generation) {
        LOG_ERROR("ignored non-current entity despawn entity=%u generation=%u hasSpawn=%u currentGeneration=%u",
                  despawn.entity_id, static_cast<unsigned>(despawn.generation),
                  remote->has_spawn ? 1u : 0u,
                  static_cast<unsigned>(remote->generation));
        return;
    }
    if (uses_native_player_slot_entity(despawn.entity_id)) {
        const bool released = release_player_slot_entity(
            despawn.entity_id, despawn.generation, "authoritative despawn");
        mark_remote_despawned(*remote, despawn.generation);
        LOG_INFO("authoritative player slot despawn entity=%u generation=%u released=%u",
                 despawn.entity_id, static_cast<unsigned>(despawn.generation),
                 released ? 1u : 0u);
        return;
    }
    const bool binding_retired = retire_bound_remote_ghost_exact(
        despawn.entity_id, despawn.generation, "authoritative despawn",
        despawn.reason == 2u);
    mark_remote_despawned(*remote, despawn.generation);
    LOG_INFO("authoritative ghost despawn accepted entity=%u generation=%u bindingRetired=%u",
             despawn.entity_id, static_cast<unsigned>(despawn.generation),
             binding_retired ? 1u : 0u);
}

void receive_entity_spawn(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    EntitySpawn spawn{};
    const EntityCodecError decoded = decode_entity_spawn(event.payload, spawn);
    if (!entity_codec_succeeded(decoded)) {
        LOG_ERROR("peer=%u invalid entity spawn code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (spawn.entity_id == g_state.local_entity_id)
        return; // the local player vehicle is map-owned, never ghosted.
    RemoteEntity* remote = find_or_add_remote(spawn.entity_id);
    if (remote == nullptr) {
        LOG_ERROR("remote entity limit reached; drop spawn entity=%u",
                  spawn.entity_id);
        return;
    }
    if (spawn.generation == kInvalidEntityGeneration) {
        LOG_ERROR("drop entity spawn with invalid generation entity=%u",
                  spawn.entity_id);
        return;
    }
    if (remote->retired &&
        remote->retired_generation != kInvalidEntityGeneration &&
        spawn.generation <= remote->retired_generation) {
        LOG_ERROR("drop retired/stale entity spawn entity=%u generation=%u retiredGeneration=%u",
                  spawn.entity_id, static_cast<unsigned>(spawn.generation),
                  static_cast<unsigned>(remote->retired_generation));
        return;
    }
    if (remote->has_spawn && remote->generation > spawn.generation) {
        LOG_ERROR("drop older entity spawn entity=%u generation=%u currentGeneration=%u",
                  spawn.entity_id, static_cast<unsigned>(spawn.generation),
                  static_cast<unsigned>(remote->generation));
        return;
    }

    const EntityGeneration prior_generation = remote->has_spawn
        ? remote->generation
        : remote->retired_generation;
    const bool generation_replacement =
        prior_generation != kInvalidEntityGeneration &&
        prior_generation != spawn.generation;
    if (generation_replacement) {
        if (!retire_stale_binding_for_replacement(spawn.entity_id,
                                                   spawn.generation))
            return;
        LOG_INFO("resetting reused entity id entity=%u oldGeneration=%u newGeneration=%u hadLoadout=%u hadSnapshots=%u hadWeapon=%u wasRetired=%u",
                 spawn.entity_id, static_cast<unsigned>(prior_generation),
                 static_cast<unsigned>(spawn.generation),
                 remote->has_loadout ? 1u : 0u,
                 remote->has_sequence ? 1u : 0u,
                 remote->has_weapon ? 1u : 0u,
                 remote->retired ? 1u : 0u);
        reset_remote_for_generation_replacement(*remote);
    }
    remote->generation = spawn.generation;
    remote->kind = spawn.kind;
    remote->prototype_id = static_cast<std::int32_t>(spawn.prototype_id);
    remote->belong = spawn.belong;
    remote->has_spawn = true;
    remote->spawn_snapshot = {};
    remote->spawn_snapshot.entity_id = spawn.entity_id;
    remote->spawn_snapshot.position = spawn.position;
    remote->spawn_snapshot.rotation = spawn.rotation;
    remote->spawn_snapshot.health_fraction = spawn.health_fraction;
    remote->has_spawn_snapshot = true;
    remote->retired = false;
    remote->retired_generation = kInvalidEntityGeneration;
    if (remote->kind == EntityKind::NpcVehicle) {
        // NPC combat is still host-only. Their replica carries no locally
        // simulated weapon state; snapshots remain the sole presentation
        // channel until authoritative NPC combat events are implemented.
        remote->weapon = WeaponCommand{};
        remote->has_weapon = false;
        remote->presented_shot_id = 0;
    }
    LOG_INFO("entity spawn entity=%u generation=%u kind=%u prototype=%d",
             spawn.entity_id, static_cast<unsigned>(spawn.generation),
             static_cast<unsigned>(spawn.kind), remote->prototype_id);
}

void receive_input(const SessionEvent& event)
{
    if (!g_state.is_host)
        return;
    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr)
        return;
    InputCommand input{};
    const InputCommandCodecError decoded = decode_input_command(event.payload, input);
    if (!input_command_codec_succeeded(decoded) || input.entity_id != controller->entity_id) {
        LOG_ERROR("drop input peer=%u code=%u", event.peer, static_cast<unsigned>(decoded));
        return;
    }
    // server_tick is process-local (each client starts it at a different
    // moment), so comparing it across the network rejects valid input. Packet
    // order is enforced by the per-peer sequence number below.
    if (controller->has_input && !sequence_is_newer(input.sequence, controller->last_sequence))
        return;
    controller->input = input;
    controller->last_sequence = input.sequence;
    controller->has_input = true;
}

bool relay_weapon_command(const WeaponCommand& command)
{
    std::array<Byte, kWeaponCommandWireSize> payload{};
    const WeaponCommandCodecError encoded =
        encode_weapon_command(command, payload);
    if (!weapon_command_codec_succeeded(encoded)) {
        LOG_ERROR("weapon presentation encode failed entity=%u shot=%u code=%u",
                  command.entity_id, command.shot_id,
                  static_cast<unsigned>(encoded));
        return false;
    }
    if (!g_state.session || !g_state.session->running()) {
        LOG_ERROR("weapon presentation relay unavailable entity=%u shot=%u",
                  command.entity_id, command.shot_id);
        return false;
    }
    bool sent_to_peer = false;
    for (const PeerId peer : g_state.peers) {
        const TransportResult result = g_state.session->send(
            peer, MessageType::WeaponCommand, Channel::Reliable, payload);
        if (!result) {
            LOG_ERROR("weapon presentation send failed entity=%u shot=%u peer=%u code=%u",
                      command.entity_id, command.shot_id, peer,
                      static_cast<unsigned>(result.code));
            continue;
        }
        sent_to_peer = true;
    }
    return sent_to_peer;
}

bool relay_impact_damage(const ImpactDamage& event)
{
    if (impact_damage_is_noop(event)) {
        LOG_DEBUG("skip no-op impact damage relay event=%u target=%u damage=%f",
                  event.event_id, event.target_entity_id, event.damage);
        return true;
    }
    if (!g_state.session || !g_state.session->running() || !g_state.is_host) {
        LOG_ERROR("impact damage relay unavailable event=%u", event.event_id);
        return false;
    }
    std::array<Byte, kImpactDamageWireSize> payload{};
    const ImpactDamageCodecError encoded = encode_impact_damage(event, payload);
    if (!impact_damage_codec_succeeded(encoded)) {
        LOG_ERROR("impact damage encode failed event=%u code=%u",
                  event.event_id, static_cast<unsigned>(encoded));
        return false;
    }
    bool sent = true;
    for (const PeerId peer : g_state.peers) {
        const TransportResult result = g_state.session->send(
            peer, MessageType::ImpactDamage, Channel::Reliable, payload);
        if (!result) {
            sent = false;
            LOG_ERROR("impact damage send failed event=%u peer=%u code=%u",
                      event.event_id, peer, static_cast<unsigned>(result.code));
        }
    }
    return sent;
}

void receive_weapon_command(const SessionEvent& event)
{
    if (!g_state.is_host) {
        WeaponCommand command{};
        const WeaponCommandCodecError decoded =
            decode_weapon_command(event.payload, command);
        if (!weapon_command_codec_succeeded(decoded)) {
            LOG_ERROR("peer=%u bad weapon command code=%u", event.peer,
                      static_cast<unsigned>(decoded));
            return;
        }
        if (command.entity_id == g_state.local_entity_id) {
            hta::ai::Player* const player = hta::ai::Player::Instance();
            hta::ai::Vehicle* const vehicle =
                player != nullptr ? player->GetVehicle() : nullptr;
            if (command.has_ammo_state) {
                if (vehicle != nullptr)
                    apply_network_weapon_ammo(*vehicle, command);
                g_state.presented_local_shot_id = command.shot_id;
                return;
            }
            g_state.local_weapon_state = command;
            g_state.has_local_weapon_state = true;
            return;
        }
        RemoteEntity* const remote = find_or_add_remote(command.entity_id);
        if (remote == nullptr) {
            LOG_ERROR("too many remote entities; drop weapon entity=%u",
                      command.entity_id);
            return;
        }
        if (remote->has_spawn && remote->kind == EntityKind::NpcVehicle)
            return;
        if (command.has_ammo_state) {
            if (remote->presented_shot_id == command.shot_id)
                return;
            hta::ai::Vehicle* const ghost = find_vehicle(command.entity_id);
            if (ghost != nullptr)
                apply_network_weapon_ammo(*ghost, command);
            remote->presented_shot_id = command.shot_id;
            return;
        }
        if (!remote->has_weapon ||
            sequence_is_newer(command.sequence, remote->weapon.sequence)) {
            remote->weapon = command;
            remote->has_weapon = true;
        }
        return;
    }
    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr)
        return;
    WeaponCommand command{};
    const WeaponCommandCodecError decoded =
        decode_weapon_command(event.payload, command);
    if (!weapon_command_codec_succeeded(decoded) ||
        command.entity_id != controller->entity_id) {
        LOG_ERROR("drop weapon peer=%u code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (controller->has_weapon &&
        !sequence_is_newer(command.sequence, controller->last_weapon_sequence))
        return;
    controller->weapon = command;
    controller->last_weapon_sequence = command.sequence;
    controller->has_weapon = true;
    // The client owns the input boundary for this scenario, while the host
    // owns the weapon simulation.  Record the latter only after accepting the
    // actual reliable trigger command, so the integration harness can prove
    // that authority—not merely the client presentation—has begun combat.
    if (g_state.combat_autotest_scenario == "client-kills-host" &&
        !g_state.combat_autotest_started && command.trigger_held &&
        command.target_entity_id == 1) {
        g_state.combat_autotest_started = true;
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST start scenario=%s shooter=%u target=%u",
                 g_state.combat_autotest_scenario.c_str(), command.entity_id,
                 command.target_entity_id);
    }
    // This is durable trigger state, not a one-frame fire event.  The host
    // holds the original Gun::Fire path until a later reliable release.
}

void receive_loadout(const SessionEvent& event)
{
    LoadoutProfile profile{};
    const LoadoutCodecError decoded = decode_loadout(event.payload, profile);
    if (!loadout_codec_succeeded(decoded)) {
        LOG_ERROR("drop loadout peer=%u code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (!g_state.is_host) {
        if (profile.entity_id == g_state.local_entity_id)
            return;
        RemoteEntity* const remote = find_or_add_remote(profile.entity_id);
        if (remote == nullptr)
            return;
        if (remote->retired && profile.generation != kInvalidEntityGeneration &&
            profile.generation <= remote->retired_generation)
            return;
        if (profile.generation != kInvalidEntityGeneration &&
            remote->has_spawn && profile.generation != remote->generation) {
            LOG_ERROR("drop stale loadout entity=%u generation=%u current=%u",
                      profile.entity_id, static_cast<unsigned>(profile.generation),
                      static_cast<unsigned>(remote->generation));
            return;
        }
        if (remote->has_loadout && !sequence_is_newer(
                profile.revision, remote->loadout.revision))
            return;
        remote->loadout = std::move(profile);
        remote->has_loadout = true;
        return;
    }
    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr || profile.entity_id != controller->entity_id) {
        LOG_ERROR("drop loadout peer=%u entity mismatch", event.peer);
        return;
    }
    EntityGeneration controller_generation = controller->generation;
    (void)g_state.entities.lookup_generation(controller->entity_id,
                                              controller_generation);
    if (profile.generation != kInvalidEntityGeneration &&
        profile.generation != controller_generation) {
        LOG_ERROR("drop loadout peer=%u entity=%u generation=%u current=%u",
                  event.peer, profile.entity_id,
                  static_cast<unsigned>(profile.generation),
                  static_cast<unsigned>(controller_generation));
        return;
    }
    if (profile.generation == kInvalidEntityGeneration)
        profile.generation = controller_generation;
    if (controller->has_loadout &&
        !sequence_is_newer(profile.revision, controller->loadout.revision))
        return;
    controller->loadout = std::move(profile);
    controller->has_loadout = true;
    std::vector<Byte> relay_payload;
    if (loadout_codec_succeeded(encode_loadout(controller->loadout,
                                                relay_payload))) {
        for (const PeerId peer : g_state.peers)
            if (peer != event.peer)
                (void)g_state.session->send(peer, MessageType::Loadout,
                                            Channel::Reliable, relay_payload);
    }
    LOG_INFO("received loadout entity=%u revision=%u basePrototype=%d parts=%u",
             controller->entity_id, controller->loadout.revision,
             controller->loadout.base_prototype_id,
             static_cast<unsigned>(controller->loadout.parts.size()));
}

void receive_loot_request(const SessionEvent& event)
{
    // The legacy request has no session epoch or loot generation.  Reject it
    // instead of silently weakening stale-packet protection; new clients use
    // WorldLootPickupRequest below.
    (void)event;
    LOG_WARNING("legacy LootRequest rejected: world loot requires epoch and generation");
}

void receive_loot_result(const SessionEvent& event)
{
    (void)event;
    LOG_WARNING("legacy LootResult ignored: world loot uses WorldLootPickupResult");
}

void receive_world_loot_pickup_request(const SessionEvent& event)
{
    if (!g_state.is_host)
        return;
    WorldLootPickupRequest request{};
    const WorldLootCodecError decoded = decode_world_loot_pickup_request(
        event.payload, request);
    if (!world_loot_codec_succeeded(decoded)) {
        LOG_ERROR("drop world loot pickup peer=%u decode=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }

    const auto prior = std::find_if(
        g_state.loot_receipts.begin(), g_state.loot_receipts.end(),
        [&event, &request](const WorldLootReceipt& receipt) {
            return receipt.peer == event.peer &&
                   receipt.request.transaction_id == request.transaction_id;
        });
    if (prior != g_state.loot_receipts.end()) {
        if (!same_world_loot_request(prior->request, request)) {
            LOG_ERROR("reject world loot txn conflict peer=%u txn=%u canonical_loot=%u incoming_loot=%u",
                      event.peer, request.transaction_id,
                      prior->request.loot_id, request.loot_id);
            return;
        }
        // A retry may need the response again, but it never repeats an
        // inventory mutation or a world-state broadcast.
        LOG_INFO("replay world loot receipt peer=%u txn=%u loot=%u",
                 event.peer, request.transaction_id, request.loot_id);
        send_world_loot_pickup_result(event.peer, prior->result);
        return;
    }

    WorldLootPickupResult result{};
    result.session_epoch = g_state.session_epoch;
    result.loot_id = request.loot_id;
    result.generation = request.generation;
    result.transaction_id = request.transaction_id;
    result.code = WorldLootPickupCode::InvalidRequest;
    result.revision = g_state.world_loot_revision;

    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr || request.entity_id != controller->entity_id) {
        LOG_ERROR("drop world loot pickup peer=%u entity=%u", event.peer,
                  request.entity_id);
    } else if (request.session_epoch != g_state.session_epoch) {
        result.code = WorldLootPickupCode::StaleSession;
    } else {
        LootRecord* const loot = find_loot(request.loot_id);
        if (loot == nullptr) {
            result.code = WorldLootPickupCode::NotFound;
        } else {
            result.item_prototype_id = loot->world.item_prototype_id;
            result.item_instance_id = loot->world.item_instance_id;
            result.remaining_amount = loot->world.amount;
            result.revision = loot->world.revision;
            if (request.generation != loot->world.generation) {
                result.code = WorldLootPickupCode::StaleGeneration;
            } else if (loot->world.owner_entity_id != 0 &&
                       loot->world.owner_entity_id != request.entity_id) {
                result.code = WorldLootPickupCode::NotOwner;
            } else if (loot->world.amount == 0) {
                result.code = WorldLootPickupCode::Exhausted;
            } else {
                hta::ai::Vehicle* const vehicle =
                    find_vehicle(controller->entity_id);
                hta::ai::CServer* const server = hta::ai::CServer::Instance();
                hta::ai::Obj* const source_object =
                    server && server->m_pObjects
                    ? server->m_pObjects->GetEntityByObjId(loot->chest_obj_id)
                    : nullptr;
                LootSourceBinding* const source = loot->object_backed
                    ? find_loot_source(loot->chest_obj_id)
                    : nullptr;
                const bool source_deleted = source_object != nullptr &&
                    source_object->GetDeletedStatus();
                const bool source_identity_valid =
                    !loot->object_backed ||
                    (source != nullptr && !source->lifecycle_tombstoned &&
                     source->object == source_object &&
                     source->generation == loot->source_generation &&
                     source_object != nullptr &&
                     source_object->GetPrototypeId() == source->prototype_id);
                if (source_deleted) {
                    if (loot->object_backed)
                        tombstone_loot_source(loot->chest_obj_id,
                                              "deleted before pickup");
                    LOG_ERROR("reject world loot pickup deleted source loot=%u objId=%d generation=%u",
                              loot->world.loot_id, loot->chest_obj_id,
                              static_cast<unsigned>(loot->world.generation));
                    result.code = WorldLootPickupCode::NotFound;
                } else if (!source_identity_valid) {
                    if (loot->object_backed)
                        tombstone_loot_source(loot->chest_obj_id,
                                              "missing or reused identity");
                    LOG_ERROR("reject world loot pickup stale source identity loot=%u objId=%d generation=%u source=%p resolved=%p",
                              loot->world.loot_id, loot->chest_obj_id,
                              static_cast<unsigned>(loot->world.generation),
                              static_cast<void*>(source),
                              static_cast<void*>(source_object));
                    result.code = WorldLootPickupCode::StaleGeneration;
                } else {
                    hta::ai::Chest* const chest =
                        chest_from_object(source_object);
                    if (vehicle == nullptr || chest == nullptr ||
                        vehicle->m_repository == nullptr ||
                        chest->GetRepository() == nullptr) {
                        LOG_ERROR("reject world loot pickup unavailable source loot=%u objId=%d",
                                  loot->world.loot_id, loot->chest_obj_id);
                        result.code = WorldLootPickupCode::NotFound;
                    } else {
                        const hta::CVector a = vehicle->GetPosition();
                        const hta::CVector b = chest->GetPosition();
                        const float dx = a.x - b.x;
                        const float dy = a.y - b.y;
                        const float dz = a.z - b.z;
                        if (dx * dx + dy * dy + dz * dz > 144.0f) {
                            result.code = WorldLootPickupCode::TooFar;
                        } else {
                            hta::ai::GeomRepository* const source_repository =
                                chest->GetRepository();
                            hta::ai::GeomRepository* const destination_repository =
                                vehicle->m_repository;
                            const std::uint32_t granted = (std::min)(
                                request.amount, loot->world.amount);
                            const int32_t transfer_amount =
                                static_cast<int32_t>(granted);
                            if (!destination_repository->CanPlaceItems(
                                    loot->world.item_prototype_id,
                                    transfer_amount)) {
                                result.code = WorldLootPickupCode::InventoryFull;
                            } else if (source_repository->GetAmountByPrototypeId(
                                           loot->world.item_prototype_id) <
                                       static_cast<std::uint32_t>(transfer_amount)) {
                                LOG_ERROR("world loot source prevalidation failed loot=%u amount=%u",
                                          loot->world.loot_id, granted);
                                result.code = WorldLootPickupCode::NotFound;
                            } else {
                                const int32_t removed =
                                    source_repository->GiveUpThingByPrototypeId(
                                        loot->world.item_prototype_id,
                                        transfer_amount);
                                if (removed != transfer_amount) {
                                    const bool restored = removed <= 0 ||
                                        source_repository->AddItems(
                                            loot->world.item_prototype_id, removed);
                                    LOG_ERROR("world loot source removal mismatch loot=%u expected=%d removed=%d restored=%u",
                                              loot->world.loot_id, transfer_amount,
                                              removed, restored ? 1u : 0u);
                                    if (!restored)
                                        LOG_PANIC("world loot source rollback failed loot=%u amount=%d",
                                                  loot->world.loot_id, removed);
                                    result.code = WorldLootPickupCode::NotFound;
                                } else if (!destination_repository->AddItems(
                                               loot->world.item_prototype_id,
                                               transfer_amount)) {
                                    const bool restored = source_repository->AddItems(
                                        loot->world.item_prototype_id,
                                        transfer_amount);
                                    LOG_ERROR("world loot destination grant failed loot=%u amount=%u rollback=%u",
                                              loot->world.loot_id, granted,
                                              restored ? 1u : 0u);
                                    if (!restored)
                                        LOG_PANIC("world loot destination rollback failed loot=%u amount=%u",
                                                  loot->world.loot_id, granted);
                                    result.code = WorldLootPickupCode::InventoryFull;
                                } else {
                                    loot->world.amount -= granted;
                                    loot->world.revision =
                                        ++g_state.world_loot_revision;
                                    result.granted_amount = granted;
                                    result.remaining_amount = loot->world.amount;
                                    result.revision = loot->world.revision;
                                    result.code = WorldLootPickupCode::Granted;
                                    const WorldLootDelta delta{
                                        g_state.session_epoch, loot->world.loot_id,
                                        loot->world.generation,
                                        loot->world.revision, loot->world.amount};
                                    (void)g_state.world_loot.apply_delta(delta);
                                    for (const PeerId peer : g_state.peers)
                                        send_world_loot_delta(peer, delta);
                                    if (loot->world.amount == 0) {
                                        const WorldLootRemove remove{
                                            g_state.session_epoch,
                                            loot->world.loot_id,
                                            loot->world.generation,
                                            ++g_state.world_loot_revision, 1};
                                        (void)g_state.world_loot.apply_remove(remove);
                                        for (const PeerId peer : g_state.peers)
                                            send_world_loot_remove(peer, remove);
                                        const WorldLootId removed_id =
                                            loot->world.loot_id;
                                        std::erase_if(
                                            g_state.loot_records,
                                            [removed_id](
                                                const LootRecord& candidate) {
                                                return candidate.world.loot_id ==
                                                           removed_id &&
                                                       !candidate.object_backed;
                                            });
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    remember_world_loot_receipt(event.peer, request, result);
    send_world_loot_pickup_result(event.peer, result);
    LOG_INFO("world loot pickup peer=%u txn=%u loot=%u generation=%u code=%u granted=%u remaining=%u revision=%u",
             event.peer, result.transaction_id, result.loot_id,
             static_cast<unsigned>(result.generation),
             static_cast<unsigned>(result.code), result.granted_amount,
             result.remaining_amount, result.revision);
}

void receive_world_loot_spawn(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    WorldLootSpawn spawn{};
    const WorldLootCodecError decoded = decode_world_loot_spawn(
        event.payload, spawn);
    if (!world_loot_codec_succeeded(decoded)) {
        LOG_ERROR("drop world loot spawn peer=%u decode=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    const WorldLootApplyResult applied = g_state.world_loot.apply_spawn(spawn);
    if (applied == WorldLootApplyResult::Applied ||
        applied == WorldLootApplyResult::Duplicate)
        adopt_client_session_epoch(spawn.record.session_epoch);
    LOG_DEBUG("world loot spawn loot=%u generation=%u revision=%u apply=%u",
              spawn.record.loot_id, static_cast<unsigned>(spawn.record.generation),
              spawn.record.revision, static_cast<unsigned>(applied));
}

void receive_world_loot_baseline(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    WorldLootBaseline baseline{};
    const WorldLootCodecError decoded = decode_world_loot_baseline(
        event.payload, baseline);
    if (!world_loot_codec_succeeded(decoded)) {
        LOG_ERROR("drop world loot baseline peer=%u decode=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    const WorldLootApplyResult applied =
        g_state.world_loot.apply_baseline(baseline);
    if (applied == WorldLootApplyResult::Applied ||
        applied == WorldLootApplyResult::Duplicate)
        adopt_client_session_epoch(baseline.session_epoch);
    LOG_INFO("world loot baseline epoch=%u revision=%u records=%u apply=%u",
             baseline.session_epoch, baseline.revision,
             static_cast<unsigned>(baseline.records.size()),
             static_cast<unsigned>(applied));
}

void receive_world_loot_delta(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    WorldLootDelta delta{};
    const WorldLootCodecError decoded = decode_world_loot_delta(
        event.payload, delta);
    if (!world_loot_codec_succeeded(decoded)) {
        LOG_ERROR("drop world loot delta peer=%u decode=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    const WorldLootApplyResult applied = g_state.world_loot.apply_delta(delta);
    LOG_DEBUG("world loot delta loot=%u generation=%u amount=%u revision=%u apply=%u",
              delta.loot_id, static_cast<unsigned>(delta.generation),
              delta.amount, delta.revision, static_cast<unsigned>(applied));
}

void receive_world_loot_remove(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    WorldLootRemove remove{};
    const WorldLootCodecError decoded = decode_world_loot_remove(
        event.payload, remove);
    if (!world_loot_codec_succeeded(decoded)) {
        LOG_ERROR("drop world loot remove peer=%u decode=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    const WorldLootApplyResult applied = g_state.world_loot.apply_remove(remove);
    LOG_DEBUG("world loot remove loot=%u generation=%u revision=%u apply=%u",
              remove.loot_id, static_cast<unsigned>(remove.generation),
              remove.revision, static_cast<unsigned>(applied));
}

void receive_world_loot_pickup_result(const SessionEvent& event)
{
    if (g_state.is_host)
        return;
    WorldLootPickupResult result{};
    const WorldLootCodecError decoded = decode_world_loot_pickup_result(
        event.payload, result);
    if (!world_loot_codec_succeeded(decoded)) {
        LOG_ERROR("drop world loot pickup result peer=%u decode=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    // Presentation is reconciled exclusively by Delta/Remove.  In particular,
    // a client result cannot call AddItems or otherwise mutate its inventory.
    LOG_INFO("world loot result txn=%u loot=%u generation=%u code=%u granted=%u remaining=%u revision=%u",
             result.transaction_id, result.loot_id,
             static_cast<unsigned>(result.generation),
             static_cast<unsigned>(result.code), result.granted_amount,
             result.remaining_amount, result.revision);
}

void receive_remote_snapshot(const SessionEvent& event)
{
    if (g_state.is_host)
        return;

    VehicleSnapshot snapshot{};
    const VehicleSnapshotCodecError decoded =
        decode_vehicle_snapshot(event.payload, snapshot);
    if (!vehicle_snapshot_codec_succeeded(decoded)) {
        LOG_ERROR("peer=%u bad vehicle snapshot code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }

    if (snapshot.entity_id == g_state.local_entity_id) {
        (void)g_state.local_correction.push(now_ms(), snapshot);
        return;
    }

    // EFA's SpawnPlayer trigger rolls a local random spawn independently on
    // every PC.  The first authoritative host snapshot is the safe point
    // after that trigger finishes to co-locate a joining client's native
    // Player vehicle without replacing it with a map proxy.
    if (g_state.spawn_together && !g_state.local_shared_spawn_applied &&
        snapshot.entity_id == 1 && g_state.local_entity_id != kInvalidNetId) {
        const PlayerSlotIndex local_slot =
            player_slot_for_entity(g_state.local_entity_id);
        hta::ai::Player* const player = hta::ai::Player::Instance();
        hta::ai::Vehicle* const local = player ? player->GetVehicle() : nullptr;
        if (local != nullptr && local_slot != kInvalidPlayerSlot) {
            const hta::CVector position = shared_spawn_position(
                to_engine_vector(snapshot.position), local_slot);
            local->SetPositionSelf(position);
            local->SetRotationSelf(to_engine_quaternion(snapshot.rotation));
            local->SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
            local->SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
            g_state.local_shared_spawn_applied = true;
            LOG_INFO("MP shared spawn applied local entity=%u slot=%u pos=%.3f %.3f %.3f",
                     g_state.local_entity_id, static_cast<unsigned>(local_slot + 1),
                     position.x, position.y, position.z);
        }
    }

    RemoteEntity* const remote = find_or_add_remote(snapshot.entity_id);
    if (remote == nullptr) {
        LOG_ERROR("too many remote entities; drop entity=%u", snapshot.entity_id);
        return;
    }
    if (remote->has_sequence &&
        !sequence_is_newer(snapshot.sequence, remote->last_sequence))
        return;

    const SnapshotInterpolationStatus pushed =
        remote->snapshots.push(now_ms(), snapshot);
    if (!snapshot_interpolation_succeeded(pushed)) {
        LOG_ERROR("drop snapshot entity=%u interpolation code=%u",
                  snapshot.entity_id, static_cast<unsigned>(pushed));
        return;
    }
    remote->last_sequence = snapshot.sequence;
    remote->has_sequence = true;
}

hta::ai::Vehicle* ensure_host_vehicle(PeerController& controller);

void handle_event(SessionEvent&& event)
{
    switch (event.type) {
    case SessionEventType::PeerConnected: {
        const bool new_peer =
            std::find(g_state.peers.begin(), g_state.peers.end(), event.peer) ==
            g_state.peers.end();
        if (new_peer)
            g_state.peers.push_back(event.peer);
        if (!g_state.is_host && new_peer) {
            reset_impact_damage_state();
            g_state.session_epoch = 0;
        }
        LOG_INFO("peer=%u handshake complete", event.peer);
        g_state.reconnect_backoff = std::chrono::seconds(1);
        if (g_state.is_host) {
            const NetId entity = event.peer + 1;
            if (find_controller(event.peer) == nullptr)
                g_state.controllers.push_back(PeerController{event.peer, entity});
            send_entity_assignment(event.peer, entity);
            PeerController* const controller = find_controller(event.peer);
            if (controller == nullptr)
                LOG_ERROR("cannot track remote player peer=%u entity=%u",
                          event.peer, entity);
            else
                LOG_INFO("host awaiting client base Vehicle/loadout peer=%u entity=%u",
                         event.peer, entity);
            publish_host_baseline_to_peer(event.peer);
            send_world_loot_baseline(event.peer);
        }
        (void)g_state.session->ping(event.peer);
        break;
    }

    case SessionEventType::PeerDisconnected:
        std::erase(g_state.peers, event.peer);
        std::erase_if(g_state.spawn_publications,
                      [peer = event.peer](const SpawnPublication& publication) {
                          return publication.peer == peer;
                      });
        std::erase_if(g_state.loadout_publications,
                      [peer = event.peer](const LoadoutPublication& publication) {
                          return publication.peer == peer;
                      });
        LOG_INFO("peer=%u disconnected", event.peer);
        if (g_state.is_host) {
            const PeerController* const controller = find_controller(event.peer);
            if (controller != nullptr) {
                const NetId entity_id = controller->entity_id;
                EntityGeneration generation = kInvalidEntityGeneration;
                (void)g_state.entities.lookup_generation(entity_id, generation);
                std::erase_if(g_state.controllers,
                              [peer = event.peer](const PeerController& c) {
                                  return c.peer == peer;
                });
                schedule_entity_removal(entity_id);
                if (generation != kInvalidEntityGeneration)
                    broadcast_entity_despawn(entity_id, generation, event.peer);
            }
        }
        else {
            // A client has exactly one authoritative host.  Its loss invalidates
            // the network session.  Do not mutate ghosts or the Player slot
            // here: a host-death transition may already be unloading this map.
            reset_impact_damage_state();
            g_state.session_epoch = 0;
            g_state.world_loot.clear();
            // A client has one authority.  Do not leave an apparently active
            // session reconnecting after that authority has ended the raid.
            g_state.session_end_preserve_client_scene = true;
            g_state.host_defeat_session_end_pending = true;
        }
        break;

    case SessionEventType::RoundTripTime:
        LOG_INFO("peer=%u rtt=%u ms", event.peer, event.round_trip_time_ms);
        break;

    case SessionEventType::Message:
        if (event.message_type == MessageType::Snapshot)
            receive_remote_snapshot(event);
        else if (event.message_type == MessageType::EntitySpawn)
            receive_entity_spawn(event);
        else if (event.message_type == MessageType::EntityAssign)
            receive_entity_assignment(event);
        else if (event.message_type == MessageType::EntityDespawn)
            receive_entity_despawn(event);
        else if (event.message_type == MessageType::Loadout)
            receive_loadout(event);
        else if (event.message_type == MessageType::Input)
            receive_input(event);
        else if (event.message_type == MessageType::WeaponCommand)
            receive_weapon_command(event);
        else if (event.message_type == MessageType::LootRequest)
            receive_loot_request(event);
        else if (event.message_type == MessageType::LootResult)
            receive_loot_result(event);
        else if (event.message_type == MessageType::WorldLootSpawn)
            receive_world_loot_spawn(event);
        else if (event.message_type == MessageType::WorldLootBaseline)
            receive_world_loot_baseline(event);
        else if (event.message_type == MessageType::WorldLootDelta)
            receive_world_loot_delta(event);
        else if (event.message_type == MessageType::WorldLootRemove)
            receive_world_loot_remove(event);
        else if (event.message_type == MessageType::WorldLootPickupRequest)
            receive_world_loot_pickup_request(event);
        else if (event.message_type == MessageType::WorldLootPickupResult)
            receive_world_loot_pickup_result(event);
        else if (event.message_type == MessageType::ImpactDamage)
            receive_impact_damage(event);
        LOG_DEBUG("peer=%u message=%u bytes=%u channel=%u", event.peer,
                  static_cast<unsigned>(event.message_type),
                  static_cast<unsigned>(event.payload.size()),
                  static_cast<unsigned>(event.channel));
        break;

    case SessionEventType::ProtocolError:
        LOG_ERROR("peer=%u protocol error=%u", event.peer,
                  static_cast<unsigned>(event.protocol_error));
        break;
    }
}

void synchronize_local_weapon_trigger(hta::ai::Vehicle& vehicle)
{
    (void)vehicle;
    // Gun::Fire(bool) is hooked before the engine's CanFire validation. It
    // supplies the true press/release transition for every supported control
    // source, so no WinAPI polling or inferred state is needed here.
}

void send_client_input()
{
    if (g_state.is_host || g_state.local_entity_id == kInvalidNetId ||
        g_state.peers.empty())
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return;
    (void)bind_local_player_vehicle();
    // This runs once per game tick and observes physical LMB independently of
    // the weapon's target-validation and presentation state.
    synchronize_local_weapon_trigger(*vehicle);
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_input)
        return;
    g_state.next_input = now + std::chrono::milliseconds(50);
    InputCommand input{};
    input.entity_id = g_state.local_entity_id;
    input.sequence = g_state.next_input_sequence++;
    input.client_tick = g_state.server_tick;
    input.throttle = vehicle->m_throttle;
    input.steer = vehicle->m_steerRadians;
    input.brake = vehicle->m_brake;
    input.handbrake = vehicle->m_bHandBrake;
    input.request_unstuck = vehicle->m_bMustGetOutOfDifficultPlace;
    input.horn = vehicle->GetHorn();
    std::array<Byte, kInputCommandWireSize> payload{};
    if (encode_input_command(input, payload) == InputCommandCodecError::None)
        (void)g_state.session->send(g_state.peers.front(), MessageType::Input,
                                    Channel::Unreliable, payload);
}

std::uint32_t loadout_revision(const LoadoutProfile& profile)
{
    // FNV-1a provides a stable revision across processes without using an
    // implementation-defined std::hash.  Zero is reserved by the protocol.
    std::uint32_t value = 2166136261u;
    const auto mix_byte = [&value](const unsigned char character) {
        value ^= character;
        value *= 16777619u;
    };
    const auto mix = [&value](const std::string& text) {
        for (const unsigned char character : text) {
            value ^= character;
            value *= 16777619u;
        }
        value ^= 0xffu;
        value *= 16777619u;
    };
    if (profile.base_prototype_id >= 0) {
        const std::uint32_t prototype =
            static_cast<std::uint32_t>(profile.base_prototype_id);
        for (int index = 0; index != 4; ++index)
            mix_byte(static_cast<unsigned char>(prototype >> (index * 8)));
        mix_byte(0xfeu);
    }
    for (const LoadoutPart& part : profile.parts) {
        mix(part.slot);
        mix(part.prototype);
    }
    return value == 0 ? 1 : value;
}

bool capture_local_loadout(LoadoutProfile& profile)
{
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr || g_state.local_entity_id == kInvalidNetId)
        return false;
    profile = {};
    profile.entity_id = g_state.local_entity_id;
    profile.base_prototype_id = vehicle->GetPrototypeId();
    if (profile.base_prototype_id < 0)
        return false;
    (void)g_state.entities.lookup_generation(profile.entity_id,
                                              profile.generation);
    if (profile.generation == kInvalidEntityGeneration)
        profile.generation = kInitialEntityGeneration;
    const auto names = vehicle->GetAttachedPartNames();
    if (names.size() > kMaxLoadoutParts)
        return false;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const hta::CStr& slot = names[index];
        const hta::ai::VehiclePart* const part = vehicle->GetPartByName(slot);
        const hta::ai::VehiclePartPrototypeInfo* const prototype =
            part ? part->GetPrototypeInfo() : nullptr;
        if (prototype == nullptr || slot.c_str() == nullptr ||
            prototype->m_prototypeName.c_str() == nullptr)
            continue;
        profile.parts.push_back({slot.c_str(), prototype->m_prototypeName.c_str()});
    }
    profile.revision = loadout_revision(profile);
    return profile.revision != 0;
}

void send_client_loadout()
{
    if (g_state.is_host || g_state.local_entity_id == kInvalidNetId ||
        g_state.peers.empty())
        return;
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_loadout)
        return;
    g_state.next_loadout = now + std::chrono::seconds(1);
    LoadoutProfile profile{};
    if (!capture_local_loadout(profile) ||
        profile.revision == g_state.last_local_loadout_revision)
        return;
    std::vector<Byte> payload;
    if (!loadout_codec_succeeded(encode_loadout(profile, payload)))
        return;
    const TransportResult result = g_state.session->send(
        g_state.peers.front(), MessageType::Loadout, Channel::Reliable, payload);
    if (!result) {
        LOG_ERROR("loadout send failed entity=%u code=%u", profile.entity_id,
                  static_cast<unsigned>(result.code));
        return;
    }
    g_state.last_local_loadout_revision = profile.revision;
    LOG_INFO("sent loadout entity=%u revision=%u basePrototype=%d parts=%u",
             profile.entity_id, profile.revision, profile.base_prototype_id,
             static_cast<unsigned>(profile.parts.size()));
}

hta::ai::Vehicle* find_vehicle(const NetId entity_id,
                               const EntityGeneration expected_generation)
{
    ObjId object_id = kInvalidObjId;
    EntityGeneration generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_obj_id(entity_id, object_id, generation) ||
        (expected_generation != kInvalidEntityGeneration &&
         expected_generation != generation))
        return nullptr;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Obj* const object = server && server->m_pObjects
        ? server->m_pObjects->GetEntityByObjId(object_id) : nullptr;
    return vehicle_from_object(object);
}

bool impact_entity_ref_is_current(NetId entity_id,
                                  EntityGeneration generation)
{
    ObjId object_id = kInvalidObjId;
    EntityGeneration current_generation = kInvalidEntityGeneration;
    return g_state.entities.lookup_obj_id(entity_id, object_id,
                                           current_generation) &&
           current_generation == generation;
}

bool present_authoritative_impact_damage(hta::ai::Vehicle& target,
                                         const ImpactDamage& event)
{
    if (impact_damage_is_noop(event))
        return false;

    const VehicleInflictDamageFn original =
        g_state.vehicle_inflict_damage_original;
    if (original == nullptr)
        return false;

    ObjId attacker_obj_id = -1;
    if (event.attacker_entity_id != kInvalidNetId) {
        EntityGeneration attacker_generation = kInvalidEntityGeneration;
        if (!g_state.entities.lookup_obj_id(event.attacker_entity_id,
                                            attacker_obj_id,
                                            attacker_generation) ||
            attacker_generation != event.attacker_generation)
            return false;
    }

    // The packet is the host's authoritative result. Re-enter the engine's
    // native damage path only for its hit/death effects, then overwrite health
    // with post_health below. This is presentation of confirmed damage, not a
    // client-side weapon or collision simulation.
    hta::ai::DamageInfo info{};
    info.attackerId = attacker_obj_id;
    info.attackingAgentId = attacker_obj_id;
    info.bDamageFriends = true;
    info.gunPrototypeId = event.gun_id;
    info.damage = event.damage;
    info.damageType = static_cast<hta::ai::DamageType>(event.damage_type);
    info.damagedPartName = hta::CStr(event.damaged_part.c_str());
    info.hitPos = {event.hit_position.x, event.hit_position.y,
                   event.hit_position.z};
    info.hitDir = {event.direction.x, event.direction.y, event.direction.z};
    info.normal = {event.normal.x, event.normal.y, event.normal.z};
    info.decalId = -1;

    g_presenting_authoritative_impact = true;
    original(&target, info);
    g_presenting_authoritative_impact = false;
    return true;
}

void diagnose_impact_health_replay(const ImpactDamage& event,
                                   const float replayed_health)
{
    constexpr float kHealthDiagnosticTolerance = 0.01f;
    constexpr std::uint32_t kMaxHealthMismatchDiagnostics = 16;
    const bool expected_finite = std::isfinite(event.post_health);
    const bool replayed_finite = std::isfinite(replayed_health);
    const float delta = expected_finite && replayed_finite
        ? std::fabs(replayed_health - event.post_health) : 0.0f;
    if ((expected_finite && replayed_finite &&
         delta <= kHealthDiagnosticTolerance) ||
        g_state.impact_health_mismatch_diagnostics >=
            kMaxHealthMismatchDiagnostics)
        return;

    ++g_state.impact_health_mismatch_diagnostics;
    const float bounded_expected = expected_finite
        ? (std::clamp)(event.post_health, 0.0f, kMaxImpactDamageHealth)
        : -1.0f;
    const float bounded_replayed = replayed_finite
        ? (std::clamp)(replayed_health, 0.0f, kMaxImpactDamageHealth)
        : -1.0f;
    const float bounded_delta = expected_finite && replayed_finite
        ? (std::clamp)(delta, 0.0f, kMaxImpactDamageHealth) : -1.0f;
    LOG_WARNING("impact damage native replay health differs event=%u target=%u expected=%f native=%f delta=%f diagnostic=%u/%u",
                event.event_id, event.target_entity_id,
                bounded_expected, bounded_replayed, bounded_delta,
                g_state.impact_health_mismatch_diagnostics,
                kMaxHealthMismatchDiagnostics);
}

bool apply_impact_damage_result(const ImpactDamage& event)
{
    if (g_state.is_host)
        return false;
    if (impact_damage_is_noop(event)) {
        LOG_DEBUG("skip no-op impact damage presentation event=%u target=%u damage=%f",
                  event.event_id, event.target_entity_id, event.damage);
        return true;
    }
    const bool environment_damage =
        event.attacker_entity_id == kInvalidNetId;
    if ((!environment_damage &&
         !impact_entity_ref_is_current(event.attacker_entity_id,
                                       event.attacker_generation)) ||
        !impact_entity_ref_is_current(event.target_entity_id,
                                      event.target_generation))
        return false;

    hta::ai::Vehicle* const target = find_vehicle(
        event.target_entity_id, event.target_generation);
    if (target == nullptr)
        return false;
    // Reliable transport preserves delivery but a previous build could emit a
    // fresh dead event for every post-destruction collision callback.  Native
    // death evaluation is not idempotent (it creates physics/effect state),
    // so accept such a packet as consumed without replaying it on an already
    // dead replica.
    if (event.target_dead && target->_GetDeadStatus()) {
        LOG_DEBUG("skip duplicate dead impact event=%u target=%u",
                  event.event_id, event.target_entity_id);
        return true;
    }
    const float maximum = target->GetMaxHealth();
    if (!std::isfinite(maximum) || maximum <= 0.0f ||
        event.post_health > maximum) {
        LOG_ERROR("impact damage health reconciliation rejected event=%u target=%u post=%f max=%f",
                  event.event_id, event.target_entity_id,
                  event.post_health, maximum);
        return true; // Do not spin forever on a structurally valid mismatch.
    }
    if (!present_authoritative_impact_damage(*target, event))
        LOG_WARNING("impact damage presentation unavailable event=%u target=%u",
                    event.event_id, event.target_entity_id);
    // The host's post_health is diagnostic evidence only. Never overwrite a
    // replica's native state: confirmed presentation must be produced by
    // Vehicle::InflictDamage, and the engine owns the resulting health.
    diagnose_impact_health_replay(event, target->GetHealth());
    if (event.target_dead) {
        // The replica is normally on ObjContainer's not-update list. Put it
        // back on the native update list before evaluating death so the
        // engine owns the dead/explosion transition rather than packet code
        // merely making the object disappear.
        hta::ai::CServer* const server = hta::ai::CServer::Instance();
        if (server != nullptr && server->m_pObjects != nullptr) {
            hta::ai::Obj* const object =
                server->m_pObjects->GetEntityByObjId(target->GetId());
            if (object == target)
                server->m_pObjects->AddObjToUpdate(object);
        }
        // InflictDamage is the hit/decal path; the engine's explicit dead
        // evaluation is what transitions a vehicle into its destroyed form.
        // _EvaluateToDead runs WeaponFirer::FireFromWeaponsIfPossible before
        // marking the vehicle dead.  Scope its nested weapon activity as
        // authoritative death presentation so the input hooks cannot read or
        // relay target state from a vehicle whose parts are being dismantled.
        const bool was_presenting_death = g_presenting_authoritative_death;
        g_presenting_authoritative_death = true;
        target->_EvaluateToDead();
        g_presenting_authoritative_death = was_presenting_death;
        // Some vehicle prototypes expose a dead result without a zero health
        // value. Keep the authoritative dead bit as a guarded fallback after
        // the native evaluation has had first opportunity to run its actions.
        if (!target->_GetDeadStatus())
            target->_SetDeadStatus();
    }
    LOG_INFO("impact damage applied event=%u attacker=%u target=%u damage=%f health=%f dead=%u",
             event.event_id, event.attacker_entity_id, event.target_entity_id,
             event.damage, event.post_health, event.target_dead ? 1u : 0u);
    return true;
}

void apply_pending_impact_damage()
{
    if (g_state.is_host || g_state.pending_impact_damage.empty())
        return;
    for (std::size_t index = 0; index < g_state.pending_impact_damage.size();) {
        if (!apply_impact_damage_result(g_state.pending_impact_damage[index])) {
            ++index;
            continue;
        }
        g_state.pending_impact_damage.erase(
            g_state.pending_impact_damage.begin() +
            static_cast<std::ptrdiff_t>(index));
    }
}

bool queue_impact_fx(const ImpactDamage& event)
{
    if (impact_damage_is_noop(event))
        return false;
    if (g_state.pending_impact_fx.size() >= kMaxPendingImpactFx) {
        // The Lua consumer is optional and may be paused while the engine is
        // loading a map. Retain the newest authoritative presentation event;
        // rejecting it can discard the death impact.
        g_state.pending_impact_fx.erase(g_state.pending_impact_fx.begin());
    }
    try {
        g_state.pending_impact_fx.push_back(event);
        return true;
    }
    catch (...) {
        LOG_ERROR("impact FX queue allocation failed event=%u target=%u",
                  event.event_id, event.target_entity_id);
        return false;
    }
}

void receive_impact_damage(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable) {
        LOG_ERROR("drop impact damage from invalid role/channel peer=%u",
                  event.peer);
        return;
    }
    ImpactDamage damage{};
    const ImpactDamageCodecError decoded = decode_impact_damage(
        event.payload, damage);
    if (!impact_damage_codec_succeeded(decoded)) {
        LOG_ERROR("peer=%u invalid impact damage code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (!g_state.impact_damage_deduplicator.accept(damage.event_id)) {
        LOG_DEBUG("duplicate impact damage ignored event=%u", damage.event_id);
        return;
    }
    if (impact_damage_is_noop(damage)) {
        LOG_DEBUG("skip no-op impact damage event=%u target=%u damage=%f",
                  damage.event_id, damage.target_entity_id, damage.damage);
        return;
    }
    (void)queue_impact_fx(damage);
    if (apply_impact_damage_result(damage))
        return;
    if (g_state.pending_impact_damage.size() >= 256)
        g_state.pending_impact_damage.erase(g_state.pending_impact_damage.begin());
    g_state.pending_impact_damage.push_back(std::move(damage));
    LOG_DEBUG("queued impact damage awaiting entity refs event=%u target=%u",
              g_state.pending_impact_damage.back().event_id,
              g_state.pending_impact_damage.back().target_entity_id);
}

bool bind_local_player_vehicle()
{
    if (g_state.is_host)
        return bind_host_player_vehicle();
    if (g_state.local_entity_id == kInvalidNetId)
        return false;
    const PlayerSlotIndex index = player_slot_for_entity(g_state.local_entity_id);
    if (index == kInvalidPlayerSlot) {
        LOG_ERROR("local entity=%u has no map-owned player slot",
                  g_state.local_entity_id);
        return false;
    }
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr) {
        LOG_ERROR("cannot bind local client player: native Player vehicle is unavailable");
        return false;
    }

    const auto existing = g_state.player_slots.current(index);
    if (existing && g_state.player_slots.is_bound(index)) {
        ObjId bound_object_id = kInvalidObjId;
        EntityGeneration bound_generation = kInvalidEntityGeneration;
        if (existing->owner_entity_id == g_state.local_entity_id &&
            g_state.entities.lookup_obj_id(g_state.local_entity_id,
                                           bound_object_id, bound_generation) &&
            bound_object_id == vehicle->GetId() &&
            bound_generation == existing->generation) {
            g_state.local_player_vehicle_obj_id = vehicle->GetId();
            return true;
        }
        LOG_ERROR("local client player slot has inconsistent native vehicle binding");
        return false;
    }

    const auto lease = g_state.player_slots.reserve(index, g_state.local_entity_id);
    if (!lease) {
        LOG_ERROR("cannot reserve deterministic local client player slot entity=%u",
                  g_state.local_entity_id);
        return false;
    }
    const EntityRegistryBindResult bound = g_state.entities.bind(
        g_state.local_entity_id, vehicle->GetId(), lease->generation);
    if (bound != EntityRegistryBindResult::Inserted &&
        bound != EntityRegistryBindResult::AlreadyBound) {
        (void)g_state.player_slots.cancel(*lease, g_state.local_entity_id);
        LOG_ERROR("cannot bind local client native vehicle entity=%u objId=%d code=%u",
                  g_state.local_entity_id, vehicle->GetId(),
                  static_cast<unsigned>(bound));
        return false;
    }
    if (!g_state.player_slots.bind(*lease, g_state.local_entity_id)) {
        if (bound == EntityRegistryBindResult::Inserted)
            (void)g_state.entities.unbind_net_id(g_state.local_entity_id);
        (void)g_state.player_slots.cancel(*lease, g_state.local_entity_id);
        LOG_ERROR("cannot finalize deterministic local client player slot entity=%u",
                  g_state.local_entity_id);
        return false;
    }
    g_state.local_player_vehicle_obj_id = vehicle->GetId();
    LOG_INFO("MP client retained native vehicle entity=%u generation=%u objId=%d; proxy slot stays dormant",
             g_state.local_entity_id, static_cast<unsigned>(lease->generation),
             vehicle->GetId());
    return true;
}

bool bind_host_player_vehicle()
{
    if (!g_state.is_host)
        return false;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player != nullptr ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr) {
        LOG_ERROR("cannot bind host player: native Player vehicle is unavailable");
        return false;
    }

    constexpr PlayerSlotIndex kHostSlot = 0;
    const auto existing = g_state.player_slots.current(kHostSlot);
    if (existing && g_state.player_slots.is_bound(kHostSlot)) {
        ObjId bound_object_id = kInvalidObjId;
        EntityGeneration bound_generation = kInvalidEntityGeneration;
        if (existing->owner_entity_id == 1 &&
            g_state.entities.lookup_obj_id(1, bound_object_id, bound_generation) &&
            bound_object_id == vehicle->GetId() &&
            bound_generation == existing->generation) {
            g_state.host_vehicle_obj_id = vehicle->GetId();
            return true;
        }
        LOG_ERROR("host player slot has inconsistent native vehicle binding");
        return false;
    }

    const auto lease = g_state.player_slots.reserve(kHostSlot, 1);
    if (!lease) {
        LOG_ERROR("cannot reserve deterministic host player slot");
        return false;
    }
    const EntityRegistryBindResult bound = g_state.entities.bind(
        1, vehicle->GetId(), lease->generation);
    if (bound != EntityRegistryBindResult::Inserted &&
        bound != EntityRegistryBindResult::AlreadyBound) {
        (void)g_state.player_slots.cancel(*lease, 1);
        LOG_ERROR("cannot bind host native vehicle objId=%d code=%u", vehicle->GetId(),
                  static_cast<unsigned>(bound));
        return false;
    }
    if (!g_state.player_slots.bind(*lease, 1)) {
        if (bound == EntityRegistryBindResult::Inserted)
            (void)g_state.entities.unbind_net_id(1);
        (void)g_state.player_slots.cancel(*lease, 1);
        LOG_ERROR("cannot finalize deterministic host player slot");
        return false;
    }
    g_state.host_vehicle_obj_id = vehicle->GetId();
    LOG_INFO("MP host retained native vehicle entity=1 generation=%u objId=%d; proxy slot stays dormant",
             static_cast<unsigned>(lease->generation), vehicle->GetId());
    return true;
}

float health_fraction(const hta::ai::Vehicle& vehicle)
{
    const float maximum = vehicle.GetMaxHealth();
    if (maximum <= 0.001f)
        return 1.0f;
    return (std::clamp)(vehicle.GetHealth() / maximum, 0.0f, 1.0f);
}

void apply_health_fraction(hta::ai::Vehicle& vehicle, float fraction)
{
    const float maximum = vehicle.GetMaxHealth();
    if (maximum > 0.001f)
        vehicle.Health().m_value.set(maximum * fraction);
}

hta::ai::Vehicle* ensure_host_vehicle(PeerController& controller)
{
    if (!controller.host_vehicle_active ||
        controller.vehicle_obj_id == kInvalidObjId)
        return nullptr;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Obj* const object = server && server->m_pObjects
        ? server->m_pObjects->GetEntityByObjId(controller.vehicle_obj_id) : nullptr;
    hta::ai::Vehicle* vehicle = vehicle_from_object(object);
    if (vehicle == nullptr) {
        const ObjId expected_object_id = controller.vehicle_obj_id;
        controller.host_vehicle_active = false;
        LOG_ERROR("host dynamic player object disappeared entity=%u objId=%d",
                  controller.entity_id, controller.vehicle_obj_id);
        ObjId bound_object_id = kInvalidObjId;
        EntityGeneration bound_generation = kInvalidEntityGeneration;
        if (g_state.entities.lookup_obj_id(controller.entity_id,
                                           bound_object_id, bound_generation) &&
            bound_object_id == expected_object_id &&
            bound_generation == controller.generation)
            (void)g_state.entities.unbind_net_id(controller.entity_id);
        controller.vehicle_obj_id = kInvalidObjId;
        controller.applied_loadout_revision = 0;
        controller.deferred_loadout_revision = 0;
        controller.shared_spawn_applied = false;
        return nullptr;
    }
    return vehicle;
}

bool resolve_host_remote_spawn_transform(const PeerController& controller,
                                         hta::CVector& position,
                                         hta::Quaternion& rotation)
{
    const PlayerSlotIndex index = player_slot_for_entity(controller.entity_id);
    if (index == kInvalidPlayerSlot)
        return false;
    if (g_state.spawn_together) {
        hta::ai::Player* const player = hta::ai::Player::Instance();
        hta::ai::Vehicle* const host = player ? player->GetVehicle() : nullptr;
        if (host == nullptr)
            return false;
        position = shared_spawn_position(host->GetPosition(), index);
        rotation = host->GetRotation();
        return true;
    }
    return resolve_player_spawn_transform(index, position, rotation);
}

bool activate_host_remote_vehicle(PeerController& controller,
                                  hta::ai::Vehicle& vehicle)
{
    if (vehicle.GetNumWheels() == 0) {
        LOG_ERROR("host dynamic player activation rejected entity=%u objId=%d wheels=%u ode=%u",
                  controller.entity_id, vehicle.GetId(),
                  static_cast<unsigned>(vehicle.GetNumWheels()),
                  vehicle.bIsUpdatingByODE() ? 1u : 0u);
        return false;
    }
    hta::CVector position{};
    hta::Quaternion rotation{};
    if (!resolve_host_remote_spawn_transform(controller, position, rotation))
        return false;
    vehicle.SetPositionSelf(position);
    vehicle.SetRotationSelf(rotation);
    vehicle.SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    vehicle.SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    vehicle.SetThrottle(0.0f, false);
    vehicle.SetBrake(1.0f);
    vehicle.m_bHandBrake = true;
    vehicle.m_AI.m_pDM = nullptr;
    vehicle.SetNpcMotionControllerId(-1);
    vehicle.SetVisible();
    vehicle.EnableSounds(true);
    // A fully PostLoad'ed vehicle already owns valid ODE wheel and suspension
    // nodes.  Do not tear those down merely to make registration look like a
    // fresh spawn: Vehicle::_EvaluateToDead assumes they remain available.
    // A part-swapped vehicle is the only case that arrives inactive and needs
    // its physics graph reconstructed here.
    if (!vehicle.bIsUpdatingByODE()) {
        vehicle.EnablePhysics();
        vehicle.SetUpdatingByODE(true);
    }
    if (!vehicle.bIsUpdatingByODE())
        return false;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return false;
    server->m_pObjects->AddObjToUpdate(&vehicle);
    controller.shared_spawn_applied = g_state.spawn_together;
    controller.host_vehicle_active = true;
    LOG_INFO("created host dynamic player entity=%u generation=%u prototype=%d objId=%d sharedSpawn=%u",
             controller.entity_id, static_cast<unsigned>(controller.generation),
             vehicle.GetPrototypeId(), vehicle.GetId(),
             controller.shared_spawn_applied ? 1u : 0u);
    return true;
}

hta::ai::Vehicle* create_host_remote_vehicle(PeerController& controller)
{
    if (!controller.has_loadout || controller.loadout.base_prototype_id < 0 ||
        controller.host_vehicle_active)
        return ensure_host_vehicle(controller);
    if (!controller.spawn_attempt.can_attempt(g_state.server_tick))
        return nullptr;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return nullptr;

    // The fourth ObjContainer argument is the object's native belong/team.
    // -1 is only suitable for temporary/map-neutral objects; a live Vehicle
    // with that value is reclaimed by the post-update ownership pass.  The
    // remote player receives its own valid relationship identity so that the
    // stock weapon code can treat this as PvP rather than friendly fire.
    hta::ai::Player* const local_player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const local_vehicle =
        local_player != nullptr ? local_player->GetVehicle() : nullptr;
    if (local_vehicle == nullptr)
        return nullptr;
    const std::int32_t host_belong = local_vehicle->GetBelong();
    const std::int32_t remote_belong =
        multiplayer_remote_belong(host_belong, controller.entity_id);
    configure_free_for_all_relationship(*server->m_pObjects, host_belong,
                                        remote_belong);
    char object_name[64]{};
    std::snprintf(object_name, sizeof(object_name), "kraken_host_player_%u_%u",
                  controller.entity_id, static_cast<unsigned>(controller.generation));
    const ObjId object_id = server->m_pObjects->CreateNewObjectWithSuspendedPostLoad(
        controller.loadout.base_prototype_id, object_name, -1, remote_belong);
    if (object_id < 0) {
        controller.spawn_attempt.defer(g_state.server_tick, 30);
        LOG_ERROR("host dynamic player creation failed entity=%u generation=%u prototype=%d",
                  controller.entity_id, static_cast<unsigned>(controller.generation),
                  controller.loadout.base_prototype_id);
        return nullptr;
    }
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id);
    hta::ai::Vehicle* vehicle = vehicle_from_object(object);
    if (vehicle == nullptr || vehicle->GetPrototypeId() != controller.loadout.base_prototype_id) {
        controller.spawn_attempt.reject_permanently();
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("host dynamic player prototype rejected entity=%u requested=%d objId=%d",
                  controller.entity_id, controller.loadout.base_prototype_id, object_id);
        return nullptr;
    }

    controller.vehicle_obj_id = object_id;
    // A suspended object does not have an ODE body yet.  SetPost* is the
    // engine's pre-physics transform channel; unlike Set*Self it only records
    // the transform and therefore cannot make collision code dereference the
    // not-yet-created body.
    hta::CVector initial_position{};
    hta::Quaternion initial_rotation{};
    if (!resolve_host_remote_spawn_transform(controller, initial_position,
                                             initial_rotation)) {
        controller.spawn_attempt.defer(g_state.server_tick, 30);
        server->m_pObjects->AddObjIdToRemove(object_id);
        controller.vehicle_obj_id = kInvalidObjId;
        return nullptr;
    }
    vehicle->SetPostPosition(initial_position);
    vehicle->SetPostRotation(initial_rotation);
    if (!complete_suspended_vehicle_loadout(*server->m_pObjects, object_id,
                                            vehicle, "host dynamic player")) {
        controller.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        controller.vehicle_obj_id = kInvalidObjId;
        return nullptr;
    }
    const bool loadout_matches =
        vehicle_matches_loadout(*vehicle, controller.loadout);
    if (!loadout_matches) {
        // Only an actual part replacement requires retiring the native ODE
        // graph.  A Disable/Enable cycle on an untouched vehicle loses the
        // Wheel::m_suspensionNode pointers that `_EvaluateToDead` requires.
        vehicle->DisablePhysics();
        if (!apply_loadout_to_inactive_vehicle(*vehicle, controller.loadout,
                                                "host dynamic player")) {
            controller.spawn_attempt.reject_permanently();
            server->m_pObjects->AddObjIdToRemove(object_id);
            controller.vehicle_obj_id = kInvalidObjId;
            controller.host_vehicle_active = false;
            return nullptr;
        }
    }
    if (!ensure_vehicle_suspension_nodes(*vehicle, "host dynamic player")) {
        controller.spawn_attempt.reject_permanently();
        // Do not queue removal of a failed suspended graph during this server
        // tick: ObjContainer may still be iterating its post-load list.
        vehicle->SetInvisible();
        server->m_pObjects->AddObjToNotUpdate(vehicle);
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        return nullptr;
    }
    vehicle->SetNpcMotionControllerId(-1);
    vehicle->m_AI.m_pDM = nullptr;
    server->m_pObjects->AddObjToNotUpdate(vehicle);
    LOG_INFO("host dynamic player post-load complete entity=%u generation=%u objId=%d wheels=%u loadoutMatched=%u",
             controller.entity_id, static_cast<unsigned>(controller.generation),
             object_id, static_cast<unsigned>(vehicle->GetNumWheels()),
             loadout_matches ? 1u : 0u);
    const EntityRegistryBindResult bound = g_state.entities.bind(
        controller.entity_id, object_id, controller.generation);
    if (bound != EntityRegistryBindResult::Inserted &&
        bound != EntityRegistryBindResult::AlreadyBound) {
        controller.spawn_attempt.reject_permanently();
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        vehicle->SetInvisible();
        LOG_ERROR("host dynamic player entity bind failed entity=%u generation=%u objId=%d code=%u",
                  controller.entity_id, static_cast<unsigned>(controller.generation),
                  object_id, static_cast<unsigned>(bound));
        return nullptr;
    }
    if (!activate_host_remote_vehicle(controller, *vehicle)) {
        (void)g_state.entities.unbind_net_id(controller.entity_id);
        controller.spawn_attempt.reject_permanently();
        vehicle->SetUpdatingByODE(false);
        vehicle->DisablePhysics();
        server->m_pObjects->AddObjToNotUpdate(vehicle);
        vehicle->SetInvisible();
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        LOG_ERROR("host dynamic player activation failed entity=%u generation=%u objId=%d",
                  controller.entity_id, static_cast<unsigned>(controller.generation),
                  object_id);
        return nullptr;
    }
    controller.applied_loadout_revision = controller.loadout.revision;
    return vehicle;
}

void apply_host_inputs()
{
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.has_input)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(controller);
        if (!vehicle)
            continue;
        vehicle->SetThrottle(controller.input.throttle, false);
        vehicle->m_steerRadians = controller.input.steer;
        vehicle->SetBrake(controller.input.brake);
        vehicle->m_bHandBrake = controller.input.handbrake;
        vehicle->SetHorn(controller.input.horn);
        if (controller.input.request_unstuck && !controller.unstuck_was_requested) {
            vehicle->GetOutOfDifficultPlace();
            LOG_INFO("host executed unstuck entity=%u", controller.entity_id);
        }
        controller.unstuck_was_requested = controller.input.request_unstuck;
    }
}

bool apply_authoritative_remote_input(hta::ai::Vehicle* const vehicle)
{
    if (!g_state.is_host || vehicle == nullptr)
        return false;
    for (const PeerController& controller : g_state.controllers) {
        if (!controller.has_input || controller.vehicle_obj_id != vehicle->GetId())
            continue;
        // Vehicle::_KeepThrottle runs after controller polling.  Applying the
        // network command here prevents the unowned clone's native controller
        // from zeroing throttle/brake between packets.
        vehicle->m_throttle = controller.input.throttle;
        vehicle->m_steerRadians = controller.input.steer;
        vehicle->m_brake = controller.input.brake;
        vehicle->m_bHandBrake = controller.input.handbrake;
        vehicle->m_bAutoBrake = false;
        vehicle->SetHorn(controller.input.horn);
        return true;
    }
    return false;
}

bool drive_network_gun(hta::ai::Vehicle& vehicle, WeaponCommand& command,
                       hta::ai::Obj* const resolved_target_object)
{
    hta::ai::Gun* fallback = nullptr;
    const auto names = vehicle.GetAttachedPartNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        hta::ai::VehiclePart* const part = vehicle.GetPartByName(names[index]);
        if (part == nullptr || !part->IsKindOf(hta::ai::Gun::p_classObject))
            continue;
        hta::ai::Gun* const gun = static_cast<hta::ai::Gun*>(part);
        if (fallback == nullptr)
            fallback = gun;
        if (gun->GetPrototypeId() == command.gun_id) {
            fallback = gun;
            break;
        }
    }
    if (fallback == nullptr) {
        LOG_ERROR("host cannot resolve network gun prototype=%d vehicle=%d",
                  command.gun_id, vehicle.GetId());
        return false;
    }
    if (!g_state.combat_autotest_scenario.empty() &&
        command.sequence % 20u == 0u) {
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST host-gun entity=%u objId=%d gun=%d canFire=%u charge=%u pool=%u targetObj=%d",
                 command.entity_id, vehicle.GetId(), fallback->GetPrototypeId(),
                 fallback->CanFire() ? 1u : 0u,
                 fallback->GetShellsInCurrentCharge(), fallback->GetShellsInPool(),
                 resolved_target_object != nullptr ? resolved_target_object->GetId()
                                                   : kInvalidObjId);
    }
    g_replaying_network_fire = true;
    g_active_host_weapon_command = &command;
    // objIds are process-local.  The client sent a stable NetId, and the
    // authority has just resolved it to this process's object.  Gun::CanFire
    // reads m_targetObjId, so coordinates alone are insufficient for a
    // vehicle target.
    if (resolved_target_object != nullptr) {
        // `FireFromWeaponCustom2` is the stock object-target convenience
        // bridge, but it declines all guns on the dynamically-owned remote
        // player Vehicle.  Invoke the selected attached Gun directly after
        // assigning its live host target. This still follows Gun::CanFire,
        // ammo, projectile and Vehicle::InflictDamage without fabricating a
        // hit or damage result.
        fallback->SetTargetId(resolved_target_object->GetId());
        (void)fallback->Fire(command.trigger_held);
    } else {
        const hta::CVector aim_point = to_engine_vector(command.aim_point);
        vehicle.FireFromWeaponCustom(command.trigger_held, aim_point, nullptr);
    }
    g_active_host_weapon_command = nullptr;
    g_replaying_network_fire = false;
    return true;
}

hta::ai::Gun* find_network_gun(hta::ai::Vehicle& vehicle,
                               const std::int32_t prototype_id)
{
    hta::ai::Gun* fallback = nullptr;
    const auto names = vehicle.GetAttachedPartNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        hta::ai::VehiclePart* const part = vehicle.GetPartByName(names[index]);
        if (part == nullptr || !part->IsKindOf(hta::ai::Gun::p_classObject))
            continue;
        hta::ai::Gun* const gun = static_cast<hta::ai::Gun*>(part);
        if (fallback == nullptr)
            fallback = gun;
        if (gun->GetPrototypeId() == prototype_id)
            return gun;
    }
    return fallback;
}

bool drive_network_weapon_presentation(hta::ai::Vehicle& vehicle,
                                       const WeaponCommand& command)
{
    hta::ai::Gun* const gun = find_network_gun(vehicle, command.gun_id);
    if (gun == nullptr)
        return false;
    g_presenting_confirmed_network_fire = true;
    // Re-run the engine's sustained trigger state locally.  It supplies the
    // native automatic-fire cadence and sound; client-side InflictDamage is
    // still blocked by vehicle_inflict_damage_hook.
    if (command.target_entity_id != kInvalidNetId) {
        hta::ai::Vehicle* const target =
            find_vehicle(command.target_entity_id);
        if (target == nullptr) {
            g_presenting_confirmed_network_fire = false;
            return false;
        }
        vehicle.FireFromWeaponCustom2(command.trigger_held, target->GetId());
    } else {
        const hta::CVector target = to_engine_vector(command.aim_point);
        vehicle.FireFromWeaponCustom(command.trigger_held, target, nullptr);
    }
    g_presenting_confirmed_network_fire = false;
    return true;
}

void apply_network_weapon_ammo(hta::ai::Vehicle& vehicle,
                               const WeaponCommand& command)
{
    if (!command.has_ammo_state)
        return;
    hta::ai::Gun* const gun = find_network_gun(vehicle, command.gun_id);
    if (gun == nullptr)
        return;
    gun->SetShellsInCurrentCharge(command.shells_in_current_charge);
    gun->SetShellsInPool(command.shells_in_pool);
    // The client receives authoritative shell counters without executing the
    // host's _DoFire transition. Recreate the engine's automatic reload
    // transition when the confirmed shot emptied a reloadable magazine.
    if (gun->IsWithCharging() &&
        command.shells_in_current_charge == 0 &&
        command.shells_in_pool != 0)
        gun->BeginReCharge();
}

void apply_host_weapons(const float elapsed_time)
{
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.has_weapon || !controller.host_vehicle_active)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(controller);
        if (vehicle == nullptr)
            continue;
        WeaponCommand command = controller.weapon;
        // This is the original Vehicle -> Gun/CompoundGun route.  Its shells,
        // collision callbacks and InflictDamage are therefore evaluated only
        // in the host's ODE simulation.
        bool applied = false;
        ObjId resolved_target_obj_id = kInvalidObjId;
        hta::ai::Obj* resolved_target_object = nullptr;
        VehicleVector3 resolved_aim{};
        bool has_resolved_aim = false;
        if (command.target_entity_id != 0) {
            hta::ai::Vehicle* const target = find_vehicle(command.target_entity_id);
            if (target != nullptr) {
                resolved_target_obj_id = target->GetId();
                resolved_target_object = target;
                // WeaponFirer/FireFromWeaponCustom2 use the physical
                // geometric centre for an object target.  The base position
                // is below the chassis on this map and makes a correctly
                // fired projectile intersect terrain before the Vehicle.
                const hta::CVector position = target->GetGeometricCenter();
                resolved_aim = {position.x, position.y, position.z};
                has_resolved_aim = true;
            } else {
                LOG_ERROR("host weapon target entity=%u is unavailable for shooter=%u",
                          command.target_entity_id, command.entity_id);
            }
        } else if (command.has_aim_point &&
                   valid_weapon_aim_point(command.aim_point)) {
            resolved_aim = command.aim_point;
            has_resolved_aim = true;
        } else {
            LOG_ERROR("host weapon command rejected entity=%u shot=%u: no valid aim point",
                      command.entity_id, command.shot_id);
        }
        if (has_resolved_aim) {
            apply_visual_weapon_aim(*vehicle, resolved_aim, command.aim_speed);
            applied = drive_network_gun(*vehicle, command,
                                         resolved_target_object);
        }
        LOG_INFO("weapon host entity=%u gun=%d target=%u trigger=%u applied=%u",
                  command.entity_id, command.gun_id,
                  command.target_entity_id,
                  command.trigger_held ? 1u : 0u, applied ? 1u : 0u);
        // Relay the held/released state once per client update.  Confirmed
        // shots are emitted separately by gun_do_fire_hook with ammo state.
        const Clock::time_point now = Clock::now();
        if (applied && (command.has_aim_point || command.target_entity_id != 0) &&
            now >= controller.next_weapon_presentation) {
            controller.next_weapon_presentation = now + std::chrono::milliseconds(50);
            relay_host_weapon_presentation(vehicle, command.trigger_held,
                                           resolved_target_obj_id,
                                           command.gun_id,
                                           has_resolved_aim ? &resolved_aim : nullptr,
                                           command.aim_speed);
        }
        if (has_resolved_aim) {
            controller.presentation_weapon = command;
            controller.presentation_weapon.aim_point = resolved_aim;
            controller.presentation_weapon.has_aim_point = true;
            controller.has_presentation_weapon = true;
        }
        if (!applied)
            LOG_ERROR("host weapon state rejected entity=%u sequence=%u",
                      command.entity_id, command.sequence);
    }
}

void apply_host_loadouts()
{
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.has_loadout ||
            controller.applied_loadout_revision == controller.loadout.revision)
            continue;
        if (controller.host_vehicle_active) {
            if (controller.deferred_loadout_revision != controller.loadout.revision) {
                controller.deferred_loadout_revision = controller.loadout.revision;
                LOG_WARNING("deferred host loadout on active dynamic vehicle entity=%u revision=%u objId=%d",
                            controller.entity_id, controller.loadout.revision,
                            controller.vehicle_obj_id);
            }
            continue;
        }
        if (controller.loadout.base_prototype_id < 0) {
            if (controller.deferred_loadout_revision != controller.loadout.revision) {
                controller.deferred_loadout_revision = controller.loadout.revision;
                LOG_ERROR("host loadout lacks client base Vehicle prototype entity=%u revision=%u",
                          controller.entity_id, controller.loadout.revision);
            }
            continue;
        }
        hta::ai::Vehicle* const vehicle = create_host_remote_vehicle(controller);
        if (vehicle == nullptr)
            continue;
        controller.applied_loadout_revision = controller.loadout.revision;
        LOG_INFO("applied host dynamic loadout entity=%u revision=%u parts=%u objId=%d prototype=%d",
                 controller.entity_id, controller.loadout.revision,
                 static_cast<unsigned>(controller.loadout.parts.size()),
                 vehicle->GetId(), vehicle->GetPrototypeId());
    }
}

void apply_host_weapon_presentations(float)
{
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.host_vehicle_active ||
            !controller.has_presentation_weapon ||
            !controller.presentation_weapon.has_aim_point)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(controller);
        if (vehicle != nullptr)
            apply_visual_weapon_aim(*vehicle,
                                    controller.presentation_weapon.aim_point,
                                    controller.presentation_weapon.aim_speed);
    }
}

hta::CVector to_engine_vector(const VehicleVector3& value)
{
    return {value.x, value.y, value.z};
}

void apply_visual_weapon_aim(hta::ai::Vehicle& vehicle,
                             const VehicleVector3& aim_point,
                             float aim_speed)
{
    if (!valid_weapon_aim_point(aim_point))
        return;
    if (!std::isfinite(aim_speed) || aim_speed < 0.0f || aim_speed > 100.0f)
        return;
    const hta::CVector target = to_engine_vector(aim_point);
    vehicle.WeaponLookAtPoint(target, aim_speed);
    const auto names = vehicle.GetAttachedPartNames();
    std::uint32_t aimed = 0;
    for (std::size_t index = 0; index < names.size(); ++index) {
        hta::ai::VehiclePart* const part = vehicle.GetPartByName(names[index]);
        if (part == nullptr || !part->IsKindOf(hta::ai::Gun::p_classObject))
            continue;
        static_cast<hta::ai::Gun*>(part)->LookAtPoint(
            target, aim_speed);
        ++aimed;
    }
    if (aimed == 0)
        LOG_ERROR("visual weapon aim found no Gun parts objId=%d", vehicle.GetId());
}

hta::CVector shared_spawn_position(const hta::CVector& host_position,
                                   const PlayerSlotIndex slot)
{
    // Keep vehicles in one formation but never overlap ODE collision bodies.
    // Entity 1 is the host; later players use a deterministic 12 m offset.
    return {host_position.x + 12.0f * static_cast<float>(slot),
            host_position.y + (slot == 0 ? 0.0f : 1.0f), host_position.z};
}

hta::Quaternion to_engine_quaternion(const VehicleQuaternion& value)
{
    return {value.x, value.y, value.z, value.w};
}

hta::ai::Vehicle* ensure_remote_vehicle_replica(RemoteEntity& remote,
                                                const VehicleSnapshot& snapshot)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return nullptr;

    ObjId bound_object_id = kInvalidObjId;
    EntityGeneration bound_generation = kInvalidEntityGeneration;
    if (g_state.entities.lookup_obj_id(remote.entity_id, bound_object_id,
                                       bound_generation)) {
        if (bound_generation != remote.generation) {
            LOG_ERROR("remote vehicle binding generation mismatch entity=%u bound=%u expected=%u objId=%d",
                      remote.entity_id, static_cast<unsigned>(bound_generation),
                      static_cast<unsigned>(remote.generation), bound_object_id);
            return nullptr;
        }
        hta::ai::Obj* const object =
            server->m_pObjects->GetEntityByObjId(bound_object_id);
        if (hta::ai::Vehicle* const vehicle = vehicle_from_object(object))
            return vehicle;

        LOG_ERROR("remote vehicle binding points to a missing/non-vehicle object entity=%u objId=%d",
                  remote.entity_id, bound_object_id);
        (void)g_state.entities.unbind_net_id(remote.entity_id);
    }

    if (!remote.spawn_attempt.can_attempt(g_state.server_tick))
        return nullptr;

    if (remote.has_loadout && remote.loadout.base_prototype_id >= 0 &&
        remote.loadout.base_prototype_id != remote.prototype_id) {
        remote.spawn_attempt.reject_permanently();
        LOG_ERROR("remote vehicle base prototype mismatch entity=%u generation=%u spawnPrototype=%d loadoutPrototype=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  remote.prototype_id, remote.loadout.base_prototype_id);
        return nullptr;
    }

    char object_name[64]{};
    std::snprintf(object_name, sizeof(object_name), "kraken_remote_vehicle_%u_%u",
                  remote.entity_id, static_cast<unsigned>(remote.generation));
    hta::ai::Player* const local_player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const local_vehicle =
        local_player != nullptr ? local_player->GetVehicle() : nullptr;
    const std::int32_t replica_belong = local_vehicle != nullptr
        ? multiplayer_remote_belong(local_vehicle->GetBelong(), remote.entity_id)
        : remote.belong;
    if (local_vehicle != nullptr)
        configure_free_for_all_relationship(*server->m_pObjects,
                                            local_vehicle->GetBelong(),
                                            replica_belong);
    const ObjId object_id = server->m_pObjects->CreateNewObjectWithSuspendedPostLoad(
        remote.prototype_id, object_name, -1, replica_belong);
    if (object_id < 0) {
        remote.spawn_attempt.defer(g_state.server_tick, 30);
        LOG_ERROR("remote vehicle creation failed entity=%u generation=%u prototype=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  remote.prototype_id);
        return nullptr;
    }

    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id);
    hta::ai::Vehicle* vehicle = vehicle_from_object(object);
    if (vehicle == nullptr) {
        remote.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("remote vehicle prototype produced a non-vehicle entity=%u prototype=%d objId=%d",
                  remote.entity_id, remote.prototype_id, object_id);
        return nullptr;
    }

    vehicle->SetPostPosition(to_engine_vector(snapshot.position));
    vehicle->SetPostRotation(to_engine_quaternion(snapshot.rotation));

    if (!complete_suspended_vehicle_loadout(*server->m_pObjects, object_id,
                                            vehicle, "remote presentation")) {
        remote.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("remote vehicle post-load failed entity=%u generation=%u objId=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  object_id);
        return nullptr;
    }

    // This is a network presentation object, not a second simulation. Retire
    // all local authority before the next ObjContainer update; the host is
    // the sole owner of AI, ODE and combat for this entity.
    vehicle->SetUpdatingByODE(false);
    vehicle->DisablePhysics();
    if (remote.has_loadout &&
        !apply_loadout_to_inactive_vehicle(*vehicle, remote.loadout,
                                            "remote presentation")) {
        remote.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("remote vehicle loadout application failed entity=%u generation=%u objId=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  object_id);
        return nullptr;
    }
    vehicle->SetNpcMotionControllerId(-1);
    vehicle->m_AI.m_pDM = nullptr;
    server->m_pObjects->AddObjToNotUpdate(object);
    vehicle->SetPositionSelf(to_engine_vector(snapshot.position));
    vehicle->SetRotationSelf(to_engine_quaternion(snapshot.rotation));
    vehicle->SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    vehicle->SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    vehicle->SetThrottle(0.0f, false);
    vehicle->SetBrake(1.0f);
    vehicle->m_bHandBrake = true;
    const EntityRegistryBindResult bound = g_state.entities.bind(
        remote.entity_id, object_id, remote.generation);
    if (bound != EntityRegistryBindResult::Inserted) {
        remote.spawn_attempt.reject_permanently();
        vehicle->SetUpdatingByODE(false);
        vehicle->DisablePhysics();
        server->m_pObjects->AddObjToNotUpdate(vehicle);
        vehicle->SetInvisible();
        LOG_ERROR("remote vehicle registry bind failed entity=%u generation=%u objId=%d code=%u",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  object_id, static_cast<unsigned>(bound));
        return nullptr;
    }
    if (remote.has_loadout)
        remote.applied_loadout_revision = remote.loadout.revision;

    LOG_INFO("created remote vehicle replica entity=%u generation=%u kind=%u prototype=%d objId=%d loadoutRevision=%u",
             remote.entity_id, static_cast<unsigned>(remote.generation),
             static_cast<unsigned>(remote.kind), remote.prototype_id, object_id,
             remote.applied_loadout_revision);
    return vehicle;
}

hta::ai::Vehicle* ensure_remote_vehicle(RemoteEntity& remote,
                                        const VehicleSnapshot&)
{
    if (!remote.has_spawn || remote.prototype_id < 0) {
        // Snapshots are unreliable and may beat the reliable EntitySpawn.
        // Buffer them; creation is retried on the next frame after metadata
        // arrives instead of guessing with the local player's prototype.
        return nullptr;
    }
    if (remote.kind == EntityKind::PlayerVehicle) {
        // ObjContainer creation is legal only before CServer::Update. This
        // post-simulation interpolation phase consumes a prebuilt replica.
        return find_vehicle(remote.entity_id, remote.generation);
    }
    return nullptr;
}

void apply_remote_loadout(RemoteEntity& remote, hta::ai::Vehicle& vehicle);

void materialize_remote_vehicle_replicas()
{
    if (g_state.is_host)
        return;
    for (RemoteEntity& remote : g_state.remote_entities) {
        if (!remote.has_spawn || remote.retired ||
            remote.kind != EntityKind::PlayerVehicle ||
            !remote.has_spawn_snapshot)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_remote_vehicle_replica(
            remote, remote.spawn_snapshot);
        if (vehicle != nullptr && remote.has_loadout &&
            remote.applied_loadout_revision != remote.loadout.revision)
            apply_remote_loadout(remote, *vehicle);
    }
}

void apply_remote_loadout(RemoteEntity& remote, hta::ai::Vehicle& vehicle)
{
    if (!remote.has_loadout ||
        remote.applied_loadout_revision == remote.loadout.revision)
        return;
    if (vehicle.bIsUpdatingByODE()) {
        LOG_WARNING("deferred remote loadout on ODE presentation entity=%u revision=%u objId=%d",
                    remote.entity_id, remote.loadout.revision, vehicle.GetId());
        return;
    }
    if (!apply_loadout_to_inactive_vehicle(vehicle, remote.loadout,
                                           "remote presentation"))
        return;
    remote.applied_loadout_revision = remote.loadout.revision;
    LOG_INFO("applied remote presentation loadout entity=%u revision=%u parts=%u objId=%d",
             remote.entity_id, remote.loadout.revision,
             static_cast<unsigned>(remote.loadout.parts.size()), vehicle.GetId());
}

void apply_remote_snapshots(float)
{
    if (g_state.is_host || g_state.remote_entities.empty())
        return;

    const SnapshotTimestampMs current = now_ms();
    const SnapshotTimestampMs target = current > kInterpolationDelayMs
        ? current - kInterpolationDelayMs : 0;
    for (RemoteEntity& remote : g_state.remote_entities) {
        VehicleSnapshot sampled{};
        if (!snapshot_interpolation_succeeded(
                remote.snapshots.sample(target, sampled)))
            continue;
        hta::ai::Vehicle* const ghost = ensure_remote_vehicle(remote, sampled);
        if (ghost == nullptr)
            continue;

        ghost->SetPositionSelf(to_engine_vector(sampled.position));
        ghost->SetRotationSelf(to_engine_quaternion(sampled.rotation));
        ghost->SetLinearVelocity(to_engine_vector(sampled.linear_velocity));
        ghost->SetAngularVelocity(to_engine_vector(sampled.angular_velocity));
        apply_health_fraction(*ghost, sampled.health_fraction);
        if (!remote.has_weapon)
            continue;

        hta::ai::Vehicle* target = nullptr;
        if (remote.weapon.target_entity_id != 0)
            target = find_vehicle(remote.weapon.target_entity_id);
        if (target != nullptr) {
            const hta::CVector position = target->GetPosition();
            apply_visual_weapon_aim(*ghost,
                                    {position.x, position.y, position.z},
                                    remote.weapon.aim_speed);
        } else if (remote.weapon.has_aim_point) {
            apply_visual_weapon_aim(*ghost, remote.weapon.aim_point,
                                    remote.weapon.aim_speed);
        } else {
            LOG_ERROR("remote weapon event entity=%u shot=%u has no usable aim",
                      remote.entity_id, remote.weapon.shot_id);
        }

        if (!drive_network_weapon_presentation(*ghost, remote.weapon))
            LOG_ERROR("remote weapon state rejected entity=%u sequence=%u gun=%d",
                      remote.entity_id, remote.weapon.sequence,
                      remote.weapon.gun_id);
    }
}

void apply_local_weapon_presentation()
{
    if (g_state.is_host || !g_state.has_local_weapon_state)
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return;
    const WeaponCommand& command = g_state.local_weapon_state;
    if (command.has_aim_point)
        apply_visual_weapon_aim(*vehicle, command.aim_point, command.aim_speed);
    if (!drive_network_weapon_presentation(*vehicle, command))
        LOG_ERROR("local weapon state rejected sequence=%u gun=%d",
                  command.sequence, command.gun_id);
}

void apply_local_correction()
{
    if (g_state.is_host || g_state.local_entity_id == kInvalidNetId)
        return;
    VehicleSnapshot authoritative{};
    const SnapshotTimestampMs now = now_ms();
    const SnapshotTimestampMs target = now > kInterpolationDelayMs ? now - kInterpolationDelayMs : 0;
    if (!snapshot_interpolation_succeeded(g_state.local_correction.sample(target, authoritative)))
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (!vehicle)
        return;
    (void)bind_local_player_vehicle();
    constexpr float alpha = 0.15f;
    const hta::CVector current = vehicle->GetPosition();
    const hta::CVector target_pos = to_engine_vector(authoritative.position);
    vehicle->SetPositionSelf(hta::CVector(current.x + (target_pos.x-current.x)*alpha,
                                           current.y + (target_pos.y-current.y)*alpha,
                                           current.z + (target_pos.z-current.z)*alpha));
    // Position-only correction made the locally controlled client vehicle
    // slide sideways: its host-authoritative heading was never consumed.
    // SLerp keeps the visual/drivetrain rotation continuous across 20 Hz
    // snapshots and avoids the sign discontinuity of raw quaternion lerp.
    vehicle->SetRotationSelf(hta::Quaternion::SLerp(
        vehicle->GetRotation(), to_engine_quaternion(authoritative.rotation), alpha));
    const hta::CVector velocity = vehicle->GetLinearVelocity();
    const hta::CVector target_velocity = to_engine_vector(authoritative.linear_velocity);
    vehicle->SetLinearVelocity(hta::CVector(velocity.x + (target_velocity.x-velocity.x)*alpha,
                                             velocity.y + (target_velocity.y-velocity.y)*alpha,
                                           velocity.z + (target_velocity.z-velocity.z)*alpha));
    apply_health_fraction(*vehicle, authoritative.health_fraction);
}

void pump()
{
    if (!g_state.session)
        return;

    g_state.lan_discovery.pump();

    const TransportResult result = g_state.session->pump();
    if (!result && result.code != TransportResultCode::WouldBlock)
        LOG_ERROR("network pump failed code=%u", static_cast<unsigned>(result.code));

    std::array<SessionEvent, 64> events{};
    for (;;) {
        const std::size_t count = g_state.session->drain_events(events);
        if (count == 0)
            break;
        for (std::size_t index = 0; index < count; ++index)
            handle_event(std::move(events[index]));
    }

    if (g_state.host_defeat_session_end_pending) {
        g_state.host_defeat_session_end_pending = false;
        LOG_INFO("ending multiplayer session after authoritative host defeat or host disconnect");
        (void)EndSession();
        return;
    }

    const Clock::time_point now = Clock::now();
    if (!g_state.is_host && g_state.peers.empty() &&
        g_state.session->state() == SessionState::Ready &&
        g_lifecycle_config && now >= g_state.next_reconnect) {
        const Endpoint endpoint{g_lifecycle_config->address,
                                g_lifecycle_config->port};
        const TransportResult reconnect = g_state.session->connect(endpoint);
        if (reconnect) {
            reset_impact_damage_state();
            g_state.session_epoch = 0;
        }
        g_state.next_reconnect = now + g_state.reconnect_backoff;
        g_state.reconnect_backoff = (std::min)(g_state.reconnect_backoff * 2,
            std::chrono::seconds(16));
        LOG_INFO("reconnect %s:%u code=%u backoff=%llds", endpoint.host.c_str(),
                 endpoint.port, static_cast<unsigned>(reconnect.code),
                 static_cast<long long>(g_state.reconnect_backoff.count()));
    }
    if (now < g_state.next_ping)
        return;
    g_state.next_ping = now + std::chrono::seconds(1);
    for (const PeerId peer : g_state.peers)
        (void)g_state.session->ping(peer);
}

VehicleVector3 to_snapshot_vector(const hta::CVector& value)
{
    return {value.x, value.y, value.z};
}

VehicleQuaternion to_snapshot_quaternion(const hta::Quaternion& value)
{
    const float norm_squared = value.x * value.x + value.y * value.y +
        value.z * value.z + value.w * value.w;
    if (!std::isfinite(norm_squared) || norm_squared <= 0.000001f)
        return {0.0f, 0.0f, 0.0f, 1.0f};
    const float inverse_norm = 1.0f / std::sqrt(norm_squared);
    return {value.x * inverse_norm, value.y * inverse_norm,
            value.z * inverse_norm, value.w * inverse_norm};
}

bool peer_is_interested(PeerId peer, NetId subject, const hta::ai::Vehicle& source)
{
    PeerController* const controller = find_controller(peer);
    if (controller == nullptr || controller->entity_id == subject)
        return true; // own correction must never be culled
    hta::ai::Vehicle* const viewer = find_vehicle(controller->entity_id);
    if (viewer == nullptr)
        return true; // until its vehicle exists, favour correctness over culling
    const hta::CVector a = source.GetPosition();
    const hta::CVector b = viewer->GetPosition();
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    const float radius = g_state.snapshot_interest_radius;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

void capture_and_broadcast_host_npc_snapshots(hta::ai::ObjContainer& objects)
{
    if (!g_state.is_host)
        return;
    reconcile_host_entities();
    for (auto iterator = objects.begin(); iterator != objects.end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        hta::ai::Vehicle* const vehicle_ptr = vehicle_from_object(object);
        if (object == nullptr || vehicle_ptr == nullptr ||
            is_player_controlled_vehicle(*vehicle_ptr) ||
            object->GetDeletedStatus() || vehicle_ptr->_GetDeadStatus() ||
            vehicle_ptr->GetHealth() <= 0.0f)
            continue;
        HostEntity* entity = find_host_entity_by_object(vehicle_ptr->GetId());
        if (entity == nullptr && !is_replicable_npc_vehicle(*object))
            continue;
        if (entity == nullptr)
            entity = register_host_entity(*vehicle_ptr, EntityKind::NpcVehicle);
        if (entity == nullptr || !entity->active)
            continue;

        hta::ai::Vehicle& vehicle = *vehicle_ptr;

        VehicleSnapshot snapshot{};
        snapshot.entity_id = entity->entity_id;
        snapshot.sequence = g_state.next_snapshot_sequence++;
        snapshot.server_tick = g_state.server_tick;
        snapshot.position = to_snapshot_vector(vehicle.GetPosition());
        snapshot.rotation = to_snapshot_quaternion(vehicle.GetRotation());
        snapshot.linear_velocity = to_snapshot_vector(vehicle.GetLinearVelocity());
        snapshot.angular_velocity = to_snapshot_vector(vehicle.GetAngularVelocity());
        snapshot.health_fraction = health_fraction(vehicle);
        std::array<Byte, kVehicleSnapshotWireSize> payload{};
        if (!vehicle_snapshot_codec_succeeded(
                encode_vehicle_snapshot(snapshot, payload)))
            continue;
        for (const PeerId peer : g_state.peers) {
            (void)publish_entity_spawn(peer, entity->entity_id, entity->kind,
                                       vehicle, kInvalidNetId,
                                       entity->generation);
            (void)publish_entity_loadout(peer, *entity);
            if (peer_is_interested(peer, entity->entity_id, vehicle))
                (void)g_state.session->send(peer, MessageType::Snapshot,
                                            Channel::Unreliable, payload);
        }
    }
}

void capture_and_broadcast_host_snapshot()
{
    if (!g_state.is_host)
        return;
    if (g_state.peers.empty()) {
        reconcile_host_entities();
        return;
    }

    const Clock::time_point now = Clock::now();
    if (now < g_state.next_snapshot)
        return;
    g_state.next_snapshot = now + std::chrono::milliseconds(50);

    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return;

    const ObjId vehicle_obj_id = vehicle->GetId();
    NetId entity_id = kInvalidNetId;
    if (!g_state.entities.lookup_net_id(vehicle_obj_id, entity_id)) {
        if (g_state.host_vehicle_obj_id != kInvalidObjId)
            (void)g_state.entities.unbind_obj_id(g_state.host_vehicle_obj_id);

        const EntityRegistryBindResult bound =
            g_state.entities.bind(1, vehicle_obj_id);
        if (bound != EntityRegistryBindResult::Inserted &&
            bound != EntityRegistryBindResult::AlreadyBound) {
            LOG_ERROR("cannot bind host vehicle objId=%d code=%u", vehicle_obj_id,
                      static_cast<unsigned>(bound));
            return;
        }
        g_state.host_vehicle_obj_id = vehicle_obj_id;
        if (!g_state.entities.lookup_net_id(vehicle_obj_id, entity_id))
            return;
    }

    VehicleSnapshot snapshot{};
    snapshot.entity_id = entity_id;
    snapshot.sequence = g_state.next_snapshot_sequence++;
    snapshot.server_tick = g_state.server_tick;
    snapshot.position = to_snapshot_vector(vehicle->GetPosition());
    snapshot.rotation = to_snapshot_quaternion(vehicle->GetRotation());
    snapshot.linear_velocity = to_snapshot_vector(vehicle->GetLinearVelocity());
    snapshot.angular_velocity = to_snapshot_vector(vehicle->GetAngularVelocity());
    snapshot.health_fraction = health_fraction(*vehicle);

    std::array<Byte, kVehicleSnapshotWireSize> payload{};
    const VehicleSnapshotCodecError encoded =
        encode_vehicle_snapshot(snapshot, MutableByteView{payload});
    if (!vehicle_snapshot_codec_succeeded(encoded)) {
        LOG_ERROR("host snapshot encode failed code=%u",
                  static_cast<unsigned>(encoded));
        return;
    }

    EntityGeneration host_generation = kInvalidEntityGeneration;
    (void)g_state.entities.lookup_generation(entity_id, host_generation);
    HostEntity host_loadout{};
    host_loadout.entity_id = entity_id;
    host_loadout.generation = host_generation;
    host_loadout.kind = EntityKind::PlayerVehicle;
    const bool has_host_loadout = capture_vehicle_loadout(
        *vehicle, entity_id, host_generation, host_loadout.loadout);
    for (const PeerId peer : g_state.peers) {
        (void)publish_entity_spawn(peer, snapshot.entity_id,
                                   EntityKind::PlayerVehicle, *vehicle,
                                   kInvalidNetId, host_generation);
        if (has_host_loadout)
            (void)publish_entity_loadout(peer, host_loadout);
        if (!peer_is_interested(peer, snapshot.entity_id, *vehicle))
            continue;
        const TransportResult result = g_state.session->send(
            peer, MessageType::Snapshot, Channel::Unreliable, ByteView{payload});
        if (!result)
            LOG_ERROR("snapshot send to peer=%u failed code=%u", peer,
                      static_cast<unsigned>(result.code));
    }

    // Each client-owned vehicle is simulated only by the host, then sent to
    // every client (including its owner) as the authoritative correction.
    for (const PeerController& controller : g_state.controllers) {
        hta::ai::Vehicle* const remote_vehicle = find_vehicle(controller.entity_id);
        if (remote_vehicle == nullptr)
            continue;
        VehicleSnapshot remote{};
        remote.entity_id = controller.entity_id;
        remote.sequence = g_state.next_snapshot_sequence++;
        remote.server_tick = g_state.server_tick;
        remote.position = to_snapshot_vector(remote_vehicle->GetPosition());
        remote.rotation = to_snapshot_quaternion(remote_vehicle->GetRotation());
        remote.linear_velocity = to_snapshot_vector(remote_vehicle->GetLinearVelocity());
        remote.angular_velocity = to_snapshot_vector(remote_vehicle->GetAngularVelocity());
        remote.health_fraction = health_fraction(*remote_vehicle);
        std::array<Byte, kVehicleSnapshotWireSize> remote_payload{};
        if (!vehicle_snapshot_codec_succeeded(
                encode_vehicle_snapshot(remote, remote_payload)))
            continue;
        EntityGeneration remote_generation = kInvalidEntityGeneration;
        (void)g_state.entities.lookup_generation(controller.entity_id,
                                                  remote_generation);
        for (const PeerId peer : g_state.peers) {
            (void)publish_entity_spawn(peer, remote.entity_id,
                                       EntityKind::PlayerVehicle,
                                       *remote_vehicle,
                                       remote.entity_id, remote_generation);
            if (controller.has_loadout) {
                HostEntity loadout_entity{};
                loadout_entity.entity_id = controller.entity_id;
                loadout_entity.generation = remote_generation;
                loadout_entity.kind = EntityKind::PlayerVehicle;
                loadout_entity.loadout = controller.loadout;
                loadout_entity.loadout.generation = remote_generation;
                (void)publish_entity_loadout(peer, loadout_entity);
            }
            if (peer_is_interested(peer, remote.entity_id, *remote_vehicle))
                (void)g_state.session->send(peer, MessageType::Snapshot,
                                            Channel::Unreliable, remote_payload);
        }
    }
    if (hta::ai::CServer* const server = hta::ai::CServer::Instance();
        server != nullptr && server->m_pObjects != nullptr)
        capture_and_broadcast_host_npc_snapshots(*server->m_pObjects);
}

void clear_network_pause_before_simulation()
{
    const bool session_active = g_state.session != nullptr &&
        g_state.session->running();
    hta::CMiracle3d* const application = hta::CMiracle3d::Instance();
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    const PauseSignals signals{
        session_active,
        application != nullptr && application->m_paused,
        server != nullptr && server->GetPause(),
        server != nullptr && server->m_InCinematic,
    };
    if (!should_clear_network_pause(signals)) {
        g_state.network_pause_was_cleared = false;
        return;
    }
    if (signals.application_paused)
        application->UnPause();
    if (signals.server_paused)
        server->SetPause(false);
    if (!g_state.network_pause_was_cleared) {
        LOG_INFO("suppressed local pause while multiplayer session is active");
        g_state.network_pause_was_cleared = true;
    }
}

void run_raid_autotest_tick()
{
    if (!g_state.raid_autotest_enabled)
        return;
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_raid_autotest)
        return;
    g_state.next_raid_autotest = now + std::chrono::seconds(1);

    hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
    hta::m3d::ScriptServer* const script_server =
        kernel ? kernel->m_scriptServer : nullptr;
    if (script_server == nullptr)
        return;

// Bootstrap follows the real map-transition trigger but deliberately opens
// the network session only after r1m1 has loaded. r0m0 is a shelter and does
// not contain the named player slots used by the multiplayer runtime. The
// authority then creates a small normal TeamCreate group near the player. The
// Lua guards make all actions idempotent and test-mode-only.
    constexpr const char* kAutotestProgram = R"lua(
local level = nil
if GET_GLOBAL_OBJECT ~= nil then level = GET_GLOBAL_OBJECT("CurrentLevel") end
if level ~= nil and level:GetLevelName() == "r0m0" and
   not KRAKEN_EFA_AUTOTEST_TRANSITION and TActivate ~= nil and SetVar ~= nil then
    KRAKEN_EFA_AUTOTEST_TRANSITION = true
    LOG("KRAKEN_AUTOTEST: transitioning r0m0 to r1m1")
    SetVar("ToMap", "r1m1")
    TActivate("PassingToMap1")
end
if level ~= nil and level:GetLevelName() == "r1m1" then
    if not KRAKEN_EFA_AUTOTEST_RAID_LOADED then
        KRAKEN_EFA_AUTOTEST_RAID_LOADED = true
        LOG("KRAKEN_AUTOTEST: r1m1 loaded")
    end
    if not KRAKEN_EFA_AUTOTEST_SESSION and EFA_MP ~= nil and
       EFA_MP.BeginRaid ~= nil then
        KRAKEN_EFA_AUTOTEST_SESSION = true
        LOG("KRAKEN_AUTOTEST: beginning r1m1 multiplayer session")
        EFA_MP.BeginRaid("r1m1")
    end
    if not KRAKEN_EFA_AUTOTEST_COMBAT and
       not KRAKEN_EFA_AUTOTEST_NPCS and EFA_MP ~= nil and EFA_MP.active == true and
       EFA_MP.IsAuthorityOrOffline ~= nil and EFA_MP.IsAuthorityOrOffline() and
       TeamCreate ~= nil and GetPlayerVehicle ~= nil then
        local player_vehicle = GetPlayerVehicle()
        if player_vehicle ~= nil then
            local p = player_vehicle:GetPosition()
            local team = TeamCreate("kraken_autotest_npcs", 1010,
                CVector(p.x + 30, p.y, p.z + 30), {"Bug01", "Bug01"}, nil)
            if team ~= nil and team ~= 0 then
                KRAKEN_EFA_AUTOTEST_NPCS = true
                LOG("KRAKEN_AUTOTEST: host NPC team created")
            end
        end
    end
end
)lua";
    const std::string autotest_program =
        std::string("KRAKEN_EFA_AUTOTEST_COMBAT = ") +
        (g_state.combat_autotest_scenario.empty() ? "false\n" : "true\n") +
        kAutotestProgram;
    const hta::m3d::eScriptError error = script_server->execute(
        autotest_program.c_str(), "kraken_raid_autotest");
    if (error != hta::m3d::eScriptError::SUCCESS &&
        error != hta::m3d::eScriptError::NOT_INITIALIZED &&
        !g_state.raid_autotest_error_logged) {
        LOG_ERROR("raid autotest Lua tick failed code=%u",
                  static_cast<unsigned>(error));
        g_state.raid_autotest_error_logged = true;
    }
}

void aim_combat_autotest_camera(const hta::ai::Vehicle& shooter,
                                const hta::ai::Vehicle& target)
{
    // Do not place either vehicle under the reticle.  Instead, reproduce the
    // camera orientation a player would use: point the actual active camera at
    // the other player's physical centre.  Stock Controls will consume these
    // angles on the next frame and its normal InfoCone/raycast then owns target
    // selection.  The target remains a real ODE-backed vehicle throughout.
    // The third-person camera is displaced from the vehicle.  Starting this
    // calculation at the vehicle centre made the crosshair describe a
    // parallel, offset ray: at close range it could visibly skim a turret
    // while the reticle was not centred on the target vehicle.  The engine
    // exposes the active camera world origin in CMiracle3d; use that exact
    // origin so this is the same line of sight a player sees on screen.
    hta::CMiracle3d* const application = hta::CMiracle3d::Instance();
    if (application == nullptr)
        return;
    const auto* const camera_bytes = reinterpret_cast<const std::byte*>(application);
    const hta::CVector from = *reinterpret_cast<const hta::CVector*>(
        camera_bytes + kCameraWorldOriginOffset);
    const hta::CVector to = target.GetGeometricCenter();
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    const float horizontal_distance = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(horizontal_distance) || horizontal_distance < 0.001f)
        return;

    // _UpdateSeenObjAndWeapons multiplies CMatrix::rotYPR(yaw, pitch, 0) by
    // INITIAL_OBJECTS_DIRECTION=(0,0,1).  In this row-vector matrix a
    // positive pitch moves that forward vector toward negative world Y, so an
    // elevated target requires a negative pitch.
    // CMatrix uses the engine's row-vector convention: for its forward
    // INITIAL_OBJECTS_DIRECTION=(0,0,1), positive world X is negative yaw.
    const float yaw = -std::atan2(dx, dz);
    const float pitch = -std::atan2(dy, horizontal_distance);
    // The injected Controls hook itself is reached through an EAX-based
    // call-site ABI, so its C++ `this` parameter cannot be trusted.  Obtain
    // the application through the engine's own singleton instead.
    auto* const bytes = reinterpret_cast<std::byte*>(application);
    *reinterpret_cast<float*>(bytes + kCameraPitchOffset) = pitch;
    *reinterpret_cast<float*>(bytes + kCameraYawOffset) = yaw;
}

void run_combat_autotest_tick(const float elapsed_time)
{
    if (g_state.combat_autotest_scenario.empty() || !g_state.session ||
        !g_state.session->running() || !g_state.player_slots_ready)
        return;
    const Clock::time_point now = Clock::now();
    // The gun and turret are held through the normal frame cadence.  Only the
    // client-to-host intent packet is rate limited; coupling both operations
    // to 50 ms made the target crosshair and muzzle lag visibly behind a real
    // player-controlled weapon.
    const bool weapon_intent_due = now >= g_state.next_combat_autotest;
    if (weapon_intent_due)
        g_state.next_combat_autotest = now + std::chrono::milliseconds(50);

    const bool host_shooter =
        g_state.combat_autotest_scenario == "host-kills-client";
    const bool client_shooter =
        g_state.combat_autotest_scenario == "client-kills-host";
    if ((!host_shooter && !client_shooter) ||
        (host_shooter != g_state.is_host))
        return;

    NetId shooter_id = kInvalidNetId;
    NetId target_id = kInvalidNetId;
    if (host_shooter) {
        if (g_state.controllers.empty())
            return;
        shooter_id = 1;
        target_id = g_state.controllers.front().entity_id;
    } else {
        shooter_id = g_state.local_entity_id;
        target_id = 1;
    }
    hta::ai::Vehicle* const shooter = find_vehicle(shooter_id);
    hta::ai::Vehicle* const target = find_vehicle(target_id);
    if (shooter == nullptr || target == nullptr || target->_GetDeadStatus())
        return;
    const bool log_aim = weapon_intent_due &&
                         ((g_state.combat_autotest_sequence++ % 30u) == 0u);
    if (!g_state.combat_autotest_started) {
        g_state.combat_autotest_started = true;
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST start scenario=%s shooter=%u target=%u",
                 g_state.combat_autotest_scenario.c_str(), shooter_id,
                 target_id);
    }
    // WeaponGroup additionally requires a live raw LMB impulse, which the
    // harness must not forge. AimAndFireFromWeapons is the verified native
    // target-aware engine entrypoint: it receives the real shooter and target
    // objects and performs normal aim, CanFire, projectile, and
    // Vehicle::InflictDamage processing. No transform, ammo, health, projectile
    // or damage state is fabricated.
    if (g_state.combat_autotest_weapon_part.empty()) {
        LOG_ERROR("KRAKEN_COMBAT_AUTOTEST has no selected weapon part scenario=%s",
                  g_state.combat_autotest_scenario.c_str());
        return;
    }
    const hta::CStr weapon_part(g_state.combat_autotest_weapon_part.c_str());
    hta::ai::VehiclePart* const weapon = shooter->GetPartByName(weapon_part);
    if (weapon == nullptr) {
        LOG_ERROR("KRAKEN_COMBAT_AUTOTEST selected weapon part is absent scenario=%s part=%s shooter=%u",
                  g_state.combat_autotest_scenario.c_str(),
                  g_state.combat_autotest_weapon_part.c_str(), shooter_id);
        return;
    }
    // Keep the camera aimed as a real player would, then let the native
    // WeaponFirer bridge perform weapon aim and firing with the gun's own
    // angular, rate, and ammo limits.
    const hta::CVector target_position = target->GetGeometricCenter();
    aim_combat_autotest_camera(*shooter, *target);
    if (log_aim) {
        const hta::CVector shooter_position = shooter->GetGeometricCenter();
        const float dx = target_position.x - shooter_position.x;
        const float dy = target_position.y - shooter_position.y;
        const float dz = target_position.z - shooter_position.z;
        const ObjId seen_obj_id = *reinterpret_cast<const ObjId*>(
            reinterpret_cast<const std::byte*>(shooter) + kVehicleSeenObjIdOffset);
        const std::uint32_t target_flags = *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<const std::byte*>(target) + 0x40);
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST aim scenario=%s shooterObj=%d targetObj=%d seenObj=%d distance=%.2f targetHealth=%.2f shooterBelong=%d targetBelong=%d enemy=%u targetFlags=0x%08X",
                 g_state.combat_autotest_scenario.c_str(), shooter->GetId(),
                 target->GetId(), seen_obj_id,
                 std::sqrt(dx * dx + dy * dy + dz * dz),
                 target->GetHealth(), shooter->GetBelong(), target->GetBelong(),
                 shooter->bIsEnemyWith(target) ? 1u : 0u, target_flags);
    }
    if (client_shooter) {
        // A client has only a presentation replica for entity 1.  Firing a
        // local projectile at that replica cannot damage the authoritative
        // host.  Keep the real camera/turret aim locally, then submit the
        // same semantic target identity that normal multiplayer input sends;
        // the host resolves it to its live player Vehicle and executes the
        // stock FireFromWeaponCustom2 -> Gun -> InflictDamage path.
        shooter->SetCustomControlWeaponsTargetObj(target->GetId());
        // WeaponLookAtPoint is also the normal outbound aim hook.  The real
        // Controls stage has just recorded an unpressed mouse button, which
        // would otherwise overwrite this test's held-fire command with a
        // release in the same frame.  Model the semantic multiplayer input,
        // not a synthetic OS click: the host still owns Gun::Fire, ammo,
        // projectiles, collision, and damage.
        g_state.local_weapon_trigger_held = true;
        g_state.local_weapon_target_obj_id = target->GetId();
        g_state.local_weapon_target_entity_id = target_id;
        g_state.local_weapon_gun_id = weapon->GetPrototypeId();
        shooter->WeaponLookAtPoint(target_position,
                                   (std::clamp)(elapsed_time, 0.0f, 0.1f));
        const VehicleVector3 authoritative_aim{
            target_position.x, target_position.y, target_position.z};
        if (weapon_intent_due) {
            (void)submit_native_weapon_intent(
                shooter, true, target->GetId(), weapon->GetPrototypeId(),
                // Preserve the engine's temporal aiming semantics on the host.
                // `Gun::LookAtPoint` consumes this as a per-frame delta, not as
                // an absolute angular-speed multiplier.  Sending 1.0 made the
                // remote turret snap and could send its shots away from the
                // camera's actual line of sight.
                &authoritative_aim, (std::clamp)(elapsed_time, 0.0f, 0.1f));
        }
        return;
    }
    // The test has already aimed the real camera and turret above.  Dispatch
    // the selected attached Gun through the same authoritative primitive used
    // for a remote player's command.  `AimAndFireFromWeapons` is an AI router
    // and, for a player-owned dynamic target, can decline before it reaches a
    // Gun at all.  Gun::Fire is the engine's actual projectile/ammo/damage
    // boundary, not a synthetic hit or damage operation.
    WeaponCommand command{};
    command.entity_id = shooter_id;
    command.sequence = g_state.combat_autotest_sequence;
    command.gun_id = weapon->GetPrototypeId();
    command.trigger_held = true;
    command.target_entity_id = target_id;
    command.aim_point = {target_position.x, target_position.y, target_position.z};
    command.has_aim_point = true;
    command.aim_speed = (std::clamp)(elapsed_time, 0.0f, 0.1f);
    // `Gun::Fire` deliberately does not steer a turret.  Match the normal
    // WeaponFirer sequence: update the real part transforms first, then fire
    // only through the stock Gun primitive.
    apply_visual_weapon_aim(*shooter, command.aim_point, command.aim_speed);
    (void)drive_network_gun(*shooter, command, target);
}

void observe_authoritative_combat_autotest_death()
{
    if (!g_state.is_host || g_state.combat_autotest_death_logged ||
        g_state.combat_autotest_scenario.empty())
        return;

    NetId shooter_id = kInvalidNetId;
    NetId target_id = kInvalidNetId;
    if (g_state.combat_autotest_scenario == "host-kills-client") {
        if (g_state.controllers.empty())
            return;
        shooter_id = 1;
        target_id = g_state.controllers.front().entity_id;
    } else if (g_state.combat_autotest_scenario == "client-kills-host") {
        shooter_id = g_state.controllers.empty()
            ? kInvalidNetId : g_state.controllers.front().entity_id;
        target_id = 1;
    } else {
        return;
    }

    hta::ai::Vehicle* const target = find_vehicle(target_id);
    if (target == nullptr || !target->_GetDeadStatus())
        return;
    g_state.combat_autotest_death_logged = true;
    LOG_INFO("KRAKEN_COMBAT_AUTOTEST death scenario=%s shooter=%u target=%u",
             g_state.combat_autotest_scenario.c_str(), shooter_id, target_id);
}

int __fastcall controls_hook(hta::CMiracle3d* const application, void*,
                             const float t0, const float tlen)
{
    const int result = g_controls(application, t0, tlen);
    // Run after the real control stage so its normal "mouse released" work
    // cannot overwrite the test-only held-fire intent in the same frame.
    run_combat_autotest_tick(tlen);
    return result;
}

// The original call is ai::CServer::Update(float): ECX=this, float on stack.
// A free __fastcall hook reserves EDX as the second dummy argument.
void __fastcall server_update_hook(void* server, void*, float elapsed_time)
{
    // Object names are built after DynamicScene itself.  MP_BeginSession is
    // triggered by Lua while that final map-load phase may still be running;
    // defer all network processing until named map slots become resolvable.
    if (g_state.session && g_state.session->running() &&
        !g_state.player_slots_ready) {
        g_server_update(server, nullptr, elapsed_time);
        ++g_state.server_tick;
        const Clock::time_point now = Clock::now();
        if (now >= g_state.next_player_slot_retry) {
            g_state.next_player_slot_retry = now + std::chrono::milliseconds(250);
            if (try_activate_player_slots())
                return;
        }
        if (now >= g_state.player_slot_deadline) {
            LOG_ERROR("MP player-slot readiness timed out after map load; ending session");
            (void)EndSession();
        }
        return;
    }
    if (g_state.session && g_state.session->running() &&
        !g_state.impact_damage_hook_installed &&
        !install_impact_damage_hook() &&
        g_state.impact_damage_hook_error_logged &&
        !g_state.combat_unavailable_logged) {
        LOG_ERROR("multiplayer combat remains disabled: InflictDamage hook unavailable");
        g_state.combat_unavailable_logged = true;
    }
    // Receive/apply packets before native gameplay and ODE advance.
    pump();
    if (!g_state.session || !g_state.session->running()) {
        g_server_update(server, nullptr, elapsed_time);
        return;
    }
    run_raid_autotest_tick();
    // EntitySpawn is reliable and is processed by pump(). CreateObject alters
    // the scene graph, so materialize remote vehicle replicas only at this
    // native pre-simulation boundary, never from post-update interpolation.
    materialize_remote_vehicle_replicas();
    clear_network_pause_before_simulation();
    // Establish host NPC identities before ODE/native combat callbacks run.
    // Snapshot-time discovery is too late for a hit delivered in this frame.
    reconcile_host_entities();
    // Do not retire objects by walking ObjContainer here.  In EFA the original
    // player vehicle remains in that container for one frame after
    // ChangeVehicleByExisting() transfers Player to its map-owned proxy; it
    // still carries an AI motion controller and is therefore
    // indistinguishable from a wanderer at this point.  Retiring it invalidates
    // the engine's ownership graph and can subsequently remove the proxy too.
    // Client wanderers are blocked at their narrow generation call site by
    // wanderer_spawn_hook(), before any game object is created.
    // Remote host players are distinct native vehicles. Their loadout is
    // installed while inactive, then the vehicle is bound and activated before
    // input and weapon processing; the dormant MP_PROXY slots are not used.
    apply_host_loadouts();
    apply_host_inputs();
    apply_host_weapons(elapsed_time);
    g_server_update(server, nullptr, elapsed_time);
    ++g_state.server_tick;
    // Vehicle::_EvaluateToDead runs inside the original server update, after
    // the damage hook returns. Observe the authoritative post-update state so
    // the combat harness records a real death rather than an early health hit.
    observe_authoritative_combat_autotest_death();
    send_client_input();
    apply_host_weapon_presentations(elapsed_time);
    apply_remote_snapshots(elapsed_time);
    apply_local_weapon_presentation();
    apply_pending_impact_damage();
    apply_local_correction();
    send_client_loadout();
    // The ODE frame is complete here; capture only through Vehicle/PhysicObj API.
    capture_and_broadcast_host_snapshot();
}

} // namespace

bool ApplyAuthoritativeRemoteInput(hta::ai::Vehicle* const vehicle)
{
    return apply_authoritative_remote_input(vehicle);
}

void Apply(const Config* config)
{
    if (!config)
        return;
    const EffectiveConfig effective = effective_config(*config);
    g_lifecycle_config = effective;
    g_state.raid_autotest_enabled =
        environment_uint("KRAKEN_EFA_RAID_AUTOTEST", 0, 0, 1) != 0;
    g_state.combat_autotest_scenario =
        environment("KRAKEN_EFA_COMBAT_AUTOTEST").value_or("");
    g_state.combat_autotest_weapon_part =
        environment("KRAKEN_EFA_COMBAT_WEAPON_PART").value_or("");
    g_state.next_combat_autotest = Clock::now();
    g_state.combat_autotest_sequence = 1;
    g_state.combat_autotest_started = false;
    g_state.combat_autotest_death_logged = false;
    g_state.next_raid_autotest = Clock::now();
    g_state.raid_autotest_error_logged = false;
    if (g_state.raid_autotest_enabled) {
        LOG_INFO("raid autotest enabled (test-only environment mode)");
        g_state.raid_autotest_bootstrap_attempted = false;
        g_state.raid_autotest_bootstrap_frames = 0;
        if (!install_raid_autotest_bootstrap_hook())
            return;
    }
    if (!install_engine_safety_hooks())
        return;
    if (g_state.session)
        return;

    // M5: a shelter can keep the engine hook/API loaded while the mod delays
    // the actual network connection until MP_BeginSession().
    if (!effective.enabled || !effective.autostart || effective.auto_lan) {
        try {
            routines::ChangeCall(reinterpret_cast<void*>(kServerUpdateCallSite),
                                 &server_update_hook);
            g_state.hook_installed = true;
        }
        catch (const std::exception& error) {
            LOG_ERROR("failed to install session hook: %s", error.what());
            return;
        }
        g_state.is_host = effective.host;
        g_state.spawn_together = effective.spawn_together;
        ::kraken::runtime::OnLoad(&register_lua_api);
        LOG_INFO("network API ready (autostart=%u enabled=%u)",
                 effective.autostart ? 1u : 0u, effective.enabled ? 1u : 0u);
        return;
    }

    SessionConfig session_config{};
    session_config.role = effective.host
                              ? SessionRole::Server
                              : SessionRole::Client;
    session_config.transport.role = session_config.role == SessionRole::Server
                                        ? TransportRole::Server
                                        : TransportRole::Client;
    session_config.transport.bind_endpoint.host =
        session_config.role == SessionRole::Server ? "0.0.0.0" : "127.0.0.1";
    session_config.transport.bind_endpoint.port =
        effective.port;
    session_config.transport.max_peers = effective.max_peers;

    g_state.session =
        std::make_unique<Session>(g_state.transport, std::move(session_config));
    TransportResult result = g_state.session->start();
    if (!result) {
        LOG_ERROR("failed to start network role=%s port=%u code=%u",
                  effective.host ? "host" : "client",
                  effective.port,
                  static_cast<unsigned>(result.code));
        g_state.session.reset();
        return;
    }

    if (!effective.host) {
        Endpoint endpoint{effective.address, effective.port};
        result = g_state.session->connect(endpoint);
        if (!result) {
            LOG_ERROR("failed to connect to %s:%u code=%u", endpoint.host.c_str(),
                      endpoint.port, static_cast<unsigned>(result.code));
            g_state.session->stop();
            g_state.session.reset();
            return;
        }
    }

    try {
        routines::ChangeCall(reinterpret_cast<void*>(kServerUpdateCallSite),
                             &server_update_hook);
        g_state.hook_installed = true;
    }
    catch (const std::exception& error) {
        LOG_ERROR("failed to install server tick hook: %s", error.what());
        g_state.session->stop();
        g_state.session.reset();
        return;
    }

    g_state.next_ping = Clock::now() + std::chrono::seconds(1);
    g_state.next_reconnect = Clock::now() + std::chrono::seconds(1);
    g_state.reconnect_backoff = std::chrono::seconds(1);
    g_state.snapshot_interest_radius = static_cast<float>(environment_uint(
        "KRAKEN_MP_INTEREST_RADIUS", 500, 25, 100000));
    g_state.next_snapshot = Clock::now();
    g_state.next_snapshot_sequence = 1;
    g_state.server_tick = 0;
    g_state.entities.clear();
    g_state.host_entities.clear();
    g_state.remote_entities.clear();
    g_state.player_slot_failure_logged.fill(false);
    g_state.player_slot_ready_logged.fill(false);
    g_state.spawn_publications.clear();
    g_state.loadout_publications.clear();
    g_state.controllers.clear();
    g_state.local_entity_id = kInvalidNetId;
    g_state.next_input = Clock::now();
    g_state.next_loadout = Clock::now();
    g_state.next_input_sequence = 1;
    g_state.next_weapon_sequence = 1;
    g_state.local_weapon_target_obj_id = kInvalidObjId;
    g_state.local_weapon_target_entity_id = kInvalidNetId;
    g_state.last_local_loadout_revision = 0;
    g_state.next_loot_id = 1;
    g_state.next_loot_container_id = 1;
    g_state.next_loot_source_generation = 1;
    g_state.session_epoch = 0;
    g_state.world_loot_revision = 1;
    g_state.loot_records.clear();
    g_state.loot_sources.clear();
    g_state.loot_receipts.clear();
    g_state.world_loot.clear();
    reset_impact_damage_state();
    g_state.host_vehicle_obj_id = kInvalidObjId;
    g_state.is_host = effective.host;
    g_state.spawn_together = effective.spawn_together;
    g_state.session_epoch = effective.host ? allocate_session_epoch() : 0;
    ::kraken::runtime::OnLoad(&register_lua_api);
    LOG_INFO("network started role=%s endpoint=%s:%u max_peers=%u",
             effective.host ? "host" : "client",
             effective.host ? "0.0.0.0" : effective.address.c_str(),
             effective.port, effective.max_peers);
}

bool IsSessionActive()
{
    return g_state.session != nullptr && g_state.session->running();
}

bool IsHost()
{
    return IsSessionActive() && g_state.is_host;
}

bool IsAuthority()
{
    return IsHost();
}

bool IsAuthorityOrOffline()
{
    return !IsSessionActive() || IsAuthority();
}

NetId LocalEntityId()
{
    return g_state.local_entity_id;
}

NetId PublishHostEntity(const std::int32_t object_id, const std::int32_t kind)
{
    if (!IsHost()) {
        LOG_ERROR("host entity publication rejected while not authoritative objId=%d kind=%d",
                  object_id, kind);
        return kInvalidNetId;
    }
    if (object_id == kInvalidObjId ||
        kind < static_cast<std::int32_t>(EntityKind::PlayerVehicle) ||
        kind > static_cast<std::int32_t>(EntityKind::LootContainer)) {
        LOG_ERROR("host entity publication rejected invalid objId=%d kind=%d",
                  object_id, kind);
        return kInvalidNetId;
    }
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Obj* const object = server && server->m_pObjects
        ? server->m_pObjects->GetEntityByObjId(object_id) : nullptr;
    hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
    if (vehicle == nullptr) {
        LOG_ERROR("host entity publication rejected non-vehicle objId=%d kind=%d",
                  object_id, kind);
        return kInvalidNetId;
    }
    HostEntity* const entity = register_host_entity(
        *vehicle, static_cast<EntityKind>(kind));
    if (entity == nullptr)
        return kInvalidNetId;
    LOG_INFO("host entity publication accepted entity=%u generation=%u kind=%d objId=%d",
             entity->entity_id, static_cast<unsigned>(entity->generation), kind,
             object_id);
    return entity->entity_id;
}

bool EndSession()
{
    if (!g_state.session)
        return false;
    const bool preserve_client_scene =
        !g_state.is_host && g_state.session_end_preserve_client_scene;
    g_state.session->stop();
    g_state.session.reset();
    g_state.lan_discovery.stop();
    if (g_state.is_host) {
        for (const PeerController& controller : g_state.controllers)
            schedule_entity_removal(controller.entity_id);
    }
    else if (!preserve_client_scene) {
        schedule_all_remote_removals();
    }
    // The local host player is not in controllers and the local client entity
    // is not a remote ghost.  Release every lease explicitly before clearing
    // the registry so both cases return their map-owned proxy safely.
    if (!preserve_client_scene)
        release_all_player_slots("session ended");
    g_state.peers.clear();
    g_state.entities.clear();
    g_state.host_entities.clear();
    g_state.remote_entities.clear();
    g_state.spawn_publications.clear();
    g_state.loadout_publications.clear();
    g_state.controllers.clear();
    g_state.loot_records.clear();
    g_state.loot_sources.clear();
    g_state.loot_receipts.clear();
    g_state.world_loot.clear();
    g_state.next_loot_id = 1;
    g_state.next_loot_container_id = 1;
    g_state.next_loot_source_generation = 1;
    g_state.session_epoch = 0;
    g_state.world_loot_revision = 1;
    g_state.invalid_lua_world_loot_object_args_logged = false;
    reset_impact_damage_state();
    g_state.local_entity_id = kInvalidNetId;
    g_state.local_weapon_target_obj_id = kInvalidObjId;
    g_state.local_weapon_target_entity_id = kInvalidNetId;
    g_state.previous_local_vehicle_obj_id = kInvalidObjId;
    g_state.local_player_vehicle_obj_id = kInvalidObjId;
    g_state.local_shared_spawn_applied = false;
    g_state.player_slots_ready = false;
    g_state.host_defeat_session_end_pending = false;
    g_state.session_end_preserve_client_scene = false;
    LOG_INFO("session ended; local shelter state is untouched%s",
             preserve_client_scene ? " (client scene teardown deferred)" : "");
    notify_lua_session_state(false, false);
    return true;
}

bool BeginSession()
{
    if (IsSessionActive()) {
        notify_lua_session_state(true, g_state.is_host);
        return true;
    }
    if (!g_lifecycle_config || !g_state.hook_installed)
        return false;
    EffectiveConfig effective = *g_lifecycle_config;
    if (effective.auto_lan) {
        // Binding a UDP port elects a host only within one operating system.
        // Different LAN machines can all bind 27016, so discover an existing
        // host first; only an unanswered broadcast creates a new listen server.
        const std::optional<Endpoint> endpoint = LanDiscovery::discover(
            kLanDiscoveryPort, kLanDiscoveryTimeout);
        const LanSessionSelection discovered = select_lan_session(endpoint, false);
        if (discovered.role == LanSessionRole::Client) {
            effective.host = false;
            effective.address = discovered.host->host;
            effective.port = discovered.host->port;
            LOG_INFO("LAN discovery found host=%s:%u",
                     effective.address.c_str(), effective.port);
        } else {
            // Simultaneous raid starts used to produce two hosts: both peers
            // completed their first broadcast timeout before either had
            // entered the server frame loop.  Let the lower IPv4 address lead
            // and then perform a short second discovery round.
            const auto delay = LanDiscovery::host_election_delay();
            Sleep(static_cast<DWORD>(delay.count()));
            const std::optional<Endpoint> elected = LanDiscovery::discover(
                kLanDiscoveryPort, std::chrono::milliseconds(1000));
            if (elected) {
                effective.host = false;
                effective.address = elected->host;
                effective.port = elected->port;
                LOG_INFO("LAN election joined host=%s:%u after %lld ms",
                         effective.address.c_str(), effective.port,
                         static_cast<long long>(delay.count()));
            } else if (select_lan_session(
                           std::nullopt,
                           g_state.lan_discovery.become_host(kLanDiscoveryPort,
                                                             effective.port)).role ==
                       LanSessionRole::Host) {
            effective.host = true;
                LOG_INFO("LAN discovery elected this peer as host port=%u delay=%lld ms",
                         effective.port, static_cast<long long>(delay.count()));
            } else {
                LOG_ERROR("LAN discovery could not find or host a LAN session");
                return false;
            }
        }
    }
    g_lifecycle_config->host = effective.host;
    g_lifecycle_config->address = effective.address;
    g_lifecycle_config->port = effective.port;
    SessionConfig session_config{};
    session_config.role = effective.host ? SessionRole::Server : SessionRole::Client;
    session_config.transport.role = effective.host ? TransportRole::Server : TransportRole::Client;
    session_config.transport.bind_endpoint.host = effective.host ? "0.0.0.0" : "127.0.0.1";
    session_config.transport.bind_endpoint.port = effective.port;
    session_config.transport.max_peers = effective.max_peers;
    g_state.session = std::make_unique<Session>(g_state.transport, std::move(session_config));
    TransportResult result = g_state.session->start();
    if (!result) {
        g_state.session.reset();
        g_state.lan_discovery.stop();
        return false;
    }
    if (!effective.host) {
        result = g_state.session->connect(Endpoint{effective.address, effective.port});
        if (!result) {
            g_state.session->stop();
            g_state.session.reset();
            g_state.lan_discovery.stop();
            return false;
        }
    }
    g_state.is_host = effective.host;
    g_state.spawn_together = effective.spawn_together;
    g_state.entities.clear();
    g_state.host_entities.clear();
    g_state.remote_entities.clear();
    g_state.spawn_publications.clear();
    g_state.loadout_publications.clear();
    g_state.next_dynamic_entity_id = 1000;
    g_state.next_loot_id = 1;
    g_state.next_loot_container_id = 1;
    g_state.next_loot_source_generation = 1;
    g_state.session_epoch = effective.host ? allocate_session_epoch() : 0;
    g_state.world_loot_revision = 1;
    g_state.invalid_lua_world_loot_object_args_logged = false;
    g_state.loot_records.clear();
    g_state.loot_sources.clear();
    g_state.loot_receipts.clear();
    g_state.world_loot.clear();
    g_state.local_player_vehicle_obj_id = kInvalidObjId;
    g_state.local_shared_spawn_applied = false;
    g_state.player_slots_ready = false;
    g_state.next_player_slot_retry = Clock::now();
    g_state.player_slot_deadline = Clock::now() + std::chrono::seconds(10);
    g_state.next_ping = Clock::now() + std::chrono::seconds(1);
    g_state.next_reconnect = Clock::now() + std::chrono::seconds(1);
    g_state.reconnect_backoff = std::chrono::seconds(1);
    g_state.snapshot_interest_radius = static_cast<float>(environment_uint(
        "KRAKEN_MP_INTEREST_RADIUS", 500, 25, 100000));
    g_state.next_snapshot = Clock::now();
    g_state.next_loadout = Clock::now();
    g_state.local_weapon_target_obj_id = kInvalidObjId;
    g_state.local_weapon_target_entity_id = kInvalidNetId;
    g_state.host_defeat_session_end_pending = false;
    g_state.session_end_preserve_client_scene = false;
    g_state.last_local_loadout_revision = 0;
    reset_impact_damage_state();
    LOG_INFO("session began role=%s endpoint=%s:%u", effective.host ? "host" : "client",
             effective.host ? "0.0.0.0" : effective.address.c_str(), effective.port);
    notify_lua_session_state(true, effective.host);
    return true;
}

bool ConfigureSession(bool host, const char* address, unsigned short port,
                      unsigned int max_peers)
{
    if (!g_lifecycle_config || IsSessionActive() || address == nullptr ||
        address[0] == '\0' || port < 1024 || max_peers < 2 || max_peers > 16)
        return false;
    constexpr std::size_t kMaxLanAddressLength = 255;
    if (std::char_traits<char>::length(address) > kMaxLanAddressLength)
        return false;
    g_lifecycle_config->host = host;
    g_lifecycle_config->address = address;
    g_lifecycle_config->port = port;
    g_lifecycle_config->max_peers = max_peers;
    g_lifecycle_config->auto_lan = false;
    LOG_INFO("LAN session configured role=%s endpoint=%s:%u max_peers=%u",
             host ? "host" : "client", address, port, max_peers);
    return true;
}

bool SubmitLocalWeaponCommand(int gun_id, bool trigger_held)
{
    if (g_state.is_host || !g_state.session ||
        g_state.local_entity_id == kInvalidNetId || g_state.peers.empty())
        return false;
    WeaponCommand command{};
    command.entity_id = g_state.local_entity_id;
    command.sequence = g_state.next_weapon_sequence++;
    command.shot_id = command.sequence;
    command.client_tick = g_state.server_tick;
    command.gun_id = gun_id;
    command.trigger_held = trigger_held;
    if (!trigger_held)
        g_state.local_weapon_target_entity_id = kInvalidNetId;
    command.target_entity_id = g_state.local_weapon_target_entity_id;
    hta::ai::Vehicle* const vehicle = current_local_weapon_vehicle_from_player(
        "SubmitLocalWeaponCommand");
    if (vehicle != nullptr) {
        if (!capture_weapon_aim_point(*vehicle, command.aim_point))
            LOG_ERROR("local weapon aim capture failed entity=%u",
                      command.entity_id);
        else
            command.has_aim_point = true;
    } else {
        LOG_ERROR("local weapon aim capture has no player vehicle entity=%u",
                  command.entity_id);
    }
    std::array<Byte, kWeaponCommandWireSize> payload{};
    const WeaponCommandCodecError encoded = encode_weapon_command(command, payload);
    if (!weapon_command_codec_succeeded(encoded)) {
        LOG_ERROR("local weapon command encode failed entity=%u shot=%u code=%u",
                  command.entity_id, command.shot_id,
                  static_cast<unsigned>(encoded));
        return false;
    }
    const TransportResult sent = g_state.session->send(
        g_state.peers.front(), MessageType::WeaponCommand, Channel::Reliable,
        payload);
    if (!sent)
        LOG_ERROR("local weapon command send failed entity=%u shot=%u code=%u",
                  command.entity_id, command.shot_id,
                  static_cast<unsigned>(sent.code));
    return static_cast<bool>(sent);
}

bool submit_native_weapon_intent(hta::ai::Vehicle* const vehicle,
                                 bool trigger_held, const ObjId target_obj_id,
                                 const std::int32_t gun_id,
                                 const VehicleVector3* aim_override,
                                 float aim_speed)
{
    if (g_state.is_host || !g_state.session ||
        g_state.local_entity_id == kInvalidNetId || g_state.peers.empty())
        return false;
    hta::ai::Vehicle* const current_vehicle = current_local_weapon_vehicle(
        vehicle, "submit native weapon intent");
    if (current_vehicle == nullptr)
        return false;

    // target_obj_id is only the caller's cached observation.  Resolve the
    // semantic current target at this serialization boundary so a zero or
    // stale cached value cannot suppress a valid EFA target or retain one.
    const NetId target_entity_id = resolve_local_native_weapon_target(
        current_vehicle);
    if (target_obj_id != g_state.local_weapon_target_obj_id) {
        LOG_DEBUG("native weapon target cache objId=%d differs from current semantic objId=%d",
                  target_obj_id, g_state.local_weapon_target_obj_id);
    }
    WeaponCommand command{};
    command.entity_id = g_state.local_entity_id;
    command.sequence = g_state.next_weapon_sequence++;
    command.shot_id = command.sequence;
    command.client_tick = g_state.server_tick;
    command.gun_id = gun_id >= 0 && gun_id <= kMaxNetworkGunId ? gun_id : 0;
    command.trigger_held = trigger_held;
    command.target_entity_id = target_entity_id;
    command.aim_speed = aim_speed;
    if (aim_override != nullptr && valid_weapon_aim_point(*aim_override)) {
        command.aim_point = *aim_override;
        command.has_aim_point = true;
    } else if (!capture_weapon_aim_point(*current_vehicle, command.aim_point))
        LOG_ERROR("native weapon aim capture failed entity=%u shot=%u",
                  command.entity_id, command.shot_id);
    else
        command.has_aim_point = true;
    std::array<Byte, kWeaponCommandWireSize> payload{};
    const WeaponCommandCodecError encoded = encode_weapon_command(command, payload);
    if (!weapon_command_codec_succeeded(encoded)) {
        LOG_ERROR("native weapon command encode failed entity=%u shot=%u code=%u",
                  command.entity_id, command.shot_id,
                  static_cast<unsigned>(encoded));
        return false;
    }
    const TransportResult sent = g_state.session->send(
        g_state.peers.front(), MessageType::WeaponCommand,
        Channel::Reliable, payload);
    if (!sent) {
        LOG_ERROR("native weapon command send failed entity=%u shot=%u code=%u",
                  command.entity_id, command.shot_id,
                  static_cast<unsigned>(sent.code));
        return false;
    }
    LOG_INFO("native weapon intent entity=%u shot=%u gun=%d target=%u aim=%u",
             command.entity_id, command.shot_id, command.gun_id,
             command.target_entity_id, command.has_aim_point ? 1u : 0u);
    return true;
}

bool RequestLocalLoot(LootId loot_id, LootTransactionId transaction_id,
                      std::uint32_t amount)
{
    const WorldLootRecord* const record = g_state.world_loot.find(loot_id);
    if (record == nullptr)
        return false;
    return RequestWorldLootPickup(loot_id, record->generation, transaction_id,
                                  amount);
}

bool RequestWorldLootPickup(const WorldLootId loot_id,
                            const WorldLootGeneration generation,
                            const std::uint32_t transaction_id,
                            const std::uint32_t amount)
{
    if (g_state.is_host || !g_state.session || g_state.session_epoch == 0 ||
        g_state.local_entity_id == kInvalidNetId || g_state.peers.empty())
        return false;
    WorldLootPickupRequest request{};
    request.session_epoch = g_state.session_epoch;
    request.entity_id = g_state.local_entity_id;
    request.loot_id = loot_id;
    request.generation = generation;
    request.transaction_id = transaction_id;
    request.amount = amount;
    std::array<Byte, kWorldLootPickupRequestWireSize> payload{};
    const WorldLootCodecError encoded = encode_world_loot_pickup_request(
        request, payload);
    if (!world_loot_codec_succeeded(encoded)) {
        LOG_ERROR("world loot pickup encode failed loot=%u generation=%u code=%u",
                  loot_id, static_cast<unsigned>(generation),
                  static_cast<unsigned>(encoded));
        return false;
    }
    const TransportResult sent = g_state.session->send(
        g_state.peers.front(), MessageType::WorldLootPickupRequest,
        Channel::Reliable, payload);
    if (!sent)
        LOG_ERROR("world loot pickup send failed loot=%u generation=%u code=%u",
                  loot_id, static_cast<unsigned>(generation),
                  static_cast<unsigned>(sent.code));
    return static_cast<bool>(sent);
}

WorldLootId PublishHostWorldLoot(
    const std::int32_t chest_prototype_id,
    const std::int32_t item_prototype_id, const std::uint32_t amount,
    const NetId owner_entity_id)
{
    if (!IsHost() || amount == 0 || chest_prototype_id < 0 ||
        item_prototype_id < 0)
        return 0;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (server == nullptr || server->m_pObjects == nullptr || vehicle == nullptr)
        return 0;
    const LootId loot_id = allocate_loot_id();
    if (loot_id == 0)
        return 0;
    char name[48]{};
    std::snprintf(name, sizeof(name), "kraken_loot_%u", loot_id);
    const ObjId object_id = server->m_pObjects->CreateNewObject(
        chest_prototype_id, name, -1, -1);
    if (object_id < 0) return 0;
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id);
    hta::ai::Chest* const chest = chest_from_object(object);
    if (chest == nullptr || chest->GetRepository() == nullptr ||
        !chest->GetRepository()->AddItems(item_prototype_id, static_cast<int32_t>(amount)))
        return 0;
    hta::CVector position = vehicle->GetPosition();
    position.x += 4.0f;
    chest->SetPositionSelf(position);
    WorldLootRecord world{};
    world.session_epoch = g_state.session_epoch;
    world.loot_id = loot_id;
    world.generation = 1;
    world.revision = ++g_state.world_loot_revision;
    world.container_id = loot_id;
    world.container_prototype_id = chest_prototype_id;
    world.owner_entity_id = owner_entity_id;
    world.transform.position_x = position.x;
    world.transform.position_y = position.y;
    world.transform.position_z = position.z;
    const hta::Quaternion rotation = chest->GetRotation();
    world.transform.rotation_x = rotation.x;
    world.transform.rotation_y = rotation.y;
    world.transform.rotation_z = rotation.z;
    world.transform.rotation_w = rotation.w;
    world.item_prototype_id = item_prototype_id;
    world.amount = amount;

    for (std::uint32_t slot = 0; slot < chest->GetRepository()->GetNumItems();
         ++slot) {
        const hta::ai::GeomRepositoryItem item =
            chest->GetRepository()->GetItem(static_cast<int32_t>(slot));
        if (item.GetPrototypeId() != item_prototype_id || item.GetAmount() == 0)
            continue;
        world.item_instance_id = item.GetObjId();
        if (hta::ai::Obj* const item_object = item.GetObj(); item_object != nullptr &&
            (!item_object->m_appliedPrefixIds.empty() ||
             !item_object->m_appliedSuffixIds.empty() ||
             !item_object->m_modifiers.empty())) {
            LOG_WARNING("world loot item affix fields unsupported loot=%u prototype=%d instance=%d; prefixes/suffixes/modifiers are not replicated",
                        loot_id, item_prototype_id, world.item_instance_id);
        }
        break;
    }
    // Resource items often expose no portable instance object.  Keep the
    // sentinel explicit and visible in logs instead of fabricating affixes or
    // an instance identity that another process cannot resolve.
    LOG_WARNING("world loot item affix fields unsupported loot=%u prototype=%d instance=%d; no affix payload is sent",
                loot_id, item_prototype_id, world.item_instance_id);

    g_state.loot_records.push_back(LootRecord{world, object_id});
    (void)g_state.world_loot.apply_spawn(WorldLootSpawn{world});
    for (const PeerId peer : g_state.peers)
        send_world_loot_spawn(peer, world);
    LOG_INFO("world loot spawned id=%u generation=%u epoch=%u objId=%d containerPrototype=%d itemPrototype=%d itemInstance=%d amount=%u",
             loot_id, static_cast<unsigned>(world.generation),
             world.session_epoch, object_id, chest_prototype_id,
             item_prototype_id, world.item_instance_id, amount);
    return loot_id;
}

WorldLootId PublishHostWorldLootObject(
    const std::int32_t object_id, const std::int32_t item_prototype_id,
    const std::uint32_t amount, const NetId owner_entity_id)
{
    if (!IsHost()) {
        LOG_ERROR("reject world loot object publish while not host/session objId=%d itemPrototype=%d amount=%u",
                  object_id, item_prototype_id, amount);
        return 0;
    }
    if (object_id <= 0 || item_prototype_id < 0 || amount == 0 ||
        amount > static_cast<std::uint32_t>(
                     (std::numeric_limits<std::int32_t>::max)())) {
        LOG_ERROR("reject world loot object publish signed/range objId=%d itemPrototype=%d amount=%u",
                  object_id, item_prototype_id, amount);
        return 0;
    }

    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr) {
        LOG_ERROR("reject world loot object publish without object container objId=%d",
                  object_id);
        return 0;
    }
    hta::ai::Obj* const object =
        server->m_pObjects->GetEntityByObjId(object_id);
    if (object == nullptr) {
        tombstone_loot_source(object_id, "unresolved during publication");
        LOG_ERROR("reject world loot object publish unresolved objId=%d",
                  object_id);
        return 0;
    }
    if (object->GetId() != object_id || object->GetDeletedStatus()) {
        tombstone_loot_source(object_id, "deleted during publication");
        LOG_ERROR("reject world loot object publish unresolved/deleted objId=%d",
                  object_id);
        return 0;
    }
    hta::ai::Chest* const chest = chest_from_object(object);
    if (chest == nullptr || chest->GetRepository() == nullptr) {
        tombstone_loot_source(object_id,
                              "incompatible during publication");
        LOG_ERROR("reject world loot object publish incompatible object objId=%d",
                  object_id);
        return 0;
    }

    const std::int32_t container_prototype_id = object->GetPrototypeId();
    if (container_prototype_id < 0) {
        tombstone_loot_source(object_id,
                              "invalid prototype during publication");
        LOG_ERROR("reject world loot object publish invalid actual prototype objId=%d prototype=%d",
                  object_id, container_prototype_id);
        return 0;
    }

    const hta::CVector position = chest->GetPosition();
    const hta::Quaternion rotation = chest->GetRotation();
    const float rotation_norm = rotation.x * rotation.x +
                                rotation.y * rotation.y +
                                rotation.z * rotation.z +
                                rotation.w * rotation.w;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
        !std::isfinite(rotation.w) || !std::isfinite(rotation_norm) ||
        std::abs(rotation_norm - 1.0f) > 0.02f) {
        LOG_ERROR("reject world loot object publish invalid transform objId=%d prototype=%d",
                  object_id, container_prototype_id);
        return 0;
    }

    const bool had_source = find_loot_source(object_id) != nullptr;
    LootSourceBinding* const source = bind_loot_source(
        object_id, object, container_prototype_id);
    if (source == nullptr)
        return 0;

    bool conflicting = false;
    if (LootRecord* const existing = find_object_publication(
            object_id, source->generation, item_prototype_id, amount,
            owner_entity_id, conflicting); existing != nullptr) {
        LOG_INFO("world loot object publication duplicate objId=%d generation=%u loot=%u itemPrototype=%d amount=%u",
                 object_id, static_cast<unsigned>(source->generation),
                 existing->world.loot_id, item_prototype_id, amount);
        return existing->world.loot_id;
    } else if (conflicting) {
        LOG_ERROR("reject world loot object conflicting duplicate objId=%d generation=%u itemPrototype=%d amount=%u",
                  object_id, static_cast<unsigned>(source->generation),
                  item_prototype_id, amount);
        return 0;
    }

    const LootId loot_id = allocate_loot_id();
    const auto rollback_new_source = [&]() {
        if (!had_source && !g_state.loot_sources.empty() &&
            g_state.loot_sources.back().object_id == object_id)
            g_state.loot_sources.pop_back();
    };
    if (loot_id == 0) {
        rollback_new_source();
        return 0;
    }
    hta::ai::GeomRepository* const repository = chest->GetRepository();
    if (!repository->CanPlaceItems(item_prototype_id,
                                   static_cast<std::int32_t>(amount))) {
        LOG_ERROR("reject world loot object source capacity objId=%d generation=%u itemPrototype=%d amount=%u",
                  object_id, static_cast<unsigned>(source->generation),
                  item_prototype_id, amount);
        rollback_new_source();
        return 0;
    }

    const WorldLootGeneration source_generation = source->generation;
    const WorldLootId source_container_id = source->container_id;
    const WorldLootRevision previous_revision = g_state.world_loot_revision;
    const WorldLootRevision publication_revision = previous_revision + 1;
    if (publication_revision == 0) {
        LOG_ERROR("reject world loot object revision overflow objId=%d generation=%u",
                  object_id, static_cast<unsigned>(source_generation));
        rollback_new_source();
        return 0;
    }

    WorldLootRecord world{};
    world.session_epoch = g_state.session_epoch;
    world.loot_id = loot_id;
    world.generation = source_generation;
    world.revision = publication_revision;
    world.container_id = source_container_id;
    world.container_prototype_id = container_prototype_id;
    world.owner_entity_id = owner_entity_id;
    world.transform.position_x = position.x;
    world.transform.position_y = position.y;
    world.transform.position_z = position.z;
    world.transform.rotation_x = rotation.x;
    world.transform.rotation_y = rotation.y;
    world.transform.rotation_z = rotation.z;
    world.transform.rotation_w = rotation.w;
    world.item_prototype_id = item_prototype_id;
    world.amount = amount;

    // Capture only metadata already present before publication. Newly inserted
    // resource stacks have no portable cross-process instance identity.
    for (std::uint32_t slot = 0; slot < repository->GetNumItems(); ++slot) {
        const hta::ai::GeomRepositoryItem item =
            repository->GetItem(static_cast<std::int32_t>(slot));
        if (item.GetPrototypeId() != item_prototype_id ||
            item.GetAmount() == 0)
            continue;
        world.item_instance_id = item.GetObjId() >= -1 ? item.GetObjId() : -1;
        if (hta::ai::Obj* const item_object = item.GetObj();
            item_object != nullptr &&
            (!item_object->m_appliedPrefixIds.empty() ||
             !item_object->m_appliedSuffixIds.empty() ||
             !item_object->m_modifiers.empty())) {
            LOG_WARNING("world loot item affix fields unsupported loot=%u prototype=%d instance=%d; prefixes/suffixes/modifiers are not replicated",
                        loot_id, item_prototype_id, world.item_instance_id);
        }
        break;
    }
    LOG_WARNING("world loot item affix fields unsupported loot=%u prototype=%d instance=%d; no affix payload is sent",
                loot_id, item_prototype_id, world.item_instance_id);

    std::array<Byte, kWorldLootSpawnWireSize> publication_payload{};
    const WorldLootCodecError encoded = encode_world_loot_spawn(
        WorldLootSpawn{world}, publication_payload);
    if (!world_loot_codec_succeeded(encoded)) {
        LOG_ERROR("reject world loot object publication encode objId=%d generation=%u loot=%u code=%u",
                  object_id, static_cast<unsigned>(source_generation),
                  loot_id, static_cast<unsigned>(encoded));
        rollback_new_source();
        return 0;
    }

    std::optional<WorldLootReplica> previous_world_loot;
    bool record_registered = false;
    try {
        previous_world_loot.emplace(g_state.world_loot);
        g_state.loot_records.push_back(LootRecord{
            world, object_id, source_generation, amount, true});
        record_registered = true;
        const WorldLootApplyResult applied =
            g_state.world_loot.apply_spawn(WorldLootSpawn{world});
        if (applied != WorldLootApplyResult::Applied) {
            g_state.loot_records.pop_back();
            record_registered = false;
            g_state.world_loot = std::move(*previous_world_loot);
            LOG_ERROR("reject world loot object registry mutation objId=%d generation=%u loot=%u apply=%u",
                      object_id, static_cast<unsigned>(source_generation),
                      loot_id, static_cast<unsigned>(applied));
            rollback_new_source();
            return 0;
        }
        g_state.world_loot_revision = publication_revision;
    }
    catch (...) {
        if (record_registered && !g_state.loot_records.empty() &&
            g_state.loot_records.back().world.loot_id == loot_id)
            g_state.loot_records.pop_back();
        if (previous_world_loot)
            g_state.world_loot = std::move(*previous_world_loot);
        g_state.world_loot_revision = previous_revision;
        rollback_new_source();
        LOG_ERROR("reject world loot object registry allocation objId=%d generation=%u loot=%u",
                  object_id, static_cast<unsigned>(source_generation), loot_id);
        return 0;
    }

    hta::ai::Obj* const current_object =
        server->m_pObjects->GetEntityByObjId(object_id);
    if (current_object != object || current_object == nullptr ||
        current_object->GetDeletedStatus() ||
        current_object->GetPrototypeId() != container_prototype_id) {
        if (!g_state.loot_records.empty() &&
            g_state.loot_records.back().world.loot_id == loot_id)
            g_state.loot_records.pop_back();
        else
            std::erase_if(g_state.loot_records,
                [loot_id](const LootRecord& record) {
                    return record.world.loot_id == loot_id;
                });
        g_state.world_loot = std::move(*previous_world_loot);
        g_state.world_loot_revision = previous_revision;
        tombstone_loot_source(object_id,
                              "identity changed before repository mutation");
        LOG_ERROR("reject world loot object identity changed before AddItems objId=%d generation=%u loot=%u",
                  object_id, static_cast<unsigned>(source_generation), loot_id);
        return 0;
    }

    if (!repository->AddItems(item_prototype_id,
                              static_cast<std::int32_t>(amount))) {
        if (!g_state.loot_records.empty() &&
            g_state.loot_records.back().world.loot_id == loot_id)
            g_state.loot_records.pop_back();
        else
            std::erase_if(g_state.loot_records,
                [loot_id](const LootRecord& record) {
                    return record.world.loot_id == loot_id;
                });
        g_state.world_loot = std::move(*previous_world_loot);
        g_state.world_loot_revision = previous_revision;
        rollback_new_source();
        LOG_ERROR("reject world loot object source AddItems failed after state prepublication objId=%d generation=%u loot=%u itemPrototype=%d amount=%u; state rolled back",
                  object_id, static_cast<unsigned>(source_generation), loot_id,
                  item_prototype_id, amount);
        return 0;
    }

    std::uint32_t broadcast_failures = 0;
    for (const PeerId peer : g_state.peers)
        if (!send_world_loot_spawn(peer, world))
            ++broadcast_failures;
    if (broadcast_failures != 0)
        LOG_ERROR("world loot object publication retained for baseline recovery loot=%u broadcastFailures=%u",
                  loot_id, broadcast_failures);
    LOG_INFO("world loot object published id=%u generation=%u epoch=%u objId=%d container=%u containerPrototype=%d itemPrototype=%d itemInstance=%d amount=%u",
             loot_id, static_cast<unsigned>(world.generation),
             world.session_epoch, object_id, world.container_id,
             world.container_prototype_id, world.item_prototype_id,
             world.item_instance_id, world.amount);
    return loot_id;
}

LootId SpawnHostLoot(const std::int32_t chest_prototype_id,
                     const std::int32_t item_prototype_id,
                     const std::uint32_t amount)
{
    return PublishHostWorldLoot(chest_prototype_id, item_prototype_id, amount,
                                 kInvalidNetId);
}

bool QueryWorldLootAuthority()
{
    return IsHost();
}

} // namespace kraken::net::runtime
