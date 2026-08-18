#ifndef KRAKEN_NET_COMBAT_RUNTIME_HPP
#define KRAKEN_NET_COMBAT_RUNTIME_HPP

#include "net/combat_presentation.hpp"
#include "net/entity_registry.hpp"
#include "net/native_object_archive.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hta::ai {
struct Shell;
struct PhysicBody;
}
struct dContact;

namespace kraken::net::combat_runtime {

// These addresses are executable addresses for the Win32 EFA image.  They
// are kept here, next to the adapter contract, so a native binding cannot
// silently drift to a guessed local address or a wire-visible object id.
inline constexpr std::uintptr_t kProcessShellAndBodyRva = 0x00207200u;
inline constexpr std::uintptr_t kProcessShellAndBodyVa = 0x00607200u;
inline constexpr std::uintptr_t kDamageCallSiteVa = 0x00607A9Du;
inline constexpr std::uintptr_t kEffectSelectorCallSiteVa = 0x00607E0Eu;
inline constexpr std::uintptr_t kTerrainEffectCallSiteVa = 0x00607E19u;
inline constexpr std::uintptr_t kLandscapeEffectCallSiteVa = 0x00607E19u;
inline constexpr std::uintptr_t kVehiclePartEffectSelectorCallSiteVa =
    0x00607E60u;
inline constexpr std::uintptr_t kRoadEffectCallSiteVa = 0x00607F22u;
inline constexpr std::uintptr_t kStaticsEffectCallSiteVa = 0x00607F4Bu;
inline constexpr std::uintptr_t kVehicleEffectCallSiteVa = 0x00607F74u;
inline constexpr std::uintptr_t kCreateEffectNodeCallSiteVa = 0x00607FB8u;
inline constexpr std::uintptr_t kShellEffectNameVa = 0x00605370u;
inline constexpr std::uintptr_t kShellRoadEffectNameVa = 0x006053E0u;
inline constexpr std::uintptr_t kShellStaticsEffectNameVa = 0x00605470u;
inline constexpr std::uintptr_t kShellVehicleEffectNameVa = 0x006054B0u;
inline constexpr std::uintptr_t kVehiclePartAddDecalCallSiteVa = 0x006D806Eu;
inline constexpr std::uintptr_t kCreateEffectNodeVa = 0x00617450u;
inline constexpr std::uintptr_t kVehiclePartAddDecalVa = 0x006D6B80u;
inline constexpr std::uintptr_t kCreateNodeVa = 0x006173B0u;
inline constexpr std::uintptr_t kSoundSetPropertyVa = 0x00661650u;
inline constexpr std::uintptr_t kSgNodeAddChildVa = 0x005B8730u;
inline constexpr std::uintptr_t kSgNodeRemoveChildVa = 0x005B8750u;
inline constexpr std::uintptr_t kVehicleHealthVa = 0x005CBE00u;
inline constexpr std::uintptr_t kVehicleGetHealthVa = 0x005D0BC0u;
inline constexpr std::uintptr_t kVehicleGetMaxHealthVa = 0x005D0C00u;
inline constexpr std::uintptr_t kPhysicBodyGetBaseClassVa = 0x00616490u;
inline constexpr std::uintptr_t kPhysicBodyGetOwnerVa = 0x006165B0u;
inline constexpr std::uintptr_t kWheelGetVehicleVa = 0x005EDE10u;
inline constexpr std::uintptr_t kVehiclePartGetOwnerCompoundVa = 0x006CDC70u;
inline constexpr std::uintptr_t kVehiclePartGetOwnerCompoundConstVa =
    0x006CDC80u;
inline constexpr std::uintptr_t kVehicleGetChassisVa = 0x005CB9A0u;
inline constexpr std::uintptr_t kComplexPhysicObjGetPartByNameVa = 0x006BF720u;
inline constexpr std::uintptr_t kNumericSetUnsafeVa = 0x0044CD10u;
inline constexpr std::uintptr_t kObjContainerAddRemoveVa = 0x0068E350u;
inline constexpr std::uintptr_t kVehicleUpdateVa = 0x005EC0D0u;
inline constexpr std::uintptr_t kVehicleEvaluateToDeadCallSiteVa = 0x005EC1D9u;
inline constexpr std::uintptr_t kVehicleEvaluateToDeadVa = 0x005E88E0u;
inline constexpr std::uintptr_t kObjContainerCreateSuspendedVa = 0x006334C0u;
inline constexpr std::uintptr_t kObjContainerAddNotUpdateVa = 0x006584B0u;
inline constexpr std::uintptr_t kObjPostLoadVa = 0x00689560u;
inline constexpr std::uintptr_t kObjCreateVisualPartVa = 0x006894D0u;
inline constexpr std::uintptr_t kVehicleDisablePhysicsVa = 0x005DA820u;
// Vehicle::_InternalPostLoad posts only these verified radio/event messages at
// the three direct callsites below. Replica wreck materialization suppresses
// them through a scoped callsite adapter; the original target is retained for
// all ordinary native loads.
inline constexpr std::uintptr_t kVehiclePostMessage46CallSiteVa = 0x005E875Eu;
inline constexpr std::uintptr_t kVehiclePostMessage47CallSiteVa = 0x005E87E9u;
inline constexpr std::uintptr_t kVehiclePostMessage45CallSiteVa = 0x005E8874u;
inline constexpr std::uintptr_t kProcessManagerPostMessageVa = 0x006A3560u;

inline constexpr std::uint32_t kObjDeadFlag = 0x08u;
inline constexpr std::uint32_t kHornLoopProperty = 0x2600u;

struct CallsiteExpectation {
    std::uintptr_t call_site = 0;
    std::uintptr_t original_target = 0;
};

using CallsiteBytesReader = bool (*)(std::uintptr_t, std::uint8_t&,
                                     std::int32_t&);

[[nodiscard]] bool preflight_rel32_call(const CallsiteExpectation&,
                                         CallsiteBytesReader);

#if defined(_MSC_VER)
#define KRAKEN_COMBAT_RUNTIME_FASTCALL __fastcall
#else
#define KRAKEN_COMBAT_RUNTIME_FASTCALL
#endif

enum class RuntimeApplyResult : std::uint8_t {
    Applied,
    Duplicate,
    Stale,
    InvalidEvent,
    InvalidIdentity,
    InvalidCue,
    NotReplica,
    InvalidHealth,
    NativeRejected,
    ResolverRejected,
    PendingRemoval,
};

[[nodiscard]] constexpr bool runtime_apply_succeeded(
    const RuntimeApplyResult result) noexcept
{
    return result == RuntimeApplyResult::Applied ||
           result == RuntimeApplyResult::Duplicate ||
           result == RuntimeApplyResult::PendingRemoval;
}

struct ReplicaBinding {
    NetEntityRef identity{};
    std::uint32_t class_id = 0;
    std::uint32_t chassis_id = 0;
    void* native_vehicle = nullptr;
    // This is deliberately local to the resolver. It is never serialized.
    std::int32_t local_object_id = 0;
    bool replica_authority = false;
    bool inert = false;
    std::int32_t prototype_id = -1;
    // Native prototype identity resolved locally; never serialized.
    std::string prototype_name{};
};

enum class WreckMaterializationResult : std::uint8_t {
    Committed,
    InvalidArchive,
    InvalidIdentity,
    MissingOperation,
    CreationRejected,
    ResourceRejected,
    StructureRejected,
    PostLoadRejected,
    VisualRejected,
    PoseRejected,
    PhysicsRejected,
    SimulationRejected,
    ValidationRejected,
    BindingRejected,
    SourceRetireRejected,
    AllocationFailure,
};

// Every callback below is a narrow lifecycle operation. None of these
// callbacks may dispatch gameplay/death/runtime restoration; the materializer
// owns the order and commits the source swap only after the final validation.
struct WreckMaterializerOperations {
    struct Transaction {
        void* root = nullptr;
        // Creation order is registration order. Cleanup always queues the
        // reverse list so descendants are removed before their parent.
        std::vector<ObjId> created_object_ids{};
        std::vector<ObjId> preexisting_object_ids{};
    };

