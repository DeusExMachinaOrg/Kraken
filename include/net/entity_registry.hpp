#ifndef KRAKEN_NET_ENTITY_REGISTRY_HPP
#define KRAKEN_NET_ENTITY_REGISTRY_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace kraken::net {

// NetId is the protocol-visible entity identity. Zero is reserved as invalid.
using NetId = std::uint32_t;

// ObjId is deliberately opaque: the registry never interprets or generates it.
// INT32_MIN is reserved as the invalid/sentinel value; every other int32 value,
// including negative values such as -1, is accepted.
using ObjId = std::int32_t;

inline constexpr NetId kInvalidNetId = 0;
inline constexpr ObjId kInvalidObjId = (std::numeric_limits<ObjId>::min)();
inline constexpr std::size_t kDefaultEntityRegistryCapacity = 16u * 1024u;

enum class EntityRegistryBindResult : std::uint8_t {
    Inserted,
    AlreadyBound,
    Collision,
    Full,
    InvalidId,
};

enum class EntityRegistryUnbindResult : std::uint8_t {
    Removed,
    NotFound,
    InvalidId,
};

// A bounded, deterministic bijection between a network entity and the local
// engine object handle.
//
// Collision/overwrite policy is intentionally strict:
//   * bind(net, obj) is idempotent only when that exact pair already exists;
//   * if either key is already paired with a different key, bind returns
//     Collision and changes nothing;
//   * there is no implicit overwrite operation. Call unbind_net_id() or
//     unbind_obj_id() explicitly before reusing either identity.
//
// Entries are kept in insertion order and removal preserves the order of the
// remaining entries. This makes all observable behavior independent of hash
// table implementation details. Lookups are linear and bounded by capacity;
// the intended capacity is 16K, where this is preferable to nondeterminism.
class EntityRegistry final {
public:
    explicit EntityRegistry(
        std::size_t capacity = kDefaultEntityRegistryCapacity);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;

    [[nodiscard]] EntityRegistryBindResult bind(NetId net_id,
                                                ObjId obj_id) noexcept;

    [[nodiscard]] bool lookup_obj_id(NetId net_id,
                                     ObjId& obj_id) const noexcept;
    [[nodiscard]] bool lookup_net_id(ObjId obj_id,
                                     NetId& net_id) const noexcept;

    [[nodiscard]] EntityRegistryUnbindResult unbind_net_id(
        NetId net_id) noexcept;
    [[nodiscard]] EntityRegistryUnbindResult unbind_obj_id(
        ObjId obj_id) noexcept;

    void clear() noexcept;

private:
    struct Entry {
        NetId net_id = kInvalidNetId;
        ObjId obj_id = kInvalidObjId;
    };

    [[nodiscard]] static bool valid_net_id(NetId net_id) noexcept;
    [[nodiscard]] static bool valid_obj_id(ObjId obj_id) noexcept;
    [[nodiscard]] std::size_t find_net_index(NetId net_id) const noexcept;
    [[nodiscard]] std::size_t find_obj_index(ObjId obj_id) const noexcept;

    std::vector<Entry> m_entries;
    std::size_t m_capacity = 0;
};

static_assert(sizeof(NetId) == sizeof(std::uint32_t));
static_assert(sizeof(ObjId) == sizeof(std::int32_t));

} // namespace kraken::net

#endif // KRAKEN_NET_ENTITY_REGISTRY_HPP
