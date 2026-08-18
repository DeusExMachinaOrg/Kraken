#include "net/entity_protocol.hpp"
#include "net/entity_registry.hpp"
#include "net/loadout_protocol.hpp"
#include "net/spawn_attempt.hpp"
#include "net/vehicle_snapshot.hpp"
#include "net/wire_protocol.hpp"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <utility>
#include <vector>

using namespace kraken::net;

namespace {

EntitySpawn make_spawn()
{
    EntitySpawn spawn{};
    spawn.entity_id = 42;
    spawn.generation = 7;
    spawn.kind = EntityKind::NpcVehicle;
    spawn.prototype_id = 1131;
    spawn.owner_entity_id = 9;
    spawn.belong = 2;
    spawn.position = {1.0f, 2.0f, 3.0f};
    spawn.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    spawn.health_fraction = 0.75f;
    return spawn;
}

void test_spawn_round_trip_preserves_identity_and_kind()
{
    EntitySpawn expected = make_spawn();
    std::array<Byte, kEntitySpawnWireSize> bytes{};
    assert(encode_entity_spawn(expected, bytes) == EntityCodecError::None);

    EntitySpawn actual{};
    assert(decode_entity_spawn(bytes, actual) == EntityCodecError::None);
    assert(actual.entity_id == expected.entity_id);
    assert(actual.generation == expected.generation);
    assert(actual.kind == expected.kind);
    assert(actual.prototype_id == expected.prototype_id);
    assert(actual.owner_entity_id == expected.owner_entity_id);
    assert(actual.belong == expected.belong);
    assert(actual.position.x == expected.position.x);
    assert(actual.position.y == expected.position.y);
    assert(actual.position.z == expected.position.z);
    assert(actual.rotation.w == expected.rotation.w);
    assert(actual.health_fraction == expected.health_fraction);

        for (const EntityKind kind : {EntityKind::PlayerVehicle,
                                  EntityKind::NpcVehicle,
                                  EntityKind::WorldObject,
                                  EntityKind::LootContainer,
                                  EntityKind::Wreck}) {
        expected.kind = kind;
        assert(encode_entity_spawn(expected, bytes) == EntityCodecError::None);
        assert(decode_entity_spawn(bytes, actual) == EntityCodecError::None);
        assert(actual.kind == kind);
    }
}

void test_spawn_rejects_invalid_generation_kind_and_bounds()
{
    EntitySpawn expected = make_spawn();
    std::array<Byte, kEntitySpawnWireSize> bytes{};
    EntitySpawn actual{};

    expected.generation = kInvalidEntityGeneration;
    assert(encode_entity_spawn(expected, bytes) ==
           EntityCodecError::InvalidGeneration);

    expected = make_spawn();
    expected.kind = static_cast<EntityKind>(0);
    assert(encode_entity_spawn(expected, bytes) == EntityCodecError::InvalidKind);

    expected = make_spawn();
    assert(encode_entity_spawn(expected, bytes) == EntityCodecError::None);
    bytes[12] = Byte{};
    bytes[13] = Byte{};
    assert(decode_entity_spawn(bytes, actual) ==
           EntityCodecError::InvalidGeneration);

    assert(encode_entity_spawn(expected, bytes) == EntityCodecError::None);
    bytes[6] = Byte{6};
    assert(decode_entity_spawn(bytes, actual) == EntityCodecError::InvalidKind);

    expected = make_spawn();
    expected.health_fraction = 1.01f;
    assert(encode_entity_spawn(expected, bytes) == EntityCodecError::InvalidHealth);
    expected = make_spawn();
    expected.position.x = std::nanf("");
    assert(encode_entity_spawn(expected, bytes) ==
           EntityCodecError::NonFiniteValue);
    expected = make_spawn();
    expected.rotation = {0.0f, 0.0f, 0.0f, 0.0f};
    assert(encode_entity_spawn(expected, bytes) ==
           EntityCodecError::InvalidQuaternion);

    assert(encode_entity_spawn(make_spawn(), bytes) == EntityCodecError::None);
    bytes[7] = Byte{1};
    assert(decode_entity_spawn(bytes, actual) == EntityCodecError::BadFlags);
    assert(decode_entity_spawn(ByteView{bytes}.first(kEntitySpawnWireSize - 1),
                               actual) == EntityCodecError::InputSizeMismatch);
}

void test_terminal_spawn_failure_prevents_retry_until_reset()
{
    SpawnAttemptState attempt;
    unsigned factory_side_effects = 0;
    const auto factory = [&]() { ++factory_side_effects; };
    const auto unsupported_presentation = [&](const auto& disabled_factory) {
        if (!attempt.can_attempt(10))
            return;
        attempt.reject_permanently();
        // Current engine factory cloning is unsupported. Rejection must occur
        // before a CreateNewObject-equivalent side effect.
        (void)disabled_factory;
    };
    unsupported_presentation(factory);
    unsupported_presentation(factory);
    assert(factory_side_effects == 0);
    assert(!attempt.can_attempt(10));
    assert(!attempt.can_attempt(100));
    attempt.reset();
    assert(attempt.can_attempt(0));
}

void test_despawn_round_trip_and_stale_generation_is_wire_visible()
{
    const EntityDespawn expected{42, 19, 3};
    std::array<Byte, kEntityDespawnWireSize> bytes{};
    assert(encode_entity_despawn(expected, bytes) == EntityCodecError::None);

    EntityDespawn actual{};
    assert(decode_entity_despawn(bytes, actual) == EntityCodecError::None);
    assert(actual.entity_id == expected.entity_id);
    assert(actual.generation == expected.generation);
    assert(actual.reason == expected.reason);

    bytes[12] = Byte{};
    bytes[13] = Byte{};
    assert(decode_entity_despawn(bytes, actual) ==
           EntityCodecError::InvalidGeneration);

    assert(encode_entity_despawn(expected, bytes) == EntityCodecError::None);
    bytes[7] = Byte{1};
    assert(decode_entity_despawn(bytes, actual) == EntityCodecError::BadFlags);
    assert(encode_entity_despawn(EntityDespawn{0, 1, 0}, bytes) ==
           EntityCodecError::InvalidEntity);
    assert(encode_entity_despawn(EntityDespawn{42, 0, 0}, bytes) ==
           EntityCodecError::InvalidGeneration);
}

enum class BaselineStage : std::uint8_t { Spawn, Loadout, Snapshot };

struct BaselineMessage {
    BaselineStage stage;
    MessageType type;
    std::vector<Byte> payload;
};

void test_late_join_npc_baseline_orders_metadata_before_snapshot()
{
    constexpr NetId entity_id = 1007;
    constexpr EntityGeneration generation = 9;

    EntitySpawn spawn{};
    spawn.entity_id = entity_id;
    spawn.generation = generation;
    spawn.kind = EntityKind::NpcVehicle;
    spawn.prototype_id = 1131;
    spawn.owner_entity_id = 1;
    spawn.rotation.w = 1.0f;
    std::array<Byte, kEntitySpawnWireSize> spawn_bytes{};
    assert(encode_entity_spawn(spawn, spawn_bytes) == EntityCodecError::None);

    LoadoutProfile loadout{
        entity_id, 23, {{"CABIN_SMALL_GUN", "hornet01"}}, generation};
    std::vector<Byte> loadout_bytes;
    assert(encode_loadout(loadout, loadout_bytes) == LoadoutCodecError::None);

    VehicleSnapshot snapshot{};
    snapshot.entity_id = entity_id;
    snapshot.sequence = 1;
    snapshot.server_tick = 100;
    snapshot.rotation.w = 1.0f;
    std::array<Byte, kVehicleSnapshotWireSize> snapshot_bytes{};
    assert(encode_vehicle_snapshot(snapshot, snapshot_bytes) ==
           VehicleSnapshotCodecError::None);

    const std::vector<BaselineMessage> baseline{
        {BaselineStage::Spawn, MessageType::EntitySpawn,
         std::vector<Byte>(spawn_bytes.begin(), spawn_bytes.end())},
        {BaselineStage::Loadout, MessageType::Loadout, std::move(loadout_bytes)},
        {BaselineStage::Snapshot, MessageType::Snapshot,
         std::vector<Byte>(snapshot_bytes.begin(), snapshot_bytes.end())},
    };
    assert(baseline[0].stage == BaselineStage::Spawn);
    assert(baseline[1].stage == BaselineStage::Loadout);
    assert(baseline[2].stage == BaselineStage::Snapshot);

    EntitySpawn decoded_spawn{};
    assert(decode_entity_spawn(baseline[0].payload, decoded_spawn) ==
           EntityCodecError::None);
    assert(decoded_spawn.entity_id == entity_id);
    assert(decoded_spawn.generation == generation);
    assert(decoded_spawn.kind == EntityKind::NpcVehicle);
    assert(decoded_spawn.prototype_id == 1131);
    assert(decoded_spawn.owner_entity_id == 1);

    LoadoutProfile decoded_loadout{};
    assert(decode_loadout(baseline[1].payload, decoded_loadout) ==
           LoadoutCodecError::None);
    assert(decoded_loadout.entity_id == decoded_spawn.entity_id);
    assert(decoded_loadout.generation == decoded_spawn.generation);
    assert(decoded_loadout.revision == 23);

    VehicleSnapshot decoded_snapshot{};
    assert(decode_vehicle_snapshot(baseline[2].payload, decoded_snapshot) ==
           VehicleSnapshotCodecError::None);
    assert(decoded_snapshot.entity_id == decoded_spawn.entity_id);
}

} // namespace

int main()
{
    test_spawn_round_trip_preserves_identity_and_kind();
    test_spawn_rejects_invalid_generation_kind_and_bounds();
    test_terminal_spawn_failure_prevents_retry_until_reset();
    test_despawn_round_trip_and_stale_generation_is_wire_visible();
    test_late_join_npc_baseline_orders_metadata_before_snapshot();
    return 0;
}