    std::function<void*(const NativeObjectArchiveV2&, const ReplicaBinding&)>
        create_suspended;
    // The native container may register wheels/cargo/trailer during
    // LoadFromXML/PostLoad. The adapter snapshots those registrations into
    // this transaction after every stage; root-only destruction is forbidden.
    std::function<void*(const NativeObjectArchiveV2&, const ReplicaBinding&,
                        Transaction&)> create_suspended_transaction;
    std::function<void(void*, Transaction&)> collect_created_objects;
    std::function<bool(void*, const NativeObjectArchiveV2&,
                       const ReplicaBinding&)> validate_resources;
    std::function<bool(void*, ByteView)> load_structure;
    std::function<bool(void*)> post_load_graph;
    std::function<bool(void*)> create_visual_part;
    std::function<bool(void*, const NativeObjectArchiveVisualRuntime&)>
        apply_visual_runtime;
    std::function<bool(void*, const CombatPose&)> apply_authoritative_pose;
    std::function<bool(void*)> disable_physics;
    std::function<bool(void*)> remove_from_simulation;
    std::function<bool(void*)> validate_wreck;
    std::function<bool(const NetEntityRef&, void*)> bind_wreck;
    std::function<bool(const NetEntityRef&)> retire_source;
    std::function<void(const NetEntityRef&, void*)> unbind_wreck;
    std::function<void(Transaction&)> destroy_transaction;
    // Kept only as a source-compatible test adapter. Production materializers
    // never use this root-only callback.
    std::function<void(void*)> destroy_partial;

