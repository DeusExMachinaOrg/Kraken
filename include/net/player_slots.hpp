#ifndef KRAKEN_NET_PLAYER_SLOTS_HPP
#define KRAKEN_NET_PLAYER_SLOTS_HPP

#include "net/entity_registry.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace kraken::net {

inline constexpr std::size_t kPlayerSlotCount = 4;
using PlayerSlotIndex = std::uint8_t;

inline constexpr PlayerSlotIndex kInvalidPlayerSlot =
    static_cast<PlayerSlotIndex>(kPlayerSlotCount);

// The map is the authority for the four physical slots. Entity IDs 1..4 are
// deliberately mapped to those slots so every peer reaches the same proxy
// without depending on connection/event arrival order.
[[nodiscard]] constexpr PlayerSlotIndex player_slot_for_entity(
    const NetId entity_id) noexcept
{
    return entity_id >= 1 && entity_id <= kPlayerSlotCount
        ? static_cast<PlayerSlotIndex>(entity_id - 1)
        : kInvalidPlayerSlot;
}

struct PlayerSlotLease {
    PlayerSlotIndex index = kInvalidPlayerSlot;
    EntityGeneration generation = kInvalidEntityGeneration;
    NetId owner_entity_id = kInvalidNetId;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return index < kPlayerSlotCount &&
            generation != kInvalidEntityGeneration &&
            owner_entity_id != kInvalidNetId;
    }
};

[[nodiscard]] constexpr bool operator==(const PlayerSlotLease& left,
                                        const PlayerSlotLease& right) noexcept
{
    return left.index == right.index && left.generation == right.generation &&
        left.owner_entity_id == right.owner_entity_id;
}

// Small, allocation-free state machine for map-owned player proxies.
// Reservation is idempotent for the same owner, binding is a separate
// transition, and release advances the generation before the slot is reused.
class PlayerSlotAllocator final {
public:
    [[nodiscard]] std::optional<PlayerSlotLease> reserve(
        const NetId owner_entity_id) noexcept
    {
        if (owner_entity_id == kInvalidNetId)
            return std::nullopt;
        for (PlayerSlotIndex index = 0; index < kPlayerSlotCount; ++index) {
            const Entry& entry = slots_[index];
            if (entry.owner_entity_id == owner_entity_id &&
                entry.state != State::Free)
                return lease(index, entry);
        }
        for (PlayerSlotIndex index = 0; index < kPlayerSlotCount; ++index) {
            if (slots_[index].state == State::Free)
                return reserve(index, owner_entity_id);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<PlayerSlotLease> reserve(
        const PlayerSlotIndex index, const NetId owner_entity_id) noexcept
    {
        if (index >= kPlayerSlotCount || owner_entity_id == kInvalidNetId)
            return std::nullopt;
        Entry& entry = slots_[index];
        if (entry.state != State::Free &&
            entry.owner_entity_id != owner_entity_id)
            return std::nullopt;
        if (entry.state == State::Free) {
            entry.owner_entity_id = owner_entity_id;
            entry.state = State::Reserved;
        }
        return lease(index, entry);
    }

    [[nodiscard]] bool bind(const PlayerSlotLease& requested,
                            const NetId owner_entity_id) noexcept
    {
        if (!requested.valid() || requested.owner_entity_id != owner_entity_id)
            return false;
        Entry& entry = slots_[requested.index];
        if (entry.state == State::Free ||
            entry.owner_entity_id != owner_entity_id ||
            entry.generation != requested.generation)
            return false;
        entry.state = State::Bound;
        return true;
    }

    [[nodiscard]] bool release(const PlayerSlotLease& requested,
                               const NetId owner_entity_id) noexcept
    {
        if (!requested.valid() || requested.owner_entity_id != owner_entity_id)
            return false;
        Entry& entry = slots_[requested.index];
        if (entry.state == State::Free ||
            entry.owner_entity_id != owner_entity_id ||
            entry.generation != requested.generation)
            return false;
        entry.owner_entity_id = kInvalidNetId;
        entry.state = State::Free;
        entry.generation = next_generation(entry.generation);
        return true;
    }

    [[nodiscard]] bool cancel(const PlayerSlotLease& requested,
                              const NetId owner_entity_id) noexcept
    {
        if (!requested.valid() || requested.owner_entity_id != owner_entity_id)
            return false;
        Entry& entry = slots_[requested.index];
        if (entry.state != State::Reserved ||
            entry.owner_entity_id != owner_entity_id ||
            entry.generation != requested.generation)
            return false;
        entry.owner_entity_id = kInvalidNetId;
        entry.state = State::Free;
        return true;
    }

    [[nodiscard]] std::optional<PlayerSlotLease> current(
        const PlayerSlotIndex index) const noexcept
    {
        if (index >= kPlayerSlotCount || slots_[index].state == State::Free)
            return std::nullopt;
        return lease(index, slots_[index]);
    }

    [[nodiscard]] bool is_bound(const PlayerSlotIndex index) const noexcept
    {
        return index < kPlayerSlotCount && slots_[index].state == State::Bound;
    }

    void clear() noexcept
    {
        for (Entry& entry : slots_) {
            entry.owner_entity_id = kInvalidNetId;
            entry.state = State::Free;
            entry.generation = kInitialEntityGeneration;
        }
    }

private:
    enum class State : std::uint8_t { Free, Reserved, Bound };

    struct Entry {
        NetId owner_entity_id = kInvalidNetId;
        EntityGeneration generation = kInitialEntityGeneration;
        State state = State::Free;
    };

    [[nodiscard]] static EntityGeneration next_generation(
        const EntityGeneration generation) noexcept
    {
        return generation == (std::numeric_limits<EntityGeneration>::max)()
            ? kInitialEntityGeneration
            : static_cast<EntityGeneration>(generation + 1);
    }

    [[nodiscard]] static PlayerSlotLease lease(
        const PlayerSlotIndex index, const Entry& entry) noexcept
    {
        return {index, entry.generation, entry.owner_entity_id};
    }

    std::array<Entry, kPlayerSlotCount> slots_{};
};

} // namespace kraken::net

#endif // KRAKEN_NET_PLAYER_SLOTS_HPP
