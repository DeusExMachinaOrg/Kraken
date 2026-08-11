#include "net/runtime.hpp"
#include "net/world_loot.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>

using namespace kraken::net;

using PublishHostWorldLootObjectSignature = WorldLootId (*)(
    std::int32_t, std::int32_t, std::uint32_t, NetId);
static_assert(std::is_same_v<
              decltype(&runtime::PublishHostWorldLootObject),
              PublishHostWorldLootObjectSignature>);

namespace {

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

WorldLootRecord make_record(const WorldLootId loot_id,
                            const WorldLootGeneration generation = 1,
                            const WorldLootRevision revision = 1,
                            const std::uint32_t amount = 8,
                            const WorldLootSessionEpoch epoch = 7)
{
    WorldLootRecord record{};
    record.session_epoch = epoch;
    record.loot_id = loot_id;
    record.generation = generation;
    record.revision = revision;
    record.container_id = 1000u + loot_id;
    record.container_prototype_id = 200;
    record.owner_entity_id = 42;
    record.transform.position_x = 1.25f;
    record.transform.position_y = -2.5f;
    record.transform.position_z = 3.75f;
    record.transform.rotation_y = 0.6f;
    record.transform.rotation_w = 0.8f;
    record.item_prototype_id = 300;
    record.item_instance_id = 301;
    record.amount = amount;
    return record;
}

bool same_record(const WorldLootRecord& left, const WorldLootRecord& right)
{
    return left.session_epoch == right.session_epoch &&
           left.loot_id == right.loot_id &&
           left.generation == right.generation &&
           left.revision == right.revision &&
           left.container_id == right.container_id &&
           left.container_prototype_id == right.container_prototype_id &&
           left.owner_entity_id == right.owner_entity_id &&
           left.transform.position_x == right.transform.position_x &&
           left.transform.position_y == right.transform.position_y &&
           left.transform.position_z == right.transform.position_z &&
           left.transform.rotation_x == right.transform.rotation_x &&
           left.transform.rotation_y == right.transform.rotation_y &&
           left.transform.rotation_z == right.transform.rotation_z &&
           left.transform.rotation_w == right.transform.rotation_w &&
           left.item_prototype_id == right.item_prototype_id &&
           left.item_instance_id == right.item_instance_id &&
           left.amount == right.amount;
}

void put_u32(std::vector<Byte>& bytes, const std::size_t offset,
             const std::uint32_t value)
{
    for (unsigned index = 0; index != 4; ++index)
        bytes[offset + index] =
            static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

template <typename Decode>
void check_fixed_wire_rejections(const std::vector<Byte>& valid,
                                  const std::size_t padding_offset,
                                  Decode decode)
{
    CHECK(decode(ByteView{valid}) == WorldLootCodecError::None);
    std::vector<Byte> malformed = valid;
    malformed[0] = Byte{};
    CHECK(decode(ByteView{malformed}) == WorldLootCodecError::BadMagic);
    malformed = valid;
    malformed[4] = static_cast<Byte>(kWorldLootWireVersion + 1);
    CHECK(decode(ByteView{malformed}) == WorldLootCodecError::BadVersion);
    malformed = valid;
    malformed[6] = Byte{1};
    CHECK(decode(ByteView{malformed}) == WorldLootCodecError::BadFlags);
    malformed = valid;
    malformed[padding_offset] = Byte{1};
    CHECK(decode(ByteView{malformed}) == WorldLootCodecError::BadFlags);
    malformed = valid;
    malformed.pop_back();
    CHECK(decode(ByteView{malformed}) == WorldLootCodecError::InputSizeMismatch);
    malformed = valid;
    malformed.push_back(Byte{});
    CHECK(decode(ByteView{malformed}) == WorldLootCodecError::InputSizeMismatch);
}

void test_codec_round_trips()
{
    const WorldLootRecord record = make_record(17, 9, 123, 55, 99);
    std::array<Byte, kWorldLootSpawnWireSize> spawn_bytes{};
    CHECK(encode_world_loot_spawn(WorldLootSpawn{record}, spawn_bytes) ==
          WorldLootCodecError::None);
    WorldLootSpawn spawn{};
    CHECK(decode_world_loot_spawn(spawn_bytes, spawn) ==
          WorldLootCodecError::None);
    CHECK(same_record(spawn.record, record));

    WorldLootBaseline baseline{99, 456,
                               {record, make_record(18, 10, 124, 56, 99)}};
    std::vector<Byte> baseline_bytes;
    CHECK(encode_world_loot_baseline(baseline, baseline_bytes) ==
          WorldLootCodecError::None);
    CHECK(baseline_bytes.size() ==
          kWorldLootBaselineHeaderWireSize + 2 * kWorldLootRecordWireSize);
    WorldLootBaseline decoded_baseline{};
    CHECK(decode_world_loot_baseline(baseline_bytes, decoded_baseline) ==
          WorldLootCodecError::None);
    CHECK(decoded_baseline.session_epoch == baseline.session_epoch);
    CHECK(decoded_baseline.revision == baseline.revision);
    CHECK(decoded_baseline.records.size() == 2);
    if (decoded_baseline.records.size() == 2) {
        CHECK(same_record(decoded_baseline.records[0], baseline.records[0]));
        CHECK(same_record(decoded_baseline.records[1], baseline.records[1]));
    }

    const WorldLootDelta delta{99, 17, 9, 124, 12};
    std::array<Byte, kWorldLootDeltaWireSize> delta_bytes{};
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::None);
    WorldLootDelta decoded_delta{};
    CHECK(decode_world_loot_delta(delta_bytes, decoded_delta) ==
          WorldLootCodecError::None);
    CHECK(decoded_delta.session_epoch == delta.session_epoch);
    CHECK(decoded_delta.loot_id == delta.loot_id);
    CHECK(decoded_delta.generation == delta.generation);
    CHECK(decoded_delta.revision == delta.revision);
    CHECK(decoded_delta.amount == delta.amount);

    const WorldLootRemove remove{99, 17, 9, 125, 3};
    std::array<Byte, kWorldLootRemoveWireSize> remove_bytes{};
    CHECK(encode_world_loot_remove(remove, remove_bytes) ==
          WorldLootCodecError::None);
    WorldLootRemove decoded_remove{};
    CHECK(decode_world_loot_remove(remove_bytes, decoded_remove) ==
          WorldLootCodecError::None);
    CHECK(decoded_remove.session_epoch == remove.session_epoch);
    CHECK(decoded_remove.loot_id == remove.loot_id);
    CHECK(decoded_remove.generation == remove.generation);
    CHECK(decoded_remove.revision == remove.revision);
    CHECK(decoded_remove.reason == remove.reason);

    const WorldLootPickupRequest request{99, 42, 17, 9, 0xdeadbeefu, 13};
    std::array<Byte, kWorldLootPickupRequestWireSize> request_bytes{};
    CHECK(encode_world_loot_pickup_request(request, request_bytes) ==
          WorldLootCodecError::None);
    WorldLootPickupRequest decoded_request{};
    CHECK(decode_world_loot_pickup_request(request_bytes, decoded_request) ==
          WorldLootCodecError::None);
    CHECK(decoded_request.session_epoch == request.session_epoch);
    CHECK(decoded_request.entity_id == request.entity_id);
    CHECK(decoded_request.loot_id == request.loot_id);
    CHECK(decoded_request.generation == request.generation);
    CHECK(decoded_request.transaction_id == request.transaction_id);
    CHECK(decoded_request.amount == request.amount);

    const WorldLootPickupResult result{
        99, 17, 9, 0xdeadbeefu, WorldLootPickupCode::Granted, 300, 301, 13,
        42, 126};
    std::array<Byte, kWorldLootPickupResultWireSize> result_bytes{};
    CHECK(encode_world_loot_pickup_result(result, result_bytes) ==
          WorldLootCodecError::None);
    WorldLootPickupResult decoded_result{};
    CHECK(decode_world_loot_pickup_result(result_bytes, decoded_result) ==
          WorldLootCodecError::None);
    CHECK(decoded_result.session_epoch == result.session_epoch);
    CHECK(decoded_result.loot_id == result.loot_id);
    CHECK(decoded_result.generation == result.generation);
    CHECK(decoded_result.transaction_id == result.transaction_id);
    CHECK(decoded_result.code == result.code);
    CHECK(decoded_result.item_prototype_id == result.item_prototype_id);
    CHECK(decoded_result.item_instance_id == result.item_instance_id);
    CHECK(decoded_result.granted_amount == result.granted_amount);
    CHECK(decoded_result.remaining_amount == result.remaining_amount);
    CHECK(decoded_result.revision == result.revision);
}

void test_wire_headers_size_flags_and_padding()
{
    const WorldLootRecord record = make_record(17, 2, 3, 4, 5);
    std::array<Byte, kWorldLootSpawnWireSize> spawn{};
    CHECK(encode_world_loot_spawn(WorldLootSpawn{record}, spawn) ==
          WorldLootCodecError::None);
    check_fixed_wire_rejections(
        std::vector<Byte>(spawn.begin(), spawn.end()), 18,
        [](ByteView input) {
            WorldLootSpawn output{};
            return decode_world_loot_spawn(input, output);
        });

    WorldLootBaseline baseline{5, 3, {record}};
    std::vector<Byte> baseline_bytes;
    CHECK(encode_world_loot_baseline(baseline, baseline_bytes) ==
          WorldLootCodecError::None);
    check_fixed_wire_rejections(
        baseline_bytes, 18,
        [](ByteView input) {
            WorldLootBaseline output{};
            return decode_world_loot_baseline(input, output);
        });

    const WorldLootDelta delta{5, 17, 2, 4, 0};
    std::array<Byte, kWorldLootDeltaWireSize> delta_bytes{};
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::None);
    check_fixed_wire_rejections(
        std::vector<Byte>(delta_bytes.begin(), delta_bytes.end()), 18,
        [](ByteView input) {
            WorldLootDelta output{};
            return decode_world_loot_delta(input, output);
        });