    [[nodiscard]] bool ready(bool has_visual_runtime) const noexcept;
};

class TransactionalWreckMaterializer final {
public:
    explicit TransactionalWreckMaterializer(WreckMaterializerOperations operations);

    [[nodiscard]] WreckMaterializationResult materialize(
        const NativeObjectArchiveV2&, const NetEntityRef& source_identity,
        const NetEntityRef& wreck_identity, const ReplicaBinding& source,
        const CombatPose& authoritative_pose) const noexcept;

private:
    WreckMaterializerOperations m_operations;
};

using NativeObjectArchiveMaterializer = TransactionalWreckMaterializer;

class ReplicaResolver {
public:
    virtual ~ReplicaResolver() = default;

    [[nodiscard]] virtual bool resolve(const NetEntityRef& identity,
                                       ReplicaBinding& output) = 0;
    [[nodiscard]] virtual bool retire(const NetEntityRef& identity) = 0;
};

struct ImpactGeometry {
    AttachmentIdentity target_part{};
    SurfaceKind surface = SurfaceKind::Unknown;
    VehicleVector3 hit_position{};
    VehicleVector3 effect_position{};
    VehicleVector3 incoming_direction{};
    VehicleVector3 contact_normal{0.0f, 1.0f, 0.0f};
    VehicleVector3 decal_tangent{};
    bool has_incoming_direction = false;
    bool has_decal_tangent = false;
    MeshIdentity mesh_id = 0;
    std::uint32_t material_id = 0;
    VehicleQuaternion effect_rotation{};
    float effect_scale = 1.0f;
    bool remove_if_free = false;
};

// Native operations are an ABI adapter, not gameplay logic.  The production
// implementation binds these operations to the verified Win32 seams above;
// tests provide fakes. In particular, set_health_unsafe must call
// Numeric<float>::SetUnsafe (0x44CD10), and set_dead_masked must change only
// Obj::m_flags+0x40 bit 0x08. No callback-bearing Numeric::set is permitted.
struct ReplicaNativeOperations {
    std::function<float(void*)> get_health;
    std::function<float(void*)> get_max_health;
    std::function<bool(void*, float)> set_health_unsafe;
    std::function<std::uint32_t(void*)> read_flags;
    std::function<bool(void*, std::uint32_t, std::uint32_t)> set_flags_masked;

    // These methods must use PhysicBody::CreateEffectNode and
    // VehiclePart::_AddDecal respectively. The presentation object carries
    // typed geometry/cues; no numeric engine handle crosses this seam.
    std::function<bool(void*, const ImpactPresentation&,
                       const ImpactGeometry&)> create_effect_node;
    std::function<bool(void*, const ImpactPresentation&,
                       const ImpactGeometry&)> add_decal;
    std::function<bool(const ImpactPresentation&,
                       const ImpactGeometry&)> create_static_effect;
    std::function<bool(const ImpactPresentation&,
                       const ImpactGeometry&)> add_static_decal;

    // Removal is called only from process_removals(true), the pre-sim
    // boundary. A production binding uses ObjContainer::AddObjIdToRemove.
    std::function<bool(void*)> schedule_removal;
    std::function<bool(void*)> has_disappeared;

    [[nodiscard]] bool health_projection_ready() const noexcept;
    [[nodiscard]] bool presentation_ready() const noexcept;
    [[nodiscard]] bool removal_ready() const noexcept;
};

class CanonicalCueResolver final {
public:
    using Resolve = std::function<bool(std::string_view, ResourceCue&)>;

    CanonicalCueResolver() = default;
    explicit CanonicalCueResolver(Resolve resolve);

    [[nodiscard]] bool resolve(std::string_view native_name,
                               ResourceCue& output) const noexcept;

private:
    Resolve m_resolve;
};

struct HostImpactObservation {
    std::uint32_t session_epoch = 0;
    CombatEventId event_id = 0;
    std::uint32_t server_tick = 0;
    CombatEventId shot_id = 0;
    NetEntityRef shooter{};
    GunAttachmentIdentity gun{};
    ImpactTargetIdentity target{ImpactTargetKind::Environment,
                                {}, {}, EnvironmentKind::UnboundStatic};
    ImpactGeometry geometry{};
    bool target_captured = false;
    bool contact_captured = false;

