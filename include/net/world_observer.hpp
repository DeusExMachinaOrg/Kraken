#ifndef KRAKEN_NET_WORLD_OBSERVER_HPP
#define KRAKEN_NET_WORLD_OBSERVER_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace kraken::net {

// A host assigns this identity and owns its lifetime.  The observer never
// derives identity from an engine pointer or from a process-local ObjId.
using HostObjectId = std::uint64_t;
using ObjectTypeId = std::uint32_t;
using PropertyId = std::uint32_t;

inline constexpr HostObjectId kInvalidHostObjectId = 0;

struct ObjectCreatedEvent {
    HostObjectId object_id = kInvalidHostObjectId;
    ObjectTypeId type_id = 0;
};

struct ObjectDespawnedEvent {
    HostObjectId object_id = kInvalidHostObjectId;
};

struct ParentChildAddedEvent {
    HostObjectId parent_id = kInvalidHostObjectId;
    HostObjectId child_id = kInvalidHostObjectId;
};

struct ParentChildRemovedEvent {
    HostObjectId parent_id = kInvalidHostObjectId;
    HostObjectId child_id = kInvalidHostObjectId;
};

struct RuntimeChangedEvent {
    HostObjectId object_id = kInvalidHostObjectId;
    std::vector<Byte> value;
};

struct PropertyChangedEvent {
    HostObjectId object_id = kInvalidHostObjectId;
    PropertyId property_id = 0;
    std::vector<Byte> value;
    // An empty value is a valid property value.  This flag distinguishes a
    // removed property from a property that was explicitly set to empty.
    bool removed = false;
};

struct DamageEvent {
    HostObjectId target_id = kInvalidHostObjectId;
    HostObjectId source_id = kInvalidHostObjectId;
    std::uint32_t damage_type = 0;
    float amount = 0.0f;
};

struct DestroyedEvent {
    HostObjectId object_id = kInvalidHostObjectId;
    std::uint32_t reason = 0;
};

struct FxEvent {
    HostObjectId object_id = kInvalidHostObjectId;
    std::uint32_t effect_id = 0;
    std::vector<Byte> payload;
};

using WorldMutationEvent = std::variant<
    ObjectCreatedEvent,
    ObjectDespawnedEvent,
    ParentChildAddedEvent,
    ParentChildRemovedEvent,
    RuntimeChangedEvent,
    PropertyChangedEvent,
    DamageEvent,
    DestroyedEvent,
    FxEvent>;

enum class WorldMutationKind : std::uint8_t {
    ObjectCreated,
    ObjectDespawned,
    ParentChildAdded,
    ParentChildRemoved,
    RuntimeChanged,
    PropertyChanged,
    Damage,
    Destroyed,
    Fx,
};

[[nodiscard]] WorldMutationKind mutation_kind(
    const WorldMutationEvent& event) noexcept;

struct PropertySnapshot {
    PropertyId property_id = 0;
    std::vector<Byte> value;
};

// A frame record is an engine-neutral snapshot.  The caller supplies the
// stable host identity; pointers and local engine object IDs are not part of
// this contract.  A zero parent denotes a root object.
struct ObjectRecord {
    HostObjectId object_id = kInvalidHostObjectId;
    ObjectTypeId type_id = 0;
    HostObjectId parent_id = kInvalidHostObjectId;
    std::vector<Byte> runtime;
    std::vector<PropertySnapshot> properties;
};

enum class ObservationResult : std::uint8_t {
    Emitted,
    Duplicate,
    Suppressed,
    NoSink,
    Invalid,
    SinkFailed,
};

struct FrameObservationResult {
    std::size_t emitted = 0;
    std::size_t duplicates = 0;
    std::size_t suppressed = 0;
    std::size_t no_sink = 0;
    std::size_t sink_failures = 0;
    std::size_t invalid_records = 0;
};

namespace detail {

struct WorldReplayState {
    std::size_t depth = 0;
};

} // namespace detail

class WorldObserver;

