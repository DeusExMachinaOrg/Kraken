#include "net/world_mutation_applier.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace kraken::net {
namespace {

bool same_float(const float left, const float right) noexcept
{
    std::uint32_t left_bits = 0;
    std::uint32_t right_bits = 0;
    std::memcpy(&left_bits, &left, sizeof(left_bits));
    std::memcpy(&right_bits, &right, sizeof(right_bits));
    return left_bits == right_bits;
}

template <typename Left, typename Right>
bool same_event_type(const Left& left, const Right& right) noexcept
{
    if constexpr (!std::is_same_v<Left, Right>) {
        return false;
    } else if constexpr (std::is_same_v<Left, ObjectCreatedEvent>) {
        return left.object_id == right.object_id &&
               left.type_id == right.type_id;
    } else if constexpr (std::is_same_v<Left, ObjectDespawnedEvent>) {
        return left.object_id == right.object_id;
    } else if constexpr (
        std::is_same_v<Left, ParentChildAddedEvent> ||
        std::is_same_v<Left, ParentChildRemovedEvent>) {
        return left.parent_id == right.parent_id &&
               left.child_id == right.child_id;
    } else if constexpr (std::is_same_v<Left, RuntimeChangedEvent>) {
        return left.object_id == right.object_id &&
               left.value == right.value;
    } else if constexpr (std::is_same_v<Left, PropertyChangedEvent>) {
        return left.object_id == right.object_id &&
               left.property_id == right.property_id &&
               left.value == right.value && left.removed == right.removed;
    } else if constexpr (std::is_same_v<Left, DamageEvent>) {
        return left.target_id == right.target_id &&
               left.source_id == right.source_id &&
               left.damage_type == right.damage_type &&
               same_float(left.amount, right.amount);
    } else if constexpr (std::is_same_v<Left, DestroyedEvent>) {
        return left.object_id == right.object_id &&
               left.reason == right.reason;
    } else if constexpr (std::is_same_v<Left, FxEvent>) {
        return left.object_id == right.object_id &&
               left.effect_id == right.effect_id &&
               left.payload == right.payload;
    } else {
        static_assert(std::is_same_v<Left, void>,
                      "unhandled world mutation event type");
    }
}

bool same_event(const WorldMutationEvent& left,
                const WorldMutationEvent& right) noexcept
{
    return std::visit(
        [](const auto& left_value, const auto& right_value) noexcept {
            return same_event_type(left_value, right_value);
        },
        left, right);
}

bool valid_replication_source(const ReplicationSourceContext source) noexcept
{
    switch (source) {
    case ReplicationSourceContext::LocalAuthoritative:
    case ReplicationSourceContext::NetworkReplay:
    case ReplicationSourceContext::MapLoad:
    case ReplicationSourceContext::Teardown:
        return true;
    }
    return false;
}

bool valid_transport_role(const TransportRole role) noexcept
{
    return role == TransportRole::Client || role == TransportRole::Server;
}

} // namespace

WorldMutationApplier::WorldMutationApplier(
    WorldMutationEngineCallbacks callbacks,
    WorldMutationApplierConfig config)
    : m_callbacks(std::move(callbacks)),
      m_config(std::move(config)),
      m_replay_guard_factory(m_config.replay_guard_factory),
      m_epoch(m_config.epoch),
      m_revision(m_config.initial_revision)
{
    if (m_epoch == kInvalidWorldEpoch)
        m_epoch = 1;
    m_objects.reserve(32);
    m_retired_ids.reserve(32);
}

WorldMutationApplier::WorldMutationApplier(
    WorldMutationEngineCallbacks callbacks, WorldObserver& observer,
    WorldMutationApplierConfig config)
    : WorldMutationApplier(std::move(callbacks), std::move(config))
{
    m_replay_observer = &observer;
}

