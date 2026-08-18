#include "net/world_replication.hpp"
#include "net/world_state_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace kraken::net;

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::vector<Byte> bytes(std::initializer_list<std::uint8_t> values)
{
    std::vector<Byte> result;
    result.reserve(values.size());
    for (const std::uint8_t value : values)
        result.push_back(static_cast<Byte>(value));
    return result;
}

ObjectRecord object(const HostObjectId id, const ObjectTypeId type,
                    const HostObjectId parent, std::initializer_list<std::uint8_t> runtime,
                    std::initializer_list<PropertySnapshot> properties = {})
{
    ObjectRecord result{};
    result.object_id = id;
    result.type_id = type;
    result.parent_id = parent;
    result.runtime = bytes(runtime);
    result.properties = properties;
    return result;
}

void put_u64(std::vector<Byte>& wire, const std::size_t offset,
             const std::uint64_t value)
{
    for (unsigned index = 0; index != 8; ++index)
        wire[offset + index] =
            static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

std::vector<ObjectRecord> rich_world()
{
    return {
        object(50, 500, 10, {5}, {{9, bytes({9})}, {3, bytes({3, 4})}}),
        object(10, 100, 0, {1, 2}, {{8, bytes({8})}}),
        object(20, 200, 10, {6}, {{1, bytes({6})}}),
    };
}

void test_empty_initial_snapshot_revision_zero()
{
    WorldJournal journal(4);
    const WorldSnapshot outer = journal.capture_snapshot();
    CHECK(outer.epoch != 0);
    CHECK(outer.revision == 0);

    std::vector<Byte> payload;
    CHECK(encode_world_state_snapshot(std::span<const ObjectRecord>{}, payload) ==
          WorldStateSnapshotCodecError::None);
    CHECK(payload.size() == kWorldStateSnapshotWireHeaderSize);
    const WorldSnapshot initial = journal.capture_snapshot(payload);
    CHECK(initial.revision == 0);

    std::vector<ObjectRecord> decoded{
        object(99, 99, 0, {9})};
    CHECK(decode_world_state_snapshot(initial.payload, decoded) ==
          WorldStateSnapshotCodecError::None);
    CHECK(decoded.empty());
    CHECK(world_state_snapshot_digest_hex(decoded).size() == 64);
}

void test_rich_graph_round_trip_and_application_order()
{
    const std::vector<ObjectRecord> source = rich_world();
    std::vector<Byte> wire;
    CHECK(encode_world_state_snapshot(source, wire) ==
          WorldStateSnapshotCodecError::None);

    std::vector<ObjectRecord> decoded;
    CHECK(decode_world_state_snapshot(wire, decoded) ==
          WorldStateSnapshotCodecError::None);
    CHECK(world_state_snapshot_semantic_equal(source, decoded));
    CHECK(decoded.size() == 3);
    CHECK(decoded[0].object_id == 10);
    CHECK(decoded[1].object_id == 20);
    CHECK(decoded[2].object_id == 50);
    CHECK(decoded[2].properties[0].property_id == 3);
    CHECK(decoded[2].properties[1].property_id == 9);

    std::vector<std::string> operations;
    WorldStateSnapshotVisitor visitor{};
    visitor.create_object = [&operations](const HostObjectId id,
                                           const ObjectTypeId type) {
        operations.push_back("create:" + std::to_string(id) + ":" +
                             std::to_string(type));
        return true;
    };
    visitor.relationship = [&operations](const HostObjectId parent,
                                          const HostObjectId child) {
        operations.push_back("parent:" + std::to_string(parent) + ">" +
                             std::to_string(child));
        return true;
    };
    visitor.runtime = [&operations](const HostObjectId id, const ByteView value) {
        operations.push_back("runtime:" + std::to_string(id) + ":" +
                             std::to_string(value.size()));
        return true;
    };
    visitor.property = [&operations](const HostObjectId id, const PropertyId property,
                                     const ByteView value) {
        operations.push_back("property:" + std::to_string(id) + ":" +
                             std::to_string(property) + ":" +
                             std::to_string(value.size()));
        return true;
    };
    CHECK(apply_world_state_snapshot(decoded, visitor) ==
          WorldStateSnapshotApplyError::None);
    CHECK((operations == std::vector<std::string>{
        "create:10:100", "create:20:200", "create:50:500",
        "parent:10>20", "parent:10>50", "runtime:10:2", "property:10:8:1",
        "runtime:20:1", "property:20:1:1", "runtime:50:1",
        "property:50:3:2", "property:50:9:1"}));
}

void test_reorder_is_canonical_and_digest_stable()
{
    std::vector<ObjectRecord> reordered = rich_world();
    std::reverse(reordered.begin(), reordered.end());
    std::reverse(reordered[2].properties.begin(), reordered[2].properties.end());

    std::vector<Byte> first;
    std::vector<Byte> second;
    CHECK(encode_world_state_snapshot(rich_world(), first) ==
          WorldStateSnapshotCodecError::None);
    CHECK(encode_world_state_snapshot(reordered, second) ==
          WorldStateSnapshotCodecError::None);
    CHECK(first == second);
    CHECK(world_state_snapshot_semantic_equal(rich_world(), reordered));
    CHECK(world_state_snapshot_digest(rich_world()) ==
          world_state_snapshot_digest(reordered));
    CHECK(world_state_snapshot_digest(first) ==
          world_state_snapshot_digest(second));
}

void test_malformed_oversize_and_graph_rejection_are_transactional()
{
    const std::vector<ObjectRecord> valid{
        object(1, 1, 0, {1}), object(2, 2, 1, {2})};
    std::vector<Byte> wire;
    CHECK(encode_world_state_snapshot(valid, wire) ==
          WorldStateSnapshotCodecError::None);

    std::vector<ObjectRecord> sentinel{object(88, 88, 0, {8})};
    std::vector<Byte> truncated = wire;
    truncated.pop_back();
    CHECK(decode_world_state_snapshot(truncated, sentinel) ==
          WorldStateSnapshotCodecError::InputSizeMismatch);
    CHECK(sentinel.size() == 1 && sentinel[0].object_id == 88);

    std::vector<Byte> cycle = wire;
    put_u64(cycle, kWorldStateSnapshotWireHeaderSize + 12, 2);
    put_u64(cycle, kWorldStateSnapshotWireHeaderSize + 28 + 1 + 12, 1);
    CHECK(decode_world_state_snapshot(cycle, sentinel) ==
          WorldStateSnapshotCodecError::ParentCycle);
    CHECK(sentinel[0].object_id == 88);

    auto unknown_parent = valid;
    unknown_parent[1].parent_id = 999;
    CHECK(encode_world_state_snapshot(unknown_parent, wire) ==
          WorldStateSnapshotCodecError::UnknownParent);

    auto duplicate_property = valid;
    duplicate_property[0].properties = {{1, bytes({1})}, {1, bytes({2})}};
    CHECK(encode_world_state_snapshot(duplicate_property, wire) ==
          WorldStateSnapshotCodecError::DuplicatePropertyId);

    auto duplicate_object = valid;
    duplicate_object[1].object_id = duplicate_object[0].object_id;
    CHECK(encode_world_state_snapshot(duplicate_object, wire) ==
          WorldStateSnapshotCodecError::DuplicateObjectId);

    auto zero_object = valid;
    zero_object[0].object_id = 0;
    CHECK(encode_world_state_snapshot(zero_object, wire) ==
          WorldStateSnapshotCodecError::InvalidObjectId);

    auto zero_property = valid;
    zero_property[0].properties = {{0, bytes({1})}};
    CHECK(encode_world_state_snapshot(zero_property, wire) ==
          WorldStateSnapshotCodecError::InvalidPropertyId);

    auto zero_id = valid;
    zero_id[0].type_id = 0;
    CHECK(encode_world_state_snapshot(zero_id, wire) ==
          WorldStateSnapshotCodecError::InvalidTypeId);

    auto oversize = valid;
    oversize[0].runtime.assign(kWorldStateSnapshotMaxBlobBytes + 1, Byte{});
    CHECK(encode_world_state_snapshot(oversize, wire) ==
          WorldStateSnapshotCodecError::BlobTooLarge);

    std::vector<Byte> oversized_wire(
        kWorldStateSnapshotMaxWireBytes + 1, Byte{});
    CHECK(decode_world_state_snapshot(oversized_wire, sentinel) ==
          WorldStateSnapshotCodecError::WireTooLarge);
    CHECK(sentinel[0].object_id == 88);
}

void test_mutation_during_barrier_fixture()
{
    WorldJournal journal(8);
    std::vector<Byte> snapshot_payload;
    CHECK(encode_world_state_snapshot(rich_world(), snapshot_payload) ==
          WorldStateSnapshotCodecError::None);
    const WorldSnapshot snapshot = journal.capture_snapshot(snapshot_payload);
    const WorldDelta during_transfer_one = journal.append(bytes({2}));
    const WorldDelta during_transfer_two = journal.append(bytes({3}));

    std::vector<std::string> applied;
    WorldJoinBarrier barrier(
        8,
        [&applied](const ByteView payload) {
            std::vector<ObjectRecord> records;
            if (decode_world_state_snapshot(payload, records) !=
                WorldStateSnapshotCodecError::None)
                return false;
            applied.push_back("snapshot:" + std::to_string(records.size()));
            return true;
        },
        [&applied](const WorldDelta& delta) {
            applied.push_back("delta:" +
                              std::to_string(std::to_integer<unsigned char>(
                                  delta.payload.front())));
            return true;
        });

    CHECK(snapshot.revision == 0);
    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(barrier.accept_delta(during_transfer_one) ==
          WorldJoinResult::Buffered);
    CHECK(barrier.accept_delta(during_transfer_two) ==
          WorldJoinResult::Buffered);
    CHECK(applied.empty());
    CHECK(barrier.commit(snapshot) == WorldJoinResult::Ready);
    CHECK((applied == std::vector<std::string>{
        "snapshot:3", "delta:2", "delta:3"}));

    const WorldDelta after_join = journal.append(bytes({4}));
    CHECK(barrier.accept_delta(after_join) == WorldJoinResult::Applied);
    CHECK(applied.back() == "delta:4");
}

} // namespace

int main()
{
    test_empty_initial_snapshot_revision_zero();
    test_rich_graph_round_trip_and_application_order();
    test_reorder_is_canonical_and_digest_stable();
    test_malformed_oversize_and_graph_rejection_are_transactional();
    test_mutation_during_barrier_fixture();

    if (failures != 0) {
        std::cerr << failures << " world state snapshot test(s) failed\n";
        return 1;
    }
    std::cout << "world state snapshot tests passed\n";
    return 0;
}
