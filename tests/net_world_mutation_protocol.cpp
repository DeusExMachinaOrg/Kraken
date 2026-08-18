#include "net/world_mutation_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <type_traits>
#include <variant>
#include <vector>

using namespace kraken::net;

namespace {

template <typename T>
void assert_round_trip(const T& expected)
{
    const WorldMutationEvent event = expected;
    std::vector<Byte> bytes;
    assert(encode_world_mutation(event, bytes) ==
           WorldMutationCodecError::None);
    WorldMutationEvent decoded{};
    assert(decode_world_mutation(bytes, decoded) ==
           WorldMutationCodecError::None);
    assert(decoded.index() == event.index());

    const T& actual = std::get<T>(decoded);
    if constexpr (std::is_same_v<T, ObjectCreatedEvent>) {
        assert(actual.object_id == expected.object_id);
        assert(actual.type_id == expected.type_id);
    } else if constexpr (std::is_same_v<T, ObjectDespawnedEvent>) {
        assert(actual.object_id == expected.object_id);
    } else if constexpr (std::is_same_v<T, ParentChildAddedEvent> ||
                         std::is_same_v<T, ParentChildRemovedEvent>) {
        assert(actual.parent_id == expected.parent_id);
        assert(actual.child_id == expected.child_id);
    } else if constexpr (std::is_same_v<T, RuntimeChangedEvent>) {
        assert(actual.object_id == expected.object_id);
        assert(actual.value == expected.value);
    } else if constexpr (std::is_same_v<T, PropertyChangedEvent>) {
        assert(actual.object_id == expected.object_id);
        assert(actual.property_id == expected.property_id);
        assert(actual.value == expected.value);
        assert(actual.removed == expected.removed);
    } else if constexpr (std::is_same_v<T, DamageEvent>) {
        assert(actual.target_id == expected.target_id);
        assert(actual.source_id == expected.source_id);
        assert(actual.damage_type == expected.damage_type);
        assert(actual.amount == expected.amount);
    } else if constexpr (std::is_same_v<T, DestroyedEvent>) {
        assert(actual.object_id == expected.object_id);
        assert(actual.reason == expected.reason);
    } else if constexpr (std::is_same_v<T, FxEvent>) {
        assert(actual.object_id == expected.object_id);
        assert(actual.effect_id == expected.effect_id);
        assert(actual.payload == expected.payload);
    }
}

void test_every_mutation_variant_round_trips()
{
    // Keep this table exhaustive with respect to WorldMutationEvent. The
    // protocol test must cover every wire kind, including removed properties,
    // damage, destruction and FX.
    assert_round_trip(ObjectCreatedEvent{7, 11});
    assert_round_trip(ObjectDespawnedEvent{7});
    assert_round_trip(ParentChildAddedEvent{3, 4});
    assert_round_trip(ParentChildRemovedEvent{3, 4});
    assert_round_trip(RuntimeChangedEvent{7, {Byte{1}, Byte{2}}});
    assert_round_trip(PropertyChangedEvent{7, 12, {Byte{3}}, false});
    assert_round_trip(PropertyChangedEvent{7, 12, {}, true});
    assert_round_trip(DamageEvent{7, 8, 9, 1.5f});
    assert_round_trip(DestroyedEvent{7, 10});
    assert_round_trip(FxEvent{7, 11, {Byte{4}, Byte{5}}});
}

} // namespace

int main()
{
    test_every_mutation_variant_round_trips();
    return 0;
}