// A network snapshot/delta applier holds this guard for the complete period
// in which it mutates engine state.  Nested guards are supported, and the
// shared state keeps a late guard destructor harmless if an observer is
// destroyed first.
class ReplayGuard final {
public:
    ReplayGuard() noexcept = default;
    ReplayGuard(const ReplayGuard&) = delete;
    ReplayGuard& operator=(const ReplayGuard&) = delete;
    ReplayGuard(ReplayGuard&& other) noexcept;
    ReplayGuard& operator=(ReplayGuard&& other) noexcept;
    ~ReplayGuard() noexcept;

private:
    explicit ReplayGuard(std::shared_ptr<detail::WorldReplayState> state)
        noexcept;

    std::shared_ptr<detail::WorldReplayState> m_state;

    friend class WorldObserver;
};

class WorldObserver final {
public:
    using Sink = std::function<void(const WorldMutationEvent&)>;

    explicit WorldObserver(Sink sink = {});
    ~WorldObserver() = default;

    WorldObserver(const WorldObserver&) = delete;
    WorldObserver& operator=(const WorldObserver&) = delete;
    WorldObserver(WorldObserver&&) = delete;
    WorldObserver& operator=(WorldObserver&&) = delete;

    void set_sink(Sink sink);

    // publish() is the common entry point for network/runtime callbacks and
    // engine seams.  It suppresses duplicates for the current observation
    // pass before invoking the injected sink.
    [[nodiscard]] ObservationResult publish(
        const WorldMutationEvent& event);

    [[nodiscard]] ObservationResult observe(
        const WorldMutationEvent& event)
    { return publish(event); }

    template <typename Event>
    [[nodiscard]] ObservationResult observe(Event event)
    {
        return publish(WorldMutationEvent{std::move(event)});
    }

    // Hook users call once at the start of a frame/pass.  observe_frame()
    // calls this automatically.  The previous frame baseline is retained.
    void begin_observation_pass() noexcept;
    void clear_duplicate_history() noexcept
    { begin_observation_pass(); }

    [[nodiscard]] FrameObservationResult observe_frame(
        std::span<const ObjectRecord> records);

    [[nodiscard]] ReplayGuard suppress_replay() noexcept;
    [[nodiscard]] bool replay_suppressed() const noexcept;

    // Clears both the frame baseline and the current duplicate set.
    void reset() noexcept;

private:
    [[nodiscard]] ObservationResult publish_internal(
        const WorldMutationEvent& event);

    Sink m_sink;
    std::shared_ptr<detail::WorldReplayState> m_replay_state;
    std::vector<WorldMutationEvent> m_seen;
    std::vector<ObjectRecord> m_previous_records;
    bool m_have_previous_frame = false;
    bool m_observation_pass_started = false;
};

// These are the known LoRA locations for the optional post-call engine seam.
// They are deliberately data, not calls or detour instructions.  The binding
// below is safe only when its caller has already arranged a vtable/post-frame
// seam that preserves and invokes the original function.
struct EngineBindingAddresses {
    static inline constexpr std::uintptr_t chest_add_child_rva = 0x3eb920u;
    static inline constexpr std::uintptr_t chest_remove_child_rva = 0x3eb460u;
    static inline constexpr std::uintptr_t chest_vtable_rva = 0x5adba8u;
    static inline constexpr std::uintptr_t obj_vtable_rva = 0x59a380u;
    static inline constexpr std::uintptr_t obj_add_child_rva = 0x28db70u;
    static inline constexpr std::uintptr_t obj_remove_child_rva = 0x28e440u;
    static inline constexpr std::uintptr_t compound_vehicle_part_vtable_rva =
        0x59fed0u;
    static inline constexpr std::uintptr_t compound_vehicle_part_add_child_rva =
        0x2f4d20u;
    static inline constexpr std::uintptr_t compound_vehicle_part_remove_child_rva =
        0x2f43a0u;
    static inline constexpr std::uintptr_t vehicle_vtable_rva = 0x593010u;
    static inline constexpr std::uintptr_t vehicle_add_child_rva = 0x1e5e00u;
    static inline constexpr std::uintptr_t vehicle_remove_child_rva = 0x1e5f10u;
    // The native body is metadata only; it must never be globally detoured.
    static inline constexpr std::uintptr_t obj_container_create_new_object_body_rva =
        0x233c60u;
    // This is the narrow Lua exporter call-site, not the native body.
    static inline constexpr std::uintptr_t obj_container_create_new_object_lua_callsite_rva =
        0x233d0du;
    static inline constexpr std::uintptr_t obj_container_create_new_object_rva =
        0x233c60u;
    static inline constexpr std::uintptr_t obj_container_add_obj_id_to_remove_rva =
        0x28e350u;

