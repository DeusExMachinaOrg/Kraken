#ifndef KRAKEN_NET_HOST_WORLD_REGISTRY_HPP
#define KRAKEN_NET_HOST_WORLD_REGISTRY_HPP

#include "net/world_observer.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace kraken::net {

// Engine handles are process-local values.  They are deliberately kept as
// int32_t: a pointer, pointer-sized integer, or opaque engine address must
// never enter this registry's lookup contract.
using HostWorldEngineHandle = std::int32_t;
using HostWorldGeneration = std::uint32_t;

inline constexpr HostWorldEngineHandle kInvalidHostWorldEngineHandle =
    (std::numeric_limits<HostWorldEngineHandle>::min)();
inline constexpr HostWorldGeneration kInvalidHostWorldGeneration = 0;
inline constexpr HostWorldGeneration kInitialHostWorldGeneration = 1;

// The high bit is reserved for IDs assigned to dynamic objects.  Static IDs
// are installed by the host and must remain outside this namespace.  A
// namespace bit is metadata only; no ID is derived from a handle, pointer,
// or enumeration order.
inline constexpr HostObjectId kHostWorldDynamicNamespaceMask =
    HostObjectId{1} << 63;
inline constexpr HostObjectId kHostWorldDynamicNamespace =
    kHostWorldDynamicNamespaceMask;
inline constexpr HostObjectId kHostWorldDynamicFirstId =
    kHostWorldDynamicNamespace | HostObjectId{1};
inline constexpr HostObjectId kHostWorldStaticNamespaceMask =
    kHostWorldDynamicNamespaceMask;

[[nodiscard]] constexpr bool is_host_world_dynamic_id(
    const HostObjectId id) noexcept
{
    return id != kInvalidHostObjectId &&
           (id & kHostWorldDynamicNamespaceMask) != 0;
}

[[nodiscard]] constexpr bool is_host_world_static_id(
    const HostObjectId id) noexcept
{
    return id != kInvalidHostObjectId &&
           (id & kHostWorldStaticNamespaceMask) == 0;
}

enum class HostWorldRegistryResult : std::uint8_t {
    Inserted,
    AlreadyBound,
    Removed,
    NotFound,
    Collision,
    InvalidHandle,
    InvalidId,
    InvalidNamespace,
    InvalidGeneration,
    StaleGeneration,
    RetiredId,
    NonMonotonicId,
    InvalidParent,
    ParentCycle,
    DynamicIdExhausted,

    // Readable aliases used by adapters that distinguish the two collision
    // directions without adding separate result handling.
    HandleCollision = Collision,
    IdCollision = Collision,
    RemovedObject = Removed,
    GenerationMismatch = StaleGeneration,
};

[[nodiscard]] constexpr bool host_world_registry_succeeded(
    const HostWorldRegistryResult result) noexcept
{
    return result == HostWorldRegistryResult::Inserted ||
           result == HostWorldRegistryResult::AlreadyBound ||
           result == HostWorldRegistryResult::Removed;
}

struct HostWorldRegistryEntry {
    HostObjectId id = kInvalidHostObjectId;
    HostWorldEngineHandle engine_handle =
        kInvalidHostWorldEngineHandle;
    HostWorldGeneration generation = kInvalidHostWorldGeneration;
    HostObjectId parent_id = kInvalidHostObjectId;
    bool dynamic = false;
};

struct HostWorldDynamicAllocation {
    HostWorldRegistryResult result = HostWorldRegistryResult::NotFound;
    HostObjectId id = kInvalidHostObjectId;

    [[nodiscard]] bool succeeded() const noexcept
    { return host_world_registry_succeeded(result); }
    [[nodiscard]] explicit operator bool() const noexcept
    { return succeeded(); }
};

// A process-local bijection between int32 engine handles and host-issued
// 64-bit object IDs.  Dynamic allocation uses a reserved namespace and a
// monotonic counter.  Removed IDs and handle generations are retained so a
// later engine-handle reuse cannot resurrect an old object identity.
class HostWorldRegistry final {
public:
    explicit HostWorldRegistry(
        HostObjectId first_dynamic_id = kHostWorldDynamicFirstId);

    HostWorldRegistry(const HostWorldRegistry&) = delete;
    HostWorldRegistry& operator=(const HostWorldRegistry&) = delete;
    HostWorldRegistry(HostWorldRegistry&&) = default;
    HostWorldRegistry& operator=(HostWorldRegistry&&) = default;
    ~HostWorldRegistry() = default;

    [[nodiscard]] std::size_t size() const noexcept
    { return m_entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] HostObjectId next_dynamic_id() const noexcept
    { return m_next_dynamic_id; }
    [[nodiscard]] HostObjectId last_dynamic_id() const noexcept
    { return m_last_dynamic_id; }

    // Static IDs are host-installed identities (normally from the static
    // world index).  They must not use the reserved dynamic namespace.
    [[nodiscard]] HostWorldRegistryResult install_static(
        HostWorldEngineHandle engine_handle, HostObjectId id,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId);
    [[nodiscard]] HostWorldRegistryResult install_static_id(
        HostWorldEngineHandle engine_handle, HostObjectId id,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId)
    { return install_static(engine_handle, id, generation, parent_id); }
    [[nodiscard]] HostWorldRegistryResult bind_static(
        HostWorldEngineHandle engine_handle, HostObjectId id,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId)
    { return install_static(engine_handle, id, generation, parent_id); }
    [[nodiscard]] HostWorldRegistryResult register_static(
        HostWorldEngineHandle engine_handle, HostObjectId id,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId)
    { return install_static(engine_handle, id, generation, parent_id); }