    // These are set only by the actual native branches. A selected name is
    // not enough: the final CreateEffectNode/_AddDecal seam must confirm that
    // the node/decal was produced.
    bool effect_produced = false;
    bool decal_produced = false;
    std::string effect_name{};
    std::string decal_name{};
    bool did_damage = false;
    ImpactBlockedReason blocked_reason = ImpactBlockedReason::Unknown;
    DamageResult damage{};
};

struct HostImpactPublication {
    std::function<bool(const ImpactPresentation&)> publish_impact;
    std::function<bool(const DamageResult&)> publish_damage;
};

class HostImpactCaptureScope final {
public:
    explicit HostImpactCaptureScope(HostImpactObservation& observation) noexcept;
    ~HostImpactCaptureScope();

    HostImpactCaptureScope(const HostImpactCaptureScope&) = delete;
    HostImpactCaptureScope& operator=(const HostImpactCaptureScope&) = delete;

    [[nodiscard]] bool outermost() const noexcept { return m_outermost; }

private:
    HostImpactObservation* m_previous = nullptr;
    bool m_outermost = false;
};

[[nodiscard]] HostImpactObservation* current_host_impact_capture() noexcept;
void record_effect_branch(std::string_view native_name) noexcept;
void record_decal_branch(std::string_view native_name) noexcept;
void record_environment_branch(EnvironmentKind kind) noexcept;
void record_effect_geometry(const VehicleVector3& position,
                            const VehicleQuaternion& rotation,
                            bool remove_if_free, float scale) noexcept;
void record_decal_geometry(const VehicleVector3& position,
                           const VehicleVector3& contact_normal,
                           const VehicleVector3& tangent, MeshIdentity mesh_id,
                           const AttachmentIdentity& part) noexcept;
void record_effect_produced() noexcept;
void record_decal_produced() noexcept;
void record_authoritative_damage(const DamageResult&) noexcept;

class HostImpactCapture final {
public:
    explicit HostImpactCapture(CanonicalCueResolver resolver = {});

    [[nodiscard]] RuntimeApplyResult publish(
        const HostImpactObservation&, const HostImpactPublication&) const;

private:
    CanonicalCueResolver m_resolver;
};

// The host hook is kept as one narrow seam around exactly one native call.
// Hook installation is supplied by the runtime's patch layer so the pure
// adapter remains testable and cannot invent a trampoline or a call target.
using ProcessShellAndBodyFunction = int(KRAKEN_COMBAT_RUNTIME_FASTCALL*)(
    hta::ai::Shell*, hta::ai::PhysicBody*, dContact*, std::uint32_t*, bool);

struct HostProcessHookInstaller {
    std::function<bool(std::uintptr_t, ProcessShellAndBodyFunction,
                       ProcessShellAndBodyFunction&)> install;
};

struct HostProcessHookCallbacks {
    std::function<void(hta::ai::Shell*, hta::ai::PhysicBody*, dContact*,
                       std::uint32_t*, bool reverse,
                       HostImpactObservation&)> initialize;
    std::function<void(const HostImpactObservation&)> complete;
};

class HostProcessShellAndBodyHook final {
public:
    [[nodiscard]] bool install(const HostProcessHookInstaller&,
                               ProcessShellAndBodyFunction replacement);
    [[nodiscard]] bool installed() const noexcept { return m_installed; }
    [[nodiscard]] ProcessShellAndBodyFunction original() const noexcept
    {
        return m_original;
    }
    void set_callbacks(HostProcessHookCallbacks callbacks);
    [[nodiscard]] int invoke(hta::ai::Shell*, hta::ai::PhysicBody*, dContact*,
                             std::uint32_t*, bool reverse) const;

private:
    ProcessShellAndBodyFunction m_original = nullptr;
    HostProcessHookCallbacks m_callbacks{};
    bool m_installed = false;
};

int KRAKEN_COMBAT_RUNTIME_FASTCALL process_shell_and_body_capture_hook(
    hta::ai::Shell*, hta::ai::PhysicBody*, dContact*, std::uint32_t*,
    bool reverse);

struct HornNodeOperations {
    std::function<void*(void*, const ResourceCue&)> create_node;
    std::function<bool(void*, std::uint32_t, bool)> set_property;
    std::function<bool(void*, void*)> add_child;
    std::function<bool(void*, void*)> remove_child;
    std::function<void(void*)> release_node;

    [[nodiscard]] bool ready() const noexcept;
};

class HornSidecar final {
public:
    explicit HornSidecar(HornNodeOperations operations = {});

