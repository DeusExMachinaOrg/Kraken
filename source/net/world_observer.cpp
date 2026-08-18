#include "net/world_observer.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

namespace kraken::net {

namespace {

bool same_float(const float left, const float right) noexcept
{
    std::uint32_t left_bits = 0;
    std::uint32_t right_bits = 0;
    static_assert(sizeof(left_bits) == sizeof(left));
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
        left,
        right);
}

bool valid_event(const WorldMutationEvent& event) noexcept
{
    return std::visit(
        [](const auto& value) noexcept {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ObjectCreatedEvent> ||
                          std::is_same_v<Event, ObjectDespawnedEvent> ||
                          std::is_same_v<Event, RuntimeChangedEvent> ||
                          std::is_same_v<Event, PropertyChangedEvent> ||
                          std::is_same_v<Event, DestroyedEvent> ||
                          std::is_same_v<Event, FxEvent>) {
                return value.object_id != kInvalidHostObjectId;
            } else if constexpr (
                std::is_same_v<Event, ParentChildAddedEvent> ||
                std::is_same_v<Event, ParentChildRemovedEvent>) {
                return value.parent_id != kInvalidHostObjectId &&
                       value.child_id != kInvalidHostObjectId &&
                       value.parent_id != value.child_id;
            } else if constexpr (std::is_same_v<Event, DamageEvent>) {
                return value.target_id != kInvalidHostObjectId;
            } else {
                static_assert(std::is_same_v<Event, void>,
                              "unhandled world mutation event type");
            }
        },
        event);
}

bool valid_record(const ObjectRecord& record) noexcept
{
    if (record.object_id == kInvalidHostObjectId ||
        record.parent_id == record.object_id)
        return false;

    for (std::size_t index = 1; index < record.properties.size(); ++index) {
        if (record.properties[index - 1].property_id ==
            record.properties[index].property_id)
            return false;
    }
    return true;
}

ObjectRecord normalized_record(const ObjectRecord& input)
{
    ObjectRecord result = input;
    std::sort(result.properties.begin(), result.properties.end(),
              [](const PropertySnapshot& left, const PropertySnapshot& right) {
                  return left.property_id < right.property_id;
              });
    return result;
}

const ObjectRecord* find_record(const std::vector<ObjectRecord>& records,
                                const HostObjectId object_id) noexcept
{
    const auto found = std::lower_bound(
        records.begin(), records.end(), object_id,
        [](const ObjectRecord& record, const HostObjectId id) {
            return record.object_id < id;
        });
    return found != records.end() && found->object_id == object_id
               ? &*found
               : nullptr;
}

} // namespace

WorldMutationKind mutation_kind(const WorldMutationEvent& event) noexcept
{
    return std::visit(
        [](const auto& value) noexcept {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ObjectCreatedEvent>)
                return WorldMutationKind::ObjectCreated;
            else if constexpr (std::is_same_v<Event, ObjectDespawnedEvent>)
                return WorldMutationKind::ObjectDespawned;
            else if constexpr (std::is_same_v<Event, ParentChildAddedEvent>)
                return WorldMutationKind::ParentChildAdded;
            else if constexpr (
                std::is_same_v<Event, ParentChildRemovedEvent>)
                return WorldMutationKind::ParentChildRemoved;
            else if constexpr (std::is_same_v<Event, RuntimeChangedEvent>)
                return WorldMutationKind::RuntimeChanged;
            else if constexpr (std::is_same_v<Event, PropertyChangedEvent>)
                return WorldMutationKind::PropertyChanged;
            else if constexpr (std::is_same_v<Event, DamageEvent>)
                return WorldMutationKind::Damage;
            else if constexpr (std::is_same_v<Event, DestroyedEvent>)
                return WorldMutationKind::Destroyed;
            else
                return WorldMutationKind::Fx;
        },
        event);
}

ReplayGuard::ReplayGuard(std::shared_ptr<detail::WorldReplayState> state)
    noexcept
    : m_state(std::move(state))
{
    if (m_state)
        ++m_state->depth;
}

ReplayGuard::ReplayGuard(ReplayGuard&& other) noexcept
    : m_state(std::move(other.m_state))
{
}

ReplayGuard& ReplayGuard::operator=(ReplayGuard&& other) noexcept
{
    if (this == &other)
        return *this;

    if (m_state && m_state->depth != 0)
        --m_state->depth;
    m_state = std::move(other.m_state);
    return *this;
}

