#include "net/vehicle_snapshot.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using namespace kraken::net;

bool check(bool condition, const char* description)
{
    if (!condition)
        std::cerr << "FAILED: " << description << '\n';
    return condition;
}

bool same(float left, float right)
{
    return std::bit_cast<std::uint32_t>(left) ==
           std::bit_cast<std::uint32_t>(right);
}

bool same(const VehicleSnapshot& left, const VehicleSnapshot& right)
{
    return left.sequence == right.sequence &&
           left.server_tick == right.server_tick &&
           same(left.position.x, right.position.x) &&
           same(left.position.y, right.position.y) &&
           same(left.position.z, right.position.z) &&
           same(left.rotation.x, right.rotation.x) &&
           same(left.rotation.y, right.rotation.y) &&
           same(left.rotation.z, right.rotation.z) &&
           same(left.rotation.w, right.rotation.w) &&
           same(left.linear_velocity.x, right.linear_velocity.x) &&
           same(left.linear_velocity.y, right.linear_velocity.y) &&
           same(left.linear_velocity.z, right.linear_velocity.z) &&
           same(left.angular_velocity.x, right.angular_velocity.x) &&
           same(left.angular_velocity.y, right.angular_velocity.y) &&
           same(left.angular_velocity.z, right.angular_velocity.z);
}

void put_u32_le(Byte* destination, std::uint32_t value)
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
    destination[2] = static_cast<Byte>((value >> 16) & 0xffu);
    destination[3] = static_cast<Byte>((value >> 24) & 0xffu);
}

} // namespace

int main()
{
    using namespace kraken::net;

    VehicleSnapshot expected{
        0xFEDCBA98u,
        0x10203040u,
        {123.5f, -456.25f, 789.75f},
        {0.0f, 0.70710677f, 0.0f, 0.70710677f},
        {-12.0f, 34.5f, 0.25f},
        {1.0f, -2.0f, 3.5f},
    };

    std::array<Byte, kVehicleSnapshotWireSize> wire{};
    bool passed = true;
    passed &= check(encode_vehicle_snapshot(expected, wire) ==
                        VehicleSnapshotCodecError::None,
                    "roundtrip encode");
    passed &= check(static_cast<std::uint8_t>(wire[8]) == 0x98u &&
                        static_cast<std::uint8_t>(wire[9]) == 0xBAu &&
                        static_cast<std::uint8_t>(wire[10]) == 0xDCu &&
                        static_cast<std::uint8_t>(wire[11]) == 0xFEu,
                    "sequence is little-endian");

    VehicleSnapshot decoded{};
    passed &= check(decode_vehicle_snapshot(wire, decoded) ==
                        VehicleSnapshotCodecError::None,
                    "roundtrip decode");
    passed &= check(same(expected, decoded), "roundtrip preserves values");

    std::array<Byte, kVehicleSnapshotWireSize - 1> truncated{};
    passed &= check(decode_vehicle_snapshot(truncated, decoded) ==
                        VehicleSnapshotCodecError::InputSizeMismatch,
                    "truncated input is rejected");
    passed &= check(encode_vehicle_snapshot(expected, truncated) ==
                        VehicleSnapshotCodecError::OutputTooSmall,
                    "truncated output is rejected");

    VehicleSnapshot nan_snapshot = expected;
    nan_snapshot.position.x = std::numeric_limits<float>::quiet_NaN();
    passed &= check(encode_vehicle_snapshot(nan_snapshot, wire) ==
                        VehicleSnapshotCodecError::NonFiniteValue,
                    "NaN is rejected on encode");

    put_u32_le(wire.data() + 16,
               std::bit_cast<std::uint32_t>(
                   std::numeric_limits<float>::quiet_NaN()));
    passed &= check(decode_vehicle_snapshot(wire, decoded) ==
                        VehicleSnapshotCodecError::NonFiniteValue,
                    "NaN is rejected on decode");

    passed &= check(encode_vehicle_snapshot(expected, wire) ==
                        VehicleSnapshotCodecError::None,
                    "restore valid wire");
    VehicleSnapshot invalid_quaternion = expected;
    invalid_quaternion.rotation = {0.0f, 0.0f, 0.0f, 0.0f};
    passed &= check(encode_vehicle_snapshot(invalid_quaternion, wire) ==
                        VehicleSnapshotCodecError::InvalidQuaternion,
                    "zero quaternion is rejected");

    passed &= check(encode_vehicle_snapshot(expected, wire) ==
                        VehicleSnapshotCodecError::None,
                    "restore valid wire before quaternion decode");
    put_u32_le(wire.data() + 28 + 12,
               std::bit_cast<std::uint32_t>(0.0f));
    passed &= check(decode_vehicle_snapshot(wire, decoded) ==
                        VehicleSnapshotCodecError::InvalidQuaternion,
                    "invalid quaternion is rejected on decode");

    VehicleSnapshot out_of_bounds = expected;
    out_of_bounds.position.y = kVehicleSnapshotMaxPositionComponent + 1.0f;
    passed &= check(encode_vehicle_snapshot(out_of_bounds, wire) ==
                        VehicleSnapshotCodecError::ValueOutOfBounds,
                    "out-of-bounds position is rejected");

    return passed ? 0 : 1;
}
