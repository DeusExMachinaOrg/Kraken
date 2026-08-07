#ifndef KRAKEN_NET_VEHICLE_SNAPSHOT_HPP
#define KRAKEN_NET_VEHICLE_SNAPSHOT_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>

namespace kraken::net {

// The snapshot wire schema is deliberately defined by offsets and field widths
// below.  It does not depend on compiler ABI, host endianness, or the layout of
// any engine/physics type.
inline constexpr std::uint32_t kVehicleSnapshotWireMagic = 0x31534B56u;
inline constexpr std::uint16_t kVehicleSnapshotWireVersion = 1;
inline constexpr std::uint16_t kVehicleSnapshotWireFlags = 0;
inline constexpr std::size_t kVehicleSnapshotWireSize = 72;

inline constexpr float kVehicleSnapshotMaxPositionComponent = 1'000'000.0f;
inline constexpr float kVehicleSnapshotMaxLinearVelocityComponent = 100'000.0f;
inline constexpr float kVehicleSnapshotMaxAngularVelocityComponent = 100'000.0f;
inline constexpr float kVehicleSnapshotQuaternionNormTolerance = 0.001f;

struct VehicleVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct VehicleQuaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct VehicleSnapshot {
    // Authoritative network entity identity. Engine objId is deliberately not
    // serialized: each process owns its local objId allocation.
    std::uint32_t entity_id = 0;
    std::uint32_t sequence = 0;
    std::uint32_t server_tick = 0;
    VehicleVector3 position{};
    VehicleQuaternion rotation{};
    VehicleVector3 linear_velocity{};
    VehicleVector3 angular_velocity{};
};

enum class VehicleSnapshotCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidEntityId,
    NonFiniteValue,
    ValueOutOfBounds,
    InvalidQuaternion,
};

[[nodiscard]] constexpr bool vehicle_snapshot_codec_succeeded(
    VehicleSnapshotCodecError error) noexcept
{
    return error == VehicleSnapshotCodecError::None;
}

// Encodes exactly kVehicleSnapshotWireSize bytes. Additional output capacity is
// permitted; bytes after the schema are left untouched.
[[nodiscard]] VehicleSnapshotCodecError encode_vehicle_snapshot(
    const VehicleSnapshot& snapshot, MutableByteView output) noexcept;

// Decodes exactly kVehicleSnapshotWireSize bytes. The destination is changed
// only after every header, finite-value, bounds, and quaternion check succeeds.
[[nodiscard]] VehicleSnapshotCodecError decode_vehicle_snapshot(
    ByteView input, VehicleSnapshot& snapshot) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_VEHICLE_SNAPSHOT_HPP
