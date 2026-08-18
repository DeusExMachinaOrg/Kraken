#include "net/host_world_registry.hpp"

#include <algorithm>
#include <limits>

namespace kraken::net {
namespace {

constexpr std::size_t kNotFound = (std::numeric_limits<std::size_t>::max)();

} // namespace

HostWorldRegistry::HostWorldRegistry(const HostObjectId first_dynamic_id)
{
    if (is_host_world_dynamic_id(first_dynamic_id) &&
        first_dynamic_id != kHostWorldDynamicNamespace) {
        m_next_dynamic_id = first_dynamic_id;
        m_last_dynamic_id = first_dynamic_id - 1;
    }
}

bool HostWorldRegistry::valid_handle(
    const HostWorldEngineHandle engine_handle) noexcept
{
    return engine_handle != kInvalidHostWorldEngineHandle;
}

bool HostWorldRegistry::valid_generation(
    const HostWorldGeneration generation) noexcept
{
    return generation != kInvalidHostWorldGeneration;
}

std::size_t HostWorldRegistry::index_for_id(const HostObjectId id) const noexcept
{
    if (id == kInvalidHostObjectId)
        return kNotFound;
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].id == id)
            return index;
    }
    return kNotFound;
}

std::size_t HostWorldRegistry::index_for_handle(
    const HostWorldEngineHandle engine_handle) const noexcept
{
    if (!valid_handle(engine_handle))
        return kNotFound;
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].engine_handle == engine_handle)
            return index;
    }
    return kNotFound;
}

const HostWorldRegistryEntry* HostWorldRegistry::find(
    const HostObjectId id) const noexcept
{
    const std::size_t index = index_for_id(id);
    return index == kNotFound ? nullptr : &m_entries[index];
}

const HostWorldRegistryEntry* HostWorldRegistry::find_by_handle(
    const HostWorldEngineHandle engine_handle) const noexcept
{
    const std::size_t index = index_for_handle(engine_handle);
    return index == kNotFound ? nullptr : &m_entries[index];
}

bool HostWorldRegistry::was_retired(const HostObjectId id) const noexcept
{
    if (id == kInvalidHostObjectId)
        return false;
    return std::any_of(
        m_retired.begin(), m_retired.end(),
        [id](const RetiredBinding& binding) {
            return binding.entry.id == id;
        });
}

bool HostWorldRegistry::generation_can_bind(
    const HostWorldEngineHandle engine_handle,
    const HostWorldGeneration generation) const noexcept
{
    if (!valid_generation(generation))
        return false;
    for (const RetiredBinding& binding : m_retired) {
        if (binding.entry.engine_handle == engine_handle &&
            generation <= binding.entry.generation)
            return false;
    }
    return true;
}

bool HostWorldRegistry::valid_parent(const HostObjectId child_id,
                                    const HostObjectId parent_id) const noexcept
{
    if (parent_id == kInvalidHostObjectId)
        return true;
    if (child_id == kInvalidHostObjectId || child_id == parent_id ||
        find(parent_id) == nullptr)
        return false;

    // A parent relation is a tree edge.  Reject a cycle before it can enter
    // the registry; the bounded walk also keeps malformed state fail-closed.
    HostObjectId current = parent_id;
    for (std::size_t steps = 0; steps <= m_entries.size(); ++steps) {
        if (current == child_id)
            return false;
        const HostWorldRegistryEntry* entry = find(current);
        if (entry == nullptr || entry->parent_id == kInvalidHostObjectId)
            return true;
        current = entry->parent_id;
    }
    return false;
}

HostWorldRegistryResult HostWorldRegistry::bind_entry(
    const HostWorldRegistryEntry entry)
{
    if (!valid_handle(entry.engine_handle))
        return HostWorldRegistryResult::InvalidHandle;
    if (entry.id == kInvalidHostObjectId)
        return HostWorldRegistryResult::InvalidId;
    if (!valid_generation(entry.generation))
        return HostWorldRegistryResult::InvalidGeneration;
    if (entry.id == entry.parent_id)
        return HostWorldRegistryResult::ParentCycle;

    const std::size_t handle_index = index_for_handle(entry.engine_handle);
    if (handle_index != kNotFound) {
        const HostWorldRegistryEntry& current = m_entries[handle_index];
        if (current.id == entry.id &&
            current.generation == entry.generation &&
            current.parent_id == entry.parent_id &&
            current.dynamic == entry.dynamic)
            return HostWorldRegistryResult::AlreadyBound;
        if (current.id == entry.id &&
            current.generation != entry.generation)
            return HostWorldRegistryResult::StaleGeneration;
        return HostWorldRegistryResult::Collision;
    }

    if (index_for_id(entry.id) != kNotFound)
        return HostWorldRegistryResult::Collision;
    if (was_retired(entry.id))
        return HostWorldRegistryResult::RetiredId;
    if (!generation_can_bind(entry.engine_handle, entry.generation))
        return HostWorldRegistryResult::StaleGeneration;
    if (!valid_parent(entry.id, entry.parent_id)) {
        if (entry.parent_id != kInvalidHostObjectId &&
            find(entry.parent_id) == nullptr)
            return HostWorldRegistryResult::InvalidParent;
        return HostWorldRegistryResult::ParentCycle;
    }

    if (entry.dynamic) {
        if (!is_host_world_dynamic_id(entry.id))
            return HostWorldRegistryResult::InvalidNamespace;
        if (entry.id <= m_last_dynamic_id)
            return HostWorldRegistryResult::NonMonotonicId;
    } else if (!is_host_world_static_id(entry.id)) {
        return HostWorldRegistryResult::InvalidNamespace;
    }

    m_entries.push_back(entry);
    return HostWorldRegistryResult::Inserted;
}

