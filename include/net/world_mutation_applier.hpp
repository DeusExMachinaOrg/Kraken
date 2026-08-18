#ifndef KRAKEN_NET_WORLD_MUTATION_APPLIER_HPP
#define KRAKEN_NET_WORLD_MUTATION_APPLIER_HPP

#include "net/vehicle_transfer.hpp"
#include "net/world_mutation_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace kraken::net {

// The source is authenticated/selected by the session adapter before the
// engine-neutral applier sees an event.  Both fields are checked strictly;
// the applier never infers authority from the event kind.
struct WorldMutationSource {
    TransportRole role = TransportRole::Server;
    ReplicationSourceContext context = ReplicationSourceContext::NetworkReplay;
    PeerId peer = kInvalidPeer;
};

using WorldMutationApplyContext = WorldMutationSource;

struct WorldMutationEnvelope {
    WorldEpoch epoch = kInvalidWorldEpoch;
    WorldRevision revision = kInvalidWorldRevision;
    WorldMutationSource source{};
    WorldMutationEvent event{};
};

using WorldMutationMessage = WorldMutationEnvelope;

enum class WorldMutationApplyResult : std::uint8_t {
    Applied,
    Duplicate,
    Reordered,
    Gap,
    WrongEpoch,
    InvalidRevision,
    InvalidRole,
    InvalidSource,
    InvalidEvent,
    UnknownObject,
    InvalidRelation,
    ObjectIdCollision,
    MissingCallback,
    CallbackFailed,
    TransactionBeginFailed,
    TransactionCommitFailed,
    RollbackFailed,
    ReplayGuardFailed,
    InvalidConfiguration,
    RevisionConflict,

    // Descriptive aliases for callers that use protocol terminology.
    StaleRevision = Reordered,
    RevisionGap = Gap,
    RoleRejected = InvalidRole,
    SourceRejected = InvalidSource,
    UnknownObjectId = UnknownObject,
};

[[nodiscard]] constexpr bool world_mutation_apply_succeeded(
    const WorldMutationApplyResult result) noexcept
{
    return result == WorldMutationApplyResult::Applied ||
           result == WorldMutationApplyResult::Duplicate;
}

struct WorldMutationEngineCallbacks {
    using Create = std::function<bool(const ObjectCreatedEvent&)>;
    using Despawn = std::function<bool(const ObjectDespawnedEvent&)>;
    using AddChild = std::function<bool(const ParentChildAddedEvent&)>;
    using RemoveChild = std::function<bool(const ParentChildRemovedEvent&)>;
    using Runtime = std::function<bool(const RuntimeChangedEvent&)>;
    using Property = std::function<bool(const PropertyChangedEvent&)>;
    using Damage = std::function<bool(const DamageEvent&)>;
    using Destroyed = std::function<bool(const DestroyedEvent&)>;
    using Fx = std::function<bool(const FxEvent&)>;
    using Transaction = std::function<bool()>;

    Create create;
    Despawn despawn;
    AddChild add_child;
    RemoveChild remove_child;
    Runtime runtime;
    Property property;
    Damage damage;
    Destroyed destroyed;
    Fx fx;

    // If one transaction hook is supplied, all three must be supplied.  A
    // callback set with no hooks is still atomic at its individual callback
    // boundary and never advances the revision on a false/throwing callback.
    Transaction begin_transaction;
    Transaction commit_transaction;
    Transaction rollback_transaction;
};

using WorldMutationCallbacks = WorldMutationEngineCallbacks;
using WorldMutationEngine = WorldMutationEngineCallbacks;
using WorldMutationReplayGuardFactory = std::function<ReplayGuard()>;

struct WorldMutationApplierConfig {
    WorldEpoch epoch = 1;
    WorldRevision initial_revision = kInvalidWorldRevision;
    TransportRole expected_source_role = TransportRole::Server;
    ReplicationSourceContext expected_source =
        ReplicationSourceContext::NetworkReplay;
    PeerId expected_peer = kInvalidPeer;
    bool require_peer = false;
    WorldMutationReplayGuardFactory replay_guard_factory;
};

struct WorldMutationObjectState {
    HostObjectId object_id = kInvalidHostObjectId;
    ObjectTypeId type_id = 0;
    HostObjectId parent_id = kInvalidHostObjectId;
    bool active = false;
    bool destroyed = false;
};

// Applies an ordered, host-authoritative stream without knowing anything
// about the engine ABI.  Object and relation state is deliberately kept in
// this class so invalid IDs cannot reach injected callbacks.
class WorldMutationApplier final {
public:
    explicit WorldMutationApplier(
        WorldMutationEngineCallbacks callbacks,
        WorldMutationApplierConfig config = {});
    WorldMutationApplier(
        WorldMutationEngineCallbacks callbacks, WorldObserver& observer,
        WorldMutationApplierConfig config = {});

    WorldMutationApplier(const WorldMutationApplier&) = delete;
    WorldMutationApplier& operator=(const WorldMutationApplier&) = delete;
    WorldMutationApplier(WorldMutationApplier&&) = delete;
    WorldMutationApplier& operator=(WorldMutationApplier&&) = delete;
    ~WorldMutationApplier() = default;