    const WorldLootRemove remove{5, 17, 2, 5, 0};
    std::array<Byte, kWorldLootRemoveWireSize> remove_bytes{};
    CHECK(encode_world_loot_remove(remove, remove_bytes) ==
          WorldLootCodecError::None);
    check_fixed_wire_rejections(
        std::vector<Byte>(remove_bytes.begin(), remove_bytes.end()), 18,
        [](ByteView input) {
            WorldLootRemove output{};
            return decode_world_loot_remove(input, output);
        });

    const WorldLootPickupRequest request{5, 42, 17, 2, 6, 1};
    std::array<Byte, kWorldLootPickupRequestWireSize> request_bytes{};
    CHECK(encode_world_loot_pickup_request(request, request_bytes) ==
          WorldLootCodecError::None);
    check_fixed_wire_rejections(
        std::vector<Byte>(request_bytes.begin(), request_bytes.end()), 22,
        [](ByteView input) {
            WorldLootPickupRequest output{};
            return decode_world_loot_pickup_request(input, output);
        });

    const WorldLootPickupResult result{
        5, 17, 2, 6, WorldLootPickupCode::NotOwner, -1, -1, 0, 4, 7};
    std::array<Byte, kWorldLootPickupResultWireSize> result_bytes{};
    CHECK(encode_world_loot_pickup_result(result, result_bytes) ==
          WorldLootCodecError::None);
    check_fixed_wire_rejections(
        std::vector<Byte>(result_bytes.begin(), result_bytes.end()), 18,
        [](ByteView input) {
            WorldLootPickupResult output{};
            return decode_world_loot_pickup_result(input, output);
        });
    std::vector<Byte> result_padding(result_bytes.begin(), result_bytes.end());
    result_padding[25] = Byte{1};
    WorldLootPickupResult decoded_result{};
    CHECK(decode_world_loot_pickup_result(result_padding, decoded_result) ==
          WorldLootCodecError::BadFlags);
}