WorldMutationApplyResult WorldMutationApplier::validate_source(
    const WorldMutationSource& source) const noexcept
{
    if (!valid_transport_role(source.role) ||
        !valid_transport_role(m_config.expected_source_role))
        return WorldMutationApplyResult::InvalidRole;
    if (source.role != m_config.expected_source_role)
        return WorldMutationApplyResult::InvalidRole;
    if (!valid_replication_source(source.context) ||
        !valid_replication_source(m_config.expected_source))
        return WorldMutationApplyResult::InvalidSource;
    if (source.context != m_config.expected_source)
        return WorldMutationApplyResult::InvalidSource;
    if (m_config.require_peer && source.peer == kInvalidPeer)
        return WorldMutationApplyResult::InvalidSource;
    if (m_config.expected_peer != kInvalidPeer &&
        source.peer != m_config.expected_peer)
        return WorldMutationApplyResult::InvalidSource;
    return WorldMutationApplyResult::Applied;
}

const WorldMutationObjectState* WorldMutationApplier::find_active(
    const HostObjectId object_id) const noexcept
{
    if (object_id == kInvalidHostObjectId)
        return nullptr;
    for (const WorldMutationObjectState& object : m_objects) {
        if (object.active && object.object_id == object_id)
            return &object;
    }
    return nullptr;
}

WorldMutationObjectState* WorldMutationApplier::find_active(
    const HostObjectId object_id) noexcept
{
    if (object_id == kInvalidHostObjectId)
        return nullptr;
    for (WorldMutationObjectState& object : m_objects) {
        if (object.active && object.object_id == object_id)
            return &object;
    }
    return nullptr;
}

bool WorldMutationApplier::has_any_object(
    const HostObjectId object_id) const noexcept
{
    for (const WorldMutationObjectState& object : m_objects) {
        if (object.object_id == object_id)
            return true;
    }
    return was_retired(object_id);
}

bool WorldMutationApplier::was_retired(
    const HostObjectId object_id) const noexcept
{
    for (const HostObjectId retired : m_retired_ids) {
        if (retired == object_id)
            return true;
    }
    return false;
}

bool WorldMutationApplier::has_active_child(
    const HostObjectId object_id) const noexcept
{
    for (const WorldMutationObjectState& object : m_objects) {
        if (object.active && object.parent_id == object_id)
            return true;
    }
    return false;
}