    [[nodiscard]] WorldMutationApplyResult apply(
        const WorldMutationEnvelope& envelope);
    [[nodiscard]] WorldMutationApplyResult apply(
        const WorldMutationEvent& event, WorldEpoch epoch,
        WorldRevision revision, WorldMutationSource source);
    [[nodiscard]] WorldMutationApplyResult apply(
        const WorldMutationEvent& event, WorldEpoch epoch,
        WorldRevision revision, TransportRole source_role,
        ReplicationSourceContext source =
            ReplicationSourceContext::NetworkReplay,
        PeerId source_peer = kInvalidPeer)
    {
        return apply(WorldMutationEnvelope{
            epoch, revision, {source_role, source, source_peer}, event});
    }
    [[nodiscard]] WorldMutationApplyResult apply(
        const WorldMutationEvent& event, WorldRevision revision)
    {
        return apply(WorldMutationEnvelope{
            m_epoch, revision,
            {m_config.expected_source_role, m_config.expected_source,
             m_config.expected_peer},
            event});
    }
    [[nodiscard]] WorldMutationApplyResult apply(
        const WorldMutationEvent& event);
    [[nodiscard]] WorldMutationApplyResult apply_event(
        const WorldMutationEnvelope& envelope)
    { return apply(envelope); }
    [[nodiscard]] WorldMutationApplyResult apply_mutation(
        const WorldMutationEnvelope& envelope)
    { return apply(envelope); }
    [[nodiscard]] WorldMutationApplyResult apply(
        const WorldDelta& delta, WorldMutationSource source);
    [[nodiscard]] WorldMutationApplyResult apply_delta(
        const WorldDelta& delta, WorldMutationSource source)
    { return apply(delta, source); }

    // Baseline/snapshot adapters call this before replaying mutations.  It
    // does not invoke an engine callback and only installs a known object.
    [[nodiscard]] WorldMutationApplyResult install_object(
        HostObjectId object_id, ObjectTypeId type_id = 0,
        HostObjectId parent_id = kInvalidHostObjectId);
    [[nodiscard]] WorldMutationApplyResult register_object(
        HostObjectId object_id, ObjectTypeId type_id = 0,
        HostObjectId parent_id = kInvalidHostObjectId)
    { return install_object(object_id, type_id, parent_id); }

    void reset(WorldEpoch epoch = 1,
               WorldRevision initial_revision = kInvalidWorldRevision) noexcept;
    [[nodiscard]] WorldEpoch epoch() const noexcept { return m_epoch; }
    [[nodiscard]] WorldRevision revision() const noexcept
    { return m_revision; }
    [[nodiscard]] WorldRevision next_revision() const noexcept;
    [[nodiscard]] bool contains(HostObjectId object_id) const noexcept;
    [[nodiscard]] std::optional<WorldMutationObjectState> object_state(
        HostObjectId object_id) const noexcept;
    [[nodiscard]] std::optional<HostObjectId> parent_for(
        HostObjectId object_id) const noexcept;
    [[nodiscard]] std::size_t object_count() const noexcept;

    void set_replay_guard_factory(WorldMutationReplayGuardFactory factory)
    { m_replay_guard_factory = std::move(factory); }
    void set_replay_observer(WorldObserver* observer) noexcept
    { m_replay_observer = observer; }

private:
    enum class UndoKind : std::uint8_t {
        None,
        Created,
        Despawned,
        ParentChanged,
        DestroyedChanged,
    };

    struct Undo {
        UndoKind kind = UndoKind::None;
        std::size_t index = 0;
        HostObjectId old_parent = kInvalidHostObjectId;
        bool old_destroyed = false;
        bool retired_added = false;
    };

    [[nodiscard]] WorldMutationApplyResult validate_source(
        const WorldMutationSource& source) const noexcept;
    [[nodiscard]] WorldMutationApplyResult validate_event(
        const WorldMutationEvent& event) const noexcept;
    [[nodiscard]] WorldMutationApplyResult prepare_state(
        const WorldMutationEvent& event, Undo& undo);
    void undo_state(const Undo& undo) noexcept;
    [[nodiscard]] bool invoke(const WorldMutationEvent& event) const;
    [[nodiscard]] bool has_required_callbacks(
        const WorldMutationEvent& event) const noexcept;
    [[nodiscard]] bool has_transaction_hooks() const noexcept;
    [[nodiscard]] const WorldMutationObjectState* find_active(
        HostObjectId object_id) const noexcept;
    [[nodiscard]] WorldMutationObjectState* find_active(
        HostObjectId object_id) noexcept;
    [[nodiscard]] bool has_any_object(HostObjectId object_id) const noexcept;
    [[nodiscard]] bool was_retired(HostObjectId object_id) const noexcept;
    [[nodiscard]] bool has_active_child(HostObjectId object_id) const noexcept;
    [[nodiscard]] bool same_as_last(
        const WorldMutationEvent& event) const noexcept;
    [[nodiscard]] WorldMutationApplyResult apply_prevalidated(
        const WorldMutationEnvelope& envelope);

    WorldMutationEngineCallbacks m_callbacks;
    WorldMutationApplierConfig m_config;
    WorldMutationReplayGuardFactory m_replay_guard_factory;
    WorldObserver* m_replay_observer = nullptr;
    std::vector<WorldMutationObjectState> m_objects;
    std::vector<HostObjectId> m_retired_ids;
    std::optional<WorldMutationEvent> m_last_event;
    WorldEpoch m_epoch = 1;
    WorldRevision m_revision = kInvalidWorldRevision;
};

using WorldMutationApply = WorldMutationApplier;
using WorldDeltaApplier = WorldMutationApplier;

} // namespace kraken::net

#endif // KRAKEN_NET_WORLD_MUTATION_APPLIER_HPP