void test_validation_bounds_and_duplicates()
{
    const WorldLootRecord valid = make_record(17, 2, 3, 4, 5);
    std::array<Byte, kWorldLootSpawnWireSize> bytes{};
    WorldLootSpawn spawn{valid};
    std::array<Byte, kWorldLootSpawnWireSize - 1> too_small{};
    CHECK(encode_world_loot_spawn(spawn, too_small) ==
          WorldLootCodecError::OutputTooSmall);

    spawn.record.session_epoch = 0;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidSessionEpoch);
    spawn.record = valid;
    spawn.record.loot_id = 0;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidLootId);
    spawn.record = valid;
    spawn.record.generation = 0;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidGeneration);
    spawn.record = valid;
    spawn.record.revision = 0;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidRevision);
    spawn.record = valid;
    spawn.record.container_id = 0;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidContainer);
    spawn.record = valid;
    spawn.record.container_prototype_id = -1;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidPrototype);
    spawn.record = valid;
    spawn.record.item_prototype_id = -1;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidPrototype);
    spawn.record = valid;
    spawn.record.item_instance_id = -2;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidPrototype);
    spawn.record = valid;
    spawn.record.amount = 0;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidAmount);
    spawn.record = valid;
    spawn.record.transform.position_x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidTransform);
    spawn.record = valid;
    spawn.record.transform.position_y =
        std::numeric_limits<float>::infinity();
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidTransform);
    spawn.record = valid;
    spawn.record.transform.rotation_w = 0.0f;
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::InvalidTransform);
    spawn.record = valid;
    spawn.record.transform.position_x =
        std::numeric_limits<float>::max();
    CHECK(encode_world_loot_spawn(spawn, bytes) ==
          WorldLootCodecError::None);

    WorldLootBaseline baseline{5, 3, {valid}};
    std::vector<Byte> baseline_bytes;
    baseline.records[0].session_epoch = 6;
    CHECK(encode_world_loot_baseline(baseline, baseline_bytes) ==
          WorldLootCodecError::InvalidSessionEpoch);
    baseline = WorldLootBaseline{5, 3, {valid, valid}};
    CHECK(encode_world_loot_baseline(baseline, baseline_bytes) ==
          WorldLootCodecError::DuplicateRecord);
    baseline = WorldLootBaseline{5, 3, {}};
    for (std::size_t index = 0; index != kMaxWorldLootBaselineRecords + 1;
         ++index)
        baseline.records.push_back(make_record(
            static_cast<WorldLootId>(index + 1), 1, 3, 1, 5));
    CHECK(encode_world_loot_baseline(baseline, baseline_bytes) ==
          WorldLootCodecError::TooManyRecords);

    WorldLootDelta delta{5, 17, 2, 3, 0};
    std::array<Byte, kWorldLootDeltaWireSize> delta_bytes{};
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::None);
    delta.session_epoch = 0;
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::InvalidSessionEpoch);
    delta = WorldLootDelta{5, 0, 2, 3, 1};
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::InvalidLootId);
    delta = WorldLootDelta{5, 17, 0, 3, 1};
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::InvalidGeneration);
    delta = WorldLootDelta{5, 17, 2, 0, 1};
    CHECK(encode_world_loot_delta(delta, delta_bytes) ==
          WorldLootCodecError::InvalidRevision);

    const WorldLootRemove invalid_remove{5, 17, 2, 3, 4};
    std::array<Byte, kWorldLootRemoveWireSize> remove_bytes{};
    CHECK(encode_world_loot_remove(invalid_remove, remove_bytes) ==
          WorldLootCodecError::InvalidReason);

    WorldLootPickupRequest request{5, 42, 17, 2, 3, 0};
    std::array<Byte, kWorldLootPickupRequestWireSize> request_bytes{};
    CHECK(encode_world_loot_pickup_request(request, request_bytes) ==
          WorldLootCodecError::InvalidAmount);
    request.amount = 1;
    request.transaction_id = 0;
    CHECK(encode_world_loot_pickup_request(request, request_bytes) ==
          WorldLootCodecError::InvalidTransactionId);

    WorldLootPickupResult result{
        5, 17, 2, 3, WorldLootPickupCode::Granted, 300, -1, 1, 0, 4};
    std::array<Byte, kWorldLootPickupResultWireSize> result_bytes{};
    CHECK(encode_world_loot_pickup_result(result, result_bytes) ==
          WorldLootCodecError::None);
    result.code = static_cast<WorldLootPickupCode>(255);
    CHECK(encode_world_loot_pickup_result(result, result_bytes) ==
          WorldLootCodecError::InvalidPickupCode);
    result.code = WorldLootPickupCode::Granted;
    result.item_instance_id = -2;
    CHECK(encode_world_loot_pickup_result(result, result_bytes) ==
          WorldLootCodecError::InvalidPrototype);
    result = WorldLootPickupResult{
        5, 17, 2, 3, WorldLootPickupCode::Granted, 300, -1, 1, 0, 0};
    CHECK(encode_world_loot_pickup_result(result, result_bytes) ==
          WorldLootCodecError::InvalidRevision);
}

