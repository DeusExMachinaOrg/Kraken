#include "net/world_loot.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace kraken::net {
namespace {

constexpr std::uint32_t kSerialHalfRange = 0x80000000u;

void put_u16(Byte* data, std::uint16_t value) noexcept
{
    data[0] = static_cast<Byte>(value & 0xffu);
    data[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* data, std::uint32_t value) noexcept
{
    for (unsigned index = 0; index != 4; ++index)
        data[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* data) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(data[0])) |
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(data[1]) << 8);
}

[[nodiscard]] std::uint32_t get_u32(const Byte* data) noexcept
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index)
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(data[index])) << (index * 8);
    return value;
}

void put_i32(Byte* data, std::int32_t value) noexcept
{
    put_u32(data, static_cast<std::uint32_t>(value));
}

[[nodiscard]] std::int32_t get_i32(const Byte* data) noexcept
{
    return static_cast<std::int32_t>(get_u32(data));
}

void put_f32(Byte* data, float value) noexcept
{
    put_u32(data, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] float get_f32(const Byte* data) noexcept
{
    return std::bit_cast<float>(get_u32(data));
}

void put_version_and_flags(Byte* data) noexcept
{
    put_u16(data + 4, kWorldLootWireVersion);
    put_u16(data + 6, 0);
}

[[nodiscard]] WorldLootCodecError check_prefix(
    ByteView bytes, std::size_t expected_size, std::uint32_t magic) noexcept
{
    if (bytes.size() != expected_size)
        return WorldLootCodecError::InputSizeMismatch;
    const Byte* data = bytes.data();
    if (get_u32(data) != magic)
        return WorldLootCodecError::BadMagic;
    if (get_u16(data + 4) != kWorldLootWireVersion)
        return WorldLootCodecError::BadVersion;
    if (get_u16(data + 6) != 0)
        return WorldLootCodecError::BadFlags;
    return WorldLootCodecError::None;
}

[[nodiscard]] bool finite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] WorldLootCodecError validate_transform(
    const WorldLootTransform& transform) noexcept
{
    const float values[] = {
        transform.position_x, transform.position_y, transform.position_z,
        transform.rotation_x, transform.rotation_y, transform.rotation_z,
        transform.rotation_w};
    for (const float value : values)
        if (!finite(value))
            return WorldLootCodecError::InvalidTransform;

    const float norm = transform.rotation_x * transform.rotation_x +
                       transform.rotation_y * transform.rotation_y +
                       transform.rotation_z * transform.rotation_z +
                       transform.rotation_w * transform.rotation_w;
    if (!finite(norm) || std::abs(norm - 1.0f) > 0.02f)
        return WorldLootCodecError::InvalidTransform;
    return WorldLootCodecError::None;
}

