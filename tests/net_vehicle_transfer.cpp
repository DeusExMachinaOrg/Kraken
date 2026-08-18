#include "net/vehicle_transfer.hpp"
#include "net/wire_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace kraken::net;

namespace {

VehicleDescriptor sample_descriptor()
{
    VehicleDescriptor descriptor{};
    descriptor.prototype_id = 42;
    descriptor.prototype_name = "vehicle_42";
    descriptor.health = 0.75f;
    descriptor.attachments.push_back({VehicleDescriptorNodeKind::Attachment,
                                      2, 0, "CABIN", 43, "gun_43"});
    return descriptor;
}

void test_snapshot_and_delta()
{
    std::vector<Byte> bytes;
    WorldSnapshotTransfer snapshot{9, 4, {3, 0, {Byte{1}, Byte{2}}}};
    assert(encode_world_snapshot_transfer(snapshot, bytes) ==
           VehicleTransferCodecError::None);
    WorldSnapshotTransfer decoded{};
    assert(decode_world_snapshot_transfer(bytes, decoded) ==
           VehicleTransferCodecError::None);
    assert(decoded.snapshot.epoch == 3);
    assert(decoded.snapshot.revision == 0);
    assert(decoded.snapshot.payload == snapshot.snapshot.payload);

    WorldDeltaTransfer delta{9, 4, {3, 7, {Byte{8}}}};
    assert(encode_world_delta_transfer(delta, bytes) ==
           VehicleTransferCodecError::None);
    WorldDeltaTransfer delta_decoded{};
    assert(decode_world_delta_transfer(bytes, delta_decoded) ==
           VehicleTransferCodecError::None);
    assert(delta_decoded.delta.revision == 7);
}

void test_descriptor_and_ready()
{
    std::vector<Byte> bytes;
    VehicleDescriptorTransfer transfer{9, 4, 1, 11, 2, sample_descriptor()};
    assert(encode_vehicle_descriptor_transfer(transfer, bytes) ==
           VehicleTransferCodecError::None);
    VehicleDescriptorTransfer decoded{};
    assert(decode_vehicle_descriptor_transfer(bytes, decoded) ==
           VehicleTransferCodecError::None);
    assert(decoded.entity_id == 11);
    assert(decoded.descriptor.semantic_equal(transfer.descriptor));

    // Descriptor v1 is intentionally not a partial-compatible payload: it
    // lacks the v2 target-resource/modifier/gun/repository contract.
    constexpr std::size_t kDescriptorTransferHeaderSize = 40;
    assert(bytes.size() > kDescriptorTransferHeaderSize + 5);
    bytes[kDescriptorTransferHeaderSize + 4] = Byte{1};
    bytes[kDescriptorTransferHeaderSize + 5] = Byte{0};
    assert(decode_vehicle_descriptor_transfer(bytes, decoded) ==
           VehicleTransferCodecError::BadVersion);

    WorldTransferReady ready{9, 4, 1, 3, 0, 2};
    assert(encode_world_transfer_ready(ready, bytes) ==
           VehicleTransferCodecError::None);
    WorldTransferReady ready_decoded{};
    assert(decode_world_transfer_ready(bytes, ready_decoded) ==
           VehicleTransferCodecError::None);
    assert(ready_decoded.world_revision == 0);
    assert(requires_reliable_channel(MessageType::MatchWorldSnapshot));
    assert(requires_reliable_channel(MessageType::MatchWorldDelta));
    assert(requires_reliable_channel(MessageType::MatchVehicleDescriptor));
    assert(requires_reliable_channel(MessageType::MatchWorldReady));
}

} // namespace

int main()
{
    assert(runtime_authority(false, false) == RuntimeAuthority::Local);
    assert(runtime_authority(true, true) == RuntimeAuthority::Host);
    assert(runtime_authority(true, false) == RuntimeAuthority::ClientReplica);
    assert(!may_authoritatively_mutate_world(RuntimeAuthority::ClientReplica));
    assert(may_authoritatively_mutate_world(RuntimeAuthority::Host));
    assert(!is_gameplay_level_name("main_menu"));
    assert(!is_gameplay_level_name(""));
    assert(is_gameplay_level_name("r1m1"));
    assert(suppress_same_belong_damage(false, 2, 2));
    assert(!suppress_same_belong_damage(true, 2, 2));
    assert(!suppress_same_belong_damage(false, 2, 3));
    test_snapshot_and_delta();
    test_descriptor_and_ready();
    return 0;
}