void test_baseline_duplicate_decode_and_order()
{
    const WorldLootRecord first = make_record(101, 1, 10, 2, 8);
    const WorldLootRecord second = make_record(102, 1, 11, 3, 8);
    WorldLootBaseline baseline{8, 11, {first, second}};
    std::vector<Byte> bytes;
    CHECK(encode_world_loot_baseline(baseline, bytes) ==
          WorldLootCodecError::None);
    WorldLootBaseline decoded{};
    CHECK(decode_world_loot_baseline(bytes, decoded) ==
          WorldLootCodecError::None);
    CHECK(decoded.records.size() == 2);
    if (decoded.records.size() == 2) {
        CHECK(decoded.records[0].loot_id == 101);
        CHECK(decoded.records[1].loot_id == 102);
    }
    put_u32(bytes, kWorldLootBaselineHeaderWireSize +
                      kWorldLootRecordWireSize + 12,
            first.loot_id);
    CHECK(decode_world_loot_baseline(bytes, decoded) ==
          WorldLootCodecError::DuplicateRecord);
}

void test_replica_order_delta_and_idempotence()
{
    WorldLootReplica replica;
    const WorldLootRecord first = make_record(101, 1, 10, 20, 8);
    const WorldLootRecord second = make_record(102, 1, 10, 30, 8);
    CHECK(replica.apply_delta(WorldLootDelta{8, 101, 1, 11, 19}) ==
          WorldLootApplyResult::Stale);
    const WorldLootBaseline baseline{8, 10, {first, second}};
    CHECK(replica.apply_baseline(baseline) == WorldLootApplyResult::Applied);
    CHECK(replica.session_epoch() == 8);
    CHECK(replica.revision() == 10);
    CHECK(replica.records().size() == 2);
    if (replica.records().size() == 2) {
        CHECK(replica.records()[0].loot_id == 101);
        CHECK(replica.records()[1].loot_id == 102);
    }
    CHECK(replica.apply_baseline(baseline) == WorldLootApplyResult::Duplicate);
    CHECK(replica.apply_delta(WorldLootDelta{8, 101, 1, 11, 19}) ==
          WorldLootApplyResult::Applied);
    CHECK(replica.find(101) != nullptr && replica.find(101)->amount == 19);
    CHECK(replica.apply_delta(WorldLootDelta{8, 101, 1, 11, 19}) ==
          WorldLootApplyResult::Duplicate);
    CHECK(replica.apply_delta(WorldLootDelta{8, 101, 1, 11, 18}) ==
          WorldLootApplyResult::Stale);
    CHECK(replica.apply_delta(WorldLootDelta{8, 101, 2, 12, 18}) ==
          WorldLootApplyResult::Stale);
    CHECK(replica.apply_delta(WorldLootDelta{8, 999, 1, 12, 18}) ==
          WorldLootApplyResult::Stale);
}