[[nodiscard]] WorldLootCodecError validate_record(
    const WorldLootRecord& record, bool require_amount) noexcept
{
    if (record.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (record.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (record.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (record.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    if (record.container_id == 0)
        return WorldLootCodecError::InvalidContainer;
    if (record.container_prototype_id < 0 || record.item_prototype_id < 0)
        return WorldLootCodecError::InvalidPrototype;
    if (record.item_instance_id < -1)
        return WorldLootCodecError::InvalidPrototype;
    if (require_amount && record.amount == 0)
        return WorldLootCodecError::InvalidAmount;
    return validate_transform(record.transform);
}

void encode_record(Byte* data, const WorldLootRecord& record) noexcept
{
    put_u32(data, kWorldLootSpawnMagic);
    put_version_and_flags(data);
    put_u32(data + 8, record.session_epoch);
    put_u32(data + 12, record.loot_id);
    put_u16(data + 16, record.generation);
    put_u16(data + 18, 0);
    put_u32(data + 20, record.revision);
    put_u32(data + 24, record.container_id);
    put_i32(data + 28, record.container_prototype_id);
    put_u32(data + 32, record.owner_entity_id);
    put_f32(data + 36, record.transform.position_x);
    put_f32(data + 40, record.transform.position_y);
    put_f32(data + 44, record.transform.position_z);
    put_f32(data + 48, record.transform.rotation_x);
    put_f32(data + 52, record.transform.rotation_y);
    put_f32(data + 56, record.transform.rotation_z);
    put_f32(data + 60, record.transform.rotation_w);
    put_i32(data + 64, record.item_prototype_id);
    put_i32(data + 68, record.item_instance_id);
    put_u32(data + 72, record.amount);
}

[[nodiscard]] WorldLootCodecError decode_record(
    const Byte* data, WorldLootRecord& record, bool require_amount) noexcept
{
    if (get_u32(data) != kWorldLootSpawnMagic)
        return WorldLootCodecError::BadMagic;
    if (get_u16(data + 4) != kWorldLootWireVersion)
        return WorldLootCodecError::BadVersion;
    if (get_u16(data + 6) != 0 || get_u16(data + 18) != 0)
        return WorldLootCodecError::BadFlags;

    WorldLootRecord decoded{};
    decoded.session_epoch = get_u32(data + 8);
    decoded.loot_id = get_u32(data + 12);
    decoded.generation = get_u16(data + 16);
    decoded.revision = get_u32(data + 20);
    decoded.container_id = get_u32(data + 24);
    decoded.container_prototype_id = get_i32(data + 28);
    decoded.owner_entity_id = get_u32(data + 32);
    decoded.transform.position_x = get_f32(data + 36);
    decoded.transform.position_y = get_f32(data + 40);
    decoded.transform.position_z = get_f32(data + 44);
    decoded.transform.rotation_x = get_f32(data + 48);
    decoded.transform.rotation_y = get_f32(data + 52);
    decoded.transform.rotation_z = get_f32(data + 56);
    decoded.transform.rotation_w = get_f32(data + 60);
    decoded.item_prototype_id = get_i32(data + 64);
    decoded.item_instance_id = get_i32(data + 68);
    decoded.amount = get_u32(data + 72);
    const WorldLootCodecError error = validate_record(decoded, require_amount);
    if (world_loot_codec_succeeded(error))
        record = decoded;
    return error;
}

[[nodiscard]] bool valid_pickup_code(WorldLootPickupCode code) noexcept
{
    return static_cast<std::uint8_t>(code) <=
           static_cast<std::uint8_t>(WorldLootPickupCode::StaleGeneration);
}

[[nodiscard]] bool same_record(const WorldLootRecord& left,
                               const WorldLootRecord& right) noexcept
{
    return left.session_epoch == right.session_epoch &&
           left.loot_id == right.loot_id &&
           left.generation == right.generation &&
           left.revision == right.revision &&
           left.container_id == right.container_id &&
           left.container_prototype_id == right.container_prototype_id &&
           left.owner_entity_id == right.owner_entity_id &&
           left.item_prototype_id == right.item_prototype_id &&
           left.item_instance_id == right.item_instance_id &&
           left.amount == right.amount &&
           left.transform.position_x == right.transform.position_x &&
           left.transform.position_y == right.transform.position_y &&
           left.transform.position_z == right.transform.position_z &&
           left.transform.rotation_x == right.transform.rotation_x &&
           left.transform.rotation_y == right.transform.rotation_y &&
           left.transform.rotation_z == right.transform.rotation_z &&
           left.transform.rotation_w == right.transform.rotation_w;
}

} // namespace

WorldLootCodecError encode_world_loot_spawn(
    const WorldLootSpawn& message, MutableByteView output) noexcept
{
    if (output.size() < kWorldLootSpawnWireSize)
        return WorldLootCodecError::OutputTooSmall;
    const WorldLootCodecError error = validate_record(message.record, true);
    if (!world_loot_codec_succeeded(error))
        return error;
    encode_record(output.data(), message.record);
    return WorldLootCodecError::None;
}

WorldLootCodecError decode_world_loot_spawn(
    ByteView input, WorldLootSpawn& message) noexcept
{
    const WorldLootCodecError prefix = check_prefix(
        input, kWorldLootSpawnWireSize, kWorldLootSpawnMagic);
    if (!world_loot_codec_succeeded(prefix))
        return prefix;
    WorldLootRecord record{};
    const WorldLootCodecError error = decode_record(input.data(), record, true);
    if (world_loot_codec_succeeded(error))
        message.record = record;
    return error;
}

WorldLootCodecError encode_world_loot_baseline(
    const WorldLootBaseline& message, std::vector<Byte>& output)
{
    if (message.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (message.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    if (message.records.size() > kMaxWorldLootBaselineRecords)
        return WorldLootCodecError::TooManyRecords;
    for (std::size_t index = 0; index != message.records.size(); ++index) {
        const WorldLootRecord& record = message.records[index];
        if (record.session_epoch != message.session_epoch)
            return WorldLootCodecError::InvalidSessionEpoch;
        const WorldLootCodecError error = validate_record(record, true);
        if (!world_loot_codec_succeeded(error))
            return error;
        for (std::size_t previous = 0; previous != index; ++previous)
            if (message.records[previous].loot_id == record.loot_id)
                return WorldLootCodecError::DuplicateRecord;
    }

    output.assign(kWorldLootBaselineHeaderWireSize +
                      message.records.size() * kWorldLootRecordWireSize,
                  Byte{});
    Byte* data = output.data();
    put_u32(data, kWorldLootBaselineMagic);
    put_version_and_flags(data);
    put_u32(data + 8, message.session_epoch);
    put_u32(data + 12, message.revision);
    put_u16(data + 16, static_cast<std::uint16_t>(message.records.size()));
    put_u16(data + 18, 0);
    for (std::size_t index = 0; index != message.records.size(); ++index)
        encode_record(data + kWorldLootBaselineHeaderWireSize +
                          index * kWorldLootRecordWireSize,
                      message.records[index]);
    return WorldLootCodecError::None;
}

WorldLootCodecError decode_world_loot_baseline(
    ByteView input, WorldLootBaseline& message) noexcept
{
    if (input.size() < kWorldLootBaselineHeaderWireSize)
        return WorldLootCodecError::InputSizeMismatch;
    const WorldLootCodecError prefix = check_prefix(
        input.subspan(0, kWorldLootBaselineHeaderWireSize),
        kWorldLootBaselineHeaderWireSize, kWorldLootBaselineMagic);
    if (!world_loot_codec_succeeded(prefix))
        return prefix;
    const Byte* data = input.data();
    const std::uint16_t count = get_u16(data + 16);
    if (get_u16(data + 18) != 0)
        return WorldLootCodecError::BadFlags;
    if (count > kMaxWorldLootBaselineRecords)
        return WorldLootCodecError::TooManyRecords;
    const std::size_t expected_size = kWorldLootBaselineHeaderWireSize +
                                      static_cast<std::size_t>(count) *
                                          kWorldLootRecordWireSize;
    if (input.size() != expected_size)
        return WorldLootCodecError::InputSizeMismatch;

    WorldLootBaseline decoded{};
    decoded.session_epoch = get_u32(data + 8);
    decoded.revision = get_u32(data + 12);
    if (decoded.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (decoded.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    decoded.records.reserve(count);
    for (std::uint16_t index = 0; index != count; ++index) {
        WorldLootRecord record{};
        const WorldLootCodecError error = decode_record(
            data + kWorldLootBaselineHeaderWireSize +
                static_cast<std::size_t>(index) * kWorldLootRecordWireSize,
            record, true);
        if (!world_loot_codec_succeeded(error))
            return error;
        if (record.session_epoch != decoded.session_epoch)
            return WorldLootCodecError::InvalidSessionEpoch;
        for (const WorldLootRecord& previous : decoded.records)
            if (previous.loot_id == record.loot_id)
                return WorldLootCodecError::DuplicateRecord;
        decoded.records.push_back(record);
    }
    message = std::move(decoded);
    return WorldLootCodecError::None;
}

WorldLootCodecError encode_world_loot_delta(
    const WorldLootDelta& message, MutableByteView output) noexcept
{
    if (output.size() < kWorldLootDeltaWireSize)
        return WorldLootCodecError::OutputTooSmall;
    if (message.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (message.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (message.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (message.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    Byte* data = output.data();
    put_u32(data, kWorldLootDeltaMagic);
    put_version_and_flags(data);
    put_u32(data + 8, message.session_epoch);
    put_u32(data + 12, message.loot_id);
    put_u16(data + 16, message.generation);
    put_u16(data + 18, 0);
    put_u32(data + 20, message.revision);
    put_u32(data + 24, message.amount);
    return WorldLootCodecError::None;
}

WorldLootCodecError decode_world_loot_delta(
    ByteView input, WorldLootDelta& message) noexcept
{
    const WorldLootCodecError prefix = check_prefix(
        input, kWorldLootDeltaWireSize, kWorldLootDeltaMagic);
    if (!world_loot_codec_succeeded(prefix))
        return prefix;
    const Byte* data = input.data();
    if (get_u16(data + 18) != 0)
        return WorldLootCodecError::BadFlags;
    WorldLootDelta decoded{};
    decoded.session_epoch = get_u32(data + 8);
    decoded.loot_id = get_u32(data + 12);
    decoded.generation = get_u16(data + 16);
    decoded.revision = get_u32(data + 20);
    decoded.amount = get_u32(data + 24);
    if (decoded.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (decoded.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (decoded.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (decoded.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    message = decoded;
    return WorldLootCodecError::None;
}

WorldLootCodecError encode_world_loot_remove(
    const WorldLootRemove& message, MutableByteView output) noexcept
{
    if (output.size() < kWorldLootRemoveWireSize)
        return WorldLootCodecError::OutputTooSmall;
    if (message.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (message.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (message.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (message.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    if (message.reason > 3)
        return WorldLootCodecError::InvalidReason;
    Byte* data = output.data();
    put_u32(data, kWorldLootRemoveMagic);
    put_version_and_flags(data);
    put_u32(data + 8, message.session_epoch);
    put_u32(data + 12, message.loot_id);
    put_u16(data + 16, message.generation);
    put_u16(data + 18, 0);
    put_u32(data + 20, message.revision);
    data[24] = static_cast<Byte>(message.reason);
    data[25] = Byte{};
    data[26] = Byte{};
    data[27] = Byte{};
    return WorldLootCodecError::None;
}

WorldLootCodecError decode_world_loot_remove(
    ByteView input, WorldLootRemove& message) noexcept
{
    const WorldLootCodecError prefix = check_prefix(
        input, kWorldLootRemoveWireSize, kWorldLootRemoveMagic);
    if (!world_loot_codec_succeeded(prefix))
        return prefix;
    const Byte* data = input.data();
    if (get_u16(data + 18) != 0 || data[25] != Byte{} ||
        data[26] != Byte{} || data[27] != Byte{})
        return WorldLootCodecError::BadFlags;
    WorldLootRemove decoded{};
    decoded.session_epoch = get_u32(data + 8);
    decoded.loot_id = get_u32(data + 12);
    decoded.generation = get_u16(data + 16);
    decoded.revision = get_u32(data + 20);
    decoded.reason = std::to_integer<std::uint8_t>(data[24]);
    if (decoded.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (decoded.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (decoded.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (decoded.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    if (decoded.reason > 3)
        return WorldLootCodecError::InvalidReason;
    message = decoded;
    return WorldLootCodecError::None;
}

WorldLootCodecError encode_world_loot_pickup_request(
    const WorldLootPickupRequest& message, MutableByteView output) noexcept
{
    if (output.size() < kWorldLootPickupRequestWireSize)
        return WorldLootCodecError::OutputTooSmall;
    if (message.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (message.entity_id == 0)
        return WorldLootCodecError::InvalidEntity;
    if (message.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (message.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (message.transaction_id == 0)
        return WorldLootCodecError::InvalidTransactionId;
    if (message.amount == 0)
        return WorldLootCodecError::InvalidAmount;
    Byte* data = output.data();
    put_u32(data, kWorldLootPickupRequestMagic);
    put_version_and_flags(data);
    put_u32(data + 8, message.session_epoch);
    put_u32(data + 12, message.entity_id);
    put_u32(data + 16, message.loot_id);
    put_u16(data + 20, message.generation);
    put_u16(data + 22, 0);
    put_u32(data + 24, message.transaction_id);
    put_u32(data + 28, message.amount);
    return WorldLootCodecError::None;
}

WorldLootCodecError decode_world_loot_pickup_request(
    ByteView input, WorldLootPickupRequest& message) noexcept
{
    const WorldLootCodecError prefix = check_prefix(
        input, kWorldLootPickupRequestWireSize, kWorldLootPickupRequestMagic);
    if (!world_loot_codec_succeeded(prefix))
        return prefix;
    const Byte* data = input.data();
    if (get_u16(data + 22) != 0)
        return WorldLootCodecError::BadFlags;
    WorldLootPickupRequest decoded{};
    decoded.session_epoch = get_u32(data + 8);
    decoded.entity_id = get_u32(data + 12);
    decoded.loot_id = get_u32(data + 16);
    decoded.generation = get_u16(data + 20);
    decoded.transaction_id = get_u32(data + 24);
    decoded.amount = get_u32(data + 28);
    if (decoded.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (decoded.entity_id == 0)
        return WorldLootCodecError::InvalidEntity;
    if (decoded.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (decoded.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (decoded.transaction_id == 0)
        return WorldLootCodecError::InvalidTransactionId;
    if (decoded.amount == 0)
        return WorldLootCodecError::InvalidAmount;
    message = decoded;
    return WorldLootCodecError::None;
}

WorldLootCodecError encode_world_loot_pickup_result(
    const WorldLootPickupResult& message, MutableByteView output) noexcept
{
    if (output.size() < kWorldLootPickupResultWireSize)
        return WorldLootCodecError::OutputTooSmall;
    if (message.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (message.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (message.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (message.transaction_id == 0)
        return WorldLootCodecError::InvalidTransactionId;
    if (message.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    if (!valid_pickup_code(message.code))
        return WorldLootCodecError::InvalidPickupCode;
    if (message.item_prototype_id < -1 || message.item_instance_id < -1)
        return WorldLootCodecError::InvalidPrototype;
    Byte* data = output.data();
    put_u32(data, kWorldLootPickupResultMagic);
    put_version_and_flags(data);
    put_u32(data + 8, message.session_epoch);
    put_u32(data + 12, message.loot_id);
    put_u16(data + 16, message.generation);
    put_u16(data + 18, 0);
    put_u32(data + 20, message.transaction_id);
    data[24] = static_cast<Byte>(message.code);
    data[25] = Byte{};
    data[26] = Byte{};
    data[27] = Byte{};
    put_i32(data + 28, message.item_prototype_id);
    put_i32(data + 32, message.item_instance_id);
    put_u32(data + 36, message.granted_amount);
    put_u32(data + 40, message.remaining_amount);
    put_u32(data + 44, message.revision);
    return WorldLootCodecError::None;
}

WorldLootCodecError decode_world_loot_pickup_result(
    ByteView input, WorldLootPickupResult& message) noexcept
{
    const WorldLootCodecError prefix = check_prefix(
        input, kWorldLootPickupResultWireSize, kWorldLootPickupResultMagic);
    if (!world_loot_codec_succeeded(prefix))
        return prefix;
    const Byte* data = input.data();
    if (get_u16(data + 18) != 0 || data[25] != Byte{} ||
        data[26] != Byte{} || data[27] != Byte{})
        return WorldLootCodecError::BadFlags;
    WorldLootPickupResult decoded{};
    decoded.session_epoch = get_u32(data + 8);
    decoded.loot_id = get_u32(data + 12);
    decoded.generation = get_u16(data + 16);
    decoded.transaction_id = get_u32(data + 20);
    decoded.code = static_cast<WorldLootPickupCode>(
        std::to_integer<std::uint8_t>(data[24]));
    decoded.item_prototype_id = get_i32(data + 28);
    decoded.item_instance_id = get_i32(data + 32);
    decoded.granted_amount = get_u32(data + 36);
    decoded.remaining_amount = get_u32(data + 40);
    decoded.revision = get_u32(data + 44);
    if (decoded.session_epoch == 0)
        return WorldLootCodecError::InvalidSessionEpoch;
    if (decoded.loot_id == 0)
        return WorldLootCodecError::InvalidLootId;
    if (decoded.generation == 0)
        return WorldLootCodecError::InvalidGeneration;
    if (decoded.transaction_id == 0)
        return WorldLootCodecError::InvalidTransactionId;
    if (decoded.revision == 0)
        return WorldLootCodecError::InvalidRevision;
    if (!valid_pickup_code(decoded.code))
        return WorldLootCodecError::InvalidPickupCode;
    if (decoded.item_prototype_id < -1 || decoded.item_instance_id < -1)
        return WorldLootCodecError::InvalidPrototype;
    message = decoded;
    return WorldLootCodecError::None;
}

bool WorldLootReplica::serial_newer(const std::uint32_t candidate,
                                    const std::uint32_t current) noexcept
{
    return candidate != current &&
           static_cast<std::uint32_t>(candidate - current) < kSerialHalfRange;
}

std::size_t WorldLootReplica::find_index(const WorldLootId loot_id) const noexcept
{
    const auto found = std::find_if(
        m_records.begin(), m_records.end(),
        [loot_id](const WorldLootRecord& record) {
            return record.loot_id == loot_id;
        });
    return static_cast<std::size_t>(found - m_records.begin());
}

std::size_t WorldLootReplica::find_tombstone(
    const WorldLootId loot_id) const noexcept
{
    const auto found = std::find_if(
        m_tombstones.begin(), m_tombstones.end(),
        [loot_id](const Tombstone& tombstone) {
            return tombstone.loot_id == loot_id;
        });
    return static_cast<std::size_t>(found - m_tombstones.begin());
}

WorldLootApplyResult WorldLootReplica::accept_epoch(
    const WorldLootSessionEpoch epoch) noexcept
{
    if (epoch == 0)
        return WorldLootApplyResult::Invalid;
    if (m_session_epoch == 0) {
        m_session_epoch = epoch;
        return WorldLootApplyResult::Applied;
    }
    if (m_session_epoch == epoch)
        return WorldLootApplyResult::Applied;
    if (!serial_newer(epoch, m_session_epoch))
        return WorldLootApplyResult::WrongSessionEpoch;
    m_session_epoch = epoch;
    m_revision = 0;
    m_records.clear();
    m_tombstones.clear();
    return WorldLootApplyResult::Applied;
}

WorldLootApplyResult WorldLootReplica::apply_spawn(
    const WorldLootSpawn& message)
{
    if (!world_loot_codec_succeeded(validate_record(message.record, true)))
        return WorldLootApplyResult::Invalid;
    const WorldLootApplyResult epoch = accept_epoch(message.record.session_epoch);
    if (epoch != WorldLootApplyResult::Applied)
        return epoch;

    const std::size_t tombstone_index = find_tombstone(message.record.loot_id);
    if (tombstone_index != m_tombstones.size()) {
        const Tombstone& tombstone = m_tombstones[tombstone_index];
        if (message.record.generation < tombstone.generation ||
            (message.record.generation == tombstone.generation &&
             !serial_newer(message.record.revision, tombstone.revision)))
            return message.record.generation == tombstone.generation &&
                           message.record.revision == tombstone.revision
                       ? WorldLootApplyResult::Duplicate
                       : WorldLootApplyResult::Stale;
    }

    const std::size_t index = find_index(message.record.loot_id);
    if (index == m_records.size()) {
        // Baselines and removals advance the global revision even when this
        // ID is absent.  A delayed spawn at or behind that frontier must not
        // recreate loot omitted by a baseline or already removed.
        if (m_revision != 0 &&
            !serial_newer(message.record.revision, m_revision))
            return WorldLootApplyResult::Stale;
        if (tombstone_index != m_tombstones.size())
            m_tombstones.erase(m_tombstones.begin() +
                               static_cast<std::ptrdiff_t>(tombstone_index));
        m_records.push_back(message.record);
        if (m_revision == 0 || serial_newer(message.record.revision, m_revision))
            m_revision = message.record.revision;
        return WorldLootApplyResult::Applied;
    }

    WorldLootRecord& current = m_records[index];
    if (message.record.generation < current.generation)
        return WorldLootApplyResult::Stale;
    if (message.record.generation == current.generation) {
        if (message.record.revision == current.revision)
            return same_record(current, message.record)
                       ? WorldLootApplyResult::Duplicate
                       : WorldLootApplyResult::Stale;
        if (!serial_newer(message.record.revision, current.revision))
            return WorldLootApplyResult::Stale;
    }
    current = message.record;
    if (m_revision == 0 || serial_newer(message.record.revision, m_revision))
        m_revision = message.record.revision;
    return WorldLootApplyResult::Applied;
}

WorldLootApplyResult WorldLootReplica::apply_baseline(
    const WorldLootBaseline& message)
{
    if (message.session_epoch == 0 || message.revision == 0 ||
        message.records.size() > kMaxWorldLootBaselineRecords)
        return WorldLootApplyResult::Invalid;
    for (std::size_t index = 0; index != message.records.size(); ++index) {
        const WorldLootRecord& record = message.records[index];
        if (record.session_epoch != message.session_epoch ||
            !world_loot_codec_succeeded(validate_record(record, true)))
            return WorldLootApplyResult::Invalid;
        for (std::size_t previous = 0; previous != index; ++previous)
            if (message.records[previous].loot_id == record.loot_id)
                return WorldLootApplyResult::Invalid;
    }
    const bool new_epoch = m_session_epoch == 0 ||
                           m_session_epoch != message.session_epoch;
    const WorldLootApplyResult epoch = accept_epoch(message.session_epoch);
    if (epoch != WorldLootApplyResult::Applied)
        return epoch;
    if (!new_epoch) {
        if (message.revision == m_revision)
            return WorldLootApplyResult::Duplicate;
        if (m_revision != 0 && !serial_newer(message.revision, m_revision))
            return WorldLootApplyResult::Stale;
    }
    m_records = message.records;
    m_tombstones.clear();
    m_revision = message.revision;
    return WorldLootApplyResult::Applied;
}

WorldLootApplyResult WorldLootReplica::apply_delta(
    const WorldLootDelta& message)
{
    if (message.session_epoch == 0 || message.loot_id == 0 ||
        message.generation == 0 || message.revision == 0)
        return WorldLootApplyResult::Invalid;
    const WorldLootApplyResult epoch = accept_epoch(message.session_epoch);
    if (epoch != WorldLootApplyResult::Applied)
        return epoch;
    const std::size_t index = find_index(message.loot_id);
    if (index == m_records.size())
        return WorldLootApplyResult::Stale;
    WorldLootRecord& current = m_records[index];
    if (message.generation != current.generation)
        return message.generation < current.generation
                   ? WorldLootApplyResult::Stale
                   : WorldLootApplyResult::Stale;
    if (message.revision == current.revision)
        return message.amount == current.amount
                   ? WorldLootApplyResult::Duplicate
                   : WorldLootApplyResult::Stale;
    if (!serial_newer(message.revision, current.revision))
        return WorldLootApplyResult::Stale;
    current.amount = message.amount;
    current.revision = message.revision;
    if (m_revision == 0 || serial_newer(message.revision, m_revision))
        m_revision = message.revision;
    return WorldLootApplyResult::Applied;
}

void WorldLootReplica::remember_tombstone(const WorldLootRemove& message)
{
    const std::size_t index = find_tombstone(message.loot_id);
    if (index == m_tombstones.size()) {
        m_tombstones.push_back(
            Tombstone{message.loot_id, message.generation, message.revision});
        return;
    }
    Tombstone& current = m_tombstones[index];
    if (message.generation > current.generation ||
        (message.generation == current.generation &&
         serial_newer(message.revision, current.revision)))
        current = Tombstone{message.loot_id, message.generation,
                            message.revision};
}

WorldLootApplyResult WorldLootReplica::apply_remove(
    const WorldLootRemove& message)
{
    if (message.session_epoch == 0 || message.loot_id == 0 ||
        message.generation == 0 || message.revision == 0 || message.reason > 3)
        return WorldLootApplyResult::Invalid;
    const WorldLootApplyResult epoch = accept_epoch(message.session_epoch);
    if (epoch != WorldLootApplyResult::Applied)
        return epoch;
    const std::size_t index = find_index(message.loot_id);
    if (index != m_records.size()) {
        const WorldLootRecord& current = m_records[index];
        if (message.generation < current.generation ||
            (message.generation == current.generation &&
             !serial_newer(message.revision, current.revision)))
            return message.generation == current.generation &&
                           message.revision == current.revision
                       ? WorldLootApplyResult::Duplicate
                       : WorldLootApplyResult::Stale;
        m_records.erase(m_records.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
        const std::size_t tombstone_index = find_tombstone(message.loot_id);
        if (tombstone_index != m_tombstones.size()) {
            const Tombstone& current = m_tombstones[tombstone_index];
            if (message.generation < current.generation ||
                (message.generation == current.generation &&
                 !serial_newer(message.revision, current.revision)))
                return message.generation == current.generation &&
                               message.revision == current.revision
                           ? WorldLootApplyResult::Duplicate
                           : WorldLootApplyResult::Stale;
        }
    }
    remember_tombstone(message);
    if (m_revision == 0 || serial_newer(message.revision, m_revision))
        m_revision = message.revision;
    return WorldLootApplyResult::Applied;
}

const WorldLootRecord* WorldLootReplica::find(const WorldLootId loot_id) const noexcept
{
    const std::size_t index = find_index(loot_id);
    return index == m_records.size() ? nullptr : &m_records[index];
}

void WorldLootReplica::clear() noexcept
{
    m_session_epoch = 0;
    m_revision = 0;
    m_records.clear();
    m_tombstones.clear();
}

} // namespace kraken::net