HostWorldRegistryResult HostWorldRegistry::install_static(
    const HostWorldEngineHandle engine_handle, const HostObjectId id,
    const HostWorldGeneration generation, const HostObjectId parent_id)
{
    if (!is_host_world_static_id(id))
        return id == kInvalidHostObjectId
                   ? HostWorldRegistryResult::InvalidId
                   : HostWorldRegistryResult::InvalidNamespace;

    return bind_entry(HostWorldRegistryEntry{
        id, engine_handle, generation, parent_id, false});
}

HostWorldRegistryResult HostWorldRegistry::bind_dynamic(
    const HostWorldEngineHandle engine_handle, const HostObjectId id,
    const HostWorldGeneration generation, const HostObjectId parent_id)
{
    if (!valid_handle(engine_handle))
        return HostWorldRegistryResult::InvalidHandle;
    if (!valid_generation(generation))
        return HostWorldRegistryResult::InvalidGeneration;
    if (!is_host_world_dynamic_id(id))
        return id == kInvalidHostObjectId
                   ? HostWorldRegistryResult::InvalidId
                   : HostWorldRegistryResult::InvalidNamespace;

    const std::size_t handle_index = index_for_handle(engine_handle);
    if (handle_index != kNotFound) {
        const HostWorldRegistryEntry& current = m_entries[handle_index];
        if (current.id == id && current.generation == generation &&
            current.parent_id == parent_id && current.dynamic)
            return HostWorldRegistryResult::AlreadyBound;
    }
    if (id <= m_last_dynamic_id)
        return HostWorldRegistryResult::NonMonotonicId;

    const HostWorldRegistryResult result = bind_entry(
        HostWorldRegistryEntry{id, engine_handle, generation, parent_id, true});
    if (result == HostWorldRegistryResult::Inserted) {
        m_last_dynamic_id = id;
        m_next_dynamic_id = id == (std::numeric_limits<HostObjectId>::max)()
                                ? kInvalidHostObjectId
                                : id + 1;
    }
    return result;
}

HostWorldDynamicAllocation HostWorldRegistry::allocate_dynamic(
    const HostWorldEngineHandle engine_handle,
    const HostWorldGeneration generation, const HostObjectId parent_id)
{
    if (!valid_handle(engine_handle))
        return {HostWorldRegistryResult::InvalidHandle,
                kInvalidHostObjectId};
    if (!valid_generation(generation))
        return {HostWorldRegistryResult::InvalidGeneration,
                kInvalidHostObjectId};
    if (find_by_handle(engine_handle) != nullptr)
        return {HostWorldRegistryResult::Collision, kInvalidHostObjectId};
    if (!is_host_world_dynamic_id(m_next_dynamic_id) ||
        m_next_dynamic_id == kHostWorldDynamicNamespace)
        return {HostWorldRegistryResult::DynamicIdExhausted,
                kInvalidHostObjectId};

    const HostObjectId candidate = m_next_dynamic_id;
    const HostWorldRegistryResult result =
        bind_dynamic(engine_handle, candidate, generation, parent_id);
    return {result, result == HostWorldRegistryResult::Inserted
                       ? candidate
                       : kInvalidHostObjectId};
}

HostWorldRegistryResult HostWorldRegistry::set_parent(
    const HostWorldEngineHandle child_handle,
    const HostWorldGeneration child_generation, const HostObjectId parent_id)
{
    if (!valid_handle(child_handle))
        return HostWorldRegistryResult::InvalidHandle;
    const HostWorldRegistryEntry* child = find_by_handle(child_handle);
    if (child == nullptr)
        return HostWorldRegistryResult::NotFound;
    if (!valid_generation(child_generation))
        return HostWorldRegistryResult::InvalidGeneration;
    if (child->generation != child_generation)
        return HostWorldRegistryResult::StaleGeneration;
    return set_parent(child->id, child_generation, parent_id);
}