void test_replica_tombstones_generation_reuse()
{
    WorldLootReplica replica;
    const WorldLootSpawn spawn{make_record(7, 1, 2, 4, 8)};
    CHECK(replica.apply_spawn(spawn) == WorldLootApplyResult::Applied);
    CHECK(replica.apply_spawn(spawn) == WorldLootApplyResult::Duplicate);
    const WorldLootRemove remove{8, 7, 1, 3, 1};
    CHECK(replica.apply_remove(remove) == WorldLootApplyResult::Applied);
    CHECK(replica.find(7) == nullptr);
    CHECK(replica.apply_remove(remove) == WorldLootApplyResult::Duplicate);
    CHECK(replica.apply_spawn(spawn) == WorldLootApplyResult::Stale);
    CHECK(replica.apply_delta(WorldLootDelta{8, 7, 1, 4, 2}) ==
          WorldLootApplyResult::Stale);
    const WorldLootSpawn reused{make_record(7, 2, 4, 9, 8)};
    CHECK(replica.apply_spawn(reused) == WorldLootApplyResult::Applied);
    CHECK(replica.find(7) != nullptr && replica.find(7)->generation == 2);
    CHECK(replica.apply_remove(WorldLootRemove{8, 7, 1, 5, 1}) ==
          WorldLootApplyResult::Stale);
    CHECK(replica.find(7) != nullptr && replica.find(7)->amount == 9);
    CHECK(replica.apply_remove(WorldLootRemove{8, 7, 2, 5, 1}) ==
          WorldLootApplyResult::Applied);
    CHECK(replica.find(7) == nullptr);
    CHECK(replica.apply_spawn(WorldLootSpawn{make_record(7, 3, 6, 10, 8)}) ==
          WorldLootApplyResult::Applied);
}