ReplayGuard::~ReplayGuard() noexcept
{
    if (m_state && m_state->depth != 0)
        --m_state->depth;
}

WorldObserver::WorldObserver(Sink sink)
    : m_sink(std::move(sink)),
      m_replay_state(std::make_shared<detail::WorldReplayState>())
{
}

void WorldObserver::set_sink(Sink sink)
{
    m_sink = std::move(sink);
}

ObservationResult WorldObserver::publish(
    const WorldMutationEvent& event)
{
    return publish_internal(event);
}

ObservationResult WorldObserver::publish_internal(
    const WorldMutationEvent& event)
{
    if (!valid_event(event))
        return ObservationResult::Invalid;
    if (replay_suppressed())
        return ObservationResult::Suppressed;
    if (std::any_of(m_seen.begin(), m_seen.end(),
                    [&event](const WorldMutationEvent& seen) {
                        return same_event(seen, event);
                    }))
        return ObservationResult::Duplicate;

    // Remember a no-sink event as well.  If a sink is attached later in this
    // pass, the same engine callback is still a duplicate, not a replay.
    m_seen.push_back(event);
    if (!m_sink)
        return ObservationResult::NoSink;

    try {
        m_sink(event);
    } catch (...) {
        // An engine hook must not let an injected observer callback unwind
        // through the engine ABI.  The event remains consumed for this pass.
        return ObservationResult::SinkFailed;
    }
    return ObservationResult::Emitted;
}

void WorldObserver::begin_observation_pass() noexcept
{
    m_seen.clear();
    m_observation_pass_started = true;
}

