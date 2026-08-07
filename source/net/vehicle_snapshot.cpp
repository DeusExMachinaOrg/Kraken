#include "net/vehicle_snapshot.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

namespace kraken::net {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kFlagsOffset = 6;
constexpr std::size_t kSequenceOffset = 8;
constexpr std::size_t kServerTickOffset = 12;
constexpr std::size_t kPositionOffset = 16;
constexpr std::size_t kRotationOffset = 28;
constexpr std::size_t kLinearVelocityOffset = 44;
constexpr std::size_t kAngularVelocityOffset = 56;

void put_u16(Byte* destination, std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* destination, std::uint32_t value) noexcept
{
    destination[0] = static_cast<Byte>((value >> 0) & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
    destination[2] = static_cast<Byte>((value >> 16) & 0xffu);
    destination[3] = static_cast<Byte>((value >> 24) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* source) noexcept
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(source[0]) << 0) |
        (static_cast<std::uint16_t>(source[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const Byte* source) noexcept
{
    return (static_cast<std::uint32_t>(source[0]) << 0) |
           (static_cast<std::uint32_t>(source[1]) << 8) |
           (static_cast<std::uint32_t>(source[2]) << 16) |
           (static_cast<std::uint32_t>(source[3]) << 24);
}

void put_f32(Byte* destination, float value) noexcept
{
    put_u32(destination, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] float get_f32(const Byte* source) noexcept
{
    return std::bit_cast<float>(get_u32(source));
}

void put_vector(Byte* destination, const VehicleVector3& value) noexcept
{
    put_f32(destination + 0, value.x);
    put_f32(destination + 4, value.y);
    put_f32(destination + 8, value.z);
}

[[nodiscard]] VehicleVector3 get_vector(const Byte* source) noexcept
{
    return {get_f32(source + 0), get_f32(source + 4), get_f32(source + 8)};
}

void put_quaternion(Byte* destination, const VehicleQuaternion& value) noexcept
{
    put_f32(destination + 0, value.x);
    put_f32(destination + 4, value.y);
    put_f32(destination + 8, value.z);
    put_f32(destination + 12, value.w);
}

[[nodiscard]] VehicleQuaternion get_quaternion(const Byte* source) noexcept
{
    return {get_f32(source + 0), get_f32(source + 4), get_f32(source + 8),
            get_f32(source + 12)};
}

[[nodiscard]] bool finite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool finite(const VehicleVector3& value) noexcept
{
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const VehicleQuaternion& value) noexcept
{
    return finite(value.x) && finite(value.y) && finite(value.z) &&
           finite(value.w);
}

[[nodiscard]] bool within_abs(float value, float limit) noexcept
{
    return finite(value) && std::fabs(value) <= limit;
}

[[nodiscard]] bool within_abs(const VehicleVector3& value, float limit) noexcept
{
    return within_abs(value.x, limit) && within_abs(value.y, limit) &&
           within_abs(value.z, limit);
}

[[nodiscard]] bool valid_quaternion(const VehicleQuaternion& value) noexcept
{
    if (!finite(value))
        return false;

    const float norm_squared = value.x * value.x + value.y * value.y +
                               value.z * value.z + value.w * value.w;
    return std::fabs(norm_squared - 1.0f) <=
               kVehicleSnapshotQuaternionNormTolerance &&
           norm_squared > 0.0f;
}

[[nodiscard]] VehicleSnapshotCodecError validate(
    const VehicleSnapshot& snapshot) noexcept
{
    if (!finite(snapshot.position) || !finite(snapshot.rotation) ||
        !finite(snapshot.linear_velocity) ||
        !finite(snapshot.angular_velocity))
        return VehicleSnapshotCodecError::NonFiniteValue;

    if (!within_abs(snapshot.position,
                    kVehicleSnapshotMaxPositionComponent) ||
        !within_abs(snapshot.linear_velocity,
                    kVehicleSnapshotMaxLinearVelocityComponent) ||
        !within_abs(snapshot.angular_velocity,
                    kVehicleSnapshotMaxAngularVelocityComponent))
        return VehicleSnapshotCodecError::ValueOutOfBounds;

    if (!valid_quaternion(snapshot.rotation))
        return VehicleSnapshotCodecError::InvalidQuaternion;

    return VehicleSnapshotCodecError::None;
}

} // namespace

VehicleSnapshotCodecError encode_vehicle_snapshot(
    const VehicleSnapshot& snapshot, MutableByteView output) noexcept
{
    if (output.size() < kVehicleSnapshotWireSize)
        return VehicleSnapshotCodecError::OutputTooSmall;

    const VehicleSnapshotCodecError validation = validate(snapshot);
    if (!vehicle_snapshot_codec_succeeded(validation))
        return validation;

    Byte* const data = output.data();
    put_u32(data + kMagicOffset, kVehicleSnapshotWireMagic);
    put_u16(data + kVersionOffset, kVehicleSnapshotWireVersion);
    put_u16(data + kFlagsOffset, kVehicleSnapshotWireFlags);
    put_u32(data + kSequenceOffset, snapshot.sequence);
    put_u32(data + kServerTickOffset, snapshot.server_tick);
    put_vector(data + kPositionOffset, snapshot.position);
    put_quaternion(data + kRotationOffset, snapshot.rotation);
    put_vector(data + kLinearVelocityOffset, snapshot.linear_velocity);
    put_vector(data + kAngularVelocityOffset, snapshot.angular_velocity);
    return VehicleSnapshotCodecError::None;
}

VehicleSnapshotCodecError decode_vehicle_snapshot(
    ByteView input, VehicleSnapshot& snapshot) noexcept
{
    if (input.size() != kVehicleSnapshotWireSize)
        return VehicleSnapshotCodecError::InputSizeMismatch;

    const Byte* const data = input.data();
    if (get_u32(data + kMagicOffset) != kVehicleSnapshotWireMagic)
        return VehicleSnapshotCodecError::BadMagic;
    if (get_u16(data + kVersionOffset) != kVehicleSnapshotWireVersion)
        return VehicleSnapshotCodecError::BadVersion;
    if (get_u16(data + kFlagsOffset) != kVehicleSnapshotWireFlags)
        return VehicleSnapshotCodecError::BadFlags;

    VehicleSnapshot decoded{};
    decoded.sequence = get_u32(data + kSequenceOffset);
    decoded.server_tick = get_u32(data + kServerTickOffset);
    decoded.position = get_vector(data + kPositionOffset);
    decoded.rotation = get_quaternion(data + kRotationOffset);
    decoded.linear_velocity = get_vector(data + kLinearVelocityOffset);
    decoded.angular_velocity = get_vector(data + kAngularVelocityOffset);

    const VehicleSnapshotCodecError validation = validate(decoded);
    if (!vehicle_snapshot_codec_succeeded(validation))
        return validation;

    snapshot = decoded;
    return VehicleSnapshotCodecError::None;
}

} // namespace kraken::net