    [[nodiscard]] RuntimeApplyResult apply(const HornState&,
                                           ReplicaResolver&);
    void cleanup(const NetEntityRef&, ReplicaResolver&) noexcept;
    void reset(ReplicaResolver&) noexcept;

private:
    struct Entry {
        NetEntityRef identity{};
        CombatTransitionId transition_id = 0;
        void* vehicle = nullptr;
        void* node = nullptr;
        bool active = false;
    };

    [[nodiscard]] Entry* find(const NetEntityRef&) noexcept;
    [[nodiscard]] const Entry* find(const NetEntityRef&) const noexcept;
    [[nodiscard]] static bool same_identity(const NetEntityRef&,
                                             const NetEntityRef&) noexcept;
    void remove_entry(Entry&) noexcept;

    HornNodeOperations m_operations;
    std::vector<Entry> m_entries;
    std::uint32_t m_epoch = 0;
    bool m_have_epoch = false;
};

struct WreckResolution {
    bool inert_replacement_ready = false;
    // A resolver may report readiness only after the archive transfer cache
    // has verified the complete digest and bound this exact wreck identity.
    bool archive_verified = false;
    NativeObjectArchiveDigest archive_digest = 0;
    NetEntityRef wreck_identity{};
    // True when the materializer has already queued the source ObjId for
    // removal after binding the inert wreck. This prevents the runtime
    // removal boundary from queueing the same source twice.
    bool source_removal_scheduled = false;
};

using WreckResolver = std::function<bool(const DeathWreckPresentation&,
                                         const ReplicaBinding&,
                                         WreckResolution&)>;

class ReplicaCombatRuntime final {
public:
    ReplicaCombatRuntime(ReplicaResolver& resolver,
                         ReplicaNativeOperations native,
                         HornNodeOperations horn = {},
                         WreckResolver wreck_resolver = {});

    [[nodiscard]] RuntimeApplyResult apply_impact(
        const ImpactPresentation&);
    [[nodiscard]] RuntimeApplyResult apply_damage(
        const DamageResult&);
    [[nodiscard]] RuntimeApplyResult apply_death(
        const DeathWreckPresentation&);
    [[nodiscard]] RuntimeApplyResult apply_horn(const HornState&);
    [[nodiscard]] RuntimeApplyResult apply_jip(const PresentationJipState&);

    [[nodiscard]] bool accept_wreck_archive_chunk(ByteView,
                                                   std::uint64_t now_ms);

    // Removal scheduling is deliberately a separate boundary. `true` means
    // the caller is at pre-simulation; false only leaves a pending request.
    [[nodiscard]] RuntimeApplyResult process_removals(bool pre_sim_boundary);
    void despawn(const NetEntityRef&) noexcept;
    void reset() noexcept;

private:
    struct PendingRemoval {
        DeathWreckPresentation death{};
        ReplicaBinding binding{};
        bool wreck_ready = false;
        bool scheduled = false;
        bool source_removal_scheduled = false;
        bool transition_accepted = false;
    };

    [[nodiscard]] RuntimeApplyResult begin_epoch(std::uint32_t) noexcept;
    [[nodiscard]] RuntimeApplyResult resolve_replica(
        const NetEntityRef&, ReplicaBinding&) const;
    [[nodiscard]] RuntimeApplyResult mark_dead(ReplicaBinding&) const;
    [[nodiscard]] bool accept_event(std::uint32_t, CombatEventId,
                                    CombatEventId&, bool&) noexcept;
    [[nodiscard]] bool accept_transition(std::uint32_t,
                                          CombatTransitionId,
                                          CombatTransitionId&) noexcept;
    [[nodiscard]] bool seen_impact(CombatEventId) const noexcept;
    void remember_impact(CombatEventId);
    ReplicaResolver& m_resolver;
    ReplicaNativeOperations m_native;
    HornSidecar m_horn;
    WreckResolver m_wreck_resolver;
    std::vector<CombatEventId> m_impact_events;
    std::vector<CombatEventId> m_damage_events;
    std::vector<CombatEventId> m_damage_impacts;
    std::vector<PendingRemoval> m_pending_removals;
    NativeObjectArchiveReassembler m_wreck_archives;
    std::uint32_t m_epoch = 0;
    bool m_have_epoch = false;
    CombatEventId m_last_impact_event = 0;
    bool m_have_impact_event = false;
    CombatEventId m_last_damage_event = 0;
    CombatEventId m_last_death_event = 0;
    CombatTransitionId m_last_horn_transition = 0;
};

#undef KRAKEN_COMBAT_RUNTIME_FASTCALL

} // namespace kraken::net::combat_runtime

#endif