void test_replica_epoch_reset_and_wrap_order()
{
    WorldLootReplica replica;
    const WorldLootBaseline epoch_10{
        10, 50, {make_record(1, 1, 50, 5, 10), make_record(2, 1, 50, 6, 10)}};
    CHECK(replica.apply_baseline(epoch_10) == WorldLootApplyResult::Applied);
    CHECK(replica.apply_spawn(WorldLootSpawn{make_record(3, 1, 51, 1, 9)}) ==
          WorldLootApplyResult::WrongSessionEpoch);
    CHECK(replica.apply_remove(WorldLootRemove{9, 1, 1, 52, 1}) ==
          WorldLootApplyResult::WrongSessionEpoch);
    const WorldLootBaseline epoch_11{11, 1, {make_record(4, 1, 1, 7, 11)}};
    CHECK(replica.apply_baseline(epoch_11) == WorldLootApplyResult::Applied);
    CHECK(replica.session_epoch() == 11);
    CHECK(replica.revision() == 1);
    CHECK(replica.records().size() == 1);
    CHECK(replica.find(1) == nullptr);
    CHECK(replica.find(4) != nullptr);
    CHECK(replica.apply_delta(WorldLootDelta{10, 4, 1, 2, 3}) ==
          WorldLootApplyResult::WrongSessionEpoch);
    CHECK(replica.apply_baseline(epoch_10) == WorldLootApplyResult::WrongSessionEpoch);

    WorldLootReplica wrapped;
    const WorldLootBaseline high_revision{
        12, 0xfffffffEu, {make_record(1, 1, 0xfffffffEu, 4, 12)}};
    CHECK(wrapped.apply_baseline(high_revision) == WorldLootApplyResult::Applied);
    CHECK(wrapped.apply_delta(WorldLootDelta{12, 1, 1, 1, 3}) ==
          WorldLootApplyResult::Applied);
    CHECK(wrapped.revision() == 1);
    CHECK(wrapped.apply_delta(WorldLootDelta{12, 1, 1, 0x80000001u, 2}) ==
          WorldLootApplyResult::Stale);
}

void test_replica_rejects_global_stale_spawn_and_clear()
{
    WorldLootReplica replica;
    CHECK(replica.apply_baseline(
              WorldLootBaseline{20, 10, {make_record(10, 1, 10, 1, 20)}}) ==
          WorldLootApplyResult::Applied);
    CHECK(replica.apply_spawn(
              WorldLootSpawn{make_record(11, 1, 9, 1, 20)}) ==
          WorldLootApplyResult::Stale);
    CHECK(replica.find(11) == nullptr);
    CHECK(replica.apply_remove(WorldLootRemove{20, 10, 1, 11, 1}) ==
          WorldLootApplyResult::Applied);
    replica.clear();
    CHECK(replica.session_epoch() == 0);
    CHECK(replica.revision() == 0);
    CHECK(replica.records().empty());
    CHECK(replica.apply_spawn(WorldLootSpawn{make_record(10, 1, 1, 1, 21)}) ==
          WorldLootApplyResult::Applied);
}

} // namespace

int main()
{
    test_codec_round_trips();
    test_wire_headers_size_flags_and_padding();
    test_validation_bounds_and_duplicates();
    test_baseline_duplicate_decode_and_order();
    test_replica_order_delta_and_idempotence();
    test_replica_tombstones_generation_reuse();
    test_replica_epoch_reset_and_wrap_order();
    test_replica_rejects_global_stale_spawn_and_clear();
    if (failures != 0) {
        std::cerr << failures << " world-loot test(s) failed\n";
        return 1;
    }
    std::cout << "world loot tests passed\n";
    return 0;
}