WorldMutationApplyResult WorldMutationApplier::validate_event(
    const WorldMutationEvent& event) const noexcept
{
    return std::visit(
        [this](const auto& value) noexcept {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ObjectCreatedEvent>) {
                if (value.object_id == kInvalidHostObjectId)
                    return WorldMutationApplyResult::InvalidEvent;
                if (has_any_object(value.object_id))
                    return WorldMutationApplyResult::ObjectIdCollision;
            } else if constexpr (std::is_same_v<Event, ObjectDespawnedEvent>) {
                const WorldMutationObjectState* object =
                    find_active(value.object_id);
                if (object == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
                if (object->parent_id != kInvalidHostObjectId ||
                    has_active_child(value.object_id))
                    return WorldMutationApplyResult::InvalidRelation;
            } else if constexpr (
                std::is_same_v<Event, ParentChildAddedEvent>) {
                if (value.parent_id == kInvalidHostObjectId ||
                    value.child_id == kInvalidHostObjectId ||
                    value.parent_id == value.child_id)
                    return WorldMutationApplyResult::InvalidRelation;
                if (find_active(value.parent_id) == nullptr ||
                    find_active(value.child_id) == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
                const WorldMutationObjectState* child =
                    find_active(value.child_id);
                if (child->parent_id != kInvalidHostObjectId)
                    return WorldMutationApplyResult::InvalidRelation;
            } else if constexpr (
                std::is_same_v<Event, ParentChildRemovedEvent>) {
                if (value.parent_id == kInvalidHostObjectId ||
                    value.child_id == kInvalidHostObjectId ||
                    value.parent_id == value.child_id)
                    return WorldMutationApplyResult::InvalidRelation;
                if (find_active(value.parent_id) == nullptr ||
                    find_active(value.child_id) == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
                const WorldMutationObjectState* child =
                    find_active(value.child_id);
                if (child->parent_id != value.parent_id)
                    return WorldMutationApplyResult::InvalidRelation;
            } else if constexpr (
                std::is_same_v<Event, RuntimeChangedEvent> ||
                std::is_same_v<Event, PropertyChangedEvent> ||
                std::is_same_v<Event, DestroyedEvent> ||
                std::is_same_v<Event, FxEvent>) {
                if (value.object_id == kInvalidHostObjectId)
                    return WorldMutationApplyResult::InvalidEvent;
                if (find_active(value.object_id) == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
            } else if constexpr (std::is_same_v<Event, DamageEvent>) {
                if (value.target_id == kInvalidHostObjectId ||
                    value.source_id == kInvalidHostObjectId ||
                    !std::isfinite(value.amount))
                    return WorldMutationApplyResult::InvalidEvent;
                if (find_active(value.target_id) == nullptr ||
                    find_active(value.source_id) == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
            } else {
                static_assert(std::is_same_v<Event, void>,
                              "unhandled world mutation event type");
            }
            return WorldMutationApplyResult::Applied;
        },
        event);
}

WorldMutationApplyResult WorldMutationApplier::install_object(
    const HostObjectId object_id, const ObjectTypeId type_id,
    const HostObjectId parent_id)
{
    if (object_id == kInvalidHostObjectId)
        return WorldMutationApplyResult::InvalidEvent;
    if (has_any_object(object_id))
        return WorldMutationApplyResult::ObjectIdCollision;
    if (parent_id != kInvalidHostObjectId &&
        find_active(parent_id) == nullptr)
        return WorldMutationApplyResult::UnknownObject;
    if (parent_id == object_id)
        return WorldMutationApplyResult::InvalidRelation;

    HostObjectId current = parent_id;
    for (std::size_t steps = 0; current != kInvalidHostObjectId &&
                                  steps <= m_objects.size(); ++steps) {
        if (current == object_id)
            return WorldMutationApplyResult::InvalidRelation;
        const WorldMutationObjectState* parent = find_active(current);
        if (parent == nullptr)
            return WorldMutationApplyResult::UnknownObject;
        current = parent->parent_id;
    }

    try {
        m_objects.push_back(
            WorldMutationObjectState{object_id, type_id, parent_id, true,
                                     false});
    } catch (...) {
        return WorldMutationApplyResult::CallbackFailed;
    }
    return WorldMutationApplyResult::Applied;
}

WorldMutationApplyResult WorldMutationApplier::prepare_state(
    const WorldMutationEvent& event, Undo& undo)
{
    return std::visit(
        [this, &undo](const auto& value) {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ObjectCreatedEvent>) {
                try {
                    m_objects.push_back(WorldMutationObjectState{
                        value.object_id, value.type_id,
                        kInvalidHostObjectId, true, false});
                } catch (...) {
                    return WorldMutationApplyResult::CallbackFailed;
                }
                undo.kind = UndoKind::Created;
                undo.index = m_objects.size() - 1;
            } else if constexpr (std::is_same_v<Event, ObjectDespawnedEvent>) {
                WorldMutationObjectState* object = find_active(value.object_id);
                if (object == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
                const std::size_t index = static_cast<std::size_t>(
                    object - m_objects.data());
                try {
                    m_retired_ids.push_back(value.object_id);
                } catch (...) {
                    return WorldMutationApplyResult::CallbackFailed;
                }
                object->active = false;
                undo.kind = UndoKind::Despawned;
                undo.index = index;
                undo.retired_added = true;
            } else if constexpr (
                std::is_same_v<Event, ParentChildAddedEvent> ||
                std::is_same_v<Event, ParentChildRemovedEvent>) {
                WorldMutationObjectState* child = find_active(value.child_id);
                if (child == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
                undo.kind = UndoKind::ParentChanged;
                undo.index = static_cast<std::size_t>(child - m_objects.data());
                undo.old_parent = child->parent_id;
                if constexpr (std::is_same_v<Event, ParentChildAddedEvent>)
                    child->parent_id = value.parent_id;
                else
                    child->parent_id = kInvalidHostObjectId;
            } else if constexpr (std::is_same_v<Event, DestroyedEvent>) {
                WorldMutationObjectState* object = find_active(value.object_id);
                if (object == nullptr)
                    return WorldMutationApplyResult::UnknownObject;
                undo.kind = UndoKind::DestroyedChanged;
                undo.index = static_cast<std::size_t>(object - m_objects.data());
                undo.old_destroyed = object->destroyed;
                object->destroyed = true;
            }
            return WorldMutationApplyResult::Applied;
        },
        event);
}

void WorldMutationApplier::undo_state(const Undo& undo) noexcept
{
    switch (undo.kind) {
    case UndoKind::None:
        return;
    case UndoKind::Created:
        if (undo.index + 1 == m_objects.size())
            m_objects.pop_back();
        return;
    case UndoKind::Despawned:
        if (undo.index < m_objects.size())
            m_objects[undo.index].active = true;
        if (undo.retired_added && !m_retired_ids.empty())
            m_retired_ids.pop_back();
        return;
    case UndoKind::ParentChanged:
        if (undo.index < m_objects.size())
            m_objects[undo.index].parent_id = undo.old_parent;
        return;
    case UndoKind::DestroyedChanged:
        if (undo.index < m_objects.size())
            m_objects[undo.index].destroyed = undo.old_destroyed;
        return;
    }
}

bool WorldMutationApplier::has_required_callbacks(
    const WorldMutationEvent& event) const noexcept
{
    return std::visit(
        [this](const auto& value) noexcept {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ObjectCreatedEvent>)
                return static_cast<bool>(m_callbacks.create);
            else if constexpr (std::is_same_v<Event, ObjectDespawnedEvent>)
                return static_cast<bool>(m_callbacks.despawn);
            else if constexpr (std::is_same_v<Event, ParentChildAddedEvent>)
                return static_cast<bool>(m_callbacks.add_child);
            else if constexpr (std::is_same_v<Event, ParentChildRemovedEvent>)
                return static_cast<bool>(m_callbacks.remove_child);
            else if constexpr (std::is_same_v<Event, RuntimeChangedEvent>)
                return static_cast<bool>(m_callbacks.runtime);
            else if constexpr (std::is_same_v<Event, PropertyChangedEvent>)
                return static_cast<bool>(m_callbacks.property);
            else if constexpr (std::is_same_v<Event, DamageEvent>)
                return static_cast<bool>(m_callbacks.damage);
            else if constexpr (std::is_same_v<Event, DestroyedEvent>)
                return static_cast<bool>(m_callbacks.destroyed);
            else if constexpr (std::is_same_v<Event, FxEvent>)
                return static_cast<bool>(m_callbacks.fx);
            else
                return false;
        },
        event);
}

bool WorldMutationApplier::has_transaction_hooks() const noexcept
{
    return static_cast<bool>(m_callbacks.begin_transaction) ||
           static_cast<bool>(m_callbacks.commit_transaction) ||
           static_cast<bool>(m_callbacks.rollback_transaction);
}

bool WorldMutationApplier::invoke(const WorldMutationEvent& event) const
{
    return std::visit(
        [this](const auto& value) {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ObjectCreatedEvent>)
                return m_callbacks.create(value);
            else if constexpr (std::is_same_v<Event, ObjectDespawnedEvent>)
                return m_callbacks.despawn(value);
            else if constexpr (std::is_same_v<Event, ParentChildAddedEvent>)
                return m_callbacks.add_child(value);
            else if constexpr (std::is_same_v<Event, ParentChildRemovedEvent>)
                return m_callbacks.remove_child(value);
            else if constexpr (std::is_same_v<Event, RuntimeChangedEvent>)
                return m_callbacks.runtime(value);
            else if constexpr (std::is_same_v<Event, PropertyChangedEvent>)
                return m_callbacks.property(value);
            else if constexpr (std::is_same_v<Event, DamageEvent>)
                return m_callbacks.damage(value);
            else if constexpr (std::is_same_v<Event, DestroyedEvent>)
                return m_callbacks.destroyed(value);
            else if constexpr (std::is_same_v<Event, FxEvent>)
                return m_callbacks.fx(value);
            else
                return false;
        },
        event);
}

bool WorldMutationApplier::same_as_last(
    const WorldMutationEvent& event) const noexcept
{
    return m_last_event.has_value() && same_event(*m_last_event, event);
}

WorldMutationApplyResult WorldMutationApplier::apply_prevalidated(
    const WorldMutationEnvelope& envelope)
{
    if (!has_required_callbacks(envelope.event))
        return WorldMutationApplyResult::MissingCallback;
    if (has_transaction_hooks() &&
        (!m_callbacks.begin_transaction || !m_callbacks.commit_transaction ||
         !m_callbacks.rollback_transaction))
        return WorldMutationApplyResult::InvalidConfiguration;

    Undo undo{};
    const WorldMutationApplyResult prepared =
        prepare_state(envelope.event, undo);
    if (prepared != WorldMutationApplyResult::Applied)
        return prepared;

    std::optional<WorldMutationEvent> previous_last = std::move(m_last_event);
    try {
        m_last_event.emplace(envelope.event);
    } catch (...) {
        m_last_event = std::move(previous_last);
        undo_state(undo);
        return WorldMutationApplyResult::CallbackFailed;
    }

    std::optional<ReplayGuard> replay_guard;
    try {
        if (m_replay_guard_factory)
            replay_guard.emplace(m_replay_guard_factory());
        else if (m_replay_observer != nullptr)
            replay_guard.emplace(m_replay_observer->suppress_replay());
    } catch (...) {
        m_last_event = std::move(previous_last);
        undo_state(undo);
        return WorldMutationApplyResult::ReplayGuardFailed;
    }

    const bool transactional = has_transaction_hooks();
    bool transaction_started = false;
    try {
        if (transactional) {
            if (!m_callbacks.begin_transaction()) {
                m_last_event = std::move(previous_last);
                undo_state(undo);
                return WorldMutationApplyResult::TransactionBeginFailed;
            }
            transaction_started = true;
        }

        if (!invoke(envelope.event)) {
            bool rollback_succeeded = true;
            if (transaction_started)
                rollback_succeeded = m_callbacks.rollback_transaction();
            undo_state(undo);
            m_last_event = std::move(previous_last);
            return rollback_succeeded
                       ? WorldMutationApplyResult::CallbackFailed
                       : WorldMutationApplyResult::RollbackFailed;
        }

        if (transactional && !m_callbacks.commit_transaction()) {
            const bool rollback_succeeded =
                m_callbacks.rollback_transaction();
            undo_state(undo);
            m_last_event = std::move(previous_last);
            return rollback_succeeded
                       ? WorldMutationApplyResult::TransactionCommitFailed
                       : WorldMutationApplyResult::RollbackFailed;
        }
    } catch (...) {
        bool rollback_succeeded = true;
        if (transaction_started) {
            try {
                rollback_succeeded = m_callbacks.rollback_transaction();
            } catch (...) {
                rollback_succeeded = false;
            }
        }
        undo_state(undo);
        m_last_event = std::move(previous_last);
        return rollback_succeeded ? WorldMutationApplyResult::CallbackFailed
                                  : WorldMutationApplyResult::RollbackFailed;
    }

    m_revision = envelope.revision;
    return WorldMutationApplyResult::Applied;
}

WorldMutationApplyResult WorldMutationApplier::apply(
    const WorldMutationEnvelope& envelope)
{
    const WorldMutationApplyResult source_result =
        validate_source(envelope.source);
    if (source_result != WorldMutationApplyResult::Applied)
        return source_result;
    if (envelope.epoch == kInvalidWorldEpoch ||
        envelope.revision == kInvalidWorldRevision)
        return WorldMutationApplyResult::InvalidRevision;
    if (envelope.epoch != m_epoch)
        return WorldMutationApplyResult::WrongEpoch;

    if (envelope.revision == m_revision) {
        if (same_as_last(envelope.event))
            return WorldMutationApplyResult::Duplicate;
        return WorldMutationApplyResult::RevisionConflict;
    }
    if (envelope.revision < m_revision)
        return WorldMutationApplyResult::Reordered;
    if (m_revision == (std::numeric_limits<WorldRevision>::max)() ||
        envelope.revision != m_revision + 1)
        return WorldMutationApplyResult::Gap;

    const WorldMutationApplyResult event_result =
        validate_event(envelope.event);
    if (event_result != WorldMutationApplyResult::Applied)
        return event_result;
    return apply_prevalidated(envelope);
}

WorldMutationApplyResult WorldMutationApplier::apply(
    const WorldMutationEvent& event, const WorldEpoch epoch,
    const WorldRevision revision, const WorldMutationSource source)
{
    return apply(WorldMutationEnvelope{epoch, revision, source, event});
}

WorldMutationApplyResult WorldMutationApplier::apply(
    const WorldMutationEvent& event)
{
    const WorldRevision next = next_revision();
    if (next == kInvalidWorldRevision)
        return WorldMutationApplyResult::InvalidRevision;
    return apply(WorldMutationEnvelope{
        m_epoch, next,
        {m_config.expected_source_role, m_config.expected_source,
         m_config.expected_peer},
        event});
}

WorldMutationApplyResult WorldMutationApplier::apply(
    const WorldDelta& delta, const WorldMutationSource source)
{
    WorldMutationEvent event{};
    if (!world_mutation_codec_succeeded(
            decode_world_mutation(ByteView{delta.payload}, event)))
        return WorldMutationApplyResult::InvalidEvent;
    return apply(WorldMutationEnvelope{delta.epoch, delta.revision, source,
                                       std::move(event)});
}

void WorldMutationApplier::reset(
    const WorldEpoch epoch, const WorldRevision initial_revision) noexcept
{
    m_objects.clear();
    m_retired_ids.clear();
    m_last_event.reset();
    m_epoch = epoch == kInvalidWorldEpoch ? 1 : epoch;
    m_revision = initial_revision;
}

WorldRevision WorldMutationApplier::next_revision() const noexcept
{
    if (m_revision == (std::numeric_limits<WorldRevision>::max)())
        return kInvalidWorldRevision;
    return m_revision + 1;
}

bool WorldMutationApplier::contains(const HostObjectId object_id) const noexcept
{
    return find_active(object_id) != nullptr;
}

std::optional<WorldMutationObjectState> WorldMutationApplier::object_state(
    const HostObjectId object_id) const noexcept
{
    const WorldMutationObjectState* object = find_active(object_id);
    if (object == nullptr)
        return std::nullopt;
    return *object;
}

std::optional<HostObjectId> WorldMutationApplier::parent_for(
    const HostObjectId object_id) const noexcept
{
    const WorldMutationObjectState* object = find_active(object_id);
    if (object == nullptr)
        return std::nullopt;
    return object->parent_id;
}

std::size_t WorldMutationApplier::object_count() const noexcept
{
    std::size_t count = 0;
    for (const WorldMutationObjectState& object : m_objects)
        if (object.active)
            ++count;
    return count;
}

} // namespace kraken::net
