#define LOGGER "multiplayer"

#include "net/runtime.hpp"

#include "config.hpp"
#include "ext/runtime.hpp"
#include "ext/logger.hpp"
#include "net/entity_protocol.hpp"
#include "net/entity_registry.hpp"
#include "net/combat_presentation.hpp"
#include "net/combat_runtime.hpp"
#include "net/host_world_registry.hpp"
#include "net/input_command.hpp"
#include "net/impact_damage.hpp"
#include "net/lan_discovery.hpp"
#include "net/loadout_protocol.hpp"
#include "net/loot_transaction.hpp"
#include "net/match_protocol.hpp"
#include "net/match_session.hpp"
#include "net/native_object_archive.hpp"
#include "net/pause_policy.hpp"
#include "net/player_slots.hpp"
#include "net/resource_fingerprint.hpp"
#include "net/session.hpp"
#include "net/spawn_attempt.hpp"
#include "net/snapshot_interpolation.hpp"
#include "net/static_world_identity.hpp"
#include "net/transport.hpp"
#include "net/vehicle_descriptor.hpp"
#include "net/vehicle_archive_validation.hpp"
#include "net/vehicle_snapshot.hpp"
#include "net/vehicle_transfer.hpp"
#include "net/weapon_command.hpp"
#include "net/world_loot.hpp"
#include "net/world_mutation_protocol.hpp"
#include "net/world_mutation_applier.hpp"
#include "net/world_observer.hpp"
#include "net/world_replication.hpp"
#include "net/world_state_snapshot.hpp"
#include "net/quest_state_projection.hpp"
#include "routines.hpp"

#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/CMiracle3d.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/Player.hpp"
#include "hta/ai/ProcessManager.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/Chest.hpp"
#include "hta/ai/CompoundVehiclePart.hpp"
#include "hta/ai/DamageInfo.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/GeomRepository.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Location.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/VehiclePart.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/native.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/Level.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/m3d/SgNode.hpp"
#include "hta/m3d/SgSoundSourceNode.hpp"
#include "hta/m3d/ScriptServer.hpp"
#include "hta/m3d/cmn/XmlFile.hpp"
#include "hta/m3d/cmn/XmlNode.hpp"
#include "hta/ref_ptr.hpp"
#include "hta/ai/QuestStateManager.hpp"
#include "hta/ai/Trigger.hpp"
#include "hta/ai/DynamicQuest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <limits>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
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
class RuntimeCombatBridge;

// Runtime-owned integration boundary.  The protocol/adapter module remains
// independent of the engine registry; this bridge supplies the proven native
// bindings and owns their lifecycle at session, despawn, and pre-simulation
// boundaries.
class RuntimeCombatBridge final {
public:
    RuntimeCombatBridge();
    ~RuntimeCombatBridge();

    RuntimeCombatBridge(const RuntimeCombatBridge&) = delete;
    RuntimeCombatBridge& operator=(const RuntimeCombatBridge&) = delete;

    [[nodiscard]] bool install_host_hooks();
    [[nodiscard]] bool request_host_wreck(const DamageResult&);
    void process_host_wreck_candidates();
    [[nodiscard]] combat_runtime::RuntimeApplyResult apply_impact(
        const ImpactPresentation&);
    [[nodiscard]] combat_runtime::RuntimeApplyResult apply_damage(
        const DamageResult&);
    [[nodiscard]] combat_runtime::RuntimeApplyResult apply_death(
        const DeathWreckPresentation&);
    [[nodiscard]] bool accept_wreck_archive_chunk(ByteView,
                                                  std::uint64_t now_ms);
    [[nodiscard]] combat_runtime::RuntimeApplyResult apply_horn(
        const HornState&);
    [[nodiscard]] combat_runtime::RuntimeApplyResult apply_jip(
        const PresentationJipState&);
    [[nodiscard]] combat_runtime::RuntimeApplyResult process_removals(
        bool pre_sim_boundary);
    void despawn(const NetEntityRef&) noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

bool RequestLocalLoot(LootId loot_id, LootTransactionId transaction_id,
                      std::uint32_t amount);
LootId SpawnHostLoot(std::int32_t chest_prototype_id, std::int32_t resource_id,
                     std::uint32_t amount);
bool BeginSession();
bool EndSession();
bool end_session_with_reason(SessionLeaveReason reason);
bool end_session_teardown();
bool IsSessionActive();
void notify_lua_match_exit(const std::string&, SessionLeaveReason);
bool submit_native_weapon_intent(::hta::ai::Vehicle* vehicle, bool trigger_held,
                                 ObjId target_obj_id, std::int32_t gun_id = 0,
                                 const VehicleVector3* aim_override = nullptr,
                                 float aim_speed = 1.0f,
                                 const GunAttachmentIdentity* gun_override = nullptr);
namespace {

constexpr uintptr_t kServerUpdateCallSite = 0x005C809D;
constexpr uintptr_t kServerUpdateAddress = 0x005F4090;
// ObjContainer::Update has one direct QuestStateManager::Update call at
// 0x00632F60.  Patch that exact E8 site; ProcessManager::Update is deliberately
// not intercepted.
constexpr uintptr_t kQuestUpdateCallSite = 0x00632F60;
constexpr uintptr_t kQuestUpdateAddress = 0x006A0200;
// ObjContainer::Update dispatches object updates virtually at 0x00632F1C.
// The Trigger vtable's verified Update slot is 0x80 in the primary table at
// 0x009B6728, whose entry is the function at RVA 0x4569A0.
constexpr uintptr_t kTriggerVtableAddress = 0x009B6728;
// LoRA: ai::Trigger::OnEvent is vtable byte offset 0x40, target VA 0x00858990.
constexpr std::size_t kTriggerOnEventVtableOffset = 0x40;
constexpr uintptr_t kTriggerOnEventAddress = 0x00858990;
constexpr std::size_t kTriggerUpdateVtableOffset = 0x80;
constexpr uintptr_t kTriggerUpdateAddress = 0x008569A0;
// CServer::Load has two verified direct calls to LoadTriggersFromXML. The
// distinct wrappers preserve XML provenance without treating runtime action
// fields as source metadata.
constexpr uintptr_t kNormalTriggerLoadCallSite = 0x005F62AF;
constexpr uintptr_t kCinematicTriggerLoadCallSite = 0x005F62E2;
constexpr uintptr_t kLoadTriggersFromXmlAddress = 0x005F2D90;
// LoadTriggersFromXML resolves each parsed Name through this exact direct
// call before deciding whether to create a Trigger. Hooking it observes
// duplicates that the engine rejects without creating an object.
constexpr uintptr_t kTriggerNameLookupCallSite = 0x005F324B;
constexpr uintptr_t kGetEntityByObjNameAddress = 0x00630470;
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
using TriggerUpdateFn = void(__thiscall*)(hta::ai::Trigger*, float,
                                          std::uint32_t);
using TriggerOnEventFn = int(__thiscall*)(hta::ai::Trigger*,
                                          const hta::ai::Event&);
using LoadTriggersFromXmlFn = void(__thiscall*)(hta::ai::CServer*, hta::CStr*);
using GetEntityByObjNameFn = hta::ai::Obj*(__thiscall*)(
    hta::ai::ObjContainer*, hta::CStr*);
using QuestUpdateFn = void(__thiscall*)(hta::ai::QuestStateManager*, float);
using EvaluateToDeadFn = void(__thiscall*)(hta::ai::Vehicle*);
using PostMessageFn = void(__thiscall*)(hta::ai::ProcessManager*,
                                        std::int32_t, std::int32_t,
                                        std::int32_t, float, hta::m3d::AIParam,
                                        hta::m3d::AIParam, std::int32_t);

EvaluateToDeadFn g_evaluate_to_dead_original =
    reinterpret_cast<EvaluateToDeadFn>(combat_runtime::kVehicleEvaluateToDeadVa);
std::vector<hta::ai::Vehicle*>* g_evaluate_to_dead_candidates = nullptr;
PostMessageFn g_post_message_original = reinterpret_cast<PostMessageFn>(
    combat_runtime::kProcessManagerPostMessageVa);
thread_local bool g_wreck_post_load_scope = false;

void __fastcall wreck_post_message_callsite_hook(
    hta::ai::ProcessManager* manager, void*, std::int32_t event_id,
    std::int32_t recipient, std::int32_t sender, float timeout,
    hta::m3d::AIParam param1, hta::m3d::AIParam param2,
    std::int32_t frames)
{
    if (g_wreck_post_load_scope &&
        (event_id == 45 || event_id == 46 || event_id == 47))
        return;
    g_post_message_original(manager, event_id, recipient, sender, timeout,
                            param1, param2, frames);
}

bool install_wreck_post_load_suppression_callsites();

void __fastcall evaluate_to_dead_wreck_hook(hta::ai::Vehicle* vehicle, void*)
{
    // The recovered callsite has ECX=Vehicle* and the original is a void
    // __thiscall. The native transition is invoked exactly once; capture is
    // queued and never serializes from inside Vehicle::Update.
    g_evaluate_to_dead_original(vehicle);
    if (g_evaluate_to_dead_candidates != nullptr && vehicle != nullptr)
        g_evaluate_to_dead_candidates->push_back(vehicle);
}

constexpr std::uint64_t kInterpolationDelayMs = 100;
constexpr float kWeaponAimDistance = 1'000.0f;
constexpr std::uint16_t kLanDiscoveryPort = 27016;
constexpr auto kLanDiscoveryTimeout = std::chrono::milliseconds(1500);
constexpr std::size_t kMaxRemoteEntities = 1024;
constexpr std::size_t kMaxPendingImpactFx = 256;
// These names are an optional compatibility fallback for legacy maps only.
// Multiplayer no longer waits for them and dynamic vehicles are the normal
// path, including original maps that contain no MP_* XML objects.
constexpr std::size_t kLegacyNamedPlayerSlotCount = 4;
constexpr std::array<const char*, kLegacyNamedPlayerSlotCount> kPlayerSpawnNames{
    "MP_SPAWN_1", "MP_SPAWN_2", "MP_SPAWN_3", "MP_SPAWN_4"};
constexpr std::array<const char*, kLegacyNamedPlayerSlotCount> kPlayerProxyNames{
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
    WeaponAimState weapon_aim{};
    bool has_weapon_aim = false;
    WeaponTriggerState weapon_trigger{};
    bool has_weapon_trigger = false;
    ShotConfirmed last_shot{};
    bool has_last_shot = false;
    LoadoutProfile loadout{};
    bool has_loadout = false;
    std::uint32_t applied_loadout_revision = 0;
    VehicleDescriptor descriptor{};
    bool has_descriptor = false;
    bool retired = false;
    ObjId wreck_object_id = kInvalidObjId;
    bool inert_wreck = false;
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
    WeaponAimState weapon_aim{};
    bool has_weapon_aim = false;
    WeaponTriggerState weapon_trigger{};
    bool has_weapon_trigger = false;
    CombatTransitionId next_weapon_transition = 1;
    CombatTransitionId next_horn_transition = 1;
    bool last_horn_state = false;
    bool have_horn_state = false;
    bool unstuck_was_requested = false;
    LoadoutProfile loadout{};
    bool has_loadout = false;
    std::uint32_t applied_loadout_revision = 0;
    std::uint32_t deferred_loadout_revision = 0;
    SpawnAttemptState spawn_attempt;
    bool host_vehicle_active = false;
    bool shared_spawn_applied = false;
    VehicleDescriptor descriptor{};
    bool has_descriptor = false;
    bool descriptor_fallback_logged = false;
};

struct HostWreckArchive {
    std::uint64_t archive_id = 0;
    std::uint32_t revision = 0;
    NativeObjectArchiveDigest digest = 0;
    std::vector<Byte> encoded{};
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

struct MatchJipPeerBarrier {
    PeerId peer = kInvalidPeer;
    MatchJipBarrier barrier{};
};

struct ClientJipMapLoad {
    PeerId host_peer = kInvalidPeer;
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    MatchPlayerId player_id = kInvalidMatchPlayerId;
    NetId entity_id = kInvalidNetId;
    std::string target_map;
    bool vehicle_descriptor_sent = false;
    bool map_ready_sent = false;
};

struct RuntimeState {
    EnetTransport transport;
    LanDiscovery lan_discovery;
    SessionIdentity session_identity{};
    bool lan_advertising = false;
    std::unique_ptr<Session> session;
    std::unique_ptr<RuntimeCombatBridge> combat_runtime;
    MatchCoordinator match;
    HostWorldRegistry host_world_registry;
    WorldJournal world_journal;
    WorldStateSnapshot world_snapshot{};
    std::vector<Byte> world_snapshot_payload;
    WorldJoinBarrier world_join_barrier;
    QuestProjectionHost quest_projection_host;
    QuestProjectionClient quest_projection_client;
    QuestTriggerProvenanceRegistry quest_trigger_provenance;
    std::string quest_projection_map_namespace;
    bool quest_projection_sample_ready = false;
    std::unique_ptr<WorldObserver> world_observer;
    std::unique_ptr<WorldMutationApplier> world_mutation_applier;
    ReplicationSourceContext world_source_context =
        ReplicationSourceContext::MapLoad;
    std::size_t world_replay_depth = 0;
    std::vector<WorldMutationEvent> queued_world_mutations;
    std::vector<PeerId> world_transfer_peers;
    std::vector<Byte> world_transfer_packets;
    bool world_transfer_expected = false;
    bool world_snapshot_committed = false;
    // Set at the first client roster/map-load boundary and held through
    // Loading, Synchronizing, and Playing. It is cleared only by match/session
    // teardown, so quest hooks cannot observe a native Loading allowance.
    bool quest_replica_active = false;
    bool quest_projection_committed = false;
    bool quest_play_pending = false;
    bool quest_local_state_logged = false;
    bool world_sync_request_received = false;
    bool world_sync_sent = false;
    PeerId world_sync_peer = kInvalidPeer;
    WorldEpoch world_join_epoch = kInvalidWorldEpoch;
    WorldRevision world_join_revision = kInvalidWorldRevision;
    std::vector<WorldDeltaTransfer> world_join_pending_deltas;
    std::vector<std::pair<MessageType, std::vector<Byte>>> world_join_packets;
    std::vector<NetId> expected_vehicle_descriptors;
    std::vector<NetId> received_vehicle_descriptors;
    std::vector<std::pair<PeerId, WorldRevision>> world_ready_acks;
    std::optional<MatchSpawn> local_assigned_spawn;
    bool local_assigned_spawn_applied = false;
    // Static-world IDs are derived from the active original scene key (namespace,
    // hierarchical dynamicscene path, prototype), never from ObjId, an
    // address, or container enumeration order.  Dynamic entities must come
    // from EntityRegistry/typed lifecycle records and remain outside this
    // index (zero means unmatched/dynamic).
    StaticWorldIdentityIndex static_world_index;
    StaticWorldMembershipStabilityGate static_world_stability;
    std::vector<StaticWorldPostLoadRecord> static_world_post_load_records;
    std::vector<StaticWorldId> static_world_ids_by_post_load_index;
    std::vector<std::pair<StaticWorldId, HostWorldEngineHandle>>
        static_world_original_bindings;
    bool static_world_identity_stable = false;
    StaticWorldIndexError static_world_identity_last_error =
        StaticWorldIndexError::None;
    std::string static_world_identity_error_map;
    std::string static_world_loaded_map;
    bool static_world_source_error_logged = false;
    bool static_world_source_loaded = false;
    bool static_world_dynamic_identity_logged = false;
    MatchConfig match_request{};
    MatchState visible_match_state = MatchState::Offline;
    std::vector<MatchPlayerId> match_jip_pending;
    std::vector<MatchJipPeerBarrier> match_jip_map_barriers;
    MatchPlayerId local_match_player_id = kInvalidMatchPlayerId;
    bool client_jip_join_requested = false;
    std::optional<ClientJipMapLoad> client_jip_map_load;
    std::uint32_t match_epoch = 0;
    std::uint32_t match_roster_revision = 0;
    Clock::time_point match_request_started{};
    bool match_request_pending = false;
    bool match_loading_announced = false;
    bool diagnostic_accept_status_valid = false;
    MatchStatus diagnostic_accept_status{};
    bool diagnostic_gameplay_open_logged = false;
    bool diagnostic_first_input_logged = false;
    bool diagnostic_host_control_ready_logged = false;
    bool auto_host_coop = false;
    std::string auto_host_map;
    bool auto_host_transition_pending = false;
    enum class DeferredRoute : std::uint8_t { None, LoadMap, MainMenu };
    DeferredRoute deferred_route = DeferredRoute::None;
    std::string deferred_route_map;
    std::string deferred_route_exit_map;
    bool deferred_exit_marker_pending = false;
    DeferredRoute deferred_exit_confirmation = DeferredRoute::None;
    std::string deferred_exit_confirmation_map;
    bool deferred_exit_was_host = false;
    SessionLeaveReason deferred_exit_reason = SessionLeaveReason::User;
    bool local_descriptor_sent = false;
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
    std::vector<WeaponAimState> latest_weapon_aim_states;
    std::vector<WeaponTriggerState> latest_weapon_trigger_states;
    std::vector<HornState> latest_horn_states;
    std::vector<DeathWreckPresentation> latest_terminal_deaths;
    std::vector<HostWreckArchive> host_wreck_archives;
    CombatTransitionId next_local_horn_transition = 1;
    bool local_horn_state = false;
    bool have_local_horn_state = false;
    WeaponAimStateTracker weapon_aim_tracker;
    CombatEventDeduplicator confirmed_shot_deduplicator;
    PresentationJipReassembler presentation_jip_reassembler;
    std::vector<ShotConfirmed> pending_confirmed_shots;
    PresentationStateRevision presentation_state_revision = 1;
    bool combat_authority_failure_emitted = false;
    Clock::time_point next_combat_marker{};
    std::uint64_t client_blocked_fire_attempt_count = 0;
    std::uint64_t client_original_fire_call_count = 0;
    std::uint64_t client_blocked_damage_attempt_count = 0;
    std::uint64_t client_original_damage_call_count = 0;
    SnapshotInterpolationBuffer local_correction;
    NetId local_entity_id = kInvalidNetId;
    Clock::time_point next_input{};
    Clock::time_point next_loadout{};
    std::uint32_t next_input_sequence = 1;
    std::uint32_t next_weapon_sequence = 1;
    std::uint32_t presented_local_shot_id = 0;
    AmmoReloadState local_confirmed_reload_state = AmmoReloadState::Ready;
    bool has_local_confirmed_reload_state = false;
    Clock::time_point next_local_weapon_aim{};
    Clock::time_point next_local_weapon_fire{};
    VehicleVector3 local_weapon_aim{};
    float local_weapon_aim_speed = 1.0f;
    bool has_local_weapon_aim = false;
    WeaponCommand local_weapon_state{};
    bool has_local_weapon_state = false;
    bool local_weapon_trigger_held = false;
    std::int32_t local_weapon_gun_id = 0;
    GunAttachmentIdentity local_weapon_gun{};
    bool has_local_weapon_gun = false;
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
    bool trigger_update_hook_installed = false;
    bool trigger_on_event_hook_installed = false;
    bool normal_trigger_provenance_hook_installed = false;
    bool cinematic_trigger_provenance_hook_installed = false;
    bool trigger_name_lookup_hook_installed = false;
    bool quest_update_hook_installed = false;
    world_authority::WorldExecutionContext authority_log_context{};
    std::uint8_t authority_log_mask = 0;
    bool impact_damage_hook_installed = false;
    bool impact_damage_hook_error_logged = false;
    bool combat_host_hooks_installed = false;
    bool combat_host_hooks_error_logged = false;
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
    // Opt-in integration smoke only. It loads a real save and enters the
    // generic match API; it never injects NPCs or rewrites mod resources.
    bool raid_autotest_enabled = false;
    bool raid_autotest_bootstrap_installed = false;
    bool raid_autotest_bootstrap_attempted = false;
    bool raid_autotest_native_save_loaded = false;
    bool raid_autotest_matchmaking_attempted = false;
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
    bool combat_autotest_armed = false;
    bool combat_autotest_death_logged = false;
    Clock::time_point next_combat_host_survival{};
    // Set at an engine-safe boundary after the authoritative host vehicle has
    // reached zero health.  Ending the transport from inside InflictDamage
    // would mutate registries while the engine is still walking damage state.
    bool host_defeat_session_end_pending = false;
    bool client_leave_session_pending = false;
    bool client_join_failure_pending = false;
    // The client can learn that the host ended a raid while EFA is already
    // unloading its current map.  In that path Kraken must only stop network
    // ownership; touching Player slots or scene ghosts races the map unload.
    bool session_end_preserve_client_scene = false;
};

RuntimeState g_state;
std::uint32_t g_next_session_epoch = 1;
inline constexpr std::int32_t kNativeNoQuestObjectId = -1;

void emit_match_accept_marker()
{
    const MatchStatus status = GetSessionStatus();
    const bool changed = !g_state.diagnostic_accept_status_valid ||
        status.state != g_state.diagnostic_accept_status.state ||
        status.ready_players != g_state.diagnostic_accept_status.ready_players ||
        status.required_players != g_state.diagnostic_accept_status.required_players ||
        status.infinite_wait != g_state.diagnostic_accept_status.infinite_wait;
    if (!changed)
        return;
    g_state.diagnostic_accept_status = status;
    g_state.diagnostic_accept_status_valid = true;
    LOG_INFO("KRAKEN_MP_ACCEPT match state=%s ready=%u required=%u remaining_ms=%llu",
             to_string(status.state), static_cast<unsigned>(status.ready_players),
             static_cast<unsigned>(status.required_players),
             static_cast<unsigned long long>(status.remaining_wait_ms));
}

void emit_gameplay_open_marker()
{
    if (g_state.diagnostic_gameplay_open_logged)
        return;
    g_state.diagnostic_gameplay_open_logged = true;
    LOG_INFO("KRAKEN_MP_ACCEPT gameplay_open");
}

void emit_first_input_marker()
{
    if (!g_state.diagnostic_gameplay_open_logged)
        return;
    if (g_state.diagnostic_first_input_logged)
        return;
    g_state.diagnostic_first_input_logged = true;
    LOG_INFO("KRAKEN_MP_ACCEPT first_input");
}

const char* acceptance_role() noexcept
{
    return g_state.is_host ? "host" : "client";
}

const char* leave_reason_name(const SessionLeaveReason reason) noexcept
{
    switch (reason) {
    case SessionLeaveReason::User: return "user";
    case SessionLeaveReason::Death: return "death";
    case SessionLeaveReason::Extract: return "extract";
    case SessionLeaveReason::HostTerminated: return "host_terminated";
    case SessionLeaveReason::MapUnload: return "map_unload";
    }
    return "unknown";
}

const char* diagnostic_mutation_kind(const WorldMutationEvent& event) noexcept
{
    switch (mutation_kind(event)) {
    case WorldMutationKind::ObjectCreated: return "ObjectCreated";
    case WorldMutationKind::ObjectDespawned: return "ObjectDespawned";
    case WorldMutationKind::ParentChildAdded: return "ParentChildAdded";
    case WorldMutationKind::ParentChildRemoved: return "ParentChildRemoved";
    case WorldMutationKind::RuntimeChanged: return "RuntimeChanged";
    case WorldMutationKind::PropertyChanged: return "PropertyChanged";
    case WorldMutationKind::Damage: return "Damage";
    case WorldMutationKind::Destroyed: return "Destroyed";
    case WorldMutationKind::Fx: return "Fx";
    }
    return "Unknown";
}

HostObjectId diagnostic_mutation_object(const WorldMutationEvent& event) noexcept
{
    return std::visit([](const auto& value) -> HostObjectId {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (requires { value.object_id; })
            return value.object_id;
        else if constexpr (requires { value.target_id; })
            return value.target_id;
        else if constexpr (requires { value.child_id; })
            return value.child_id;
        else if constexpr (requires { value.parent_id; })
            return value.parent_id;
        else
            return kInvalidHostObjectId;
    }, event);
}

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
TriggerUpdateFn g_trigger_update_original =
    reinterpret_cast<TriggerUpdateFn>(kTriggerUpdateAddress);
TriggerOnEventFn g_trigger_on_event_original =
    reinterpret_cast<TriggerOnEventFn>(kTriggerOnEventAddress);
LoadTriggersFromXmlFn g_load_triggers_from_xml_original =
    reinterpret_cast<LoadTriggersFromXmlFn>(kLoadTriggersFromXmlAddress);
GetEntityByObjNameFn g_get_entity_by_obj_name_original =
    reinterpret_cast<GetEntityByObjNameFn>(kGetEntityByObjNameAddress);
QuestUpdateFn g_quest_update_original =
    reinterpret_cast<QuestUpdateFn>(kQuestUpdateAddress);

QuestReplicaPhase current_quest_replica_phase() noexcept
{
    switch (g_state.visible_match_state) {
    case MatchState::Loading: return QuestReplicaPhase::Loading;
    case MatchState::Synchronizing: return QuestReplicaPhase::Synchronizing;
    case MatchState::Playing: return QuestReplicaPhase::Playing;
    case MatchState::Leaving: return QuestReplicaPhase::Teardown;
    default: return QuestReplicaPhase::Offline;
    }
}

world_authority::WorldExecutionContext current_world_execution_context(
    const bool replay = false, const bool presentation = false) noexcept
{
    const bool session_active = g_state.session != nullptr &&
        g_state.session->running();
    return detail::derive_world_execution_context(
        session_active, g_state.is_host, g_state.match.state(),
        g_state.visible_match_state, replay, presentation);
}

void log_denied_authoritative_origin(
    const world_authority::WorldAction action, const char* const origin)
{
    const world_authority::WorldExecutionContext context =
        world_authority::current_context();
    if (context != g_state.authority_log_context) {
        g_state.authority_log_context = context;
        g_state.authority_log_mask = 0;
    }
    const std::uint8_t bit = static_cast<std::uint8_t>(1u <<
        static_cast<std::uint8_t>(action));
    if ((g_state.authority_log_mask & bit) != 0)
        return;
    g_state.authority_log_mask = static_cast<std::uint8_t>(
        g_state.authority_log_mask | bit);
    LOG_WARNING("world authority denied origin=%s phase=%u authority=%u action=%u",
                origin, static_cast<unsigned>(context.phase),
                static_cast<unsigned>(context.authority),
                static_cast<unsigned>(action));
}

bool send_weapon_intent(const WeaponCommand& command);
bool send_combat_payload(PeerId peer, MessageType type, Channel channel,
                         const std::vector<Byte>& payload);
void publish_host_weapon_aim(const WeaponAimState& state);
void publish_host_weapon_trigger(const WeaponTriggerState& state,
                                 bool transition);
void publish_host_shot_confirmed(const ShotConfirmed& state);
void publish_host_impact(const ImpactPresentation& state);
void publish_host_damage(const DamageResult& state);
void publish_host_death(const DeathWreckPresentation& state);
bool publish_host_wreck_archive(PeerId peer, const DeathWreckPresentation& state);
bool publish_host_wreck_spawn(PeerId peer, const DeathWreckPresentation& state);
void publish_host_horn(const HornState& state);
void receive_combat_weapon_aim(const SessionEvent& event);
void receive_combat_weapon_trigger(const SessionEvent& event);
void receive_combat_shot_confirmed(const SessionEvent& event);
void receive_combat_impact(const SessionEvent& event);
void receive_combat_damage(const SessionEvent& event);
void receive_combat_death(const SessionEvent& event);
void receive_combat_horn(const SessionEvent& event);
void receive_combat_presentation_jip(const SessionEvent& event);
bool send_presentation_jip_state(PeerId peer);
void accept_combat_weapon_aim(const WeaponAimState& state);
void accept_combat_weapon_trigger(const WeaponTriggerState& state);
void accept_combat_shot_confirmed(const ShotConfirmed& state);
bool bind_local_player_vehicle();
bool bind_host_player_vehicle();
bool initialize_player_slots();
bool activate_dynamic_player_bindings();
bool is_player_controlled_vehicle(const hta::ai::Vehicle& vehicle);
void run_raid_autotest_tick();
void run_combat_autotest_tick(float elapsed_time);
int __fastcall controls_hook(hta::CMiracle3d*, void*, float, float);
bool release_player_slot_entity(NetId entity_id,
                                EntityGeneration expected_generation,
                                const char* reason);
bool relay_impact_damage(const ImpactDamage& event);
void receive_impact_damage(const SessionEvent& event);
void apply_pending_impact_damage();
void reconcile_host_entities();
void publish_host_baseline_to_peer(PeerId peer);
void send_world_loot_baseline(PeerId peer);
void observe_authoritative_world();
void observe_authoritative_quest_state();
bool send_quest_snapshot(PeerId peer);
void receive_quest_snapshot(const SessionEvent& event);
void receive_quest_delta(const SessionEvent& event);
void maybe_commit_quest_projection();
bool apply_authoritative_quest_projection(
    std::span<const QuestProjectionRecord> previous,
    std::span<const QuestProjectionRecord> target);
bool send_world_snapshot_and_descriptors(PeerId peer);
void receive_world_snapshot(const SessionEvent& event);
void receive_world_delta(const SessionEvent& event);
void receive_vehicle_descriptor(const SessionEvent& event);
void receive_world_ready(const SessionEvent& event);
bool send_local_vehicle_descriptor(PeerId peer);
hta::ai::Vehicle* create_host_remote_vehicle(PeerController& controller);
void apply_deferred_route();
void confirm_deferred_exit_route(hta::CMiracle3d* application);
std::string current_level_name();
void maybe_send_jip_map_ready();
PeerController* find_controller(PeerId peer);
std::string static_world_map_namespace();
std::optional<StaticWorldPostLoadRecord> static_world_record_for_object(
    hta::ai::CServer&, hta::ai::Obj&, const std::string&);
void suppress_client_dynamic_entities();
void retire_network_vehicle(hta::ai::ObjContainer&, hta::ai::Obj&,
                            hta::ai::Vehicle&, bool, bool = false);
bool capture_vehicle_loadout(const hta::ai::Vehicle&, NetId,
                             EntityGeneration, LoadoutProfile&);
VehicleDescriptor make_vehicle_descriptor(hta::ai::Vehicle&,
                                          const LoadoutProfile*,
                                          const char* archive_object_name);
bool apply_vehicle_descriptor_to_inactive_vehicle(
    hta::ai::Vehicle&, const VehicleDescriptor&, const char*);
bool validate_vehicle_descriptor_structure(
    hta::ai::Vehicle&, const VehicleDescriptor&, const char*);
hta::ai::Vehicle* find_vehicle(NetId, EntityGeneration = 0);
hta::ai::Vehicle* ensure_host_vehicle(PeerController& controller);
RemoteEntity* find_or_add_remote(NetId);
AttachmentIdentity native_part_identity(const hta::ai::VehiclePart&);
hta::ai::VehiclePart* find_native_part_by_path(hta::ai::VehiclePart&,
                                                std::string_view);
hta::ai::VehiclePart* find_native_part_by_path(hta::ai::Vehicle&,
                                                std::string_view);
void receive_entity_spawn(const SessionEvent&);
void receive_remote_snapshot(const SessionEvent&);
void receive_loadout(const SessionEvent&);
void receive_world_loot_spawn(const SessionEvent&);
void receive_world_loot_baseline(const SessionEvent&);
void receive_world_loot_delta(const SessionEvent&);
void receive_world_loot_remove(const SessionEvent&);

void reset_impact_damage_state()
{
    g_state.next_impact_event_id = 1;
    g_state.impact_health_mismatch_diagnostics = 0;
    g_state.impact_damage_deduplicator.clear();
    g_state.pending_impact_damage.clear();
    g_state.pending_impact_fx.clear();
}

bool start_matchmaking_impl(std::uint8_t required_players,
                            const char* target_map, const char* exit_map,
                            std::int32_t wait_timeout_seconds,
                            bool friendly_fire);
void update_lan_advertisement();
bool add_match_spawn_impl(float x, float y, float z, float yaw,
                          std::int32_t belong);
void handle_match_message(const SessionEvent& event);
void tick_match(Clock::time_point now);
void reset_match_state() noexcept;

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
    if (index >= kLegacyNamedPlayerSlotCount) {
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
    if (index >= kLegacyNamedPlayerSlotCount)
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

// Archive restoration can materialize descendants that are not present in
// the root Vehicle's initial child map.  The runtime adapter below owns the
// bounded graph barrier; keep the call here so every suspended path uses the
// same PostLoad/visual finalization contract.
bool finalize_native_vehicle_graph(hta::ai::ObjContainer& objects,
                                   hta::ai::Vehicle& vehicle,
                                   const char* const context);

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
    // open. Native XML/archive restore and omitted resource stacks must finish
    // before this bounded barrier seals every descendant exactly once.
    if (!finalize_native_vehicle_graph(objects, *current, context))
        return false;
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
    for (PlayerSlotIndex index = 0;
         index < static_cast<PlayerSlotIndex>(kLegacyNamedPlayerSlotCount);
         ++index) {
        ResolvedPlayerSlot slot{};
        if (!resolve_player_slot(index, slot))
            continue;
        ++ready;
        if (slot.proxy_obj_id != current_player_obj_id)
            deactivate_player_slot_vehicle(slot);
    }
    LOG_INFO("MP legacy named player-slot fallback present=%u/%u",
             static_cast<unsigned>(ready),
             static_cast<unsigned>(kLegacyNamedPlayerSlotCount));
    // Named slots are optional.  Dynamic entity binding is authoritative for
    // every player, so an original map with no MP_* objects remains valid.
    return true;
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

bool activate_dynamic_player_bindings()
{
    if (g_state.is_host)
        return bind_host_player_vehicle();
    if (g_state.local_entity_id == kInvalidNetId)
        return true; // EntityAssign has not arrived yet.
    return bind_local_player_vehicle();
}

bool try_activate_player_slots()
{
    if (g_state.player_slots_ready)
        return true;
    (void)initialize_player_slots();
    if (!activate_dynamic_player_bindings())
        return false;
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

bool vehicle_part_contains_gun(hta::ai::VehiclePart& part,
                               const hta::ai::Gun* const gun)
{
    if (&part == gun)
        return true;
    if (!part.IsKindOf(hta::ai::CompoundVehiclePart::p_classObject))
        return false;
    const auto& compound =
        static_cast<const hta::ai::CompoundVehiclePart&>(part);
    for (auto iterator = compound.begin(); iterator != compound.end();
         ++iterator) {
        hta::ai::VehiclePart* const child = iterator->second.vp;
        if (child != nullptr && vehicle_part_contains_gun(*child, gun))
            return true;
    }
    return false;
}

hta::ai::Vehicle* vehicle_from_gun_owner(hta::ai::Gun* gun)
{
    if (gun == nullptr)
        return nullptr;
    const auto contains_gun = [gun](hta::ai::Vehicle& vehicle) {
        const auto names = vehicle.GetAttachedPartNames();
        for (std::size_t index = 0; index < names.size(); ++index) {
            hta::ai::VehiclePart* const part =
                vehicle.GetPartByName(names[index]);
            if (part != nullptr && vehicle_part_contains_gun(*part, gun))
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

std::uint64_t weapon_identity_hash(const std::string_view domain,
                                   const std::string_view value) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](const unsigned char byte) noexcept {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    };
    for (const char byte : domain)
        add(static_cast<unsigned char>(byte));
    add(0);
    for (const char byte : value)
        add(static_cast<unsigned char>(byte));
    return hash == 0 ? 1 : hash;
}

std::string native_part_path(const hta::ai::VehiclePart& part)
{
    std::vector<std::string> components;
    const hta::ai::VehiclePart* current = &part;
    std::array<const hta::ai::VehiclePart*, 256> visited{};
    std::size_t visited_count = 0;
    for (std::size_t depth = 0; current != nullptr && depth != 256; ++depth) {
        for (std::size_t index = 0; index != visited_count; ++index) {
            if (visited[index] == current)
                return {};
        }
        visited[visited_count++] = current;
        const char* const name = current->GetPartName().c_str();
        if (name == nullptr || name[0] == '\0')
            return {};
        components.emplace_back(name);
        current = current->GetOwnerCompoundVehiclePart();
    }
    if (current != nullptr || components.empty())
        return {};
    std::string path;
    for (auto iterator = components.rbegin(); iterator != components.rend();
         ++iterator) {
        if (!path.empty())
            path.push_back('/');
        path += *iterator;
    }
    return path;
}

GunAttachmentIdentity make_weapon_identity(const std::string_view path,
                                           const std::int32_t prototype_id)
{
    GunAttachmentIdentity identity{};
    if (path.empty() || prototype_id < 0)
        return identity;
    identity.path_hash = weapon_identity_hash("kraken/gun-path/v1", path);
    const std::string attachment_key = std::string(path) + "#" +
        std::to_string(prototype_id);
    identity.attachment_id = weapon_identity_hash(
        "kraken/gun-attachment/v1", attachment_key);
    return identity;
}

bool weapon_identity_equal(const GunAttachmentIdentity& left,
                           const GunAttachmentIdentity& right) noexcept
{
    return left.attachment_id != 0 && left.path_hash != 0 &&
           left.attachment_id == right.attachment_id &&
           left.path_hash == right.path_hash;
}

using GunIdentityBinding = std::pair<hta::ai::Gun*, GunAttachmentIdentity>;

void collect_native_guns(hta::ai::VehiclePart& part,
                         const std::string& path,
                         std::vector<GunIdentityBinding>& output)
{
    if (part.IsKindOf(hta::ai::Gun::p_classObject))
        output.emplace_back(static_cast<hta::ai::Gun*>(&part),
                            make_weapon_identity(path, part.GetPrototypeId()));
    if (!part.IsKindOf(hta::ai::CompoundVehiclePart::p_classObject))
        return;
    const auto& compound =
        static_cast<const hta::ai::CompoundVehiclePart&>(part);
    for (auto iterator = compound.begin(); iterator != compound.end();
         ++iterator) {
        if (iterator->second.vp == nullptr)
            continue;
        const char* const child_name = iterator->first.c_str();
        if (child_name == nullptr || child_name[0] == '\0')
            continue;
        collect_native_guns(*iterator->second.vp,
                            path + "/" + child_name, output);
    }
}

void collect_vehicle_guns(hta::ai::Vehicle& vehicle,
                          std::vector<GunIdentityBinding>& output)
{
    const auto names = vehicle.GetAttachedPartNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        hta::ai::VehiclePart* const part = vehicle.GetPartByName(names[index]);
        const char* const name = names[index].c_str();
        if (part == nullptr || name == nullptr || name[0] == '\0')
            continue;
        collect_native_guns(*part, name, output);
    }
}

bool capture_weapon_identity(hta::ai::Vehicle& vehicle,
                             hta::ai::Gun* const selected,
                             GunAttachmentIdentity& output)
{
    if (selected == nullptr)
        return false;
    std::vector<GunIdentityBinding> bindings;
    collect_vehicle_guns(vehicle, bindings);
    for (const GunIdentityBinding& binding : bindings) {
        if (binding.first == selected && binding.second.attachment_id != 0) {
            output = binding.second;
            return true;
        }
    }
    return false;
}

bool capture_unique_weapon_identity(hta::ai::Vehicle& vehicle,
                                   const std::int32_t prototype_id,
                                   GunAttachmentIdentity& output)
{
    std::vector<GunIdentityBinding> bindings;
    collect_vehicle_guns(vehicle, bindings);
    const GunIdentityBinding* found = nullptr;
    for (const GunIdentityBinding& binding : bindings) {
        if (binding.first->GetPrototypeId() != prototype_id)
            continue;
        if (found != nullptr)
            return false;
        found = &binding;
    }
    if (found == nullptr)
        return false;
    output = found->second;
    return output.attachment_id != 0 && output.path_hash != 0;
}

hta::ai::Gun* resolve_exact_weapon(hta::ai::Vehicle& vehicle,
                                   const GunAttachmentIdentity& identity)
{
    if (identity.attachment_id == 0 || identity.path_hash == 0)
        return nullptr;
    std::vector<GunIdentityBinding> bindings;
    collect_vehicle_guns(vehicle, bindings);
    hta::ai::Gun* resolved = nullptr;
    for (const GunIdentityBinding& binding : bindings) {
        if (!weapon_identity_equal(binding.second, identity))
            continue;
        if (resolved != nullptr)
            return nullptr;
        resolved = binding.first;
    }
    return resolved;
}

bool g_host_network_shot_fired = false;
// Vehicle::_EvaluateToDead deliberately fires attached weapons while it tears
// down the vehicle.  That is native death presentation, not player input and
// must never pass through the multiplayer weapon-capture/relay path.
bool g_presenting_authoritative_death = false;
// Set only around the host's replay of a client shot.  Gun::_DoFire still
// performs the original projectile work, but its hook must not emit a second
// presentation built from a process-local fallback target.
bool g_replaying_network_fire = false;
WeaponCommand* g_active_host_weapon_command = nullptr;

bool active_client_replica() noexcept
{
    return !g_state.is_host && g_state.session &&
           g_state.session->running();
}

bool call_original_gun_do_fire(hta::ai::Gun* const gun)
{
    const bool client_authority_active = active_client_replica();
    if (client_authority_active) {
        ++g_state.client_blocked_fire_attempt_count;
        return false;
    }
    const bool fired = g_gun_do_fire(gun);
    // This counter describes completed original invocations only. The guard
    // above makes a true value impossible in safe production flow.
    if (client_authority_active)
        ++g_state.client_original_fire_call_count;
    return fired;
}

bool call_original_vehicle_inflict_damage(
    const VehicleInflictDamageFn original, hta::ai::Vehicle* const vehicle,
    const hta::ai::DamageInfo& info)
{
    const bool client_authority_active = active_client_replica();
    if (client_authority_active) {
        ++g_state.client_blocked_damage_attempt_count;
        return false;
    }
    original(vehicle, info);
    // Increment only after the native original really returned. Safe code
    // cannot reach this point with client authority active.
    if (client_authority_active)
        ++g_state.client_original_damage_call_count;
    return true;
}
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
        GunAttachmentIdentity identity = g_state.has_local_weapon_gun
            ? g_state.local_weapon_gun : GunAttachmentIdentity{};
        if (identity.attachment_id == 0 &&
            !capture_unique_weapon_identity(*vehicle,
                                            g_state.local_weapon_gun_id,
                                            identity))
            return;
        NetId shooter_id = kInvalidNetId;
        EntityGeneration shooter_generation = kInvalidEntityGeneration;
        if (!g_state.entities.lookup_net_id(vehicle->GetId(), shooter_id,
                                            shooter_generation))
            return;
        WeaponAimState state{};
        state.session_epoch = g_state.session_epoch;
        state.update_sequence = g_state.next_weapon_sequence++;
        state.server_tick = g_state.server_tick;
        state.shooter = {shooter_id, shooter_generation};
        state.gun = identity;
        state.aim_point = exact_aim;
        const hta::CVector position = vehicle->GetPosition();
        const float dx = exact_aim.x - position.x;
        const float dy = exact_aim.y - position.y;
        const float dz = exact_aim.z - position.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(length) && length > 0.0001f)
            state.aim_direction = {dx / length, dy / length, dz / length};
        state.aim_speed = speed;
        if (g_state.local_weapon_target_entity_id != kInvalidNetId) {
            EntityGeneration target_generation = kInvalidEntityGeneration;
            if (g_state.entities.lookup_generation(
                    g_state.local_weapon_target_entity_id,
                    target_generation)) {
                state.has_target = true;
                state.target = {g_state.local_weapon_target_entity_id,
                                target_generation};
            }
        }
        g_state.local_weapon_gun = identity;
        g_state.has_local_weapon_gun = true;
        publish_host_weapon_aim(state);
    } else if (!submit_native_weapon_intent(
                   vehicle, g_state.local_weapon_trigger_held,
                   g_state.local_weapon_target_obj_id,
                   g_state.local_weapon_gun_id, &exact_aim, speed,
                   g_state.has_local_weapon_gun ? &g_state.local_weapon_gun
                                                : nullptr)) {
        LOG_ERROR("local weapon aim state was not sent");
    }
}

void publish_weapon_group_input(hta::ai::Vehicle* const vehicle,
                                const bool trigger_held,
                                const std::int32_t gun_id,
                                const GunAttachmentIdentity* const gun_override = nullptr)
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
    if (gun_id >= 0)
        g_state.local_weapon_gun_id = gun_id;
    GunAttachmentIdentity identity = gun_override != nullptr
        ? *gun_override : GunAttachmentIdentity{};
    if (identity.attachment_id == 0 &&
        !capture_unique_weapon_identity(*current_vehicle, gun_id, identity)) {
        LOG_ERROR("weapon intent rejected: gun prototype=%d is not unique on vehicle=%d",
                  gun_id, current_vehicle->GetId());
        g_state.has_local_weapon_gun = false;
        return;
    }
    g_state.local_weapon_gun = identity;
    g_state.has_local_weapon_gun = true;

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
    if (!g_state.is_host && !submit_native_weapon_intent(
                   current_vehicle, trigger_held,
                   g_state.local_weapon_target_obj_id,
                   g_state.local_weapon_gun_id, aim,
                   g_state.local_weapon_aim_speed, &identity)) {
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
    GunAttachmentIdentity identity{};
    hta::ai::Vehicle* const current_vehicle = current_local_weapon_vehicle(
        vehicle, "weapon group hook");
    if (current_vehicle != nullptr && gun_part_name != nullptr) {
        hta::ai::VehiclePart* const part =
            current_vehicle->GetPartByName(*gun_part_name);
        if (part != nullptr) {
            gun_id = part->GetPrototypeId();
            if (part->IsKindOf(hta::ai::Gun::p_classObject))
                (void)capture_weapon_identity(
                    *current_vehicle, static_cast<hta::ai::Gun*>(part), identity);
        }
    }
    publish_weapon_group_input(current_vehicle, trigger_held, gun_id,
                               identity.attachment_id != 0 ? &identity : nullptr);

    if (!g_state.is_host && g_state.session && g_state.session->running())
        return true;
    return g_fire_from_weapon_by_part_name(vehicle, gun_part_name,
                                           trigger_held);
}

bool __fastcall weapon_group_fire_by_part_name_hook(
    hta::ai::Vehicle* const vehicle, void*, const hta::CStr* const gun_part_name,
    const bool trigger_held)
{
    return fire_local_weapon_by_part_name(vehicle, gun_part_name, trigger_held);
}

void emit_host_shot_confirmed(hta::ai::Gun& gun,
                              const WeaponCommand* const command)
{
    if (!g_state.is_host || !g_state.session || !g_state.session->running())
        return;
    hta::ai::Vehicle* const owner = vehicle_from_gun_owner(&gun);
    if (owner == nullptr || !is_player_controlled_vehicle(*owner))
        return;
    NetId shooter_id = kInvalidNetId;
    EntityGeneration shooter_generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_net_id(owner->GetId(), shooter_id,
                                        shooter_generation))
        return;
    GunAttachmentIdentity identity = command != nullptr ? command->gun
                                                         : GunAttachmentIdentity{};
    if (identity.attachment_id == 0 &&
        !capture_weapon_identity(*owner, &gun, identity))
        return;
    if (command != nullptr &&
        !g_state.combat_autotest_scenario.empty() &&
        command->target_entity_id != kInvalidNetId) {
        hta::ai::Vehicle* const target = find_vehicle(
            command->target_entity_id, command->target_generation);
        if (target != nullptr) {
            const hta::CVector shot_position = gun._CalcPosForNextShot();
            const hta::CVector shot_direction = gun._CalcDirForNextShot();
            const hta::CVector target_position = target->GetGeometricCenter();
            const float dx = target_position.x - shot_position.x;
            const float dy = target_position.y - shot_position.y;
            const float dz = target_position.z - shot_position.z;
            const float distance_squared = dx * dx + dy * dy + dz * dz;
            const float direction_squared =
                shot_direction.x * shot_direction.x +
                shot_direction.y * shot_direction.y +
                shot_direction.z * shot_direction.z;
            float alignment = -1.0f;
            float lateral_miss = -1.0f;
            if (std::isfinite(distance_squared) && distance_squared > 0.0001f &&
                std::isfinite(direction_squared) && direction_squared > 0.0001f) {
                const float distance = std::sqrt(distance_squared);
                const float direction_length = std::sqrt(direction_squared);
                alignment = (dx * shot_direction.x + dy * shot_direction.y +
                             dz * shot_direction.z) /
                    (distance * direction_length);
                const float projection = alignment * distance;
                lateral_miss = std::sqrt((std::max)(
                    0.0f, distance_squared - projection * projection));
            }
            LOG_INFO("KRAKEN_COMBAT_AUTOTEST shot-geometry scenario=%s shooter=%u target=%u gunObj=%d targetObj=%d align=%.6f lateralMiss=%.3f muzzle=%.3f,%.3f,%.3f dir=%.4f,%.4f,%.4f target=%.3f,%.3f,%.3f canShot=%u",
                     g_state.combat_autotest_scenario.c_str(), shooter_id,
                     command->target_entity_id, gun.GetId(), target->GetId(),
                     alignment, lateral_miss, shot_position.x,
                     shot_position.y, shot_position.z, shot_direction.x,
                     shot_direction.y, shot_direction.z, target_position.x,
                     target_position.y, target_position.z,
                     gun.CanShotToTarget(target->GetId()) ? 1u : 0u);
        }
    }
    hta::m3d::SgNode* const barrel = gun.GetBarrelNode();
    if (barrel == nullptr)
        return;
    const hta::CVector& muzzle_position = barrel->GetOriginWorldAbs();
    const hta::Quaternion& muzzle_rotation = barrel->GetRotationWorldAbs();
    ShotConfirmed confirmed{};
    confirmed.session_epoch = g_state.session_epoch;
    // Accepted remote commands already carry the stable shot correlation
    // chosen at the input boundary. Preserve it for the authoritative
    // confirmation and the ProcessShellAndBody impact capture; local host
    // fire has no command and receives the host sequence identity.
    confirmed.shot_id = command != nullptr && command->shot_id != 0
        ? static_cast<CombatEventId>(command->shot_id)
        : static_cast<CombatEventId>(g_state.next_weapon_sequence++);
    confirmed.server_tick = g_state.server_tick;
    confirmed.shooter = {shooter_id, shooter_generation};
    confirmed.gun = identity;
    confirmed.burst_id = static_cast<std::uint32_t>(confirmed.shot_id);
    confirmed.burst_index = 0;
    confirmed.burst_size = 1;
    confirmed.muzzle_pose.position = {muzzle_position.x, muzzle_position.y,
                                      muzzle_position.z};
    confirmed.muzzle_pose.rotation = {muzzle_rotation.x, muzzle_rotation.y,
                                      muzzle_rotation.z, muzzle_rotation.w};
    confirmed.shells_in_current_charge = gun.GetShellsInCurrentCharge();
    confirmed.shells_in_pool = gun.GetShellsInPool();
    confirmed.reload_state = gun.GetChargeState() == hta::ai::Gun::csInCharging
        ? AmmoReloadState::Reloading : AmmoReloadState::Ready;
    publish_host_shot_confirmed(confirmed);
}

bool __fastcall gun_do_fire_hook(hta::ai::Gun* gun, void*)
{
    // Replica denial is the first policy decision. Death/presentation scopes
    // cannot bypass it and therefore can never reach the original _DoFire.
    if (active_client_replica()) {
        ++g_state.client_blocked_fire_attempt_count;
        return false;
    }
    if (g_presenting_authoritative_death)
        return call_original_gun_do_fire(gun);

    const bool fired = call_original_gun_do_fire(gun);
    if (g_replaying_network_fire) {
        g_host_network_shot_fired = g_host_network_shot_fired || fired;
        if (fired && gun != nullptr)
            emit_host_shot_confirmed(*gun, g_active_host_weapon_command);
        return fired;
    }
    if (fired && g_state.is_host && gun != nullptr) {
        if (hta::ai::Vehicle* const owner = vehicle_from_gun_owner(gun)) {
            if (!g_state.combat_autotest_scenario.empty() &&
                is_player_controlled_vehicle(*owner)) {
                LOG_INFO("KRAKEN_COMBAT_AUTOTEST fired scenario=%s ownerObj=%d gun_attachment=%llu targetObj=%d",
                         g_state.combat_autotest_scenario.c_str(), owner->GetId(),
                         static_cast<unsigned long long>(
                             g_state.has_local_weapon_gun
                                 ? g_state.local_weapon_gun.attachment_id : 0),
                         gun->m_targetObjId);
            }
            emit_host_shot_confirmed(*gun, nullptr);
        } else
            LOG_ERROR("host gun fire has a non-vehicle owner");
    }
    return fired;
}

void __fastcall trigger_update_hook(hta::ai::Trigger* const trigger, void*,
                                    const float elapsed_time,
                                    const std::uint32_t work_time)
{
    if (quest_replica_execution_suppressed(
            g_state.is_host,
            g_state.quest_replica_active,
            current_quest_replica_phase())) {
        log_denied_authoritative_origin(
            world_authority::WorldAction::ScriptWorldMutation,
            "Trigger::Update/Replica");
        return;
    }
    if (!world_authority::allows(world_authority::WorldAction::ScriptWorldMutation)) {
        log_denied_authoritative_origin(
            world_authority::WorldAction::ScriptWorldMutation,
            "Trigger::Update/Lua");
        return;
    }
    g_trigger_update_original(trigger, elapsed_time, work_time);
}

int __fastcall trigger_on_event_hook(hta::ai::Trigger* const trigger, void*,
                                     const hta::ai::Event* const event)
{
    if (event == nullptr)
        return 0;
    if (quest_replica_execution_suppressed(
            g_state.is_host,
            g_state.quest_replica_active,
            current_quest_replica_phase())) {
        log_denied_authoritative_origin(
            world_authority::WorldAction::ScriptWorldMutation,
            "Trigger::OnEvent/Replica");
        return 0;
    }
    return g_trigger_on_event_original(trigger, *event);
}

void __fastcall quest_update_hook(hta::ai::QuestStateManager* const manager,
                                  void*, const float elapsed_time)
{
    if (quest_replica_execution_suppressed(
            g_state.is_host,
            g_state.quest_replica_active,
            current_quest_replica_phase())) {
        log_denied_authoritative_origin(
            world_authority::WorldAction::QuestAdvance,
            "QuestStateManager::Update/Replica");
        return;
    }
    if (!world_authority::allows(world_authority::WorldAction::QuestAdvance)) {
        log_denied_authoritative_origin(
            world_authority::WorldAction::QuestAdvance,
            "QuestStateManager::Update");
        return;
    }
    g_quest_update_original(manager, elapsed_time);
}

std::string resolved_trigger_resource(
    hta::ai::CServer& server, const QuestProjectionSourceKind source_kind,
    const hta::CStr* const file_name)
{
    if (file_name != nullptr && file_name->c_str() != nullptr &&
        file_name->c_str()[0] != '\0')
        return file_name->c_str();
    if (server.m_level == nullptr)
        return {};
    const hta::CStr& configured =
        source_kind == QuestProjectionSourceKind::TriggerCinematic
            ? server.m_level->m_cinemaTriggersName
            : server.m_level->m_TriggersName;
    if (configured.empty())
        return {};
    const hta::CStr resolved = server.m_level->GetFullPathNameA(configured);
    return resolved.c_str() == nullptr ? std::string{} : resolved.c_str();
}

struct ActiveTriggerProvenanceContext final {
    QuestProjectionSourceKind source_kind =
        QuestProjectionSourceKind::TriggerNormal;
    std::string resource_path;
};

thread_local std::optional<ActiveTriggerProvenanceContext>
    g_active_trigger_provenance_context;

class ScopedTriggerProvenanceContext final {
public:
    ScopedTriggerProvenanceContext(
        const QuestProjectionSourceKind source_kind,
        std::string resource_path)
    {
        m_previous = std::move(g_active_trigger_provenance_context);
        try {
            g_active_trigger_provenance_context.emplace(
                ActiveTriggerProvenanceContext{source_kind, std::move(resource_path)});
        } catch (...) {
            g_active_trigger_provenance_context = std::move(m_previous);
            throw;
        }
    }

    ~ScopedTriggerProvenanceContext()
    { g_active_trigger_provenance_context = std::move(m_previous); }

    ScopedTriggerProvenanceContext(const ScopedTriggerProvenanceContext&) = delete;
    ScopedTriggerProvenanceContext& operator=(
        const ScopedTriggerProvenanceContext&) = delete;

private:
    std::optional<ActiveTriggerProvenanceContext> m_previous;
};

hta::ai::Obj* __fastcall trigger_name_lookup_hook(
    hta::ai::ObjContainer* const container, void*, hta::CStr* const name)
{
    if (g_active_trigger_provenance_context.has_value()) {
        const ActiveTriggerProvenanceContext& context =
            *g_active_trigger_provenance_context;
        try {
            const char* const requested_name =
                name == nullptr || name->c_str() == nullptr ? "" : name->c_str();
            const QuestTriggerProvenanceBindResult result =
                g_state.quest_trigger_provenance.bind(
                    context.source_kind, context.resource_path, requested_name);
            if (result != QuestTriggerProvenanceBindResult::Bound) {
                LOG_ERROR("quest trigger provenance rejected source=%u path=%s name=%s result=%u locked=%u",
                          static_cast<unsigned>(context.source_kind),
                          context.resource_path.c_str(), requested_name,
                          static_cast<unsigned>(result),
                          g_state.quest_trigger_provenance.locked() ? 1u : 0u);
            }
        } catch (...) {
            LOG_ERROR("quest trigger provenance lookup bind failed; original lookup continues");
        }
    }
    return g_get_entity_by_obj_name_original(container, name);
}

void __fastcall load_normal_triggers_hook(
    hta::ai::CServer* const server, void*, hta::CStr* const file_name)
{
    if (server == nullptr) {
        g_load_triggers_from_xml_original(server, file_name);
        return;
    }
    g_state.quest_trigger_provenance.begin_map_load(
        QuestProjectionSourceKind::TriggerNormal);
    const ScopedTriggerProvenanceContext context(
        QuestProjectionSourceKind::TriggerNormal,
        resolved_trigger_resource(*server,
                                  QuestProjectionSourceKind::TriggerNormal,
                                  file_name));
    g_load_triggers_from_xml_original(server, file_name);
}

void __fastcall load_cinematic_triggers_hook(
    hta::ai::CServer* const server, void*, hta::CStr* const file_name)
{
    if (server == nullptr) {
        g_load_triggers_from_xml_original(server, file_name);
        return;
    }
    const ScopedTriggerProvenanceContext context(
        QuestProjectionSourceKind::TriggerCinematic,
        resolved_trigger_resource(*server,
                                  QuestProjectionSourceKind::TriggerCinematic,
                                  file_name));
    g_load_triggers_from_xml_original(server, file_name);
}

void verify_quest_update_call_site()
{
    auto* const site = reinterpret_cast<const std::uint8_t*>(
        kQuestUpdateCallSite);
    if (*site == 0xE8) {
        std::int32_t relative = 0;
        std::memcpy(&relative, site + 1, sizeof(relative));
        const uintptr_t target = kQuestUpdateCallSite + 5 + relative;
        if (target == kQuestUpdateAddress)
            return;
    }
    throw std::runtime_error("QuestStateManager::Update call site mismatch");
}

void verify_trigger_load_call_site(const uintptr_t call_site,
                                   const uintptr_t target,
                                   const char* const name)
{
    const auto* const site = reinterpret_cast<const std::uint8_t*>(call_site);
    if (*site == 0xE8) {
        std::int32_t relative = 0;
        std::memcpy(&relative, site + 1, sizeof(relative));
        if (call_site + 5 + relative == target)
            return;
    }
    throw std::runtime_error(std::string(name) + " call site mismatch");
}

void install_trigger_provenance_hooks()
{
    if (g_state.normal_trigger_provenance_hook_installed &&
        g_state.cinematic_trigger_provenance_hook_installed &&
        g_state.trigger_name_lookup_hook_installed)
        return;
    if (sizeof(void*) != sizeof(std::uint32_t))
        throw std::runtime_error("trigger provenance hooks require Win32");
    if (!g_state.normal_trigger_provenance_hook_installed) {
        verify_trigger_load_call_site(kNormalTriggerLoadCallSite,
                                      kLoadTriggersFromXmlAddress,
                                      "normal trigger loader");
        routines::ChangeCall(reinterpret_cast<void*>(kNormalTriggerLoadCallSite),
                             &load_normal_triggers_hook);
        g_state.normal_trigger_provenance_hook_installed = true;
    }
    if (!g_state.cinematic_trigger_provenance_hook_installed) {
        verify_trigger_load_call_site(kCinematicTriggerLoadCallSite,
                                      kLoadTriggersFromXmlAddress,
                                      "cinematic trigger loader");
        routines::ChangeCall(reinterpret_cast<void*>(kCinematicTriggerLoadCallSite),
                             &load_cinematic_triggers_hook);
        g_state.cinematic_trigger_provenance_hook_installed = true;
    }
    if (!g_state.trigger_name_lookup_hook_installed) {
        verify_trigger_load_call_site(kTriggerNameLookupCallSite,
                                      kGetEntityByObjNameAddress,
                                      "trigger name lookup");
        routines::ChangeCall(reinterpret_cast<void*>(kTriggerNameLookupCallSite),
                             &trigger_name_lookup_hook);
        g_state.trigger_name_lookup_hook_installed = true;
    }
}

void install_trigger_update_vtable_hook()
{
    if (g_state.trigger_update_hook_installed &&
        g_state.trigger_on_event_hook_installed)
        return;
    if (sizeof(void*) != sizeof(std::uint32_t))
        throw std::runtime_error("Trigger::Update hook requires Win32");

    auto install_slot = [](std::size_t offset, void* expected, void* replacement,
                           bool& installed, auto& original, const char* name) {
        if (installed)
            return;
        auto** const slot = reinterpret_cast<void**>(
            kTriggerVtableAddress + offset);
        void* const current = *slot;
        if (current == replacement) {
            installed = true;
            return;
        }
        if (current != expected)
            throw std::runtime_error(std::string(name) + " vtable slot mismatch");
        DWORD old_protection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protection))
            throw std::runtime_error(std::string(name) + " vtable protection failed");
        original = reinterpret_cast<std::decay_t<decltype(original)>>(current);
        *slot = replacement;
        DWORD ignored = 0;
        (void)VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        installed = true;
    };
    install_slot(kTriggerUpdateVtableOffset,
                 reinterpret_cast<void*>(kTriggerUpdateAddress),
                 reinterpret_cast<void*>(&trigger_update_hook),
                 g_state.trigger_update_hook_installed,
                 g_trigger_update_original, "Trigger::Update");
    install_slot(kTriggerOnEventVtableOffset,
                 reinterpret_cast<void*>(kTriggerOnEventAddress),
                 reinterpret_cast<void*>(&trigger_on_event_hook),
                 g_state.trigger_on_event_hook_installed,
                 g_trigger_on_event_original, "Trigger::OnEvent");
}

void install_quest_update_call_hook()
{
    if (g_state.quest_update_hook_installed)
        return;
    verify_quest_update_call_site();
    routines::ChangeCall(reinterpret_cast<void*>(kQuestUpdateCallSite),
                         &quest_update_hook);
    g_state.quest_update_hook_installed = true;
}

void __fastcall wanderer_spawn_hook(void* manager, void*)
{
    // The listen-server is the only process allowed to advance the random
    // wanderer generator.  Client ghosts arrive through EntitySpawn/Snapshot;
    // generating locally here caused each peer to get a different bot set.
    if (!world_authority::allows(world_authority::WorldAction::AiGeneration)) {
        log_denied_authoritative_origin(
            world_authority::WorldAction::AiGeneration,
            "WanderersManager::_SpawnWanderer");
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
        install_trigger_provenance_hooks();
        install_trigger_update_vtable_hook();
        install_quest_update_call_hook();
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
    char scenario_buffer[64]{};
    char role_buffer[16]{};
    std::size_t scenario_length = 0;
    std::size_t role_length = 0;
    (void)getenv_s(&scenario_length, scenario_buffer, sizeof(scenario_buffer),
                   "KRAKEN_MP_ACCEPT_SCENARIO");
    (void)getenv_s(&role_length, role_buffer, sizeof(role_buffer),
                   "KRAKEN_MP_ACCEPT_ROLE");
    const char* const scenario = scenario_length > 1 ? scenario_buffer : "forming";
    const char* const role = role_length > 1 ? role_buffer : "auto";
    const std::string save_maps_directory = find_autotest_save_maps_directory();
    if (save_maps_directory.empty()) {
        LOG_ERROR("raid autotest bootstrap: no data/profiles/*/saves/*/maps/currentmap.xml");
        LOG_INFO("KRAKEN_MP_ACCEPT native_saved_game role=%s scenario=%s result=rejected",
                 role, scenario);
        return;
    }
    hta::CStr save_directory(save_maps_directory.c_str());
    const bool loaded = call_load_saved_game(application, &save_directory);
    g_state.raid_autotest_native_save_loaded = loaded;
    LOG_INFO("raid autotest bootstrap: LoadSavedGame('%s') returned %d",
             save_maps_directory.c_str(), loaded ? 1 : 0);
    LOG_INFO("KRAKEN_MP_ACCEPT native_saved_game role=%s scenario=%s result=%s",
             role, scenario, loaded ? "loaded" : "rejected");
}

void __fastcall process_all_events_raid_autotest_hook(void* application)
{
    reinterpret_cast<ProcessAllEventsFn>(kProcessAllEventsAddress)(application);
    confirm_deferred_exit_route(
        reinterpret_cast<hta::CMiracle3d*>(application));
    if (!g_state.raid_autotest_enabled)
        return;
    if (!g_state.raid_autotest_bootstrap_attempted) {
        // ProcessAllEvents precedes OneFrame. The stock main menu must
        // therefore have completed before invoking its native load path.
        if (++g_state.raid_autotest_bootstrap_frames < 300)
            return;
        g_state.raid_autotest_bootstrap_attempted = true;
        try_raid_autotest_bootstrap(application);
    }
    // Map transitions can temporarily stop CServer::Update. Keep test-only
    // session orchestration on the application loop as well; the attempted
    // flag inside run_raid_autotest_tick preserves exactly-once startup.
    run_raid_autotest_tick();
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
    // Replica denial is the first policy decision. Impact/death/presentation
    // state can never bypass this native damage boundary.
    if (active_client_replica()) {
        ++g_state.client_blocked_damage_attempt_count;
        return;
    }
    const VehicleInflictDamageFn original =
        g_state.vehicle_inflict_damage_original;
    if (vehicle == nullptr || original == nullptr) {
        LOG_ERROR("impact damage hook has no valid original target");
        return;
    }

    if (!g_state.session || !g_state.session->running() || !g_state.is_host) {
        (void)call_original_vehicle_inflict_damage(original, vehicle, info);
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
    combat_runtime::HostImpactObservation* const typed_capture =
        combat_runtime::current_host_impact_capture();
    const float pre_health = vehicle->GetHealth();
    if (typed_capture != nullptr && input_captured) {
        // The ProcessShellAndBody body/contact resolver owns the actual hit
        // target. Vehicle::InflictDamage may refine authoritative damage, but
        // it must never replace a body-resolved target with weapon-command
        // prediction or a damaged-part string.
        if (g_active_host_weapon_command != nullptr &&
            g_active_host_weapon_command->shot_id != 0) {
            typed_capture->shot_id = g_active_host_weapon_command->shot_id;
            typed_capture->gun = g_active_host_weapon_command->gun;
        } else if (event.attacker_entity_id != kInvalidNetId) {
            hta::ai::Vehicle* const attacker = find_vehicle(
                event.attacker_entity_id, event.attacker_generation);
            if (attacker != nullptr && capture_unique_weapon_identity(
                    *attacker, event.gun_id, typed_capture->gun)) {
                // A native damage callback without the active accepted-shot
                // command has no authoritative shot correlation. Keep the
                // canonical gun identity for diagnostics but fail closed.
                typed_capture->shot_id = 0;
            }
        }
    }
    if (input_captured && g_state.match.state() != MatchState::Offline &&
        event.attacker_entity_id != kInvalidNetId &&
        !g_state.match.config().friendly_fire) {
        hta::ai::Vehicle* const attacker = find_vehicle(
            event.attacker_entity_id, event.attacker_generation);
        if (attacker != nullptr && suppress_same_belong_damage(
                false, attacker->GetBelong(), vehicle->GetBelong())) {
            LOG_INFO("friendly-fire damage suppressed attacker=%u target=%u belong=%d",
                     event.attacker_entity_id, event.target_entity_id,
                     attacker->GetBelong());
            return;
        }
    }
    // The authoritative engine transition is always exactly one original
    // invocation, even when the registry cannot represent this impact.
    (void)call_original_vehicle_inflict_damage(original, vehicle, info);
    if (!input_captured || was_dead)
        return;

    if (typed_capture != nullptr && typed_capture->event_id != 0)
        event.event_id = static_cast<std::uint32_t>(typed_capture->event_id);
    else {
        event.event_id = g_state.next_impact_event_id++;
        if (g_state.next_impact_event_id == 0)
            g_state.next_impact_event_id = 1;
    }
    event.server_tick = g_state.server_tick;
    const float post_health = vehicle->GetHealth();
    event.post_health = std::isfinite(post_health)
        ? (std::max)(0.0f, post_health) : -1.0f;
    event.target_dead = vehicle->_GetDeadStatus();
    if (typed_capture != nullptr && input_captured &&
        typed_capture->shot_id != 0) {
        NetId authoritative_target_id = kInvalidNetId;
        EntityGeneration authoritative_target_generation =
            kInvalidEntityGeneration;
        const bool authoritative_target_bound =
            g_state.entities.lookup_net_id(
                vehicle->GetId(), authoritative_target_id,
                authoritative_target_generation);
        typed_capture->did_damage = std::isfinite(pre_health) &&
            std::isfinite(post_health) && post_health < pre_health;
        typed_capture->blocked_reason = typed_capture->did_damage
            ? ImpactBlockedReason::None : ImpactBlockedReason::Unknown;
        if (typed_capture->did_damage && authoritative_target_bound) {
            DamageResult damage{};
            damage.session_epoch = typed_capture->session_epoch;
            damage.event_id = typed_capture->event_id;
            damage.server_tick = typed_capture->server_tick;
            damage.shot_id = typed_capture->shot_id;
            damage.impact_event_id = typed_capture->event_id;
            damage.shooter = typed_capture->shooter;
            damage.target = {authoritative_target_id,
                             authoritative_target_generation};
            damage.damage = std::isfinite(event.damage)
                ? (std::max)(0.0f, event.damage) : 0.0f;
            damage.post_health = event.post_health;
            damage.damaged_part = event.damaged_part;
            damage.dead_transition = event.target_dead;
            if (validate_damage_result(damage) ==
                CombatPresentationCodecError::None) {
                combat_runtime::record_authoritative_damage(damage);
                if (damage.dead_transition && g_state.combat_runtime != nullptr &&
                    !g_state.combat_runtime->request_host_wreck(damage))
                    LOG_WARNING("typed death wreck request closed: archive resolver unavailable entity=%u",
                                damage.target.net_id);
            }
        }
    }
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

    if (IsSessionActive()) {
        // The typed ProcessShellAndBody capture publishes the confirmed
        // impact/damage pair after the native call returns. Legacy packets
        // are deliberately not relayed in active multiplayer.
        return;
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
            max_peers <= static_cast<std::int32_t>(kMaxSessionPlayers) &&
            address != nullptr)
            accepted = ConfigureSession(args.m_InArgs[0].GetB(), address,
                                        static_cast<unsigned short>(port),
                                        static_cast<unsigned int>(max_peers));
    }
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_end_session(hta::m3d::sArgStack& args)
{
    SessionLeaveReason reason = SessionLeaveReason::User;
    bool valid = args.m_numInArgs == 0;
    if (args.m_numInArgs == 1 &&
        args.m_InArgs[0].GetType() == hta::m3d::sArg::ARGTYPE_STRING) {
        const char* const text = args.m_InArgs[0].GetS();
        if (text != nullptr) {
            const std::string_view value(text);
            if (value == "unknown" || value == "user")
                reason = SessionLeaveReason::User;
            else if (value == "death")
                reason = SessionLeaveReason::Death;
            else if (value == "extract")
                reason = SessionLeaveReason::Extract;
            else if (value == "host_terminated" || value == "hostTerminated")
                reason = SessionLeaveReason::HostTerminated;
            else if (value == "disconnect") {
                // The public compatibility enum predates disconnect.  Keep
                // the wire value valid and retain the generic user reason.
                reason = SessionLeaveReason::User;
            }
            else if (value == "map_unload" || value == "mapUnload")
                reason = SessionLeaveReason::MapUnload;
            else
                valid = false;
        }
        else {
            valid = false;
        }
    }
    const bool accepted = valid && end_session_with_reason(reason);
    if (!accepted)
        notify_lua_session_state(false, false);
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

bool read_lua_timeout_seconds(const hta::m3d::sArg& argument,
                              std::int32_t& seconds) noexcept
{
    // Lua nil arrives as a void argument and denotes an unbounded forming
    // phase, equivalent to the documented zero timeout.
    if (argument.GetType() == hta::m3d::sArg::ARGTYPE_VOID) {
        seconds = 0;
        return true;
    }
    if (argument.GetType() == hta::m3d::sArg::ARGTYPE_INT) {
        seconds = argument.GetI();
        return seconds >= 0;
    }
    if (argument.GetType() != hta::m3d::sArg::ARGTYPE_FLOAT)
        return false;
    const double value = static_cast<double>(argument.GetF());
    if (!std::isfinite(value) || std::trunc(value) != value || value < 0.0 ||
        value > static_cast<double>((std::numeric_limits<std::int32_t>::max)()))
        return false;
    seconds = static_cast<std::int32_t>(value);
    return true;
}

int __fastcall lua_start_matchmaking(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t required = 0;
    std::int32_t wait_timeout_seconds = 0;
    if (args.m_numInArgs == 5 &&
        read_lua_integral_arg(args.m_InArgs[0], required) &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_STRING &&
        args.m_InArgs[2].GetType() == hta::m3d::sArg::ARGTYPE_STRING &&
        read_lua_timeout_seconds(args.m_InArgs[3], wait_timeout_seconds) &&
        args.m_InArgs[4].GetType() == hta::m3d::sArg::ARGTYPE_BOOL &&
        required > 0 && required <= static_cast<std::int32_t>(kMaxSessionPlayers)) {
        accepted = start_matchmaking_impl(
            static_cast<std::uint8_t>(required), args.m_InArgs[1].GetS(),
            args.m_InArgs[2].GetS(), wait_timeout_seconds,
            args.m_InArgs[4].GetB());
    }
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(accepted);
    return 0;
}

int __fastcall lua_add_spawn(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t belong = 0;
    if (args.m_numInArgs == 3 &&
        args.m_InArgs[0].GetType() == hta::m3d::sArg::ARGTYPE_VECTOR &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_FLOAT &&
        read_lua_integral_arg(args.m_InArgs[2], belong)) {
        const hta::CVector position = args.m_InArgs[0].GetV();
        const float yaw = args.m_InArgs[1].GetF();
        accepted = add_match_spawn_impl(position.x, position.y, position.z,
                                        yaw, belong);
    }
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(accepted);
    return 0;
}

bool read_lua_real_arg(const hta::m3d::sArg& argument, float& value) noexcept
{
    if (argument.GetType() == hta::m3d::sArg::ARGTYPE_FLOAT) {
        value = argument.GetF();
        return std::isfinite(value);
    }
    if (argument.GetType() == hta::m3d::sArg::ARGTYPE_INT) {
        value = static_cast<float>(argument.GetI());
        return std::isfinite(value);
    }
    return false;
}

int __fastcall lua_add_spawn_components(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    std::int32_t belong = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
    if (args.m_numInArgs == 5 &&
        read_lua_real_arg(args.m_InArgs[0], x) &&
        read_lua_real_arg(args.m_InArgs[1], y) &&
        read_lua_real_arg(args.m_InArgs[2], z) &&
        read_lua_real_arg(args.m_InArgs[3], yaw) &&
        read_lua_integral_arg(args.m_InArgs[4], belong))
        accepted = add_match_spawn_impl(x, y, z, yaw, belong);
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(accepted);
    return 0;
}

int __fastcall lua_get_session_state(hta::m3d::sArgStack& args)
{
    const MatchStatus status = args.m_numInArgs == 0
        ? GetSessionStatus() : MatchStatus{};
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetS(to_string(status.state));
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetI(status.connected_players);
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetI(status.ready_players);
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetI(status.required_players);
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(status.infinite_wait);
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetI(static_cast<std::int32_t>((std::min)(
            status.remaining_wait_ms,
            static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()))));
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetF(static_cast<float>(status.remaining_wait_ms) / 1000.0f);
    return 0;
}

int __fastcall lua_leave_session(hta::m3d::sArgStack& args)
{
    SessionLeaveReason reason = SessionLeaveReason::User;
    bool valid = args.m_numInArgs == 0;
    if (args.m_numInArgs == 1 && args.m_InArgs[0].GetType() ==
            hta::m3d::sArg::ARGTYPE_STRING) {
        const char* const text = args.m_InArgs[0].GetS();
        if (text != nullptr) {
            const std::string_view value(text);
            if (value == "unknown" || value == "user")
                reason = SessionLeaveReason::User;
            else if (value == "death")
                reason = SessionLeaveReason::Death;
            else if (value == "extract")
                reason = SessionLeaveReason::Extract;
            else if (value == "host_terminated" || value == "hostTerminated")
                reason = SessionLeaveReason::HostTerminated;
            else if (value == "disconnect")
                reason = SessionLeaveReason::User;
            else if (value == "map_unload" || value == "mapUnload")
                reason = SessionLeaveReason::MapUnload;
            else
                valid = false;
        }
        else {
            valid = false;
        }
    }
    // Preserve the old numeric bridge while the generic string ABI rolls out.
    else if (args.m_numInArgs == 1 && args.m_InArgs[0].GetType() ==
             hta::m3d::sArg::ARGTYPE_INT) {
        const std::int32_t value = args.m_InArgs[0].GetI();
        valid = value >= static_cast<std::int32_t>(SessionLeaveReason::User) &&
                value <= static_cast<std::int32_t>(SessionLeaveReason::MapUnload);
        if (valid)
            reason = static_cast<SessionLeaveReason>(value);
    }
    const bool accepted = valid && LeaveSession(reason);
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(accepted);
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
        "MP_EndSession", "bool", "string reason (optional)",
        "End the generic multiplayer session and route to its exit map");
    const hta::m3d::eScriptError matchmaking_error =
        script_server->registerGlobalFunction(&lua_start_matchmaking,
            "MP_StartMatchmaking", "bool",
            "int required, string targetMap, string exitMap, int waitTimeoutSeconds, bool friendlyFire",
            "Start a generic host-authoritative match roster");
    const hta::m3d::eScriptError spawn_match_error =
        script_server->registerGlobalFunction(&lua_add_spawn,
            "MP_AddSpawn", "bool", "vector position, float yaw, int belong",
            "Add a generic match spawn transform before the roster locks");
    const hta::m3d::eScriptError spawn_components_error =
        script_server->registerGlobalFunction(&lua_add_spawn_components,
            "MP_AddSpawnComponents", "bool",
            "float x, float y, float z, float rotation, int belong",
            "Generic scalar bridge used by the structured MP.AddSpawn table");
    (void)spawn_components_error;
    const hta::m3d::eScriptError state_error =
        script_server->registerGlobalFunction(&lua_get_session_state,
            "MP_GetSessionState", "string, int, int, int, bool, int, float", "",
            "Read state, connected/ready/required counts, wait mode and remaining time");
    const hta::m3d::eScriptError leave_error =
        script_server->registerGlobalFunction(&lua_leave_session,
            "MP_LeaveSession", "bool", "string reason (optional)",
            "Leave the generic multiplayer session with a generic reason");
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
    constexpr const char* kMpTableProgram = R"lua(
if type(MP) ~= "table" then MP = {} end
MP.StartMatchmaking = function(options, targetMap, exitMap, waitTimeoutSeconds, friendlyFire)
    if type(options) == "table" then
        local required = options.requiredPlayers
        local target = options.targetMap
        local exit = options.exitMap or ""
        local timeout = options.waitTimeoutSeconds
        local fire = options.friendlyFire
        if required == nil or target == nil or fire == nil then return false end
        return MP_StartMatchmaking(required, target, exit, timeout, fire)
    end
    -- Temporary positional compatibility for existing scripts.
    return MP_StartMatchmaking(options, targetMap, exitMap, waitTimeoutSeconds, friendlyFire)
end
MP.AddSpawn = function(options, yaw, belong)
    if type(options) == "table" and options.position ~= nil then
        local source = options.position
        if type(source) ~= "table" then return false end
        local x = source.x or source[1]
        local y = source.y or source[2]
        local z = source.z or source[3]
        if x == nil or y == nil or z == nil or options.rotation == nil or
           options.belong == nil then return false end
        -- ScriptServer converts a three-element Lua sequence to ARGTYPE_VECTOR.
        return MP_AddSpawnComponents(x, y, z, options.rotation, options.belong)
    end
    -- Temporary positional compatibility for existing scripts.
    return MP_AddSpawn(options, yaw, belong)
end
MP.GetSessionState = function()
    local state, connected, ready, required, infinite, remaining_ms, remaining_s =
        MP_GetSessionState()
    return {
        state = state,
        connectedPlayers = connected,
        readyPlayers = ready,
        requiredPlayers = required,
        infiniteWait = infinite,
        remainingWaitMilliseconds = remaining_ms,
        remainingWaitSeconds = remaining_s,
        -- Compatibility aliases.
        connected = connected, ready = ready, required = required,
        infinite = infinite, remaining = remaining_ms,
        remaining_ms = remaining_ms
    }
end
MP.EndSession = function(reason)
    if reason == nil then return MP_EndSession() end
    return MP_EndSession(reason)
end
MP.LeaveSession = function(reason)
    if reason == nil then return MP_LeaveSession() end
    return MP_LeaveSession(reason)
end
)lua";
    const hta::m3d::eScriptError table_error = script_server->execute(
        kMpTableProgram, "kraken_mp_table_install");
    if (table_error != hta::m3d::eScriptError::SUCCESS &&
        table_error != hta::m3d::eScriptError::NOT_INITIALIZED)
        LOG_ERROR("Lua MP table installation failed code=%u",
                  static_cast<unsigned>(table_error));
    LOG_INFO("Lua multiplayer API registered weapon=%u loot_request=%u loot_spawn=%u world_publish=%u world_object_publish=%u world_request=%u world_authority=%u begin=%u configure=%u end=%u matchmaking=%u spawn=%u state=%u leave=%u active=%u host=%u authority=%u authority_or_offline=%u publish_entity=%u entity=%u impact_poll=%u",
             static_cast<unsigned>(error), static_cast<unsigned>(request_error),
             static_cast<unsigned>(spawn_error),
             static_cast<unsigned>(publish_world_loot_error),
             static_cast<unsigned>(publish_world_loot_object_error),
             static_cast<unsigned>(request_world_loot_error),
             static_cast<unsigned>(query_world_loot_authority_error),
             static_cast<unsigned>(begin_error),
             static_cast<unsigned>(configure_error), static_cast<unsigned>(end_error),
             static_cast<unsigned>(matchmaking_error),
             static_cast<unsigned>(spawn_match_error),
             static_cast<unsigned>(state_error),
             static_cast<unsigned>(leave_error),
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
    std::uint32_t max_peers = static_cast<std::uint32_t>(kMaxSessionPlayers);
    std::uint8_t match_max_players = static_cast<std::uint8_t>(kMaxSessionPlayers);
    JoinPolicy join_policy = JoinPolicy::ClosedAfterStart;
    bool auto_host_coop = false;
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

std::optional<std::filesystem::path> engine_install_root()
{
    const hta::CMiracle3d* const application = hta::CMiracle3d::Instance();
    if (application == nullptr || application->GetImageName().c_str() == nullptr)
        return std::nullopt;
    std::filesystem::path image(application->GetImageName().c_str());
    if (image.empty())
        return std::nullopt;
    if (image.is_relative()) {
        const char* const startup = application->GetStartupFolder().c_str();
        image = startup != nullptr && startup[0] != '\0'
            ? std::filesystem::path(startup) / image
            : std::filesystem::absolute(image);
    }
    image = image.lexically_normal();
    const std::filesystem::path root = image.parent_path();
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error) || error)
        return std::nullopt;
    return root;
}

void add_fingerprint_input(const std::filesystem::path& root,
                           const std::filesystem::path& candidate,
                           std::vector<std::filesystem::path>& inputs)
{
    if (candidate.empty())
        return;
    std::filesystem::path absolute = candidate.is_absolute()
        ? candidate : root / candidate;
    absolute = absolute.lexically_normal();
    std::error_code error;
    if (!std::filesystem::exists(absolute, error) || error)
        return;
    const std::filesystem::path relative = absolute.lexically_relative(root);
    if (relative.empty() || relative.string().starts_with(".."))
        return;
    (void)insert_resource_fingerprint_input_coverage(inputs, relative);
}

std::vector<std::filesystem::path> active_resource_inputs(
    const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> inputs;
    // These are original install resources, not mod-provided metadata.  The
    // active serverdyn/level path is added below when the native level has
    // finished loading, so LAN identity follows the actual original scene.
    add_fingerprint_input(root, "data", inputs);
    add_fingerprint_input(root, "serverdyn", inputs);

    const hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::m3d::Level* const level = server != nullptr ? server->m_level : nullptr;
    if (level != nullptr) {
        // DynamicScene/serverdyn is the authoritative active-scene input.
        // m_levelPath names the terrain resource and is not a serverdyn path.
        if (level->m_dsSrvName.c_str() != nullptr &&
            level->m_dsSrvName.c_str()[0] != '\0') {
            const hta::CStr full_path = level->GetFullPathNameA(level->m_dsSrvName);
            add_fingerprint_input(root, full_path.c_str() != nullptr
                                           ? full_path.c_str() : "", inputs);
            add_fingerprint_input(root, level->m_dsSrvName.c_str(), inputs);
        }
        // Retain the raw terrain path as a fingerprint input, but never pass
        // it through GetFullPathNameA: it is a different native resource.
        add_fingerprint_input(root, level->m_levelPath.c_str(), inputs);
        add_fingerprint_input(root, level->m_serversname.c_str(), inputs);
        add_fingerprint_input(root, level->m_staticServers.c_str(), inputs);
        add_fingerprint_input(root, level->m_dsSrvName.c_str(), inputs);
    }
    if (inputs.empty())
        add_fingerprint_input(root, ".", inputs);
    return inputs;
}

std::optional<SessionIdentity> production_session_identity()
{
    // Version fields are build identity.  Resource identity is computed from
    // the running executable and original level/serverdyn resources; no
    // environment-provided path or digest can make a production session pass.
    SessionIdentity identity{};
    identity.protocol_version = "kraken-match-1";
    identity.kraken_version = "kraken-0.1.1";
    identity.game_version = "hta-0.1.0";
    identity.mod_version = "kraken-mod-0.1.1";

    const std::optional<std::filesystem::path> root = engine_install_root();
    if (!root) {
        LOG_ERROR("multiplayer identity unavailable; native application image root is unavailable");
        return std::nullopt;
    }
    ResourceFingerprintRequest request{};
    request.install_root = *root;
    request.inputs = active_resource_inputs(*root);
    // Session identity covers shared game/mod resources, never a player's
    // profile, saves, local renderer/editor settings, or Kraken policy. Those
    // are intentionally different between peers and are not replicated world
    // content.
    request.policy.ignored_directories = {"data/profiles"};
    request.policy.ignored_files = {
        "data/config.cfg", "data/kraken.ini", "data/m3deditor.cfg"};
    const ResourceFingerprintResult fingerprint = fingerprint_resources(request);
    if (!fingerprint.succeeded() || fingerprint.digest.empty()) {
        LOG_ERROR("multiplayer identity unavailable; resource fingerprint failed code=%s path=%s error=%s",
                  resource_fingerprint_error_name(fingerprint.error),
                  fingerprint.error_path.string().c_str(),
                  fingerprint.error_message.c_str());
        return std::nullopt;
    }
    identity.resource_fingerprint = fingerprint.digest;
    LOG_INFO("multiplayer resource identity digest=%s files=%llu bytes=%llu ignored_dirs=%llu ignored_files=%llu",
             fingerprint.digest.c_str(),
             static_cast<unsigned long long>(fingerprint.stats.file_count),
             static_cast<unsigned long long>(fingerprint.stats.total_bytes),
             static_cast<unsigned long long>(fingerprint.stats.ignored_directory_count),
             static_cast<unsigned long long>(fingerprint.stats.ignored_file_count));
    if (!is_valid_session_identity(identity)) {
        LOG_ERROR("multiplayer identity unavailable; refusing production session (identity fields must be non-empty)");
        return std::nullopt;
    }
    return identity;
}

void populate_session_identity(SessionConfig& config,
                               const SessionIdentity& identity)
{
    config.protocol_version = identity.protocol_version;
    config.kraken_version = identity.kraken_version;
    config.game_version = identity.game_version;
    config.mod_version = identity.mod_version;
    config.resource_fingerprint = identity.resource_fingerprint;
}

template <typename Value>
decltype(auto) config_member_value(const Value& value)
{
    if constexpr (requires { value.value; })
        return (value.value);
    else
        return (value);
}

template <typename Value>
JoinPolicy config_join_policy_value(const Value& value)
{
    const auto& raw = config_member_value(value);
    using Raw = std::remove_cvref_t<decltype(raw)>;
    if constexpr (std::is_same_v<Raw, JoinPolicy>) {
        return raw;
    } else if constexpr (std::is_same_v<Raw, std::string>) {
        if (const std::optional<JoinPolicy> parsed = parse_join_policy(raw))
            return *parsed;
        if (raw == "join-in-progress" || raw == "joinInProgress")
            return JoinPolicy::JoinInProgress;
        return JoinPolicy::ClosedAfterStart;
    } else if constexpr (std::is_integral_v<Raw> || std::is_enum_v<Raw>) {
        return static_cast<std::uint32_t>(raw) != 0
            ? JoinPolicy::JoinInProgress : JoinPolicy::ClosedAfterStart;
    } else {
        return JoinPolicy::ClosedAfterStart;
    }
}

template <typename Value>
std::uint8_t config_max_players_value(const Value& value)
{
    const auto& raw = config_member_value(value);
    using Raw = std::remove_cvref_t<decltype(raw)>;
    if constexpr (std::is_integral_v<Raw> || std::is_enum_v<Raw>) {
        return static_cast<std::uint8_t>((std::clamp)(
            static_cast<std::int64_t>(raw), std::int64_t{1},
            static_cast<std::int64_t>(kMaxSessionPlayers)));
    } else {
        return static_cast<std::uint8_t>(kMaxSessionPlayers);
    }
}

template <typename Value>
bool config_bool_value(const Value& value)
{
    const auto& raw = config_member_value(value);
    using Raw = std::remove_cvref_t<decltype(raw)>;
    if constexpr (std::is_same_v<Raw, bool>)
        return raw;
    else if constexpr (std::is_integral_v<Raw> || std::is_enum_v<Raw>)
        return static_cast<std::uint32_t>(raw) != 0;
    else
        return false;
}

template <typename ConfigType>
JoinPolicy config_join_policy(const ConfigType& config)
{
    if constexpr (requires { config.joinPolicy; })
        return config_join_policy_value(config.joinPolicy);
    else if constexpr (requires { config.join_policy; })
        return config_join_policy_value(config.join_policy);
    else if constexpr (requires { config.multiplayer_join_policy; })
        return config_join_policy_value(config.multiplayer_join_policy);
    else
        return JoinPolicy::ClosedAfterStart;
}

template <typename ConfigType>
std::uint8_t config_max_players(const ConfigType& config)
{
    if constexpr (requires { config.maxPlayers; })
        return config_max_players_value(config.maxPlayers);
    else if constexpr (requires { config.max_players; })
        return config_max_players_value(config.max_players);
    else if constexpr (requires { config.multiplayer_max_players; })
        return config_max_players_value(config.multiplayer_max_players);
    else
        return static_cast<std::uint8_t>(kMaxSessionPlayers);
}

template <typename ConfigType>
bool config_auto_host_coop(const ConfigType& config)
{
    if constexpr (requires { config.autoHostCoop; })
        return config_bool_value(config.autoHostCoop);
    else if constexpr (requires { config.auto_host_coop; })
        return config_bool_value(config.auto_host_coop);
    else if constexpr (requires { config.multiplayer_auto_host_coop; })
        return config_bool_value(config.multiplayer_auto_host_coop);
    else
        return false;
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
    result.match_max_players = config_max_players(config);
    result.join_policy = config_join_policy(config);
    result.auto_host_coop = environment_uint(
        "KRAKEN_MP_AUTO_HOST_COOP", config_auto_host_coop(config) ? 1 : 0,
        0, 1) != 0;
    result.max_peers = environment_uint("KRAKEN_MP_MAX_PEERS",
        (std::max)(config.multiplayer_max_peers.value,
                   static_cast<std::uint32_t>(result.match_max_players)),
        2, static_cast<std::uint32_t>(kMaxSessionPlayers));
    // An auto-hosted cooperative session is still the generic listen-server
    // authority; this does not expose an EFA-specific topology to Lua.
    if (result.auto_host_coop)
        result.host = true;
    result.spawn_together = environment_uint("KRAKEN_MP_SPAWN_TOGETHER",
        config.multiplayer_spawn_together.value, 0, 1) != 0;
    result.autostart = environment_uint("KRAKEN_MP_AUTOSTART", 1, 0, 1) != 0;
    result.auto_lan = environment_uint("KRAKEN_MP_AUTO_LAN", 1, 0, 1) != 0;
    if (result.auto_host_coop)
        result.auto_lan = false;
    return result;
}

bool send_match_payload(const PeerId peer, const MessageType type,
                        const std::vector<Byte>& payload)
{
    if (!g_state.session || peer == kInvalidPeer)
        return false;
    const TransportResult result = g_state.session->send(
        peer, type, Channel::Reliable, payload);
    if (!result)
        LOG_ERROR("match control send failed peer=%u type=%u code=%u", peer,
                  static_cast<unsigned>(type),
                  static_cast<unsigned>(result.code));
    return static_cast<bool>(result);
}

const MatchPlayer* match_player(const MatchPlayerId player_id)
{
    const auto found = std::find_if(
        g_state.match.players().begin(), g_state.match.players().end(),
        [player_id](const MatchPlayer& player) {
            return player.id == player_id;
        });
    return found == g_state.match.players().end() ? nullptr : &*found;
}

const MatchPlayer* match_player_for_peer(const PeerId peer)
{
    const auto found = std::find_if(
        g_state.match.players().begin(), g_state.match.players().end(),
        [peer](const MatchPlayer& player) { return player.peer == peer; });
    return found == g_state.match.players().end() ? nullptr : &*found;
}

MatchJipPeerBarrier* match_jip_barrier_for_peer(const PeerId peer)
{
    const auto found = std::find_if(
        g_state.match_jip_map_barriers.begin(),
        g_state.match_jip_map_barriers.end(),
        [peer](const MatchJipPeerBarrier& barrier) {
            return barrier.peer == peer;
        });
    return found == g_state.match_jip_map_barriers.end() ? nullptr : &*found;
}

void erase_match_jip_barrier(const PeerId peer)
{
    std::erase_if(g_state.match_jip_map_barriers,
                  [peer](const MatchJipPeerBarrier& barrier) {
                      return barrier.peer == peer;
                  });
}

bool host_peer_world_transfer_permitted(const PeerId peer)
{
    if (!g_state.is_host || g_state.match.state() == MatchState::Offline)
        return true;
    const MatchJipPeerBarrier* const barrier = match_jip_barrier_for_peer(peer);
    // Forming peers still own the lobby map, while Playing peers that have no
    // barrier have not completed the JIP map load yet.  In either case a
    // permissive "missing barrier" default leaks stale EntitySpawn metadata
    // from the previous map into the next descriptor snapshot.
    return barrier != nullptr &&
        match_jip_snapshot_permitted(barrier->barrier);
}

bool encode_and_send_match_ready_request(const PeerId peer)
{
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_ready_request(
        {g_state.match_epoch, g_state.match_roster_revision,
         g_state.match.state() == MatchState::Playing}, payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchReadyRequest, payload);
}

bool send_match_reject(const PeerId peer, const MatchRejectReason reason)
{
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_reject(
        {g_state.match_epoch == 0 ? 1u : g_state.match_epoch, reason}, payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchReject, payload);
}

bool send_match_ready(const PeerId peer, const std::uint32_t epoch,
                      const MatchPlayerId player_id, const NetId entity_id)
{
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_ready(
        {epoch == 0 ? 1u : epoch, player_id,
         entity_id == kInvalidNetId ? 1u : entity_id, true}, payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchReady, payload);
}

MatchRosterLock make_match_roster_lock()
{
    MatchRosterLock message{};
    message.session_epoch = g_state.match_epoch;
    message.roster_revision = g_state.match_roster_revision;
    message.required_players = g_state.match.config().required_players;
    message.max_players = g_state.match.config().max_players;
    message.join_policy = g_state.match.config().join_policy;
    message.friendly_fire = g_state.match.config().friendly_fire;
    for (const MatchPlayer& player : g_state.match.players()) {
        message.players.push_back({player.id, player.peer, player.entity_id,
                                  player.join_order, player.host, player.ready});
        if (const std::optional<MatchSpawn> spawn =
                g_state.match.spawn_for(player.id))
            message.spawns.push_back(*spawn);
    }
    return message;
}

bool send_match_roster_lock(const PeerId peer)
{
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_roster_lock(
        make_match_roster_lock(), payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchRosterLock, payload);
}

bool send_match_load_for_map(const PeerId peer, const std::string& target_map)
{
    const MatchConfig& config = g_state.match.config();
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_load(
        {g_state.match_epoch, g_state.match_roster_revision,
         target_map, config.exit_map, config.friendly_fire}, payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchLoad, payload);
}

bool send_match_load(const PeerId peer)
{
    return send_match_load_for_map(peer, g_state.match.config().target_map);
}

bool send_match_sync(const PeerId peer, const MatchPlayerId player_id,
                     const bool request)
{
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_sync(
        {g_state.match_epoch, g_state.match_roster_revision, player_id,
         g_state.world_journal.revision(), request}, payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchSync, payload);
}

bool send_match_play(const PeerId peer)
{
    std::vector<Byte> payload;
    const MatchCodecError encoded = encode_match_play(
        {g_state.match_epoch, g_state.match_roster_revision}, payload);
    return match_codec_succeeded(encoded) && send_match_payload(
        peer, MessageType::MatchPlay, payload);
}

bool send_world_transfer_payload(const PeerId peer, const MessageType type,
                                 const std::vector<Byte>& payload)
{
    if (g_state.is_host && !host_peer_world_transfer_permitted(peer)) {
        LOG_DEBUG("deferred world transfer peer=%u type=%u until JIP map-ready",
                  peer, static_cast<unsigned>(type));
        return false;
    }
    if (!vehicle_transfer_codec_succeeded(
            payload.empty() ? VehicleTransferCodecError::InputSizeMismatch
                            : VehicleTransferCodecError::None))
        return false;
    return send_match_payload(peer, type, payload);
}

bool send_world_snapshot(const PeerId peer)
{
    if (g_state.world_snapshot_payload.empty()) {
        std::vector<Byte> empty_snapshot;
        if (!world_state_snapshot_codec_succeeded(
                encode_world_state_snapshot(std::span<const ObjectRecord>{},
                                             empty_snapshot)))
            return false;
        g_state.world_snapshot_payload = std::move(empty_snapshot);
    }
    const WorldSnapshotTransfer snapshot{
        g_state.match_epoch, g_state.match_roster_revision,
        g_state.world_journal.capture_snapshot(g_state.world_snapshot_payload)};
    std::vector<Byte> payload;
    if (!vehicle_transfer_codec_succeeded(
            encode_world_snapshot_transfer(snapshot, payload)))
        return false;
    if (std::find(g_state.world_transfer_peers.begin(),
                  g_state.world_transfer_peers.end(), peer) ==
        g_state.world_transfer_peers.end())
        g_state.world_transfer_peers.push_back(peer);
    std::erase_if(g_state.world_ready_acks,
                  [peer](const auto& ack) { return ack.first == peer; });
    return send_world_transfer_payload(peer, MessageType::MatchWorldSnapshot,
                                       payload);
}

bool send_vehicle_descriptor(const PeerId peer, const MatchPlayerId player_id,
                             const NetId entity_id,
                             const EntityGeneration generation,
                             hta::ai::Vehicle& vehicle,
                             const LoadoutProfile* loadout)
{
    char archive_object_name[64]{};
    std::snprintf(archive_object_name, sizeof(archive_object_name),
                  "kraken_remote_vehicle_%u_%u", entity_id,
                  static_cast<unsigned>(generation));
    const VehicleDescriptorTransfer transfer{
        g_state.match_epoch, g_state.match_roster_revision, player_id,
        entity_id, generation,
        make_vehicle_descriptor(vehicle, loadout, archive_object_name)};
    if (transfer.descriptor.native_structure.empty()) {
        LOG_ERROR("vehicle descriptor archive capture failed peer=%u entity=%u",
                  peer, entity_id);
        return false;
    }
    std::vector<Byte> payload;
    if (!vehicle_transfer_codec_succeeded(
            encode_vehicle_descriptor_transfer(transfer, payload)))
        return false;
    return send_world_transfer_payload(peer, MessageType::MatchVehicleDescriptor,
                                       payload);
}

bool send_local_vehicle_descriptor(const PeerId peer)
{
    if (g_state.is_host || peer == kInvalidPeer || !g_state.session)
        return false;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player != nullptr
        ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return false;
    const NetId entity_id = g_state.local_entity_id == kInvalidNetId
        ? 1u : g_state.local_entity_id;
    EntityGeneration generation = kInitialEntityGeneration;
    (void)g_state.entities.lookup_generation(entity_id, generation);
    char archive_object_name[64]{};
    std::snprintf(archive_object_name, sizeof(archive_object_name),
                  "kraken_host_player_%u_%u", entity_id,
                  static_cast<unsigned>(generation));
    const VehicleDescriptorTransfer transfer{
        g_state.match_epoch, g_state.match_roster_revision, entity_id,
        entity_id, generation,
        make_vehicle_descriptor(*vehicle, nullptr, archive_object_name)};
    if (transfer.descriptor.native_structure.empty()) {
        LOG_ERROR("local vehicle descriptor archive capture failed entity=%u",
                  entity_id);
        return false;
    }
    std::vector<Byte> payload;
    if (!vehicle_transfer_codec_succeeded(
            encode_vehicle_descriptor_transfer(transfer, payload)))
        return false;
    const bool sent = send_world_transfer_payload(
        peer, MessageType::MatchVehicleDescriptor, payload);
    if (sent)
        g_state.local_descriptor_sent = true;
    return sent;
}

bool send_world_snapshot_and_descriptors(const PeerId peer)
{
    if (!send_world_snapshot(peer))
        return false;
    if (!send_quest_snapshot(peer))
        return false;
    bool descriptors_sent = true;
    auto send_entity_descriptor = [peer, &descriptors_sent](
                                      const MatchPlayerId player_id,
                                      const NetId entity_id,
                                      const EntityGeneration generation,
                                      hta::ai::Vehicle* vehicle,
                                      const LoadoutProfile* loadout,
                                      const bool required) {
        if (vehicle == nullptr) {
            if (required)
                descriptors_sent = false;
            return;
        }
        if (!send_vehicle_descriptor(peer, player_id, entity_id, generation,
                                     *vehicle, loadout))
            descriptors_sent = false;
    };
    EntityGeneration local_generation = kInitialEntityGeneration;
    (void)g_state.entities.lookup_generation(1, local_generation);
    send_entity_descriptor(1, 1, local_generation, find_vehicle(1), nullptr,
                           true);
    for (const PeerController& controller : g_state.controllers) {
        EntityGeneration generation = controller.generation;
        (void)g_state.entities.lookup_generation(controller.entity_id,
                                                  generation);
        send_entity_descriptor(controller.entity_id, controller.entity_id,
                               generation, find_vehicle(controller.entity_id),
                               controller.has_loadout ? &controller.loadout
                                                      : nullptr, true);
    }
    for (const HostEntity& entity : g_state.host_entities) {
        if (!entity.active)
            continue;
        send_entity_descriptor(entity.entity_id, entity.entity_id,
                               entity.generation, find_vehicle(entity.entity_id),
                               &entity.loadout, false);
    }
    return descriptors_sent;
}

void broadcast_new_world_deltas(const WorldRevision previous_revision)
{
    if (!g_state.is_host || g_state.match.state() == MatchState::Offline ||
        !g_state.session)
        return;
    std::vector<WorldDelta> deltas;
    if (!g_state.world_journal.deltas_after(g_state.world_journal.epoch(),
                                            previous_revision, deltas))
        return;
    for (const WorldDelta& delta : deltas) {
        const WorldDeltaTransfer transfer{
            g_state.match_epoch, g_state.match_roster_revision, delta};
        std::vector<Byte> payload;
        if (!vehicle_transfer_codec_succeeded(
                encode_world_delta_transfer(transfer, payload)))
            continue;
        for (const PeerId peer : g_state.world_transfer_peers)
            (void)send_world_transfer_payload(peer, MessageType::MatchWorldDelta,
                                               payload);
    }
}

std::string lua_string_literal(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20u) {
                char escaped[5]{};
                std::snprintf(escaped, sizeof(escaped), "\\%03u",
                              static_cast<unsigned>(character));
                result += escaped;
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

void notify_lua_match_map_transition(const MatchLoad& load)
{
    hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
    hta::m3d::ScriptServer* const script_server =
        kernel ? kernel->m_scriptServer : nullptr;
    if (script_server == nullptr)
        return;
    const std::string script =
        "if MP_OnMatchMapTransition ~= nil then MP_OnMatchMapTransition(" +
        lua_string_literal(load.target_map) + "," +
        lua_string_literal(load.exit_map) + ") end";
    const hta::m3d::eScriptError error = script_server->execute(
        script.c_str(), "kraken_match_map_transition");
    if (error != hta::m3d::eScriptError::SUCCESS &&
        error != hta::m3d::eScriptError::NOT_INITIALIZED)
        LOG_ERROR("Lua match map transition callback failed code=%u",
                  static_cast<unsigned>(error));
}

void queue_deferred_match_route(const std::string& map)
{
    g_state.quest_trigger_provenance.begin_map_load(
        QuestProjectionSourceKind::TriggerNormal);
    if (map.empty()) {
        g_state.deferred_route = RuntimeState::DeferredRoute::MainMenu;
        g_state.deferred_route_map.clear();
    } else {
        g_state.deferred_route = RuntimeState::DeferredRoute::LoadMap;
        g_state.deferred_route_map = map;
    }
}

void queue_deferred_match_exit_route(const std::string& map,
                                     const SessionLeaveReason reason)
{
    g_state.deferred_exit_marker_pending = true;
    g_state.deferred_exit_confirmation = RuntimeState::DeferredRoute::None;
    g_state.deferred_exit_confirmation_map.clear();
    g_state.deferred_exit_was_host = g_state.is_host;
    g_state.deferred_exit_reason = reason;
    queue_deferred_match_route(map);
}

void apply_deferred_route()
{
    if (g_state.deferred_route == RuntimeState::DeferredRoute::None)
        return;
    const RuntimeState::DeferredRoute route = g_state.deferred_route;
    const std::string map = g_state.deferred_route_map;
    g_state.deferred_route = RuntimeState::DeferredRoute::None;
    g_state.deferred_route_map.clear();
    hta::CMiracle3d* const application = hta::CMiracle3d::Instance();
    if (application == nullptr)
        return;
    if (route == RuntimeState::DeferredRoute::MainMenu) {
        application->StartMainMenu();
        LOG_INFO("deferred multiplayer route StartMainMenu requested");
        if (g_state.deferred_exit_marker_pending)
            g_state.deferred_exit_confirmation = route;
        return;
    }
    hta::CStr level(map.c_str());
    const bool loaded = application->LoadMap(
        level, false, nullptr, nullptr, hta::ai::ObjContainer::SAVE_LEVEL);
    LOG_INFO("deferred multiplayer route LoadMap map=%s result=%u",
             map.c_str(), loaded ? 1u : 0u);
    if (g_state.deferred_exit_marker_pending && !loaded) {
        LOG_INFO("KRAKEN_MP_ACCEPT session_exit role=%s reason=%s route=%s result=%s",
                 g_state.deferred_exit_was_host ? "host" : "client",
                 leave_reason_name(g_state.deferred_exit_reason), map.c_str(),
                 "failed");
        g_state.deferred_exit_marker_pending = false;
        g_state.deferred_exit_confirmation = RuntimeState::DeferredRoute::None;
        g_state.deferred_exit_confirmation_map.clear();
    } else if (g_state.deferred_exit_marker_pending) {
        g_state.deferred_exit_confirmation = route;
        g_state.deferred_exit_confirmation_map = map;
    }
}

void confirm_deferred_exit_route(hta::CMiracle3d* const application)
{
    if (!g_state.deferred_exit_marker_pending || application == nullptr ||
        g_state.deferred_exit_confirmation == RuntimeState::DeferredRoute::None)
        return;

    const RuntimeState::DeferredRoute route = g_state.deferred_exit_confirmation;
    const bool completed = route == RuntimeState::DeferredRoute::MainMenu
        ? application->GetCurGameMode() == hta::GS_MAINMENU
        : application->GetCurGameMode() == hta::GS_GAME &&
          current_level_name() == g_state.deferred_exit_confirmation_map;
    if (!completed)
        return;

    const char* const route_name = route == RuntimeState::DeferredRoute::MainMenu
        ? "main_menu" : g_state.deferred_exit_confirmation_map.c_str();
    LOG_INFO("KRAKEN_MP_ACCEPT session_exit role=%s reason=%s route=%s result=success",
             g_state.deferred_exit_was_host ? "host" : "client",
             leave_reason_name(g_state.deferred_exit_reason), route_name);
    g_state.deferred_exit_marker_pending = false;
    g_state.deferred_exit_confirmation = RuntimeState::DeferredRoute::None;
    g_state.deferred_exit_confirmation_map.clear();
}

void reset_match_state() noexcept
{
    g_state.match.reset();
    g_state.authority_log_context = {};
    g_state.authority_log_mask = 0;
    g_state.match_request = {};
    g_state.visible_match_state = MatchState::Offline;
    g_state.match_jip_pending.clear();
    g_state.match_jip_map_barriers.clear();
    g_state.local_match_player_id = kInvalidMatchPlayerId;
    g_state.client_jip_join_requested = false;
    g_state.client_jip_map_load.reset();
    g_state.match_epoch = 0;
    g_state.match_roster_revision = 0;
    g_state.match_request_started = {};
    g_state.match_request_pending = false;
    g_state.match_loading_announced = false;
    g_state.diagnostic_accept_status_valid = false;
    g_state.diagnostic_accept_status = {};
    g_state.diagnostic_gameplay_open_logged = false;
    g_state.diagnostic_first_input_logged = false;
    g_state.diagnostic_host_control_ready_logged = false;
    g_state.world_transfer_expected = false;
    g_state.world_snapshot_committed = false;
    g_state.quest_replica_active = false;
    g_state.quest_trigger_provenance.preserve_session_reset();
    g_state.quest_projection_host.reset();
    g_state.quest_projection_client.reset();
    g_state.quest_projection_map_namespace.clear();
    g_state.quest_projection_sample_ready = false;
    g_state.quest_projection_committed = false;
    g_state.quest_play_pending = false;
    g_state.quest_local_state_logged = false;
    g_state.world_sync_request_received = false;
    g_state.world_sync_sent = false;
    g_state.world_sync_peer = kInvalidPeer;
    g_state.expected_vehicle_descriptors.clear();
    g_state.received_vehicle_descriptors.clear();
    g_state.local_assigned_spawn.reset();
    g_state.local_assigned_spawn_applied = false;
    g_state.local_descriptor_sent = false;
    g_state.client_join_failure_pending = false;
    g_state.world_ready_acks.clear();
}

bool start_matchmaking_impl(const std::uint8_t required_players,
                            const char* const target_map,
                            const char* const exit_map,
                            const std::int32_t wait_timeout_seconds,
                            const bool friendly_fire)
{
    if (required_players == 0 || target_map == nullptr || target_map[0] == '\0' ||
        wait_timeout_seconds < -1)
        return false;
    if (!g_state.session && !BeginSession())
        return false;

    MatchConfig config{};
    config.required_players = required_players;
    config.max_players = g_lifecycle_config
        ? g_lifecycle_config->match_max_players
        : static_cast<std::uint8_t>(kMaxSessionPlayers);
    config.join_policy = g_lifecycle_config
        ? g_lifecycle_config->join_policy : JoinPolicy::ClosedAfterStart;
    config.wait_timeout = wait_timeout_seconds > 0
        ? std::optional<std::chrono::milliseconds>{
              std::chrono::seconds(wait_timeout_seconds)}
        : std::nullopt;
    config.target_map = target_map;
    config.exit_map = exit_map != nullptr ? exit_map : "";
    config.friendly_fire = friendly_fire;
    if (config.required_players > config.max_players)
        return false;

    g_state.match_request = config;
    g_state.match_request_started = Clock::now();
    g_state.match_request_pending = true;
    g_state.visible_match_state = MatchState::Forming;

    // The client may select its map before the host has answered.  Retain the
    // request and answer the host's reliable ReadyRequest after handshake.
    if (!g_state.is_host) {
        emit_match_accept_marker();
        return true;
    }
    if (g_state.match.state() != MatchState::Offline)
        return g_state.match.state() == MatchState::Forming;

    g_state.match_epoch = g_state.session_epoch != 0
        ? g_state.session_epoch : allocate_session_epoch();
    g_state.match_roster_revision = 1;
    const NetId host_entity = g_state.local_entity_id == kInvalidNetId
        ? 1u : g_state.local_entity_id;
    if (!g_state.match.start(config, 1, host_entity, Clock::now())) {
        reset_match_state();
        return false;
    }
    g_state.match_request_pending = false;
    // A session may have published an Offline/co-op baseline before a mod
    // starts matchmaking.  The locked target-map baseline has a different
    // native object graph and must be emitted again after its map barrier.
    g_state.spawn_publications.clear();
    g_state.loadout_publications.clear();

    // A peer can finish transport handshaking before the menu invokes
    // StartMatchmaking.  Add it to the unready forming roster now and use the
    // same reliable request path as a newly connected peer.
    for (const PeerId peer : g_state.peers) {
        const MatchPlayerId player_id = peer + 1;
        if (!g_state.match.add_player(player_id, peer, peer + 1)) {
            (void)send_match_reject(peer, MatchRejectReason::RosterFull);
            (void)g_state.transport.disconnect(peer);
            continue;
        }
        (void)encode_and_send_match_ready_request(peer);
    }
    update_lan_advertisement();
    emit_match_accept_marker();
    return true;
}

bool add_match_spawn_impl(const float x, const float y, const float z,
                          const float yaw, const std::int32_t belong)
{
    if (!g_state.is_host || g_state.match.state() != MatchState::Forming ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(yaw))
        return false;
    return g_state.match.add_spawn({x, y, z, yaw, belong});
}

bool start_join_in_progress_snapshot(const PeerId peer,
                                     const MatchPlayerId player_id)
{
    if (std::find(g_state.match_jip_pending.begin(),
                  g_state.match_jip_pending.end(), player_id) !=
        g_state.match_jip_pending.end())
        return true;
    const MatchJipPeerBarrier* const barrier = match_jip_barrier_for_peer(peer);
    if (barrier == nullptr || !match_jip_snapshot_permitted(barrier->barrier)) {
        LOG_WARNING("JIP snapshot denied before map-ready peer=%u player=%u",
                    peer, player_id);
        return false;
    }
    const MatchPlayer* const player = match_player_for_peer(peer);
    if (player == nullptr || player->id != player_id)
        return false;
    PeerController* const controller = find_controller(peer);
    if (controller == nullptr) {
        LOG_ERROR("JIP snapshot denied: peer controller missing peer=%u", peer);
        return false;
    }
    if (!controller->host_vehicle_active) {
        // The upload normally arrived before MatchReady.  If it did not,
        // create_host_remote_vehicle selects the exact host clone fallback;
        // either path completes before any JIP world snapshot is released.
        if (create_host_remote_vehicle(*controller) == nullptr) {
            LOG_ERROR("JIP snapshot denied: client vehicle materialization failed peer=%u entity=%u",
                      peer, controller->entity_id);
            (void)send_match_reject(peer, MatchRejectReason::InvalidRequest);
            (void)g_state.transport.disconnect(peer);
            erase_match_jip_barrier(peer);
            return false;
        }
    }
    g_state.match_jip_pending.push_back(player_id);
    // Reliable metadata/baseline precedes the sync acknowledgement.  The
    // existing entity/world baseline is the generic snapshot path; no map or
    // EFA object is reconstructed by this controller.
    (void)send_match_roster_lock(peer);
    publish_host_baseline_to_peer(peer);
    send_world_loot_baseline(peer);
    if (!send_world_snapshot_and_descriptors(peer)) {
        (void)send_match_reject(peer, MatchRejectReason::InvalidRequest);
        (void)g_state.transport.disconnect(peer);
        return false;
    }
    (void)send_match_sync(peer, player_id, true);
    return true;
}

void tick_match(const Clock::time_point now)
{
    if (!g_state.is_host || !g_state.session ||
        g_state.match.state() == MatchState::Offline)
        return;

    if (g_state.match.state() == MatchState::Forming) {
        if (g_state.match.update(now) != MatchAction::BeginLoading)
            return;
        g_state.visible_match_state = MatchState::Loading;
        g_state.match_loading_announced = true;
        g_state.match_jip_map_barriers.clear();
        emit_match_accept_marker();

        // The coordinator removes unready peers while locking the roster.
        // Explicitly reject and disconnect them so a closed roster cannot
        // continue sending gameplay traffic after the load barrier.
        for (const PeerId peer : g_state.peers) {
            const MatchPlayer* const player = match_player_for_peer(peer);
            if (player == nullptr) {
                (void)send_match_reject(peer, MatchRejectReason::NotReady);
                (void)g_state.transport.disconnect(peer);
                continue;
            }
            MatchJipPeerBarrier barrier{peer, {}};
            if (!begin_match_jip_map_load(
                    barrier.barrier, g_state.match_epoch,
                    g_state.match_roster_revision, player->id,
                    player->entity_id)) {
                (void)send_match_reject(peer,
                                       MatchRejectReason::InvalidRequest);
                (void)g_state.transport.disconnect(peer);
                continue;
            }
            g_state.match_jip_map_barriers.push_back(std::move(barrier));
            (void)send_match_roster_lock(peer);
            (void)send_match_load(peer);
        }
        notify_lua_match_map_transition({
            g_state.match_epoch, g_state.match_roster_revision,
            g_state.match.config().target_map, g_state.match.config().exit_map,
            g_state.match.config().friendly_fire});
        queue_deferred_match_route(g_state.match.config().target_map);
        update_lan_advertisement();
        return;
    }

    if (g_state.match.state() == MatchState::Loading &&
        g_state.match_loading_announced) {
        // Loading a map is synchronous inside the native engine, but peers
        // complete it independently.  Never construct a remote native graph
        // or release a snapshot while either side can still own the lobby.
        if (current_level_name() != g_state.match.config().target_map)
            return;
        for (const MatchPlayer& player : g_state.match.players()) {
            if (player.host || player.peer == kInvalidPeer)
                continue;
            const MatchJipPeerBarrier* const barrier =
                match_jip_barrier_for_peer(player.peer);
            if (barrier == nullptr ||
                !match_jip_snapshot_permitted(barrier->barrier))
                return;
        }
        if (!g_state.match.begin_synchronizing())
            return;
        g_state.visible_match_state = MatchState::Synchronizing;
        (void)g_state.match.set_synchronized(1, true);
        bool has_remote = false;
        for (const MatchPlayer& player : g_state.match.players()) {
            if (player.host || player.peer == kInvalidPeer)
                continue;
            has_remote = true;
            PeerController* const controller = find_controller(player.peer);
            if (controller == nullptr ||
                create_host_remote_vehicle(*controller) == nullptr) {
                LOG_ERROR("initial snapshot denied: client vehicle materialization failed peer=%u entity=%u",
                          player.peer, player.entity_id);
                (void)send_match_reject(player.peer,
                                         MatchRejectReason::InvalidRequest);
                (void)g_state.transport.disconnect(player.peer);
                continue;
            }
            publish_host_baseline_to_peer(player.peer);
            send_world_loot_baseline(player.peer);
            if (!send_world_snapshot_and_descriptors(player.peer)) {
                (void)send_match_reject(player.peer,
                                         MatchRejectReason::InvalidRequest);
                (void)g_state.transport.disconnect(player.peer);
                continue;
            }
            if (MatchJipPeerBarrier* const barrier =
                    match_jip_barrier_for_peer(player.peer))
                (void)mark_match_jip_snapshot_started(barrier->barrier);
            (void)send_match_sync(player.peer, player.id, true);
        }
        if (!has_remote && g_state.match.begin_playing()) {
            g_state.visible_match_state = MatchState::Playing;
            g_state.match_loading_announced = false;
            emit_gameplay_open_marker();
            emit_match_accept_marker();
        }
        update_lan_advertisement();
        return;
    }

    if (g_state.match.state() == MatchState::Synchronizing) {
        const bool synchronized = std::all_of(
            g_state.match.players().begin(), g_state.match.players().end(),
            [](const MatchPlayer& player) { return player.synchronized; });
        if (!synchronized || !g_state.match.begin_playing())
            return;
        g_state.visible_match_state = MatchState::Playing;
        g_state.match_loading_announced = false;
        emit_gameplay_open_marker();
        emit_match_accept_marker();
        for (const MatchPlayer& player : g_state.match.players())
            if (!player.host && player.peer != kInvalidPeer)
                (void)send_match_play(player.peer);
        update_lan_advertisement();
    }
}

bool is_world_baseline_message(const MessageType type) noexcept
{
    return type == MessageType::EntitySpawn || type == MessageType::Snapshot ||
           type == MessageType::Loadout || type == MessageType::WorldLootSpawn ||
           type == MessageType::WorldLootBaseline ||
           type == MessageType::WorldLootDelta ||
           type == MessageType::WorldLootRemove;
}

void queue_world_baseline_packet(const SessionEvent& event)
{
    if (!is_world_baseline_message(event.message_type))
        return;
    g_state.world_join_packets.push_back({event.message_type, event.payload});
    if (g_state.world_join_packets.size() > 512)
        g_state.world_join_packets.erase(g_state.world_join_packets.begin());
}

void apply_world_baseline_packet(const MessageType type,
                                 const std::vector<Byte>& payload,
                                 const PeerId peer)
{
    SessionEvent event{};
    event.type = SessionEventType::Message;
    event.message_type = type;
    event.channel = Channel::Reliable;
    event.peer = peer;
    event.payload = payload;
    switch (type) {
    case MessageType::EntitySpawn: receive_entity_spawn(event); break;
    case MessageType::Snapshot: receive_remote_snapshot(event); break;
    case MessageType::Loadout: receive_loadout(event); break;
    case MessageType::WorldLootSpawn: receive_world_loot_spawn(event); break;
    case MessageType::WorldLootBaseline: receive_world_loot_baseline(event); break;
    case MessageType::WorldLootDelta: receive_world_loot_delta(event); break;
    case MessageType::WorldLootRemove: receive_world_loot_remove(event); break;
    default: break;
    }
}

bool descriptor_set_complete()
{
    return !g_state.expected_vehicle_descriptors.empty() &&
        std::all_of(g_state.expected_vehicle_descriptors.begin(),
                    g_state.expected_vehicle_descriptors.end(),
                    [](const NetId entity_id) {
                        return std::find(g_state.received_vehicle_descriptors.begin(),
                                         g_state.received_vehicle_descriptors.end(),
                                         entity_id) !=
                               g_state.received_vehicle_descriptors.end();
                    });
}

void maybe_send_world_ready_and_sync()
{
    if (g_state.is_host || !g_state.world_snapshot_committed ||
        !g_state.quest_projection_committed ||
        !g_state.world_sync_request_received || g_state.world_sync_sent ||
        !descriptor_set_complete() || g_state.world_sync_peer == kInvalidPeer)
        return;
    WorldTransferReady ready{
        g_state.match_epoch, g_state.match_roster_revision,
        g_state.local_entity_id == kInvalidNetId ? 1u : g_state.local_entity_id,
        g_state.world_join_epoch, g_state.world_join_revision,
        static_cast<std::uint16_t>(g_state.received_vehicle_descriptors.size())};
    std::vector<Byte> payload;
    if (!vehicle_transfer_codec_succeeded(
            encode_world_transfer_ready(ready, payload)) ||
        !send_world_transfer_payload(g_state.world_sync_peer,
                                      MessageType::MatchWorldReady, payload) ||
        !send_match_sync(g_state.world_sync_peer, ready.player_id, false))
        return;
    g_state.world_sync_sent = true;
    LOG_INFO("KRAKEN_MP_ACCEPT world_ready epoch=%u revision=%llu descriptors=%u",
             ready.world_epoch,
             static_cast<unsigned long long>(ready.world_revision),
             static_cast<unsigned>(ready.descriptor_count));
}

hta::ai::Obj* world_engine_object(const HostObjectId object_id)
{
    if (object_id == kInvalidHostObjectId)
        return nullptr;
    const std::optional<HostWorldEngineHandle> handle =
        g_state.host_world_registry.handle_for(object_id);
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    return handle && server != nullptr && server->m_pObjects != nullptr
        ? server->m_pObjects->GetEntityByObjId(*handle) : nullptr;
}

bool apply_world_object_created(const ObjectCreatedEvent& event)
{
    if (event.object_id == kInvalidHostObjectId || event.type_id == 0)
        return false;
    if (world_engine_object(event.object_id) != nullptr)
        return true;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return false;
    char name[72]{};
    std::snprintf(name, sizeof(name), "kraken_world_%llu",
                  static_cast<unsigned long long>(event.object_id));
    const ObjId engine_id = server->m_pObjects->
        CreateNewObjectWithSuspendedPostLoad(
            static_cast<std::int32_t>(event.type_id), name, -1, 0);
    if (engine_id < 0)
        return false;
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(engine_id);
    if (object == nullptr) {
        server->m_pObjects->AddObjIdToRemove(engine_id);
        return false;
    }
    object->PostLoad();
    server->m_pObjects->AddObjToUpdate(object);
    const HostWorldRegistryResult bound = is_host_world_dynamic_id(event.object_id)
        ? g_state.host_world_registry.bind_dynamic(engine_id, event.object_id)
        : g_state.host_world_registry.install_static(engine_id, event.object_id);
    if (!host_world_registry_succeeded(bound)) {
        server->m_pObjects->AddObjIdToRemove(engine_id);
        return false;
    }
    LOG_INFO("world replay created object=%llu type=%u engine=%d",
             static_cast<unsigned long long>(event.object_id), event.type_id,
             engine_id);
    return true;
}

bool apply_world_object_despawned(const ObjectDespawnedEvent& event)
{
    hta::ai::Obj* const object = world_engine_object(event.object_id);
    if (object == nullptr)
        return false;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return false;
    server->m_pObjects->AddObjIdToRemove(object->GetId());
    const std::optional<HostWorldGeneration> generation =
        g_state.host_world_registry.generation_for(event.object_id);
    if (generation)
        (void)g_state.host_world_registry.remove_id(event.object_id, *generation);
    return true;
}

bool apply_world_parent_added(const ParentChildAddedEvent& event)
{
    hta::ai::Obj* const parent = world_engine_object(event.parent_id);
    hta::ai::Obj* const child = world_engine_object(event.child_id);
    if (parent == nullptr || child == nullptr)
        return false;
    parent->AddChild(child);
    return true;
}

bool apply_world_parent_removed(const ParentChildRemovedEvent& event)
{
    hta::ai::Obj* const parent = world_engine_object(event.parent_id);
    hta::ai::Obj* const child = world_engine_object(event.child_id);
    if (parent == nullptr || child == nullptr)
        return false;
    return parent->RemoveChild(child);
}

bool apply_world_runtime_changed(const RuntimeChangedEvent& event)
{
    if (event.value.empty())
        return true;
    // Broad generic objects currently rely on the next native-archive payload
    // package for complete non-default runtime state.  The descriptor v2 path
    // is the only typed runtime-state boundary available here.  A generic
    // WorldMutation runtime blob has no native ABI, so retaining it in the
    // ordered applier is not enough to claim materialization; do not fake
    // opaque runtime support in this follow-up.
    LOG_WARNING("world mutation runtime rejected unsupported opaque payload object=%llu bytes=%u",
                static_cast<unsigned long long>(event.object_id),
                static_cast<unsigned>(event.value.size()));
    return false;
}

bool apply_world_property_changed(const PropertyChangedEvent& event)
{
    // AIParam/property encoding is engine-specific and no typed wire seam is
    // available here.  Reject instead of reporting a fake native success.
    LOG_WARNING("world mutation property rejected unsupported opaque payload object=%llu property=%u bytes=%u removed=%u",
                static_cast<unsigned long long>(event.object_id),
                event.property_id, static_cast<unsigned>(event.value.size()),
                event.removed ? 1u : 0u);
    return false;
}

bool apply_world_damage(const DamageEvent& event)
{
    if (active_client_replica()) {
        ++g_state.client_blocked_damage_attempt_count;
        return false;
    }
    if (!std::isfinite(event.amount) || event.amount <= 0.0f)
        return false;
    hta::ai::Obj* const target = world_engine_object(event.target_id);
    if (target == nullptr)
        return false;
    hta::ai::Obj* const source = event.source_id == kInvalidHostObjectId
        ? nullptr : world_engine_object(event.source_id);
    if (event.source_id != kInvalidHostObjectId && source == nullptr)
        return false;

    hta::ai::DamageInfo info{};
    info.attackerId = source != nullptr ? source->GetId() : 0;
    info.attackingAgentId = info.attackerId;
    info.bDamageFriends = true;
    info.damage = event.amount;
    info.damageType = static_cast<hta::ai::DamageType>(event.damage_type);
    info.decalId = -1;
    if (hta::ai::Vehicle* const vehicle = vehicle_from_object(target)) {
        // The original function is the verified Vehicle::InflictDamage
        // boundary used by authoritative impact presentation.
        if (g_state.vehicle_inflict_damage_original == nullptr)
            return false;
        return call_original_vehicle_inflict_damage(
            g_state.vehicle_inflict_damage_original, vehicle, info);
    }
    else {
        // Obj::InflictDamage is the verified generic native boundary for
        // typed non-vehicle objects (including Chest subclasses).
        target->InflictDamage(info);
    }
    return true;
}

bool apply_world_destroyed(const DestroyedEvent& event)
{
    hta::ai::Obj* const object = world_engine_object(event.object_id);
    if (object == nullptr)
        return false;
    object->_SetDeadStatus();
    return true;
}

bool apply_world_fx(const FxEvent& event)
{
    // No generic effect/presentation ABI is exposed by the native layer.  FX
    // remains fail-closed until a typed native effect path is available.
    LOG_WARNING("world mutation fx rejected unsupported effect object=%llu effect=%u bytes=%u",
                static_cast<unsigned long long>(event.object_id),
                event.effect_id, static_cast<unsigned>(event.payload.size()));
    return false;
}

void bind_snapshot_static_objects(const WorldStateSnapshot& snapshot)
{
    const std::string map_namespace = static_world_map_namespace();
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (map_namespace.empty() || server == nullptr ||
        server->m_pObjects == nullptr)
        return;
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        if (object == nullptr || object->GetDeletedStatus() ||
            object->m_bIsUpdating || object->m_bNeedPostLoad ||
            object->m_bMustCreateVisualPart)
            continue;
        const std::optional<StaticWorldPostLoadRecord> identity =
            static_world_record_for_object(*server, *object, map_namespace);
        if (!identity)
            continue;
        const StaticWorldId id = static_world_id_hash(
            static_world_canonical_key(*identity));
        const auto snapshot_object = std::find_if(
            snapshot.objects.begin(), snapshot.objects.end(),
            [id](const ObjectRecord& record) { return record.object_id == id; });
        if (snapshot_object == snapshot.objects.end())
            continue;
        const HostWorldRegistryResult bound =
            g_state.host_world_registry.install_static(
                static_cast<HostWorldEngineHandle>(object->GetId()), id);
        if (!host_world_registry_succeeded(bound) &&
            bound != HostWorldRegistryResult::Collision)
            LOG_WARNING("snapshot static object bind rejected id=%llu objId=%d code=%u",
                        static_cast<unsigned long long>(id), object->GetId(),
                        static_cast<unsigned>(bound));
    }
}

bool apply_world_snapshot_payload(ByteView payload)
{
    WorldStateSnapshot snapshot{};
    if (!world_state_snapshot_codec_succeeded(
            decode_world_state_snapshot(payload, snapshot)) ||
        !g_state.world_mutation_applier)
        return false;
    g_state.world_snapshot = snapshot;
    bind_snapshot_static_objects(snapshot);
    g_state.world_mutation_applier->reset(
        g_state.world_join_epoch, g_state.world_join_revision);
    WorldStateSnapshotVisitor visitor{};
    visitor.create_record = [](const ObjectRecord& record) {
        // Static records are expected to already exist in the original map;
        // dynamic records must cross the real native creation boundary before
        // the engine-neutral applier state is installed.  This is also what
        // lets Chest/item records materialize instead of becoming bookkeeping
        // ghosts only visible to the applier.
        if (is_host_world_dynamic_id(record.object_id) &&
            world_engine_object(record.object_id) == nullptr &&
            !apply_world_object_created(ObjectCreatedEvent{
                record.object_id, record.type_id}))
            return false;
        if (world_engine_object(record.object_id) == nullptr)
            return false;
        const WorldMutationApplyResult installed =
            g_state.world_mutation_applier->install_object(
                record.object_id, record.type_id, record.parent_id);
        if (!world_mutation_apply_succeeded(installed) &&
            installed != WorldMutationApplyResult::ObjectIdCollision)
            return false;
        if (record.parent_id != kInvalidHostObjectId) {
            const std::optional<HostWorldEngineHandle> handle =
                g_state.host_world_registry.handle_for(record.object_id);
            const std::optional<HostWorldGeneration> generation =
                g_state.host_world_registry.generation_for(record.object_id);
            if (!handle || !generation ||
                !host_world_registry_succeeded(
                    g_state.host_world_registry.set_parent(
                        *handle, *generation, record.parent_id)))
                return false;
        }
        return true;
    };
    visitor.relationship = [](const HostObjectId parent,
                              const HostObjectId child) {
        return g_state.world_mutation_applier != nullptr &&
               g_state.world_mutation_applier->contains(parent) &&
               g_state.world_mutation_applier->contains(child);
    };
    visitor.runtime = [](const HostObjectId object, const ByteView value) {
        return apply_world_runtime_changed(RuntimeChangedEvent{object,
            std::vector<Byte>(value.begin(), value.end())});
    };
    visitor.property = [](const HostObjectId object, const PropertyId property,
                          const ByteView value) {
        return apply_world_property_changed(PropertyChangedEvent{
            object, property, std::vector<Byte>(value.begin(), value.end()),
            false});
    };
    return world_state_snapshot_apply_succeeded(
        apply_world_state_snapshot(snapshot, visitor));
}

bool apply_world_mutation_delta(const WorldDelta& delta)
{
    if (!g_state.world_mutation_applier)
        return false;
    const WorldMutationApplyResult result = g_state.world_mutation_applier->apply(
        delta, WorldMutationSource{TransportRole::Server,
                                   ReplicationSourceContext::NetworkReplay,
                                   g_state.world_sync_peer});
    if (!world_mutation_apply_succeeded(result))
        LOG_WARNING("world mutation replay rejected epoch=%u revision=%llu code=%u",
                    delta.epoch,
                    static_cast<unsigned long long>(delta.revision),
                    static_cast<unsigned>(result));
    return world_mutation_apply_succeeded(result);
}

void receive_world_snapshot(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable)
        return;
    WorldSnapshotTransfer transfer{};
    if (!vehicle_transfer_codec_succeeded(
            decode_world_snapshot_transfer(event.payload, transfer)) ||
        (g_state.match_epoch != 0 && g_state.match_epoch != transfer.session_epoch))
        return;
    g_state.match_epoch = transfer.session_epoch;
    g_state.match_roster_revision = transfer.roster_revision;
    if (g_state.world_join_barrier.begin(transfer.snapshot) ==
        WorldJoinResult::InvalidSnapshot)
        return;
    g_state.world_join_epoch = transfer.snapshot.epoch;
    g_state.world_join_revision = transfer.snapshot.revision;
    for (const WorldDeltaTransfer& pending : g_state.world_join_pending_deltas)
        (void)g_state.world_join_barrier.accept_delta(pending.delta);
    g_state.world_join_pending_deltas.clear();
    const WorldJoinResult committed = g_state.world_join_barrier.commit(
        transfer.snapshot,
        [](ByteView payload) {
            ScopedReplaySuppression replay(g_state.world_replay_depth);
            const world_authority::ScopedWorldExecutionContext execution_scope(
                current_world_execution_context(true, false));
            std::optional<ReplayGuard> observer_replay;
            if (g_state.world_observer)
                observer_replay.emplace(g_state.world_observer->suppress_replay());
            if (!apply_world_snapshot_payload(payload))
                return false;
            for (const auto& packet : g_state.world_join_packets)
                apply_world_baseline_packet(packet.first, packet.second,
                                             g_state.world_sync_peer);
            g_state.world_join_packets.clear();
            return true;
        },
        [](const WorldDelta& delta) {
            ScopedReplaySuppression replay(g_state.world_replay_depth);
            const world_authority::ScopedWorldExecutionContext execution_scope(
                current_world_execution_context(true, false));
            std::optional<ReplayGuard> observer_replay;
            if (g_state.world_observer)
                observer_replay.emplace(g_state.world_observer->suppress_replay());
            return apply_world_mutation_delta(delta);
        });
    g_state.world_snapshot_committed = committed == WorldJoinResult::Ready;
    if (g_state.world_snapshot_committed) {
        LOG_INFO("KRAKEN_MP_ACCEPT snapshot_committed epoch=%u revision=%llu descriptors=%u",
                 transfer.snapshot.epoch,
                 static_cast<unsigned long long>(transfer.snapshot.revision),
                 static_cast<unsigned>(g_state.received_vehicle_descriptors.size()));
    }
    maybe_commit_quest_projection();
    maybe_send_world_ready_and_sync();
}

void receive_world_delta(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable)
        return;
    WorldDeltaTransfer transfer{};
    if (!vehicle_transfer_codec_succeeded(
            decode_world_delta_transfer(event.payload, transfer)) ||
        (g_state.match_epoch != 0 && g_state.match_epoch != transfer.session_epoch))
        return;
    if (g_state.world_join_barrier.state() == WorldJoinState::Idle) {
        g_state.world_join_pending_deltas.push_back(std::move(transfer));
        return;
    }
    const WorldJoinResult result = g_state.world_join_barrier.accept_delta(
        transfer.delta);
    if (result == WorldJoinResult::Gap || result == WorldJoinResult::Overflow ||
        result == WorldJoinResult::ResnapshotRequired)
        LOG_WARNING("world delta requires resnapshot peer=%u epoch=%u revision=%llu code=%u",
                    event.peer, transfer.delta.epoch,
                    static_cast<unsigned long long>(transfer.delta.revision),
                    static_cast<unsigned>(result));
}

void receive_vehicle_descriptor(const SessionEvent& event)
{
    if (event.channel != Channel::Reliable)
        return;
    VehicleDescriptorTransfer transfer{};
    if (!vehicle_transfer_codec_succeeded(
            decode_vehicle_descriptor_transfer(event.payload, transfer)) ||
        (g_state.match_epoch != 0 && g_state.match_epoch != transfer.session_epoch))
        return;
    const NativeObjectArchiveErrorCode archive_error =
        validate_native_object_archive(ByteView(
            transfer.descriptor.native_structure.data(),
            transfer.descriptor.native_structure.size()));
    if (archive_error != NativeObjectArchiveErrorCode::None) {
        LOG_ERROR("reject vehicle descriptor archive peer=%u entity=%u error=%s",
                  event.peer, transfer.entity_id,
                  native_object_archive_error_name(archive_error));
        if (g_state.is_host) {
            (void)send_match_reject(event.peer, MatchRejectReason::InvalidRequest);
            (void)g_state.transport.disconnect(event.peer);
        } else {
            (void)LeaveSession(SessionLeaveReason::User);
        }
        return;
    }
    if (g_state.is_host) {
        PeerController* const controller = find_controller(event.peer);
        const MatchPlayer* const player = match_player_for_peer(event.peer);
        if (controller == nullptr || player == nullptr ||
            player->id != transfer.player_id ||
            player->entity_id != transfer.entity_id ||
            controller->entity_id != transfer.entity_id ||
            controller->generation != transfer.generation) {
            LOG_WARNING("reject vehicle descriptor upload peer=%u entity=%u",
                        event.peer, transfer.entity_id);
            return;
        }
        controller->descriptor = std::move(transfer.descriptor);
        controller->has_descriptor = true;
        LOG_INFO("host accepted client vehicle descriptor peer=%u entity=%u prototype=%d",
                 event.peer, controller->entity_id,
                 controller->descriptor.prototype_id);
        return;
    }
    RemoteEntity* const remote = find_or_add_remote(transfer.entity_id);
    if (remote != nullptr) {
        if (remote->has_spawn && remote->prototype_id >= 0 &&
            remote->prototype_id != transfer.descriptor.prototype_id) {
            LOG_ERROR("discarding vehicle descriptor prototype mismatch entity=%u "
                      "spawn=%d descriptor=%d",
                      transfer.entity_id, remote->prototype_id,
                      transfer.descriptor.prototype_id);
            return;
        }
        remote->descriptor = std::move(transfer.descriptor);
        remote->has_descriptor = true;
        remote->prototype_id = remote->descriptor.prototype_id;
    }
    if (std::find(g_state.received_vehicle_descriptors.begin(),
                  g_state.received_vehicle_descriptors.end(), transfer.entity_id) ==
        g_state.received_vehicle_descriptors.end())
        g_state.received_vehicle_descriptors.push_back(transfer.entity_id);
    maybe_send_world_ready_and_sync();
}

void receive_world_ready(const SessionEvent& event)
{
    if (!g_state.is_host || event.channel != Channel::Reliable)
        return;
    WorldTransferReady ready{};
    if (!vehicle_transfer_codec_succeeded(
            decode_world_transfer_ready(event.payload, ready)) ||
        ready.session_epoch != g_state.match_epoch)
        return;
    const MatchPlayer* player = match_player_for_peer(event.peer);
    if (player == nullptr || player->id != ready.player_id)
        return;
    const auto found = std::find_if(
        g_state.world_ready_acks.begin(), g_state.world_ready_acks.end(),
        [event](const auto& ack) { return ack.first == event.peer; });
    if (found == g_state.world_ready_acks.end())
        g_state.world_ready_acks.push_back({event.peer, ready.world_revision});
    else
        found->second = ready.world_revision;
    LOG_INFO("KRAKEN_MP_ACCEPT world_ready peer=%u epoch=%u revision=%llu descriptors=%u",
             event.peer, ready.world_epoch,
             static_cast<unsigned long long>(ready.world_revision),
             static_cast<unsigned>(ready.descriptor_count));
}

void handle_match_message(const SessionEvent& event)
{
    if (event.channel != Channel::Reliable)
        return;
    switch (event.message_type) {
    case MessageType::MatchReadyRequest: {
        MatchReadyRequest request{};
        if (!match_codec_succeeded(decode_match_ready_request(
                event.payload, request)))
            return;
        if (g_state.is_host)
            return;
        g_state.match_epoch = request.session_epoch;
        g_state.match_roster_revision = request.roster_revision;
        g_state.client_jip_join_requested = request.join_in_progress;
        if (!request.join_in_progress)
            g_state.client_jip_map_load.reset();
        if (g_state.visible_match_state != MatchState::Playing)
            g_state.visible_match_state = MatchState::Forming;
        const MatchPlayerId player_id = g_state.local_entity_id == kInvalidNetId
            ? 1u : g_state.local_entity_id;
        const NetId entity_id = g_state.local_entity_id == kInvalidNetId
            ? 1u : g_state.local_entity_id;
        // Upload the rich v2 descriptor before Ready.  If the native player
        // vehicle is not available yet, the host deliberately falls back to
        // its validated loadout/prototype clone rather than blocking the
        // roster indefinitely.
        hta::ai::Player* const local_player = hta::ai::Player::Instance();
        const bool native_vehicle_available = local_player != nullptr &&
            local_player->GetVehicle() != nullptr;
        if (!g_state.local_descriptor_sent && native_vehicle_available &&
            !send_local_vehicle_descriptor(event.peer)) {
            LOG_ERROR("match join failed: local native vehicle archive upload rejected");
            (void)LeaveSession(SessionLeaveReason::User);
            return;
        }
        if (!native_vehicle_available)
            LOG_WARNING("match descriptor upload deferred; host loadout clone fallback remains active");
        (void)send_match_ready(event.peer, request.session_epoch, player_id,
                               entity_id);
        break;
    }
    case MessageType::MatchReady: {
        MatchReady ready{};
        if (!g_state.is_host || !match_codec_succeeded(
                decode_match_ready(event.payload, ready)) ||
            ready.session_epoch != g_state.match_epoch)
            return;
        const MatchPlayer* player = match_player_for_peer(event.peer);
        if (player == nullptr || player->id != ready.player_id ||
            player->entity_id != ready.entity_id) {
            // Entity ids are host-assigned.  A client can only acknowledge the
            // identity associated with its transport peer.
            (void)send_match_reject(event.peer, MatchRejectReason::InvalidRequest);
            return;
        }
        if (g_state.match.state() == MatchState::Forming) {
            (void)g_state.match.set_ready(player->id, ready.ready);
            emit_match_accept_marker();
            break;
        }
        if (g_state.match.state() == MatchState::Playing && ready.ready) {
            if (!g_state.match.set_ready(player->id, true))
                break;
            // A repeated Ready is harmless, but must not skip the map barrier.
            if (match_jip_barrier_for_peer(event.peer) != nullptr)
                break;
            const std::string loaded_map = current_level_name();
            const std::string& target_map = loaded_map.empty()
                ? g_state.match.config().target_map : loaded_map;
            MatchJipPeerBarrier barrier{event.peer, {}};
            if (!begin_match_jip_map_load(
                    barrier.barrier, g_state.match_epoch,
                    g_state.match_roster_revision, player->id,
                    player->entity_id))
                break;
            g_state.match_jip_map_barriers.push_back(barrier);
            if (!send_match_roster_lock(event.peer) ||
                !send_match_load_for_map(event.peer, target_map)) {
                erase_match_jip_barrier(event.peer);
                (void)send_match_reject(event.peer,
                                         MatchRejectReason::InvalidRequest);
                (void)g_state.transport.disconnect(event.peer);
            } else {
                LOG_INFO("JIP map load sent peer=%u epoch=%u roster=%u map=%s",
                         event.peer, g_state.match_epoch,
                         g_state.match_roster_revision, target_map.c_str());
            }
        }
        break;
    }
    case MessageType::MatchMapReady: {
        MatchMapReady acknowledgement{};
        if (!g_state.is_host || !match_codec_succeeded(
                decode_match_map_ready(event.payload, acknowledgement)))
            return;
        if (acknowledgement.session_epoch != g_state.match_epoch ||
            acknowledgement.roster_revision != g_state.match_roster_revision)
            return;
        const MatchPlayer* const player = match_player_for_peer(event.peer);
        MatchJipPeerBarrier* const barrier =
            match_jip_barrier_for_peer(event.peer);
        if (player == nullptr || barrier == nullptr ||
            player->id != acknowledgement.player_id ||
            player->entity_id != acknowledgement.entity_id) {
            LOG_INFO("ignored JIP map-ready identity peer=%u player=%u entity=%u",
                     event.peer, acknowledgement.player_id,
                     acknowledgement.entity_id);
            return;
        }
        const MatchJipBarrierResult result = accept_match_jip_map_ready(
            barrier->barrier, acknowledgement);
        if (result != MatchJipBarrierResult::Accepted) {
            LOG_INFO("ignored JIP map-ready peer=%u result=%u",
                     event.peer, static_cast<unsigned>(result));
            return;
        }
        if (g_state.match.state() == MatchState::Loading) {
            LOG_INFO("match map-ready accepted peer=%u player=%u entity=%u",
                     event.peer, acknowledgement.player_id,
                     acknowledgement.entity_id);
            break;
        }
        if (g_state.match.state() != MatchState::Playing)
            return;
        if (!start_join_in_progress_snapshot(event.peer, player->id))
            LOG_ERROR("JIP snapshot start failed after valid map-ready peer=%u",
                      event.peer);
        else {
            (void)mark_match_jip_snapshot_started(barrier->barrier);
            // Only persistent trigger/aim state is sent, and only after the
            // client proved that the target map is loaded. Shot events never
            // enter the JIP stream.
            (void)send_presentation_jip_state(event.peer);
        }
        break;
    }
    case MessageType::MatchRosterLock: {
        MatchRosterLock roster{};
        if (g_state.is_host || !match_codec_succeeded(
                decode_match_roster_lock(event.payload, roster)))
            return;
        if (g_state.match_epoch != 0 && g_state.match_epoch != roster.session_epoch)
            return;
        g_state.match_epoch = roster.session_epoch;
        g_state.match_roster_revision = roster.roster_revision;
        g_state.match_request.required_players = roster.required_players;
        g_state.match_request.max_players = roster.max_players;
        g_state.match_request.join_policy = roster.join_policy;
        g_state.match_request.friendly_fire = roster.friendly_fire;
        g_state.expected_vehicle_descriptors.clear();
        g_state.received_vehicle_descriptors.clear();
        g_state.local_match_player_id = kInvalidMatchPlayerId;
        g_state.local_assigned_spawn.reset();
        g_state.local_assigned_spawn_applied = false;
        for (std::size_t index = 0; index < roster.players.size(); ++index) {
            const MatchRosterPlayer& player = roster.players[index];
            g_state.expected_vehicle_descriptors.push_back(player.entity_id);
            if (player.entity_id == g_state.local_entity_id)
                g_state.local_match_player_id = player.player_id;
            if (player.entity_id == g_state.local_entity_id &&
                index < roster.spawns.size())
                g_state.local_assigned_spawn = roster.spawns[index];
        }
        g_state.world_transfer_expected = true;
        g_state.world_snapshot_committed = false;
        g_state.quest_replica_active = true;
        g_state.quest_projection_client.reset();
        g_state.quest_projection_committed = false;
        g_state.quest_play_pending = false;
        g_state.world_sync_request_received = false;
        g_state.world_sync_sent = false;
        g_state.world_sync_peer = event.peer;
        g_state.world_join_barrier.reset();
        g_state.world_join_pending_deltas.clear();
        g_state.world_join_packets.clear();
        if (g_state.visible_match_state == MatchState::Offline)
            g_state.visible_match_state = MatchState::Forming;
        emit_match_accept_marker();
        break;
    }
    case MessageType::MatchLoad: {
        MatchLoad load{};
        if (g_state.is_host || !match_codec_succeeded(
                decode_match_load(event.payload, load)))
            return;
        if (g_state.match_epoch != 0 && g_state.match_epoch != load.session_epoch)
            return;
        g_state.match_epoch = load.session_epoch;
        g_state.match_roster_revision = load.roster_revision;
        g_state.quest_replica_active = true;
        g_state.match_request.target_map = load.target_map;
        g_state.match_request.exit_map = load.exit_map;
        g_state.match_request.friendly_fire = load.friendly_fire;
        if (g_state.local_match_player_id == kInvalidMatchPlayerId ||
            g_state.local_entity_id == kInvalidNetId) {
            LOG_ERROR("match map load rejected: local roster identity is unavailable");
            return;
        }
        g_state.client_jip_map_load = ClientJipMapLoad{
            event.peer, load.session_epoch, load.roster_revision,
            g_state.local_match_player_id, g_state.local_entity_id,
            load.target_map, false, false};
        g_state.visible_match_state = MatchState::Loading;
        g_state.local_assigned_spawn_applied = false;
        emit_match_accept_marker();
        notify_lua_match_map_transition(load);
        queue_deferred_match_route(load.target_map);
        break;
    }
    case MessageType::MatchSync: {
        MatchSync sync{};
        if (!match_codec_succeeded(decode_match_sync(event.payload, sync)) ||
            (g_state.match_epoch != 0 && g_state.match_epoch != sync.session_epoch))
            return;
        g_state.match_epoch = sync.session_epoch;
        g_state.match_roster_revision = sync.roster_revision;
        if (g_state.is_host) {
            if (sync.request)
                return;
            const MatchPlayer* player = match_player_for_peer(event.peer);
            if (player == nullptr || player->id != sync.player_id)
                return;
            const auto ready_ack = std::find_if(
                g_state.world_ready_acks.begin(), g_state.world_ready_acks.end(),
                [event](const auto& ack) { return ack.first == event.peer; });
            if (ready_ack == g_state.world_ready_acks.end() ||
                ready_ack->second != sync.snapshot_revision)
                return;
            if (g_state.match.state() == MatchState::Synchronizing) {
                (void)g_state.match.set_synchronized(player->id, true);
            } else if (g_state.match.state() == MatchState::Playing &&
                       std::find(g_state.match_jip_pending.begin(),
                                 g_state.match_jip_pending.end(), player->id) !=
                           g_state.match_jip_pending.end()) {
                std::erase(g_state.match_jip_pending, player->id);
                (void)send_match_play(event.peer);
            } else {
                return;
            }
            break;
        }
        if (sync.request) {
            g_state.world_sync_request_received = true;
            g_state.world_sync_peer = event.peer;
            if (g_state.visible_match_state != MatchState::Playing)
                g_state.visible_match_state = MatchState::Synchronizing;
            maybe_send_world_ready_and_sync();
        }
        break;
    }
    case MessageType::MatchPlay: {
        MatchPlay play{};
        if (g_state.is_host || !match_codec_succeeded(
                decode_match_play(event.payload, play)))
            return;
        if (g_state.match_epoch != 0 && g_state.match_epoch != play.session_epoch)
            return;
        g_state.match_epoch = play.session_epoch;
        g_state.match_roster_revision = play.roster_revision;
        if (!g_state.quest_projection_committed) {
            g_state.quest_play_pending = true;
            g_state.visible_match_state = MatchState::Synchronizing;
            return;
        }
        g_state.visible_match_state = MatchState::Playing;
        g_state.match_request_pending = false;
        emit_gameplay_open_marker();
        emit_match_accept_marker();
        break;
    }
    case MessageType::MatchReject: {
        MatchReject reject{};
        if (g_state.is_host || !match_codec_succeeded(
                decode_match_reject(event.payload, reject)))
            return;
        LOG_INFO("match join rejected by peer=%u reason=%u", event.peer,
                 static_cast<unsigned>(reject.reason));
        g_state.quest_projection_client.reset();
        g_state.quest_projection_committed = false;
        g_state.quest_play_pending = false;
        g_state.visible_match_state = MatchState::Offline;
        g_state.match_request_pending = false;
        break;
    }
    case MessageType::MatchLeave: {
        MatchLeave leave{};
        if (!match_codec_succeeded(decode_match_leave(event.payload, leave)))
            return;
        const SessionLeaveReason reason = is_valid_match_leave_reason(leave.reason)
            ? static_cast<SessionLeaveReason>(leave.reason)
            : SessionLeaveReason::User;
        if (g_state.is_host) {
            if (leave.terminate_match) {
                // A terminating leave is authoritative only when the host
                // owns the session; rebroadcast through the normal host end
                // path so every client receives the same reason and route.
                (void)end_session_with_reason(reason);
                break;
            }
            if (const MatchPlayer* player = match_player_for_peer(event.peer)) {
                const MatchPlayerId player_id = player->id;
                (void)g_state.match.remove_player(player_id);
                std::erase(g_state.match_jip_pending, player_id);
            }
        } else if (leave.terminate_match) {
            (void)end_session_with_reason(reason);
        }
        break;
    }
    default:
        break;
    }
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

hta::ai::Vehicle* find_vehicle(NetId entity_id,
                               EntityGeneration expected_generation);
float health_fraction(const hta::ai::Vehicle& vehicle);
VehicleVector3 to_snapshot_vector(const hta::CVector& value);
hta::CVector to_engine_vector(const VehicleVector3& value);
void apply_visual_weapon_aim(hta::ai::Vehicle& vehicle,
                             const GunAttachmentIdentity& identity,
                             const VehicleVector3& aim_point,
                             float elapsed_time);
bool apply_network_weapon_ammo(hta::ai::Vehicle& vehicle,
                               const GunAttachmentIdentity& identity,
                               std::uint32_t shells_in_current_charge,
                               std::uint32_t shells_in_pool,
                               AmmoReloadState reload_state,
                               const ShotConfirmed* confirmed);
void apply_pending_confirmed_shots();
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

void suppress_client_dynamic_entities()
{
    if (runtime_authority(IsSessionActive(), g_state.is_host) !=
        RuntimeAuthority::ClientReplica)
        return;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const local_vehicle = player ? player->GetVehicle() : nullptr;
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
        if (object == nullptr || vehicle == nullptr || vehicle == local_vehicle ||
            object->GetDeletedStatus() || vehicle->m_AI.m_pDM == nullptr)
            continue;
        NetId entity_id = kInvalidNetId;
        EntityGeneration generation = kInvalidEntityGeneration;
        if (g_state.entities.lookup_net_id(vehicle->GetId(), entity_id,
                                           generation)) {
            // Host-created replica: the remote materializer owns its inert
            // presentation state and it remains eligible for snapshots.
            vehicle->m_AI.m_pDM = nullptr;
            vehicle->SetNpcMotionControllerId(-1);
            continue;
        }
        LOG_INFO("client replica suppressed locally generated dynamic entity objId=%d prototype=%d",
                 vehicle->GetId(), vehicle->GetPrototypeId());
        retire_network_vehicle(*server->m_pObjects, *object, *vehicle, true);
    }
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

std::string native_prototype_name(const hta::ai::Obj& object)
{
    const hta::ai::PrototypeInfo* const info = object.GetPrototypeInfo();
    return info != nullptr && info->m_prototypeName.c_str() != nullptr
        ? info->m_prototypeName.c_str() : std::string{};
}

std::string native_prototype_name(const std::int32_t prototype_id)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr)
        return {};
    const hta::ai::PrototypeInfo* const info =
        server->GetPrototypeInfo(prototype_id);
    return info != nullptr && info->m_prototypeName.c_str() != nullptr
        ? info->m_prototypeName.c_str() : std::string{};
}

template <typename T>
void append_bytes(std::vector<Byte>& output, const T& value)
{
    const std::size_t old_size = output.size();
    output.resize(old_size + sizeof(T));
    std::memcpy(output.data() + old_size, &value, sizeof(T));
}

void capture_native_modifier(const hta::ai::Modifier& native,
                             VehicleModifier& output)
{
    output.timeout = native.m_timeOut;
    output.operation = static_cast<VehicleModifierOperation>(
        static_cast<std::uint8_t>(native.m_Operation));
    output.magic_prototype_id = native.m_magicPrototypeId;
    output.property_name = native.m_PropertyName.c_str() != nullptr
        ? native.m_PropertyName.c_str() : "";
    output.sender_id = native.m_SenderID;
    output.value_type = static_cast<VehicleModifierValueType>(
        static_cast<std::uint8_t>(native.m_Value.GetType()));
    switch (native.m_Value.GetType()) {
    case hta::m3d::AIPARAM_VECTOR: {
        const hta::CVector value = native.m_Value.GetAsVector();
        append_bytes(output.value_payload, value.x);
        append_bytes(output.value_payload, value.y);
        append_bytes(output.value_payload, value.z);
        break;
    }
    case hta::m3d::AIPARAM_QUATERNION: {
        const hta::Quaternion value = native.m_Value.GetAsQuaternion();
        append_bytes(output.value_payload, value.x);
        append_bytes(output.value_payload, value.y);
        append_bytes(output.value_payload, value.z);
        append_bytes(output.value_payload, value.w);
        break;
    }
    case hta::m3d::AIPARAM_ID: {
        const std::int32_t value = native.m_Value.GetAsID();
        append_bytes(output.value_payload, value);
        break;
    }
    case hta::m3d::AIPARAM_FLOAT: {
        const float value = native.m_Value.GetAsFloat();
        append_bytes(output.value_payload, value);
        break;
    }
    case hta::m3d::AIPARAM_STRING: {
        const hta::CStr value = native.m_Value.GetAsStr();
        if (value.c_str() != nullptr)
            output.value_payload.assign(
                reinterpret_cast<const Byte*>(value.c_str()),
                reinterpret_cast<const Byte*>(value.c_str() + value.length()));
        break;
    }
    case hta::m3d::AIPARAM_ID_LIST: {
        const auto values = native.m_Value.GetAsIdList();
        for (std::size_t index = 0; index < values.size(); ++index)
            append_bytes(output.value_payload, values[index]);
        break;
    }
    case hta::m3d::AIPARAM_RANGE: {
        const hta::CVector2 value = native.m_Value.GetAsRange();
        append_bytes(output.value_payload, value.x);
        append_bytes(output.value_payload, value.y);
        break;
    }
    case hta::m3d::AIPARAM_STRING_LIST:
        // The native list is not a wire ABI.  Preserve its canonical textual
        // form through AIParam so replay does not guess at vector internals.
        if (const hta::CStr value = native.m_Value.ToStr();
            value.c_str() != nullptr)
            output.value_payload.assign(
                reinterpret_cast<const Byte*>(value.c_str()),
                reinterpret_cast<const Byte*>(value.c_str() + value.length()));
        break;
    case hta::m3d::AIPARAM_UNDEFINE:
        break;
    }
}

void capture_native_state(const hta::ai::Obj& object,
                          std::vector<VehicleAffix>& prefixes,
                          std::vector<VehicleAffix>& suffixes,
                          std::vector<VehicleModifier>& modifiers)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::AffixManager* const manager =
        server != nullptr ? server->GetAffixManager() : nullptr;
    auto append_affixes = [manager](const auto& ids, auto& output,
                                    const std::size_t limit) {
        for (std::size_t index = 0;
             index < ids.size() && output.size() < limit; ++index) {
            const std::int32_t id = ids[index];
            const hta::ai::Affix* const affix =
                manager != nullptr ? manager->GetAffixById(id) : nullptr;
            const hta::ai::AffixGroup* const group =
                affix != nullptr ? affix->GetAffixGroup() : nullptr;
            if (affix == nullptr || group == nullptr ||
                affix->GetName().c_str() == nullptr ||
                group->GetName().c_str() == nullptr)
                continue;
            output.push_back({id, affix->GetName().c_str(),
                              group->GetTargetResourceId(),
                              group->GetName().c_str()});
        }
    };
    append_affixes(object.m_appliedPrefixIds, prefixes,
                   kMaxVehicleDescriptorAffixes);
    append_affixes(object.m_appliedSuffixIds, suffixes,
                   kMaxVehicleDescriptorAffixes);
    for (std::size_t index = 0;
         index < object.m_modifiers.size() &&
         modifiers.size() < kMaxVehicleDescriptorModifiers; ++index) {
        VehicleModifier modifier{};
        capture_native_modifier(object.m_modifiers[index], modifier);
        modifiers.push_back(std::move(modifier));
    }
}

void capture_native_gun(const hta::ai::Gun& gun, VehicleGunState& state)
{
    state.present = true;
    state.barrel_index = gun.m_curBarrelIndex;
    state.rotation = gun.m_currentDesiredAlpha;
    state.charge_state = gun.GetChargeState() == hta::ai::Gun::csInCharging
        ? VehicleGunChargeState::Charging : VehicleGunChargeState::Ready;
    state.current_charge = gun.GetShellsInCurrentCharge();
    state.pool = gun.GetShellsInPool();
    state.reload = (std::max)(0.0f, gun.GetCurrentRechargingTime());
    state.firing = gun.m_bIsFiring;
}

bool capture_native_part(const hta::ai::VehiclePart& part,
                         const std::string& slot,
                         const VehicleInstanceId parent_id,
                         VehicleInstanceId& next_id,
                         std::vector<VehicleDescriptorNode>& output)
{
    constexpr VehicleInstanceId kMaxGraphInstances =
        static_cast<VehicleInstanceId>(kMaxVehicleDescriptorNodes);
    if (output.size() >= kMaxVehicleDescriptorNodes || next_id == 0 ||
        next_id > kMaxGraphInstances + 1) {
        LOG_ERROR("vehicle descriptor attachment limit exceeded objId=%d slot=%s count=%llu next=%u",
                  part.GetId(), slot.c_str(),
                  static_cast<unsigned long long>(output.size()), next_id);
        return false;
    }
    const std::string prototype_name = native_prototype_name(part);
    if (part.GetPrototypeId() < 0 || prototype_name.empty() || slot.empty()) {
        LOG_ERROR("vehicle descriptor attachment metadata invalid objId=%d slot=%s prototype=%d name=%s",
                  part.GetId(), slot.empty() ? "<empty>" : slot.c_str(),
                  part.GetPrototypeId(),
                  prototype_name.empty() ? "<empty>" : prototype_name.c_str());
        return false;
    }
    VehicleDescriptorNode node{};
    node.kind = VehicleDescriptorNodeKind::Attachment;
    node.instance_id = next_id++;
    node.parent_instance_id = parent_id;
    node.slot = slot;
    node.prototype_id = part.GetPrototypeId();
    node.prototype_name = prototype_name;
    node.skin = part.GetSkin();
    node.durability = (std::max)(0.0f, part.Durability().value().get());
    capture_native_state(part, node.prefixes, node.suffixes, node.modifiers);
    if (part.IsKindOf(hta::ai::Gun::p_classObject))
        capture_native_gun(static_cast<const hta::ai::Gun&>(part), node.gun);
    if (node.gun.present) {
        node.ammo = node.gun.current_charge;
        node.magazine = node.gun.pool;
        node.reload = node.gun.reload;
    }
    const VehicleInstanceId current_id = node.instance_id;
    output.push_back(std::move(node));
    if (!part.IsKindOf(hta::ai::CompoundVehiclePart::p_classObject))
        return true;
    const auto& compound = static_cast<const hta::ai::CompoundVehiclePart&>(part);
    for (auto iterator = compound.begin(); iterator != compound.end(); ++iterator) {
        if (iterator->second.vp == nullptr) {
            LOG_ERROR("vehicle descriptor compound attachment is null parentObjId=%d slot=%s",
                      part.GetId(), iterator->first.c_str());
            return false;
        }
        if (next_id > kMaxGraphInstances ||
            !capture_native_part(*iterator->second.vp, iterator->first.c_str(),
                                 current_id, next_id, output))
            return false;
    }
    return true;
}

bool capture_native_object_tree(const hta::ai::Obj& object,
                                VehicleDescriptorNode& node,
                                const std::size_t depth,
                                VehicleInstanceId& next_id)
{
    constexpr VehicleInstanceId kMaxGraphInstances =
        static_cast<VehicleInstanceId>(kMaxVehicleDescriptorNodes);
    if (depth > kMaxVehicleDescriptorCargoDepth || next_id == 0 ||
        next_id > kMaxGraphInstances + 1)
        return false;
    // The shipped Obj ABI exposes GetChildren only non-const.  Capture does
    // not mutate the graph; this cast is limited to that read-only traversal.
    auto& children = const_cast<hta::ai::Obj&>(object).GetChildren();
    if (children.size() > kMaxVehicleDescriptorCargoObjects)
        return false;
    for (auto iterator = children.begin(); iterator != children.end();
         ++iterator) {
        hta::ai::Obj* const child = iterator->second;
        if (child == nullptr || next_id > kMaxGraphInstances ||
            child->GetName() == nullptr ||
            child->GetName()[0] == '\0' || next_id == 0)
            return false;
        const std::string child_name = child->GetName();
        if (child_name.size() > kMaxVehicleDescriptorSlotLength)
            return false;
        if (node.cargo_objects.size() >= kMaxVehicleDescriptorCargoObjects)
            return false;
        VehicleDescriptorNode child_node{};
        child_node.kind = VehicleDescriptorNodeKind::Container;
        child_node.instance_id = next_id++;
        child_node.parent_instance_id = node.instance_id;
        child_node.slot = child_name;
        child_node.prototype_id = child->GetPrototypeId();
        child_node.prototype_name = native_prototype_name(*child);
        if (child_node.prototype_id < 0 || child_node.prototype_name.empty())
            return false;
        capture_native_state(*child, child_node.prefixes,
                             child_node.suffixes, child_node.modifiers);
        if (!capture_native_object_tree(*child, child_node, depth + 1,
                                        next_id))
            return false;
        node.cargo_objects.push_back(std::move(child_node));
    }
    return true;
}

bool capture_native_repository_contents(
    const hta::ai::GeomRepository* repository,
    std::vector<VehicleCargoStack>& stacks,
    std::vector<VehicleDescriptorNode>& objects,
    const VehicleCargoRepository repository_kind,
    const VehicleInstanceId parent_id, const std::size_t depth,
    VehicleInstanceId& next_id)
{
    constexpr VehicleInstanceId kMaxGraphInstances =
        static_cast<VehicleInstanceId>(kMaxVehicleDescriptorNodes);
    if (repository == nullptr || depth > kMaxVehicleDescriptorCargoDepth ||
        next_id == 0 || next_id > kMaxGraphInstances + 1)
        return repository == nullptr;
    if (repository->m_slots.size() > kMaxVehicleDescriptorCargoStacks) {
        LOG_ERROR("vehicle descriptor repository slot limit exceeded kind=%u slots=%llu",
                  static_cast<unsigned>(repository_kind),
                  static_cast<unsigned long long>(repository->m_slots.size()));
        return false;
    }
    for (std::size_t index = 0;
         index < repository->m_slots.size();
         ++index) {
        const hta::ai::GeomRepositoryItem item = repository->GetItem(
            static_cast<std::int32_t>(index));
        if (!item.IsValid())
            continue;
        if (item.bIsResourceItem()) {
            if (item.m_origin.x < 0 || item.m_origin.y < 0 ||
                item.m_origin.x > kMaxVehicleDescriptorCargoCoordinate ||
                item.m_origin.y > kMaxVehicleDescriptorCargoCoordinate) {
                LOG_ERROR("vehicle descriptor cargo coordinate invalid kind=%u index=%llu x=%d y=%d",
                          static_cast<unsigned>(repository_kind),
                          static_cast<unsigned long long>(index),
                          item.m_origin.x, item.m_origin.y);
                return false;
            }
            std::string name = native_prototype_name(item.GetPrototypeId());
            if (name.empty())
                name = native_prototype_name(item.GetResourceId());
            if (name.empty()) {
                LOG_WARNING("vehicle descriptor skipped unnamed cargo resource=%d",
                            item.GetResourceId());
                return false;
            }
            if (stacks.size() >= kMaxVehicleDescriptorCargoStacks)
                return false;
            stacks.push_back({item.GetResourceId(), name, item.GetAmount(),
                              {repository_kind, item.m_origin.x,
                               item.m_origin.y}});
            continue;
        }
        const hta::ai::Obj* const object = item.GetObj();
        if (object == nullptr || next_id > kMaxGraphInstances || objects.size() >=
                kMaxVehicleDescriptorCargoObjects || next_id == 0) {
            LOG_ERROR("vehicle descriptor cargo object invalid kind=%u index=%llu object=%p count=%llu next=%u",
                      static_cast<unsigned>(repository_kind),
                      static_cast<unsigned long long>(index), object,
                      static_cast<unsigned long long>(objects.size()), next_id);
            return false;
        }
        // GeomRepository::m_slots contains both a root container and the
        // hierarchical children stored inside it.  Children are already
        // serialized by capture_native_object_tree; emitting them again as
        // roots produces two placements at the same origin on restore.
        if (object->bHasParent()) {
            const hta::ai::Obj* const parent = object->GetParent();
            if (parent == nullptr ||
                parent->GetParentRepository() != repository) {
                LOG_ERROR("vehicle descriptor cargo child has an invalid repository parent kind=%u index=%llu objId=%d parentId=%d",
                          static_cast<unsigned>(repository_kind),
                          static_cast<unsigned long long>(index),
                          object->GetId(), object->GetParentId());
                return false;
            }
            continue;
        }
        const char* const object_name = object->GetName();
        std::string name = object_name != nullptr ? object_name : "";
        const std::string prototype_name = native_prototype_name(*object);
        if (name.empty()) {
            // Repository placement is the native stable identity for a root
            // cargo object. Stock/EFA objects are allowed to have no Obj
            // name; never substitute a process-local ObjId.
            char generated[64]{};
            std::snprintf(generated, sizeof(generated), "cargo-%u-%d-%d-p%d",
                          static_cast<unsigned>(repository_kind),
                          item.m_origin.x, item.m_origin.y,
                          object->GetPrototypeId());
            name = generated;
        }
        if (object->GetPrototypeId() < 0 || prototype_name.empty() ||
            name.size() > kMaxVehicleDescriptorSlotLength) {
            LOG_ERROR("vehicle descriptor cargo metadata invalid kind=%u index=%llu objId=%d prototype=%d prototypeName=%s objectName=%s",
                      static_cast<unsigned>(repository_kind),
                      static_cast<unsigned long long>(index), object->GetId(),
                      object->GetPrototypeId(),
                      prototype_name.empty() ? "<empty>" : prototype_name.c_str(),
                      name.empty() ? "<empty>" : name.c_str());
            return false;
        }
        VehicleDescriptorNode node{};
        node.kind = VehicleDescriptorNodeKind::Container;
        node.instance_id = next_id++;
        node.parent_instance_id = parent_id;
        node.slot = name;
        node.prototype_id = object->GetPrototypeId();
        node.prototype_name = prototype_name;
        node.cargo_placement = {repository_kind, item.m_origin.x,
                                item.m_origin.y};
        capture_native_state(*object, node.prefixes, node.suffixes,
                             node.modifiers);
        if (!capture_native_object_tree(*object, node, depth + 1, next_id))
            return false;
        objects.push_back(std::move(node));
    }
    return true;
}

bool capture_native_repository(const hta::ai::GeomRepository* repository,
                               VehicleDescriptor& descriptor,
                               const VehicleCargoRepository repository_kind,
                               VehicleInstanceId& next_id)
{
    return capture_native_repository_contents(
        repository, descriptor.cargo_stacks, descriptor.cargo_objects,
        repository_kind, 0, 0, next_id);
}

VehicleDescriptor make_vehicle_descriptor(hta::ai::Vehicle& vehicle,
                                          const LoadoutProfile* loadout,
                                          const char* archive_object_name)
{
    (void)loadout;
    VehicleDescriptor descriptor{};
    descriptor.prototype_id = vehicle.GetPrototypeId();
    descriptor.prototype_name = native_prototype_name(vehicle);
    descriptor.skin = vehicle.GetSkin();
    descriptor.health = (std::max)(0.0f, vehicle.GetHealth());
    descriptor.durability = (std::max)(0.0f, vehicle.GetFullDurability());
    descriptor.fuel = (std::max)(0.0f, vehicle.GetFuel());
    capture_native_state(vehicle, descriptor.prefixes, descriptor.suffixes,
                         descriptor.modifiers);
    VehicleInstanceId next_id = 1;
    bool metadata_complete = true;
    const auto names = vehicle.GetAttachedPartNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        const hta::CStr& native_slot = names[index];
        const hta::ai::VehiclePart* const part =
            vehicle.GetPartByName(native_slot);
        if (part == nullptr || native_slot.c_str() == nullptr ||
            !capture_native_part(*part, native_slot.c_str(), 0, next_id,
                                 descriptor.attachments))
            metadata_complete = false;
    }
    if (!capture_native_repository(vehicle.GetRepository(), descriptor,
                                   VehicleCargoRepository::Main, next_id) ||
        !capture_native_repository(vehicle.GetGroundRepository(), descriptor,
                                   VehicleCargoRepository::Ground, next_id))
        metadata_complete = false;
    const char* const original_name = vehicle.GetName();
    const std::string saved_name = original_name != nullptr ? original_name : "";
    if (!metadata_complete) {
        LOG_ERROR("vehicle descriptor typed repository/graph capture incomplete vehicleId=%d",
                  vehicle.GetId());
        descriptor.native_structure.clear();
    }
    else if (archive_object_name == nullptr || archive_object_name[0] == '\0' ||
        saved_name.empty()) {
        LOG_ERROR("vehicle descriptor archive context missing vehicleId=%d",
                  vehicle.GetId());
    } else {
        // NativeObjectArchive validates class/prototype/name on restore.  The
        // source object and the suspended destination therefore use the same
        // deterministic network name while the archive is captured; the
        // source object is restored immediately after the native save.
        vehicle.SetName(hta::CStr(archive_object_name));
        const NativeObjectArchiveResult archived =
            capture_native_object_archive(vehicle, descriptor.native_structure);
        vehicle.SetName(hta::CStr(saved_name.c_str()));
        if (!archived) {
            LOG_ERROR("vehicle descriptor native archive failed vehicleId=%d "
                      "error=%s detail=%s",
                      vehicle.GetId(),
                      native_object_archive_error_name(archived.error),
                      archived.detail.c_str());
            descriptor.native_structure.clear();
        }
    }
    if (descriptor.prototype_name.empty())
        LOG_ERROR("vehicle descriptor has no native prototype name entity vehicleId=%d prototype=%d",
                  vehicle.GetId(), descriptor.prototype_id);
    return descriptor;
}

std::string static_world_map_namespace()
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_level == nullptr)
        return {};
    const char* const name = server->m_level->GetLevelName();
    return name != nullptr ? std::string(name) : std::string{};
}

std::string normalize_static_world_path(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

std::optional<StaticWorldPostLoadRecord> static_world_record_for_object(
    hta::ai::CServer& server, hta::ai::Obj& object,
    const std::string& map_namespace)
{
    if (map_namespace.empty() || server.m_pObjects == nullptr)
        return std::nullopt;
    const hta::CStr full_name = server.m_pObjects->GetObjectFullName(
        object.m_name);
    const hta::CStr prototype_name = server.m_pObjects->GetPrototypeName(
        object.GetPrototypeId());
    if (full_name.c_str() == nullptr || prototype_name.c_str() == nullptr)
        return std::nullopt;
    StaticWorldPostLoadRecord record{};
    record.map_namespace = normalize_static_world_path(map_namespace);
    record.object_path = normalize_static_world_path(full_name.c_str());
    record.prototype_identity = prototype_name.c_str();
    if (validate_static_world_record(record) != StaticWorldIndexError::None)
        return std::nullopt;
    return record;
}

const char* first_xml_attribute(hta::m3d::cmn::XmlNode& node,
                                const std::initializer_list<const char*>& names)
{
    for (const char* const name : names) {
        const char* const value = node.GetAttribute(name);
        if (value != nullptr && value[0] != '\0')
            return value;
    }
    return nullptr;
}

std::vector<StaticWorldSourceRecord> load_active_dynamic_scene_source(
    const std::string& map_namespace)
{
    std::vector<StaticWorldSourceRecord> records;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::m3d::Level* const level = server != nullptr ? server->m_level : nullptr;
    if (map_namespace.empty() || level == nullptr ||
        level->m_dsSrvName.c_str() == nullptr ||
        level->m_dsSrvName.c_str()[0] == '\0')
        return records;

    const hta::CStr full_path = level->GetFullPathNameA(level->m_dsSrvName);
    if (full_path.c_str() == nullptr || full_path.c_str()[0] == '\0')
        return records;
    hta::CStr parse_error;
    ref_ptr<hta::m3d::cmn::XmlFile> xml_file =
        hta::m3d::cmn::ReadXmlFile(full_path.c_str(), &parse_error);
    if (!xml_file)
        return records;

    ref_ptr<hta::m3d::cmn::XmlNode> document =
        xml_file->CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
    if (!document)
        return records;

    // DynamicScene files in the shipped engine use one of these root names;
    // no fallback to the live ObjContainer is allowed because that would
    // classify runtime-created objects as static.
    constexpr const char* kRootNames[] = {
        "DynamicScene", "ServerDynamicScene", "Scene", "Objects"};
    ref_ptr<hta::m3d::cmn::XmlNode> root;
    for (const char* const name : kRootNames) {
        root = xml_file->CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
        if (root && xml_file->GetFirstChild(root, name))
            break;
        root = {};
    }
    if (!root)
        return records;

    const std::string normalized_namespace =
        normalize_static_world_path(map_namespace);
    const auto add_node = [&records, &normalized_namespace](
                              hta::m3d::cmn::XmlNode& node,
                              const std::string& parent_path) {
        const char* const name = first_xml_attribute(node, {"name", "Name"});
        if (name == nullptr || name[0] == '\0')
            return std::string{};
        const std::string path = parent_path.empty()
            ? normalize_static_world_path(name)
            : normalize_static_world_path(parent_path + "/" + name);
        const char* const prototype = first_xml_attribute(
            node, {"prototype", "Prototype", "prototypeName", "PrototypeName"});
        // A class name is not a prototype identity.  If the source record
        // does not carry a prototype, leave it unmatched/dynamic rather than
        // manufacturing a static identity from an unrelated field.
        if (prototype != nullptr && prototype[0] != '\0' && !path.empty()) {
            StaticWorldSourceRecord record{
                normalized_namespace, path, prototype};
            if (validate_static_world_record(record) ==
                    StaticWorldIndexError::None &&
                std::none_of(records.begin(), records.end(),
                    [&record](const StaticWorldSourceRecord& current) {
                        return static_world_canonical_key(current) ==
                               static_world_canonical_key(record);
                    }))
                records.push_back(std::move(record));
        }
        return path;
    };

    // Keep traversal explicit because XmlNode exposes filtered sibling APIs,
    // not a generic child iterator.
    const auto walk = [&xml_file, &add_node](auto&& self,
                                             hta::m3d::cmn::XmlNode& parent,
                                             const std::string& parent_path) -> void {
        constexpr const char* kNodeNames[] = {"Node", "Obj", "Vehicle", "Object"};
        for (const char* const node_name : kNodeNames) {
            ref_ptr<hta::m3d::cmn::XmlNode> child =
                xml_file->CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
            if (!child || !parent.GetFirstChild(child, node_name))
                continue;
            while (!child->IsEmpty()) {
                const std::string path = add_node(*child, parent_path);
                if (!path.empty())
                    self(self, *child, path);
                if (!child->GetNextSibling(child, node_name))
                    break;
            }
        }
    };
    walk(walk, *root, std::string{});
    return records;
}

bool install_static_world_source(const std::string& map_namespace,
                                 const std::vector<StaticWorldPostLoadRecord>&
                                     post_load_records,
                                 const std::vector<hta::ai::Obj*>& objects)
{
    if (g_state.static_world_loaded_map != map_namespace) {
        g_state.static_world_index.clear();
        g_state.static_world_stability.reset();
        g_state.static_world_ids_by_post_load_index.clear();
        g_state.static_world_original_bindings.clear();
        g_state.static_world_identity_stable = false;
        g_state.static_world_identity_last_error = StaticWorldIndexError::None;
        g_state.static_world_identity_error_map.clear();
        g_state.static_world_loaded_map = map_namespace;
        g_state.static_world_source_error_logged = false;
        g_state.static_world_source_loaded = false;
    }
    if (!g_state.static_world_index.empty())
        return true;
    (void)post_load_records;
    (void)objects;
    if (g_state.static_world_source_loaded)
        return true;
    const std::vector<StaticWorldSourceRecord> source =
        load_active_dynamic_scene_source(map_namespace);
    g_state.static_world_source_loaded = true;
    if (source.empty())
        return true;
    const StaticWorldIndexBuildResult result =
        g_state.static_world_index.install(source);
    if (!result) {
        if (!g_state.static_world_source_error_logged) {
            LOG_ERROR("active original scene static-world identity rejected map=%s record=%u error=%u",
                      map_namespace.c_str(),
                      static_cast<unsigned>(result.record_index),
                      static_cast<unsigned>(result.error));
            g_state.static_world_source_error_logged = true;
        }
        return false;
    }
    LOG_INFO("active original scene static-world identity installed map=%s records=%u",
             map_namespace.c_str(), static_cast<unsigned>(source.size()));
    return true;
}

bool object_is_bound_vehicle_or_descendant(hta::ai::Obj* object)
{
    for (hta::ai::Obj* current = object; current != nullptr;
         current = current->GetParent()) {
        hta::ai::Vehicle* const vehicle = vehicle_from_object(current);
        if (vehicle == nullptr)
            continue;
        NetId entity_id = kInvalidNetId;
        EntityGeneration generation = kInvalidEntityGeneration;
        if (g_state.entities.lookup_net_id(vehicle->GetId(), entity_id,
                                           generation))
            return true;
    }
    return false;
}

bool quest_projection_object_name_is_unique(
    hta::ai::ObjContainer& objects, const hta::ai::Obj& expected,
    const char* const name)
{
    if (name == nullptr || name[0] == '\0')
        return false;
    const hta::ai::Obj* match = nullptr;
    for (auto iterator = objects.begin(); iterator != objects.end(); ++iterator) {
        const hta::ai::Obj* const candidate = *iterator;
        if (candidate == nullptr || candidate->GetDeletedStatus())
            continue;
        const char* const candidate_name = candidate->GetName();
        if (candidate_name == nullptr || std::strcmp(candidate_name, name) != 0)
            continue;
        if (match != nullptr)
            return false;
        match = candidate;
    }
    return match == &expected;
}

std::optional<QuestProjectionIdentity> quest_projection_identity_for_object(
    hta::ai::CServer& server, hta::ai::Obj& object,
    const std::string& map_namespace, const bool reference_identity = false)
{
    if (server.m_level == nullptr || map_namespace.empty() ||
        g_state.session_identity.resource_fingerprint.empty())
        return std::nullopt;
    const char* const object_name = object.GetName();
    hta::CStr resource_name;
    std::string stable_name;
    QuestProjectionSourceKind source_kind =
        QuestProjectionSourceKind::ReferencedObject;
    if (reference_identity) {
        // Runtime player/AI vehicle names and ObjIds are process-local. Resolve
        // a shared vehicle reference through the same authoritative NetId and
        // generation that already bind its world replica on every peer.
        hta::ai::Vehicle* const vehicle = vehicle_from_object(&object);
        NetId entity_id = kInvalidNetId;
        EntityGeneration generation = kInvalidEntityGeneration;
        if (vehicle != nullptr &&
            static_cast<hta::ai::Obj*>(vehicle) == &object &&
            g_state.entities.lookup_net_id(vehicle->GetId(), entity_id,
                                           generation) &&
            entity_id != kInvalidNetId &&
            generation != kInvalidEntityGeneration) {
            resource_name = hta::CStr("network-entity");
            stable_name = std::to_string(entity_id) + ":" +
                std::to_string(generation);
        }
    }
    if (resource_name.empty() &&
        (object_name == nullptr || object_name[0] == '\0'))
        return std::nullopt;
    if (resource_name.empty() && !reference_identity && object.IsKindOf("Trigger")) {
        const auto provenance = g_state.quest_trigger_provenance.lookup(object_name);
        if (!provenance.has_value()) {
            LOG_WARNING("quest trigger provenance %s for name=%s",
                        g_state.quest_trigger_provenance.locked()
                            ? "locked" : "missing",
                        object_name);
            return std::nullopt;
        }
        resource_name = hta::CStr(provenance->resource_path.c_str());
        source_kind = provenance->source_kind;
        stable_name = object_name;
    } else if (resource_name.empty() && !reference_identity &&
               object.IsKindOf("DynamicQuest")) {
        const hta::CStr& configured = server.m_level->m_questStatesFileName;
        if (configured.empty())
            return std::nullopt;
        resource_name = server.m_level->GetFullPathNameA(configured);
        source_kind = QuestProjectionSourceKind::DynamicQuest;
        stable_name = object_name;
    } else if (resource_name.empty()) {
        // References may point at special engine-owned objects for which the
        // native full-name lookup is not valid. A name that is unique in
        // the active map is a stable reference when combined with the map and
        // resource fingerprint. Ambiguous names fail closed; local ObjIds and
        // pointers never cross the wire.
        if (server.m_pObjects == nullptr ||
            !quest_projection_object_name_is_unique(
                *server.m_pObjects, object, object_name))
            return std::nullopt;
        resource_name = hta::CStr("unique-map-object");
        stable_name = object_name;
    }
    if (resource_name.empty() || resource_name.c_str() == nullptr ||
        stable_name.empty())
        return std::nullopt;
    QuestProjectionIdentity identity;
    identity.resource_fingerprint = g_state.session_identity.resource_fingerprint;
    identity.map_namespace = map_namespace;
    identity.source_kind = source_kind;
    identity.resource_path = resource_name.c_str();
    identity.stable_name = std::move(stable_name);
    identity.id = quest_projection_id_hash(identity.canonical_key());
    return identity.valid() ? std::optional<QuestProjectionIdentity>(std::move(identity))
                            : std::nullopt;
}

std::uint32_t quest_projection_dependency_order(const hta::ai::Obj& object)
{
    std::uint32_t depth = 0;
    for (const hta::m3d::Object* parent = object.GetParent();
         parent != nullptr && depth != (std::numeric_limits<std::uint32_t>::max)();
         parent = parent->GetParent())
        ++depth;
    return depth;
}

void broadcast_quest_projection_delta(const QuestProjectionDelta& delta)
{
    if (!g_state.is_host || !g_state.session ||
        g_state.match.state() == MatchState::Offline)
        return;
    std::vector<Byte> payload;
    if (encode_quest_projection_delta(delta, payload) !=
        QuestProjectionCodecError::None) {
        LOG_ERROR("quest projection delta encode failed epoch=%u revision=%llu",
                  delta.epoch,
                  static_cast<unsigned long long>(delta.revision));
        return;
    }
    for (const PeerId peer : g_state.world_transfer_peers)
        (void)send_match_payload(peer, MessageType::MatchQuestDelta, payload);
}

void observe_authoritative_quest_state()
{
    if (!g_state.is_host || !IsSessionActive() ||
        g_state.world_replay_depth != 0)
        return;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr ||
        server->m_pObjects->m_inUpdate || server->m_pObjects->m_inPurge ||
        server->m_pObjects->m_denyCreationCount != 0)
        return;
    const std::string map_namespace = static_world_map_namespace();
    if (map_namespace.empty())
        return;
    const QuestProjectionEpoch epoch = g_state.match_epoch != 0
        ? g_state.match_epoch : g_state.session_epoch;
    if (epoch == kInvalidQuestProjectionEpoch)
        return;
    const std::string& resource_fingerprint =
        g_state.session_identity.resource_fingerprint;
    if (resource_fingerprint.empty())
        return;
    if (g_state.quest_projection_map_namespace != map_namespace) {
        g_state.quest_projection_map_namespace = map_namespace;
        g_state.quest_projection_host.reset(epoch, resource_fingerprint);
        g_state.quest_projection_sample_ready = false;
        g_state.quest_local_state_logged = false;
    } else if (g_state.quest_projection_host.epoch() != epoch) {
        g_state.quest_projection_host.reset(epoch, resource_fingerprint);
        g_state.quest_projection_sample_ready = false;
    } else {
        g_state.quest_projection_host.set_resource_fingerprint(resource_fingerprint);
    }

    std::vector<QuestProjectionRecord> records;
    bool identity_complete = true;
    std::string first_identity_failure;
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        const bool pending_removal = object != nullptr && std::find(
            server->m_pObjects->m_objIdsToRemove.begin(),
            server->m_pObjects->m_objIdsToRemove.end(), object->GetId()) !=
            server->m_pObjects->m_objIdsToRemove.end();
        if (object == nullptr || object->GetDeletedStatus() ||
            object->m_bNeedPostLoad ||
            object->m_bMustCreateVisualPart || object->m_bPassedToAnotherMap ||
            pending_removal || object_is_bound_vehicle_or_descendant(object))
            continue;

        // The shipped HTA import does not export the Trigger/DynamicQuest
        // static Class objects.  IsKindOf(name) is the native RTTI seam used
        // elsewhere in this runtime; map/path/prototype identity remains the
        // network identity and is never replaced by this class name.
        const bool is_trigger = object->IsKindOf("Trigger");
        const bool is_dynamic_quest = object->IsKindOf("DynamicQuest");
        if (!is_trigger && !is_dynamic_quest)
            continue;
        const std::optional<QuestProjectionIdentity> identity =
            quest_projection_identity_for_object(*server, *object, map_namespace);
        if (!identity) {
            identity_complete = false;
            if (first_identity_failure.empty())
                first_identity_failure = std::string("shared object name=") +
                    (object->GetName() != nullptr ? object->GetName() : "<unnamed>");
            continue;
        }
        QuestProjectionRecord record;
        record.identity = *identity;
        record.dependency_order = quest_projection_dependency_order(*object);
        if (is_trigger) {
            const auto* const trigger = static_cast<hta::ai::Trigger*>(object);
            TriggerProjectionState state;
            state.state = static_cast<QuestTriggerState>(trigger->m_state);
            state.state_keep = trigger->m_StateKeep;
            state.count = trigger->m_Count;
            state.timeout_for_time_period = trigger->m_timeOutForTimePeriod;
            state.frames_for_frames_passed = trigger->m_framesForFramesPassed;
            state.fly_path_for_cinematic_fly =
                trigger->m_flyPathForCinematicFly.c_str() != nullptr
                    ? trigger->m_flyPathForCinematicFly.c_str() : "";
            state.id_for_cinema_msg = trigger->m_idForCinemaMsg;
            state.can_update = trigger->m_bCanUpdate;
            for (auto ref_iterator = trigger->m_ObjIDs.begin();
                 ref_iterator != trigger->m_ObjIDs.end(); ++ref_iterator) {
                hta::ai::Obj* const referenced =
                    server->m_pObjects->GetEntityByObjId(*ref_iterator);
                const std::optional<QuestProjectionIdentity> reference =
                    referenced != nullptr
                        ? quest_projection_identity_for_object(
                              *server, *referenced, map_namespace, true)
                        : std::nullopt;
                if (!reference || std::any_of(
                        state.object_refs.begin(), state.object_refs.end(),
                        [&reference](const QuestProjectionIdentity& item) {
                            return item.canonical_key() == reference->canonical_key();
                        })) {
                    identity_complete = false;
                    if (first_identity_failure.empty())
                        first_identity_failure = std::string("trigger reference owner=") +
                            (object->GetName() != nullptr
                                 ? object->GetName() : "<unnamed>") +
                            " refObjId=" +
                            std::to_string(*ref_iterator) + " refName=" +
                            (referenced != nullptr && referenced->GetName() != nullptr
                                 ? referenced->GetName() : "<missing>");
                    break;
                }
                state.object_refs.push_back(*reference);
            }
            state.call_event_id = static_cast<std::int32_t>(
                trigger->m_callEvent.m_eventId);
            state.call_obj_name = trigger->m_callEvent.m_objName.c_str() != nullptr
                ? trigger->m_callEvent.m_objName.c_str() : "";
            // LoRA-verified ai::Trigger::_StoreCallEvent stores the most
            // recent event caller here and writes Unknown/-1 when that caller
            // has already disappeared. It is transient execution context,
            // not a persistent quest reference. Preserve the event/name for
            // presentation while deliberately keeping the process-local ID
            // absent on replicas.
            state.call_obj_ref.reset();
            record.state = state;
        } else {
            const auto* const quest =
                static_cast<hta::ai::DynamicQuest*>(object);
            DynamicQuestProjectionState state;
            state.reward = quest->GetReward();
            state.take_game_time = quest->GetTakeGameTime().asInt64();
            state.status = static_cast<DynamicQuestStatus>(quest->GetQuestStatus());
            state.hirer_name = quest->GetHirerName().c_str() != nullptr
                ? quest->GetHirerName().c_str() : "";
            state.target_name = quest->GetTargetName().c_str() != nullptr
                ? quest->GetTargetName().c_str() : "";
            const auto capture_dynamic_reference =
                [&server, &map_namespace](const std::int32_t object_id,
                                          std::optional<QuestProjectionIdentity>& output) {
                    if (object_id <= 0)
                        return true;
                    hta::ai::Obj* const referenced =
                        server->m_pObjects->GetEntityByObjId(object_id);
                    if (referenced == nullptr)
                        return false;
                    output = quest_projection_identity_for_object(
                        *server, *referenced, map_namespace, true);
                    return output.has_value();
                };
            if (!capture_dynamic_reference(quest->GetHirerObjId(),
                                           state.hirer_reference) ||
                !capture_dynamic_reference(quest->GetTargetObjId(),
                                           state.target_reference))
                identity_complete = false;
            record.state = std::move(state);
        }
        records.push_back(std::move(record));
    }
    if (!identity_complete) {
        g_state.quest_projection_sample_ready = false;
        if (!g_state.quest_local_state_logged) {
            LOG_WARNING("quest projection incomplete map=%s reason=%s; authoritative snapshot withheld",
                        map_namespace.c_str(),
                        first_identity_failure.empty()
                            ? "unbound trigger/quest identity"
                            : first_identity_failure.c_str());
            g_state.quest_local_state_logged = true;
        }
        return;
    }
    QuestProjectionDelta delta;
    const QuestProjectionHostResult result =
        g_state.quest_projection_host.observe(records, delta);
    if (result == QuestProjectionHostResult::DeltaProduced) {
        LOG_INFO("quest projection delta produced epoch=%u base=%llu revision=%llu records=%u",
                 delta.epoch,
                 static_cast<unsigned long long>(delta.base_revision),
                 static_cast<unsigned long long>(delta.revision),
                 static_cast<unsigned>(delta.records.size()));
        broadcast_quest_projection_delta(delta);
    } else if (result == QuestProjectionHostResult::InvalidInput)
        LOG_WARNING("quest projection post-update sample rejected fail-closed");
    g_state.quest_projection_sample_ready =
        result != QuestProjectionHostResult::InvalidInput &&
        result != QuestProjectionHostResult::RevisionExhausted &&
        result != QuestProjectionHostResult::HistoryOverflow &&
        g_state.quest_projection_host.initialized();
}

bool apply_authoritative_quest_projection(
    const std::span<const QuestProjectionRecord> previous,
    const std::span<const QuestProjectionRecord> target)
{
    (void)previous;
    if (g_state.is_host || !IsSessionActive()) {
        LOG_ERROR("quest projection apply failed: invalid runtime role host=%u sessionActive=%u",
                  g_state.is_host ? 1u : 0u, IsSessionActive() ? 1u : 0u);
        return false;
    }
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr ||
        server->m_level == nullptr ||
        g_state.session_identity.resource_fingerprint.empty()) {
        LOG_ERROR("quest projection apply failed: native world unavailable server=%p objects=%p level=%p fingerprint=%u",
                  server, server != nullptr ? server->m_pObjects : nullptr,
                  server != nullptr ? server->m_level : nullptr,
                  g_state.session_identity.resource_fingerprint.empty() ? 0u : 1u);
        return false;
    }

    struct NativeChange {
        hta::ai::Trigger* trigger = nullptr;
        hta::ai::DynamicQuest* quest = nullptr;
        TriggerProjectionState old_trigger{};
        DynamicQuestProjectionState old_quest{};
        std::int32_t old_hirer_obj_id = kInvalidObjId;
        std::int32_t old_target_obj_id = kInvalidObjId;
        std::vector<int> old_object_ids;
        std::int32_t old_call_obj_id = kInvalidObjId;
        std::vector<int> new_object_ids;
        std::int32_t new_call_obj_id = kNativeNoQuestObjectId;
        std::int32_t new_hirer_obj_id = kNativeNoQuestObjectId;
        std::int32_t new_target_obj_id = kNativeNoQuestObjectId;
        QuestProjectionRecord record;
    };
    std::vector<NativeChange> changes;
    changes.reserve(target.size());
    const std::string map_namespace = static_world_map_namespace();
    if (map_namespace.empty()) {
        LOG_ERROR("quest projection apply failed: active map namespace is empty");
        return false;
    }

    // A complete baseline must account for every native shared trigger/quest.
    // References are resolved only for identities explicitly requested by the
    // target records. Unrelated unnamed objects are not part of this contract.
    std::vector<std::string> requested_reference_keys;
    if (!collect_quest_projection_reference_keys(target, requested_reference_keys))
    {
        LOG_ERROR("quest projection apply failed: invalid referenced-object identity");
        return false;
    }
    struct NativeIdentity {
        std::string key;
        QuestProjectionRecordKind kind = QuestProjectionRecordKind::Trigger;
        hta::ai::Obj* object = nullptr;
    };
    std::vector<NativeIdentity> native_identities;
    std::vector<std::pair<std::string, hta::ai::Obj*>> reference_identities;
    std::vector<std::string> reference_candidate_keys;
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        if (object == nullptr || object->GetDeletedStatus())
            continue;
        if (!requested_reference_keys.empty()) {
            const std::optional<QuestProjectionIdentity> reference =
                quest_projection_identity_for_object(*server, *object,
                                                     map_namespace, true);
            if (reference) {
                const std::string reference_key = reference->canonical_key();
                if (std::find(requested_reference_keys.begin(),
                              requested_reference_keys.end(), reference_key) !=
                    requested_reference_keys.end()) {
                    reference_identities.emplace_back(reference_key, object);
                    reference_candidate_keys.push_back(reference_key);
                }
            }
        }
        if (object_is_bound_vehicle_or_descendant(object))
            continue;
        const bool is_trigger = object->IsKindOf("Trigger");
        const bool is_dynamic_quest = object->IsKindOf("DynamicQuest");
        if (!is_trigger && !is_dynamic_quest)
            continue;
        const std::optional<QuestProjectionIdentity> identity =
            quest_projection_identity_for_object(*server, *object, map_namespace);
        if (!identity) {
            LOG_ERROR("quest projection apply failed: local object has no stable identity objId=%d",
                      object->GetId());
            return false;
        }
        const std::string key = identity->canonical_key();
        if (std::any_of(native_identities.begin(), native_identities.end(),
                        [&key](const NativeIdentity& item) {
                            return item.key == key;
                        })) {
            LOG_ERROR("quest projection apply failed: duplicate local identity key=%s objId=%d",
                      key.c_str(), object->GetId());
            return false;
        }
        native_identities.push_back({
            key, is_trigger ? QuestProjectionRecordKind::Trigger
                            : QuestProjectionRecordKind::DynamicQuest, object});
    }
    for (const NativeIdentity& native : native_identities) {
        const auto present = std::find_if(
            target.begin(), target.end(), [&native](const QuestProjectionRecord& record) {
                return record.identity.canonical_key() == native.key &&
                       record.kind() == native.kind;
            });
        if (present == target.end()) {
            LOG_ERROR("quest projection apply failed: local identity absent from authoritative target key=%s",
                      native.key.c_str());
            return false;
        }
    }

    QuestProjectionTransactionPlan transaction_plan;
    std::vector<std::string> locally_present_keys;
    locally_present_keys.reserve(native_identities.size());
    for (const NativeIdentity& native : native_identities)
        locally_present_keys.push_back(native.key);
    const QuestProjectionTransactionPlanResult plan_result =
        build_quest_projection_transaction_plan(
            target, locally_present_keys, transaction_plan);
    if (plan_result != QuestProjectionTransactionPlanResult::Ready) {
        LOG_ERROR("quest projection apply failed: transaction plan result=%u local=%u target=%u",
                  static_cast<unsigned>(plan_result),
                  static_cast<unsigned>(locally_present_keys.size()),
                  static_cast<unsigned>(target.size()));
        return false;
    }

    std::vector<QuestProjectionRecord> ordered(
        std::move(transaction_plan.apply_records));
    std::sort(ordered.begin(), ordered.end(),
              [](const QuestProjectionRecord& left,
                 const QuestProjectionRecord& right) {
                  if (left.dependency_order != right.dependency_order)
                      return left.dependency_order < right.dependency_order;
                  return left.identity.canonical_key() <
                         right.identity.canonical_key();
              });
    const auto resolve_reference =
        [&reference_identities, &reference_candidate_keys](
            const QuestProjectionIdentity& requested,
            std::int32_t& object_id) {
            const std::string key = requested.canonical_key();
            const QuestProjectionReferenceResolution resolution =
                resolve_quest_projection_reference(key,
                                                   reference_candidate_keys);
            if (resolution != QuestProjectionReferenceResolution::Resolved) {
                LOG_ERROR("quest projection apply failed: reference resolution=%u key=%s candidates=%u",
                          static_cast<unsigned>(resolution), key.c_str(),
                          static_cast<unsigned>(reference_candidate_keys.size()));
                return false;
            }
            const auto found = std::find_if(
                reference_identities.begin(), reference_identities.end(),
                [&key](const auto& item) { return item.first == key; });
            if (found == reference_identities.end()) {
                LOG_ERROR("quest projection apply failed: resolved reference has no object key=%s",
                          key.c_str());
                return false;
            }
            object_id = found->second->GetId();
            if (object_id <= 0)
                LOG_ERROR("quest projection apply failed: resolved reference has invalid objId=%d key=%s",
                          object_id, key.c_str());
            return object_id > 0;
        };
    for (const QuestProjectionRecord& record : ordered) {
        hta::ai::Obj* found = nullptr;
        for (const NativeIdentity& identity : native_identities) {
            if (identity.key != record.identity.canonical_key())
                continue;
            if (identity.kind != record.kind() || found != nullptr) {
                LOG_ERROR("quest projection apply failed: local identity kind/uniqueness mismatch key=%s",
                          record.identity.canonical_key().c_str());
                return false;
            }
            found = identity.object;
        }
        if (found == nullptr) {
            LOG_ERROR("quest projection apply failed: authoritative identity absent locally key=%s",
                      record.identity.canonical_key().c_str());
            return false;
        }
        NativeChange change;
        change.record = record;
        if (record.kind() == QuestProjectionRecordKind::Trigger) {
            if (!found->IsKindOf("Trigger")) {
                LOG_ERROR("quest projection apply failed: trigger identity resolved to wrong native type key=%s",
                          record.identity.canonical_key().c_str());
                return false;
            }
            change.trigger = static_cast<hta::ai::Trigger*>(found);
            change.old_trigger.state = static_cast<QuestTriggerState>(
                change.trigger->m_state);
            change.old_trigger.state_keep = change.trigger->m_StateKeep;
            change.old_trigger.count = change.trigger->m_Count;
            change.old_trigger.timeout_for_time_period =
                change.trigger->m_timeOutForTimePeriod;
            change.old_trigger.frames_for_frames_passed =
                change.trigger->m_framesForFramesPassed;
            change.old_trigger.fly_path_for_cinematic_fly =
                change.trigger->m_flyPathForCinematicFly.c_str() != nullptr
                    ? change.trigger->m_flyPathForCinematicFly.c_str() : "";
            change.old_trigger.id_for_cinema_msg = change.trigger->m_idForCinemaMsg;
            change.old_trigger.can_update = change.trigger->m_bCanUpdate;
            for (auto id_iterator = change.trigger->m_ObjIDs.begin();
                 id_iterator != change.trigger->m_ObjIDs.end(); ++id_iterator)
                change.old_object_ids.push_back(*id_iterator);
            change.old_call_obj_id = change.trigger->m_callEvent.m_callObjId;
            change.old_trigger.call_event_id = static_cast<std::int32_t>(
                change.trigger->m_callEvent.m_eventId);
            change.old_trigger.call_obj_name =
                change.trigger->m_callEvent.m_objName.c_str() != nullptr
                    ? change.trigger->m_callEvent.m_objName.c_str() : "";
            const TriggerProjectionState& state =
                std::get<TriggerProjectionState>(record.state);
            for (const QuestProjectionIdentity& reference : state.object_refs) {
                std::int32_t referenced_id = kInvalidObjId;
                if (!resolve_reference(reference, referenced_id)) {
                    LOG_ERROR("quest projection apply failed: trigger object reference unresolved owner=%s",
                              record.identity.canonical_key().c_str());
                    return false;
                }
                change.new_object_ids.push_back(referenced_id);
            }
            if (state.call_obj_ref.has_value()) {
                if (!resolve_reference(*state.call_obj_ref,
                                       change.new_call_obj_id)) {
                    LOG_ERROR("quest projection apply failed: trigger call reference unresolved owner=%s",
                              record.identity.canonical_key().c_str());
                    return false;
                }
            }
        } else {
            if (!found->IsKindOf("DynamicQuest")) {
                LOG_ERROR("quest projection apply failed: quest identity resolved to wrong native type key=%s",
                          record.identity.canonical_key().c_str());
                return false;
            }
            change.quest = static_cast<hta::ai::DynamicQuest*>(found);
            change.old_quest.reward = change.quest->GetReward();
            change.old_quest.take_game_time = change.quest->GetTakeGameTime().asInt64();
            change.old_quest.status = static_cast<DynamicQuestStatus>(
                change.quest->GetQuestStatus());
            change.old_quest.hirer_name = change.quest->GetHirerName().c_str() != nullptr
                ? change.quest->GetHirerName().c_str() : "";
            change.old_quest.target_name = change.quest->GetTargetName().c_str() != nullptr
                ? change.quest->GetTargetName().c_str() : "";
            change.old_hirer_obj_id = change.quest->GetHirerObjId();
            change.old_target_obj_id = change.quest->GetTargetObjId();
            const DynamicQuestProjectionState& state =
                std::get<DynamicQuestProjectionState>(record.state);
            if (state.hirer_reference.has_value() &&
                !resolve_reference(*state.hirer_reference,
                                   change.new_hirer_obj_id)) {
                    LOG_ERROR("quest projection apply failed: quest hirer unresolved owner=%s",
                              record.identity.canonical_key().c_str());
                    return false;
            }
            if (state.target_reference.has_value() &&
                !resolve_reference(*state.target_reference,
                                   change.new_target_obj_id)) {
                    LOG_ERROR("quest projection apply failed: quest target unresolved owner=%s",
                              record.identity.canonical_key().c_str());
                    return false;
            }
        }
        changes.push_back(std::move(change));
    }

    const ReplicationSourceContext prior_source = g_state.world_source_context;
    g_state.world_source_context = ReplicationSourceContext::NetworkReplay;
    const world_authority::ScopedWorldExecutionContext execution_scope(
        current_world_execution_context(true, false));
    ScopedReplaySuppression replay(g_state.world_replay_depth);
    std::optional<ReplayGuard> observer_replay;
    if (g_state.world_observer)
        observer_replay.emplace(g_state.world_observer->suppress_replay());

    // These are direct assignments to the exact ABI fields documented by the
    // native headers/LoRA. No native action, update, script, event, reward,
    // loot, RNG, or object creation path is reachable here.
    auto restore = [&changes]() {
        for (NativeChange& change : changes) {
            if (change.trigger != nullptr) {
                change.trigger->m_state = static_cast<hta::ai::Trigger::eTriggerState>(change.old_trigger.state);
                change.trigger->m_StateKeep = change.old_trigger.state_keep;
                change.trigger->m_Count = change.old_trigger.count;
                change.trigger->m_timeOutForTimePeriod = change.old_trigger.timeout_for_time_period;
                change.trigger->m_framesForFramesPassed = change.old_trigger.frames_for_frames_passed;
                change.trigger->m_flyPathForCinematicFly = hta::CStr(change.old_trigger.fly_path_for_cinematic_fly.c_str());
                change.trigger->m_idForCinemaMsg = change.old_trigger.id_for_cinema_msg;
                change.trigger->m_bCanUpdate = change.old_trigger.can_update;
                change.trigger->m_ObjIDs.clear();
                for (const int id : change.old_object_ids)
                    change.trigger->m_ObjIDs.push_back(id);
                change.trigger->m_callEvent.m_eventId = static_cast<decltype(change.trigger->m_callEvent.m_eventId)>(change.old_trigger.call_event_id);
                change.trigger->m_callEvent.m_objName = hta::CStr(change.old_trigger.call_obj_name.c_str());
                change.trigger->m_callEvent.m_callObjId = change.old_call_obj_id;
            } else if (change.quest != nullptr) {
                change.quest->m_reward = change.old_quest.reward;
                change.quest->m_takeGameTime.setInt64(change.old_quest.take_game_time);
                change.quest->m_questStatus = static_cast<hta::ai::DynamicQuest::QuestStatus>(change.old_quest.status);
                change.quest->m_hirerName = hta::CStr(change.old_quest.hirer_name.c_str());
                change.quest->m_targetName = hta::CStr(change.old_quest.target_name.c_str());
                change.quest->m_hirerObjId = change.old_hirer_obj_id;
                change.quest->m_targetObjId = change.old_target_obj_id;
            }
        }
    };
    try {
        for (NativeChange& change : changes) {
            if (change.trigger != nullptr) {
                const TriggerProjectionState& state =
                    std::get<TriggerProjectionState>(change.record.state);
                change.trigger->m_state = static_cast<hta::ai::Trigger::eTriggerState>(state.state);
                change.trigger->m_StateKeep = state.state_keep;
                change.trigger->m_Count = state.count;
                change.trigger->m_timeOutForTimePeriod = state.timeout_for_time_period;
                change.trigger->m_framesForFramesPassed = state.frames_for_frames_passed;
                change.trigger->m_flyPathForCinematicFly = hta::CStr(state.fly_path_for_cinematic_fly.c_str());
                change.trigger->m_idForCinemaMsg = state.id_for_cinema_msg;
                change.trigger->m_bCanUpdate = state.can_update;
                change.trigger->m_ObjIDs.clear();
                for (const int id : change.new_object_ids)
                    change.trigger->m_ObjIDs.push_back(id);
                change.trigger->m_callEvent.m_eventId = static_cast<decltype(change.trigger->m_callEvent.m_eventId)>(state.call_event_id);
                change.trigger->m_callEvent.m_objName = hta::CStr(state.call_obj_name.c_str());
                change.trigger->m_callEvent.m_callObjId = change.new_call_obj_id;
            } else if (change.quest != nullptr) {
                const DynamicQuestProjectionState& state =
                    std::get<DynamicQuestProjectionState>(change.record.state);
                change.quest->m_reward = state.reward;
                change.quest->m_takeGameTime.setInt64(state.take_game_time);
                change.quest->m_questStatus = static_cast<hta::ai::DynamicQuest::QuestStatus>(state.status);
                change.quest->m_hirerName = hta::CStr(state.hirer_name.c_str());
                change.quest->m_targetName = hta::CStr(state.target_name.c_str());
                change.quest->m_hirerObjId = change.new_hirer_obj_id;
                change.quest->m_targetObjId = change.new_target_obj_id;
            }
        }
    } catch (...) {
        restore();
        g_state.world_source_context = prior_source;
        LOG_ERROR("quest projection apply failed: exception while assigning native state");
        return false;
    }
    g_state.world_source_context = prior_source;
    return true;
}

bool send_quest_snapshot(const PeerId peer)
{
    if (!g_state.is_host || peer == kInvalidPeer)
        return false;
    // Map loading may have initialized the host projection with the lobby's
    // empty state. Always sample at the send barrier and require that sample
    // to belong to the currently active map.
    observe_authoritative_quest_state();
    const std::string map_namespace = static_world_map_namespace();
    if (!g_state.quest_projection_sample_ready || map_namespace.empty() ||
        g_state.quest_projection_map_namespace != map_namespace ||
        !g_state.quest_projection_host.initialized()) {
        LOG_ERROR("quest snapshot withheld peer=%u map=%s sampledMap=%s ready=%u initialized=%u",
                  peer, map_namespace.c_str(),
                  g_state.quest_projection_map_namespace.c_str(),
                  g_state.quest_projection_sample_ready ? 1u : 0u,
                  g_state.quest_projection_host.initialized() ? 1u : 0u);
        return false;
    }
    const QuestProjectionSnapshot snapshot =
        g_state.quest_projection_host.snapshot();
    std::vector<Byte> payload;
    if (encode_quest_projection_snapshot(snapshot, payload) !=
        QuestProjectionCodecError::None)
        return false;
    return send_match_payload(peer, MessageType::MatchQuestSnapshot, payload);
}

void receive_quest_snapshot(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable)
        return;
    QuestProjectionSnapshot snapshot;
    const QuestProjectionCodecError decoded =
        decode_quest_projection_snapshot(event.payload, snapshot);
    if (decoded != QuestProjectionCodecError::None ||
        (g_state.match_epoch != 0 && g_state.match_epoch != snapshot.epoch) ||
        snapshot.resource_fingerprint != g_state.session_identity.resource_fingerprint) {
        LOG_ERROR("quest snapshot rejected codec=%u localEpoch=%u snapshotEpoch=%u fingerprintMatch=%u records=%u",
                  static_cast<unsigned>(decoded), g_state.match_epoch,
                  snapshot.epoch,
                  snapshot.resource_fingerprint ==
                          g_state.session_identity.resource_fingerprint
                      ? 1u : 0u,
                  static_cast<unsigned>(snapshot.records.size()));
        g_state.client_join_failure_pending = true;
        return;
    }
    g_state.match_epoch = snapshot.epoch;
    g_state.quest_projection_client.set_applier(
        [](std::span<const QuestProjectionRecord> previous,
           std::span<const QuestProjectionRecord> target) {
            return apply_authoritative_quest_projection(previous, target);
        });
    const QuestProjectionClientResult result =
        g_state.quest_projection_client.begin_snapshot(snapshot);
    LOG_INFO("quest snapshot accepted epoch=%u revision=%llu records=%u result=%u",
             snapshot.epoch,
             static_cast<unsigned long long>(snapshot.revision),
             static_cast<unsigned>(snapshot.records.size()),
             static_cast<unsigned>(result));
    if (result == QuestProjectionClientResult::Invalid ||
        result == QuestProjectionClientResult::WrongEpoch ||
        result == QuestProjectionClientResult::WrongFingerprint ||
        result == QuestProjectionClientResult::ResnapshotRequired)
        g_state.client_join_failure_pending = true;
    maybe_commit_quest_projection();
}

void emit_quest_projection_committed_marker(
    const QuestProjectionRevision revision)
{
    LOG_INFO("KRAKEN_MP_ACCEPT quest_committed epoch=%u revision=%llu",
             g_state.match_epoch,
             static_cast<unsigned long long>(revision));
}

void receive_quest_delta(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable)
        return;
    QuestProjectionDelta delta;
    if (decode_quest_projection_delta(event.payload, delta) !=
            QuestProjectionCodecError::None ||
        (g_state.match_epoch != 0 && g_state.match_epoch != delta.epoch) ||
        delta.resource_fingerprint != g_state.session_identity.resource_fingerprint) {
        g_state.client_join_failure_pending = true;
        return;
    }
    g_state.match_epoch = delta.epoch;
    const QuestProjectionClientResult result =
        g_state.quest_projection_client.accept_delta(delta);
    if (result != QuestProjectionClientResult::Applied &&
        result != QuestProjectionClientResult::Buffered &&
        result != QuestProjectionClientResult::Duplicate) {
        LOG_ERROR("quest projection delta rejected epoch=%u base=%llu revision=%llu applied=%llu state=%u result=%u records=%u",
                  delta.epoch,
                  static_cast<unsigned long long>(delta.base_revision),
                  static_cast<unsigned long long>(delta.revision),
                  static_cast<unsigned long long>(
                      g_state.quest_projection_client.applied_revision()),
                  static_cast<unsigned>(g_state.quest_projection_client.state()),
                  static_cast<unsigned>(result),
                  static_cast<unsigned>(delta.records.size()));
    }
    if (result == QuestProjectionClientResult::Invalid ||
        result == QuestProjectionClientResult::WrongEpoch ||
        result == QuestProjectionClientResult::WrongFingerprint ||
        result == QuestProjectionClientResult::Gap ||
        result == QuestProjectionClientResult::Overflow ||
        result == QuestProjectionClientResult::ApplyFailed ||
        result == QuestProjectionClientResult::ResnapshotRequired)
        g_state.client_join_failure_pending = true;
    if (result == QuestProjectionClientResult::Applied) {
        emit_quest_projection_committed_marker(
            g_state.quest_projection_client.applied_revision());
        return;
    }
    maybe_commit_quest_projection();
}

void maybe_commit_quest_projection()
{
    if (g_state.is_host || !g_state.world_snapshot_committed ||
        g_state.client_join_failure_pending)
        return;
    if (g_state.quest_projection_client.state() ==
            QuestProjectionClientState::Idle ||
        g_state.quest_projection_client.ready())
        return;
    g_state.quest_projection_client.set_applier(
        [](std::span<const QuestProjectionRecord> previous,
           std::span<const QuestProjectionRecord> target) {
            return apply_authoritative_quest_projection(previous, target);
        });
    (void)g_state.quest_projection_client.mark_world_ready();
    const QuestProjectionClientResult result =
        g_state.quest_projection_client.commit();
    if (result != QuestProjectionClientResult::Ready) {
        LOG_ERROR("quest projection commit deferred/failed state=%u result=%u worldReady=%u",
                  static_cast<unsigned>(g_state.quest_projection_client.state()),
                  static_cast<unsigned>(result),
                  g_state.world_snapshot_committed ? 1u : 0u);
        if (result == QuestProjectionClientResult::Gap ||
            result == QuestProjectionClientResult::Invalid ||
            result == QuestProjectionClientResult::ApplyFailed ||
            result == QuestProjectionClientResult::WrongFingerprint ||
            result == QuestProjectionClientResult::ResnapshotRequired)
            g_state.client_join_failure_pending = true;
        return;
    }
    g_state.quest_projection_committed = true;
    emit_quest_projection_committed_marker(
        g_state.quest_projection_client.applied_revision());
    if (g_state.quest_play_pending) {
        g_state.quest_play_pending = false;
        g_state.visible_match_state = MatchState::Playing;
        emit_gameplay_open_marker();
        emit_match_accept_marker();
    }
    maybe_send_world_ready_and_sync();
}

void observe_authoritative_world()
{
    if (!g_state.is_host || !g_state.world_observer ||
        !IsSessionActive() || g_state.world_replay_depth != 0)
        return;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return;
    // The observer is called from the post-update boundary.  If an engine
    // transition still owns the container, do not take a half-membership
    // sample: the stability gate requires two complete, equal observations.
    if (server->m_pObjects->m_inUpdate || server->m_pObjects->m_inPurge ||
        server->m_pObjects->m_denyCreationCount != 0)
        return;
    const std::string map_namespace = static_world_map_namespace();
    if (map_namespace.empty())
        return;
    std::vector<ObjectRecord> records;
    std::vector<hta::ai::Obj*> observed_objects;
    std::vector<std::size_t> identity_record_for_object;
    g_state.static_world_post_load_records.clear();
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        const bool pending_removal = object != nullptr && std::find(
            server->m_pObjects->m_objIdsToRemove.begin(),
            server->m_pObjects->m_objIdsToRemove.end(), object->GetId()) !=
            server->m_pObjects->m_objIdsToRemove.end();
        if (object == nullptr || object->GetDeletedStatus() ||
            object->m_bIsUpdating || object->m_bNeedPostLoad ||
            object->m_bMustCreateVisualPart ||
            object->m_bPassedToAnotherMap || pending_removal)
            continue;
        // Player/NPC vehicles and their complete descendant trees already
        // have the typed EntitySpawn/VehicleDescriptor path.  Every other
        // native object, including arbitrary nested containers, belongs to
        // this generic graph sample.
        if (object_is_bound_vehicle_or_descendant(object))
            continue;
        const std::int32_t prototype_id = object->GetPrototypeId();
        if (prototype_id <= 0)
            continue;
        ObjectRecord record{};
        record.type_id = static_cast<ObjectTypeId>(prototype_id);
        observed_objects.push_back(object);
        records.push_back(std::move(record));
        const std::optional<StaticWorldPostLoadRecord> identity_record =
            static_world_record_for_object(*server, *object, map_namespace);
        if (identity_record) {
            identity_record_for_object.push_back(
                g_state.static_world_post_load_records.size());
            g_state.static_world_post_load_records.push_back(*identity_record);
        } else {
            identity_record_for_object.push_back(kInvalidStaticWorldIndex);
        }
    }
    (void)install_static_world_source(map_namespace,
                                       g_state.static_world_post_load_records,
                                       observed_objects);
    const StaticWorldMatchResult identity_match = match_static_world_records(
        g_state.static_world_index, g_state.static_world_post_load_records);
    if (!identity_match.ok()) {
        // An ambiguous or partial source match is not evidence of a dynamic
        // object.  Fail closed before touching the registry, observer
        // baseline, snapshot payload, or journal; otherwise an old stable
        // sample could demote original static objects into new dynamic IDs.
        g_state.static_world_identity_stable = false;
        g_state.static_world_stability.reset();
        if (g_state.static_world_identity_last_error != identity_match.error ||
            g_state.static_world_identity_error_map != map_namespace) {
            LOG_WARNING("static world post-load identity sample rejected map=%s error=%u; retaining prior committed identities",
                        map_namespace.c_str(),
                        static_cast<unsigned>(identity_match.error));
            g_state.static_world_identity_last_error = identity_match.error;
            g_state.static_world_identity_error_map = map_namespace;
        }
        return;
    }
    else {
        g_state.static_world_identity_last_error = StaticWorldIndexError::None;
        g_state.static_world_identity_error_map.clear();
        g_state.static_world_ids_by_post_load_index =
            identity_match.ids_by_post_load_index;
        g_state.static_world_identity_stable =
            g_state.static_world_stability.observe(identity_match);
    }
    std::vector<StaticWorldId> static_ids_by_object(records.size(),
                                                    kInvalidStaticWorldId);
    if (identity_match.ok()) {
        for (std::size_t index = 0; index < records.size(); ++index) {
            const std::size_t identity_index =
                identity_record_for_object[index];
            if (identity_index != kInvalidStaticWorldIndex &&
                identity_index < identity_match.ids_by_post_load_index.size())
                static_ids_by_object[index] =
                    identity_match.ids_by_post_load_index[identity_index];
        }
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        StaticWorldId static_id = static_ids_by_object[index];
        const HostWorldEngineHandle handle = static_cast<HostWorldEngineHandle>(
            observed_objects[index]->GetId());
        std::optional<HostObjectId> prebound_id =
            g_state.host_world_registry.id_for(handle);
        if (prebound_id && is_host_world_static_id(*prebound_id) &&
            static_id == kInvalidStaticWorldId) {
            const std::optional<HostWorldGeneration> generation =
                g_state.host_world_registry.generation_for(*prebound_id);
            if (generation)
                (void)g_state.host_world_registry.remove(
                    handle, *generation);
            prebound_id.reset();
        }
        if (prebound_id && is_host_world_dynamic_id(*prebound_id)) {
            records[index].object_id = *prebound_id;
            continue;
        }
        if (static_id != kInvalidStaticWorldId &&
            !g_state.static_world_identity_stable &&
            !(prebound_id && *prebound_id == static_id)) {
            // A source candidate waits for the second equal membership
            // digest; it must not be assigned a provisional dynamic ID.
            records[index].object_id = kInvalidHostObjectId;
            continue;
        }
        if (static_id != kInvalidStaticWorldId) {
            if (prebound_id && *prebound_id != static_id) {
                LOG_ERROR("static world binding collision object=%d existing=%llu expected=%llu",
                          observed_objects[index]->GetId(),
                          static_cast<unsigned long long>(*prebound_id),
                          static_cast<unsigned long long>(static_id));
                records[index].object_id = kInvalidHostObjectId;
                continue;
            }
        }
        if (static_id != kInvalidStaticWorldId) {
            const auto original = std::find_if(
                g_state.static_world_original_bindings.begin(),
                g_state.static_world_original_bindings.end(),
                [static_id](const auto& binding) {
                    return binding.first == static_id;
                });
            if (original != g_state.static_world_original_bindings.end() &&
                original->second != handle) {
                // Same source key on a later runtime object is not enough to
                // regain a static identity after the original object leaves.
                static_id = kInvalidStaticWorldId;
            }
        }
        if (static_id != kInvalidStaticWorldId) {
            records[index].object_id = static_id;
            const HostWorldRegistryResult bound =
                g_state.host_world_registry.install_static(
                    handle, static_id);
            if (!host_world_registry_succeeded(bound) &&
                bound != HostWorldRegistryResult::Collision) {
                LOG_ERROR("static world registry bind failed object=%llu objId=%d code=%u",
                          static_cast<unsigned long long>(static_id),
                          observed_objects[index]->GetId(),
                          static_cast<unsigned>(bound));
                records[index].object_id = kInvalidHostObjectId;
            }
            else if (std::none_of(
                         g_state.static_world_original_bindings.begin(),
                         g_state.static_world_original_bindings.end(),
                         [static_id](const auto& binding) {
                             return binding.first == static_id;
                         })) {
                g_state.static_world_original_bindings.push_back(
                    {static_id, handle});
            }
            continue;
        }
        HostWorldDynamicAllocation allocation{};
        allocation = g_state.host_world_registry.allocate_dynamic(handle);
        if (!allocation.succeeded()) {
            LOG_ERROR("dynamic world registry allocation failed object=%d code=%u",
                      observed_objects[index]->GetId(),
                      static_cast<unsigned>(allocation.result));
            records[index].object_id = kInvalidHostObjectId;
        } else {
            records[index].object_id = allocation.id;
            if (!g_state.static_world_dynamic_identity_logged) {
                LOG_INFO("dynamic world object assigned host id=%llu map=%s",
                         static_cast<unsigned long long>(allocation.id),
                         map_namespace.c_str());
                g_state.static_world_dynamic_identity_logged = true;
            }
        }
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        hta::ai::Obj* const parent = observed_objects[index]->GetParent();
        if (records[index].object_id == kInvalidHostObjectId)
            continue;
        HostObjectId parent_id = kInvalidHostObjectId;
        if (parent != nullptr) {
            const auto parent_it = std::find(observed_objects.begin(),
                                             observed_objects.end(), parent);
            if (parent_it != observed_objects.end()) {
                const std::size_t parent_index = static_cast<std::size_t>(
                    std::distance(observed_objects.begin(), parent_it));
                if (parent_index < records.size())
                    parent_id = records[parent_index].object_id;
            }
            if (parent_id == kInvalidHostObjectId) {
                const std::optional<HostObjectId> bound_parent =
                    g_state.host_world_registry.id_for(
                        static_cast<HostWorldEngineHandle>(parent->GetId()));
                if (bound_parent)
                    parent_id = *bound_parent;
            }
        }
        records[index].parent_id = parent_id;
        const std::optional<HostWorldGeneration> generation =
            g_state.host_world_registry.generation_for(records[index].object_id);
        if (!generation)
            continue;
        const HostWorldRegistryResult parent_result =
            g_state.host_world_registry.set_parent(
                static_cast<HostWorldEngineHandle>(
                    observed_objects[index]->GetId()), *generation,
                parent_id);
        if (parent_result != HostWorldRegistryResult::Inserted &&
            parent_result != HostWorldRegistryResult::AlreadyBound)
            LOG_WARNING("world parent identity bind rejected child=%llu parent=%llu code=%u",
                        static_cast<unsigned long long>(records[index].object_id),
                        static_cast<unsigned long long>(parent_id),
                        static_cast<unsigned>(parent_result));
    }
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [](const ObjectRecord& record) {
                                     return record.object_id ==
                                         kInvalidHostObjectId;
                                 }),
                  records.end());
    const WorldRevision previous_revision = g_state.world_journal.revision();
    const ReplicationSourceContext prior = g_state.world_source_context;
    g_state.world_source_context = ReplicationSourceContext::LocalAuthoritative;
    (void)g_state.world_observer->observe_frame(records);
    g_state.world_source_context = prior;
    g_state.world_snapshot.objects = records;
    std::vector<Byte> snapshot_payload;
    if (world_state_snapshot_codec_succeeded(
            encode_world_state_snapshot(g_state.world_snapshot,
                                        snapshot_payload)))
        g_state.world_snapshot_payload = std::move(snapshot_payload);
    else
        LOG_ERROR("authoritative world snapshot encode failed records=%u",
                  static_cast<unsigned>(records.size()));
    broadcast_new_world_deltas(previous_revision);
}

void retire_network_vehicle(hta::ai::ObjContainer& objects,
                            hta::ai::Obj& object,
                            hta::ai::Vehicle& vehicle,
                            bool hide_visual,
                            bool preserve_destroyed_visual)
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
    if (registered.kind == EntityKind::NpcVehicle &&
        g_state.match.state() == MatchState::Playing) {
        // NPCs remain exclusively on EntitySpawn/VehicleDescriptor.  This
        // revision is the independent generic-world barrier captured after
        // the typed entity is fully registered; it is not an NPC mutation.
        LOG_INFO("KRAKEN_MP_ACCEPT native_entity_registered entity=%u generation=%u kind=%u barrierRevision=%llu",
                 registered.entity_id,
                 static_cast<unsigned>(registered.generation),
                 static_cast<unsigned>(registered.kind),
                 static_cast<unsigned long long>(
                     g_state.world_journal.revision()));
    }
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
    if (g_state.is_host && !host_peer_world_transfer_permitted(peer))
        return false;
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
    if (!g_state.session ||
        (g_state.is_host && !host_peer_world_transfer_permitted(peer)))
        return;
    std::array<Byte, kWorldLootDeltaWireSize> payload{};
    if (!world_loot_codec_succeeded(encode_world_loot_delta(delta, payload)))
        return;
    (void)g_state.session->send(peer, MessageType::WorldLootDelta,
                                Channel::Reliable, payload);
}

void send_world_loot_remove(PeerId peer, const WorldLootRemove& remove)
{
    if (!g_state.session ||
        (g_state.is_host && !host_peer_world_transfer_permitted(peer)))
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
    if (!g_state.session || !g_state.is_host ||
        !host_peer_world_transfer_permitted(peer))
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
    if (g_state.is_host && !host_peer_world_transfer_permitted(peer))
        return false;
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
    if (g_state.is_host && !host_peer_world_transfer_permitted(peer))
        return false;
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
    if (!host_peer_world_transfer_permitted(peer))
        return;
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
    if (!g_state.is_host || !g_state.session || !g_state.session->running() ||
        !host_peer_world_transfer_permitted(peer))
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
    remote.weapon_aim = WeaponAimState{};
    remote.has_weapon_aim = false;
    remote.weapon_trigger = WeaponTriggerState{};
    remote.has_weapon_trigger = false;
    remote.last_shot = ShotConfirmed{};
    remote.has_last_shot = false;
    remote.loadout = LoadoutProfile{};
    remote.has_loadout = false;
    remote.applied_loadout_revision = 0;
    remote.descriptor = VehicleDescriptor{};
    remote.has_descriptor = false;
    remote.wreck_object_id = kInvalidObjId;
    remote.inert_wreck = false;
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
        if (g_state.combat_runtime != nullptr)
            g_state.combat_runtime->despawn(
                {despawn.entity_id, despawn.generation});
        const bool released = release_player_slot_entity(
            despawn.entity_id, despawn.generation, "authoritative despawn");
        mark_remote_despawned(*remote, despawn.generation);
        LOG_INFO("authoritative player slot despawn entity=%u generation=%u released=%u",
                 despawn.entity_id, static_cast<unsigned>(despawn.generation),
                 released ? 1u : 0u);
        return;
    }
    if (g_state.combat_runtime != nullptr)
        g_state.combat_runtime->despawn(
            {despawn.entity_id, despawn.generation});
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
        LOG_INFO("resetting reused entity id entity=%u oldGeneration=%u newGeneration=%u hadLoadout=%u hadSnapshots=%u hadWeaponAim=%u wasRetired=%u",
                 spawn.entity_id, static_cast<unsigned>(prior_generation),
                 static_cast<unsigned>(spawn.generation),
                 remote->has_loadout ? 1u : 0u,
                 remote->has_sequence ? 1u : 0u,
                 remote->has_weapon_aim ? 1u : 0u,
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
    LOG_INFO("entity spawn entity=%u generation=%u kind=%u prototype=%d",
             spawn.entity_id, static_cast<unsigned>(spawn.generation),
             static_cast<unsigned>(spawn.kind), remote->prototype_id);
}

void receive_input(const SessionEvent& event)
{
    if (!g_state.is_host || g_state.match.state() != MatchState::Playing)
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
    if (!g_state.diagnostic_host_control_ready_logged) {
        g_state.diagnostic_host_control_ready_logged = true;
        LOG_INFO("KRAKEN_MP_ACCEPT host_control_ready peer=%u epoch=%u entity=%u sequence=%u",
                 event.peer, g_state.match_epoch, input.entity_id, input.sequence);
    }
}

bool send_weapon_intent(const WeaponCommand& command)
{
    std::array<Byte, kWeaponCommandWireSize> payload{};
    const WeaponCommandCodecError encoded =
        encode_weapon_command(command, payload);
    if (!weapon_command_codec_succeeded(encoded)) {
        LOG_ERROR("weapon intent encode failed entity=%u sequence=%u code=%u",
                  command.entity_id, command.sequence,
                  static_cast<unsigned>(encoded));
        return false;
    }
    if (g_state.is_host || !g_state.session || !g_state.session->running() ||
        g_state.peers.empty()) {
        LOG_ERROR("weapon intent send unavailable entity=%u sequence=%u",
                  command.entity_id, command.sequence);
        return false;
    }
    const TransportResult result = g_state.session->send(
        g_state.peers.front(), MessageType::WeaponCommand, Channel::Reliable,
        payload);
    if (!result)
        LOG_ERROR("weapon intent send failed entity=%u sequence=%u code=%u",
                  command.entity_id, command.sequence,
                  static_cast<unsigned>(result.code));
    return static_cast<bool>(result);
}

bool send_combat_payload(const PeerId peer, const MessageType type,
                         const Channel channel, const std::vector<Byte>& payload)
{
    if (!g_state.session || !g_state.session->running() ||
        peer == kInvalidPeer)
        return false;
    const TransportResult result = g_state.session->send(
        peer, type, channel, payload);
    if (!result)
        LOG_ERROR("combat state send failed peer=%u message=%u code=%u",
                  peer, static_cast<unsigned>(type),
                  static_cast<unsigned>(result.code));
    return static_cast<bool>(result);
}

void publish_host_weapon_aim(const WeaponAimState& state)
{
    if (!g_state.is_host ||
        validate_weapon_aim_state(state) != CombatPresentationCodecError::None)
        return;
    auto found = std::find_if(
        g_state.latest_weapon_aim_states.begin(),
        g_state.latest_weapon_aim_states.end(),
        [&state](const WeaponAimState& current) {
            return current.session_epoch == state.session_epoch &&
                   current.shooter.net_id == state.shooter.net_id &&
                   current.shooter.generation == state.shooter.generation &&
                   weapon_identity_equal(current.gun, state.gun);
        });
    if (found == g_state.latest_weapon_aim_states.end())
        g_state.latest_weapon_aim_states.push_back(state);
    else if (found->update_sequence < state.update_sequence)
        *found = state;
    std::vector<Byte> payload;
    if (encode_weapon_aim_state(state, payload) !=
        CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)send_combat_payload(peer, MessageType::CombatWeaponAimState,
                                   Channel::Unreliable, payload);
}

void publish_host_weapon_trigger(const WeaponTriggerState& state,
                                 const bool transition)
{
    if (!g_state.is_host || !transition ||
        validate_weapon_trigger_state(state) != CombatPresentationCodecError::None)
        return;
    auto found = std::find_if(
        g_state.latest_weapon_trigger_states.begin(),
        g_state.latest_weapon_trigger_states.end(),
        [&state](const WeaponTriggerState& current) {
            return current.session_epoch == state.session_epoch &&
                   current.shooter.net_id == state.shooter.net_id &&
                   current.shooter.generation == state.shooter.generation &&
                   weapon_identity_equal(current.gun, state.gun);
        });
    if (found == g_state.latest_weapon_trigger_states.end())
        g_state.latest_weapon_trigger_states.push_back(state);
    else if (found->transition_id < state.transition_id)
        *found = state;
    std::vector<Byte> payload;
    if (encode_weapon_trigger_state(state, payload) !=
        CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)send_combat_payload(peer, MessageType::CombatWeaponTriggerState,
                                   Channel::Reliable, payload);
}

void emit_combat_authority_marker(const NetId shooter,
                                  const CombatEventId shot_id)
{
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_combat_marker)
        return;
    g_state.next_combat_marker = now + std::chrono::seconds(1);
    const bool violation = g_state.client_original_fire_call_count != 0 ||
        g_state.client_original_damage_call_count != 0;
    const auto blocked_fire = static_cast<unsigned long long>(
        g_state.client_blocked_fire_attempt_count);
    const auto original_fire = static_cast<unsigned long long>(
        g_state.client_original_fire_call_count);
    const auto blocked_damage = static_cast<unsigned long long>(
        g_state.client_blocked_damage_attempt_count);
    const auto original_damage = static_cast<unsigned long long>(
        g_state.client_original_damage_call_count);
    const auto shot = static_cast<unsigned long long>(shot_id);
    if (violation) {
        LOG_ERROR("KRAKEN_MP_COMBAT host_shot=1 client_blocked_fire=%llu client_projectile=%llu client_blocked_damage=%llu client_damage=%llu authority_ok=0 shooter=%u shot=%llu",
                  blocked_fire, original_fire, blocked_damage, original_damage,
                  shooter, shot);
        if (!g_state.combat_authority_failure_emitted) {
            g_state.combat_authority_failure_emitted = true;
            LOG_ERROR("KRAKEN_COMBAT_AUTOTEST FAIL authority_violation=1 client_projectile=%llu client_damage=%llu",
                      original_fire, original_damage);
        }
        return;
    }
    LOG_INFO("KRAKEN_MP_COMBAT host_shot=1 client_blocked_fire=%llu client_projectile=%llu client_blocked_damage=%llu client_damage=%llu authority_ok=1 shooter=%u shot=%llu",
             blocked_fire, original_fire, blocked_damage, original_damage,
             shooter, shot);
}

void publish_host_shot_confirmed(const ShotConfirmed& state)
{
    if (!g_state.is_host ||
        validate_shot_confirmed(state) != CombatPresentationCodecError::None)
        return;
    std::vector<Byte> payload;
    if (encode_shot_confirmed(state, payload) !=
        CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)send_combat_payload(peer, MessageType::CombatShotConfirmed,
                                   Channel::Reliable, payload);
    emit_combat_authority_marker(state.shooter.net_id, state.shot_id);
}

void publish_host_impact(const ImpactPresentation& state)
{
    if (!g_state.is_host ||
        validate_impact_presentation(state) != CombatPresentationCodecError::None)
        return;
    std::vector<Byte> payload;
    if (encode_impact_presentation(state, payload) !=
        CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)send_combat_payload(peer, MessageType::CombatImpactPresentation,
                                   Channel::Reliable, payload);
}

void publish_host_damage(const DamageResult& state)
{
    if (!g_state.is_host ||
        validate_damage_result(state) != CombatPresentationCodecError::None)
        return;
    std::vector<Byte> payload;
    if (encode_damage_result(state, payload) !=
        CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)send_combat_payload(peer, MessageType::CombatDamageResult,
                                   Channel::Reliable, payload);
}

bool publish_host_wreck_archive(const PeerId peer,
                                const DeathWreckPresentation& state)
{
    if (!g_state.is_host || peer == kInvalidPeer || !g_state.session ||
        !g_state.session->running())
        return false;
    const auto found = std::find_if(
        g_state.host_wreck_archives.begin(), g_state.host_wreck_archives.end(),
        [&state](const HostWreckArchive& archive) {
            return archive.archive_id == state.wreck_archive_id &&
                   archive.revision == state.wreck_archive_revision &&
                   archive.digest == state.wreck_archive_digest;
        });
    if (found == g_state.host_wreck_archives.end())
        return false;
    std::vector<NativeObjectArchiveChunk> chunks;
    if (make_native_object_archive_chunks(ByteView{found->encoded},
                                           found->archive_id, found->revision,
                                           found->digest, chunks) !=
            NativeObjectArchiveTransferResult::Accepted)
        return false;
    for (const NativeObjectArchiveChunk& chunk : chunks) {
        std::vector<Byte> payload;
        if (encode_native_object_archive_chunk(chunk, payload) !=
            NativeObjectArchiveErrorCode::None ||
            !send_combat_payload(peer, MessageType::CombatWreckArchiveChunk,
                                 Channel::Reliable, payload))
            return false;
    }
    return true;
}

bool publish_host_wreck_spawn(const PeerId peer,
                              const DeathWreckPresentation& state)
{
    if (!g_state.is_host || g_state.session == nullptr ||
        peer == kInvalidPeer || state.wreck_entity.net_id == kInvalidNetId ||
        state.wreck_entity.generation == kInvalidEntityGeneration)
        return false;
    if (std::find_if(g_state.spawn_publications.begin(),
                     g_state.spawn_publications.end(),
                     [peer, &state](const SpawnPublication& publication) {
                         return publication.peer == peer &&
                             publication.entity_id == state.wreck_entity.net_id &&
                             publication.generation == state.wreck_entity.generation;
                     }) != g_state.spawn_publications.end())
        return true;
    EntitySpawn spawn{};
    spawn.entity_id = state.wreck_entity.net_id;
    spawn.generation = state.wreck_entity.generation;
    spawn.kind = EntityKind::Wreck;
    spawn.prototype_id = 0;
    spawn.health_fraction = 0.0f;
    std::array<Byte, kEntitySpawnWireSize> payload{};
    if (encode_entity_spawn(spawn, payload) != EntityCodecError::None)
        return false;
    if (!g_state.session->send(peer, MessageType::EntitySpawn,
                               Channel::Reliable, payload))
        return false;
    g_state.spawn_publications.push_back(
        {peer, state.wreck_entity.net_id, state.wreck_entity.generation});
    return true;
}

void publish_host_death(const DeathWreckPresentation& state)
{
    if (!g_state.is_host ||
        validate_death_wreck_presentation(state) !=
            CombatPresentationCodecError::None)
        return;
    auto found = std::find_if(
        g_state.latest_terminal_deaths.begin(),
        g_state.latest_terminal_deaths.end(),
        [&state](const DeathWreckPresentation& current) {
            return current.entity.net_id == state.entity.net_id &&
                   current.entity.generation == state.entity.generation;
        });
    if (found == g_state.latest_terminal_deaths.end()) {
        if (g_state.latest_terminal_deaths.size() >=
            kNativeObjectArchiveMaxCacheEntries)
            g_state.latest_terminal_deaths.erase(
                g_state.latest_terminal_deaths.begin());
        g_state.latest_terminal_deaths.push_back(state);
    }
    else if (found->transition_id < state.transition_id)
        *found = state;
    std::vector<Byte> payload;
    if (encode_death_wreck_presentation(state, payload) !=
        CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers) {
        if (!publish_host_wreck_spawn(peer, state))
            continue;
        // Reliable ordering is intentional: archive chunks are admitted to
        // the replica cache before the terminal linkage can be applied.
        // Chunks are published by the host-side archive cache below.
        if (!publish_host_wreck_archive(peer, state))
            continue;
        (void)send_combat_payload(peer,
                                   MessageType::CombatDeathWreckPresentation,
                                   Channel::Reliable, payload);
    }
}

void publish_host_horn(const HornState& state)
{
    if (!g_state.is_host ||
        validate_horn_state(state) != CombatPresentationCodecError::None)
        return;
    auto found = std::find_if(
        g_state.latest_horn_states.begin(), g_state.latest_horn_states.end(),
        [&state](const HornState& current) {
            return current.vehicle.net_id == state.vehicle.net_id &&
                   current.vehicle.generation == state.vehicle.generation;
        });
    if (found == g_state.latest_horn_states.end())
        g_state.latest_horn_states.push_back(state);
    else if (found->transition_id < state.transition_id)
        *found = state;
    std::vector<Byte> payload;
    if (encode_horn_state(state, payload) != CombatPresentationCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)send_combat_payload(peer, MessageType::CombatHornState,
                                   Channel::Reliable, payload);
}

bool send_presentation_jip_state(const PeerId peer)
{
    if (!g_state.is_host || peer == kInvalidPeer ||
        g_state.session_epoch == 0)
        return false;
    const std::size_t aim_count = g_state.latest_weapon_aim_states.size();
    const std::size_t trigger_count =
        g_state.latest_weapon_trigger_states.size();
    const std::size_t horn_count = g_state.latest_horn_states.size();
    const std::size_t death_count = g_state.latest_terminal_deaths.size();
    const std::size_t chunk_count = (std::max)(
        std::size_t{1}, (std::max)(
            (aim_count + kMaxPresentationJipAimStatesPerChunk - 1) /
                kMaxPresentationJipAimStatesPerChunk,
            (std::max)(
                (trigger_count + kMaxPresentationJipWeaponStatesPerChunk - 1) /
                    kMaxPresentationJipWeaponStatesPerChunk,
                (std::max)(
                    (horn_count + kMaxPresentationJipHornStatesPerChunk - 1) /
                        kMaxPresentationJipHornStatesPerChunk,
                    (death_count + kMaxPresentationJipDeathStatesPerChunk - 1) /
                        kMaxPresentationJipDeathStatesPerChunk))));
    if (chunk_count > kMaxPresentationJipChunks)
        return false;
    // Terminal death linkage is sent only after every referenced archive has
    // been transferred on the reliable channel. This ordering is also used
    // by ordinary live peers and makes JIP replay safe when packets are
    // re-ordered by the session barrier.
    for (const DeathWreckPresentation& death : g_state.latest_terminal_deaths) {
        if (!publish_host_wreck_spawn(peer, death) ||
            !publish_host_wreck_archive(peer, death))
            return false;
    }
    const PresentationStateRevision revision =
        ++g_state.presentation_state_revision;
    bool sent = true;
    for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        PresentationJipState state{};
        state.session_epoch = g_state.session_epoch;
        state.state_revision = revision;
        state.server_tick = g_state.server_tick;
        state.chunk_index = static_cast<std::uint16_t>(chunk_index);
        state.chunk_count = static_cast<std::uint16_t>(chunk_count);
        const std::size_t trigger_begin = chunk_index *
            kMaxPresentationJipWeaponStatesPerChunk;
        const std::size_t aim_begin = chunk_index *
            kMaxPresentationJipAimStatesPerChunk;
        const std::size_t trigger_end = (std::min)(
            trigger_begin + kMaxPresentationJipWeaponStatesPerChunk,
            trigger_count);
        const std::size_t aim_end = (std::min)(
            aim_begin + kMaxPresentationJipAimStatesPerChunk, aim_count);
        const std::size_t horn_begin = chunk_index *
            kMaxPresentationJipHornStatesPerChunk;
        const std::size_t horn_end = (std::min)(
            horn_begin + kMaxPresentationJipHornStatesPerChunk, horn_count);
        const std::size_t death_begin = chunk_index *
            kMaxPresentationJipDeathStatesPerChunk;
        const std::size_t death_end = (std::min)(
            death_begin + kMaxPresentationJipDeathStatesPerChunk, death_count);
        state.weapon_triggers.assign(
            g_state.latest_weapon_trigger_states.begin() +
                static_cast<std::ptrdiff_t>(trigger_begin),
            g_state.latest_weapon_trigger_states.begin() +
                static_cast<std::ptrdiff_t>(trigger_end));
        state.weapon_aims.assign(
            g_state.latest_weapon_aim_states.begin() +
                static_cast<std::ptrdiff_t>(aim_begin),
            g_state.latest_weapon_aim_states.begin() +
                static_cast<std::ptrdiff_t>(aim_end));
        state.horn_states.assign(
            g_state.latest_horn_states.begin() +
                static_cast<std::ptrdiff_t>(horn_begin),
            g_state.latest_horn_states.begin() +
                static_cast<std::ptrdiff_t>(horn_end));
        state.terminal_deaths.assign(
            g_state.latest_terminal_deaths.begin() +
                static_cast<std::ptrdiff_t>(death_begin),
            g_state.latest_terminal_deaths.begin() +
                static_cast<std::ptrdiff_t>(death_end));
        std::vector<Byte> payload;
        if (encode_presentation_jip_state(state, payload) !=
            CombatPresentationCodecError::None ||
            !send_combat_payload(peer, MessageType::CombatPresentationJipState,
                                 Channel::Reliable, payload))
            sent = false;
    }
    if (sent)
        LOG_INFO("KRAKEN_MP_COMBAT jip_states_sent peer=%u chunks=%u shots=0",
                 peer, static_cast<unsigned>(chunk_count));
    return sent;
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
    if (!g_state.is_host || event.channel != Channel::Reliable ||
        std::find(g_state.peers.begin(), g_state.peers.end(), event.peer) ==
            g_state.peers.end())
        return;
    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr)
        return;
    WeaponCommand command{};
    const WeaponCommandCodecError decoded =
        decode_weapon_command(event.payload, command);
    if (!weapon_command_codec_succeeded(decoded) ||
        command.entity_id != controller->entity_id ||
        command.session_epoch != g_state.session_epoch ||
        command.entity_generation != controller->generation) {
        LOG_ERROR("drop weapon peer=%u code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (command.target_entity_id != kInvalidNetId) {
        EntityGeneration target_generation = kInvalidEntityGeneration;
        if (!g_state.entities.lookup_generation(command.target_entity_id,
                                                target_generation) ||
            target_generation != command.target_generation)
            return;
    }
    if (controller->host_vehicle_active) {
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(*controller);
        if (vehicle == nullptr || resolve_exact_weapon(*vehicle, command.gun) == nullptr) {
            LOG_ERROR("drop weapon peer=%u: attachment identity not materialized",
                      event.peer);
            return;
        }
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

void accept_combat_weapon_aim(const WeaponAimState& state)
{
    if (g_state.is_host || state.session_epoch == 0 ||
        state.session_epoch != g_state.session_epoch ||
        validate_weapon_aim_state(state) != CombatPresentationCodecError::None)
        return;
    if (state.shooter.net_id == g_state.local_entity_id)
        return;
    RemoteEntity* const remote = find_or_add_remote(state.shooter.net_id);
    if (remote == nullptr)
        return;
    if (remote->has_spawn && remote->generation != state.shooter.generation)
        return;
    if (!g_state.weapon_aim_tracker.accept(state))
        return;
    remote->weapon_aim = state;
    remote->has_weapon_aim = true;
}

void accept_combat_weapon_trigger(const WeaponTriggerState& state)
{
    if (g_state.is_host || state.session_epoch == 0 ||
        state.session_epoch != g_state.session_epoch ||
        validate_weapon_trigger_state(state) != CombatPresentationCodecError::None)
        return;
    if (state.shooter.net_id == g_state.local_entity_id)
        return;
    RemoteEntity* const remote = find_or_add_remote(state.shooter.net_id);
    if (remote == nullptr)
        return;
    if (remote->has_spawn && remote->generation != state.shooter.generation)
        return;
    if (remote->has_weapon_trigger &&
        remote->weapon_trigger.transition_id >= state.transition_id)
        return;
    remote->weapon_trigger = state;
    remote->has_weapon_trigger = true;
}

void accept_combat_shot_confirmed(const ShotConfirmed& state)
{
    if (g_state.is_host || state.session_epoch == 0 ||
        state.session_epoch != g_state.session_epoch ||
        validate_shot_confirmed(state) != CombatPresentationCodecError::None)
        return;
    if (state.shooter.net_id != g_state.local_entity_id) {
        RemoteEntity* const remote = find_or_add_remote(state.shooter.net_id);
        if (remote == nullptr)
            return;
        if (remote->has_spawn && remote->generation != state.shooter.generation)
            return;
    } else {
        EntityGeneration local_generation = kInvalidEntityGeneration;
        if (!g_state.entities.lookup_generation(state.shooter.net_id,
                                                local_generation) ||
            local_generation != state.shooter.generation)
            return;
    }
    if (!g_state.confirmed_shot_deduplicator.accept(
            state.session_epoch, state.shot_id))
        return;
    g_state.pending_confirmed_shots.push_back(state);
    if (g_state.pending_confirmed_shots.size() > kMaxPendingImpactFx)
        g_state.pending_confirmed_shots.erase(
            g_state.pending_confirmed_shots.begin());
    apply_pending_confirmed_shots();
}

void receive_combat_weapon_aim(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Unreliable ||
        event.peer == kInvalidPeer)
        return;
    WeaponAimState state{};
    if (decode_weapon_aim_state(event.payload, state) !=
        CombatPresentationCodecError::None)
        return;
    accept_combat_weapon_aim(state);
}

void receive_combat_weapon_trigger(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer)
        return;
    WeaponTriggerState state{};
    if (decode_weapon_trigger_state(event.payload, state) !=
        CombatPresentationCodecError::None)
        return;
    accept_combat_weapon_trigger(state);
}

void receive_combat_shot_confirmed(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer)
        return;
    ShotConfirmed state{};
    if (decode_shot_confirmed(event.payload, state) !=
        CombatPresentationCodecError::None)
        return;
    accept_combat_shot_confirmed(state);
}

void receive_combat_impact(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer || g_state.combat_runtime == nullptr)
        return;
    ImpactPresentation state{};
    if (decode_impact_presentation(event.payload, state) !=
            CombatPresentationCodecError::None ||
        state.session_epoch != g_state.session_epoch)
        return;
    const combat_runtime::RuntimeApplyResult result =
        g_state.combat_runtime->apply_impact(state);
    if (!combat_runtime::runtime_apply_succeeded(result))
        LOG_DEBUG("typed impact replica application closed result=%u event=%llu",
                  static_cast<unsigned>(result),
                  static_cast<unsigned long long>(state.event_id));
}

void receive_combat_damage(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer || g_state.combat_runtime == nullptr)
        return;
    DamageResult state{};
    if (decode_damage_result(event.payload, state) !=
            CombatPresentationCodecError::None ||
        state.session_epoch != g_state.session_epoch)
        return;
    const combat_runtime::RuntimeApplyResult result =
        g_state.combat_runtime->apply_damage(state);
    if (!combat_runtime::runtime_apply_succeeded(result))
        LOG_DEBUG("typed damage replica application closed result=%u event=%llu",
                  static_cast<unsigned>(result),
                  static_cast<unsigned long long>(state.event_id));
}

void receive_combat_death(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer || g_state.combat_runtime == nullptr)
        return;
    DeathWreckPresentation state{};
    if (decode_death_wreck_presentation(event.payload, state) !=
            CombatPresentationCodecError::None ||
        state.session_epoch != g_state.session_epoch)
        return;
    const combat_runtime::RuntimeApplyResult result =
        g_state.combat_runtime->apply_death(state);
    if (!combat_runtime::runtime_apply_succeeded(result))
        LOG_DEBUG("typed death replica application closed result=%u entity=%u",
                  static_cast<unsigned>(result), state.entity.net_id);
}

void receive_combat_wreck_archive_chunk(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer || g_state.combat_runtime == nullptr)
        return;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
    if (!g_state.combat_runtime->accept_wreck_archive_chunk(event.payload,
                                                             now_ms))
        LOG_DEBUG("drop unverified wreck archive chunk peer=%u", event.peer);
}

void receive_combat_horn(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer || g_state.combat_runtime == nullptr)
        return;
    HornState state{};
    if (decode_horn_state(event.payload, state) !=
            CombatPresentationCodecError::None ||
        state.session_epoch != g_state.session_epoch)
        return;
    const combat_runtime::RuntimeApplyResult result =
        g_state.combat_runtime->apply_horn(state);
    if (!combat_runtime::runtime_apply_succeeded(result))
        LOG_DEBUG("typed horn replica application closed result=%u entity=%u",
                  static_cast<unsigned>(result), state.vehicle.net_id);
}

void receive_combat_presentation_jip(const SessionEvent& event)
{
    if (g_state.is_host || event.channel != Channel::Reliable ||
        event.peer == kInvalidPeer || !g_state.client_jip_map_load ||
        !g_state.client_jip_map_load->map_ready_sent)
        return;
    PresentationJipState state{};
    if (decode_presentation_jip_state(event.payload, state) !=
        CombatPresentationCodecError::None ||
        state.session_epoch != g_state.session_epoch)
        return;
    const PresentationJipAssemblyResult result =
        g_state.presentation_jip_reassembler.accept(state);
    if (result != PresentationJipAssemblyResult::Complete)
        return;
    ReassembledPresentationJipState assembled{};
    if (!g_state.presentation_jip_reassembler.assemble(assembled))
        return;
    for (const WeaponAimState& aim : assembled.weapon_aims)
        accept_combat_weapon_aim(aim);
    for (const WeaponTriggerState& trigger : assembled.weapon_triggers)
        accept_combat_weapon_trigger(trigger);
    if (g_state.combat_runtime != nullptr) {
        PresentationJipState combat_state{};
        combat_state.session_epoch = assembled.session_epoch;
        combat_state.state_revision = assembled.state_revision;
        combat_state.server_tick = assembled.server_tick;
        combat_state.horn_states = assembled.horn_states;
        combat_state.terminal_deaths = assembled.terminal_deaths;
        const combat_runtime::RuntimeApplyResult result =
            g_state.combat_runtime->apply_jip(combat_state);
        if (!combat_runtime::runtime_apply_succeeded(result))
            LOG_DEBUG("typed JIP combat application closed result=%u",
                      static_cast<unsigned>(result));
    }
    g_state.presentation_jip_reassembler.clear();
    LOG_INFO("KRAKEN_MP_COMBAT jip_states_applied epoch=%u revision=%llu shots=0",
             assembled.session_epoch,
             static_cast<unsigned long long>(assembled.state_revision));
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
        if (g_state.is_host && new_peer &&
            g_state.match.state() != MatchState::Offline &&
            !g_state.match.can_join()) {
            LOG_INFO("rejecting peer=%u after match roster closed state=%s",
                     event.peer, to_string(g_state.match.state()));
            (void)send_match_reject(event.peer,
                                     g_state.match.state() == MatchState::Playing
                                         ? MatchRejectReason::ClosedAfterStart
                                         : MatchRejectReason::RosterFull);
            (void)g_state.transport.disconnect(event.peer);
            break;
        }
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
            if (g_state.match.state() == MatchState::Offline) {
                publish_host_baseline_to_peer(event.peer);
                send_world_loot_baseline(event.peer);
            }
            if (g_state.match.state() == MatchState::Forming ||
                g_state.match.state() == MatchState::Playing) {
                const MatchPlayerId player_id = event.peer + 1;
                if (match_player_for_peer(event.peer) == nullptr) {
                    if (!g_state.match.add_player(player_id, event.peer,
                                                  entity)) {
                        (void)send_match_reject(event.peer,
                                                 MatchRejectReason::RosterFull);
                        (void)g_state.transport.disconnect(event.peer);
                    } else {
                        (void)encode_and_send_match_ready_request(event.peer);
                    }
                }
            }
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
            if (const MatchPlayer* player = match_player_for_peer(event.peer))
                (void)g_state.match.remove_player(player->id);
            std::erase(g_state.match_jip_pending, event.peer + 1);
            erase_match_jip_barrier(event.peer);
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
            g_state.quest_projection_client.reset();
            g_state.quest_projection_committed = false;
            g_state.quest_play_pending = false;
            g_state.quest_replica_active = false;
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
        if (static_cast<std::uint8_t>(event.message_type) >=
                static_cast<std::uint8_t>(MessageType::MatchReadyRequest) &&
            static_cast<std::uint8_t>(event.message_type) <=
                static_cast<std::uint8_t>(MessageType::MatchLeave))
            handle_match_message(event);
        else if (event.message_type == MessageType::MatchMapReady)
            handle_match_message(event);
        else if (event.message_type == MessageType::MatchWorldSnapshot)
            receive_world_snapshot(event);
        else if (event.message_type == MessageType::MatchWorldDelta)
            receive_world_delta(event);
        else if (event.message_type == MessageType::MatchQuestSnapshot)
            receive_quest_snapshot(event);
        else if (event.message_type == MessageType::MatchQuestDelta)
            receive_quest_delta(event);
        else if (event.message_type == MessageType::MatchVehicleDescriptor)
            receive_vehicle_descriptor(event);
        else if (event.message_type == MessageType::MatchWorldReady)
            receive_world_ready(event);
        else if (!g_state.is_host && g_state.world_transfer_expected &&
                 !g_state.world_snapshot_committed &&
                 is_world_baseline_message(event.message_type))
            queue_world_baseline_packet(event);
        else if (event.message_type == MessageType::Snapshot)
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
        else if (event.message_type == MessageType::CombatWeaponAimState)
            receive_combat_weapon_aim(event);
        else if (event.message_type == MessageType::CombatWeaponTriggerState)
            receive_combat_weapon_trigger(event);
        else if (event.message_type == MessageType::CombatShotConfirmed)
            receive_combat_shot_confirmed(event);
        else if (event.message_type == MessageType::CombatImpactPresentation)
            receive_combat_impact(event);
        else if (event.message_type == MessageType::CombatDamageResult)
            receive_combat_damage(event);
        else if (event.message_type == MessageType::CombatDeathWreckPresentation)
            receive_combat_death(event);
        else if (event.message_type == MessageType::CombatWreckArchiveChunk)
            receive_combat_wreck_archive_chunk(event);
        else if (event.message_type == MessageType::CombatHornState)
            receive_combat_horn(event);
        else if (event.message_type == MessageType::CombatPresentationJipState)
            receive_combat_presentation_jip(event);
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
        {
            // Legacy ImpactDamage can remain decodable for compatibility, but
            // it is not an active multiplayer replica path. Typed impact,
            // damage, death, and horn messages own all remote presentation.
            if (IsSessionActive()) {
                LOG_WARNING("drop legacy ImpactDamage in active multiplayer peer=%u",
                            event.peer);
            } else {
                receive_impact_damage(event);
            }
        }
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
    if (g_state.is_host || g_state.visible_match_state != MatchState::Playing ||
        !g_state.quest_projection_committed ||
        !g_state.quest_projection_client.input_unlocked() ||
        g_state.local_entity_id == kInvalidNetId ||
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
    if (encode_input_command(input, payload) == InputCommandCodecError::None) {
        const TransportResult sent = g_state.session->send(
            g_state.peers.front(), MessageType::Input, Channel::Unreliable,
            payload);
        if (sent)
            emit_first_input_marker();
    }
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

    // Build the verified host result, then submit it to the same guarded
    // native boundary. Active replicas fail closed there; no presentation
    // scope is permitted to bypass the authority check.
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

    return call_original_vehicle_inflict_damage(original, &target, info);
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
    if (g_state.is_host || IsSessionActive())
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
        if (!g_state.is_host && event.target_entity_id == g_state.local_entity_id)
            g_state.client_leave_session_pending = true;
    }
    LOG_INFO("impact damage applied event=%u attacker=%u target=%u damage=%f health=%f dead=%u",
             event.event_id, event.attacker_entity_id, event.target_entity_id,
             event.damage, event.post_health, event.target_dead ? 1u : 0u);
    return true;
}

void apply_pending_impact_damage()
{
    if (g_state.is_host || IsSessionActive() ||
        g_state.pending_impact_damage.empty())
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
    if (g_state.is_host || IsSessionActive() ||
        event.channel != Channel::Reliable) {
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
        if (existing->owner_entity_id != g_state.local_entity_id) {
            LOG_ERROR("local client player slot is owned by another entity local=%u owner=%u",
                      g_state.local_entity_id, existing->owner_entity_id);
            return false;
        }
        ObjId bound_object_id = kInvalidObjId;
        EntityGeneration bound_generation = kInvalidEntityGeneration;
        const bool has_binding = g_state.entities.lookup_obj_id(
            g_state.local_entity_id, bound_object_id, bound_generation);
        if (has_binding &&
            bound_object_id == vehicle->GetId() &&
            bound_generation == existing->generation) {
            g_state.local_player_vehicle_obj_id = vehicle->GetId();
            return true;
        }
        if (has_binding && bound_generation != existing->generation) {
            LOG_ERROR("local client player slot generation mismatch entity=%u slot=%u registry=%u",
                      g_state.local_entity_id,
                      static_cast<unsigned>(existing->generation),
                      static_cast<unsigned>(bound_generation));
            return false;
        }
        // LoadMap replaces the engine-owned Player vehicle while the network
        // player slot and its generation remain valid. Explicitly retire the
        // stale map-local ObjId, then bind the new native vehicle. This is the
        // only permitted same-generation rebind path.
        if (has_binding &&
            g_state.entities.unbind_net_id(g_state.local_entity_id) !=
                EntityRegistryUnbindResult::Removed) {
            LOG_ERROR("local client map-transition unbind failed entity=%u oldObjId=%d",
                      g_state.local_entity_id, bound_object_id);
            return false;
        }
        const EntityRegistryBindResult rebound = g_state.entities.bind(
            g_state.local_entity_id, vehicle->GetId(), existing->generation);
        if (rebound != EntityRegistryBindResult::Inserted &&
            rebound != EntityRegistryBindResult::AlreadyBound) {
            LOG_ERROR("local client map-transition rebind failed entity=%u oldObjId=%d newObjId=%d code=%u",
                      g_state.local_entity_id, bound_object_id, vehicle->GetId(),
                      static_cast<unsigned>(rebound));
            return false;
        }
        g_state.local_player_vehicle_obj_id = vehicle->GetId();
        // Every native attachment pointer and path belongs to the replaced
        // map-local vehicle.  Do not let a cached lobby weapon identity cross
        // the LoadMap boundary; the next native weapon callback resolves the
        // selected gun from the new graph.
        g_state.local_weapon_gun = {};
        g_state.has_local_weapon_gun = false;
        g_state.local_weapon_gun_id = 0;
        g_state.local_weapon_trigger_held = false;
        g_state.has_local_weapon_aim = false;
        g_state.local_weapon_target_obj_id = kInvalidObjId;
        g_state.local_weapon_target_entity_id = kInvalidNetId;
        LOG_INFO("MP client rebound native vehicle after map transition entity=%u generation=%u oldObjId=%d newObjId=%d",
                 g_state.local_entity_id,
                 static_cast<unsigned>(existing->generation),
                 bound_object_id, vehicle->GetId());
        return true;
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

bool host_relative_spawn_is_clear(const hta::CVector& candidate,
                                  const ObjId ignored_object_id)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return false;
    constexpr float kMinimumSeparation = 8.0f;
    constexpr float kMinimumSeparationSquared =
        kMinimumSeparation * kMinimumSeparation;
    for (auto iterator = server->m_pObjects->begin();
         iterator != server->m_pObjects->end(); ++iterator) {
        hta::ai::Obj* const object = *iterator;
        hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
        if (vehicle == nullptr || object->GetDeletedStatus() ||
            object->GetId() == ignored_object_id)
            continue;
        const hta::CVector other = vehicle->GetGeometricCenter();
        const float dx = other.x - candidate.x;
        const float dz = other.z - candidate.z;
        if (dx * dx + dz * dz < kMinimumSeparationSquared)
            return false;
    }
    return true;
}

void apply_belong_to_object_tree(hta::ai::Obj& root,
                                 const std::int32_t belong,
                                 std::vector<hta::ai::Obj*>& visited)
{
    if (std::find(visited.begin(), visited.end(), &root) != visited.end())
        return;
    visited.push_back(&root);
    root.SetBelong(belong);
    auto& children = root.GetChildren();
    for (auto iterator = children.begin(); iterator != children.end();
         ++iterator) {
        if (iterator->second != nullptr)
            apply_belong_to_object_tree(*iterator->second, belong, visited);
    }
}

void apply_belong_to_vehicle_tree(hta::ai::Vehicle& vehicle,
                                  const std::int32_t belong)
{
    std::vector<hta::ai::Obj*> visited;
    visited.reserve(kMaxVehicleDescriptorNodes +
                    kMaxVehicleDescriptorCargoObjects);
    apply_belong_to_object_tree(vehicle, belong, visited);
    const std::array<hta::ai::GeomRepository*, 2> repositories{{
        vehicle.GetRepository(), vehicle.GetGroundRepository()}};
    for (hta::ai::GeomRepository* const repository : repositories) {
        if (repository == nullptr)
            continue;
        for (std::size_t index = 0; index < repository->m_slots.size();
             ++index) {
            const hta::ai::GeomRepositoryItem item = repository->GetItem(
                static_cast<std::int32_t>(index));
            hta::ai::Obj* const object =
                item.IsValid() && !item.bIsResourceItem()
                    ? item.GetObj() : nullptr;
            if (object != nullptr)
                apply_belong_to_object_tree(*object, belong, visited);
        }
    }
}

bool choose_host_relative_spawn(const PeerController& controller,
                                const hta::CVector& host_position,
                                hta::CVector& position)
{
    const std::uint32_t index = controller.entity_id > 1
        ? controller.entity_id - 1 : 0;
    constexpr float kPi = 3.14159265358979323846f;
    for (std::uint32_t attempt = 0; attempt != 64; ++attempt) {
        const std::uint32_t ring = attempt / 8;
        const std::uint32_t spoke = (index + attempt) % 8;
        const float radius = 10.0f + 8.0f * static_cast<float>(ring);
        const float angle = (2.0f * kPi / 8.0f) * static_cast<float>(spoke);
        const hta::CVector candidate{
            host_position.x + std::cos(angle) * radius,
            host_position.y,
            host_position.z + std::sin(angle) * radius};
        if (host_relative_spawn_is_clear(candidate, controller.vehicle_obj_id)) {
            position = candidate;
            return true;
        }
    }
    // Never fall back to a colocated/fixed transform.  A full candidate set
    // is a real collision failure and the caller will retry after the native
    // world changes.
    LOG_WARNING("host-relative spawn has no collision-free candidate entity=%u",
                controller.entity_id);
    return false;
}

std::optional<std::int32_t> assigned_spawn_belong(
    const PeerController& controller)
{
    if (const std::optional<MatchSpawn> assigned =
            g_state.match.spawn_for(controller.entity_id))
        return assigned->belong;
    return std::nullopt;
}

bool resolve_host_remote_spawn_transform(const PeerController& controller,
                                         hta::CVector& position,
                                         hta::Quaternion& rotation)
{
    // MatchSpawn is authoritative and ordered by roster join order.  It is
    // independent of any map-owned XML object.
    if (const std::optional<MatchSpawn> assigned =
            g_state.match.spawn_for(controller.entity_id)) {
        position = {assigned->x, assigned->y, assigned->z};
        rotation = hta::Quaternion(hta::CVector(0.0f, assigned->yaw, 0.0f));
        return true;
    }
    if (g_state.spawn_together) {
        hta::ai::Player* const player = hta::ai::Player::Instance();
        hta::ai::Vehicle* const host = player ? player->GetVehicle() : nullptr;
        if (host == nullptr)
            return false;
        if (!choose_host_relative_spawn(controller, host->GetPosition(),
                                         position))
            return false;
        rotation = host->GetRotation();
        return true;
    }
    // Legacy named markers remain an optional fallback.  If absent, use the
    // host-relative formation so gameplay does not depend on map XML.
    const PlayerSlotIndex index = player_slot_for_entity(controller.entity_id);
    if (index < kLegacyNamedPlayerSlotCount &&
        resolve_player_spawn_transform(index, position, rotation))
        return true;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const host = player ? player->GetVehicle() : nullptr;
    if (host == nullptr)
        return false;
    if (!choose_host_relative_spawn(controller, host->GetPosition(), position))
        return false;
    rotation = host->GetRotation();
    return true;
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
    const std::int32_t belong = assigned_spawn_belong(controller).value_or(
        vehicle.GetBelong());
    apply_belong_to_vehicle_tree(vehicle, belong);
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
    const MatchState match_state = g_state.match.state();
    if (match_state != MatchState::Synchronizing &&
        match_state != MatchState::Playing)
        return nullptr;
    const bool has_rich_descriptor = controller.has_descriptor &&
        controller.descriptor.prototype_id >= 0;
    if (controller.host_vehicle_active)
        return ensure_host_vehicle(controller);
    if (!controller.spawn_attempt.can_attempt(g_state.server_tick))
        return nullptr;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return nullptr;

    // The fourth ObjContainer argument is the object's native belong/team.
    // Suspended creation uses the neutral value while archive state is
    // restored; the full graph receives its authoritative relationship before
    // PostLoad and never enters an update pass with the neutral value.
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
    const bool has_legacy_loadout = controller.has_loadout &&
        controller.loadout.base_prototype_id >= 0;
    const bool use_exact_host_clone = !has_rich_descriptor &&
        !has_legacy_loadout;
    if (use_exact_host_clone && local_vehicle == nullptr) {
        controller.spawn_attempt.defer(g_state.server_tick, 30);
        return nullptr;
    }
    const std::int32_t prototype_id = has_rich_descriptor
        ? controller.descriptor.prototype_id
        : (has_legacy_loadout ? controller.loadout.base_prototype_id
                              : local_vehicle->GetPrototypeId());
    VehicleDescriptor host_clone_descriptor{};
    if (use_exact_host_clone) {
        host_clone_descriptor = make_vehicle_descriptor(
            *local_vehicle, nullptr, object_name);
        if (host_clone_descriptor.native_structure.empty() ||
            host_clone_descriptor.prototype_id < 0) {
            controller.spawn_attempt.reject_permanently();
            LOG_ERROR("host clone fallback rejected entity=%u: native archive unavailable",
                      controller.entity_id);
            return nullptr;
        }
        LOG_WARNING("client descriptor absent; using exact host vehicle clone entity=%u prototype=%d",
                    controller.entity_id, host_clone_descriptor.prototype_id);
    }
    // The suspended native object is map-neutral until archive restoration is
    // complete.  Belong is assigned to the full graph immediately afterward;
    // assigning it here would make the archive path observe a partial tree.
    const ObjId object_id = server->m_pObjects->CreateNewObjectWithSuspendedPostLoad(
        prototype_id, object_name, -1, -1);
    if (object_id < 0) {
        controller.spawn_attempt.defer(g_state.server_tick, 30);
        LOG_ERROR("host dynamic player creation failed entity=%u generation=%u prototype=%d",
                  controller.entity_id, static_cast<unsigned>(controller.generation),
                  controller.loadout.base_prototype_id);
        return nullptr;
    }
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id);
    hta::ai::Vehicle* vehicle = vehicle_from_object(object);
    if (vehicle == nullptr || vehicle->GetPrototypeId() != prototype_id) {
        controller.spawn_attempt.reject_permanently();
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("host dynamic player prototype rejected entity=%u requested=%d objId=%d",
                  controller.entity_id, prototype_id, object_id);
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
    const VehicleDescriptor* const descriptor_to_apply = has_rich_descriptor
        ? &controller.descriptor
        : (use_exact_host_clone ? &host_clone_descriptor : nullptr);
    if (descriptor_to_apply != nullptr &&
        !apply_vehicle_descriptor_to_inactive_vehicle(
            *vehicle, *descriptor_to_apply,
            has_rich_descriptor ? "host client descriptor" :
                                  "host exact-clone fallback")) {
        controller.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        LOG_ERROR("host client vehicle descriptor v3 rejected entity=%u generation=%u",
                  controller.entity_id, static_cast<unsigned>(controller.generation));
        (void)send_match_reject(controller.peer, MatchRejectReason::InvalidRequest);
        (void)g_state.transport.disconnect(controller.peer);
        return nullptr;
    }
    const std::optional<MatchSpawn> assigned = g_state.match.spawn_for(
        controller.entity_id);
    const std::int32_t assigned_belong = assigned.has_value()
        ? assigned->belong : remote_belong;
    apply_belong_to_vehicle_tree(*vehicle, assigned_belong);
    vehicle->SetPostPosition(initial_position);
    vehicle->SetPostRotation(initial_rotation);
    if (!complete_suspended_vehicle_loadout(*server->m_pObjects, object_id,
                                            vehicle, "host dynamic player")) {
        controller.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        controller.vehicle_obj_id = kInvalidObjId;
        return nullptr;
    }
    if (descriptor_to_apply != nullptr &&
        !validate_vehicle_descriptor_structure(
            *vehicle, *descriptor_to_apply, "host dynamic player")) {
        controller.spawn_attempt.reject_permanently();
        // PostLoad has already linked this graph into ODE/collision cells.
        // Retire it in place; asynchronous removal here leaves null cell
        // entries until purge and crashes the native collision walker.
        retire_network_vehicle(*server->m_pObjects, *object, *vehicle, true);
        controller.vehicle_obj_id = kInvalidObjId;
        controller.host_vehicle_active = false;
        LOG_ERROR("host vehicle descriptor structural validation failed entity=%u generation=%u objId=%d",
                  controller.entity_id, static_cast<unsigned>(controller.generation),
                  object_id);
        return nullptr;
    }
    vehicle->RefreshMass();
    const bool loadout_matches = has_rich_descriptor || use_exact_host_clone ||
        vehicle_matches_loadout(*vehicle, controller.loadout);
    if (!loadout_matches) {
        // Only an actual part replacement requires retiring the native ODE
        // graph.  A Disable/Enable cycle on an untouched vehicle loses the
        // Wheel::m_suspensionNode pointers that `_EvaluateToDead` requires.
        vehicle->DisablePhysics();
        if (!has_rich_descriptor && !use_exact_host_clone &&
            !apply_loadout_to_inactive_vehicle(*vehicle, controller.loadout,
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

void publish_host_horn_transition(PeerController& controller,
                                  hta::ai::Vehicle& vehicle)
{
    const bool active = controller.input.horn;
    if (controller.have_horn_state && controller.last_horn_state == active)
        return;
    HornState state{};
    state.session_epoch = g_state.session_epoch;
    state.transition_id = controller.next_horn_transition++;
    if (controller.next_horn_transition == 0)
        controller.next_horn_transition = 1;
    state.server_tick = g_state.server_tick;
    state.vehicle = {controller.entity_id, controller.generation};
    state.active = active;
    if (active) {
        const hta::ai::VehiclePrototypeInfo* const info =
            vehicle.GetPrototypeInfo();
        const char* const horn_name =
            info != nullptr ? info->m_hornSoundName.c_str() : nullptr;
        if (horn_name == nullptr || horn_name[0] == '\0' ||
            !try_make_resource_cue(horn_name, state.horn_cue)) {
            LOG_WARNING("host horn transition closed: canonical cue unavailable entity=%u",
                        controller.entity_id);
            return;
        }
    }
    controller.last_horn_state = active;
    controller.have_horn_state = true;
    publish_host_horn(state);
}

void publish_host_local_horn_state()
{
    if (!g_state.is_host)
        return;
    hta::ai::DynamicScene* const scene = hta::ai::DynamicScene::Instance();
    hta::ai::Vehicle* const vehicle =
        scene != nullptr ? scene->GetVehicleControlledByPlayer() : nullptr;
    if (vehicle == nullptr)
        return;
    NetId entity_id = kInvalidNetId;
    EntityGeneration generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_net_id(vehicle->GetId(), entity_id,
                                        generation))
        return;
    const bool active = vehicle->GetHorn();
    if (g_state.have_local_horn_state && g_state.local_horn_state == active)
        return;
    HornState state{};
    state.session_epoch = g_state.session_epoch;
    state.transition_id = g_state.next_local_horn_transition++;
    if (g_state.next_local_horn_transition == 0)
        g_state.next_local_horn_transition = 1;
    state.server_tick = g_state.server_tick;
    state.vehicle = {entity_id, generation};
    state.active = active;
    if (active) {
        const hta::ai::VehiclePrototypeInfo* const info =
            vehicle->GetPrototypeInfo();
        const char* const horn_name =
            info != nullptr ? info->m_hornSoundName.c_str() : nullptr;
        if (horn_name == nullptr || horn_name[0] == '\0' ||
            !try_make_resource_cue(horn_name, state.horn_cue))
            return;
    }
    g_state.local_horn_state = active;
    g_state.have_local_horn_state = true;
    publish_host_horn(state);
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
        publish_host_horn_transition(controller, *vehicle);
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
    hta::ai::Gun* const gun = resolve_exact_weapon(vehicle, command.gun);
    if (gun == nullptr) {
        LOG_ERROR("host cannot resolve exact network gun entity=%u vehicle=%d",
                  command.entity_id, vehicle.GetId());
        return false;
    }
    if (resolved_target_object != nullptr)
        gun->SetTargetId(resolved_target_object->GetId());
    else
        gun->SetTargetId(kInvalidObjId);
    if (command.has_aim_point && valid_weapon_aim_point(command.aim_point))
        gun->LookAtPoint(to_engine_vector(command.aim_point), command.aim_speed);
    g_replaying_network_fire = true;
    g_active_host_weapon_command = &command;
    (void)gun->Fire(command.trigger_held);
    g_active_host_weapon_command = nullptr;
    g_replaying_network_fire = false;
    if (!g_state.combat_autotest_scenario.empty() &&
        command.sequence % 20u == 0u) {
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST host-gun entity=%u objId=%d gun_attachment=%llu canFire=%u charge=%u pool=%u targetObj=%d",
                 command.entity_id, vehicle.GetId(),
                 static_cast<unsigned long long>(command.gun.attachment_id),
                 gun->CanFire() ? 1u : 0u,
                 gun->GetShellsInCurrentCharge(), gun->GetShellsInPool(),
                 resolved_target_object != nullptr ? resolved_target_object->GetId()
                                                   : kInvalidObjId);
    }
    // Gun::Fire reports whether this particular native update emitted a shot.
    // A false result is normal between rounds, while reloading, and on trigger
    // release; the network command was still applied successfully.
    return true;
}

bool apply_network_weapon_ammo(hta::ai::Vehicle& vehicle,
                               const GunAttachmentIdentity& identity,
                               const std::uint32_t shells_in_current_charge,
                               const std::uint32_t shells_in_pool,
                               const AmmoReloadState reload_state,
                               const ShotConfirmed* const confirmed)
{
    (void)reload_state;
    hta::ai::Gun* const gun = resolve_exact_weapon(vehicle, identity);
    if (gun == nullptr)
        return false;
    gun->SetShellsInCurrentCharge(shells_in_current_charge);
    gun->SetShellsInPool(shells_in_pool);
    if (confirmed != nullptr) {
        // LoRA-verified presentation flags consumed by 0x006DE1A0. They do
        // not enter recoil, recharge, projectile, damage, or RNG paths.
        gun->m_bWasShot = true;
        gun->m_bJustShot = true;
        gun->_UpdateNodeFiringAction();
    }
    return true;
}

void apply_pending_confirmed_shots()
{
    if (g_state.is_host || g_state.pending_confirmed_shots.empty())
        return;
    std::vector<ShotConfirmed> pending;
    pending.swap(g_state.pending_confirmed_shots);
    for (const ShotConfirmed& shot : pending) {
        hta::ai::Vehicle* const vehicle = find_vehicle(
            shot.shooter.net_id, shot.shooter.generation);
        if (vehicle == nullptr) {
            g_state.pending_confirmed_shots.push_back(shot);
            continue;
        }
        bool has_reload = g_state.has_local_confirmed_reload_state;
        AmmoReloadState previous_reload = g_state.local_confirmed_reload_state;
        RemoteEntity* const remote = shot.shooter.net_id == g_state.local_entity_id
            ? nullptr : find_or_add_remote(shot.shooter.net_id);
        if (remote != nullptr) {
            has_reload = remote->has_last_shot;
            previous_reload = remote->has_last_shot
                ? remote->last_shot.reload_state : AmmoReloadState::Ready;
        }
        const bool reload_transition = shot.reload_state == AmmoReloadState::Reloading &&
            (!has_reload || previous_reload != AmmoReloadState::Reloading);
        if (!apply_network_weapon_ammo(
                *vehicle, shot.gun, shot.shells_in_current_charge,
                shot.shells_in_pool, shot.reload_state, &shot)) {
            g_state.pending_confirmed_shots.push_back(shot);
            continue;
        }
        if (reload_transition)
            vehicle->PlaySoundOnRechargeWeapon();
        if (remote != nullptr) {
            remote->last_shot = shot;
            remote->has_last_shot = true;
        } else {
            g_state.local_confirmed_reload_state = shot.reload_state;
            g_state.has_local_confirmed_reload_state = true;
            g_state.presented_local_shot_id =
                static_cast<std::uint32_t>(shot.shot_id);
        }
        emit_combat_authority_marker(shot.shooter.net_id, shot.shot_id);
    }
}

void apply_host_weapons(const float elapsed_time)
{
    (void)elapsed_time;
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.has_weapon || !controller.host_vehicle_active)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(controller);
        if (vehicle == nullptr)
            continue;
        WeaponCommand command = controller.weapon;
        bool applied = false;
        hta::ai::Obj* resolved_target_object = nullptr;
        VehicleVector3 resolved_aim{};
        bool has_resolved_aim = false;
        if (command.target_entity_id != 0) {
            hta::ai::Vehicle* const target = find_vehicle(
                command.target_entity_id, command.target_generation);
            if (target != nullptr) {
                resolved_target_object = target;
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
            command.aim_point = resolved_aim;
            command.has_aim_point = true;
            applied = drive_network_gun(*vehicle, command,
                                         resolved_target_object);
        }
        LOG_INFO("weapon host entity=%u gun_attachment=%llu target=%u trigger=%u applied=%u",
                  command.entity_id,
                  static_cast<unsigned long long>(command.gun.attachment_id),
                  command.target_entity_id,
                  command.trigger_held ? 1u : 0u, applied ? 1u : 0u);
        if (has_resolved_aim) {
            NetEntityRef target_ref{};
            if (command.target_entity_id != kInvalidNetId)
                target_ref = {command.target_entity_id,
                              command.target_generation};
            WeaponAimState aim{};
            aim.session_epoch = command.session_epoch;
            aim.update_sequence = command.sequence;
            aim.server_tick = g_state.server_tick;
            aim.shooter = {command.entity_id, command.entity_generation};
            aim.gun = command.gun;
            aim.has_target = target_ref.net_id != kInvalidNetId;
            aim.target = target_ref;
            aim.aim_point = resolved_aim;
            const hta::CVector position = vehicle->GetPosition();
            const float dx = resolved_aim.x - position.x;
            const float dy = resolved_aim.y - position.y;
            const float dz = resolved_aim.z - position.z;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(length) && length > 0.0001f)
                aim.aim_direction = {dx / length, dy / length, dz / length};
            aim.aim_speed = command.aim_speed;
            controller.weapon_aim = aim;
            controller.has_weapon_aim = true;
            publish_host_weapon_aim(aim);
        }
        {
            hta::ai::Gun* const gun = resolve_exact_weapon(*vehicle,
                                                            command.gun);
            if (gun != nullptr) {
                const bool transition = !controller.has_weapon_trigger ||
                    controller.weapon_trigger.trigger_held != command.trigger_held;
                WeaponTriggerState trigger{};
                trigger.session_epoch = command.session_epoch;
                trigger.transition_id = transition
                    ? controller.next_weapon_transition++
                    : controller.weapon_trigger.transition_id;
                trigger.server_tick = g_state.server_tick;
                trigger.shooter = {command.entity_id, command.entity_generation};
                trigger.gun = command.gun;
                trigger.trigger_held = command.trigger_held;
                trigger.reloading = gun->GetChargeState() == hta::ai::Gun::csInCharging;
                trigger.shells_in_current_charge = gun->GetShellsInCurrentCharge();
                trigger.shells_in_pool = gun->GetShellsInPool();
                const float recharge = gun->GetRechargingTime();
                const float current = gun->GetCurrentRechargingTime();
                trigger.reload_fraction = recharge > 0.0f
                    ? (std::clamp)(current / recharge, 0.0f, 1.0f) : 0.0f;
                controller.weapon_trigger = trigger;
                controller.has_weapon_trigger = true;
                publish_host_weapon_trigger(trigger, transition);
            }
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
    // Host aim is applied exactly once by apply_host_weapons before Gun::Fire.
}

hta::CVector to_engine_vector(const VehicleVector3& value)
{
    return {value.x, value.y, value.z};
}

void apply_visual_weapon_aim(hta::ai::Vehicle& vehicle,
                             const GunAttachmentIdentity& identity,
                             const VehicleVector3& aim_point,
                             float aim_speed)
{
    if (!valid_weapon_aim_point(aim_point))
        return;
    if (!std::isfinite(aim_speed) || aim_speed < 0.0f || aim_speed > 100.0f)
        return;
    hta::ai::Gun* const gun = resolve_exact_weapon(vehicle, identity);
    if (gun == nullptr)
        return;
    gun->LookAtPoint(to_engine_vector(aim_point), aim_speed);
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

bool decode_descriptor_modifier_value(const VehicleModifier& modifier,
                                      hta::m3d::AIParam& value)
{
    const Byte* const data = modifier.value_payload.data();
    const std::size_t size = modifier.value_payload.size();
    switch (modifier.value_type) {
    case VehicleModifierValueType::Undefined:
        value = hta::m3d::AIParam{};
        return size == 0;
    case VehicleModifierValueType::Vector: {
        if (size != sizeof(float) * 3) return false;
        float values[3]{};
        std::memcpy(values, data, sizeof(values));
        value = hta::CVector(values[0], values[1], values[2]);
        return true;
    }
    case VehicleModifierValueType::Quaternion: {
        if (size != sizeof(float) * 4) return false;
        float values[4]{};
        std::memcpy(values, data, sizeof(values));
        value = hta::Quaternion{values[0], values[1], values[2], values[3]};
        return true;
    }
    case VehicleModifierValueType::Id: {
        if (size != sizeof(std::int32_t)) return false;
        std::int32_t id = 0;
        std::memcpy(&id, data, sizeof(id));
        value = id;
        return true;
    }
    case VehicleModifierValueType::Float: {
        if (size != sizeof(float)) return false;
        float scalar = 0.0f;
        std::memcpy(&scalar, data, sizeof(scalar));
        value = scalar;
        return std::isfinite(scalar);
    }
    case VehicleModifierValueType::String: {
        const std::string text(reinterpret_cast<const char*>(data), size);
        value = hta::CStr(text.c_str());
        return true;
    }
    case VehicleModifierValueType::IdList: {
        if (size % sizeof(std::int32_t) != 0) return false;
        vc3::vector<std::int32_t> ids(size / sizeof(std::int32_t));
        if (!ids.empty()) std::memcpy(&ids[0], data, size);
        value = ids;
        return true;
    }
    case VehicleModifierValueType::Range: {
        if (size != sizeof(float) * 2) return false;
        float values[2]{};
        std::memcpy(values, data, sizeof(values));
        value = hta::CVector2(values[0], values[1]);
        return true;
    }
    case VehicleModifierValueType::StringList: {
        // AIParam's textual form is the only native-stable representation
        // available for a string-list value; keep it opaque if the engine
        // rejects the text rather than inventing a vector ABI.
        const std::string text(reinterpret_cast<const char*>(data), size);
        value.ReadFromString(hta::CStr(text.c_str()));
        return true;
    }
    }
    return false;
}

hta::ai::VehiclePart* resolve_descriptor_part(
    hta::ai::Vehicle& vehicle,
    const std::vector<VehicleDescriptorNode>& nodes,
    const VehicleDescriptorNode& node);

bool apply_descriptor_state(hta::ai::Obj& object,
                            const std::vector<VehicleAffix>& prefixes,
                            const std::vector<VehicleAffix>& suffixes,
                            const std::vector<VehicleModifier>& modifiers,
                            const char* const context)
{
    // Obj::LoadFromXML treats AffixesWasApplied as a policy switch: depending
    // on the source save it can restore only the affix IDs or omit an affix
    // whose resource-name lookup differs.  The typed descriptor is therefore
    // the authoritative, idempotent supplement to native XML.  Existing exact
    // state is retained; only missing state crosses ApplyAffix/ApplyModifier.
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::AffixManager* const manager =
        server != nullptr ? server->GetAffixManager() : nullptr;
    const auto apply_affixes = [&](const std::vector<VehicleAffix>& affixes,
                                   auto& applied_ids,
                                   const hta::ai::AffixType expected_type) {
        for (const VehicleAffix& descriptor : affixes) {
            hta::ai::Affix* const affix = manager != nullptr
                ? manager->GetAffixById(descriptor.id) : nullptr;
            const hta::ai::AffixGroup* const group =
                affix != nullptr ? affix->GetAffixGroup() : nullptr;
            if (affix == nullptr || group == nullptr ||
                affix->GetName().c_str() == nullptr ||
                group->GetName().c_str() == nullptr ||
                descriptor.name != affix->GetName().c_str() ||
                descriptor.target_resource_id != group->GetTargetResourceId() ||
                descriptor.target_resource_name != group->GetName().c_str() ||
                affix->GetAffixType() != expected_type) {
                LOG_ERROR("%s descriptor affix identity mismatch objId=%d affix=%d name=%s target=%d/%s",
                          context, object.GetId(), descriptor.id,
                          descriptor.name.c_str(), descriptor.target_resource_id,
                          descriptor.target_resource_name.c_str());
                return false;
            }
            if (std::find(applied_ids.begin(), applied_ids.end(), descriptor.id) !=
                applied_ids.end())
                continue;
            if (!object.ApplyAffix(affix) ||
                std::find(applied_ids.begin(), applied_ids.end(), descriptor.id) ==
                    applied_ids.end()) {
                LOG_ERROR("%s descriptor affix apply failed objId=%d affix=%d",
                          context, object.GetId(), descriptor.id);
                return false;
            }
        }
        return true;
    };
    if (!apply_affixes(prefixes, object.m_appliedPrefixIds,
                       hta::ai::AFFIXTYPE_PREFIX) ||
        !apply_affixes(suffixes, object.m_appliedSuffixIds,
                       hta::ai::AFFIXTYPE_SUFFIX))
        return false;
    for (const VehicleModifier& descriptor : modifiers) {
        const auto exact = std::find_if(
            object.m_modifiers.begin(), object.m_modifiers.end(),
            [&descriptor](const hta::ai::Modifier& candidate) {
                VehicleModifier native{};
                capture_native_modifier(candidate, native);
                return native == descriptor;
            });
        if (exact != object.m_modifiers.end())
            continue;
        hta::m3d::AIParam value{};
        if (!decode_descriptor_modifier_value(descriptor, value)) {
            LOG_ERROR("%s descriptor modifier payload invalid objId=%d property=%s",
                      context, object.GetId(), descriptor.property_name.c_str());
            return false;
        }
        std::aligned_storage_t<sizeof(hta::ai::Modifier),
                               alignof(hta::ai::Modifier)> storage{};
        hta::ai::Modifier* const native =
            reinterpret_cast<hta::ai::Modifier*>(&storage);
        ::CallCtor<0x007DEE40, hta::ai::Modifier>(native);
        native->m_timeOut = descriptor.timeout;
        native->m_Operation = static_cast<hta::ai::eModifierOperation>(
            static_cast<std::uint8_t>(descriptor.operation));
        native->m_magicPrototypeId = descriptor.magic_prototype_id;
        native->m_PropertyName = hta::CStr(descriptor.property_name.c_str());
        native->m_SenderID = descriptor.sender_id;
        native->m_Value = value;
        const bool applied = object.ApplyModifier(*native);
        ::CallDtor<0x007DEF20, hta::ai::Modifier>(native);
        if (!applied) {
            LOG_ERROR("%s descriptor modifier apply failed objId=%d property=%s",
                      context, object.GetId(), descriptor.property_name.c_str());
            return false;
        }
    }
    return true;
}

const VehicleDescriptorNode* descriptor_node_by_id(
    const std::vector<VehicleDescriptorNode>& nodes,
    const VehicleInstanceId instance_id)
{
    const auto found = std::find_if(
        nodes.begin(), nodes.end(), [instance_id](const auto& node) {
            return node.instance_id == instance_id;
        });
    return found == nodes.end() ? nullptr : &*found;
}

hta::ai::VehiclePart* resolve_descriptor_part(
    hta::ai::Vehicle& vehicle,
    const std::vector<VehicleDescriptorNode>& nodes,
    const VehicleDescriptorNode& node)
{
    std::vector<const VehicleDescriptorNode*> chain;
    const VehicleDescriptorNode* current = &node;
    for (std::size_t count = 0; current != nullptr && count <= nodes.size();
         ++count) {
        chain.push_back(current);
        if (current->parent_instance_id == 0)
            break;
        current = descriptor_node_by_id(nodes, current->parent_instance_id);
    }
    if (chain.empty() || chain.back()->parent_instance_id != 0)
        return nullptr;
    std::reverse(chain.begin(), chain.end());

    hta::ai::VehiclePart* part = nullptr;
    for (const VehicleDescriptorNode* const element : chain) {
        if (part == nullptr) {
            part = vehicle.GetPartByName(hta::CStr(element->slot.c_str()));
            continue;
        }
        if (!part->IsKindOf(hta::ai::CompoundVehiclePart::p_classObject))
            return nullptr;
        const auto& compound = static_cast<const hta::ai::CompoundVehiclePart&>(*part);
        const hta::ai::VehiclePart* child = nullptr;
        for (auto iterator = compound.begin(); iterator != compound.end();
             ++iterator) {
            if (iterator->first.c_str() != nullptr &&
                element->slot == iterator->first.c_str()) {
                if (child != nullptr)
                    return nullptr;
                child = iterator->second.vp;
            }
        }
        if (child == nullptr)
            return nullptr;
        part = const_cast<hta::ai::VehiclePart*>(child);
    }
    return part;
}

bool append_native_part_bindings(
    const hta::ai::VehiclePart& part, const std::string& parent_path,
    std::vector<VehicleArchiveNameBinding>& output)
{
    if (output.size() >= kMaxVehicleDescriptorNodes)
        return false;
    const std::string prototype_name = native_prototype_name(part);
    if (prototype_name.empty() || part.GetPrototypeId() < 0)
        return false;
    output.push_back({parent_path, part.GetPrototypeId(), prototype_name});
    if (!part.IsKindOf(hta::ai::CompoundVehiclePart::p_classObject))
        return true;
    const auto& compound = static_cast<const hta::ai::CompoundVehiclePart&>(part);
    for (auto iterator = compound.begin(); iterator != compound.end();
         ++iterator) {
        if (iterator->second.vp == nullptr || iterator->first.c_str() == nullptr)
            return false;
        std::string child_path = parent_path;
        child_path += "/";
        child_path += iterator->first.c_str();
        if (!append_native_part_bindings(*iterator->second.vp, child_path,
                                         output))
            return false;
    }
    return true;
}

bool collect_native_part_bindings(
    hta::ai::Vehicle& vehicle,
    std::vector<VehicleArchiveNameBinding>& output)
{
    const auto names = vehicle.GetAttachedPartNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        const hta::CStr& slot = names[index];
        const hta::ai::VehiclePart* const part = vehicle.GetPartByName(slot);
        if (part == nullptr || slot.c_str() == nullptr ||
            !append_native_part_bindings(*part, slot.c_str(), output))
            return false;
    }
    return true;
}

bool reconcile_vehicle_descriptor_state(hta::ai::Vehicle& vehicle,
                                        const VehicleDescriptor& descriptor,
                                        const char* const context)
{
    if (!apply_descriptor_state(vehicle, descriptor.prefixes,
                                descriptor.suffixes, descriptor.modifiers,
                                context))
        return false;
    for (const VehicleDescriptorNode& node : descriptor.attachments) {
        hta::ai::VehiclePart* const part = resolve_descriptor_part(
            vehicle, descriptor.attachments, node);
        if (part == nullptr || part->GetPrototypeId() != node.prototype_id ||
            native_prototype_name(*part) != node.prototype_name) {
            LOG_ERROR("%s descriptor state target missing objId=%d slot=%s expected=%d/%s",
                      context, vehicle.GetId(), node.slot.c_str(),
                      node.prototype_id, node.prototype_name.c_str());
            return false;
        }
        if (!apply_descriptor_state(*part, node.prefixes, node.suffixes,
                                    node.modifiers, context))
            return false;
    }
    return true;
}

bool normalize_native_vehicle_attachment_roots(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr) {
        LOG_ERROR("%s attachment-root object container unavailable objId=%d",
                  context, vehicle.GetId());
        return false;
    }
    // m_bNeedPostLoad is the authoritative transaction boundary here.
    // Freshly created Vehicles can retain their prototype's
    // bIsUpdatingByODE policy bit before _InternalPostLoad has created any
    // ODE body, so that bit cannot distinguish a suspended graph from a live
    // one.
    if (!vehicle.m_bNeedPostLoad) {
        LOG_ERROR("%s attachment-root normalization requires suspended vehicle objId=%d",
                  context, vehicle.GetId());
        return false;
    }

    const ObjId object_id = vehicle.GetId();
    for (const VehicleDescriptorNode& requested : descriptor.attachments) {
        if (requested.parent_instance_id != 0)
            continue;

        hta::ai::Obj* const object =
            server->m_pObjects->GetEntityByObjId(object_id);
        hta::ai::Vehicle* const current = vehicle_from_object(object);
        if (current != &vehicle || !current->m_bNeedPostLoad) {
            LOG_ERROR("%s attachment-root ownership/suspension check failed objId=%d slot=%s",
                      context, object_id, requested.slot.c_str());
            return false;
        }

        const hta::CStr slot(requested.slot.c_str());
        hta::ai::VehiclePart* const existing = current->GetPartByName(slot);
        if (existing != nullptr &&
            existing->GetPrototypeId() == requested.prototype_id &&
            native_prototype_name(*existing) == requested.prototype_name)
            continue;

        // CreateNewObjectWithSuspendedPostLoad builds the base prototype's
        // stock attachment graph.  NPC generators are allowed to replace a
        // root part afterwards, so that stock graph is not necessarily the
        // graph captured by the host.  SetNewPart is the engine's native,
        // ownership-aware boundary for making that exact replacement before
        // PostLoad.  Its compound children are then checked by the full
        // archive preflight below; no mismatch is hidden or guessed.
        if (!current->SetNewPart(slot,
                                 hta::CStr(requested.prototype_name.c_str()))) {
            LOG_ERROR("%s native attachment-root replacement failed objId=%d slot=%s expected=%d/%s actual=%d/%s",
                      context, object_id, requested.slot.c_str(),
                      requested.prototype_id, requested.prototype_name.c_str(),
                      existing != nullptr ? existing->GetPrototypeId() : -1,
                      existing != nullptr ? native_prototype_name(*existing).c_str()
                                          : "<missing>");
            return false;
        }

        // SetNewPart creates/removes native objects.  Resolve the vehicle and
        // the part again before dereferencing either of them.
        hta::ai::Obj* const rebound_object =
            server->m_pObjects->GetEntityByObjId(object_id);
        hta::ai::Vehicle* const rebound = vehicle_from_object(rebound_object);
        hta::ai::VehiclePart* const replacement =
            rebound != nullptr ? rebound->GetPartByName(slot) : nullptr;
        if (rebound != &vehicle || !rebound->m_bNeedPostLoad ||
            replacement == nullptr ||
            replacement->GetPrototypeId() != requested.prototype_id ||
            native_prototype_name(*replacement) != requested.prototype_name) {
            LOG_ERROR("%s attachment-root replacement verification failed objId=%d slot=%s expected=%d/%s actual=%d/%s",
                      context, object_id, requested.slot.c_str(),
                      requested.prototype_id, requested.prototype_name.c_str(),
                      replacement != nullptr ? replacement->GetPrototypeId() : -1,
                      replacement != nullptr
                          ? native_prototype_name(*replacement).c_str()
                          : "<missing>");
            return false;
        }
    }
    return true;
}

hta::ai::GeomRepository* cargo_repository_for(
    hta::ai::Vehicle& vehicle, const VehicleCargoRepository repository)
{
    return repository == VehicleCargoRepository::Ground
        ? vehicle.GetGroundRepository()
        : vehicle.GetRepository();
}

struct VehicleCargoRestoreTransaction {
    hta::ai::ObjContainer& objects;
    std::vector<ObjId> created_object_ids;
    std::vector<std::pair<hta::ai::GeomRepository*, ObjId>> placed_objects;
    bool committed = false;

    ~VehicleCargoRestoreTransaction()
    {
        if (committed)
            return;
        for (auto iterator = placed_objects.rbegin();
             iterator != placed_objects.rend(); ++iterator) {
            if (iterator->first != nullptr)
                (void)iterator->first->GiveUpThingByObjId(iterator->second);
        }
        for (auto iterator = created_object_ids.rbegin();
             iterator != created_object_ids.rend(); ++iterator) {
            if (objects.GetEntityByObjId(*iterator) != nullptr)
                objects.AddObjIdToRemove(*iterator);
        }
    }
};

bool clear_suspended_vehicle_repositories(hta::ai::Vehicle& vehicle,
                                          const char* const context)
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr ||
        !vehicle.m_bNeedPostLoad)
        return false;
    const std::array<hta::ai::GeomRepository*, 2> repositories{{
        vehicle.GetRepository(), vehicle.GetGroundRepository()}};
    for (std::size_t repository_index = 0;
         repository_index < repositories.size(); ++repository_index) {
        hta::ai::GeomRepository* const repository =
            repositories[repository_index];
        if (repository == nullptr) {
            LOG_ERROR("%s suspended repository unavailable objId=%d repo=%u",
                      context, vehicle.GetId(),
                      static_cast<unsigned>(repository_index));
            return false;
        }
        std::vector<ObjId> detached_objects;
        detached_objects.reserve(repository->m_slots.size());
        for (std::size_t slot = 0; slot < repository->m_slots.size(); ++slot) {
            const hta::ai::GeomRepositoryItem item = repository->GetItem(
                static_cast<std::int32_t>(slot));
            if (!item.IsValid() || item.bIsResourceItem())
                continue;
            hta::ai::Obj* const object = item.GetObj();
            if (object == nullptr || object->GetParentRepository() != repository) {
                LOG_ERROR("%s suspended repository has invalid object binding vehicle=%d repo=%u slot=%u",
                          context, vehicle.GetId(),
                          static_cast<unsigned>(repository_index),
                          static_cast<unsigned>(slot));
                return false;
            }
            detached_objects.push_back(object->GetId());
        }
        // LoRA/game.pdb: Clear(false) invokes GeomRepositoryItem::Clear(false)
        // for every slot, which only detaches object cargo from the repository;
        // unlike Clear(true), it does not invoke the object's visual-world
        // transition.  This is the safe suspended-graph normalization seam.
        repository->Clear(false);
        if (!repository->IsEmpty()) {
            LOG_ERROR("%s suspended repository clear failed vehicle=%d repo=%u",
                      context, vehicle.GetId(),
                      static_cast<unsigned>(repository_index));
            return false;
        }
        for (const ObjId object_id : detached_objects)
            if (server->m_pObjects->GetEntityByObjId(object_id) != nullptr)
                server->m_pObjects->AddObjIdToRemove(object_id);
    }
    return true;
}

hta::ai::Obj* find_named_child(hta::ai::Obj& parent,
                               const std::string& name);

hta::ai::Obj* restore_vehicle_cargo_node(
    const VehicleDescriptorNode& node, hta::ai::ObjContainer& objects,
    VehicleCargoRestoreTransaction& transaction, const char* const context,
    const std::size_t depth)
{
    if (depth > kMaxVehicleDescriptorCargoDepth ||
        node.kind != VehicleDescriptorNodeKind::Container ||
        node.prototype_id < 0 || node.prototype_name.empty() ||
        node.slot.empty()) {
        LOG_ERROR("%s cargo descriptor invalid depth=%u prototype=%d slot=%s",
                  context, static_cast<unsigned>(depth), node.prototype_id,
                  node.slot.empty() ? "<empty>" : node.slot.c_str());
        return nullptr;
    }
    const ObjId object_id = objects.CreateNewObjectWithSuspendedPostLoad(
        node.prototype_id, node.slot.c_str(), -1, -1);
    if (object_id < 0) {
        LOG_ERROR("%s cargo creation failed depth=%u prototype=%d/%s slot=%s",
                  context, static_cast<unsigned>(depth), node.prototype_id,
                  node.prototype_name.c_str(), node.slot.c_str());
        return nullptr;
    }
    transaction.created_object_ids.push_back(object_id);
    hta::ai::Obj* const object = objects.GetEntityByObjId(object_id);
    if (object == nullptr || !object->m_bNeedPostLoad ||
        object->GetPrototypeId() != node.prototype_id ||
        native_prototype_name(*object) != node.prototype_name) {
        LOG_ERROR("%s cargo prototype mismatch objId=%d expected=%d/%s actual=%d/%s suspended=%u",
                  context, object_id, node.prototype_id,
                  node.prototype_name.c_str(),
                  object != nullptr ? object->GetPrototypeId() : -1,
                  object != nullptr ? native_prototype_name(*object).c_str()
                                    : "<missing>",
                  object != nullptr && object->m_bNeedPostLoad ? 1u : 0u);
        return nullptr;
    }
    if (!apply_descriptor_state(*object, node.prefixes, node.suffixes,
                                node.modifiers, context))
        return nullptr;
    for (const VehicleDescriptorNode& child_node : node.cargo_objects) {
        hta::ai::Obj* const child = restore_vehicle_cargo_node(
            child_node, objects, transaction, context, depth + 1);
        if (child == nullptr)
            return nullptr;
        object->AddChild(child);
        if (find_named_child(*object, child_node.slot) != child) {
            LOG_ERROR("%s cargo child link failed parent=%d child=%d slot=%s",
                      context, object_id, child->GetId(),
                      child_node.slot.c_str());
            return nullptr;
        }
    }
    return object;
}

bool restore_vehicle_repository_objects(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    if (descriptor.cargo_objects.empty())
        return true;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return false;
    VehicleCargoRestoreTransaction transaction{*server->m_pObjects};
    for (const VehicleDescriptorNode& node : descriptor.cargo_objects) {
        hta::ai::GeomRepository* const repository = cargo_repository_for(
            vehicle, node.cargo_placement.repository);
        if (repository == nullptr) {
            LOG_ERROR("%s cargo repository unavailable repo=%u prototype=%d",
                      context,
                      static_cast<unsigned>(node.cargo_placement.repository),
                      node.prototype_id);
            return false;
        }
        hta::ai::Obj* const object = restore_vehicle_cargo_node(
            node, *server->m_pObjects, transaction, context, 0);
        if (object == nullptr)
            return false;
        const hta::PointBase<int> place{node.cargo_placement.x,
                                        node.cargo_placement.y};
        std::aligned_storage_t<sizeof(hta::ai::GeomRepositoryItem),
                               alignof(hta::ai::GeomRepositoryItem)> storage{};
        hta::ai::GeomRepositoryItem* const item =
            reinterpret_cast<hta::ai::GeomRepositoryItem*>(&storage);
        ::CallCtor<0x006CDBA0, hta::ai::GeomRepositoryItem>(item,
                                                            object->GetId());
        std::int32_t accepted = 0;
        const std::int32_t can_add = item->IsValid()
            ? repository->CanAddThingToPlace(*item, place, &accepted) : 0;
        const bool placed_exactly = item->IsValid() &&
            repository->AddThingToPlace(*item, place);
        // GeomRepository::LoadFromXML uses the same policy: saved PosX/PosY
        // is preferred, but stale/overlapping generator layout is fed through
        // native AddThing so the repository can deterministically repack it.
        const bool added = placed_exactly ||
            (item->IsValid() && repository->AddThing(*item, 0));
        if (!added) {
            const hta::PointBase<int> repository_size =
                repository->GetGeomSize();
            const hta::PointBase<int> item_size = item->GetGeomSize();
            LOG_ERROR("%s cargo placement failed objId=%d prototype=%d repo=%u place=%d,%d canAdd=%d accepted=%d repoSize=%d,%d itemSize=%d,%d slots=%u",
                      context, object->GetId(),
                      object->GetPrototypeId(),
                      static_cast<unsigned>(node.cargo_placement.repository),
                      node.cargo_placement.x, node.cargo_placement.y,
                      can_add, accepted, repository_size.x, repository_size.y,
                      item_size.x, item_size.y,
                      static_cast<unsigned>(repository->m_slots.size()));
            for (std::size_t slot_index = 0;
                 slot_index < repository->m_slots.size(); ++slot_index) {
                const hta::ai::GeomRepositoryItem occupied =
                    repository->GetItem(static_cast<std::int32_t>(slot_index));
                if (!occupied.IsValid())
                    continue;
                const hta::PointBase<int> occupied_size =
                    occupied.GetGeomSize();
                LOG_ERROR("%s cargo occupied slot=%u type=%u resource=%d objId=%d amount=%u origin=%d,%d size=%d,%d",
                          context, static_cast<unsigned>(slot_index),
                          occupied.bIsResourceItem() ? 0u : 1u,
                          occupied.GetResourceId(), occupied.GetObjId(),
                          occupied.GetAmount(), occupied.m_origin.x,
                          occupied.m_origin.y, occupied_size.x,
                          occupied_size.y);
            }
            item->~GeomRepositoryItem();
            return false;
        }
        item->~GeomRepositoryItem();
        transaction.placed_objects.emplace_back(repository, object->GetId());
        const std::int32_t slot = repository->GetSlotByObjId(object->GetId());
        if (slot < 0) {
            LOG_ERROR("%s cargo placement verification failed objId=%d repo=%u preferredPlace=%d,%d slot=%d",
                      context, object->GetId(),
                      static_cast<unsigned>(node.cargo_placement.repository),
                      node.cargo_placement.x, node.cargo_placement.y, slot);
            return false;
        }
        if (!placed_exactly) {
            LOG_INFO("%s cargo native repack accepted objId=%d prototype=%d repo=%u preferredPlace=%d,%d slot=%d",
                     context, object->GetId(), object->GetPrototypeId(),
                     static_cast<unsigned>(node.cargo_placement.repository),
                     node.cargo_placement.x, node.cargo_placement.y, slot);
        }
        const hta::ai::GeomRepositoryItem placed = repository->GetItem(slot);
        if (!placed.IsValid() || placed.GetObj() != object) {
            LOG_ERROR("%s cargo placement verification failed objId=%d repo=%u place=%d,%d slot=%d",
                      context, object->GetId(),
                      static_cast<unsigned>(node.cargo_placement.repository),
                      node.cargo_placement.x, node.cargo_placement.y, slot);
            return false;
        }
    }
    transaction.committed = true;
    return true;
}

bool restore_vehicle_repository_resources(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    for (const VehicleCargoStack& stack : descriptor.cargo_stacks) {
        hta::ai::GeomRepository* const repository = cargo_repository_for(
            vehicle, stack.placement.repository);
        if (repository == nullptr) {
            LOG_ERROR("%s repository missing repo=%u resource=%d",
                      context,
                      static_cast<unsigned>(stack.placement.repository),
                      stack.resource_id);
            return false;
        }
        const hta::PointBase<int> place{stack.placement.x,
                                        stack.placement.y};
        // The checked-in HTA headers expose this constructor but the native
        // import TU intentionally does not wrap it.  Invoke the verified
        // x86 constructor ABI into local storage, then use only the existing
        // repository placement boundary.  No pointer or ObjId crosses the
        // network and the temporary item is destroyed before returning.
        std::aligned_storage_t<sizeof(hta::ai::GeomRepositoryItem),
                               alignof(hta::ai::GeomRepositoryItem)> storage{};
        hta::ai::GeomRepositoryItem* const item =
            reinterpret_cast<hta::ai::GeomRepositoryItem*>(&storage);
        ::CallCtor<0x006CD0D0, hta::ai::GeomRepositoryItem>(
            item, stack.resource_id, stack.amount);
        const bool placed_exactly = item->IsValid() &&
            repository->AddThingToPlace(*item, place);
        const bool added = placed_exactly ||
            (item->IsValid() && repository->AddThing(*item, 0));
        item->~GeomRepositoryItem();
        if (!added) {
            LOG_ERROR("%s repository resource restore failed resource=%d amount=%u repo=%u place=%d,%d",
                      context, stack.resource_id, stack.amount,
                      static_cast<unsigned>(stack.placement.repository),
                      stack.placement.x, stack.placement.y);
            return false;
        }
        if (!placed_exactly)
            LOG_INFO("%s repository resource accepted by native repack resource=%d amount=%u repo=%u preferredPlace=%d,%d",
                     context, stack.resource_id, stack.amount,
                     static_cast<unsigned>(stack.placement.repository),
                     stack.placement.x, stack.placement.y);
    }
    return true;
}

bool validate_vehicle_repository_resources(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    struct ResourceTotal {
        std::int32_t resource_id = -1;
        std::uint64_t amount = 0;
    };
    std::array<std::vector<ResourceTotal>, 2> expected_by_repository;
    for (const VehicleCargoStack& stack : descriptor.cargo_stacks) {
        const unsigned repository_index = static_cast<unsigned>(
            stack.placement.repository);
        if (repository_index >= expected_by_repository.size())
            return false;
        auto& expected = expected_by_repository[repository_index];
        const auto existing = std::find_if(
            expected.begin(), expected.end(), [&stack](const ResourceTotal& value) {
                return value.resource_id == stack.resource_id;
            });
        if (existing == expected.end())
            expected.push_back({stack.resource_id, stack.amount});
        else
            existing->amount += stack.amount;
    }
    for (unsigned repository_index = 0; repository_index != 2;
         ++repository_index) {
        const auto repository_kind = repository_index == 0
            ? VehicleCargoRepository::Main : VehicleCargoRepository::Ground;
        hta::ai::GeomRepository* const repository = cargo_repository_for(
            vehicle, repository_kind);
        if (repository == nullptr) {
            LOG_ERROR("%s repository unavailable repo=%u",
                      context, repository_index);
            return false;
        }
        std::vector<ResourceTotal> actual;
        for (std::size_t index = 0; index < repository->m_slots.size();
             ++index) {
            const hta::ai::GeomRepositoryItem item = repository->GetItem(
                static_cast<std::int32_t>(index));
            if (!item.IsValid() || !item.bIsResourceItem())
                continue;
            const auto existing = std::find_if(
                actual.begin(), actual.end(), [&item](const ResourceTotal& value) {
                    return value.resource_id == item.GetResourceId();
                });
            if (existing == actual.end())
                actual.push_back({item.GetResourceId(), item.GetAmount()});
            else
                existing->amount += item.GetAmount();
        }
        const auto& expected = expected_by_repository[repository_index];
        const bool exact_totals = expected.size() == actual.size() &&
            std::all_of(expected.begin(), expected.end(),
                [&actual](const ResourceTotal& value) {
                    const auto matching = std::find_if(
                        actual.begin(), actual.end(),
                        [&value](const ResourceTotal& candidate) {
                            return candidate.resource_id == value.resource_id;
                        });
                    return matching != actual.end() &&
                        matching->amount == value.amount;
                });
        if (!exact_totals) {
            LOG_ERROR("%s repository resource totals mismatch repo=%u expectedKinds=%u actualKinds=%u",
                      context, repository_index,
                      static_cast<unsigned>(expected.size()),
                      static_cast<unsigned>(actual.size()));
            return false;
        }
    }
    return true;
}

bool validate_native_descriptor_state(
    const hta::ai::Obj& object,
    const std::vector<VehicleAffix>& prefixes,
    const std::vector<VehicleAffix>& suffixes,
    const std::vector<VehicleModifier>& modifiers,
    const char* const context)
{
    const auto has_affix = [](const auto& ids, const std::int32_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    for (const VehicleAffix& affix : prefixes)
        if (!has_affix(object.m_appliedPrefixIds, affix.id)) {
            LOG_ERROR("%s descriptor prefix missing objId=%d affix=%d",
                      context, object.GetId(), affix.id);
            return false;
        }
    for (const VehicleAffix& affix : suffixes)
        if (!has_affix(object.m_appliedSuffixIds, affix.id)) {
            LOG_ERROR("%s descriptor suffix missing objId=%d affix=%d",
                      context, object.GetId(), affix.id);
            return false;
        }
    for (const VehicleModifier& modifier : modifiers) {
        const auto found = std::find_if(
            object.m_modifiers.begin(), object.m_modifiers.end(),
            [&modifier](const hta::ai::Modifier& candidate) {
                return candidate.m_PropertyName.c_str() != nullptr &&
                    modifier.property_name == candidate.m_PropertyName.c_str();
            });
        if (found == object.m_modifiers.end()) {
            LOG_ERROR("%s descriptor modifier missing objId=%d property=%s",
                      context, object.GetId(),
                      modifier.property_name.c_str());
            return false;
        }
    }
    return true;
}

hta::ai::Obj* find_named_child(hta::ai::Obj& parent,
                               const std::string& name)
{
    auto& children = parent.GetChildren();
    hta::ai::Obj* result = nullptr;
    for (auto iterator = children.begin(); iterator != children.end();
         ++iterator) {
        hta::ai::Obj* const child = iterator->second;
        if (child == nullptr || child->GetName() == nullptr ||
            name != child->GetName())
            continue;
        if (result != nullptr)
            return nullptr;
        result = child;
    }
    return result;
}

bool validate_native_cargo_node(hta::ai::Obj& object,
                                const VehicleDescriptorNode& node,
                                const char* const context,
                                const std::size_t depth)
{
    if (depth > kMaxVehicleDescriptorCargoDepth ||
        object.GetPrototypeId() != node.prototype_id ||
        native_prototype_name(object) != node.prototype_name) {
        LOG_ERROR("%s cargo node mismatch objId=%d depth=%u expected=%d/%s actual=%d/%s",
                  context, object.GetId(), static_cast<unsigned>(depth),
                  node.prototype_id, node.prototype_name.c_str(),
                  object.GetPrototypeId(), native_prototype_name(object).c_str());
        return false;
    }
    if (!validate_native_descriptor_state(object, node.prefixes,
                                          node.suffixes, node.modifiers,
                                          context))
        return false;
    auto& children = object.GetChildren();
    std::size_t actual_children = 0;
    for (auto iterator = children.begin(); iterator != children.end();
         ++iterator) {
        if (iterator->second != nullptr)
            ++actual_children;
    }
    if (actual_children != node.cargo_objects.size()) {
        LOG_ERROR("%s nested cargo child count mismatch object=%d expected=%u actual=%u",
                  context, object.GetId(),
                  static_cast<unsigned>(node.cargo_objects.size()),
                  static_cast<unsigned>(actual_children));
        return false;
    }
    for (const VehicleDescriptorNode& child_node : node.cargo_objects) {
        hta::ai::Obj* const child = find_named_child(object, child_node.slot);
        if (child == nullptr || !validate_native_cargo_node(
                *child, child_node, context, depth + 1))
            return false;
    }
    return true;
}

bool validate_vehicle_cargo_objects(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    std::array<std::vector<ObjId>, 2> validated_roots;
    for (const VehicleDescriptorNode& node : descriptor.cargo_objects) {
        const unsigned repository_index = static_cast<unsigned>(
            node.cargo_placement.repository);
        if (repository_index >= validated_roots.size())
            return false;
        hta::ai::GeomRepository* const repository = cargo_repository_for(
            vehicle, node.cargo_placement.repository);
        if (repository == nullptr) {
            LOG_ERROR("%s cargo repository missing repo=%u place=%d,%d prototype=%d",
                      context,
                      static_cast<unsigned>(node.cargo_placement.repository),
                      node.cargo_placement.x, node.cargo_placement.y,
                      node.prototype_id);
            return false;
        }
        hta::ai::Obj* object = nullptr;
        for (std::size_t slot = 0; slot < repository->m_slots.size(); ++slot) {
            const hta::ai::GeomRepositoryItem item = repository->GetItem(
                static_cast<std::int32_t>(slot));
            hta::ai::Obj* const candidate = item.IsValid() &&
                    !item.bIsResourceItem() ? item.GetObj() : nullptr;
            if (candidate == nullptr || candidate->bHasParent() ||
                candidate->GetParentRepository() != repository ||
                candidate->GetName() == nullptr ||
                node.slot != candidate->GetName())
                continue;
            if (object != nullptr && object != candidate) {
                LOG_ERROR("%s duplicate cargo identity repo=%u slotName=%s",
                          context, repository_index, node.slot.c_str());
                return false;
            }
            object = candidate;
        }
        if (object == nullptr) {
            LOG_ERROR("%s cargo object missing repo=%u preferredPlace=%d,%d prototype=%d slotName=%s",
                      context,
                      static_cast<unsigned>(node.cargo_placement.repository),
                      node.cargo_placement.x, node.cargo_placement.y,
                      node.prototype_id, node.slot.c_str());
            return false;
        }
        auto& roots = validated_roots[repository_index];
        if (std::find(roots.begin(), roots.end(), object->GetId()) != roots.end()) {
            LOG_ERROR("%s cargo object matched more than once repo=%u objId=%d",
                      context, repository_index, object->GetId());
            return false;
        }
        roots.push_back(object->GetId());
        if (!validate_native_cargo_node(*object, node, context, 0))
            return false;
    }
    for (unsigned repository_index = 0; repository_index != 2;
         ++repository_index) {
        const auto repository_kind = repository_index == 0
            ? VehicleCargoRepository::Main : VehicleCargoRepository::Ground;
        hta::ai::GeomRepository* const repository = cargo_repository_for(
            vehicle, repository_kind);
        if (repository == nullptr)
            return false;
        std::vector<ObjId> actual_roots;
        for (std::size_t slot = 0; slot < repository->m_slots.size(); ++slot) {
            const hta::ai::GeomRepositoryItem item = repository->GetItem(
                static_cast<std::int32_t>(slot));
            hta::ai::Obj* const object = item.IsValid() &&
                    !item.bIsResourceItem() ? item.GetObj() : nullptr;
            if (object == nullptr || object->bHasParent())
                continue;
            if (object->GetParentRepository() != repository) {
                LOG_ERROR("%s cargo root repository mismatch repo=%u objId=%d",
                          context, repository_index, object->GetId());
                return false;
            }
            if (std::find(actual_roots.begin(), actual_roots.end(),
                          object->GetId()) == actual_roots.end())
                actual_roots.push_back(object->GetId());
        }
        if (actual_roots.size() != validated_roots[repository_index].size()) {
            LOG_ERROR("%s cargo root count mismatch repo=%u expected=%u actual=%u",
                      context, repository_index,
                      static_cast<unsigned>(validated_roots[repository_index].size()),
                      static_cast<unsigned>(actual_roots.size()));
            return false;
        }
    }
    return true;
}

bool validate_vehicle_descriptor_structure(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    if (vehicle.GetPrototypeId() != descriptor.prototype_id ||
        native_prototype_name(vehicle) != descriptor.prototype_name) {
        LOG_ERROR("%s descriptor root mismatch objId=%d expected=%d/%s actual=%d/%s",
                  context, vehicle.GetId(), descriptor.prototype_id,
                  descriptor.prototype_name.c_str(), vehicle.GetPrototypeId(),
                  native_prototype_name(vehicle).c_str());
        return false;
    }
    if (!validate_native_descriptor_state(vehicle, descriptor.prefixes,
                                          descriptor.suffixes,
                                          descriptor.modifiers, context))
        return false;
    for (const VehicleDescriptorNode& node : descriptor.attachments) {
        hta::ai::VehiclePart* const part = resolve_descriptor_part(
            vehicle, descriptor.attachments, node);
        if (part == nullptr || part->GetPrototypeId() != node.prototype_id ||
            native_prototype_name(*part) != node.prototype_name) {
            LOG_ERROR("%s descriptor part mismatch objId=%d slot=%s expected=%d/%s actual=%d/%s",
                      context, vehicle.GetId(), node.slot.c_str(),
                      node.prototype_id, node.prototype_name.c_str(),
                      part != nullptr ? part->GetPrototypeId() : -1,
                      part != nullptr ? native_prototype_name(*part).c_str()
                                      : "<missing>");
            return false;
        }
        if (!validate_native_descriptor_state(*part, node.prefixes,
                                              node.suffixes, node.modifiers,
                                              context))
            return false;
        if (node.gun.present) {
            if (!part->IsKindOf(hta::ai::Gun::p_classObject)) {
                LOG_ERROR("%s descriptor gun class mismatch objId=%d slot=%s",
                          context, vehicle.GetId(), node.slot.c_str());
                return false;
            }
            const auto& gun = static_cast<const hta::ai::Gun&>(*part);
            if (gun.m_curBarrelIndex != node.gun.barrel_index ||
                gun.GetShellsInCurrentCharge() != node.gun.current_charge ||
                gun.GetShellsInPool() != node.gun.pool ||
                gun.m_bIsFiring != node.gun.firing) {
                LOG_ERROR("%s descriptor gun state mismatch objId=%d slot=%s barrel=%d/%d charge=%d/%d pool=%d/%d firing=%u/%u",
                          context, vehicle.GetId(), node.slot.c_str(),
                          gun.m_curBarrelIndex, node.gun.barrel_index,
                          gun.GetShellsInCurrentCharge(), node.gun.current_charge,
                          gun.GetShellsInPool(), node.gun.pool,
                          gun.m_bIsFiring ? 1u : 0u,
                          node.gun.firing ? 1u : 0u);
                return false;
            }
        }
    }
    if (!validate_vehicle_repository_resources(vehicle, descriptor, context) ||
        !validate_vehicle_cargo_objects(vehicle, descriptor, context))
        return false;
    return true;
}

bool finalize_native_vehicle_graph(hta::ai::ObjContainer& objects,
                                   hta::ai::Vehicle& vehicle,
                                   const char* const context)
{
    constexpr std::size_t kMaxGraphObjects =
        kMaxVehicleDescriptorNodes + kMaxVehicleDescriptorCargoObjects;
    std::vector<hta::ai::Obj*> pending{&vehicle};
    const std::array<hta::ai::GeomRepository*, 2> repositories{{
        vehicle.GetRepository(), vehicle.GetGroundRepository()}};
    for (hta::ai::GeomRepository* const repository : repositories) {
        if (repository == nullptr)
            continue;
        for (std::size_t index = 0; index < repository->m_slots.size();
             ++index) {
            const hta::ai::GeomRepositoryItem item = repository->GetItem(
                static_cast<std::int32_t>(index));
            if (item.IsValid() && !item.bIsResourceItem() &&
                item.GetObj() != nullptr)
                pending.push_back(item.GetObj());
        }
    }
    std::vector<hta::ai::Obj*> visited;
    visited.reserve(kMaxGraphObjects);
    while (!pending.empty()) {
        hta::ai::Obj* const object = pending.back();
        pending.pop_back();
        if (object == nullptr ||
            std::find(visited.begin(), visited.end(), object) != visited.end())
            continue;
        if (visited.size() >= kMaxGraphObjects ||
            objects.GetEntityByObjId(object->GetId()) != object) {
            LOG_ERROR("%s native graph contains an external or unbounded descendant",
                      context);
            return false;
        }
        visited.push_back(object);
        if (object->m_bNeedPostLoad)
            object->PostLoad();
        auto& children = object->GetChildren();
        for (auto iterator = children.begin(); iterator != children.end();
             ++iterator)
            if (iterator->second != nullptr)
                pending.push_back(iterator->second);
    }
    for (hta::ai::Obj* const object : visited) {
        if (object->m_bNeedPostLoad) {
            LOG_ERROR("%s native graph retained m_bNeedPostLoad objId=%d",
                      context, object->GetId());
            return false;
        }
        if (object->m_bMustCreateVisualPart)
            object->CreateVisualPart();
        if (object->m_bMustCreateVisualPart) {
            LOG_ERROR("%s native graph retained pending visual objId=%d",
                      context, object->GetId());
            return false;
        }
    }
    return true;
}

bool apply_vehicle_descriptor_to_inactive_vehicle(
    hta::ai::Vehicle& vehicle, const VehicleDescriptor& descriptor,
    const char* const context)
{
    if (descriptor.prototype_id < 0 ||
        descriptor.prototype_id != vehicle.GetPrototypeId() ||
        descriptor.native_structure.empty()) {
        LOG_ERROR("%s native vehicle archive context unavailable objId=%d "
                  "descriptorPrototype=%d objectPrototype=%d archiveBytes=%u",
                  context, vehicle.GetId(), descriptor.prototype_id,
                  vehicle.GetPrototypeId(),
                  static_cast<unsigned>(descriptor.native_structure.size()));
        return false;
    }

    // Validate all untrusted bounds, graph links, resource names and the
    // native-archive envelope before the first native mutation.  The encoded
    // bytes are intentionally discarded: restore uses the original archive,
    // while this pass is the mutation-safety gate.
    std::vector<Byte> descriptor_validation_wire;
    const VehicleDescriptorCodecError descriptor_codec_error =
        encode_vehicle_descriptor(descriptor, descriptor_validation_wire);
    if (!vehicle_descriptor_codec_succeeded(descriptor_codec_error)) {
        LOG_ERROR("%s native vehicle descriptor rejected before graph normalization objId=%d codec=%u",
                  context, vehicle.GetId(),
                  static_cast<unsigned>(descriptor_codec_error));
        return false;
    }
    if (!normalize_native_vehicle_attachment_roots(vehicle, descriptor,
                                                   context)) {
        LOG_ERROR("%s native destination root graph could not be normalized objId=%d",
                  context, vehicle.GetId());
        return false;
    }
    std::vector<VehicleArchiveNameBinding> destination_bindings;
    if (!collect_native_part_bindings(vehicle, destination_bindings)) {
        LOG_ERROR("%s native destination graph could not be enumerated objId=%d",
                  context, vehicle.GetId());
        return false;
    }
    const VehicleArchivePreflightResult preflight = preflight_vehicle_archive(
        descriptor, destination_bindings);
    if (!preflight) {
        LOG_ERROR("%s native archive preflight rejected objId=%d error=%u codec=%u index=%u",
                  context, vehicle.GetId(),
                  static_cast<unsigned>(preflight.error),
                  static_cast<unsigned>(preflight.codec_error),
                  static_cast<unsigned>(preflight.record_index));
        return false;
    }
    // The native XML may contain local runtime references that have no safe
    // remap context in this ABI.  They are not interpreted or rewritten here;
    // the post-load structural barrier below is the fail-closed boundary, and
    // its caller retires the object before registration if any reference
    // leaves the graph inconsistent.
    const NativeObjectArchiveResult restored = restore_native_object_archive(
        ByteView(descriptor.native_structure.data(),
                 descriptor.native_structure.size()), vehicle);
    if (!restored) {
        LOG_ERROR("%s native vehicle archive restore failed objId=%d "
                  "error=%s detail=%s",
                  context, vehicle.GetId(),
                  native_object_archive_error_name(restored.error),
                  restored.detail.c_str());
        return false;
    }
    if (!reconcile_vehicle_descriptor_state(vehicle, descriptor, context)) {
        LOG_ERROR("%s native vehicle typed state reconciliation failed objId=%d",
                  context, vehicle.GetId());
        return false;
    }
    if (!clear_suspended_vehicle_repositories(vehicle, context)) {
        LOG_ERROR("%s native vehicle repository normalization failed objId=%d",
                  context, vehicle.GetId());
        return false;
    }
    // Repository branches are deliberately omitted from the canonical XML:
    // they contain local ObjIds and gameplay ownership.  The fresh native
    // prototype can nevertheless carry stock cargo, so normalize both
    // repositories to empty first and reconstruct their typed, stable-ID
    // representation exactly once while the whole graph is still suspended.
    // The shared PostLoad barrier then seals the complete graph.
    return restore_vehicle_repository_resources(vehicle, descriptor, context) &&
        restore_vehicle_repository_objects(vehicle, descriptor, context);
}

hta::ai::Vehicle* ensure_remote_vehicle_replica(RemoteEntity& remote,
                                                const VehicleSnapshot& snapshot)
{
    if (!remote.has_descriptor)
        return nullptr;
    if (remote.descriptor.prototype_id < 0 ||
        (remote.prototype_id >= 0 &&
         remote.prototype_id != remote.descriptor.prototype_id)) {
        remote.spawn_attempt.reject_permanently();
        LOG_ERROR("remote vehicle descriptor/spawn prototype mismatch entity=%u "
                  "descriptor=%d spawn=%d",
                  remote.entity_id, remote.descriptor.prototype_id,
                  remote.prototype_id);
        return nullptr;
    }
    const std::int32_t descriptor_prototype_id = remote.descriptor.prototype_id;
    remote.prototype_id = descriptor_prototype_id;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_pObjects == nullptr)
        return nullptr;

    // A network descriptor is replayed state. Keep every native mutation in
    // the scoped replay boundary so the post-update observer cannot journal a
    // client presentation object as a host-authoritative mutation.
    ScopedReplaySuppression replay(g_state.world_replay_depth);
    const world_authority::ScopedWorldExecutionContext execution_scope(
        current_world_execution_context(true, false));

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
        remote.loadout.base_prototype_id != descriptor_prototype_id) {
        remote.spawn_attempt.reject_permanently();
        LOG_ERROR("remote vehicle base prototype mismatch entity=%u generation=%u spawnPrototype=%d loadoutPrototype=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  descriptor_prototype_id, remote.loadout.base_prototype_id);
        return nullptr;
    }

    char object_name[64]{};
    std::snprintf(object_name, sizeof(object_name), "kraken_remote_vehicle_%u_%u",
                  remote.entity_id, static_cast<unsigned>(remote.generation));
    hta::ai::Player* const local_player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const local_vehicle =
        local_player != nullptr ? local_player->GetVehicle() : nullptr;
    // EntitySpawn carries the host's assigned MatchSpawn faction.  Zero is a
    // valid native belong, so only the absence of an authoritative spawn may
    // use the legacy relationship fallback.
    const bool has_authoritative_spawn_belong = remote.has_spawn;
    const std::int32_t replica_belong = has_authoritative_spawn_belong
        ? remote.belong
        : (local_vehicle != nullptr
            ? multiplayer_remote_belong(local_vehicle->GetBelong(), remote.entity_id)
            : remote.belong);
    if (local_vehicle != nullptr)
        configure_free_for_all_relationship(*server->m_pObjects,
                                            local_vehicle->GetBelong(),
                                            replica_belong);
    // Keep the graph neutral while native archive/resource restoration runs;
    // the authoritative belong is propagated to root and descendants only
    // after the canonical archive has been accepted.
    const ObjId object_id = server->m_pObjects->CreateNewObjectWithSuspendedPostLoad(
        descriptor_prototype_id, object_name, -1, -1);
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
                  remote.entity_id, descriptor_prototype_id, object_id);
        return nullptr;
    }

    // Descriptor v3 is the canonical pre-PostLoad graph. It carries native
    // prototype identity, attachment hierarchy, affixes/modifiers, guns and
    // repository stacks; the old loadout profile is retained only for old
    // peers whose descriptor has no rich graph.
    if (!apply_vehicle_descriptor_to_inactive_vehicle(
            *vehicle, remote.descriptor, "remote presentation")) {
        remote.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("remote vehicle descriptor v3 materialization failed entity=%u "
                  "generation=%u objId=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  object_id);
        g_state.client_join_failure_pending = true;
        return nullptr;
    }
    vehicle->SetPostPosition(to_engine_vector(snapshot.position));
    vehicle->SetPostRotation(to_engine_quaternion(snapshot.rotation));
    // Zero is a valid authoritative faction.  Always propagate the resolved
    // value through the native attachment tree, including parts and guns.
    apply_belong_to_vehicle_tree(*vehicle, replica_belong);

    if (!complete_suspended_vehicle_loadout(*server->m_pObjects, object_id,
                                            vehicle, "remote presentation")) {
        remote.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("remote vehicle post-load failed entity=%u generation=%u objId=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  object_id);
        return nullptr;
    }

    if (!validate_vehicle_descriptor_structure(
            *vehicle, remote.descriptor, "remote presentation")) {
        remote.spawn_attempt.reject_permanently();
        server->m_pObjects->AddObjIdToRemove(object_id);
        LOG_ERROR("remote vehicle descriptor structural validation failed entity=%u generation=%u objId=%d",
                  remote.entity_id, static_cast<unsigned>(remote.generation),
                  object_id);
        return nullptr;
    }

    // Native visual/runtime and mass state must be rebuilt after PostLoad,
    // before the replica is retired from physics/update ownership.
    vehicle->RefreshMass();

    // This is a network presentation object, not a second simulation. Retire
    // all local authority before the next ObjContainer update; the host is
    // the sole owner of AI, ODE and combat for this entity.
    vehicle->SetUpdatingByODE(false);
    vehicle->DisablePhysics();
    if (remote.has_loadout && remote.descriptor.attachments.empty() &&
        remote.descriptor.prefixes.empty() &&
        remote.descriptor.suffixes.empty() &&
        remote.descriptor.modifiers.empty() &&
        remote.descriptor.cargo_stacks.empty() &&
        remote.descriptor.cargo_objects.empty() &&
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

    LOG_INFO("KRAKEN_MP_ACCEPT replica_materialized entity=%u generation=%u kind=%u prototype=%d objId=%d loadoutRevision=%u",
             remote.entity_id, static_cast<unsigned>(remote.generation),
             static_cast<unsigned>(remote.kind), descriptor_prototype_id, object_id,
             remote.applied_loadout_revision);
    return vehicle;
}

hta::ai::Vehicle* ensure_remote_vehicle(RemoteEntity& remote,
                                        const VehicleSnapshot&)
{
    if (!remote.has_spawn || remote.prototype_id < 0 || !remote.has_descriptor) {
        // Snapshots are unreliable and may beat the reliable EntitySpawn.
        // Buffer them; creation is retried on the next frame after both the
        // reliable spawn and rich descriptor arrive instead of guessing with
        // the local player's prototype.
        return nullptr;
    }
    if (remote.kind == EntityKind::PlayerVehicle ||
        remote.kind == EntityKind::NpcVehicle) {
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
        const bool supported_vehicle =
            remote.kind == EntityKind::PlayerVehicle ||
            remote.kind == EntityKind::NpcVehicle;
        if (!remote.has_spawn || remote.retired ||
            !supported_vehicle ||
            !remote.has_spawn_snapshot)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_remote_vehicle_replica(
            remote, remote.spawn_snapshot);
        if (vehicle != nullptr && remote.has_loadout &&
            remote.applied_loadout_revision != remote.loadout.revision)
            apply_remote_loadout(remote, *vehicle);
        if (g_state.client_join_failure_pending)
            break;
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
    // Rich descriptors are authoritative native XML/archive state.  The
    // legacy SetNewPart path is intentionally not a post-PostLoad fallback
    // for them; it would duplicate or corrupt archive attachments.
    if (remote.has_descriptor && !remote.descriptor.native_structure.empty()) {
        remote.applied_loadout_revision = remote.loadout.revision;
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

void apply_remote_snapshots(const float elapsed_time)
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
        if (!remote.has_weapon_aim)
            continue;
        WeaponAimInterpolationInput interpolation{};
        if (!g_state.weapon_aim_tracker.interpolation_input(
                g_state.session_epoch, remote.weapon_aim.shooter,
                remote.weapon_aim.gun, interpolation))
            continue;
        const float alpha = (std::clamp)(elapsed_time * 20.0f, 0.0f, 1.0f);
        const VehicleVector3 aim_point{
            interpolation.previous.aim_point.x +
                (interpolation.current.aim_point.x -
                 interpolation.previous.aim_point.x) * alpha,
            interpolation.previous.aim_point.y +
                (interpolation.current.aim_point.y -
                 interpolation.previous.aim_point.y) * alpha,
            interpolation.previous.aim_point.z +
                (interpolation.current.aim_point.z -
                 interpolation.previous.aim_point.z) * alpha};
        apply_visual_weapon_aim(*ghost, remote.weapon_aim.gun, aim_point,
                                interpolation.current.aim_speed);
    }
}

void apply_local_weapon_presentation()
{
    // The local replica keeps its native camera/aim only. Confirmed ammo and
    // reload presentation arrive through ShotConfirmed and are applied after
    // exact attachment resolution; no trigger is reconstructed locally.
    apply_pending_confirmed_shots();
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
    Session* const active_session = g_state.session.get();
    if (active_session == nullptr)
        return;

    g_state.lan_discovery.pump();

    const TransportResult result = active_session->pump();
    if (!result && result.code != TransportResultCode::WouldBlock)
        LOG_ERROR("network pump failed code=%u", static_cast<unsigned>(result.code));

    std::array<SessionEvent, 64> events{};
    for (;;) {
        if (g_state.session.get() != active_session)
            return;
        const std::size_t count = active_session->drain_events(events);
        if (count == 0)
            break;
        for (std::size_t index = 0; index < count; ++index) {
            handle_event(std::move(events[index]));
            // A handler may deliberately leave/end the session (for example
            // after a fail-closed descriptor rejection). Do not process the
            // remainder of the detached batch or touch the destroyed Session.
            if (g_state.session.get() != active_session)
                return;
        }
    }

    if (g_state.host_defeat_session_end_pending) {
        g_state.host_defeat_session_end_pending = false;
        LOG_INFO("ending multiplayer session after authoritative host defeat or host disconnect");
        (void)LeaveSession(g_state.is_host ? SessionLeaveReason::Death
                                           : SessionLeaveReason::HostTerminated);
        return;
    }
    const Clock::time_point now = Clock::now();
    tick_match(now);
    emit_match_accept_marker();
    update_lan_advertisement();
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
            if (!host_peer_world_transfer_permitted(peer))
                continue;
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
        if (!host_peer_world_transfer_permitted(peer))
            continue;
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
            if (!host_peer_world_transfer_permitted(peer))
                continue;
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

void apply_local_assigned_spawn()
{
    if (g_state.is_host || g_state.local_assigned_spawn_applied ||
        !g_state.local_assigned_spawn)
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player != nullptr
        ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return;
    const MatchSpawn& spawn = *g_state.local_assigned_spawn;
    vehicle->SetPositionSelf({spawn.x, spawn.y, spawn.z});
    vehicle->SetRotationSelf(hta::Quaternion(hta::CVector(
        0.0f, spawn.yaw, 0.0f)));
    apply_belong_to_vehicle_tree(*vehicle, spawn.belong);
    vehicle->SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    vehicle->SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    g_state.local_assigned_spawn_applied = true;
    LOG_INFO("client applied assigned spawn entity=%u belong=%d pos=%.3f %.3f %.3f",
             g_state.local_entity_id, spawn.belong, spawn.x, spawn.y, spawn.z);
}

void run_raid_autotest_tick()
{
    if (!g_state.raid_autotest_enabled ||
        !g_state.raid_autotest_bootstrap_attempted ||
        !g_state.raid_autotest_native_save_loaded ||
        g_state.raid_autotest_matchmaking_attempted)
        return;
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_raid_autotest)
        return;
    g_state.next_raid_autotest = now + std::chrono::seconds(1);
    const std::string loaded_map = current_level_name();
    if (loaded_map.empty())
        return;
    const std::string scenario = environment("KRAKEN_MP_ACCEPT_SCENARIO")
        .value_or("forming");
    const std::string expected_role = environment("KRAKEN_MP_ACCEPT_ROLE")
        .value_or("auto");
    const std::string target_map = environment("KRAKEN_MP_MATCH_TARGET")
        .value_or(loaded_map);
    const std::string exit_map = environment("KRAKEN_MP_MATCH_EXIT")
        .value_or("");
    const std::uint8_t required_players = static_cast<std::uint8_t>(
        environment_uint("KRAKEN_MP_MATCH_REQUIRED",
                         scenario == "jip" ? 1u : 2u, 1u,
                         static_cast<std::uint32_t>(kMaxSessionPlayers)));
    if (!BeginSession())
        return;
    if ((expected_role == "host" && !g_state.is_host) ||
        (expected_role == "client" && g_state.is_host)) {
        g_state.raid_autotest_matchmaking_attempted = true;
        LOG_ERROR("KRAKEN_MP_ACCEPT matchmaking role=%s scenario=%s result=rejected reason=role_mismatch",
                  acceptance_role(), scenario.c_str());
        return;
    }
    g_state.raid_autotest_matchmaking_attempted = true;
    if (!g_state.is_host) {
        LOG_INFO("KRAKEN_MP_ACCEPT matchmaking role=client scenario=%s result=accepted mode=follow_host",
                 scenario.c_str());
        return;
    }
    const bool accepted = StartMatchmaking(
        required_players, target_map.c_str(), exit_map.c_str(), -1, false);
    LOG_INFO("KRAKEN_MP_ACCEPT matchmaking role=host scenario=%s result=%s required=%u target=%s exit=%s",
             scenario.c_str(), accepted ? "accepted" : "rejected",
             static_cast<unsigned>(required_players), target_map.c_str(),
             exit_map.empty() ? "main_menu" : exit_map.c_str());
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
    const bool locally_ready = g_state.raid_autotest_native_save_loaded &&
        g_state.diagnostic_gameplay_open_logged &&
        (g_state.is_host
            ? (g_state.diagnostic_host_control_ready_logged &&
               !g_state.world_ready_acks.empty())
            : (g_state.world_snapshot_committed &&
               g_state.quest_projection_committed &&
               g_state.diagnostic_first_input_logged));
    if (!locally_ready)
        return;

    const bool host_shooter_scenario =
        g_state.combat_autotest_scenario == "host-kills-client";
    const bool client_shooter_scenario =
        g_state.combat_autotest_scenario == "client-kills-host";
    if (!host_shooter_scenario && !client_shooter_scenario)
        return;

    NetId shooter_id = kInvalidNetId;
    NetId target_id = kInvalidNetId;
    if (host_shooter_scenario) {
        shooter_id = 1;
        target_id = g_state.is_host
            ? (g_state.controllers.empty() ? kInvalidNetId
                                           : g_state.controllers.front().entity_id)
            : g_state.local_entity_id;
    } else {
        shooter_id = g_state.is_host
            ? (g_state.controllers.empty() ? kInvalidNetId
                                           : g_state.controllers.front().entity_id)
            : g_state.local_entity_id;
        target_id = 1;
    }
    hta::ai::Vehicle* const shooter = find_vehicle(shooter_id);
    hta::ai::Vehicle* const target = find_vehicle(target_id);
    if (shooter == nullptr || target == nullptr || target->_GetDeadStatus() ||
        target->GetHealth() <= 0.0f || g_state.combat_autotest_weapon_part.empty())
        return;
    const hta::CStr weapon_part(g_state.combat_autotest_weapon_part.c_str());
    hta::ai::VehiclePart* const selected_part = shooter->GetPartByName(weapon_part);
    if (selected_part == nullptr ||
        !selected_part->IsKindOf(hta::ai::Gun::p_classObject)) {
        LOG_ERROR("KRAKEN_COMBAT_AUTOTEST selected weapon part is absent scenario=%s part=%s shooter=%u",
                  g_state.combat_autotest_scenario.c_str(),
                  g_state.combat_autotest_weapon_part.c_str(), shooter_id);
        return;
    }
    auto* const weapon = static_cast<hta::ai::Gun*>(selected_part);
    const bool local_is_shooter = host_shooter_scenario == g_state.is_host;
    GunAttachmentIdentity validated_weapon_identity{};
    EntityGeneration shooter_generation = kInvalidEntityGeneration;
    EntityGeneration target_generation = kInvalidEntityGeneration;
    if (!capture_weapon_identity(*shooter, weapon, validated_weapon_identity) ||
        (local_is_shooter && !weapon->CanFire()) || g_state.peers.empty() ||
        !g_state.entities.lookup_generation(shooter_id, shooter_generation) ||
        !g_state.entities.lookup_generation(target_id, target_generation))
        return;

    if (!g_state.combat_autotest_armed) {
        const PeerId peer = g_state.peers.empty() ? kInvalidPeer
                                                  : g_state.peers.front();
        g_state.combat_autotest_armed = true;
        LOG_INFO("KRAKEN_MP_ACCEPT combat_armed role=%s peer=%u epoch=%u",
                 acceptance_role(), peer, g_state.match_epoch);
    }
    // Both peers arm from their real local/replicated view. Only the scenario's
    // shooter enters the native firing path; the non-shooter proves that the
    // same entities and attached gun are materialized without synthesizing an
    // action.
    if (!local_is_shooter)
        return;

    const Clock::time_point now = Clock::now();
    // The gun and turret are held through the normal frame cadence.  Only the
    // client-to-host intent packet is rate limited; coupling both operations
    // to 50 ms made the target crosshair and muzzle lag visibly behind a real
    // player-controlled weapon.
    const bool weapon_intent_due = now >= g_state.next_combat_autotest;
    if (weapon_intent_due)
        g_state.next_combat_autotest = now + std::chrono::milliseconds(50);
    const bool log_aim = weapon_intent_due &&
                         ((g_state.combat_autotest_sequence++ % 30u) == 0u);
    if (!g_state.combat_autotest_started) {
        g_state.combat_autotest_started = true;
        LOG_INFO("KRAKEN_COMBAT_AUTOTEST start scenario=%s shooter=%u target=%u",
                 g_state.combat_autotest_scenario.c_str(), shooter_id,
                 target_id);
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
    if (client_shooter_scenario) {
        // A client has only a presentation replica for entity 1.  Firing a
        // local projectile at that replica cannot damage the authoritative
        // host.  Keep the real camera/turret aim locally, then submit the
        // same semantic target identity that normal multiplayer input sends;
        // the host resolves it to its live player Vehicle and executes the
        // The host resolves the typed shooter/target and executes Gun::Fire.
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
        g_state.local_weapon_gun = validated_weapon_identity;
        g_state.has_local_weapon_gun = true;
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
                &authoritative_aim, (std::clamp)(elapsed_time, 0.0f, 0.1f),
                &g_state.local_weapon_gun);
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
    command.session_epoch = g_state.session_epoch;
    command.entity_id = shooter_id;
    (void)g_state.entities.lookup_generation(shooter_id,
                                             command.entity_generation);
    command.sequence = g_state.combat_autotest_sequence;
    command.gun = validated_weapon_identity;
    command.gun_id = weapon->GetPrototypeId();
    command.trigger_held = true;
    command.target_entity_id = target_id;
    (void)g_state.entities.lookup_generation(target_id,
                                             command.target_generation);
    command.aim_point = {target_position.x, target_position.y, target_position.z};
    command.has_aim_point = true;
    command.aim_speed = (std::clamp)(elapsed_time, 0.0f, 0.1f);
    // `Gun::Fire` deliberately does not steer a turret.  Match the normal
    // WeaponFirer sequence: update the real part transforms first, then fire
    // only through the stock Gun primitive.
    apply_visual_weapon_aim(*shooter, command.gun, command.aim_point,
                            command.aim_speed);
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

void emit_combat_host_survival_heartbeat()
{
    if (!g_state.is_host || !g_state.combat_autotest_death_logged ||
        g_state.combat_autotest_scenario != "host-kills-client" ||
        !g_state.session || !g_state.session->running() ||
        g_state.match.state() != MatchState::Playing)
        return;
    hta::ai::Vehicle* const host = find_vehicle(1);
    if (host == nullptr || host->_GetDeadStatus() || host->GetHealth() <= 0.0f)
        return;
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_combat_host_survival)
        return;
    g_state.next_combat_host_survival = now + std::chrono::milliseconds(500);
    LOG_INFO("KRAKEN_MP_ACCEPT combat_host_surviving scenario=host-kills-client epoch=%u health=%.2f",
             g_state.match_epoch, host->GetHealth());
}

std::string current_level_name()
{
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    if (server == nullptr || server->m_level == nullptr)
        return {};
    const char* const name = server->m_level->GetLevelName();
    return name != nullptr ? std::string(name) : std::string{};
}

void maybe_send_jip_map_ready()
{
    if (g_state.is_host || !g_state.session ||
        !g_state.session->running() || !g_state.client_jip_map_load)
        return;
    ClientJipMapLoad& pending = *g_state.client_jip_map_load;
    if (pending.map_ready_sent || pending.host_peer == kInvalidPeer)
        return;
    const std::string loaded_map = current_level_name();
    if (loaded_map.empty() || loaded_map != pending.target_map)
        return;
    // The descriptor uploaded during Forming describes the vehicle in the
    // lobby/save map.  LoadMap replaces that native graph.  Upload the exact
    // post-load graph before acknowledging map readiness; both messages use
    // the same reliable channel, so the host cannot materialize a stale
    // lobby vehicle and then accept commands from different attachments.
    if (!pending.vehicle_descriptor_sent) {
        if (!send_local_vehicle_descriptor(pending.host_peer))
            return;
        pending.vehicle_descriptor_sent = true;
        LOG_INFO("post-load vehicle descriptor sent peer=%u epoch=%u entity=%u map=%s",
                 pending.host_peer, pending.session_epoch, pending.entity_id,
                 loaded_map.c_str());
    }
    const MatchMapReady acknowledgement{
        pending.session_epoch, pending.roster_revision,
        pending.player_id, pending.entity_id};
    std::vector<Byte> payload;
    if (!match_codec_succeeded(encode_match_map_ready(
            acknowledgement, payload)) ||
        !send_match_payload(pending.host_peer, MessageType::MatchMapReady,
                            payload))
        return;
    pending.map_ready_sent = true;
    LOG_INFO("match map-ready sent peer=%u epoch=%u roster=%u player=%u entity=%u map=%s",
             pending.host_peer, pending.session_epoch, pending.roster_revision,
             pending.player_id, pending.entity_id, loaded_map.c_str());
}

std::optional<LanSessionAdvertisement> current_lan_advertisement()
{
    if (!g_state.is_host || !g_state.session ||
        !is_valid_session_identity(g_state.session_identity) ||
        !g_lifecycle_config)
        return std::nullopt;

    const MatchState state = g_state.match.state();
    if (state != MatchState::Forming && state != MatchState::Playing)
        return std::nullopt;
    const MatchConfig& match = g_state.match.config();
    if (match.target_map.empty() ||
        (state == MatchState::Playing &&
         match.join_policy != JoinPolicy::JoinInProgress))
        return std::nullopt;

    const std::size_t player_count = g_state.match.players().size();
    if (match.max_players == 0 || player_count >= match.max_players)
        return std::nullopt;

    LanSessionAdvertisement advertisement{};
    advertisement.game_port = g_lifecycle_config->port;
    advertisement.state = state;
    advertisement.join_policy = match.join_policy;
    advertisement.current_map = current_level_name();
    advertisement.target_map = match.target_map;
    advertisement.current_players = static_cast<std::uint8_t>(player_count);
    advertisement.max_players = match.max_players;
    advertisement.identity = g_state.session_identity;
    return advertisement;
}

void update_lan_advertisement()
{
    const std::optional<LanSessionAdvertisement> advertisement =
        current_lan_advertisement();
    if (!advertisement) {
        if (g_state.lan_advertising) {
            g_state.lan_discovery.stop();
            g_state.lan_advertising = false;
            LOG_INFO("LAN matchmaking advertisement stopped state=%s",
                     to_string(g_state.match.state()));
        }
        return;
    }

    bool published = false;
    if (g_state.lan_discovery.hosting())
        published = g_state.lan_discovery.set_advertisement(*advertisement);
    else
        published = g_state.lan_discovery.become_host(
            kLanDiscoveryPort, *advertisement);
    if (!published) {
        LOG_ERROR("LAN matchmaking advertisement update failed state=%s map=%s",
                  to_string(advertisement->state),
                  advertisement->target_map.c_str());
        g_state.lan_advertising = false;
        return;
    }
    if (!g_state.lan_advertising)
        LOG_INFO("LAN matchmaking advertisement started state=%s map=%s players=%u/%u",
                 to_string(advertisement->state), advertisement->target_map.c_str(),
                 static_cast<unsigned>(advertisement->current_players),
                 static_cast<unsigned>(advertisement->max_players));
    g_state.lan_advertising = true;
}

void auto_host_coop_tick()
{
    if (!g_state.auto_host_coop)
        return;
    const std::string level_name = current_level_name();
    const bool gameplay = is_gameplay_level_name(level_name);
    if (!gameplay) {
        if (g_state.session && g_state.session->running()) {
            LOG_INFO("auto-host leaving non-gameplay level=%s",
                     level_name.empty() ? "<none>" : level_name.c_str());
            (void)EndSession();
        }
        g_state.auto_host_map.clear();
        return;
    }
    if (g_state.session && g_state.session->running() &&
        !g_state.auto_host_map.empty() && g_state.auto_host_map != level_name) {
        LOG_INFO("auto-host leaving changed gameplay map old=%s new=%s",
                 g_state.auto_host_map.c_str(), level_name.c_str());
        (void)EndSession();
    }
    if (!g_state.session || !g_state.session->running()) {
        if (!BeginSession())
            return;
    }
    g_state.auto_host_map = level_name;
    if (g_state.match.state() == MatchState::Offline) {
        if (!start_matchmaking_impl(1, level_name.c_str(), "", 0, false))
            LOG_ERROR("auto-host failed to start match map=%s", level_name.c_str());
        else
            LOG_INFO("auto-host started generic match map=%s", level_name.c_str());
    }
}

[[nodiscard]] AttachmentIdentity native_part_identity(
    const hta::ai::VehiclePart& part)
{
    const std::string path = native_part_path(part);
    if (path.empty() || part.GetPrototypeId() < 0)
        return {};
    AttachmentIdentity identity{};
    identity.path_hash = weapon_identity_hash("kraken/part-path/v1", path);
    identity.attachment_id = weapon_identity_hash(
        "kraken/part-attachment/v1",
        path + "#" + std::to_string(part.GetPrototypeId()));
    return identity;
}

hta::ai::VehiclePart* find_native_part_by_path(
    hta::ai::VehiclePart& part, const std::string_view path)
{
    if (native_part_path(part) == path)
        return &part;
    if (!part.IsKindOf(hta::ai::CompoundVehiclePart::p_classObject))
        return nullptr;
    const auto& compound =
        static_cast<const hta::ai::CompoundVehiclePart&>(part);
    for (auto iterator = compound.begin(); iterator != compound.end();
         ++iterator) {
        if (iterator->second.vp == nullptr)
            continue;
        if (hta::ai::VehiclePart* const found = find_native_part_by_path(
                *iterator->second.vp, path))
            return found;
    }
    return nullptr;
}

hta::ai::VehiclePart* find_native_part_by_path(
    hta::ai::Vehicle& vehicle, const std::string_view path)
{
    const auto names = vehicle.GetAttachedPartNames();
    hta::ai::VehiclePart* found = nullptr;
    for (std::size_t index = 0; index < names.size(); ++index) {
        hta::ai::VehiclePart* const root = vehicle.GetPartByName(names[index]);
        if (root == nullptr)
            continue;
        hta::ai::VehiclePart* const candidate =
            find_native_part_by_path(*root, path);
        if (candidate == nullptr)
            continue;
        if (found != nullptr)
            return nullptr;
        found = candidate;
    }
    return found;
}

hta::ai::VehiclePart* find_native_part_by_identity(
    hta::ai::Vehicle& vehicle, const AttachmentIdentity& identity)
{
    if (identity.attachment_id == 0 || identity.path_hash == 0)
        return nullptr;
    const auto names = vehicle.GetAttachedPartNames();
    hta::ai::VehiclePart* found = nullptr;
    for (std::size_t index = 0; index < names.size(); ++index) {
        hta::ai::VehiclePart* const root = vehicle.GetPartByName(names[index]);
        if (root == nullptr)
            continue;
        const std::string path = native_part_path(*root);
        if (path.empty() || weapon_identity_hash("kraken/part-path/v1", path) !=
                                identity.path_hash)
            continue;
        hta::ai::VehiclePart* const candidate = find_native_part_by_path(
            *root, path);
        if (candidate == nullptr ||
            native_part_identity(*candidate).attachment_id !=
                identity.attachment_id)
            continue;
        if (found != nullptr)
            return nullptr;
        found = candidate;
    }
    return found;
}

bool resolve_process_body_target(hta::ai::PhysicBody* const body,
                                 ImpactTargetIdentity& target,
                                 AttachmentIdentity& target_part)
{
    target = {ImpactTargetKind::Environment, {}, {},
              EnvironmentKind::UnboundStatic};
    target_part = {};
    if (body == nullptr)
        return false;

    // ProcessShellAndBody's nominal PhysicBody* is a heterogeneous collider
    // ABI.  Only the common m3d::Object RTTI probe is legal before the exact
    // collider type is known; no PhysicBody/PhysicObj member may be invoked on
    // an unproven sibling such as a direct Vehicle, Wheel, or GeomObject.
    hta::m3d::Object* const collider =
        reinterpret_cast<hta::m3d::Object*>(body);
    const auto collider_is_kind =
        [collider](const hta::m3d::Class* const class_object) {
            return collider->IsKindOf(class_object);
        };
    const auto resolve_owner_vehicle = [](hta::ai::PhysicObj* const owner) {
        if (owner == nullptr)
            return static_cast<hta::ai::Vehicle*>(nullptr);
        hta::m3d::Object* const owner_object =
            reinterpret_cast<hta::m3d::Object*>(owner);
        if (owner_object->IsKindOf(hta::ai::Wheel::p_classObject))
            return reinterpret_cast<hta::ai::Wheel*>(owner)->GetVehicle();
        if (owner_object->IsKindOf(hta::ai::Vehicle::p_classObject))
            return reinterpret_cast<hta::ai::Vehicle*>(owner);
        return static_cast<hta::ai::Vehicle*>(nullptr);
    };

    hta::ai::Vehicle* vehicle = nullptr;
    hta::ai::VehiclePart* part = nullptr;
    using GetPhysicBodyBaseClass = hta::m3d::Class*(__fastcall *)();
    const hta::m3d::Class* const physic_body_class =
        reinterpret_cast<GetPhysicBodyBaseClass>(
            combat_runtime::kPhysicBodyGetBaseClassVa)();
    if (collider_is_kind(hta::ai::Wheel::p_classObject)) {
        vehicle = reinterpret_cast<hta::ai::Wheel*>(body)->GetVehicle();
    }
    else if (collider_is_kind(hta::ai::Vehicle::p_classObject)) {
        vehicle = reinterpret_cast<hta::ai::Vehicle*>(body);
    }
    else if (collider_is_kind(hta::ai::VehiclePart::p_classObject)) {
        part = reinterpret_cast<hta::ai::VehiclePart*>(body);
        std::array<const hta::ai::VehiclePart*, 256> visited{};
        std::size_t visited_count = 0;
        const hta::ai::VehiclePart* current = part;
        for (std::size_t depth = 0;
             current != nullptr && depth != visited.size(); ++depth) {
            for (std::size_t index = 0; index != visited_count; ++index) {
                if (visited[index] == current)
                    return false;
            }
            visited[visited_count++] = current;
            // This is the verified const VehiclePart method at 0x6CDC80
            // (the mutable overload is 0x6CDC70); do not walk a generic
            // PhysicObj owner as though it were a VehiclePart.
            using GetOwnerCompound = const hta::ai::CompoundVehiclePart* (
                __thiscall *)(const hta::ai::VehiclePart*);
            const hta::ai::CompoundVehiclePart* const compound_owner =
                reinterpret_cast<GetOwnerCompound>(
                    combat_runtime::kVehiclePartGetOwnerCompoundConstVa)(
                    current);
            if (compound_owner != nullptr) {
                current = compound_owner;
                continue;
            }
            // GetOwner is called only on a VehiclePart proven by the RTTI
            // branch above.  The owner is still RTTI-probed as an Object;
            // it is never reinterpreted as another VehiclePart.
            vehicle = resolve_owner_vehicle(current->GetOwner());
            break;
        }
    }
    else if (physic_body_class != nullptr &&
             collider_is_kind(physic_body_class)) {
        // Generic proven PhysicBody is the only case where the nominal
        // PhysicBody::GetOwner ABI is used directly.
        vehicle = resolve_owner_vehicle(body->GetOwner());
    }
    if (vehicle == nullptr || vehicle->GetChassis() == nullptr)
        return false;

    NetId entity_id = kInvalidNetId;
    EntityGeneration generation = kInvalidEntityGeneration;
    if (!g_state.entities.lookup_net_id(vehicle->GetId(), entity_id,
                                        generation))
        return false;

    EntityKind kind = EntityKind::WorldObject;
    if (is_player_controlled_vehicle(*vehicle)) {
        kind = EntityKind::PlayerVehicle;
    }
    else if (const HostEntity* const host =
                 find_host_entity_by_object(vehicle->GetId());
             host != nullptr && host->kind == EntityKind::NpcVehicle) {
        kind = EntityKind::NpcVehicle;
    }
    else {
        return false;
    }

    if (part != nullptr) {
        const AttachmentIdentity identity = native_part_identity(*part);
        if (identity.attachment_id == 0 || identity.path_hash == 0 ||
            find_native_part_by_identity(*vehicle, identity) != part)
            return false;
        target_part = identity;
    }
    target = {ImpactTargetKind::DynamicEntity,
              {entity_id, generation}, {}, EnvironmentKind::UnboundStatic};
    (void)kind; // EntityKind is enforced by the player/NPC branches above.
    return true;
}

void capture_process_contact(dContact* const contacts,
                             std::uint32_t* const contact_count,
                             combat_runtime::HostImpactObservation& observation)
{
    if (contacts == nullptr || contact_count == nullptr || *contact_count == 0 ||
        *contact_count > 1024u)
        return;
    const dContactGeom& geom = contacts[0].geom;
    observation.geometry.hit_position = {geom.pos[0], geom.pos[1], geom.pos[2]};
    observation.geometry.contact_normal = {
        geom.normal[0], geom.normal[1], geom.normal[2]};
    observation.geometry.effect_position = observation.geometry.hit_position;
    observation.contact_captured =
        std::isfinite(geom.pos[0]) && std::isfinite(geom.pos[1]) &&
        std::isfinite(geom.pos[2]) && std::isfinite(geom.normal[0]) &&
        std::isfinite(geom.normal[1]) && std::isfinite(geom.normal[2]);
}

void copy_native_name(const hta::CStr& native_name, std::string& output)
{
    output.clear();
    const char* const value = native_name.c_str();
    const int length = native_name.length();
    if (value == nullptr || length <= 0 ||
        static_cast<std::size_t>(length) > kMaxCombatPresentationStringBytes)
        return;
    output.assign(value, static_cast<std::size_t>(length));
}

hta::m3d::SgNode* __fastcall capture_effect_node_call_impl(
    const hta::CStr& name, const hta::CVector& position,
    const hta::Quaternion& rotation, const bool remove_if_free,
    const float scale, const bool landscape_branch)
{
    const auto original = reinterpret_cast<decltype(
        &hta::ai::PhysicBody::CreateEffectNode)>(
        combat_runtime::kCreateEffectNodeVa);
    if (auto* const capture = combat_runtime::current_host_impact_capture()) {
        std::string selected;
        copy_native_name(name, selected);
        combat_runtime::record_effect_branch(selected);
        combat_runtime::record_environment_branch(
            EnvironmentKind::UnboundStatic);
        if (landscape_branch)
            combat_runtime::record_environment_branch(
                EnvironmentKind::Terrain);
        combat_runtime::record_effect_geometry(
            {position.x, position.y, position.z},
            {rotation.x, rotation.y, rotation.z, rotation.w},
            remove_if_free, scale);
    }
    hta::m3d::SgNode* const node = original(name, position, rotation,
                                             remove_if_free, scale);
    if (node != nullptr)
        combat_runtime::record_effect_produced();
    return node;
}

hta::m3d::SgNode* __fastcall capture_effect_node_call(
    const hta::CStr& name, const hta::CVector& position,
    const hta::Quaternion& rotation, const bool remove_if_free,
    const float scale)
{
    return capture_effect_node_call_impl(name, position, rotation,
                                         remove_if_free, scale, false);
}

hta::m3d::SgNode* __fastcall capture_landscape_effect_node_call(
    const hta::CStr& name, const hta::CVector& position,
    const hta::Quaternion& rotation, const bool remove_if_free,
    const float scale)
{
    return capture_effect_node_call_impl(name, position, rotation,
                                         remove_if_free, scale, true);
}

using ShellEffectNameFn = const hta::CStr&(__thiscall *)(
    const hta::ai::DynamicScene*, std::uint16_t, std::uint16_t);
using ShellSurfaceEffectNameFn = const hta::CStr&(__thiscall *)(
    const hta::ai::DynamicScene*, std::uint16_t);

const hta::CStr& __fastcall capture_terrain_selector(
    const hta::ai::DynamicScene* scene, void*, const std::uint16_t shell_type,
    const std::uint16_t soil_type)
{
    const auto original = reinterpret_cast<ShellEffectNameFn>(
        combat_runtime::kShellEffectNameVa);
    const hta::CStr& result = original(scene, shell_type, soil_type);
    combat_runtime::record_environment_branch(EnvironmentKind::Terrain);
    return result;
}

const hta::CStr& __fastcall capture_road_selector(
    const hta::ai::DynamicScene* scene, void*, const std::uint16_t shell_type)
{
    const auto original = reinterpret_cast<ShellSurfaceEffectNameFn>(
        combat_runtime::kShellRoadEffectNameVa);
    const hta::CStr& result = original(scene, shell_type);
    combat_runtime::record_environment_branch(EnvironmentKind::Road);
    return result;
}

const hta::CStr& __fastcall capture_statics_selector(
    const hta::ai::DynamicScene* scene, void*, const std::uint16_t shell_type)
{
    const auto original = reinterpret_cast<ShellSurfaceEffectNameFn>(
        combat_runtime::kShellStaticsEffectNameVa);
    const hta::CStr& result = original(scene, shell_type);
    combat_runtime::record_environment_branch(EnvironmentKind::Statics);
    return result;
}

const hta::CStr& __fastcall capture_vehicle_selector(
    const hta::ai::DynamicScene* scene, void*, const std::uint16_t shell_type)
{
    const auto original = reinterpret_cast<ShellSurfaceEffectNameFn>(
        combat_runtime::kShellVehicleEffectNameVa);
    return original(scene, shell_type);
}

void __fastcall capture_decal_call(
    hta::ai::VehiclePart* const part, void*, const hta::CVector& position,
    const hta::CVector& normal, const hta::CVector& tangent,
    const std::uint32_t mesh_id, const std::int32_t decal_id)
{
    const auto original = reinterpret_cast<void(__fastcall *)(
        hta::ai::VehiclePart*, void*, const hta::CVector&,
        const hta::CVector&, const hta::CVector&, std::uint32_t,
        std::int32_t)>(combat_runtime::kVehiclePartAddDecalVa);
    original(part, nullptr, position, normal, tangent, mesh_id, decal_id);
    if (auto* const capture = combat_runtime::current_host_impact_capture()) {
        hta::ai::CServer* const server = hta::ai::CServer::Instance();
        hta::ai::DynamicScene* const scene =
            server != nullptr ? server->m_pDynamicScene : nullptr;
        if (scene == nullptr || decal_id < 0)
            return;
        std::string selected;
        copy_native_name(scene->GetDecalName(decal_id), selected);
        combat_runtime::record_decal_branch(selected);
        if (!selected.empty()) {
            combat_runtime::record_decal_geometry(
                {position.x, position.y, position.z},
                {normal.x, normal.y, normal.z},
                {tangent.x, tangent.y, tangent.z}, mesh_id,
                part != nullptr ? native_part_identity(*part)
                                 : AttachmentIdentity{});
            combat_runtime::record_decal_produced();
        }
    }
}

bool install_process_shell_and_body_detour(
    const std::uintptr_t address,
    const combat_runtime::ProcessShellAndBodyFunction replacement,
    combat_runtime::ProcessShellAndBodyFunction& original,
    void*& trampoline)
{
    constexpr std::array<std::uint8_t, 10> kExpectedPrologue{
        0x8B, 0x44, 0x24, 0x08, 0x81, 0xEC, 0x2C, 0x01, 0x00, 0x00};
    if (replacement == nullptr || trampoline != nullptr ||
        std::memcmp(reinterpret_cast<const void*>(address),
                    kExpectedPrologue.data(), kExpectedPrologue.size()) != 0)
        return false;

    constexpr std::size_t kCopiedBytes = kExpectedPrologue.size();
    constexpr std::size_t kJumpBytes = 5;
    auto* const memory = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, kCopiedBytes + kJumpBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (memory == nullptr)
        return false;
    std::memcpy(memory, reinterpret_cast<const void*>(address), kCopiedBytes);
    memory[kCopiedBytes] = 0xE9;
    const std::intptr_t relative =
        static_cast<std::intptr_t>(address + kCopiedBytes) -
        static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(memory) +
                                   kCopiedBytes + kJumpBytes);
    if (relative < (std::numeric_limits<std::int32_t>::min)() ||
        relative > (std::numeric_limits<std::int32_t>::max)()) {
        (void)VirtualFree(memory, 0, MEM_RELEASE);
        return false;
    }
    const auto relative32 = static_cast<std::int32_t>(relative);
    std::memcpy(memory + kCopiedBytes + 1, &relative32, sizeof(relative32));
    try {
        routines::Redirect(kCopiedBytes, reinterpret_cast<void*>(address),
                           reinterpret_cast<void*>(replacement));
    }
    catch (...) {
        (void)VirtualFree(memory, 0, MEM_RELEASE);
        return false;
    }
    trampoline = memory;
    original = reinterpret_cast<combat_runtime::ProcessShellAndBodyFunction>(
        memory);
    return true;
}

bool read_rel32_callsite(const std::uintptr_t address, std::uint8_t& opcode,
                         std::int32_t& displacement)
{
    if (address == 0)
        return false;
    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(address);
    opcode = bytes[0];
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    return true;
}

bool install_wreck_post_load_suppression_callsites()
{
    static bool installed = false;
    if (installed)
        return true;
    constexpr std::array<combat_runtime::CallsiteExpectation, 3> expectations{{
        {combat_runtime::kVehiclePostMessage46CallSiteVa,
         combat_runtime::kProcessManagerPostMessageVa},
        {combat_runtime::kVehiclePostMessage47CallSiteVa,
         combat_runtime::kProcessManagerPostMessageVa},
        {combat_runtime::kVehiclePostMessage45CallSiteVa,
         combat_runtime::kProcessManagerPostMessageVa},
    }};
    for (const auto& expectation : expectations) {
        if (!combat_runtime::preflight_rel32_call(
                expectation, &read_rel32_callsite)) {
            LOG_ERROR("wreck PostMessage scope preflight failed callsite=0x%08llx target=0x%08llx",
                      static_cast<unsigned long long>(expectation.call_site),
                      static_cast<unsigned long long>(expectation.original_target));
            return false;
        }
    }
    std::size_t changed = 0;
    try {
        for (const auto& expectation : expectations) {
            routines::ChangeCall(
                reinterpret_cast<void*>(expectation.call_site),
                reinterpret_cast<void*>(&wreck_post_message_callsite_hook));
            ++changed;
        }
    }
    catch (const std::exception& error) {
        for (std::size_t index = 0; index < changed; ++index) {
            try {
                routines::ChangeCall(
                    reinterpret_cast<void*>(expectations[index].call_site),
                    reinterpret_cast<void*>(expectations[index].original_target));
            }
            catch (...) {
            }
        }
        LOG_ERROR("wreck PostMessage scope installation failed: %s", error.what());
        return false;
    }
    installed = true;
    return true;
}

bool preflight_combat_branch_callsites()
{
    using combat_runtime::CallsiteExpectation;
    constexpr std::array<CallsiteExpectation, 8> expectations{{
        {combat_runtime::kEffectSelectorCallSiteVa,
         combat_runtime::kShellEffectNameVa},
        {combat_runtime::kRoadEffectCallSiteVa,
         combat_runtime::kShellRoadEffectNameVa},
        {combat_runtime::kStaticsEffectCallSiteVa,
         combat_runtime::kShellStaticsEffectNameVa},
        {combat_runtime::kVehicleEffectCallSiteVa,
         combat_runtime::kShellVehicleEffectNameVa},
        {combat_runtime::kVehiclePartEffectSelectorCallSiteVa,
         combat_runtime::kShellVehicleEffectNameVa},
        {combat_runtime::kLandscapeEffectCallSiteVa,
         combat_runtime::kCreateEffectNodeVa},
        {combat_runtime::kCreateEffectNodeCallSiteVa,
         combat_runtime::kCreateEffectNodeVa},
        {combat_runtime::kVehiclePartAddDecalCallSiteVa,
         combat_runtime::kVehiclePartAddDecalVa},
    }};
    for (const CallsiteExpectation& expectation : expectations) {
        if (!combat_runtime::preflight_rel32_call(
                expectation, &read_rel32_callsite)) {
            LOG_ERROR("typed combat branch preflight failed callsite=0x%08llx expected=0x%08llx",
                      static_cast<unsigned long long>(expectation.call_site),
                      static_cast<unsigned long long>(
                          expectation.original_target));
            return false;
        }
    }
    return true;
}

// The original call is ai::CServer::Update(float): ECX=this, float on stack.
// A free __fastcall hook reserves EDX as the second dummy argument.
void __fastcall server_update_hook(void* server, void*, float elapsed_time)
{
    confirm_deferred_exit_route(hta::CMiracle3d::Instance());
    auto_host_coop_tick();
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
        const world_authority::ScopedWorldExecutionContext execution_scope(
            current_world_execution_context());
        g_server_update(server, nullptr, elapsed_time);
        apply_deferred_route();
        maybe_send_jip_map_ready();
        return;
    }
    if (g_state.combat_runtime == nullptr)
        g_state.combat_runtime = std::make_unique<RuntimeCombatBridge>();
    if (g_state.is_host && !g_state.combat_host_hooks_installed) {
        if (g_state.combat_runtime->install_host_hooks())
            g_state.combat_host_hooks_installed = true;
        else if (!g_state.combat_host_hooks_error_logged) {
            LOG_ERROR("typed host combat capture hook installation failed; typed publication is fail-closed");
            g_state.combat_host_hooks_error_logged = true;
        }
    }
    if (!g_state.is_host && g_state.combat_runtime != nullptr) {
        const combat_runtime::RuntimeApplyResult removal_result =
            g_state.combat_runtime->process_removals(true);
        if (removal_result == combat_runtime::RuntimeApplyResult::NativeRejected)
            LOG_DEBUG("typed wreck removal pre-sim binding unavailable");
    }
    const ReplicationSourceContext prior_world_source =
        g_state.world_source_context;
    const bool host_world_pass = g_state.is_host &&
        g_state.world_observer != nullptr;
    if (host_world_pass) {
        g_state.world_source_context =
            ReplicationSourceContext::LocalAuthoritative;
        // Native post-call seams and the post-update graph sample share this
        // pass cache, so an object/link observed by both is journaled once.
        g_state.world_observer->begin_observation_pass();
    }
    // Replica mode is a safe engine-boundary policy, not an XML/Lua guard:
    // local dynamic AI is retired before the native update and only entities
    // already admitted by the host registry remain materializable.
    suppress_client_dynamic_entities();
    if (!g_state.player_slots_ready)
        (void)try_activate_player_slots();
    run_raid_autotest_tick();
    apply_local_assigned_spawn();
    // EntitySpawn is reliable and is processed by pump(). CreateObject alters
    // the scene graph, so materialize remote vehicle replicas only at this
    // native pre-simulation boundary, never from post-update interpolation.
    materialize_remote_vehicle_replicas();
    if (!g_state.is_host && g_state.client_join_failure_pending) {
        g_state.client_join_failure_pending = false;
        LOG_ERROR("match join failed during native synchronization materialization");
        if (host_world_pass)
            g_state.world_source_context = prior_world_source;
        (void)LeaveSession(SessionLeaveReason::User);
        apply_deferred_route();
        maybe_send_jip_map_ready();
        return;
    }
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
    publish_host_local_horn_state();
    apply_host_weapons(elapsed_time);
    {
        const world_authority::ScopedWorldExecutionContext execution_scope(
            current_world_execution_context());
        g_server_update(server, nullptr, elapsed_time);
    }
    if (g_state.is_host && g_state.combat_runtime != nullptr)
        g_state.combat_runtime->process_host_wreck_candidates();
    ++g_state.server_tick;
    if (g_state.deferred_route != RuntimeState::DeferredRoute::None) {
        if (host_world_pass)
            g_state.world_source_context = prior_world_source;
        apply_deferred_route();
        maybe_send_jip_map_ready();
        return;
    }
    if (g_state.is_host) {
        observe_authoritative_world();
        observe_authoritative_quest_state();
    }
    // Vehicle::_EvaluateToDead runs inside the original server update, after
    // the damage hook returns. Observe the authoritative post-update state so
    // the combat harness records a real death rather than an early health hit.
    observe_authoritative_combat_autotest_death();
    emit_combat_host_survival_heartbeat();
    send_client_input();
    {
        const world_authority::ScopedWorldExecutionContext presentation_scope(
            current_world_execution_context(false, true));
        apply_host_weapon_presentations(elapsed_time);
        apply_remote_snapshots(elapsed_time);
        apply_local_weapon_presentation();
        // Legacy ImpactDamage is not an active multiplayer replica path.
        // Typed impact/damage/death messages are applied by combat_runtime;
        // retaining this call here would keep the compatibility queue
        // reachable from an active client even though its native callbacks
        // are deliberately prohibited.
        if (!g_state.is_host && !IsSessionActive())
            apply_pending_impact_damage();
        apply_local_correction();
    }
    if (!g_state.is_host && g_state.client_leave_session_pending) {
        g_state.client_leave_session_pending = false;
        if (host_world_pass)
            g_state.world_source_context = prior_world_source;
        (void)LeaveSession(SessionLeaveReason::Death);
        apply_deferred_route();
        return;
    }
    send_client_loadout();
    // The ODE frame is complete here; capture only through Vehicle/PhysicObj API.
    capture_and_broadcast_host_snapshot();
    if (host_world_pass)
        g_state.world_source_context = prior_world_source;
    maybe_send_jip_map_ready();
}

} // namespace

[[nodiscard]] AttachmentIdentity descriptor_part_identity(
    const std::vector<VehicleDescriptorNode>& nodes,
    const VehicleDescriptorNode& node)
{
    std::vector<const VehicleDescriptorNode*> chain;
    const VehicleDescriptorNode* current = &node;
    for (std::size_t count = 0; current != nullptr && count <= nodes.size();
         ++count) {
        chain.push_back(current);
        if (current->parent_instance_id == 0)
            break;
        current = descriptor_node_by_id(nodes, current->parent_instance_id);
    }
    if (chain.empty() || chain.back()->parent_instance_id != 0)
        return {};
    std::string path;
    for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
        if ((*iterator)->slot.empty())
            return {};
        if (!path.empty())
            path.push_back('/');
        path += (*iterator)->slot;
    }
    if (path.empty() || node.prototype_id < 0)
        return {};
    AttachmentIdentity identity{};
    identity.path_hash = weapon_identity_hash("kraken/part-path/v1", path);
    identity.attachment_id = weapon_identity_hash(
        "kraken/part-attachment/v1",
        path + "#" + std::to_string(node.prototype_id));
    return identity;
}

struct RuntimeCombatBridge::Impl {
    struct Resolver final : combat_runtime::ReplicaResolver {
        [[nodiscard]] bool resolve(const NetEntityRef& identity,
                                   combat_runtime::ReplicaBinding& output) override
        {
            output = {};
            if (g_state.is_host || !IsSessionActive() ||
                identity.net_id == kInvalidNetId ||
                identity.generation == kInvalidEntityGeneration)
                return false;
            RemoteEntity* const remote = find_remote(identity.net_id);
            const bool supported_replica_kind = remote != nullptr &&
                (remote->kind == EntityKind::PlayerVehicle ||
                 remote->kind == EntityKind::NpcVehicle);
            if (remote == nullptr || !remote->has_spawn || remote->retired ||
                remote->generation != identity.generation ||
                !supported_replica_kind ||
                remote->prototype_id < 0)
                return false;
            ObjId object_id = kInvalidObjId;
            EntityGeneration bound_generation = kInvalidEntityGeneration;
            if (!g_state.entities.lookup_obj_id(identity.net_id, object_id,
                                                bound_generation) ||
                bound_generation != identity.generation)
                return false;
            hta::ai::CServer* const server = hta::ai::CServer::Instance();
            hta::ai::Obj* const object =
                server != nullptr && server->m_pObjects != nullptr
                    ? server->m_pObjects->GetEntityByObjId(object_id) : nullptr;
            hta::ai::Vehicle* const vehicle = vehicle_from_object(object);
            if (vehicle == nullptr || object == nullptr ||
                object->GetDeletedStatus() ||
                vehicle->GetPrototypeId() != remote->prototype_id ||
                vehicle->GetChassis() == nullptr ||
                vehicle->GetChassis()->GetPrototypeId() < 0 ||
                vehicle->GetClass() == nullptr ||
                vehicle->GetClass()->m_index <= 0 ||
                vehicle->bIsUpdatingByODE() || vehicle->m_AI.m_pDM != nullptr)
                return false;
            output.identity = identity;
            output.class_id = static_cast<std::uint32_t>(
                vehicle->GetClass()->m_index);
            output.chassis_id = static_cast<std::uint32_t>(
                vehicle->GetChassis()->GetPrototypeId());
            output.prototype_id = vehicle->GetPrototypeId();
            output.prototype_name = native_prototype_name(*vehicle);
            if (output.prototype_name.empty())
                return false;
            output.native_vehicle = vehicle;
            // ObjId is a local lookup handle and never enters a typed packet.
            output.local_object_id = object_id;
            output.replica_authority = true;
            output.inert = true;
            return true;
        }

        [[nodiscard]] bool retire(const NetEntityRef& identity) override
        {
            if (identity.net_id == kInvalidNetId ||
                identity.generation == kInvalidEntityGeneration)
                return false;
            EntityGeneration current = kInvalidEntityGeneration;
            if (!g_state.entities.lookup_generation(identity.net_id, current) ||
                current != identity.generation)
                return false;
            const bool unbound = g_state.entities.unbind_net_id(identity.net_id) ==
                EntityRegistryUnbindResult::Removed;
            if (RemoteEntity* const remote = find_remote(identity.net_id);
                remote != nullptr && remote->generation == identity.generation)
                mark_remote_despawned(*remote, identity.generation);
            return unbound;
        }
    } resolver;

    combat_runtime::ReplicaNativeOperations native;
    combat_runtime::HornNodeOperations horn;
    std::unique_ptr<combat_runtime::ReplicaCombatRuntime> replica;
    std::unique_ptr<combat_runtime::TransactionalWreckMaterializer>
        wreck_materializer;
    combat_runtime::CanonicalCueResolver cues;
    combat_runtime::HostImpactCapture host_capture;
    combat_runtime::HostProcessShellAndBodyHook process_hook;
    NativeObjectArchiveReassembler wreck_archives;
    std::vector<hta::ai::Vehicle*> evaluate_to_dead_candidates;
    std::vector<DamageResult> pending_wreck_requests;
    NativeObjectArchiveV2* active_wreck_archive = nullptr;
    void* process_trampoline = nullptr;
    bool branch_hooks_installed = false;
    bool evaluate_to_dead_hook_installed = false;

    Impl()
        : cues(combat_runtime::CanonicalCueResolver::Resolve(
              [](const std::string_view native_name, ResourceCue& output) {
                  return try_make_resource_cue(native_name, output);
              })),
        host_capture(cues)
    {
        native.get_health = [](void* const value) {
            return value != nullptr
                ? static_cast<hta::ai::Vehicle*>(value)->GetHealth() : -1.0f;
        };
        native.get_max_health = [](void* const value) {
            return value != nullptr
                ? static_cast<hta::ai::Vehicle*>(value)->GetMaxHealth() : -1.0f;
        };
        native.set_health_unsafe = [](void* const value, const float health) {
            if (value == nullptr || !std::isfinite(health))
                return false;
            auto* const vehicle = static_cast<hta::ai::Vehicle*>(value);
            const float maximum = vehicle->GetMaxHealth();
            if (!std::isfinite(maximum) || health < 0.0f || health > maximum)
                return false;
            // Vehicle::Health().value() is the verified
            // NumericInRange::m_value at +0x58. Bind the exact SetUnsafe VA
            // because the restored header has no linked implementation;
            // callback-bearing Numeric::set is forbidden here.
            auto& numeric = vehicle->Health().m_value;
            using SetUnsafe = void(__thiscall *)(hta::ai::Numeric<float>*,
                                                  float);
            reinterpret_cast<SetUnsafe>(combat_runtime::kNumericSetUnsafeVa)(
                &numeric, health);
            return true;
        };
        native.read_flags = [](void* const value) {
            if (value == nullptr)
                return std::uint32_t{0};
            const auto* const flags = reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::byte*>(value) + 0x40);
            return *flags;
        };
        native.set_flags_masked = [](void* const value,
                                     const std::uint32_t mask,
                                     const std::uint32_t masked_value) {
            if (value == nullptr || mask != combat_runtime::kObjDeadFlag ||
                (masked_value & ~mask) != 0)
                return false;
            auto* const flags = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::byte*>(value) + 0x40);
            *flags = (*flags & ~mask) | (masked_value & mask);
            return true;
        };
        native.create_effect_node =
            [](void*, const ImpactPresentation& event,
               const combat_runtime::ImpactGeometry& geometry) {
                if (validate_resource_cue(event.effect_cue, false) !=
                    CombatPresentationCodecError::None)
                    return false;
                const auto original = reinterpret_cast<decltype(
                    &hta::ai::PhysicBody::CreateEffectNode)>(
                    combat_runtime::kCreateEffectNodeVa);
                const hta::CStr name(event.effect_cue.name.c_str());
                const hta::CVector position(
                    geometry.effect_position.x, geometry.effect_position.y,
                    geometry.effect_position.z);
                const hta::Quaternion rotation(
                    geometry.effect_rotation.x, geometry.effect_rotation.y,
                    geometry.effect_rotation.z, geometry.effect_rotation.w);
                return original(name, position, rotation,
                                geometry.remove_if_free, geometry.effect_scale)
                    != nullptr;
            };
        native.add_decal =
            [](void* const value, const ImpactPresentation& event,
               const combat_runtime::ImpactGeometry& geometry) {
                if (value == nullptr ||
                    validate_resource_cue(event.decal_cue, false) !=
                    CombatPresentationCodecError::None ||
                    geometry.mesh_id > (std::numeric_limits<std::uint32_t>::max)() ||
                    !geometry.has_decal_tangent)
                    return false;
                auto* const vehicle = static_cast<hta::ai::Vehicle*>(value);
                RemoteEntity* const remote = find_remote(event.target.dynamic.net_id);
                if (remote == nullptr ||
                    remote->generation != event.target.dynamic.generation)
                    return false;
                hta::ai::VehiclePart* const part =
                    find_native_part_by_identity(*vehicle, geometry.target_part);
                hta::ai::CServer* const server = hta::ai::CServer::Instance();
                hta::ai::DynamicScene* const scene =
                    server != nullptr ? server->m_pDynamicScene : nullptr;
                if (part == nullptr || scene == nullptr)
                    return false;
                const hta::CStr cue_name(event.decal_cue.name.c_str());
                const std::int32_t decal_id = scene->AddDecalName(cue_name);
                if (decal_id < 0)
                    return false;
                using AddDecal = void(__fastcall *)(
                    hta::ai::VehiclePart*, void*, const hta::CVector&,
                    const hta::CVector&, const hta::CVector&, std::uint32_t,
                    std::int32_t);
                const auto add_decal = reinterpret_cast<AddDecal>(
                    combat_runtime::kVehiclePartAddDecalVa);
                add_decal(
                    part, nullptr,
                    hta::CVector(geometry.hit_position.x,
                                 geometry.hit_position.y,
                                 geometry.hit_position.z),
                    hta::CVector(geometry.contact_normal.x,
                                 geometry.contact_normal.y,
                                 geometry.contact_normal.z),
                    hta::CVector(geometry.decal_tangent.x,
                                 geometry.decal_tangent.y,
                                 geometry.decal_tangent.z),
                    static_cast<std::uint32_t>(geometry.mesh_id), decal_id);
                return true;
            };
        native.schedule_removal = [](void* const value) {
            if (value == nullptr)
                return false;
            hta::ai::CServer* const server = hta::ai::CServer::Instance();
            if (server == nullptr || server->m_pObjects == nullptr)
                return false;
            server->m_pObjects->AddObjIdToRemove(
                static_cast<hta::ai::Obj*>(value)->GetId());
            return true;
        };
        native.has_disappeared = [](void* const value) {
            if (value == nullptr)
                return true;
            const auto* const object = static_cast<const hta::ai::Obj*>(value);
            hta::ai::CServer* const server = hta::ai::CServer::Instance();
            if (server == nullptr || server->m_pObjects == nullptr)
                return false;
            const hta::ai::Obj* const current =
                server->m_pObjects->GetEntityByObjId(object->GetId());
            return current == nullptr || current->GetDeletedStatus();
        };

        horn.create_node = [](void* const value, const ResourceCue& cue) -> void* {
            if (value == nullptr ||
                validate_resource_cue(cue, false) !=
                    CombatPresentationCodecError::None)
                return static_cast<void*>(nullptr);
            using CreateNode = decltype(&hta::ai::PhysicBody::CreateNode);
            const hta::CVector origin(0.0f, 0.0f, 0.0f);
            const hta::CStr name(cue.name.c_str());
            return reinterpret_cast<CreateNode>(combat_runtime::kCreateNodeVa)(
                name, hta::m3d::SgNode::RITUAL_NONE, origin,
                static_cast<hta::ai::PhysicBody*>(value), false);
        };
        horn.set_property = [](void* const value, const std::uint32_t property,
                               const bool loop) {
            if (value == nullptr || property != combat_runtime::kHornLoopProperty)
                return false;
            using SetProperty = bool(__thiscall *)(
                hta::m3d::SgSoundSourceNode*, std::uint32_t, void*);
            bool mutable_loop = loop;
            return reinterpret_cast<SetProperty>(
                combat_runtime::kSoundSetPropertyVa)(
                static_cast<hta::m3d::SgSoundSourceNode*>(value), property,
                &mutable_loop);
        };
        horn.add_child = [](void* const value, void* const node) {
            if (value == nullptr || node == nullptr)
                return false;
            auto* const chassis = static_cast<hta::ai::Vehicle*>(value)->GetChassis();
            auto* const body = static_cast<hta::ai::PhysicBody*>(chassis);
            if (chassis == nullptr || body->m_Node == nullptr)
                return false;
            using AddChild = bool(__thiscall *)(hta::m3d::SgNode*,
                                                hta::m3d::Object*);
            return reinterpret_cast<AddChild>(combat_runtime::kSgNodeAddChildVa)(
                body->m_Node, static_cast<hta::m3d::Object*>(node));
        };
        horn.remove_child = [](void* const value, void* const node) {
            if (value == nullptr || node == nullptr)
                return false;
            auto* const chassis = static_cast<hta::ai::Vehicle*>(value)->GetChassis();
            auto* const body = static_cast<hta::ai::PhysicBody*>(chassis);
            if (chassis == nullptr || body->m_Node == nullptr)
                return false;
            using RemoveChild = bool(__thiscall *)(hta::m3d::SgNode*,
                                                   hta::m3d::Object*);
            return reinterpret_cast<RemoveChild>(
                combat_runtime::kSgNodeRemoveChildVa)(
                body->m_Node, static_cast<hta::m3d::Object*>(node));
        };
        horn.release_node = [](void* const value) {
            if (value == nullptr)
                return;
            using CanBeFree = void(__thiscall *)(hta::m3d::SgSoundSourceNode*);
            reinterpret_cast<CanBeFree>(0x006617E0u)(
                static_cast<hta::m3d::SgSoundSourceNode*>(value));
        };

        const bool post_load_scope_ready =
            install_wreck_post_load_suppression_callsites();
        combat_runtime::WreckMaterializerOperations wreck_operations{};
        wreck_operations.create_suspended_transaction =
            [](const NativeObjectArchiveV2&,
               const combat_runtime::ReplicaBinding& source,
               combat_runtime::WreckMaterializerOperations::Transaction& tx)
                -> void* {
                hta::ai::CServer* const server = hta::ai::CServer::Instance();
                if (server == nullptr || server->m_pObjects == nullptr ||
                    source.prototype_id < 0)
                    return nullptr;
                for (auto iterator = server->m_pObjects->begin();
                     iterator != server->m_pObjects->end(); ++iterator) {
                    if (*iterator != nullptr)
                        tx.preexisting_object_ids.push_back((*iterator)->GetId());
                }
                char name[64]{};
                std::snprintf(name, sizeof(name), "kraken_wreck_%u_%u",
                              source.identity.net_id,
                              static_cast<unsigned>(source.identity.generation));
                const ObjId object_id = server->m_pObjects->
                    CreateNewObjectWithSuspendedPostLoad(
                        source.prototype_id, name, -1, -1);
                if (object_id < 0)
                    return nullptr;
                tx.created_object_ids.push_back(object_id);
                return server->m_pObjects->GetEntityByObjId(object_id);
            };
        wreck_operations.collect_created_objects =
            [](void*, combat_runtime::WreckMaterializerOperations::Transaction& tx) {
                hta::ai::CServer* const server = hta::ai::CServer::Instance();
                if (server == nullptr || server->m_pObjects == nullptr)
                    return;
                for (auto iterator = server->m_pObjects->begin();
                     iterator != server->m_pObjects->end(); ++iterator) {
                    const hta::ai::Obj* const object = *iterator;
                    if (object == nullptr)
                        continue;
                    const ObjId id = object->GetId();
                    const bool was_present = std::find(
                        tx.preexisting_object_ids.begin(),
                        tx.preexisting_object_ids.end(), id) !=
                        tx.preexisting_object_ids.end();
                    const bool already_recorded = std::find(
                        tx.created_object_ids.begin(),
                        tx.created_object_ids.end(), id) !=
                        tx.created_object_ids.end();
                    if (!was_present && !already_recorded)
                        tx.created_object_ids.push_back(id);
                }
            };
        wreck_operations.validate_resources =
            [](void* root, const NativeObjectArchiveV2& archive,
               const combat_runtime::ReplicaBinding& source) {
                auto* const object = static_cast<hta::ai::Obj*>(root);
                if (object == nullptr || !object->IsKindOf("Vehicle") ||
                    validate_native_object_archive_v2(archive) !=
                        NativeObjectArchiveErrorCode::None ||
                    archive.map_namespace != static_world_map_namespace() ||
                    archive.resource_fingerprint !=
                        g_state.session_identity.resource_fingerprint ||
                    source.prototype_id < 0 || source.prototype_name.empty() ||
                    object->GetPrototypeId() != source.prototype_id ||
                    native_prototype_name(*object) != source.prototype_name)
                    return false;
                hta::ai::CServer* const server = hta::ai::CServer::Instance();
                if (server == nullptr)
                    return false;
                const auto root_entry = std::find_if(
                    archive.manifest.begin(), archive.manifest.end(),
                    [](const NativeObjectArchiveManifestEntry& entry) {
                        return entry.parent_path.empty();
                    });
                if (root_entry == archive.manifest.end() ||
                    root_entry->prototype_name != source.prototype_name)
                    return false;
                const auto resolve_prototype =
                    [server](const std::string& name) {
                        if (name.empty())
                            return static_cast<const hta::ai::PrototypeInfo*>(
                                nullptr);
                        const hta::CStr native_name(name.c_str());
                        const std::int32_t id =
                            server->GetPrototypeId(native_name);
                        if (id < 0)
                            return static_cast<const hta::ai::PrototypeInfo*>(
                                nullptr);
                        const hta::ai::PrototypeInfo* const info =
                            server->GetPrototypeInfo(id);
                        if (info == nullptr || info->m_prototypeName.c_str() ==
                                nullptr ||
                            !(info->m_prototypeName == native_name))
                            return static_cast<const hta::ai::PrototypeInfo*>(
                                nullptr);
                        return info;
                    };
                const hta::ai::PrototypeInfo* const root_info =
                    resolve_prototype(root_entry->prototype_name);
                if (root_info == nullptr || root_info->m_prototypeId !=
                        source.prototype_id)
                    return false;
                for (const NativeObjectArchiveManifestEntry& entry :
                     archive.manifest) {
                    if (!entry.prototype_name.empty() &&
                        resolve_prototype(entry.prototype_name) == nullptr)
                        return false;
                }
                return true;
            };
        // Bind the archive value through a small per-materialization slot so
        // the callback still receives the exact canonical archive bytes.
        // The slot is populated by the resolver immediately before the call.
        wreck_operations.load_structure =
            [this](void* root, const ByteView) {
                if (root == nullptr || active_wreck_archive == nullptr)
                    return false;
                const auto restored = restore_native_object_archive_v2(
                    *active_wreck_archive, *static_cast<hta::ai::Obj*>(root));
                return restored.succeeded();
            };
        wreck_operations.post_load_graph =
            [post_load_scope_ready](void* root) {
                if (!post_load_scope_ready || root == nullptr)
                    return false;
                struct PostLoadScopeGuard final {
                    bool prior;
                    PostLoadScopeGuard() noexcept
                        : prior(g_wreck_post_load_scope)
                    {
                        g_wreck_post_load_scope = true;
                    }
                    ~PostLoadScopeGuard() noexcept
                    {
                        g_wreck_post_load_scope = prior;
                    }
                } scope_guard;
                static_cast<hta::ai::Obj*>(root)->PostLoad();
                return true;
            };
        wreck_operations.create_visual_part = [](void* root) {
            if (root == nullptr)
                return false;
            static_cast<hta::ai::Obj*>(root)->CreateVisualPart();
            return true;
        };
        wreck_operations.apply_visual_runtime =
            [](void* root, const NativeObjectArchiveVisualRuntime& runtime) {
                auto* const object = static_cast<hta::ai::Obj*>(root);
                if (object == nullptr)
                    return false;
                if (runtime.kind == NativeObjectArchiveVisualRuntimeKind::Visibility) {
                    if (runtime.enabled)
                        object->SetVisible();
                    else
                        object->SetInvisible();
                }
                // Mesh/material/transform/broken-part fields are already
                // consumed by native LoadFromXML/CreateVisualPart. They are
                // accepted here only as typed allowlisted values; no native
                // runtime property bag is restored.
                return true;
            };
        wreck_operations.apply_authoritative_pose =
            [](void* root, const CombatPose& pose) {
                auto* const vehicle = vehicle_from_object(
                    static_cast<hta::ai::Obj*>(root));
                if (vehicle == nullptr)
                    return false;
                vehicle->SetPositionSelf(to_engine_vector(pose.position));
                vehicle->SetRotationSelf(to_engine_quaternion(pose.rotation));
                return true;
            };
        wreck_operations.disable_physics = [](void* root) {
            auto* const vehicle = vehicle_from_object(
                static_cast<hta::ai::Obj*>(root));
            if (vehicle == nullptr)
                return false;
            vehicle->SetUpdatingByODE(false);
            vehicle->DisablePhysics();
            return true;
        };
        wreck_operations.remove_from_simulation = [](void* root) {
            hta::ai::CServer* const server = hta::ai::CServer::Instance();
            if (server == nullptr || server->m_pObjects == nullptr || root == nullptr)
                return false;
            server->m_pObjects->AddObjToNotUpdate(
                static_cast<hta::ai::Obj*>(root));
            return true;
        };
        wreck_operations.validate_wreck = [](void* root) {
            auto* const vehicle = vehicle_from_object(
                static_cast<hta::ai::Obj*>(root));
            return vehicle != nullptr && !vehicle->bIsUpdatingByODE() &&
                vehicle->m_AI.m_pDM == nullptr;
        };
        wreck_operations.bind_wreck = [](const NetEntityRef& identity,
                                          void* root) {
            if (identity.net_id == kInvalidNetId ||
                identity.generation == kInvalidEntityGeneration || root == nullptr)
                return false;
            auto* const object = static_cast<hta::ai::Obj*>(root);
            RemoteEntity* const remote = find_or_add_remote(identity.net_id);
            if (remote == nullptr)
                return false;
            const EntityRegistryBindResult bound = g_state.entities.bind(
                identity.net_id, object->GetId(), identity.generation);
            if (bound != EntityRegistryBindResult::Inserted &&
                bound != EntityRegistryBindResult::AlreadyBound)
                return false;
            remote->generation = identity.generation;
            remote->kind = EntityKind::Wreck;
            remote->has_spawn = true;
            remote->prototype_id = object->GetPrototypeId();
            remote->wreck_object_id = object->GetId();
            remote->inert_wreck = true;
            remote->retired = false;
            return true;
        };
        wreck_operations.retire_source = [](const NetEntityRef& identity) {
            ObjId object_id = kInvalidObjId;
            EntityGeneration generation = kInvalidEntityGeneration;
            hta::ai::CServer* const server = hta::ai::CServer::Instance();
            if (server == nullptr || server->m_pObjects == nullptr ||
                !g_state.entities.lookup_obj_id(identity.net_id, object_id,
                                                generation) ||
                generation != identity.generation)
                return false;
            server->m_pObjects->AddObjIdToRemove(object_id);
            return true;
        };
        wreck_operations.unbind_wreck = [](const NetEntityRef& identity, void*) {
            (void)g_state.entities.unbind_net_id(identity.net_id);
            if (RemoteEntity* const remote = find_remote(identity.net_id);
                remote != nullptr && remote->generation == identity.generation) {
                remote->wreck_object_id = kInvalidObjId;
                remote->inert_wreck = false;
                remote->has_spawn = false;
            }
        };
        wreck_operations.destroy_transaction =
            [](combat_runtime::WreckMaterializerOperations::Transaction& tx) {
                hta::ai::CServer* const server = hta::ai::CServer::Instance();
                if (server != nullptr && server->m_pObjects != nullptr) {
                    for (auto iterator = tx.created_object_ids.rbegin();
                         iterator != tx.created_object_ids.rend(); ++iterator)
                        server->m_pObjects->AddObjIdToRemove(*iterator);
                }
                tx.created_object_ids.clear();
                tx.preexisting_object_ids.clear();
                tx.root = nullptr;
            };
        wreck_materializer = std::make_unique<
            combat_runtime::TransactionalWreckMaterializer>(
                std::move(wreck_operations));
        replica = std::make_unique<combat_runtime::ReplicaCombatRuntime>(
            resolver, std::move(native), std::move(horn),
            [this](
                const DeathWreckPresentation& event,
                const combat_runtime::ReplicaBinding& binding,
                combat_runtime::WreckResolution& output) {
                std::vector<Byte> encoded;
                NativeObjectArchiveV2 archive{};
                if (!wreck_archives.lookup(event.wreck_archive_digest, encoded) ||
                    decode_native_object_archive_v2(ByteView{encoded}, archive) !=
                        NativeObjectArchiveErrorCode::None ||
                    wreck_materializer == nullptr)
                    return false;
                this->active_wreck_archive = &archive;
                const CombatPose pose{
                    to_snapshot_vector(static_cast<hta::ai::Vehicle*>(
                        binding.native_vehicle)->GetPosition()),
                    to_snapshot_quaternion(static_cast<hta::ai::Vehicle*>(
                        binding.native_vehicle)->GetRotation())};
                const combat_runtime::WreckMaterializationResult materialized =
                    wreck_materializer->materialize(
                        archive, event.entity, event.wreck_entity, binding, pose);
                this->active_wreck_archive = nullptr;
                if (materialized !=
                    combat_runtime::WreckMaterializationResult::Committed)
                    return false;
                output.inert_replacement_ready = true;
                output.archive_verified = true;
                output.archive_digest = archive.digest;
                output.wreck_identity = event.wreck_entity;
                output.source_removal_scheduled = true;
                return true;
            });
    }
};

RuntimeCombatBridge::RuntimeCombatBridge()
    : m_impl(std::make_unique<Impl>())
{
}

RuntimeCombatBridge::~RuntimeCombatBridge()
{
    if (m_impl != nullptr &&
        g_evaluate_to_dead_candidates == &m_impl->evaluate_to_dead_candidates)
        g_evaluate_to_dead_candidates = nullptr;
}

bool RuntimeCombatBridge::request_host_wreck(const DamageResult& damage)
{
    if (!m_impl || !g_state.is_host || !IsSessionActive() ||
        validate_damage_result(damage) != CombatPresentationCodecError::None)
        return false;
    const auto duplicate = std::find_if(
        m_impl->pending_wreck_requests.begin(),
        m_impl->pending_wreck_requests.end(),
        [&damage](const DamageResult& current) {
            return current.event_id == damage.event_id &&
                   current.target.net_id == damage.target.net_id &&
                   current.target.generation == damage.target.generation;
        });
    if (duplicate == m_impl->pending_wreck_requests.end()) {
        try {
            m_impl->pending_wreck_requests.push_back(damage);
        }
        catch (...) {
            return false;
        }
    }
    // Damage can be observed before Vehicle::Update performs the terminal
    // transition. The request is only a linkage hint; capture is deferred to
    // the exact post-_EvaluateToDead candidate boundary.
    return true;
}

void RuntimeCombatBridge::process_host_wreck_candidates()
{
    if (!m_impl || !g_state.is_host || !IsSessionActive())
        return;
    for (hta::ai::Vehicle* const vehicle : m_impl->evaluate_to_dead_candidates) {
        if (vehicle == nullptr)
            continue;
        const HostEntity* const source = find_host_entity_by_object(vehicle->GetId());
        if (source == nullptr || !source->active)
            continue;
        const auto pending = std::find_if(
            m_impl->pending_wreck_requests.begin(),
            m_impl->pending_wreck_requests.end(),
            [source](const DamageResult& damage) {
                return damage.target.net_id == source->entity_id &&
                       damage.target.generation == source->generation;
            });
        if (pending == m_impl->pending_wreck_requests.end())
            continue;
        std::vector<Byte> encoded;
        const NativeObjectArchiveResult captured =
            capture_native_object_archive(
                *vehicle, static_world_map_namespace(),
                g_state.session_identity.resource_fingerprint, encoded);
        if (!captured) {
            LOG_ERROR("wreck archive capture rejected entity=%u objId=%d error=%s detail=%s",
                      source->entity_id, vehicle->GetId(),
                      native_object_archive_error_name(captured.error),
                      captured.detail.c_str());
            continue;
        }
        const NetId wreck_id = allocate_dynamic_entity_id();
        if (wreck_id == kInvalidNetId)
            continue;
        const std::uint32_t revision = 1; // MVP policy: inert wreck is frozen.
        HostWreckArchive archive{};
        archive.archive_id = wreck_id;
        archive.revision = revision;
        archive.digest = captured.digest;
        archive.encoded = encoded;
        if (encoded.size() > kNativeObjectArchiveMaxCacheBytes)
            continue;
        std::size_t cached_bytes = 0;
        for (const HostWreckArchive& cached : g_state.host_wreck_archives)
            cached_bytes += cached.encoded.size();
        while ((!g_state.host_wreck_archives.empty() &&
                (g_state.host_wreck_archives.size() >=
                     kNativeObjectArchiveMaxCacheEntries ||
                 cached_bytes + encoded.size() >
                     kNativeObjectArchiveMaxCacheBytes))) {
            cached_bytes -= g_state.host_wreck_archives.front().encoded.size();
            g_state.host_wreck_archives.erase(
                g_state.host_wreck_archives.begin());
        }
        g_state.host_wreck_archives.push_back(std::move(archive));

        DeathWreckPresentation state{};
        state.session_epoch = g_state.session_epoch;
        state.transition_id = pending->event_id;
        state.server_tick = g_state.server_tick;
        state.entity = {source->entity_id, source->generation};
        state.wreck_entity = {wreck_id, kInitialEntityGeneration};
        state.reason = DeathWreckReason::Combat;
        state.death_cue = make_resource_cue("effects/death/vehicle");
        state.wreck_cue = make_resource_cue("wrecks/vehicle/default");
        state.wreck_archive_id = wreck_id;
        state.wreck_archive_revision = revision;
        state.wreck_archive_digest = captured.digest;
        state.wreck_archive_size = static_cast<std::uint32_t>(encoded.size());
        state.wreck_archive_chunk_size = static_cast<std::uint16_t>(
            kNativeObjectArchiveChunkPayloadBytes);
        state.wreck_archive_chunk_count = static_cast<std::uint16_t>(
            (encoded.size() + kNativeObjectArchiveChunkPayloadBytes - 1) /
            kNativeObjectArchiveChunkPayloadBytes);
        state.wreck_variant_id = resource_cue_hash("wrecks/vehicle/default");
        state.terminal = true;
        if (validate_death_wreck_presentation(state) ==
            CombatPresentationCodecError::None)
            publish_host_death(state);
        m_impl->pending_wreck_requests.erase(pending);
    }
    m_impl->evaluate_to_dead_candidates.clear();
}

bool RuntimeCombatBridge::install_host_hooks()
{
    if (!m_impl || !g_state.is_host || !IsSessionActive())
        return false;
    if (!m_impl->evaluate_to_dead_hook_installed) {
        const combat_runtime::CallsiteExpectation expectation{
            combat_runtime::kVehicleEvaluateToDeadCallSiteVa,
            combat_runtime::kVehicleEvaluateToDeadVa};
        // The recovered callsite is inside Vehicle::Update at +0x109. Keep
        // the patch scoped to that exact native function; a relocated or
        // mismatched image fails closed before any callsite mutation.
        if (combat_runtime::kVehicleEvaluateToDeadCallSiteVa -
                combat_runtime::kVehicleUpdateVa != 0x109u) {
            LOG_ERROR("wreck hook Vehicle::Update callsite scope mismatch");
            return false;
        }
        if (!combat_runtime::preflight_rel32_call(
                expectation, &read_rel32_callsite)) {
            LOG_ERROR("wreck hook preflight failed callsite=0x%08llx target=0x%08llx bytes must be E8 02 C7 FF FF",
                      static_cast<unsigned long long>(expectation.call_site),
                      static_cast<unsigned long long>(expectation.original_target));
            return false;
        }
        try {
            routines::ChangeCall(
                reinterpret_cast<void*>(expectation.call_site),
                reinterpret_cast<void*>(&evaluate_to_dead_wreck_hook));
        }
        catch (const std::exception& error) {
            LOG_ERROR("wreck hook installation failed: %s", error.what());
            return false;
        }
        g_evaluate_to_dead_candidates = &m_impl->evaluate_to_dead_candidates;
        m_impl->evaluate_to_dead_hook_installed = true;
    }
    // All branch callsites are checked before the ProcessShellAndBody detour
    // is installed. A mismatched opcode/target leaves every branch untouched
    // and still permits the caller to fail closed without changing gameplay.
    if (!m_impl->branch_hooks_installed &&
        !preflight_combat_branch_callsites())
        return false;
    if (!m_impl->process_hook.installed()) {
        combat_runtime::HostProcessHookCallbacks callbacks{};
        callbacks.initialize = [](hta::ai::Shell*, hta::ai::PhysicBody* body,
                                  dContact* contacts,
                                  std::uint32_t* contact_count,
                                  const bool,
                                  combat_runtime::HostImpactObservation& observation) {
            observation.session_epoch = g_state.session_epoch;
            observation.event_id = g_state.next_impact_event_id++;
            if (g_state.next_impact_event_id == 0)
                g_state.next_impact_event_id = 1;
            observation.server_tick = g_state.server_tick;
            observation.geometry.effect_rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            capture_process_contact(contacts, contact_count, observation);
            ImpactTargetIdentity actual_target{};
            AttachmentIdentity actual_part{};
            if (resolve_process_body_target(body, actual_target, actual_part)) {
                observation.target = actual_target;
                observation.geometry.target_part = actual_part;
                observation.target_captured = true;
            }
            if (g_active_host_weapon_command != nullptr) {
                const WeaponCommand& command = *g_active_host_weapon_command;
                observation.shot_id = command.shot_id;
                observation.gun = command.gun;
                observation.shooter = {command.entity_id,
                                       command.entity_generation};
            }
        };
        callbacks.complete = [this](const combat_runtime::HostImpactObservation& observation) {
            if (!g_state.session || !g_state.session->running() ||
                !g_state.is_host)
                return;
            combat_runtime::HostImpactPublication publication{};
            publication.publish_impact = [](const ImpactPresentation& state) {
                publish_host_impact(state);
                return true;
            };
            publication.publish_damage = [](const DamageResult& state) {
                publish_host_damage(state);
                return true;
            };
            const combat_runtime::RuntimeApplyResult result =
                m_impl->host_capture.publish(observation, publication);
            if (result != combat_runtime::RuntimeApplyResult::Applied)
                LOG_DEBUG("typed host impact capture closed result=%u event=%llu",
                          static_cast<unsigned>(result),
                          static_cast<unsigned long long>(observation.event_id));
        };
        m_impl->process_hook.set_callbacks(std::move(callbacks));
        combat_runtime::HostProcessHookInstaller installer{};
        installer.install = [this](const std::uintptr_t address,
                                    const combat_runtime::ProcessShellAndBodyFunction replacement,
                                    combat_runtime::ProcessShellAndBodyFunction& original) {
            return install_process_shell_and_body_detour(
                address, replacement, original, m_impl->process_trampoline);
        };
        if (!m_impl->process_hook.install(
                installer, &combat_runtime::process_shell_and_body_capture_hook))
            return false;
    }
    if (!m_impl->branch_hooks_installed) {
        using combat_runtime::CallsiteExpectation;
        constexpr std::array<CallsiteExpectation, 8> expectations{{
            {combat_runtime::kEffectSelectorCallSiteVa,
             combat_runtime::kShellEffectNameVa},
            {combat_runtime::kRoadEffectCallSiteVa,
             combat_runtime::kShellRoadEffectNameVa},
            {combat_runtime::kStaticsEffectCallSiteVa,
             combat_runtime::kShellStaticsEffectNameVa},
            {combat_runtime::kVehicleEffectCallSiteVa,
             combat_runtime::kShellVehicleEffectNameVa},
            {combat_runtime::kVehiclePartEffectSelectorCallSiteVa,
             combat_runtime::kShellVehicleEffectNameVa},
            {combat_runtime::kLandscapeEffectCallSiteVa,
             combat_runtime::kCreateEffectNodeVa},
            {combat_runtime::kCreateEffectNodeCallSiteVa,
             combat_runtime::kCreateEffectNodeVa},
            {combat_runtime::kVehiclePartAddDecalCallSiteVa,
             combat_runtime::kVehiclePartAddDecalVa},
        }};
        const std::array<void*, 8> replacements{
            reinterpret_cast<void*>(&capture_terrain_selector),
            reinterpret_cast<void*>(&capture_road_selector),
            reinterpret_cast<void*>(&capture_statics_selector),
            reinterpret_cast<void*>(&capture_vehicle_selector),
            reinterpret_cast<void*>(&capture_vehicle_selector),
            reinterpret_cast<void*>(&capture_landscape_effect_node_call),
            reinterpret_cast<void*>(&capture_effect_node_call),
            reinterpret_cast<void*>(&capture_decal_call)};
        std::size_t changed = 0;
        try {
            for (std::size_t index = 0; index != expectations.size(); ++index) {
                routines::ChangeCall(
                    reinterpret_cast<void*>(expectations[index].call_site),
                    replacements[index]);
                ++changed;
            }
        }
        catch (const std::exception& error) {
            for (std::size_t index = 0; index != changed; ++index) {
                try {
                    routines::ChangeCall(
                        reinterpret_cast<void*>(expectations[index].call_site),
                        reinterpret_cast<void*>(
                            expectations[index].original_target));
                }
                catch (...) {
                    // The original preflight succeeded; a restoration failure
                    // is logged below and the branch path remains disabled.
                }
            }
            LOG_ERROR("typed combat branch hook installation failed: %s",
                      error.what());
            return false;
        }
        m_impl->branch_hooks_installed = true;
    }
    return true;
}

combat_runtime::RuntimeApplyResult RuntimeCombatBridge::apply_impact(
    const ImpactPresentation& state)
{
    return m_impl && m_impl->replica
        ? m_impl->replica->apply_impact(state)
        : combat_runtime::RuntimeApplyResult::NativeRejected;
}

combat_runtime::RuntimeApplyResult RuntimeCombatBridge::apply_damage(
    const DamageResult& state)
{
    return m_impl && m_impl->replica
        ? m_impl->replica->apply_damage(state)
        : combat_runtime::RuntimeApplyResult::NativeRejected;
}

combat_runtime::RuntimeApplyResult RuntimeCombatBridge::apply_death(
    const DeathWreckPresentation& state)
{
    return m_impl && m_impl->replica
        ? m_impl->replica->apply_death(state)
        : combat_runtime::RuntimeApplyResult::NativeRejected;
}

bool RuntimeCombatBridge::accept_wreck_archive_chunk(
    const ByteView payload, const std::uint64_t now_ms)
{
    if (!m_impl || g_state.is_host || !IsSessionActive())
        return false;
    NativeObjectArchiveChunk chunk{};
    if (decode_native_object_archive_chunk(payload, chunk) !=
        NativeObjectArchiveErrorCode::None)
        return false;
    std::vector<Byte> complete;
    const NativeObjectArchiveTransferResult result =
        m_impl->wreck_archives.accept(chunk, now_ms, complete);
    if (result == NativeObjectArchiveTransferResult::Complete) {
        LOG_INFO("verified wreck archive received digest=%llu bytes=%u",
                 static_cast<unsigned long long>(chunk.digest),
                 static_cast<unsigned>(complete.size()));
    }
    const bool bridge_accepted =
        result == NativeObjectArchiveTransferResult::Accepted ||
        result == NativeObjectArchiveTransferResult::Complete ||
        result == NativeObjectArchiveTransferResult::Duplicate;
    if (!bridge_accepted || m_impl->replica == nullptr)
        return bridge_accepted;
    // Keep the runtime cache and the replica's verified cache in lockstep.
    // The death linkage may arrive on the same reliable stream immediately
    // after the final chunk, so applying only to the bridge cache would leave
    // the production resolver unable to materialize the wreck.
    return m_impl->replica->accept_wreck_archive_chunk(payload, now_ms);
}

combat_runtime::RuntimeApplyResult RuntimeCombatBridge::apply_horn(
    const HornState& state)
{
    return m_impl && m_impl->replica
        ? m_impl->replica->apply_horn(state)
        : combat_runtime::RuntimeApplyResult::NativeRejected;
}

combat_runtime::RuntimeApplyResult RuntimeCombatBridge::apply_jip(
    const PresentationJipState& state)
{
    return m_impl && m_impl->replica
        ? m_impl->replica->apply_jip(state)
        : combat_runtime::RuntimeApplyResult::NativeRejected;
}

combat_runtime::RuntimeApplyResult RuntimeCombatBridge::process_removals(
    const bool pre_sim_boundary)
{
    return m_impl && m_impl->replica
        ? m_impl->replica->process_removals(pre_sim_boundary)
        : combat_runtime::RuntimeApplyResult::NativeRejected;
}

void RuntimeCombatBridge::despawn(const NetEntityRef& identity) noexcept
{
    if (m_impl && m_impl->replica)
        m_impl->replica->despawn(identity);
}

void RuntimeCombatBridge::reset() noexcept
{
    if (m_impl) {
        m_impl->wreck_archives.clear();
        m_impl->evaluate_to_dead_candidates.clear();
        m_impl->pending_wreck_requests.clear();
        m_impl->active_wreck_archive = nullptr;
    }
    if (m_impl && m_impl->replica)
        m_impl->replica->reset();
}

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
    g_state.auto_host_coop = effective.auto_host_coop;
    if (g_state.match.state() == MatchState::Offline) {
        g_state.match_request.max_players = effective.match_max_players;
        g_state.match_request.join_policy = effective.join_policy;
    }
    g_state.raid_autotest_enabled =
        environment_uint("KRAKEN_EFA_RAID_AUTOTEST", 0, 0, 1) != 0;
    g_state.combat_autotest_scenario =
        environment("KRAKEN_EFA_COMBAT_AUTOTEST").value_or("");
    g_state.combat_autotest_weapon_part =
        environment("KRAKEN_EFA_COMBAT_WEAPON_PART").value_or("");
    g_state.next_combat_autotest = Clock::now();
    g_state.next_combat_host_survival = Clock::now();
    g_state.combat_autotest_sequence = 1;
    g_state.combat_autotest_started = false;
    g_state.combat_autotest_armed = false;
    g_state.combat_autotest_death_logged = false;
    g_state.next_raid_autotest = Clock::now();
    g_state.raid_autotest_error_logged = false;
    if (g_state.raid_autotest_enabled) {
        LOG_INFO("raid autotest enabled (test-only environment mode)");
        g_state.raid_autotest_bootstrap_attempted = false;
        g_state.raid_autotest_native_save_loaded = false;
        g_state.raid_autotest_matchmaking_attempted = false;
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
        if (g_state.combat_runtime == nullptr)
            g_state.combat_runtime = std::make_unique<RuntimeCombatBridge>();
        else
            g_state.combat_runtime->reset();
        g_state.combat_host_hooks_installed = false;
        g_state.combat_host_hooks_error_logged = false;
        g_state.spawn_together = effective.spawn_together;
        ::kraken::runtime::OnLoad(&register_lua_api);
        LOG_INFO("network API ready (autostart=%u enabled=%u)",
                 effective.autostart ? 1u : 0u, effective.enabled ? 1u : 0u);
        return;
    }

    const std::optional<SessionIdentity> production_identity =
        production_session_identity();
    if (!production_identity)
        return;
    SessionConfig session_config{};
    populate_session_identity(session_config, *production_identity);
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
    g_state.latest_weapon_aim_states.clear();
    g_state.latest_weapon_trigger_states.clear();
    g_state.latest_horn_states.clear();
    g_state.latest_terminal_deaths.clear();
    g_state.host_wreck_archives.clear();
    g_state.next_local_horn_transition = 1;
    g_state.local_horn_state = false;
    g_state.have_local_horn_state = false;
    g_state.weapon_aim_tracker.clear();
    g_state.confirmed_shot_deduplicator.clear();
    g_state.presentation_jip_reassembler.clear();
    g_state.pending_confirmed_shots.clear();
    g_state.presentation_state_revision = 1;
    g_state.next_combat_marker = Clock::time_point{};
    g_state.combat_authority_failure_emitted = false;
    g_state.client_blocked_fire_attempt_count = 0;
    g_state.client_original_fire_call_count = 0;
    g_state.client_blocked_damage_attempt_count = 0;
    g_state.client_original_damage_call_count = 0;
    g_state.has_local_confirmed_reload_state = false;
    g_state.local_entity_id = kInvalidNetId;
    g_state.next_input = Clock::now();
    g_state.next_loadout = Clock::now();
    g_state.next_input_sequence = 1;
    g_state.next_weapon_sequence = 1;
    g_state.local_weapon_gun = {};
    g_state.has_local_weapon_gun = false;
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
    if (g_state.combat_runtime == nullptr)
        g_state.combat_runtime = std::make_unique<RuntimeCombatBridge>();
    else
        g_state.combat_runtime->reset();
    g_state.combat_host_hooks_installed = false;
    g_state.combat_host_hooks_error_logged = false;
    g_state.session_identity = *production_identity;
    g_state.lan_advertising = false;
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

bool StartMatchmaking(const std::uint8_t required_players,
                      const char* const target_map, const char* const exit_map,
                      const std::int32_t wait_timeout_seconds,
                      const bool friendly_fire)
{
    return start_matchmaking_impl(required_players, target_map, exit_map,
                                  wait_timeout_seconds, friendly_fire);
}

bool AddSpawn(const float x, const float y, const float z, const float yaw,
              const std::int32_t belong)
{
    return add_match_spawn_impl(x, y, z, yaw, belong);
}

const char* GetSessionState()
{
    const MatchState state = g_state.is_host
        ? g_state.match.state() : g_state.visible_match_state;
    return to_string(state);
}

MatchStatus GetSessionStatus()
{
    if (g_state.is_host && g_state.match.state() != MatchState::Offline)
        return g_state.match.status(Clock::now());

    MatchStatus status{};
    status.state = g_state.visible_match_state;
    status.required_players = g_state.match_request.required_players;
    status.connected_players = static_cast<std::uint8_t>((std::min)(
        std::size_t{kMaxSessionPlayers},
        g_state.peers.size() + (IsSessionActive() ? std::size_t{1} : 0)));
    status.ready_players = status.state == MatchState::Offline
        ? 0 : (static_cast<std::uint8_t>(status.state) >=
               static_cast<std::uint8_t>(MatchState::Loading)
            ? status.connected_players : 0);
    status.infinite_wait = !g_state.match_request.wait_timeout.has_value();
    if (!status.infinite_wait && g_state.match_request_started != Clock::time_point{}) {
        const auto deadline = g_state.match_request_started +
                              *g_state.match_request.wait_timeout;
        const auto now = Clock::now();
        if (now < deadline)
            status.remaining_wait_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count());
    }
    return status;
}

void notify_lua_match_exit(const std::string& exit_map,
                           const SessionLeaveReason reason)
{
    queue_deferred_match_exit_route(exit_map, reason);
    hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
    hta::m3d::ScriptServer* const script_server =
        kernel ? kernel->m_scriptServer : nullptr;
    if (script_server == nullptr)
        return;
    const std::string script =
        "if MP_OnMatchMapTransition ~= nil then MP_OnMatchMapTransition(" +
        lua_string_literal(std::string{}) + "," +
        lua_string_literal(exit_map) + ") end";
    const hta::m3d::eScriptError error = script_server->execute(
        script.c_str(), "kraken_match_leave");
    if (error != hta::m3d::eScriptError::SUCCESS &&
        error != hta::m3d::eScriptError::NOT_INITIALIZED)
        LOG_ERROR("Lua match leave callback failed reason=%u code=%u",
                  static_cast<unsigned>(reason), static_cast<unsigned>(error));
    if (exit_map.empty())
        LOG_INFO("match leave reason=%u requested generic main-menu routing",
                 static_cast<unsigned>(reason));
}

bool send_match_leave_to_peers(const SessionLeaveReason reason,
                               const bool terminate_match)
{
    if (!g_state.session || g_state.match.state() == MatchState::Offline)
        return false;
    const MatchLeaveReason wire_reason = static_cast<MatchLeaveReason>(reason);
    std::vector<Byte> payload;
    if (!match_codec_succeeded(encode_match_leave({
            g_state.match_epoch == 0 ? 1u : g_state.match_epoch,
            wire_reason, terminate_match}, payload)))
        return false;
    bool sent = true;
    if (g_state.is_host) {
        for (const PeerId peer : g_state.peers)
            sent = send_match_payload(peer, MessageType::MatchLeave, payload) &&
                   sent;
    }
    else if (!g_state.peers.empty()) {
        sent = send_match_payload(g_state.peers.front(),
                                  MessageType::MatchLeave, payload);
    }
    return sent;
}

std::string current_match_exit_map()
{
    return g_state.is_host ? g_state.match.config().exit_map
                           : g_state.match_request.exit_map;
}

bool end_session_with_reason(const SessionLeaveReason reason)
{
    if (!g_state.session)
        return false;
    if (g_state.match.state() != MatchState::Offline) {
        // EndSession is a match-wide operation only on the listen host.  A
        // client-side EndSession is deliberately local; LeaveSession below
        // sends a non-terminating leave for that one client.
        if (g_state.is_host)
            (void)send_match_leave_to_peers(reason, true);
        if (g_state.is_host)
            (void)g_state.match.begin_leaving();
        g_state.visible_match_state = MatchState::Leaving;
        notify_lua_match_exit(current_match_exit_map(), reason);
    }
    return end_session_teardown();
}

bool LeaveSession(const SessionLeaveReason reason)
{
    if (!g_state.session)
        return false;
    if (g_state.match.state() != MatchState::Offline) {
        (void)send_match_leave_to_peers(reason, g_state.is_host);
        if (g_state.is_host)
            (void)g_state.match.begin_leaving();
        g_state.visible_match_state = MatchState::Leaving;
        notify_lua_match_exit(current_match_exit_map(), reason);
    }
    return end_session_teardown();
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

bool end_session_teardown()
{
    if (!g_state.session)
        return false;
    const world_authority::ScopedWorldExecutionContext teardown_scope(
        world_authority::WorldExecutionPhase::Teardown,
        g_state.is_host ? world_authority::RuntimeAuthority::Host
                        : world_authority::RuntimeAuthority::Replica);
    if (g_state.combat_runtime != nullptr)
        g_state.combat_runtime->reset();
    g_state.combat_host_hooks_installed = false;
    g_state.combat_host_hooks_error_logged = false;
    const bool preserve_client_scene =
        !g_state.is_host && g_state.session_end_preserve_client_scene;
    g_state.session->stop();
    g_state.session.reset();
    g_state.lan_discovery.stop();
    g_state.lan_advertising = false;
    g_state.session_identity = {};
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
    g_state.latest_weapon_aim_states.clear();
    g_state.latest_weapon_trigger_states.clear();
    g_state.latest_horn_states.clear();
    g_state.latest_terminal_deaths.clear();
    g_state.host_wreck_archives.clear();
    g_state.next_local_horn_transition = 1;
    g_state.local_horn_state = false;
    g_state.have_local_horn_state = false;
    g_state.loot_records.clear();
    g_state.loot_sources.clear();
    g_state.loot_receipts.clear();
    g_state.world_loot.clear();
    g_state.world_observer.reset();
    g_state.world_mutation_applier.reset();
    g_state.host_world_registry = HostWorldRegistry{};
    g_state.world_snapshot = {};
    g_state.world_snapshot_payload.clear();
    g_state.world_journal = WorldJournal{};
    g_state.world_transfer_peers.clear();
    g_state.static_world_index.clear();
    g_state.static_world_stability.reset();
    g_state.static_world_post_load_records.clear();
    g_state.static_world_ids_by_post_load_index.clear();
    g_state.static_world_original_bindings.clear();
    g_state.static_world_identity_stable = false;
    g_state.static_world_identity_last_error = StaticWorldIndexError::None;
    g_state.static_world_identity_error_map.clear();
    g_state.static_world_loaded_map.clear();
    g_state.static_world_dynamic_identity_logged = false;
    g_state.static_world_source_loaded = false;
    g_state.queued_world_mutations.clear();
    g_state.world_join_pending_deltas.clear();
    g_state.world_join_packets.clear();
    g_state.world_replay_depth = 0;
    g_state.world_source_context = ReplicationSourceContext::Teardown;
    g_state.auto_host_map.clear();
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
    g_state.client_leave_session_pending = false;
    g_state.session_end_preserve_client_scene = false;
    g_state.combat_autotest_armed = false;
    reset_match_state();
    LOG_INFO("session ended; local shelter state is untouched%s",
             preserve_client_scene ? " (client scene teardown deferred)" : "");
    notify_lua_session_state(false, false);
    return true;
}

bool EndSession()
{
    return end_session_with_reason(SessionLeaveReason::User);
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
    const std::optional<SessionIdentity> production_identity =
        production_session_identity();
    if (!production_identity)
        return false;
    if (effective.auto_lan) {
        // Binding a UDP port elects a host only within one operating system.
        // Different LAN machines can all bind 27016, so discover an existing
        // host first; only an unanswered broadcast creates a new listen server.
        const std::optional<LanDiscoveredSession> discovered =
            LanDiscovery::discover(kLanDiscoveryPort, kLanDiscoveryTimeout,
                                   *production_identity);
        if (discovered) {
            effective.host = false;
            effective.address = discovered->endpoint.host;
            effective.port = discovered->endpoint.port;
            LOG_INFO("LAN discovery found host=%s:%u",
                     effective.address.c_str(), effective.port);
        } else {
            // Simultaneous raid starts used to produce two hosts: both peers
            // completed their first broadcast timeout before either had
            // entered the server frame loop.  Let the lower IPv4 address lead
            // and then perform a short second discovery round.
            const auto delay = LanDiscovery::host_election_delay();
            Sleep(static_cast<DWORD>(delay.count()));
            const std::optional<LanDiscoveredSession> elected =
                LanDiscovery::discover(kLanDiscoveryPort,
                                       std::chrono::milliseconds(1000),
                                       *production_identity);
            if (elected) {
                effective.host = false;
                effective.address = elected->endpoint.host;
                effective.port = elected->endpoint.port;
                LOG_INFO("LAN election joined host=%s:%u after %lld ms",
                         effective.address.c_str(), effective.port,
                         static_cast<long long>(delay.count()));
            } else {
                // The election socket is metadata-aware even before a match
                // exists.  BeginSession immediately stops this provisional
                // advertisement; matchmaking publishes the real roster/map.
                LanSessionAdvertisement election{};
                election.game_port = effective.port;
                election.state = MatchState::Forming;
                election.join_policy = effective.join_policy;
                election.target_map = "kraken-pending-match";
                election.max_players = effective.match_max_players;
                election.identity = *production_identity;
                if (!g_state.lan_discovery.become_host(
                        kLanDiscoveryPort, election)) {
                    LOG_ERROR("LAN discovery could not find or host a LAN session");
                    return false;
                }
                effective.host = true;
                LOG_INFO("LAN discovery elected this peer as host port=%u delay=%lld ms",
                         effective.port, static_cast<long long>(delay.count()));
            }
        }
    }
    g_lifecycle_config->host = effective.host;
    g_lifecycle_config->address = effective.address;
    g_lifecycle_config->port = effective.port;
    SessionConfig session_config{};
    populate_session_identity(session_config, *production_identity);
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
    g_state.session_identity = *production_identity;
    g_state.lan_advertising = false;
    g_state.spawn_together = effective.spawn_together;
    g_state.auto_host_coop = effective.auto_host_coop;
    reset_match_state();
    g_state.match_request.max_players = effective.match_max_players;
    g_state.match_request.join_policy = effective.join_policy;
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
    g_state.world_journal = WorldJournal{};
    g_state.world_observer = std::make_unique<WorldObserver>(
        [](const WorldMutationEvent& event) {
            if (!g_state.is_host || g_state.world_replay_depth != 0 ||
                !replication_source_emits_delta(g_state.world_source_context))
                return;
            std::vector<Byte> payload;
            if (!world_mutation_codec_succeeded(
                    encode_world_mutation(event, payload)))
                return;
            const std::optional<WorldDelta> delta =
                g_state.world_journal.try_append(std::move(payload));
            if (delta) {
                LOG_INFO("KRAKEN_MP_ACCEPT mutation kind=%s object=%llu revision=%llu",
                         diagnostic_mutation_kind(event),
                         static_cast<unsigned long long>(
                             diagnostic_mutation_object(event)),
                          static_cast<unsigned long long>(delta->revision));
            }
        });
    WorldMutationEngineCallbacks world_callbacks{};
    world_callbacks.create = apply_world_object_created;
    world_callbacks.despawn = apply_world_object_despawned;
    world_callbacks.add_child = apply_world_parent_added;
    world_callbacks.remove_child = apply_world_parent_removed;
    world_callbacks.runtime = apply_world_runtime_changed;
    world_callbacks.property = apply_world_property_changed;
    world_callbacks.damage = apply_world_damage;
    world_callbacks.destroyed = apply_world_destroyed;
    world_callbacks.fx = apply_world_fx;
    WorldMutationApplierConfig world_applier_config{};
    world_applier_config.epoch = g_state.world_journal.epoch();
    world_applier_config.initial_revision = g_state.world_journal.revision();
    world_applier_config.expected_source_role = TransportRole::Server;
    world_applier_config.expected_source = ReplicationSourceContext::NetworkReplay;
    world_applier_config.replay_guard_factory = [] {
        return g_state.world_observer
            ? g_state.world_observer->suppress_replay() : ReplayGuard{};
    };
    g_state.world_mutation_applier = std::make_unique<WorldMutationApplier>(
        std::move(world_callbacks), *g_state.world_observer,
        std::move(world_applier_config));
    g_state.world_snapshot = {};
    g_state.world_snapshot_payload.clear();
    g_state.host_world_registry = HostWorldRegistry{};
    g_state.world_source_context = ReplicationSourceContext::MapLoad;
    g_state.world_replay_depth = 0;
    g_state.world_transfer_peers.clear();
    g_state.static_world_index.clear();
    g_state.static_world_stability.reset();
    g_state.static_world_post_load_records.clear();
    g_state.static_world_ids_by_post_load_index.clear();
    g_state.static_world_original_bindings.clear();
    g_state.static_world_identity_stable = false;
    g_state.static_world_identity_last_error = StaticWorldIndexError::None;
    g_state.static_world_identity_error_map.clear();
    g_state.static_world_loaded_map.clear();
    g_state.static_world_dynamic_identity_logged = false;
    g_state.static_world_source_loaded = false;
    g_state.auto_host_map.clear();
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
    g_state.latest_weapon_aim_states.clear();
    g_state.latest_weapon_trigger_states.clear();
    g_state.latest_horn_states.clear();
    g_state.latest_terminal_deaths.clear();
    g_state.host_wreck_archives.clear();
    g_state.next_local_horn_transition = 1;
    g_state.local_horn_state = false;
    g_state.have_local_horn_state = false;
    g_state.weapon_aim_tracker.clear();
    g_state.confirmed_shot_deduplicator.clear();
    g_state.presentation_jip_reassembler.clear();
    g_state.pending_confirmed_shots.clear();
    g_state.presentation_state_revision = 1;
    g_state.next_combat_marker = Clock::time_point{};
    g_state.combat_authority_failure_emitted = false;
    g_state.client_blocked_fire_attempt_count = 0;
    g_state.client_original_fire_call_count = 0;
    g_state.client_blocked_damage_attempt_count = 0;
    g_state.client_original_damage_call_count = 0;
    g_state.has_local_confirmed_reload_state = false;
    g_state.local_weapon_target_obj_id = kInvalidObjId;
    g_state.local_weapon_target_entity_id = kInvalidNetId;
    g_state.host_defeat_session_end_pending = false;
    g_state.session_end_preserve_client_scene = false;
    g_state.combat_autotest_armed = false;
    g_state.next_combat_host_survival = Clock::now();
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
        address[0] == '\0' || port < 1024 || max_peers < 2 ||
        max_peers > kMaxSessionPlayers)
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
    command.session_epoch = g_state.session_epoch;
    command.entity_id = g_state.local_entity_id;
    if (!g_state.entities.lookup_generation(command.entity_id,
                                            command.entity_generation))
        return false;
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
        if (!capture_unique_weapon_identity(*vehicle, gun_id, command.gun))
            return false;
        g_state.local_weapon_gun = command.gun;
        g_state.has_local_weapon_gun = true;
        if (command.target_entity_id != kInvalidNetId &&
            !g_state.entities.lookup_generation(command.target_entity_id,
                                                command.target_generation))
            return false;
        if (!capture_weapon_aim_point(*vehicle, command.aim_point))
            LOG_ERROR("local weapon aim capture failed entity=%u",
                      command.entity_id);
        else
            command.has_aim_point = true;
    } else {
        LOG_ERROR("local weapon aim capture has no player vehicle entity=%u",
                  command.entity_id);
    }
    return send_weapon_intent(command);
}

bool submit_native_weapon_intent(hta::ai::Vehicle* const vehicle,
                                 bool trigger_held, const ObjId target_obj_id,
                                 const std::int32_t gun_id,
                                 const VehicleVector3* aim_override,
                                 float aim_speed,
                                 const GunAttachmentIdentity* const gun_override)
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
    command.session_epoch = g_state.session_epoch;
    command.entity_id = g_state.local_entity_id;
    if (!g_state.entities.lookup_generation(command.entity_id,
                                            command.entity_generation))
        return false;
    command.sequence = g_state.next_weapon_sequence++;
    command.shot_id = command.sequence;
    command.client_tick = g_state.server_tick;
    command.gun_id = gun_id;
    if (gun_override != nullptr)
        command.gun = *gun_override;
    else if (!capture_unique_weapon_identity(*current_vehicle, gun_id,
                                             command.gun))
        return false;
    command.trigger_held = trigger_held;
    command.target_entity_id = target_entity_id;
    if (target_entity_id != kInvalidNetId &&
        !g_state.entities.lookup_generation(target_entity_id,
                                            command.target_generation))
        return false;
    command.aim_speed = aim_speed;
    if (aim_override != nullptr && valid_weapon_aim_point(*aim_override)) {
        command.aim_point = *aim_override;
        command.has_aim_point = true;
    } else if (!capture_weapon_aim_point(*current_vehicle, command.aim_point))
        LOG_ERROR("native weapon aim capture failed entity=%u shot=%u",
                  command.entity_id, command.shot_id);
    else
        command.has_aim_point = true;
    if (!send_weapon_intent(command))
        return false;
    LOG_INFO("native weapon intent entity=%u sequence=%u gun_attachment=%llu target=%u aim=%u",
             command.entity_id, command.sequence,
             static_cast<unsigned long long>(command.gun.attachment_id),
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
