#include "net/entity_registry.hpp"

#include <algorithm>

namespace kraken::net {

EntityRegistry::EntityRegistry(const std::size_t capacity)
    : m_capacity(capacity)
{
    // Reserve once so bind() is non-allocating and can remain noexcept.
    m_entries.reserve(m_capacity);
}

std::size_t EntityRegistry::size() const noexcept
{
    return m_entries.size();
}

std::size_t EntityRegistry::capacity() const noexcept
{
    return m_capacity;
}

bool EntityRegistry::empty() const noexcept
{
    return m_entries.empty();
}

bool EntityRegistry::full() const noexcept
{
    return m_entries.size() == m_capacity;
}

EntityRegistryBindResult EntityRegistry::bind(const NetId net_id,
                                              const ObjId obj_id) noexcept
{
    return bind(net_id, obj_id, kInitialEntityGeneration);
}

EntityRegistryBindResult EntityRegistry::bind(
    const NetId net_id, const ObjId obj_id,
    const EntityGeneration generation) noexcept
{
    if (!valid_net_id(net_id) || !valid_obj_id(obj_id))
        return EntityRegistryBindResult::InvalidId;
    if (!valid_generation(generation))
        return EntityRegistryBindResult::InvalidGeneration;

    const std::size_t net_index = find_net_index(net_id);
    const std::size_t obj_index = find_obj_index(obj_id);

    if (net_index != m_entries.size() && obj_index != m_entries.size()) {
        return m_entries[net_index].obj_id == obj_id &&
                       m_entries[obj_index].net_id == net_id &&
                       m_entries[net_index].generation == generation
                   ? EntityRegistryBindResult::AlreadyBound
                   : EntityRegistryBindResult::Collision;
    }

    if (net_index != m_entries.size() || obj_index != m_entries.size())
        return EntityRegistryBindResult::Collision;

    if (full())
        return EntityRegistryBindResult::Full;

    m_entries.push_back(Entry{net_id, obj_id, generation});
    return EntityRegistryBindResult::Inserted;
}

bool EntityRegistry::lookup_obj_id(const NetId net_id,
                                   ObjId& obj_id) const noexcept
{
    EntityGeneration generation = kInvalidEntityGeneration;
    return lookup_obj_id(net_id, obj_id, generation);
}

bool EntityRegistry::lookup_obj_id(
    const NetId net_id, ObjId& obj_id,
    EntityGeneration& generation) const noexcept
{
    if (!valid_net_id(net_id))
        return false;

    const std::size_t index = find_net_index(net_id);
    if (index == m_entries.size())
        return false;

    obj_id = m_entries[index].obj_id;
    generation = m_entries[index].generation;
    return true;
}

bool EntityRegistry::lookup_net_id(const ObjId obj_id,
                                   NetId& net_id) const noexcept
{
    EntityGeneration generation = kInvalidEntityGeneration;
    return lookup_net_id(obj_id, net_id, generation);
}

bool EntityRegistry::lookup_net_id(
    const ObjId obj_id, NetId& net_id,
    EntityGeneration& generation) const noexcept
{
    if (!valid_obj_id(obj_id))
        return false;

    const std::size_t index = find_obj_index(obj_id);
    if (index == m_entries.size())
        return false;

    net_id = m_entries[index].net_id;
    generation = m_entries[index].generation;
    return true;
}

bool EntityRegistry::lookup_generation(
    const NetId net_id, EntityGeneration& generation) const noexcept
{
    ObjId obj_id = kInvalidObjId;
    return lookup_obj_id(net_id, obj_id, generation);
}

bool EntityRegistry::valid_generation(
    const EntityGeneration generation) noexcept
{
    return generation != kInvalidEntityGeneration;
}

bool EntityRegistry::valid_obj_id(const ObjId obj_id) noexcept
{
    return obj_id != kInvalidObjId;
}

bool EntityRegistry::valid_net_id(const NetId net_id) noexcept
{
    return net_id != kInvalidNetId;
}

EntityRegistryUnbindResult EntityRegistry::unbind_net_id(
    const NetId net_id) noexcept
{
    if (!valid_net_id(net_id))
        return EntityRegistryUnbindResult::InvalidId;

    const std::size_t index = find_net_index(net_id);
    if (index == m_entries.size())
        return EntityRegistryUnbindResult::NotFound;

    // Erase-shift, rather than swap-with-last, preserves deterministic order.
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
    return EntityRegistryUnbindResult::Removed;
}

EntityRegistryUnbindResult EntityRegistry::unbind_obj_id(
    const ObjId obj_id) noexcept
{
    if (!valid_obj_id(obj_id))
        return EntityRegistryUnbindResult::InvalidId;

    const std::size_t index = find_obj_index(obj_id);
    if (index == m_entries.size())
        return EntityRegistryUnbindResult::NotFound;

    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
    return EntityRegistryUnbindResult::Removed;
}

void EntityRegistry::clear() noexcept
{
    m_entries.clear();
}

std::size_t EntityRegistry::find_net_index(const NetId net_id) const noexcept
{
    const auto it = std::find_if(
        m_entries.begin(), m_entries.end(),
        [net_id](const Entry& entry) { return entry.net_id == net_id; });
    return static_cast<std::size_t>(it - m_entries.begin());
}

std::size_t EntityRegistry::find_obj_index(const ObjId obj_id) const noexcept
{
    const auto it = std::find_if(
        m_entries.begin(), m_entries.end(),
        [obj_id](const Entry& entry) { return entry.obj_id == obj_id; });
    return static_cast<std::size_t>(it - m_entries.begin());
}

} // namespace kraken::net