HostWorldRegistryResult HostWorldRegistry::set_parent(
    const HostObjectId child_id, const HostWorldGeneration child_generation,
    const HostObjectId parent_id)
{
    if (child_id == kInvalidHostObjectId)
        return HostWorldRegistryResult::InvalidId;
    const std::size_t child_index = index_for_id(child_id);
    if (child_index == kNotFound)
        return HostWorldRegistryResult::NotFound;
    HostWorldRegistryEntry& child = m_entries[child_index];
    if (!valid_generation(child_generation))
        return HostWorldRegistryResult::InvalidGeneration;
    if (child.generation != child_generation)
        return HostWorldRegistryResult::StaleGeneration;
    if (child.parent_id == parent_id)
        return HostWorldRegistryResult::AlreadyBound;
    if (parent_id != kInvalidHostObjectId &&
        find(parent_id) == nullptr)
        return HostWorldRegistryResult::InvalidParent;
    if (!valid_parent(child.id, parent_id))
        return HostWorldRegistryResult::ParentCycle;
    child.parent_id = parent_id;
    return HostWorldRegistryResult::Inserted;
}

HostWorldRegistryResult HostWorldRegistry::remove_entry(
    const std::size_t index, const HostWorldGeneration expected_generation)
{
    if (index == kNotFound || index >= m_entries.size())
        return HostWorldRegistryResult::NotFound;
    if (!valid_generation(expected_generation))
        return HostWorldRegistryResult::InvalidGeneration;
    if (m_entries[index].generation != expected_generation)
        return HostWorldRegistryResult::StaleGeneration;
    if (m_entries[index].parent_id != kInvalidHostObjectId)
        return HostWorldRegistryResult::InvalidParent;
    for (const HostWorldRegistryEntry& entry : m_entries) {
        if (entry.parent_id == m_entries[index].id)
            return HostWorldRegistryResult::InvalidParent;
    }

    m_retired.push_back(RetiredBinding{m_entries[index]});
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
    return HostWorldRegistryResult::Removed;
}

HostWorldRegistryResult HostWorldRegistry::remove(
    const HostWorldEngineHandle engine_handle,
    const HostWorldGeneration expected_generation)
{
    if (!valid_handle(engine_handle))
        return HostWorldRegistryResult::InvalidHandle;
    return remove_entry(index_for_handle(engine_handle), expected_generation);
}

HostWorldRegistryResult HostWorldRegistry::remove(
    const HostWorldEngineHandle engine_handle, const HostObjectId expected_id,
    const HostWorldGeneration expected_generation)
{
    if (!valid_handle(engine_handle))
        return HostWorldRegistryResult::InvalidHandle;
    const std::size_t index = index_for_handle(engine_handle);
    if (index == kNotFound)
        return HostWorldRegistryResult::NotFound;
    if (m_entries[index].id != expected_id)
        return HostWorldRegistryResult::Collision;
    return remove_entry(index, expected_generation);
}

HostWorldRegistryResult HostWorldRegistry::remove_id(
    const HostObjectId id, const HostWorldGeneration expected_generation)
{
    if (id == kInvalidHostObjectId)
        return HostWorldRegistryResult::InvalidId;
    return remove_entry(index_for_id(id), expected_generation);
}

std::optional<HostObjectId> HostWorldRegistry::id_for(
    const HostWorldEngineHandle engine_handle,
    const HostWorldGeneration expected_generation) const noexcept
{
    const HostWorldRegistryEntry* entry = find_by_handle(engine_handle);
    if (entry == nullptr ||
        (expected_generation != kInvalidHostWorldGeneration &&
         entry->generation != expected_generation))
        return std::nullopt;
    return entry->id;
}

std::optional<HostWorldEngineHandle> HostWorldRegistry::handle_for(
    const HostObjectId id,
    const HostWorldGeneration expected_generation) const noexcept
{
    const HostWorldRegistryEntry* entry = find(id);
    if (entry == nullptr ||
        (expected_generation != kInvalidHostWorldGeneration &&
         entry->generation != expected_generation))
        return std::nullopt;
    return entry->engine_handle;
}

std::optional<HostWorldGeneration> HostWorldRegistry::generation_for(
    const HostObjectId id) const noexcept
{
    const HostWorldRegistryEntry* entry = find(id);
    return entry == nullptr ? std::nullopt
                            : std::optional<HostWorldGeneration>{
                                  entry->generation};
}

std::optional<HostObjectId> HostWorldRegistry::parent_for(
    const HostObjectId child_id,
    const HostWorldGeneration expected_generation) const noexcept
{
    const HostWorldRegistryEntry* entry = find(child_id);
    if (entry == nullptr ||
        (expected_generation != kInvalidHostWorldGeneration &&
         entry->generation != expected_generation))
        return std::nullopt;
    return entry->parent_id;
}

std::optional<HostObjectId> HostWorldRegistry::parent_for_handle(
    const HostWorldEngineHandle child_handle,
    const HostWorldGeneration expected_generation) const noexcept
{
    const HostWorldRegistryEntry* entry = find_by_handle(child_handle);
    if (entry == nullptr ||
        (expected_generation != kInvalidHostWorldGeneration &&
         entry->generation != expected_generation))
        return std::nullopt;
    return entry->parent_id;
}

void HostWorldRegistry::clear() noexcept
{
    for (const HostWorldRegistryEntry& entry : m_entries)
        m_retired.push_back(RetiredBinding{entry});
    m_entries.clear();
}

} // namespace kraken::net