    // The host may provide a dynamic ID explicitly, but it must be in the
    // reserved namespace and strictly newer than every prior dynamic ID.
    [[nodiscard]] HostWorldRegistryResult bind_dynamic(
        HostWorldEngineHandle engine_handle, HostObjectId id,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId);
    [[nodiscard]] HostWorldRegistryResult register_dynamic(
        HostWorldEngineHandle engine_handle, HostObjectId id,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId)
    { return bind_dynamic(engine_handle, id, generation, parent_id); }

    // This allocator is independent of the local handle.  Its counter is a
    // host-issued sequence, not an identity function of the handle.
    [[nodiscard]] HostWorldDynamicAllocation allocate_dynamic(
        HostWorldEngineHandle engine_handle,
        HostWorldGeneration generation = kInitialHostWorldGeneration,
        HostObjectId parent_id = kInvalidHostObjectId);

    [[nodiscard]] HostWorldRegistryResult set_parent(
        HostWorldEngineHandle child_handle,
        HostWorldGeneration child_generation, HostObjectId parent_id);
    [[nodiscard]] HostWorldRegistryResult set_parent(
        HostObjectId child_id, HostWorldGeneration child_generation,
        HostObjectId parent_id);

    [[nodiscard]] HostWorldRegistryResult remove(
        HostWorldEngineHandle engine_handle,
        HostWorldGeneration expected_generation);
    [[nodiscard]] HostWorldRegistryResult remove(
        HostWorldEngineHandle engine_handle, HostObjectId expected_id,
        HostWorldGeneration expected_generation);
    [[nodiscard]] HostWorldRegistryResult remove_id(
        HostObjectId id, HostWorldGeneration expected_generation);
    [[nodiscard]] HostWorldRegistryResult unbind(
        HostWorldEngineHandle engine_handle,
        HostWorldGeneration expected_generation)
    { return remove(engine_handle, expected_generation); }

    [[nodiscard]] std::optional<HostObjectId> id_for(
        HostWorldEngineHandle engine_handle,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept;
    [[nodiscard]] std::optional<HostObjectId> lookup_id(
        HostWorldEngineHandle engine_handle,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept
    { return id_for(engine_handle, expected_generation); }

    [[nodiscard]] std::optional<HostWorldEngineHandle> handle_for(
        HostObjectId id,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept;
    [[nodiscard]] std::optional<HostWorldEngineHandle> lookup_handle(
        HostObjectId id,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept
    { return handle_for(id, expected_generation); }

    [[nodiscard]] std::optional<HostWorldGeneration> generation_for(
        HostObjectId id) const noexcept;
    [[nodiscard]] std::optional<HostObjectId> parent_for(
        HostObjectId child_id,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept;
    [[nodiscard]] std::optional<HostObjectId> parent_for_handle(
        HostWorldEngineHandle child_handle,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept;
    [[nodiscard]] std::optional<HostObjectId> lookup_parent(
        HostObjectId child_id,
        HostWorldGeneration expected_generation =
            kInvalidHostWorldGeneration) const noexcept
    { return parent_for(child_id, expected_generation); }

    [[nodiscard]] const HostWorldRegistryEntry* find(
        HostObjectId id) const noexcept;
    [[nodiscard]] const HostWorldRegistryEntry* find_by_handle(
        HostWorldEngineHandle engine_handle) const noexcept;
    [[nodiscard]] bool contains(HostObjectId id) const noexcept
    { return find(id) != nullptr; }
    [[nodiscard]] bool contains_handle(
        HostWorldEngineHandle engine_handle) const noexcept
    { return find_by_handle(engine_handle) != nullptr; }
    [[nodiscard]] bool was_retired(HostObjectId id) const noexcept;

    // Clear live bindings while retaining retirement history and the dynamic
    // counter.  This keeps non-reuse true across a map/object reset.
    void clear() noexcept;

private:
    struct RetiredBinding {
        HostWorldRegistryEntry entry{};
    };

    [[nodiscard]] static bool valid_handle(
        HostWorldEngineHandle engine_handle) noexcept;
    [[nodiscard]] static bool valid_generation(
        HostWorldGeneration generation) noexcept;
    [[nodiscard]] bool valid_parent(HostObjectId child_id,
                                    HostObjectId parent_id) const noexcept;
    [[nodiscard]] bool generation_can_bind(
        HostWorldEngineHandle engine_handle,
        HostWorldGeneration generation) const noexcept;
    [[nodiscard]] HostWorldRegistryResult bind_entry(
        HostWorldRegistryEntry entry);
    [[nodiscard]] HostWorldRegistryResult remove_entry(
        std::size_t index, HostWorldGeneration expected_generation);
    [[nodiscard]] std::size_t index_for_id(HostObjectId id) const noexcept;
    [[nodiscard]] std::size_t index_for_handle(
        HostWorldEngineHandle engine_handle) const noexcept;

    std::vector<HostWorldRegistryEntry> m_entries;
    std::vector<RetiredBinding> m_retired;
    HostObjectId m_next_dynamic_id = kHostWorldDynamicFirstId;
    HostObjectId m_last_dynamic_id = kHostWorldDynamicNamespace;
};

using HostWorldIdRegistry = HostWorldRegistry;
using WorldObjectRegistry = HostWorldRegistry;

} // namespace kraken::net

#endif // KRAKEN_NET_HOST_WORLD_REGISTRY_HPP
