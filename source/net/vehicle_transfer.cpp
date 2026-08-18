#include "net/vehicle_transfer.hpp"

#include <cstring>
#include <utility>

namespace kraken::net {
namespace {

constexpr std::size_t kHeaderSize = 8;
// The common transport header is 8 bytes.  The world header carries an
// eight-byte revision plus payload-size and two reserved words, so its full
// size is 40 bytes.  Descriptor transfer has the same 8-byte transport
// header and four-byte trailing reserved word.
constexpr std::size_t kWorldTransferHeaderSize = 40;
constexpr std::size_t kDescriptorTransferHeaderSize = 40;
constexpr std::size_t kReadySize = 40;

void put_u16(Byte* const dst, const std::uint16_t value) noexcept
{
    dst[0] = static_cast<Byte>(value & 0xffu);
    dst[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* const dst, const std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < 4; ++index)
        dst[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

void put_u64(Byte* const dst, const std::uint64_t value) noexcept
{
    for (unsigned index = 0; index < 8; ++index)
        dst[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* const src) noexcept
{
    return static_cast<std::uint16_t>(std::uint8_t(src[0])) |
           static_cast<std::uint16_t>(std::uint8_t(src[1])) << 8;
}

[[nodiscard]] std::uint32_t get_u32(const Byte* const src) noexcept
{
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index)
        result |= static_cast<std::uint32_t>(std::uint8_t(src[index]))
                  << (index * 8);
    return result;
}

[[nodiscard]] std::uint64_t get_u64(const Byte* const src) noexcept
{
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index)
        result |= static_cast<std::uint64_t>(std::uint8_t(src[index]))
                  << (index * 8);
    return result;
}

void initialize_header(std::vector<Byte>& output, const std::size_t size)
{
    output.assign(size, Byte{});
    put_u32(output.data(), kVehicleTransferWireMagic);
    put_u16(output.data() + 4, kVehicleTransferWireVersion);
    put_u16(output.data() + 6, 0);
}

[[nodiscard]] VehicleTransferCodecError decode_header(
    const ByteView input) noexcept
{
    if (input.size() < kHeaderSize)
        return VehicleTransferCodecError::InputSizeMismatch;
    if (get_u32(input.data()) != kVehicleTransferWireMagic)
        return VehicleTransferCodecError::BadMagic;
    if (get_u16(input.data() + 4) != kVehicleTransferWireVersion)
        return VehicleTransferCodecError::BadVersion;
    if (get_u16(input.data() + 6) != 0)
        return VehicleTransferCodecError::BadFlags;
    return VehicleTransferCodecError::None;
}

[[nodiscard]] VehicleTransferCodecError validate_world_prefix(
    const std::uint32_t session_epoch, const std::uint32_t roster_revision)
{
    if (session_epoch == 0)
        return VehicleTransferCodecError::InvalidEpoch;
    if (roster_revision == 0)
        return VehicleTransferCodecError::InvalidRosterRevision;
    return VehicleTransferCodecError::None;
}

[[nodiscard]] VehicleTransferCodecError validate_descriptor_prefix(
    const VehicleDescriptorTransfer& value)
{
    if (const auto error = validate_world_prefix(value.session_epoch,
                                                 value.roster_revision);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    if (value.player_id == 0)
        return VehicleTransferCodecError::InvalidPlayer;
    if (value.entity_id == 0)
        return VehicleTransferCodecError::InvalidEntity;
    if (value.generation == 0)
        return VehicleTransferCodecError::InvalidGeneration;
    return VehicleTransferCodecError::None;
}

} // namespace

VehicleTransferCodecError encode_world_snapshot_transfer(
    const WorldSnapshotTransfer& value, std::vector<Byte>& output)
{
    if (const auto error = validate_world_prefix(value.session_epoch,
                                                 value.roster_revision);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    if (value.snapshot.epoch == kInvalidWorldEpoch)
        return VehicleTransferCodecError::InvalidWorldEpoch;
    if (value.snapshot.payload.size() > kMaxVehicleTransferPayload)
        return VehicleTransferCodecError::PayloadTooLarge;
    initialize_header(output, kWorldTransferHeaderSize +
                               value.snapshot.payload.size());
    Byte* const data = output.data() + kHeaderSize;
    put_u32(data, value.session_epoch);
    put_u32(data + 4, value.roster_revision);
    put_u32(data + 8, value.snapshot.epoch);
    put_u64(data + 12, value.snapshot.revision);
    put_u32(data + 20, static_cast<std::uint32_t>(value.snapshot.payload.size()));
    put_u32(data + 24, 0);
    put_u32(data + 28, 0);
    if (!value.snapshot.payload.empty())
        std::memcpy(output.data() + kWorldTransferHeaderSize,
                    value.snapshot.payload.data(), value.snapshot.payload.size());
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError decode_world_snapshot_transfer(
    const ByteView input, WorldSnapshotTransfer& output) noexcept
{
    if (input.size() < kWorldTransferHeaderSize)
        return VehicleTransferCodecError::InputSizeMismatch;
    if (const auto error = decode_header(input);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    const Byte* const data = input.data() + kHeaderSize;
    const std::uint32_t payload_size = get_u32(data + 20);
    if (get_u32(data + 24) != 0 || get_u32(data + 28) != 0)
        return VehicleTransferCodecError::BadFlags;
    if (payload_size > kMaxVehicleTransferPayload ||
        input.size() != kWorldTransferHeaderSize + payload_size)
        return payload_size > kMaxVehicleTransferPayload
            ? VehicleTransferCodecError::PayloadTooLarge
            : VehicleTransferCodecError::InputSizeMismatch;
    WorldSnapshotTransfer value{};
    value.session_epoch = get_u32(data);
    value.roster_revision = get_u32(data + 4);
    value.snapshot.epoch = get_u32(data + 8);
    value.snapshot.revision = get_u64(data + 12);
    if (const auto error = validate_world_prefix(value.session_epoch,
                                                 value.roster_revision);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    if (value.snapshot.epoch == kInvalidWorldEpoch)
        return VehicleTransferCodecError::InvalidWorldEpoch;
    value.snapshot.payload.assign(input.begin() + kWorldTransferHeaderSize,
                                  input.end());
    output = std::move(value);
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError encode_world_delta_transfer(
    const WorldDeltaTransfer& value, std::vector<Byte>& output)
{
    if (const auto error = validate_world_prefix(value.session_epoch,
                                                 value.roster_revision);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    if (value.delta.epoch == kInvalidWorldEpoch)
        return VehicleTransferCodecError::InvalidWorldEpoch;
    if (value.delta.revision == kInvalidWorldRevision)
        return VehicleTransferCodecError::InvalidRevision;
    if (value.delta.payload.size() > kMaxVehicleTransferPayload)
        return VehicleTransferCodecError::PayloadTooLarge;
    initialize_header(output, kWorldTransferHeaderSize + value.delta.payload.size());
    Byte* const data = output.data() + kHeaderSize;
    put_u32(data, value.session_epoch);
    put_u32(data + 4, value.roster_revision);
    put_u32(data + 8, value.delta.epoch);
    put_u64(data + 12, value.delta.revision);
    put_u32(data + 20, static_cast<std::uint32_t>(value.delta.payload.size()));
    put_u32(data + 24, 0);
    put_u32(data + 28, 0);
    if (!value.delta.payload.empty())
        std::memcpy(output.data() + kWorldTransferHeaderSize,
                    value.delta.payload.data(), value.delta.payload.size());
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError decode_world_delta_transfer(
    const ByteView input, WorldDeltaTransfer& output) noexcept
{
    if (input.size() < kWorldTransferHeaderSize)
        return VehicleTransferCodecError::InputSizeMismatch;
    if (const auto error = decode_header(input);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    const Byte* const data = input.data() + kHeaderSize;
    const std::uint32_t payload_size = get_u32(data + 20);
    if (get_u32(data + 24) != 0 || get_u32(data + 28) != 0)
        return VehicleTransferCodecError::BadFlags;
    if (payload_size > kMaxVehicleTransferPayload ||
        input.size() != kWorldTransferHeaderSize + payload_size)
        return payload_size > kMaxVehicleTransferPayload
            ? VehicleTransferCodecError::PayloadTooLarge
            : VehicleTransferCodecError::InputSizeMismatch;
    WorldDeltaTransfer value{};
    value.session_epoch = get_u32(data);
    value.roster_revision = get_u32(data + 4);
    value.delta.epoch = get_u32(data + 8);
    value.delta.revision = get_u64(data + 12);
    if (const auto error = validate_world_prefix(value.session_epoch,
                                                 value.roster_revision);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    if (value.delta.epoch == kInvalidWorldEpoch)
        return VehicleTransferCodecError::InvalidWorldEpoch;
    if (value.delta.revision == kInvalidWorldRevision)
        return VehicleTransferCodecError::InvalidRevision;
    value.delta.payload.assign(input.begin() + kWorldTransferHeaderSize,
                               input.end());
    output = std::move(value);
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError encode_vehicle_descriptor_transfer(
    const VehicleDescriptorTransfer& value, std::vector<Byte>& output)
{
    if (const auto error = validate_descriptor_prefix(value);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    std::vector<Byte> descriptor;
    if (!vehicle_descriptor_codec_succeeded(
            encode_vehicle_descriptor(value.descriptor, descriptor)))
        return VehicleTransferCodecError::DescriptorInvalid;
    if (descriptor.size() > kMaxVehicleTransferPayload)
        return VehicleTransferCodecError::PayloadTooLarge;
    initialize_header(output, kDescriptorTransferHeaderSize + descriptor.size());
    Byte* const data = output.data() + kHeaderSize;
    put_u32(data, value.session_epoch);
    put_u32(data + 4, value.roster_revision);
    put_u32(data + 8, value.player_id);
    put_u32(data + 12, value.entity_id);
    put_u16(data + 16, value.generation);
    put_u16(data + 18, 0);
    put_u32(data + 20, static_cast<std::uint32_t>(descriptor.size()));
    put_u32(data + 24, 0);
    put_u32(data + 28, 0);
    if (!descriptor.empty())
        std::memcpy(output.data() + kDescriptorTransferHeaderSize,
                    descriptor.data(), descriptor.size());
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError decode_vehicle_descriptor_transfer(
    const ByteView input, VehicleDescriptorTransfer& output) noexcept
{
    if (input.size() < kDescriptorTransferHeaderSize)
        return VehicleTransferCodecError::InputSizeMismatch;
    if (const auto error = decode_header(input);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    const Byte* const data = input.data() + kHeaderSize;
    const std::uint32_t descriptor_size = get_u32(data + 20);
    if (get_u16(data + 18) != 0 || get_u32(data + 24) != 0 ||
        get_u32(data + 28) != 0)
        return VehicleTransferCodecError::BadFlags;
    if (descriptor_size > kMaxVehicleTransferPayload ||
        input.size() != kDescriptorTransferHeaderSize + descriptor_size)
        return descriptor_size > kMaxVehicleTransferPayload
            ? VehicleTransferCodecError::PayloadTooLarge
            : VehicleTransferCodecError::InputSizeMismatch;
    VehicleDescriptorTransfer value{};
    value.session_epoch = get_u32(data);
    value.roster_revision = get_u32(data + 4);
    value.player_id = get_u32(data + 8);
    value.entity_id = get_u32(data + 12);
    value.generation = get_u16(data + 16);
    if (const auto error = validate_descriptor_prefix(value);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    const VehicleDescriptorCodecError descriptor_error =
        decode_vehicle_descriptor(
            input.subspan(kDescriptorTransferHeaderSize, descriptor_size),
            value.descriptor);
    if (!vehicle_descriptor_codec_succeeded(descriptor_error))
        return descriptor_error == VehicleDescriptorCodecError::BadVersion
            ? VehicleTransferCodecError::BadVersion
            : VehicleTransferCodecError::DescriptorInvalid;
    output = std::move(value);
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError encode_world_transfer_ready(
    const WorldTransferReady& value, std::vector<Byte>& output)
{
    if (value.session_epoch == 0)
        return VehicleTransferCodecError::InvalidEpoch;
    if (value.roster_revision == 0)
        return VehicleTransferCodecError::InvalidRosterRevision;
    if (value.player_id == 0)
        return VehicleTransferCodecError::InvalidPlayer;
    if (value.world_epoch == kInvalidWorldEpoch)
        return VehicleTransferCodecError::InvalidWorldEpoch;
    if (value.descriptor_count == 0)
        return VehicleTransferCodecError::TooManyDescriptors;
    initialize_header(output, kReadySize);
    Byte* const data = output.data() + kHeaderSize;
    put_u32(data, value.session_epoch);
    put_u32(data + 4, value.roster_revision);
    put_u32(data + 8, value.player_id);
    put_u32(data + 12, value.world_epoch);
    put_u64(data + 16, value.world_revision);
    put_u16(data + 24, value.descriptor_count);
    put_u16(data + 26, 0);
    put_u32(data + 28, 0);
    return VehicleTransferCodecError::None;
}

VehicleTransferCodecError decode_world_transfer_ready(
    const ByteView input, WorldTransferReady& output) noexcept
{
    if (input.size() != kReadySize)
        return VehicleTransferCodecError::InputSizeMismatch;
    if (const auto error = decode_header(input);
        !vehicle_transfer_codec_succeeded(error))
        return error;
    const Byte* const data = input.data() + kHeaderSize;
    if (get_u16(data + 26) != 0 || get_u32(data + 28) != 0)
        return VehicleTransferCodecError::BadFlags;
    WorldTransferReady value{};
    value.session_epoch = get_u32(data);
    value.roster_revision = get_u32(data + 4);
    value.player_id = get_u32(data + 8);
    value.world_epoch = get_u32(data + 12);
    value.world_revision = get_u64(data + 16);
    value.descriptor_count = get_u16(data + 24);
    if (value.session_epoch == 0)
        return VehicleTransferCodecError::InvalidEpoch;
    if (value.roster_revision == 0)
        return VehicleTransferCodecError::InvalidRosterRevision;
    if (value.player_id == 0)
        return VehicleTransferCodecError::InvalidPlayer;
    if (value.world_epoch == kInvalidWorldEpoch)
        return VehicleTransferCodecError::InvalidWorldEpoch;
    if (value.descriptor_count == 0)
        return VehicleTransferCodecError::TooManyDescriptors;
    output = value;
    return VehicleTransferCodecError::None;
}

} // namespace kraken::net