FrameObservationResult WorldObserver::observe_frame(
    const std::span<const ObjectRecord> records)
{
    // A native post-call observer may have published into this pass before
    // the post-update graph sample. Preserve that cache so the frame diff
    // reconciles against it instead of appending a duplicate journal delta.
    if (!m_observation_pass_started)
        m_seen.clear();
    m_observation_pass_started = false;
    FrameObservationResult result{};

    std::vector<ObjectRecord> current;
    current.reserve(records.size());
    for (const ObjectRecord& record : records) {
        ObjectRecord normalized = normalized_record(record);
        if (!valid_record(normalized)) {
            ++result.invalid_records;
            continue;
        }
        current.push_back(std::move(normalized));
    }
    std::sort(current.begin(), current.end(),
              [](const ObjectRecord& left, const ObjectRecord& right) {
                  return left.object_id < right.object_id;
              });

    // Duplicate host IDs cannot be reconciled without inventing identity.
    // Drop the entire run so input order cannot choose a winner.
    std::vector<ObjectRecord> unique_current;
    unique_current.reserve(current.size());
    for (std::size_t index = 0; index < current.size();) {
        std::size_t end = index + 1;
        while (end < current.size() &&
               current[end].object_id == current[index].object_id)
            ++end;
        if (end - index == 1) {
            unique_current.push_back(std::move(current[index]));
        } else {
            result.invalid_records += end - index;
        }
        index = end;
    }

    // A partial/ambiguous frame must not turn missing records into false
    // despawns.  Keep the previous baseline intact until a complete frame is
    // available.
    if (result.invalid_records != 0)
        return result;

    auto account = [&result](const ObservationResult status) {
        switch (status) {
        case ObservationResult::Emitted:
            ++result.emitted;
            break;
        case ObservationResult::Duplicate:
            ++result.duplicates;
            break;
        case ObservationResult::Suppressed:
            ++result.suppressed;
            break;
        case ObservationResult::NoSink:
            ++result.no_sink;
            break;
        case ObservationResult::SinkFailed:
            ++result.sink_failures;
            break;
        case ObservationResult::Invalid:
            ++result.invalid_records;
            break;
        }
    };

    // Keep publication local to preserve per-frame result accounting.
    auto publish_event = [this, &account](const WorldMutationEvent& event) {
        account(publish(event));
    };

    if (!m_have_previous_frame) {
        // Creation is a graph phase: every object exists before any nested
        // parent link is announced, even when a child ID sorts first.
        for (const ObjectRecord& record : unique_current)
            publish_event(ObjectCreatedEvent{record.object_id,
                                             record.type_id});
        for (const ObjectRecord& record : unique_current) {
            if (record.parent_id != kInvalidHostObjectId)
                publish_event(ParentChildAddedEvent{record.parent_id,
                                                    record.object_id});
        }
        // Creation is not complete until the object's initial native/runtime
        // state and properties are delivered.  In particular, the runtime
        // event (including an intentionally empty value) is the deterministic
        // completion boundary used by engine adapters that create an object
        // with suspended PostLoad.
        for (const ObjectRecord& record : unique_current) {
            publish_event(RuntimeChangedEvent{record.object_id,
                                              record.runtime});
            for (const PropertySnapshot& property : record.properties) {
                publish_event(PropertyChangedEvent{
                    record.object_id, property.property_id,
                    property.value, false});
            }
        }
        m_previous_records = std::move(unique_current);
        m_have_previous_frame = true;
        return result;
    }

    // Removed relationships precede despawns, and creations precede new
    // relationships.  The remaining phases are all sorted by host ID.
    for (const ObjectRecord& old_record : m_previous_records) {
        if (find_record(unique_current, old_record.object_id) == nullptr) {
            if (old_record.parent_id != kInvalidHostObjectId) {
                publish_event(ParentChildRemovedEvent{old_record.parent_id,
                                                       old_record.object_id});
            }
            publish_event(ObjectDespawnedEvent{old_record.object_id});
        }
    }

    for (const ObjectRecord& record : unique_current) {
        if (find_record(m_previous_records, record.object_id) == nullptr) {
            publish_event(ObjectCreatedEvent{record.object_id,
                                             record.type_id});
        }
    }

    for (const ObjectRecord& record : unique_current) {
        if (find_record(m_previous_records, record.object_id) == nullptr &&
            record.parent_id != kInvalidHostObjectId)
            publish_event(ParentChildAddedEvent{record.parent_id,
                                                record.object_id});
    }

    for (const ObjectRecord& record : unique_current) {
        const ObjectRecord* old_record =
            find_record(m_previous_records, record.object_id);
        if (old_record == nullptr || old_record->parent_id == record.parent_id)
            continue;

        if (old_record->parent_id != kInvalidHostObjectId) {
            publish_event(ParentChildRemovedEvent{old_record->parent_id,
                                                   record.object_id});
        }
        if (record.parent_id != kInvalidHostObjectId) {
            publish_event(ParentChildAddedEvent{record.parent_id,
                                                record.object_id});
        }
    }

    for (const ObjectRecord& record : unique_current) {
        const ObjectRecord* old_record =
            find_record(m_previous_records, record.object_id);
        if (old_record == nullptr) {
            publish_event(RuntimeChangedEvent{record.object_id,
                                              record.runtime});
            for (const PropertySnapshot& property : record.properties) {
                publish_event(PropertyChangedEvent{
                    record.object_id, property.property_id,
                    property.value, false});
            }
            continue;
        }

        if (old_record->runtime != record.runtime) {
            publish_event(RuntimeChangedEvent{record.object_id,
                                              record.runtime});
        }

        std::size_t current_property_index = 0;
        std::size_t old_property_index = 0;
        while (current_property_index < record.properties.size() ||
               old_property_index < old_record->properties.size()) {
            const PropertySnapshot* current_property = nullptr;
            const PropertySnapshot* old_property = nullptr;

            if (current_property_index < record.properties.size())
                current_property = &record.properties[current_property_index];
            if (old_property_index < old_record->properties.size())
                old_property = &old_record->properties[old_property_index];

            if (old_property == nullptr ||
                (current_property != nullptr &&
                 current_property->property_id < old_property->property_id)) {
                publish_event(PropertyChangedEvent{
                    record.object_id, current_property->property_id,
                    current_property->value, false});
                ++current_property_index;
                continue;
            }

            if (current_property == nullptr ||
                old_property->property_id < current_property->property_id) {
                publish_event(PropertyChangedEvent{
                    record.object_id, old_property->property_id, {}, true});
                ++old_property_index;
                continue;
            }

            if (current_property->value != old_property->value) {
                publish_event(PropertyChangedEvent{
                    record.object_id, current_property->property_id,
                    current_property->value, false});
            }
            ++current_property_index;
            ++old_property_index;
        }
    }

    m_previous_records = std::move(unique_current);
    return result;
}

ReplayGuard WorldObserver::suppress_replay() noexcept
{
    return ReplayGuard{m_replay_state};
}

bool WorldObserver::replay_suppressed() const noexcept
{
    return m_replay_state && m_replay_state->depth != 0;
}