    [[nodiscard]] static constexpr EngineBindingAddresses recommended()
        noexcept
    { return {}; }
};

struct EngineBindingConfig {
    // A resolver must return the host-assigned identity, never a pointer
    // cast.  Returning null means the operation cannot be observed safely.
    using ObjectIdentityResolver =
        std::function<std::optional<HostObjectId>(const void*)>;
    using EngineObjIdResolver =
        std::function<std::optional<HostObjectId>(std::int32_t)>;
    using ObjectTypeResolver =
        std::function<ObjectTypeId(const void*)>;
    using ParentResolver =
        std::function<const void*(const void*)>;

    ObjectIdentityResolver identity_from_object;
    EngineObjIdResolver identity_from_engine_obj_id;
    ObjectTypeResolver type_from_object;
    ParentResolver parent_from_object;
};

// Optional binding helpers.  These do not install detours, call an RVA, or
// depend on mod configuration.  The caller invokes them after a successfully called
// original operation (or from a post-frame diff), so an unsafe/unavailable
// original can simply leave the helper unused.
class EngineBinding final {
public:
    EngineBinding(WorldObserver& observer, EngineBindingConfig config = {});

    [[nodiscard]] bool observe_chest_add_child_succeeded(
        const void* chest, const void* child);
    [[nodiscard]] bool observe_add_child_succeeded(
        const void* parent, const void* child,
        bool original_succeeded = true);
    [[nodiscard]] bool observe_chest_remove_child_succeeded(
        const void* chest, const void* child, bool original_succeeded);
    [[nodiscard]] bool observe_remove_child_succeeded(
        const void* parent, const void* child,
        bool original_succeeded = true);

    [[nodiscard]] bool observe_obj_container_create_succeeded(
        const void* object);
    [[nodiscard]] bool observe_obj_container_create_succeeded(
        HostObjectId object_id, ObjectTypeId type_id = 0);

    [[nodiscard]] bool observe_obj_container_remove_succeeded(
        const void* object, bool original_succeeded = true);
    [[nodiscard]] bool observe_obj_container_remove_succeeded(
        std::int32_t engine_obj_id, bool original_succeeded = true);
    [[nodiscard]] bool observe_obj_container_remove_succeeded(
        HostObjectId object_id, bool original_succeeded = true);

    // Short aliases make the post-call seam convenient without suggesting
    // that these functions themselves invoke the engine original.
    [[nodiscard]] bool on_chest_add_child(
        const void* chest, const void* child)
    { return observe_add_child_succeeded(chest, child, true); }
    [[nodiscard]] bool on_chest_remove_child(
        const void* chest, const void* child, bool original_succeeded)
    { return observe_chest_remove_child_succeeded(
          chest, child, original_succeeded); }
    [[nodiscard]] bool on_obj_container_create(const void* object)
    { return observe_obj_container_create_succeeded(object); }
    [[nodiscard]] bool on_obj_container_remove(
        std::int32_t engine_obj_id, bool original_succeeded = true)
    { return observe_obj_container_remove_succeeded(
          engine_obj_id, original_succeeded); }

    [[nodiscard]] static constexpr EngineBindingAddresses addresses() noexcept
    { return EngineBindingAddresses::recommended(); }

private:
    WorldObserver* m_observer = nullptr;
    EngineBindingConfig m_config;
};

using WorldMutationObserver = WorldObserver;
using WorldObservationBridge = WorldObserver;
using ObjectCreateEvent = ObjectCreatedEvent;
using ObjectDespawnEvent = ObjectDespawnedEvent;
using ParentChildAddEvent = ParentChildAddedEvent;
using ParentChildRemoveEvent = ParentChildRemovedEvent;

} // namespace kraken::net

#endif // KRAKEN_NET_WORLD_OBSERVER_HPP
