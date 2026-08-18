#include "net/combat_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#if defined(_MSC_VER)
#define KRAKEN_COMBAT_RUNTIME_FASTCALL __fastcall
#else
#define KRAKEN_COMBAT_RUNTIME_FASTCALL
#endif

namespace kraken::net::combat_runtime {
namespace {

thread_local HostImpactObservation* g_impact_capture = nullptr;
HostProcessShellAndBodyHook* g_process_hook = nullptr;

[[nodiscard]] bool valid_identity(const NetEntityRef& value) noexcept
{
    return value.net_id != kInvalidNetId &&
           value.generation != kInvalidEntityGeneration;
}

[[nodiscard]] bool same_identity(const NetEntityRef& left,
                                 const NetEntityRef& right) noexcept
{
    return left.net_id == right.net_id &&
           left.generation == right.generation;
}

[[nodiscard]] bool newer(const CombatEventId previous,
                         const CombatEventId candidate) noexcept
{
    const CombatEventId delta = candidate - previous;
    return candidate != previous && delta < 0x8000000000000000ull;
}

[[nodiscard]] bool newer_transition(const CombatTransitionId previous,
                                    const CombatTransitionId candidate) noexcept
{
    const CombatTransitionId delta = candidate - previous;
    return candidate != previous && delta < 0x8000000000000000ull;
}

[[nodiscard]] bool finite_in_range(const float value, const float minimum,
                                   const float maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] bool cue_present(const ResourceCue& cue) noexcept
{
    return !cue.empty() &&
           validate_resource_cue(cue, false) ==
               CombatPresentationCodecError::None;
}

[[nodiscard]] bool epoch_is_newer(const std::uint32_t previous,
                                  const std::uint32_t candidate) noexcept
{
    const std::uint32_t delta = candidate - previous;
    return candidate != previous && delta < 0x80000000u;
}

} // namespace

bool preflight_rel32_call(const CallsiteExpectation& expectation,
                          const CallsiteBytesReader reader)
{
    if (expectation.call_site == 0 || expectation.original_target == 0 ||
        reader == nullptr)
        return false;
    std::uint8_t opcode = 0;
    std::int32_t displacement = 0;
    if (!reader(expectation.call_site, opcode, displacement) || opcode != 0xE8u)
        return false;
    const std::int64_t decoded_target =
        static_cast<std::int64_t>(expectation.call_site) + 5 + displacement;
    return decoded_target == static_cast<std::int64_t>(
                                 expectation.original_target);
}

bool ReplicaNativeOperations::health_projection_ready() const noexcept
{
    return static_cast<bool>(get_health) &&
           static_cast<bool>(get_max_health) &&
           static_cast<bool>(set_health_unsafe) &&
           static_cast<bool>(read_flags) &&
           static_cast<bool>(set_flags_masked);
}

bool ReplicaNativeOperations::presentation_ready() const noexcept
{
    return static_cast<bool>(create_effect_node) ||
           static_cast<bool>(add_decal) ||
           static_cast<bool>(create_static_effect) ||
           static_cast<bool>(add_static_decal);
}

bool ReplicaNativeOperations::removal_ready() const noexcept
{
    return static_cast<bool>(schedule_removal) &&
           static_cast<bool>(has_disappeared);
}

bool WreckMaterializerOperations::ready(const bool has_visual_runtime) const noexcept
{
    const bool transaction_creation = static_cast<bool>(
        create_suspended_transaction) && static_cast<bool>(collect_created_objects) &&
        static_cast<bool>(destroy_transaction);
    const bool legacy_creation = static_cast<bool>(create_suspended) &&
        static_cast<bool>(destroy_partial);
    return (transaction_creation || legacy_creation) &&
           static_cast<bool>(validate_resources) &&
           static_cast<bool>(load_structure) &&
           static_cast<bool>(post_load_graph) &&
           static_cast<bool>(create_visual_part) &&
           (!has_visual_runtime || static_cast<bool>(apply_visual_runtime)) &&
           static_cast<bool>(apply_authoritative_pose) &&
           static_cast<bool>(disable_physics) &&
           static_cast<bool>(remove_from_simulation) &&
           static_cast<bool>(validate_wreck) &&
           static_cast<bool>(bind_wreck) &&
           static_cast<bool>(retire_source) &&
           static_cast<bool>(unbind_wreck);
}

TransactionalWreckMaterializer::TransactionalWreckMaterializer(
    WreckMaterializerOperations operations)
    : m_operations(std::move(operations))
{
}

WreckMaterializationResult TransactionalWreckMaterializer::materialize(
    const NativeObjectArchiveV2& archive, const NetEntityRef& source_identity,
    const NetEntityRef& wreck_identity, const ReplicaBinding& source,
    const CombatPose& authoritative_pose) const noexcept
{
    NativeObjectArchiveV2 verified_archive{};
    try {
        if (archive.digest == 0 ||
            validate_native_object_archive_v2(archive) !=
                NativeObjectArchiveErrorCode::None)
            return WreckMaterializationResult::InvalidArchive;
        std::vector<Byte> encoded_archive;
        if (encode_native_object_archive_v2(archive, encoded_archive) !=
                NativeObjectArchiveErrorCode::None ||
            decode_native_object_archive_v2(ByteView{encoded_archive},
                                            verified_archive) !=
                NativeObjectArchiveErrorCode::None ||
            verified_archive.digest != archive.digest)
            return WreckMaterializationResult::InvalidArchive;
    }
    catch (const std::bad_alloc&) {
        return WreckMaterializationResult::AllocationFailure;
    }
    if (!valid_identity(source_identity) || !valid_identity(wreck_identity) ||
        same_identity(source_identity, wreck_identity) ||
        !same_identity(source.identity, source_identity) ||
        source.native_vehicle == nullptr || !source.replica_authority ||
        !source.inert)
        return WreckMaterializationResult::InvalidIdentity;
    if (!m_operations.ready(!verified_archive.visual_runtime.empty()))
        return WreckMaterializationResult::MissingOperation;

    WreckMaterializerOperations::Transaction transaction{};
    void* created = nullptr;
    bool bound = false;
    const auto cleanup = [&]() noexcept {
        try {
            if (bound)
                m_operations.unbind_wreck(wreck_identity, created);
        }
        catch (...) {
        }
        try {
            if (m_operations.destroy_transaction)
                m_operations.destroy_transaction(transaction);
            else if (created != nullptr && m_operations.destroy_partial)
                m_operations.destroy_partial(created);
        }
        catch (...) {
        }
    };
    const auto reject = [&](const WreckMaterializationResult value) noexcept {
        cleanup();
        return value;
    };

    try {
        if (m_operations.create_suspended_transaction)
            created = m_operations.create_suspended_transaction(
                verified_archive, source, transaction);
        else
            created = m_operations.create_suspended(verified_archive, source);
        transaction.root = created;
        if (created == nullptr)
            return reject(WreckMaterializationResult::CreationRejected);
        if (m_operations.collect_created_objects)
            m_operations.collect_created_objects(created, transaction);
        // Resource and recursive manifest validation precede any native XML
        // mutation. The archive validator above has already rejected unknown
        // visual runtime types and paths.
        if (!m_operations.validate_resources(
                created, verified_archive, source))
            return reject(WreckMaterializationResult::ResourceRejected);
        if (!m_operations.load_structure(
                created, ByteView{verified_archive.canonical_xml}))
            return reject(WreckMaterializationResult::StructureRejected);
        if (m_operations.collect_created_objects)
            m_operations.collect_created_objects(created, transaction);
        if (!m_operations.post_load_graph(created))
            return reject(WreckMaterializationResult::PostLoadRejected);
        if (m_operations.collect_created_objects)
            m_operations.collect_created_objects(created, transaction);
        if (!m_operations.create_visual_part(created))
            return reject(WreckMaterializationResult::VisualRejected);
        for (const NativeObjectArchiveVisualRuntime& runtime :
             verified_archive.visual_runtime) {
            if (!m_operations.apply_visual_runtime(created, runtime))
                return reject(WreckMaterializationResult::VisualRejected);
        }
        if (!m_operations.apply_authoritative_pose(created, authoritative_pose))
            return reject(WreckMaterializationResult::PoseRejected);
        if (!m_operations.disable_physics(created))
            return reject(WreckMaterializationResult::PhysicsRejected);
        if (!m_operations.remove_from_simulation(created))
            return reject(WreckMaterializationResult::SimulationRejected);
        if (!m_operations.validate_wreck(created))
            return reject(WreckMaterializationResult::ValidationRejected);
        if (!m_operations.bind_wreck(wreck_identity, created)) {
            // The bind callback may have installed a partial entry before
            // reporting failure, so cleanup invokes the inverse unconditionally
            // for this failure boundary as well.
            try {
                m_operations.unbind_wreck(wreck_identity, created);
            }
            catch (...) {
            }
            return reject(WreckMaterializationResult::BindingRejected);
        }
        bound = true;
        // This is the first operation that is allowed to retire the source
        // binding. Every prior failure leaves it untouched.
        if (!m_operations.retire_source(source_identity))
            return reject(WreckMaterializationResult::SourceRetireRejected);
        return WreckMaterializationResult::Committed;
    }
    catch (const std::bad_alloc&) {
        return reject(WreckMaterializationResult::AllocationFailure);
    }
    catch (...) {
        return reject(WreckMaterializationResult::CreationRejected);
    }
}

CanonicalCueResolver::CanonicalCueResolver(Resolve resolve)
    : m_resolve(std::move(resolve))
{
}

bool CanonicalCueResolver::resolve(const std::string_view native_name,
                                   ResourceCue& output) const noexcept
{
    output = {};
    if (native_name.empty() || !m_resolve)
        return false;

    ResourceCue candidate{};
    try {
        if (!m_resolve(native_name, candidate))
            return false;
    }
    catch (...) {
        return false;
    }

    const std::string canonical = normalize_resource_name(native_name);
    if (canonical.empty() || candidate.name != canonical ||
        candidate.hash != resource_cue_hash(canonical) ||
        validate_resource_cue(candidate, false) !=
            CombatPresentationCodecError::None)
        return false;
    output = std::move(candidate);
    return true;
}

HostImpactCaptureScope::HostImpactCaptureScope(
    HostImpactObservation& observation) noexcept
    : m_previous(g_impact_capture),
      m_outermost(g_impact_capture == nullptr)
{
    g_impact_capture = &observation;
}

HostImpactCaptureScope::~HostImpactCaptureScope()
{
    g_impact_capture = m_previous;
}

HostImpactObservation* current_host_impact_capture() noexcept
{
    return g_impact_capture;
}

void record_effect_branch(const std::string_view native_name) noexcept
{
    if (g_impact_capture == nullptr)
        return;
    try {
        g_impact_capture->effect_name.assign(native_name.data(),
                                             native_name.size());
    }
    catch (...) {
        g_impact_capture->effect_name.clear();
    }
}

void record_decal_branch(const std::string_view native_name) noexcept
{
    if (g_impact_capture == nullptr)
        return;
    try {
        g_impact_capture->decal_name.assign(native_name.data(),
                                            native_name.size());
    }
    catch (...) {
        g_impact_capture->decal_name.clear();
    }
}

void record_environment_branch(const EnvironmentKind kind) noexcept
{
    if (g_impact_capture == nullptr ||
        g_impact_capture->target.kind != ImpactTargetKind::Environment ||
        !is_valid_environment_kind(kind))
        return;
    if (g_impact_capture->target.environment_kind ==
            EnvironmentKind::UnboundStatic ||
        kind != EnvironmentKind::UnboundStatic)
        g_impact_capture->target.environment_kind = kind;
    g_impact_capture->target_captured = true;
}

void record_effect_geometry(const VehicleVector3& position,
                            const VehicleQuaternion& rotation,
                            const bool remove_if_free,
                            const float scale) noexcept
{
    if (g_impact_capture == nullptr)
        return;
    g_impact_capture->geometry.effect_position = position;
    g_impact_capture->geometry.effect_rotation = rotation;
    g_impact_capture->geometry.remove_if_free = remove_if_free;
    g_impact_capture->geometry.effect_scale = scale;
}

void record_decal_geometry(const VehicleVector3& position,
                           const VehicleVector3& contact_normal,
                           const VehicleVector3& tangent,
                           const MeshIdentity mesh_id,
                           const AttachmentIdentity& part) noexcept
{
    if (g_impact_capture == nullptr)
        return;
    g_impact_capture->geometry.hit_position = position;
    g_impact_capture->geometry.contact_normal = contact_normal;
    g_impact_capture->geometry.decal_tangent = tangent;
    g_impact_capture->geometry.has_decal_tangent = true;
    g_impact_capture->geometry.mesh_id = mesh_id;
    g_impact_capture->geometry.target_part = part;
}

void record_effect_produced() noexcept
{
    if (g_impact_capture != nullptr)
        g_impact_capture->effect_produced = true;
}

void record_decal_produced() noexcept
{
    if (g_impact_capture != nullptr)
        g_impact_capture->decal_produced = true;
}

void record_authoritative_damage(const DamageResult& damage) noexcept
{
    if (g_impact_capture == nullptr)
        return;
    g_impact_capture->did_damage = true;
    g_impact_capture->damage = damage;
}

HostImpactCapture::HostImpactCapture(CanonicalCueResolver resolver)
    : m_resolver(std::move(resolver))
{
}

RuntimeApplyResult HostImpactCapture::publish(
    const HostImpactObservation& observation,
    const HostImpactPublication& publication) const
{
    if (observation.session_epoch == 0 || observation.event_id == 0 ||
        observation.shot_id == 0 || !valid_identity(observation.shooter))
        return RuntimeApplyResult::InvalidEvent;

    DamageResult damage{};
    bool damage_ready = false;
    if (observation.did_damage) {
        damage = observation.damage;
        const bool target_matches_capture =
            !observation.target_captured ||
            (observation.target.kind == ImpactTargetKind::DynamicEntity &&
             damage.target.net_id == observation.target.dynamic.net_id &&
             damage.target.generation == observation.target.dynamic.generation);
        damage_ready = publication.publish_damage != nullptr &&
            damage.event_id == observation.event_id &&
            damage.shot_id == observation.shot_id &&
            damage.shooter.net_id == observation.shooter.net_id &&
            damage.shooter.generation == observation.shooter.generation &&
            target_matches_capture &&
            validate_damage_result(damage) ==
                CombatPresentationCodecError::None;
    }

    bool visual_published = false;
    const bool visual_candidate =
        (observation.effect_produced || observation.decal_produced) &&
        observation.target_captured && observation.contact_captured &&
        publication.publish_impact != nullptr;
    if (visual_candidate) {
        ImpactPresentation impact{};
        impact.session_epoch = observation.session_epoch;
        impact.event_id = observation.event_id;
        impact.server_tick = observation.server_tick;
        impact.shot_id = observation.shot_id;
        impact.shooter = observation.shooter;
        impact.gun = observation.gun;
        impact.target = observation.target;
        impact.target_part = observation.geometry.target_part;
        impact.surface = observation.geometry.surface;
        impact.hit_position = observation.geometry.hit_position;
        impact.effect_position = observation.geometry.effect_position;
        impact.incoming_direction = observation.geometry.incoming_direction;
        impact.contact_normal = observation.geometry.contact_normal;
        impact.decal_tangent = observation.geometry.decal_tangent;
        impact.has_incoming_direction = observation.geometry.has_incoming_direction;
        impact.has_decal_tangent = observation.geometry.has_decal_tangent;
        impact.mesh_id = observation.geometry.mesh_id;
        impact.material_id = observation.geometry.material_id;
        impact.effect_rotation = observation.geometry.effect_rotation;
        impact.effect_scale = observation.geometry.effect_scale;
        impact.remove_if_free = observation.geometry.remove_if_free;
        impact.did_damage = observation.did_damage;
        impact.blocked_reason = observation.blocked_reason;

        bool visual_valid = true;
        if (observation.effect_produced &&
            !m_resolver.resolve(observation.effect_name, impact.effect_cue))
            visual_valid = false;
        if (observation.decal_produced &&
            !m_resolver.resolve(observation.decal_name, impact.decal_cue))
            visual_valid = false;
        if (visual_valid && validate_impact_presentation(impact) ==
                                CombatPresentationCodecError::None)
            visual_published = publication.publish_impact(impact);
    }

    if (damage_ready) {
        damage.impact_event_id = visual_published ? observation.event_id : 0;
        if (!publication.publish_damage(damage))
            return RuntimeApplyResult::NativeRejected;
    }
    if (visual_published || damage_ready)
        return RuntimeApplyResult::Applied;
    return RuntimeApplyResult::InvalidEvent;
}

bool HostProcessShellAndBodyHook::install(
    const HostProcessHookInstaller& installer,
    const ProcessShellAndBodyFunction replacement)
{
    if (m_installed || !replacement || !installer.install)
        return m_installed;
    ProcessShellAndBodyFunction original_function = nullptr;
    if (!installer.install(kProcessShellAndBodyVa, replacement,
                           original_function) || !original_function)
        return false;
    m_original = original_function;
    m_installed = true;
    g_process_hook = this;
    return true;
}

void HostProcessShellAndBodyHook::set_callbacks(
    HostProcessHookCallbacks callbacks)
{
    m_callbacks = std::move(callbacks);
}

int HostProcessShellAndBodyHook::invoke(
    hta::ai::Shell* shell, hta::ai::PhysicBody* body, dContact* contact,
    std::uint32_t* contact_count,
    const bool reverse) const
{
    if (m_original == nullptr)
        return 0;
    HostImpactObservation observation{};
    if (m_callbacks.initialize)
        m_callbacks.initialize(shell, body, contact, contact_count, reverse,
                               observation);
    HostImpactCaptureScope scope(observation);
    const int result = m_original(shell, body, contact, contact_count,
                                  reverse);
    if (scope.outermost() && m_callbacks.complete)
        m_callbacks.complete(observation);
    return result;
}

int KRAKEN_COMBAT_RUNTIME_FASTCALL process_shell_and_body_capture_hook(
    hta::ai::Shell* shell, hta::ai::PhysicBody* body, dContact* contact,
    std::uint32_t* contact_count,
    const bool reverse)
{
    return g_process_hook == nullptr
               ? 0
               : g_process_hook->invoke(shell, body, contact, contact_count,
                                        reverse);
}

bool HornNodeOperations::ready() const noexcept
{
    return static_cast<bool>(create_node) && static_cast<bool>(set_property) &&
           static_cast<bool>(add_child) && static_cast<bool>(remove_child) &&
           static_cast<bool>(release_node);
}

HornSidecar::HornSidecar(HornNodeOperations operations)
    : m_operations(std::move(operations))
{
}

bool HornSidecar::same_identity(const NetEntityRef& left,
                                const NetEntityRef& right) noexcept
{
    return ::kraken::net::combat_runtime::same_identity(left, right);
}

HornSidecar::Entry* HornSidecar::find(const NetEntityRef& identity) noexcept
{
    const auto iterator = std::find_if(
        m_entries.begin(), m_entries.end(),
        [&identity](const Entry& entry) {
            return same_identity(entry.identity, identity);
        });
    return iterator == m_entries.end() ? nullptr : &*iterator;
}

const HornSidecar::Entry* HornSidecar::find(
    const NetEntityRef& identity) const noexcept
{
    const auto iterator = std::find_if(
        m_entries.begin(), m_entries.end(),
        [&identity](const Entry& entry) {
            return same_identity(entry.identity, identity);
        });
    return iterator == m_entries.end() ? nullptr : &*iterator;
}

void HornSidecar::remove_entry(Entry& entry) noexcept
{
    if (entry.node != nullptr) {
        if (entry.vehicle != nullptr)
            (void)m_operations.remove_child(entry.vehicle, entry.node);
        m_operations.release_node(entry.node);
    }
    entry.node = nullptr;
    entry.vehicle = nullptr;
    entry.active = false;
}

RuntimeApplyResult HornSidecar::apply(const HornState& state,
                                      ReplicaResolver& resolver)
{
    const CombatPresentationCodecError validation = validate_horn_state(state);
    if (validation == CombatPresentationCodecError::InvalidCue)
        return RuntimeApplyResult::InvalidCue;
    if (validation != CombatPresentationCodecError::None ||
        !valid_identity(state.vehicle))
        return RuntimeApplyResult::InvalidEvent;
    if (!m_have_epoch) {
        m_epoch = state.session_epoch;
        m_have_epoch = true;
    }
    else if (state.session_epoch != m_epoch) {
        if (!epoch_is_newer(m_epoch, state.session_epoch))
            return RuntimeApplyResult::Stale;
        reset(resolver);
        m_epoch = state.session_epoch;
        m_have_epoch = true;
    }

    Entry* entry = find(state.vehicle);
    if (entry != nullptr && state.transition_id == entry->transition_id)
        return RuntimeApplyResult::Duplicate;
    if (entry != nullptr &&
        !newer_transition(entry->transition_id, state.transition_id))
        return RuntimeApplyResult::Stale;

    ReplicaBinding binding{};
    if (!resolver.resolve(state.vehicle, binding) ||
        !same_identity(binding.identity, state.vehicle) ||
        binding.native_vehicle == nullptr || !binding.replica_authority ||
        !binding.inert)
        return RuntimeApplyResult::InvalidIdentity;

    if (!state.active) {
        if (entry == nullptr)
            return RuntimeApplyResult::Duplicate;
        entry->transition_id = state.transition_id;
        remove_entry(*entry);
        return RuntimeApplyResult::Applied;
    }
    if (!cue_present(state.horn_cue) || !m_operations.ready())
        return RuntimeApplyResult::InvalidCue;

    if (entry != nullptr)
        remove_entry(*entry);
    else {
        try {
            m_entries.push_back(Entry{});
            entry = &m_entries.back();
        }
        catch (...) {
            return RuntimeApplyResult::NativeRejected;
        }
    }
    entry->identity = state.vehicle;
    entry->transition_id = state.transition_id;
    entry->vehicle = binding.native_vehicle;
    entry->node = m_operations.create_node(binding.native_vehicle,
                                           state.horn_cue);
    if (entry->node == nullptr ||
        !m_operations.set_property(entry->node, kHornLoopProperty, true) ||
        !m_operations.add_child(entry->vehicle, entry->node)) {
        if (entry->node != nullptr)
            m_operations.release_node(entry->node);
        entry->node = nullptr;
        entry->vehicle = nullptr;
        entry->active = false;
        return RuntimeApplyResult::NativeRejected;
    }
    entry->active = true;
    return RuntimeApplyResult::Applied;
}

void HornSidecar::cleanup(const NetEntityRef& identity,
                          ReplicaResolver& resolver) noexcept
{
    Entry* const entry = find(identity);
    if (entry == nullptr)
        return;
    remove_entry(*entry);
    m_entries.erase(std::remove_if(
        m_entries.begin(), m_entries.end(),
        [&identity](const Entry& candidate) {
            return same_identity(candidate.identity, identity);
        }), m_entries.end());
}

void HornSidecar::reset(ReplicaResolver& resolver) noexcept
{
    for (Entry& entry : m_entries)
        remove_entry(entry);
    m_entries.clear();
    m_have_epoch = false;
    m_epoch = 0;
}

ReplicaCombatRuntime::ReplicaCombatRuntime(
    ReplicaResolver& resolver, ReplicaNativeOperations native,
    HornNodeOperations horn, WreckResolver wreck_resolver)
    : m_resolver(resolver),
      m_native(std::move(native)),
      m_horn(std::move(horn)),
      m_wreck_resolver(std::move(wreck_resolver))
{
}

RuntimeApplyResult ReplicaCombatRuntime::begin_epoch(
    const std::uint32_t epoch) noexcept
{
    if (epoch == 0)
        return RuntimeApplyResult::InvalidEvent;
    if (!m_have_epoch) {
        m_epoch = epoch;
        m_have_epoch = true;
        return RuntimeApplyResult::Applied;
    }
    if (epoch == m_epoch)
        return RuntimeApplyResult::Applied;
    if (!epoch_is_newer(m_epoch, epoch))
        return RuntimeApplyResult::Stale;
    reset();
    m_epoch = epoch;
    m_have_epoch = true;
    return RuntimeApplyResult::Applied;
}

RuntimeApplyResult ReplicaCombatRuntime::resolve_replica(
    const NetEntityRef& identity, ReplicaBinding& output) const
{
    if (!valid_identity(identity) || !m_resolver.resolve(identity, output) ||
        !same_identity(identity, output.identity) ||
        output.native_vehicle == nullptr || output.class_id == 0 ||
        output.chassis_id == 0 || !output.replica_authority || !output.inert)
        return RuntimeApplyResult::InvalidIdentity;
    return RuntimeApplyResult::Applied;
}

RuntimeApplyResult ReplicaCombatRuntime::mark_dead(
    ReplicaBinding& binding) const
{
    if (!m_native.health_projection_ready())
        return RuntimeApplyResult::NativeRejected;
    const float maximum = m_native.get_max_health(binding.native_vehicle);
    const float health = m_native.get_health(binding.native_vehicle);
    if (!finite_in_range(maximum, 0.0f,
                         static_cast<float>(kMaxCombatHealth)) ||
        !finite_in_range(health, 0.0f, maximum) || health > 0.0f)
        return RuntimeApplyResult::InvalidHealth;
    const std::uint32_t before = m_native.read_flags(binding.native_vehicle);
    if (!m_native.set_flags_masked(binding.native_vehicle, kObjDeadFlag,
                                   kObjDeadFlag))
        return RuntimeApplyResult::NativeRejected;
    const std::uint32_t after = m_native.read_flags(binding.native_vehicle);
    if ((after & kObjDeadFlag) == 0 ||
        (after & ~kObjDeadFlag) != (before & ~kObjDeadFlag))
        return RuntimeApplyResult::NativeRejected;
    return RuntimeApplyResult::Applied;
}

bool ReplicaCombatRuntime::accept_event(const std::uint32_t epoch,
                                         const CombatEventId event_id,
                                         CombatEventId& high_water,
                                         bool& have_high_water) noexcept
{
    if (begin_epoch(epoch) != RuntimeApplyResult::Applied || event_id == 0)
        return false;
    if (!have_high_water) {
        high_water = event_id;
        have_high_water = true;
        return true;
    }
    if (!newer(high_water, event_id))
        return false;
    high_water = event_id;
    return true;
}

bool ReplicaCombatRuntime::accept_transition(
    const std::uint32_t epoch, const CombatTransitionId transition_id,
    CombatTransitionId& high_water) noexcept
{
    const RuntimeApplyResult epoch_result = begin_epoch(epoch);
    if (epoch_result != RuntimeApplyResult::Applied || transition_id == 0)
        return false;
    if (high_water == 0) {
        high_water = transition_id;
        return true;
    }
    if (!newer_transition(high_water, transition_id))
        return false;
    high_water = transition_id;
    return true;
}

bool ReplicaCombatRuntime::seen_impact(const CombatEventId event_id) const noexcept
{
    return std::find(m_impact_events.begin(), m_impact_events.end(), event_id) !=
           m_impact_events.end();
}

void ReplicaCombatRuntime::remember_impact(const CombatEventId event_id)
{
    constexpr std::size_t kMaxRememberedImpacts = 4096;
    try {
        m_impact_events.push_back(event_id);
        if (m_impact_events.size() > kMaxRememberedImpacts)
            m_impact_events.erase(m_impact_events.begin());
    }
    catch (...) {
        // An event cannot be made safely correlatable if the bounded journal
        // cannot retain its identity. The caller has already applied FX, so
        // this only affects a later DamageResult correlation.
    }
}

RuntimeApplyResult ReplicaCombatRuntime::apply_impact(
    const ImpactPresentation& event)
{
    if (validate_impact_presentation(event) !=
        CombatPresentationCodecError::None)
        return RuntimeApplyResult::InvalidEvent;
    const RuntimeApplyResult epoch_result = begin_epoch(event.session_epoch);
    if (epoch_result != RuntimeApplyResult::Applied)
        return epoch_result;
    if (seen_impact(event.event_id))
        return RuntimeApplyResult::Duplicate;
    if (m_have_impact_event && !newer(m_last_impact_event, event.event_id))
        return RuntimeApplyResult::Stale;

    const ImpactGeometry geometry{
        event.target_part, event.surface, event.hit_position,
        event.effect_position, event.incoming_direction,
        event.contact_normal, event.decal_tangent,
        event.has_incoming_direction, event.has_decal_tangent,
        event.mesh_id, event.material_id, event.effect_rotation,
        event.effect_scale, event.remove_if_free};
    if (event.target.kind == ImpactTargetKind::DynamicEntity) {
        ReplicaBinding binding{};
        if (resolve_replica(event.target.dynamic, binding) !=
            RuntimeApplyResult::Applied)
            return RuntimeApplyResult::InvalidIdentity;
        if ((cue_present(event.effect_cue) &&
             !m_native.create_effect_node) ||
            (cue_present(event.decal_cue) && !m_native.add_decal))
            return RuntimeApplyResult::NativeRejected;
        if (cue_present(event.effect_cue) &&
            !m_native.create_effect_node(binding.native_vehicle, event,
                                          geometry))
            return RuntimeApplyResult::NativeRejected;
        if (cue_present(event.decal_cue) &&
            !m_native.add_decal(binding.native_vehicle, event, geometry))
            return RuntimeApplyResult::NativeRejected;
    }
    else if (event.target.kind == ImpactTargetKind::StableStatic) {
        if (event.target.stable.stable_id == 0 ||
            event.target.stable.path_hash == 0 ||
            (cue_present(event.effect_cue) &&
             !m_native.create_static_effect) ||
            (cue_present(event.decal_cue) && !m_native.add_static_decal))
            return RuntimeApplyResult::InvalidIdentity;
        if (cue_present(event.effect_cue) &&
            !m_native.create_static_effect(event, geometry))
            return RuntimeApplyResult::NativeRejected;
        if (cue_present(event.decal_cue) &&
            !m_native.add_static_decal(event, geometry))
            return RuntimeApplyResult::NativeRejected;
    }
    else if (event.target.kind == ImpactTargetKind::Environment) {
        // Environment effects are world-space only. No object-bound static
        // decal ABI is proven, so a decal-bearing environment event closes
        // fail-closed instead of inventing a target or local mesh handle.
        if (!event.decal_cue.empty())
            return RuntimeApplyResult::NativeRejected;
        if (cue_present(event.effect_cue) &&
            !m_native.create_effect_node)
            return RuntimeApplyResult::NativeRejected;
        if (cue_present(event.effect_cue) &&
            !m_native.create_effect_node(nullptr, event, geometry))
            return RuntimeApplyResult::NativeRejected;
    }
    else {
        return RuntimeApplyResult::InvalidIdentity;
    }
    m_last_impact_event = event.event_id;
    m_have_impact_event = true;
    remember_impact(event.event_id);
    return RuntimeApplyResult::Applied;
}

RuntimeApplyResult ReplicaCombatRuntime::apply_damage(
    const DamageResult& event)
{
    if (validate_damage_result(event) != CombatPresentationCodecError::None)
        return RuntimeApplyResult::InvalidEvent;
    if (event.impact_event_id != 0 &&
        !seen_impact(event.impact_event_id))
        return RuntimeApplyResult::InvalidEvent;
    if (std::find(m_damage_events.begin(), m_damage_events.end(),
                  event.event_id) != m_damage_events.end())
        return RuntimeApplyResult::Duplicate;
    if (std::find(m_damage_impacts.begin(), m_damage_impacts.end(),
                  event.impact_event_id) != m_damage_impacts.end() &&
        event.impact_event_id != 0)
        return RuntimeApplyResult::Duplicate;
    const RuntimeApplyResult epoch_result = begin_epoch(event.session_epoch);
    if (epoch_result != RuntimeApplyResult::Applied)
        return epoch_result;
    if (m_last_damage_event != 0 &&
        !newer(m_last_damage_event, event.event_id))
        return RuntimeApplyResult::Stale;

    ReplicaBinding binding{};
    if (resolve_replica(event.target, binding) !=
        RuntimeApplyResult::Applied)
        return RuntimeApplyResult::InvalidIdentity;
    if (!m_native.health_projection_ready())
        return RuntimeApplyResult::NativeRejected;
    const float maximum = m_native.get_max_health(binding.native_vehicle);
    if (!finite_in_range(maximum, 0.0f,
                         static_cast<float>(kMaxCombatHealth)) ||
        !finite_in_range(event.post_health, 0.0f, maximum) ||
        !finite_in_range(event.damage, 0.0f,
                         static_cast<float>(kMaxCombatHealth)))
        return RuntimeApplyResult::InvalidHealth;
    if (!m_native.set_health_unsafe(binding.native_vehicle, event.post_health))
        return RuntimeApplyResult::NativeRejected;
    const float read_back = m_native.get_health(binding.native_vehicle);
    const float tolerance = (std::max)(0.001f, maximum * 0.0001f);
    if (!finite_in_range(read_back, 0.0f, maximum) ||
        std::fabs(read_back - event.post_health) > tolerance)
        return RuntimeApplyResult::InvalidHealth;
    if (event.dead_transition) {
        if (read_back > 0.0f)
            return RuntimeApplyResult::InvalidHealth;
        const RuntimeApplyResult result = mark_dead(binding);
        if (result == RuntimeApplyResult::Applied) {
            try {
                m_damage_events.push_back(event.event_id);
                if (event.impact_event_id != 0)
                    m_damage_impacts.push_back(event.impact_event_id);
            }
            catch (...) {
                return RuntimeApplyResult::NativeRejected;
            }
            m_last_damage_event = event.event_id;
        }
        return result;
    }
    try {
        m_damage_events.push_back(event.event_id);
        if (event.impact_event_id != 0)
            m_damage_impacts.push_back(event.impact_event_id);
    }
    catch (...) {
        return RuntimeApplyResult::NativeRejected;
    }
    m_last_damage_event = event.event_id;
    return RuntimeApplyResult::Applied;
}

RuntimeApplyResult ReplicaCombatRuntime::apply_death(
    const DeathWreckPresentation& event)
{
    if (validate_death_wreck_presentation(event) !=
            CombatPresentationCodecError::None ||
        !event.terminal)
        return RuntimeApplyResult::InvalidEvent;
    ReplicaBinding binding{};
    if (resolve_replica(event.entity, binding) != RuntimeApplyResult::Applied)
        return RuntimeApplyResult::InvalidIdentity;
    if (!m_wreck_resolver)
        return RuntimeApplyResult::ResolverRejected;
    for (const PendingRemoval& pending : m_pending_removals) {
        if (same_identity(pending.death.entity, event.entity) &&
            pending.death.transition_id == event.transition_id)
            return RuntimeApplyResult::Duplicate;
    }
    std::vector<Byte> encoded_archive;
    NativeObjectArchiveV2 archive{};
    if (!m_wreck_archives.lookup(event.wreck_archive_digest,
                                 encoded_archive) ||
        decode_native_object_archive_v2(ByteView{encoded_archive}, archive) !=
            NativeObjectArchiveErrorCode::None)
    {
        try {
            m_pending_removals.push_back(PendingRemoval{
                event, binding, false, false, false, false});
        }
        catch (...) {
            return RuntimeApplyResult::NativeRejected;
        }
        // The terminal event is intentionally not consumed yet. A reliable
        // archive chunk may arrive after it; process_removals will retry the
        // exact event once the digest cache is complete.
        return RuntimeApplyResult::PendingRemoval;
    }
    try {
        m_pending_removals.push_back(PendingRemoval{
            event, binding, false, false, false, false});
    }
    catch (...) {
        return RuntimeApplyResult::NativeRejected;
    }
    // Packet handling never mutates the native object graph. Even with a
    // complete archive, the resolver and atomic swap run only from
    // process_removals(true), immediately before the next native tick.
    return RuntimeApplyResult::PendingRemoval;
}

bool ReplicaCombatRuntime::accept_wreck_archive_chunk(
    const ByteView payload, const std::uint64_t now_ms)
{
    NativeObjectArchiveChunk chunk{};
    if (decode_native_object_archive_chunk(payload, chunk) !=
        NativeObjectArchiveErrorCode::None)
        return false;
    std::vector<Byte> complete;
    const NativeObjectArchiveTransferResult result =
        m_wreck_archives.accept(chunk, now_ms, complete);
    return result == NativeObjectArchiveTransferResult::Accepted ||
           result == NativeObjectArchiveTransferResult::Complete ||
           result == NativeObjectArchiveTransferResult::Duplicate;
}

RuntimeApplyResult ReplicaCombatRuntime::apply_horn(const HornState& event)
{
    const RuntimeApplyResult result = m_horn.apply(event, m_resolver);
    return result;
}

RuntimeApplyResult ReplicaCombatRuntime::apply_jip(
    const PresentationJipState& state)
{
    if (validate_presentation_jip_state(state) !=
        CombatPresentationCodecError::None)
        return RuntimeApplyResult::InvalidEvent;
    RuntimeApplyResult result = RuntimeApplyResult::Applied;
    for (const HornState& horn : state.horn_states) {
        const RuntimeApplyResult current = apply_horn(horn);
        if (current != RuntimeApplyResult::Applied &&
            current != RuntimeApplyResult::Duplicate)
            result = current;
    }
    for (const DeathWreckPresentation& death : state.terminal_deaths) {
        const RuntimeApplyResult current = apply_death(death);
        if (current != RuntimeApplyResult::PendingRemoval &&
            current != RuntimeApplyResult::Duplicate)
            result = current;
    }
    return result;
}

RuntimeApplyResult ReplicaCombatRuntime::process_removals(
    const bool pre_sim_boundary)
{
    if (!pre_sim_boundary)
        return m_pending_removals.empty() ? RuntimeApplyResult::Applied
                                           : RuntimeApplyResult::PendingRemoval;
    if (!m_native.removal_ready())
        return RuntimeApplyResult::NativeRejected;
    RuntimeApplyResult result = RuntimeApplyResult::Applied;
    for (PendingRemoval& pending : m_pending_removals) {
        if (!pending.wreck_ready) {
            if (!m_wreck_resolver)
                return RuntimeApplyResult::ResolverRejected;
            ReplicaBinding refreshed_binding{};
            if (resolve_replica(pending.death.entity, refreshed_binding) !=
                RuntimeApplyResult::Applied) {
                result = RuntimeApplyResult::InvalidIdentity;
                continue;
            }
            pending.binding = refreshed_binding;
            std::vector<Byte> encoded_archive;
            NativeObjectArchiveV2 archive{};
            if (!m_wreck_archives.lookup(pending.death.wreck_archive_digest,
                                         encoded_archive) ||
                decode_native_object_archive_v2(ByteView{encoded_archive},
                                                archive) !=
                    NativeObjectArchiveErrorCode::None) {
                result = RuntimeApplyResult::PendingRemoval;
                continue;
            }
            // The dead-bit projection is a narrow, callback-free flag write.
            // Do it before materialization so a later materializer failure
            // leaves the source binding intact and cannot strand a committed
            // wreck with an unprojected terminal source.
            if (mark_dead(pending.binding) != RuntimeApplyResult::Applied) {
                result = RuntimeApplyResult::NativeRejected;
                continue;
            }
            if (!pending.transition_accepted) {
                if (!accept_transition(pending.death.session_epoch,
                                       pending.death.transition_id,
                                       m_last_death_event)) {
                    result = RuntimeApplyResult::Stale;
                    continue;
                }
                pending.transition_accepted = true;
            }
            WreckResolution resolution{};
            if (!m_wreck_resolver(pending.death, pending.binding,
                                  resolution) ||
                !resolution.inert_replacement_ready ||
                !resolution.archive_verified ||
                resolution.archive_digest != pending.death.wreck_archive_digest ||
                !same_identity(resolution.wreck_identity,
                               pending.death.wreck_entity)) {
                result = RuntimeApplyResult::PendingRemoval;
                continue;
            }
            pending.wreck_ready = true;
            pending.source_removal_scheduled =
                resolution.source_removal_scheduled;
        }
        if (!pending.scheduled) {
            if (!pending.source_removal_scheduled &&
                !m_native.schedule_removal(pending.binding.native_vehicle)) {
                result = RuntimeApplyResult::NativeRejected;
                continue;
            }
            pending.scheduled = true;
        }
        if (pending.scheduled &&
            m_native.has_disappeared(pending.binding.native_vehicle)) {
            (void)m_resolver.retire(pending.binding.identity);
            pending.binding = {};
        }
        else {
            result = RuntimeApplyResult::PendingRemoval;
        }
    }
    m_pending_removals.erase(std::remove_if(
        m_pending_removals.begin(), m_pending_removals.end(),
        [](const PendingRemoval& pending) {
            return pending.binding.native_vehicle == nullptr;
        }), m_pending_removals.end());
    return result;
}

void ReplicaCombatRuntime::despawn(const NetEntityRef& identity) noexcept
{
    m_horn.cleanup(identity, m_resolver);
    m_pending_removals.erase(std::remove_if(
        m_pending_removals.begin(), m_pending_removals.end(),
        [&identity](const PendingRemoval& pending) {
            return same_identity(pending.binding.identity, identity);
        }), m_pending_removals.end());
}

void ReplicaCombatRuntime::reset() noexcept
{
    m_horn.reset(m_resolver);
    m_impact_events.clear();
    m_damage_events.clear();
    m_damage_impacts.clear();
    m_pending_removals.clear();
    m_wreck_archives.clear();
    m_epoch = 0;
    m_have_epoch = false;
    m_last_impact_event = 0;
    m_have_impact_event = false;
    m_last_damage_event = 0;
    m_last_death_event = 0;
    m_last_horn_transition = 0;
}

} // namespace kraken::net::combat_runtime

#undef KRAKEN_COMBAT_RUNTIME_FASTCALL
