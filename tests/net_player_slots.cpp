#include "net/player_slots.hpp"

#include <array>
#include <cassert>

using namespace kraken::net;

int main()
{
    static_assert(kPlayerSlotCount == 64);
    static_assert(player_slot_for_entity(1) == 0);
    static_assert(player_slot_for_entity(64) == 63);
    static_assert(player_slot_for_entity(65) == kInvalidPlayerSlot);

    PlayerSlotAllocator allocator;
    std::array<PlayerSlotLease, kPlayerSlotCount> leases{};
    for (PlayerSlotIndex index = 0; index < kPlayerSlotCount; ++index) {
        const NetId owner = static_cast<NetId>(index) + 101;
        const auto lease = allocator.reserve(owner);
        assert(lease.has_value());
        assert(lease->index == index);
        assert(lease->generation == kInitialEntityGeneration);
        assert(allocator.reserve(owner) == lease);
        assert(allocator.bind(*lease, owner));
        assert(allocator.bind(*lease, owner));
        assert(allocator.is_bound(index));
        leases[index] = *lease;
    }
    assert(!allocator.reserve(165).has_value());
    assert(!allocator.reserve(0, 0).has_value());
    assert(!allocator.reserve(0, 165).has_value());

    assert(!allocator.release(leases[0], 202));
    assert(allocator.release(leases[0], 101));
    assert(!allocator.release(leases[0], 101));

    const auto reused = allocator.reserve(202);
    assert(reused.has_value());
    assert(reused->index == leases[0].index);
    assert(reused->generation ==
           static_cast<EntityGeneration>(leases[0].generation + 1));
    assert(!allocator.bind(leases[0], 101));
    assert(allocator.bind(*reused, 202));
    assert(allocator.release(*reused, 202));

    const auto deterministic = allocator.reserve(303);
    assert(deterministic.has_value());
    assert(deterministic->index == leases[0].index);
    assert(deterministic->generation ==
           static_cast<EntityGeneration>(reused->generation + 1));
    assert(allocator.release(*deterministic, 303));

    for (PlayerSlotIndex index = 1; index < kPlayerSlotCount; ++index) {
        const NetId replacement_owner = static_cast<NetId>(index) + 501;
        assert(allocator.release(leases[index],
                                 static_cast<NetId>(index) + 101));
        const auto replacement = allocator.reserve(index, replacement_owner);
        assert(replacement.has_value());
        assert(replacement->index == index);
        assert(replacement->generation ==
               static_cast<EntityGeneration>(leases[index].generation + 1));
        assert(allocator.bind(*replacement, replacement_owner));
        assert(allocator.release(*replacement, replacement_owner));
    }

    return 0;
}