void WorldObserver::reset() noexcept
{
    m_seen.clear();
    m_previous_records.clear();
    m_have_previous_frame = false;
    m_observation_pass_started = false;
}

EngineBinding::EngineBinding(WorldObserver& observer,
                             EngineBindingConfig config)
    : m_observer(&observer), m_config(std::move(config))
{
}

bool EngineBinding::observe_add_child_succeeded(
    const void* parent, const void* child, const bool original_succeeded)
{
    if (!original_succeeded || !m_observer ||
        !m_config.identity_from_object)
        return false;
    try {
        if (m_config.parent_from_object &&
            m_config.parent_from_object(child) != parent)
            return false;
        const std::optional<HostObjectId> parent_id =
            m_config.identity_from_object(parent);
        const std::optional<HostObjectId> child_id =
            m_config.identity_from_object(child);
        if (!parent_id.has_value() || !child_id.has_value())
            return false;
        return m_observer->publish(ParentChildAddedEvent{
                   *parent_id, *child_id}) == ObservationResult::Emitted;
    } catch (...) {
        return false;
    }
}

bool EngineBinding::observe_chest_add_child_succeeded(
    const void* chest, const void* child)
{
    return observe_add_child_succeeded(chest, child, true);
}

bool EngineBinding::observe_remove_child_succeeded(
    const void* parent, const void* child, const bool original_succeeded)
{
    if (!original_succeeded || !m_observer || !m_config.identity_from_object)
        return false;
    try {
        if (m_config.parent_from_object &&
            m_config.parent_from_object(child) == parent)
            return false;
        const std::optional<HostObjectId> parent_id =
            m_config.identity_from_object(parent);
        const std::optional<HostObjectId> child_id =
            m_config.identity_from_object(child);
        if (!parent_id.has_value() || !child_id.has_value())
            return false;
        return m_observer->publish(ParentChildRemovedEvent{
                   *parent_id, *child_id}) == ObservationResult::Emitted;
    } catch (...) {
        return false;
    }
}

bool EngineBinding::observe_chest_remove_child_succeeded(
    const void* chest, const void* child, const bool original_succeeded)
{
    return observe_remove_child_succeeded(chest, child, original_succeeded);
}

bool EngineBinding::observe_obj_container_create_succeeded(
    const void* object)
{
    if (!m_observer || !m_config.identity_from_object)
        return false;
    try {
        const std::optional<HostObjectId> object_id =
            m_config.identity_from_object(object);
        if (!object_id.has_value())
            return false;
        const ObjectTypeId type_id = m_config.type_from_object
                                         ? m_config.type_from_object(object)
                                         : 0;
        return observe_obj_container_create_succeeded(*object_id, type_id);
    } catch (...) {
        return false;
    }
}

bool EngineBinding::observe_obj_container_create_succeeded(
    const HostObjectId object_id, const ObjectTypeId type_id)
{
    if (!m_observer)
        return false;
    return m_observer->publish(ObjectCreatedEvent{object_id, type_id}) ==
           ObservationResult::Emitted;
}

bool EngineBinding::observe_obj_container_remove_succeeded(
    const void* object, const bool original_succeeded)
{
    if (!original_succeeded || !m_observer || !m_config.identity_from_object)
        return false;
    try {
        const std::optional<HostObjectId> object_id =
            m_config.identity_from_object(object);
        return object_id.has_value() &&
               observe_obj_container_remove_succeeded(*object_id, true);
    } catch (...) {
        return false;
    }
}

bool EngineBinding::observe_obj_container_remove_succeeded(
    const std::int32_t engine_obj_id, const bool original_succeeded)
{
    if (!original_succeeded || !m_observer ||
        !m_config.identity_from_engine_obj_id)
        return false;
    try {
        const std::optional<HostObjectId> object_id =
            m_config.identity_from_engine_obj_id(engine_obj_id);
        return object_id.has_value() &&
               observe_obj_container_remove_succeeded(*object_id, true);
    } catch (...) {
        return false;
    }
}

bool EngineBinding::observe_obj_container_remove_succeeded(
    const HostObjectId object_id, const bool original_succeeded)
{
    if (!original_succeeded || !m_observer)
        return false;
    return m_observer->publish(ObjectDespawnedEvent{object_id}) ==
           ObservationResult::Emitted;
}

} // namespace kraken::net
