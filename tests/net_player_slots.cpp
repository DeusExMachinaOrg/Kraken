#include "net/player_slots.hpp"

#include <cassert>

using namespace kraken::net;

int main()
{
    static_assert(kPlayerSlotCount == 4);
    static_assert(player_slot_for_entity(1) == 0);
    static_assert(player_slot_for_entity(4) == 3);
    static_assert(player_slot_for_entity(5) == kInvalidPlayerSlot);

    PlayerSlotAllocator allocator;
    const auto first = allocator.reserve(101);
    assert(first.has_value());
    assert(first->index == 0);
    assert(first->generation == kInitialEntityGeneration);
    assert(allocator.reserve(101) == first);
    assert(allocator.bind(*first, 101));
    assert(allocator.is_bound(0));
    assert(!allocator.reserve(0, 202).has_value());

    const auto second = allocator.reserve(202);
    assert(second.has_value());
    assert(second->index == 1);
    assert(allocator.bind(*second, 202));
    assert(!allocator.release(*first, 202));
    assert(allocator.release(*first, 101));

    const auto reused = allocator.reserve(303);
    assert(reused.has_value());
    assert(reused->index == first->index);
    assert(reused->generation ==
           static_cast<EntityGeneration>(first->generation + 1));
    assert(!allocator.bind(*first, 303));
    assert(allocator.bind(*reused, 303));

    assert(allocator.reserve(0, 404).has_value());
    assert(allocator.reserve(3, 505).has_value());
    assert(!allocator.reserve(606).has_value());
    assert(!allocator.release(*reused, 404));
    assert(allocator.release(*reused, 303));

    const auto deterministic = allocator.reserve(606);
    assert(deterministic.has_value());
    assert(deterministic->index == 0);
    assert(deterministic->generation ==
           static_cast<EntityGeneration>(reused->generation + 1));
    return 0;
}
