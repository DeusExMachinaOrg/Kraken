#include "net/world_state_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

namespace kraken::net {
namespace {

constexpr std::size_t kObjectWireSize = 28;
constexpr std::size_t kPropertyWireSize = 8;
constexpr std::size_t kNoIndex = (std::numeric_limits<std::size_t>::max)();

void put_u16(Byte* const destination, const std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* const destination, const std::uint32_t value) noexcept
{
    for (unsigned index = 0; index != 4; ++index)
        destination[index] =
            static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

void put_u64(Byte* const destination, const std::uint64_t value) noexcept
{
    for (unsigned index = 0; index != 8; ++index)
        destination[index] =
            static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* const source) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(source[0]) |
        (std::to_integer<std::uint16_t>(source[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const Byte* const source) noexcept
{
    std::uint32_t result = 0;
    for (unsigned index = 0; index != 4; ++index)
        result |= std::to_integer<std::uint32_t>(source[index]) << (index * 8);
    return result;
}

[[nodiscard]] std::uint64_t get_u64(const Byte* const source) noexcept
{
    std::uint64_t result = 0;
    for (unsigned index = 0; index != 8; ++index)
        result |= std::to_integer<std::uint64_t>(source[index]) << (index * 8);
    return result;
}

[[nodiscard]] bool add_bounded(
    std::size_t& total, const std::size_t amount) noexcept
{
    if (amount > kWorldStateSnapshotMaxWireBytes - total)
        return false;
    total += amount;
    return true;
}

struct ValidatedRecords {
    std::vector<std::size_t> object_order;
    std::vector<std::vector<std::size_t>> property_order;
    std::vector<std::size_t> parent_index;
    std::vector<std::size_t> topological_order;
    std::size_t property_count = 0;
    std::size_t wire_size = kWorldStateSnapshotWireHeaderSize;
};

[[nodiscard]] WorldStateSnapshotCodecError validate_records(
    const std::span<const ObjectRecord> records, ValidatedRecords& result)
{
    if (records.size() > kWorldStateSnapshotMaxObjects)
        return WorldStateSnapshotCodecError::TooManyObjects;

    result = {};
    result.object_order.resize(records.size());
    std::iota(result.object_order.begin(), result.object_order.end(), 0);
    std::sort(result.object_order.begin(), result.object_order.end(),
              [&records](const std::size_t left, const std::size_t right) {
                  return records[left].object_id < records[right].object_id;
              });

    for (std::size_t position = 0; position != result.object_order.size();
         ++position) {
        const ObjectRecord& record = records[result.object_order[position]];
        if (record.object_id == kInvalidHostObjectId)
            return WorldStateSnapshotCodecError::InvalidObjectId;
        if (record.type_id == 0)
            return WorldStateSnapshotCodecError::InvalidTypeId;
        if (position != 0 &&
            records[result.object_order[position - 1]].object_id ==
                record.object_id)
            return WorldStateSnapshotCodecError::DuplicateObjectId;
        if (record.runtime.size() > kWorldStateSnapshotMaxBlobBytes)
            return WorldStateSnapshotCodecError::BlobTooLarge;
        if (record.properties.size() >
            kWorldStateSnapshotMaxPropertiesPerObject)
            return WorldStateSnapshotCodecError::TooManyProperties;
        if (record.properties.size() >
            kWorldStateSnapshotMaxProperties - result.property_count)
            return WorldStateSnapshotCodecError::TooManyProperties;
        result.property_count += record.properties.size();
    }

    result.parent_index.assign(records.size(), kNoIndex);
    for (std::size_t index = 0; index != records.size(); ++index) {
        const ObjectRecord& record = records[index];
        if (record.parent_id == kInvalidHostObjectId)
            continue;
        if (record.parent_id == record.object_id)
            return WorldStateSnapshotCodecError::SelfParent;

        const auto parent = std::lower_bound(
            result.object_order.begin(), result.object_order.end(),
            record.parent_id,
            [&records](const std::size_t object_index,
                       const HostObjectId object_id) {
                return records[object_index].object_id < object_id;
            });
        if (parent == result.object_order.end() ||
            records[*parent].object_id != record.parent_id)
            return WorldStateSnapshotCodecError::UnknownParent;
        result.parent_index[index] = *parent;
    }

    result.property_order.resize(records.size());
    for (std::size_t index = 0; index != records.size(); ++index) {
        const ObjectRecord& record = records[index];
        std::vector<std::size_t>& properties = result.property_order[index];
        properties.resize(record.properties.size());
        std::iota(properties.begin(), properties.end(), 0);
        std::sort(properties.begin(), properties.end(),
                  [&record](const std::size_t left, const std::size_t right) {
                      return record.properties[left].property_id <
                             record.properties[right].property_id;
                  });
        for (std::size_t position = 0; position != properties.size();
             ++position) {
            const PropertySnapshot& property =
                record.properties[properties[position]];
            if (property.property_id == 0)
                return WorldStateSnapshotCodecError::InvalidPropertyId;
            if (property.value.size() > kWorldStateSnapshotMaxBlobBytes)
                return WorldStateSnapshotCodecError::BlobTooLarge;
            if (position != 0 &&
                record.properties[properties[position - 1]].property_id ==
                    property.property_id)
                return WorldStateSnapshotCodecError::DuplicatePropertyId;
        }
    }

    // A record has at most one parent.  The color walk is therefore enough to
    // reject every self/cyclic chain without recursion or unbounded memory.
    std::vector<std::uint8_t> colors(records.size(), 0);
    for (std::size_t start = 0; start != records.size(); ++start) {
        if (colors[start] != 0)
            continue;
        std::vector<std::size_t> path;
        std::size_t current = start;
        while (current != kNoIndex && colors[current] == 0) {
            colors[current] = 1;
            path.push_back(current);
            current = result.parent_index[current];
        }
        if (current != kNoIndex && colors[current] == 1)
            return WorldStateSnapshotCodecError::ParentCycle;
        for (const std::size_t index : path)
            colors[index] = 2;
    }

    // Walking each sorted object up to its already visited ancestor gives a
    // stable topological order.  Siblings are ordered by host ID.
    std::vector<std::uint8_t> visited(records.size(), 0);
    result.topological_order.reserve(records.size());
    for (const std::size_t start : result.object_order) {
        if (visited[start] != 0)
            continue;
        std::vector<std::size_t> chain;
        std::size_t current = start;
        while (current != kNoIndex && visited[current] == 0) {
            visited[current] = 1;
            chain.push_back(current);
            current = result.parent_index[current];
        }
        for (auto iterator = chain.rbegin(); iterator != chain.rend();
             ++iterator)
            result.topological_order.push_back(*iterator);
    }

    for (const std::size_t index : result.object_order) {
        const ObjectRecord& record = records[index];
        if (!add_bounded(result.wire_size, kObjectWireSize) ||
            !add_bounded(result.wire_size, record.runtime.size()))
            return WorldStateSnapshotCodecError::WireTooLarge;
        for (const std::size_t property_index : result.property_order[index]) {
            const PropertySnapshot& property = record.properties[property_index];
            if (!add_bounded(result.wire_size, kPropertyWireSize) ||
                !add_bounded(result.wire_size, property.value.size()))
                return WorldStateSnapshotCodecError::WireTooLarge;
        }
    }
    return WorldStateSnapshotCodecError::None;
}

[[nodiscard]] bool bytes_equal(
    const std::vector<Byte>& left, const std::vector<Byte>& right) noexcept
{ return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin()); }

class Sha256 final {
public:
    void update(const Byte* data, std::size_t size) noexcept
    {
        while (size != 0) {
            const std::size_t available = buffer_.size() - buffer_size_;
            const std::size_t count = std::min(available, size);
            for (std::size_t index = 0; index != count; ++index)
                buffer_[buffer_size_ + index] =
                    std::to_integer<std::uint8_t>(data[index]);
            buffer_size_ += count;
            data += count;
            size -= count;
            total_bytes_ += count;
            if (buffer_size_ == buffer_.size()) {
                compress(buffer_.data());
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] WorldStateSnapshotDigest finish() const noexcept
    {
        Sha256 copy = *this;
        const std::uint64_t bit_count = copy.total_bytes_ * 8;
        copy.update_byte(0x80);
        while (copy.buffer_size_ != 56)
            copy.update_byte(0);
        for (unsigned index = 0; index != 8; ++index)
            copy.update_byte(static_cast<std::uint8_t>(
                bit_count >> ((7u - index) * 8u)));

        WorldStateSnapshotDigest result{};
        for (std::size_t index = 0; index != copy.state_.size(); ++index) {
            result[index * 4 + 0] =
                static_cast<std::uint8_t>(copy.state_[index] >> 24);
            result[index * 4 + 1] =
                static_cast<std::uint8_t>(copy.state_[index] >> 16);
            result[index * 4 + 2] =
                static_cast<std::uint8_t>(copy.state_[index] >> 8);
            result[index * 4 + 3] =
                static_cast<std::uint8_t>(copy.state_[index]);
        }
        return result;
    }

private:
    static constexpr std::uint32_t rotate_right(
        const std::uint32_t value, const unsigned count) noexcept
    { return (value >> count) | (value << (32u - count)); }

    static constexpr std::uint32_t choose(
        const std::uint32_t x, const std::uint32_t y,
        const std::uint32_t z) noexcept
    { return (x & y) ^ (~x & z); }

    static constexpr std::uint32_t majority(
        const std::uint32_t x, const std::uint32_t y,
        const std::uint32_t z) noexcept
    { return (x & y) ^ (x & z) ^ (y & z); }

    static constexpr std::uint32_t big_sigma0(
        const std::uint32_t value) noexcept
    { return rotate_right(value, 2) ^ rotate_right(value, 13) ^
             rotate_right(value, 22); }

    static constexpr std::uint32_t big_sigma1(
        const std::uint32_t value) noexcept
    { return rotate_right(value, 6) ^ rotate_right(value, 11) ^
             rotate_right(value, 25); }

    static constexpr std::uint32_t small_sigma0(
        const std::uint32_t value) noexcept
    { return rotate_right(value, 7) ^ rotate_right(value, 18) ^ (value >> 3); }

    static constexpr std::uint32_t small_sigma1(
        const std::uint32_t value) noexcept
    { return rotate_right(value, 17) ^ rotate_right(value, 19) ^ (value >> 10); }

    void update_byte(const std::uint8_t value) noexcept
    {
        const Byte byte = static_cast<Byte>(value);
        update(&byte, 1);
    }

    void compress(const std::uint8_t* const block) noexcept
    {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index != 16; ++index)
            schedule[index] =
                (static_cast<std::uint32_t>(block[index * 4 + 0]) << 24) |
                (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                static_cast<std::uint32_t>(block[index * 4 + 3]);
        for (std::size_t index = 16; index != schedule.size(); ++index)
            schedule[index] = small_sigma1(schedule[index - 2]) +
                              schedule[index - 7] +
                              small_sigma0(schedule[index - 15]) +
                              schedule[index - 16];

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index != schedule.size(); ++index) {
            const std::uint32_t t1 = h + big_sigma1(e) + choose(e, f, g) +
                                     kRoundConstants[index] + schedule[index];
            const std::uint32_t t2 = big_sigma0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    static constexpr std::array<std::uint32_t, 64> kRoundConstants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abb, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

constexpr std::array<std::uint32_t, 64> Sha256::kRoundConstants;

[[nodiscard]] WorldStateSnapshotDigest digest_bytes(ByteView input) noexcept
{
    Sha256 hash;
    if (!input.empty())
        hash.update(input.data(), input.size());
    return hash.finish();
}

[[nodiscard]] std::string hex_digest(const WorldStateSnapshotDigest& digest)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        result.push_back(kHex[(byte >> 4) & 0x0f]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

} // namespace

WorldStateSnapshotCodecError encode_world_state_snapshot(
    const std::span<const ObjectRecord> records, std::vector<Byte>& output)
{
    ValidatedRecords validation{};
    const WorldStateSnapshotCodecError error =
        validate_records(records, validation);
    if (!world_state_snapshot_codec_succeeded(error))
        return error;

    std::vector<Byte> encoded(validation.wire_size, Byte{});
    Byte* const data = encoded.data();
    put_u32(data + 0, kWorldStateSnapshotWireMagic);
    put_u16(data + 4, kWorldStateSnapshotWireVersion);
    put_u16(data + 6, kWorldStateSnapshotWireFlags);
    put_u32(data + 8, static_cast<std::uint32_t>(records.size()));
    put_u32(data + 12, static_cast<std::uint32_t>(encoded.size()));

    std::size_t offset = kWorldStateSnapshotWireHeaderSize;
    for (const std::size_t index : validation.object_order) {
        const ObjectRecord& record = records[index];
        put_u64(data + offset, record.object_id);
        offset += 8;
        put_u32(data + offset, record.type_id);
        offset += 4;
        put_u64(data + offset, record.parent_id);
        offset += 8;
        put_u32(data + offset, static_cast<std::uint32_t>(record.runtime.size()));
        offset += 4;
        put_u32(data + offset,
                static_cast<std::uint32_t>(record.properties.size()));
        offset += 4;
        if (!record.runtime.empty()) {
            std::memcpy(data + offset, record.runtime.data(), record.runtime.size());
            offset += record.runtime.size();
        }
        for (const std::size_t property_index : validation.property_order[index]) {
            const PropertySnapshot& property = record.properties[property_index];
            put_u32(data + offset, property.property_id);
            offset += 4;
            put_u32(data + offset, static_cast<std::uint32_t>(property.value.size()));
            offset += 4;
            if (!property.value.empty()) {
                std::memcpy(data + offset, property.value.data(), property.value.size());
                offset += property.value.size();
            }
        }
    }

    output.swap(encoded);
    return WorldStateSnapshotCodecError::None;
}

WorldStateSnapshotCodecError decode_world_state_snapshot(
    const ByteView input, std::vector<ObjectRecord>& output)
{
    if (input.size() < kWorldStateSnapshotWireHeaderSize)
        return WorldStateSnapshotCodecError::InputSizeMismatch;
    if (input.size() > kWorldStateSnapshotMaxWireBytes)
        return WorldStateSnapshotCodecError::WireTooLarge;

    const Byte* const data = input.data();
    if (get_u32(data + 0) != kWorldStateSnapshotWireMagic)
        return WorldStateSnapshotCodecError::BadMagic;
    if (get_u16(data + 4) != kWorldStateSnapshotWireVersion)
        return WorldStateSnapshotCodecError::BadVersion;
    if (get_u16(data + 6) != kWorldStateSnapshotWireFlags)
        return WorldStateSnapshotCodecError::BadFlags;
    const std::uint32_t object_count = get_u32(data + 8);
    const std::uint32_t declared_size = get_u32(data + 12);
    if (declared_size > kWorldStateSnapshotMaxWireBytes)
        return WorldStateSnapshotCodecError::WireTooLarge;
    if (declared_size != input.size())
        return WorldStateSnapshotCodecError::InputSizeMismatch;
    if (object_count > kWorldStateSnapshotMaxObjects)
        return WorldStateSnapshotCodecError::TooManyObjects;

    std::vector<ObjectRecord> decoded;
    decoded.reserve(object_count);
    std::size_t offset = kWorldStateSnapshotWireHeaderSize;
    std::size_t total_properties = 0;
    for (std::uint32_t object_index = 0; object_index != object_count;
         ++object_index) {
        if (input.size() - offset < kObjectWireSize)
            return WorldStateSnapshotCodecError::InputSizeMismatch;
        const Byte* const object_data = data + offset;
        ObjectRecord record{};
        record.object_id = get_u64(object_data + 0);
        record.type_id = get_u32(object_data + 8);
        record.parent_id = get_u64(object_data + 12);
        const std::uint32_t runtime_size = get_u32(object_data + 20);
        const std::uint32_t property_count = get_u32(object_data + 24);
        offset += kObjectWireSize;

        if (runtime_size > kWorldStateSnapshotMaxBlobBytes)
            return WorldStateSnapshotCodecError::BlobTooLarge;
        if (property_count > kWorldStateSnapshotMaxPropertiesPerObject)
            return WorldStateSnapshotCodecError::TooManyProperties;
        if (property_count > kWorldStateSnapshotMaxProperties - total_properties)
            return WorldStateSnapshotCodecError::TooManyProperties;
        total_properties += property_count;
        if (runtime_size > input.size() - offset)
            return WorldStateSnapshotCodecError::InputSizeMismatch;
        record.runtime.assign(data + offset, data + offset + runtime_size);
        offset += runtime_size;

        record.properties.reserve(property_count);
        for (std::uint32_t property_index = 0; property_index != property_count;
             ++property_index) {
            if (input.size() - offset < kPropertyWireSize)
                return WorldStateSnapshotCodecError::InputSizeMismatch;
            const Byte* const property_data = data + offset;
            const PropertyId id = get_u32(property_data + 0);
            const std::uint32_t value_size = get_u32(property_data + 4);
            offset += kPropertyWireSize;
            if (value_size > kWorldStateSnapshotMaxBlobBytes)
                return WorldStateSnapshotCodecError::BlobTooLarge;
            if (value_size > input.size() - offset)
                return WorldStateSnapshotCodecError::InputSizeMismatch;
            PropertySnapshot property{};
            property.property_id = id;
            property.value.assign(data + offset, data + offset + value_size);
            offset += value_size;
            record.properties.push_back(std::move(property));
        }
        decoded.push_back(std::move(record));
    }

    if (offset != input.size())
        return WorldStateSnapshotCodecError::InputSizeMismatch;

    ValidatedRecords validation{};
    const WorldStateSnapshotCodecError validation_error =
        validate_records(std::span<const ObjectRecord>{decoded}, validation);
    if (!world_state_snapshot_codec_succeeded(validation_error))
        return validation_error;
    if (validation.wire_size != input.size())
        return WorldStateSnapshotCodecError::InputSizeMismatch;

    output = std::move(decoded);
    return WorldStateSnapshotCodecError::None;
}

WorldStateSnapshotCodecError decode_world_state_snapshot(
    const ByteView input, WorldStateSnapshot& output)
{
    std::vector<ObjectRecord> decoded;
    const WorldStateSnapshotCodecError error =
        decode_world_state_snapshot(input, decoded);
    if (world_state_snapshot_codec_succeeded(error))
        output.objects = std::move(decoded);
    return error;
}

bool world_state_snapshot_semantic_equal(
    const std::span<const ObjectRecord> left,
    const std::span<const ObjectRecord> right)
{
    ValidatedRecords left_validation{};
    ValidatedRecords right_validation{};
    if (!world_state_snapshot_codec_succeeded(
            validate_records(left, left_validation)) ||
        !world_state_snapshot_codec_succeeded(
            validate_records(right, right_validation)))
        return false;
    if (left.size() != right.size())
        return false;
    for (std::size_t position = 0; position != left_validation.object_order.size();
         ++position) {
        const ObjectRecord& left_record =
            left[left_validation.object_order[position]];
        const ObjectRecord& right_record =
            right[right_validation.object_order[position]];
        if (left_record.object_id != right_record.object_id ||
            left_record.type_id != right_record.type_id ||
            left_record.parent_id != right_record.parent_id ||
            !bytes_equal(left_record.runtime, right_record.runtime) ||
            left_record.properties.size() != right_record.properties.size())
            return false;
        const auto& left_properties =
            left_validation.property_order[left_validation.object_order[position]];
        const auto& right_properties = right_validation
                                            .property_order[right_validation
                                                                .object_order[position]];
        for (std::size_t property_position = 0;
             property_position != left_properties.size(); ++property_position) {
            const PropertySnapshot& left_property =
                left_record.properties[left_properties[property_position]];
            const PropertySnapshot& right_property =
                right_record.properties[right_properties[property_position]];
            if (left_property.property_id != right_property.property_id ||
                !bytes_equal(left_property.value, right_property.value))
                return false;
        }
    }
    return true;
}

bool world_state_snapshot_semantic_equal(const ByteView left, const ByteView right)
{
    std::vector<ObjectRecord> left_records;
    std::vector<ObjectRecord> right_records;
    return world_state_snapshot_codec_succeeded(
               decode_world_state_snapshot(left, left_records)) &&
           world_state_snapshot_codec_succeeded(
               decode_world_state_snapshot(right, right_records)) &&
           world_state_snapshot_semantic_equal(left_records, right_records);
}

WorldStateSnapshotDigest world_state_snapshot_digest(
    const std::span<const ObjectRecord> records)
{
    std::vector<Byte> encoded;
    if (!world_state_snapshot_codec_succeeded(
            encode_world_state_snapshot(records, encoded)))
        return {};
    return digest_bytes(encoded);
}

WorldStateSnapshotDigest world_state_snapshot_digest(const ByteView input)
{
    std::vector<ObjectRecord> records;
    if (!world_state_snapshot_codec_succeeded(
            decode_world_state_snapshot(input, records)))
        return {};
    return world_state_snapshot_digest(records);
}

bool try_world_state_snapshot_digest(
    const std::span<const ObjectRecord> records,
    WorldStateSnapshotDigest& output)
{
    std::vector<Byte> encoded;
    if (!world_state_snapshot_codec_succeeded(
            encode_world_state_snapshot(records, encoded)))
        return false;
    output = digest_bytes(encoded);
    return true;
}

std::string world_state_snapshot_digest_hex(
    const std::span<const ObjectRecord> records)
{
    WorldStateSnapshotDigest digest{};
    if (!try_world_state_snapshot_digest(records, digest))
        return {};
    return hex_digest(digest);
}

bool WorldStateSnapshot::semantic_equal(
    const WorldStateSnapshot& other) const
{ return world_state_snapshot_semantic_equal(objects, other.objects); }

WorldStateSnapshotApplyError apply_world_state_snapshot(
    const std::span<const ObjectRecord> records,
    const WorldStateSnapshotVisitor& visitor)
{
    ValidatedRecords validation{};
    if (!world_state_snapshot_codec_succeeded(
            validate_records(records, validation)))
        return WorldStateSnapshotApplyError::InvalidSnapshot;

    for (const std::size_t index : validation.topological_order) {
        const ObjectRecord& record = records[index];
        if (visitor.create_record && !visitor.create_record(record))
            return WorldStateSnapshotApplyError::CallbackFailed;
        if (!visitor.create_record && visitor.on_create_record &&
            !visitor.on_create_record(record))
            return WorldStateSnapshotApplyError::CallbackFailed;
        if (!visitor.create_record && !visitor.on_create_record &&
            visitor.create_object &&
            !visitor.create_object(record.object_id, record.type_id))
            return WorldStateSnapshotApplyError::CallbackFailed;
        if (!visitor.create_record && !visitor.on_create_record &&
            !visitor.create_object && visitor.on_create &&
            !visitor.on_create(record.object_id, record.type_id))
            return WorldStateSnapshotApplyError::CallbackFailed;
    }

    for (const std::size_t index : validation.topological_order) {
        const ObjectRecord& record = records[index];
        if (record.parent_id == kInvalidHostObjectId)
            continue;
        if (visitor.relationship &&
            !visitor.relationship(record.parent_id, record.object_id))
            return WorldStateSnapshotApplyError::CallbackFailed;
        if (!visitor.relationship && visitor.on_relationship &&
            !visitor.on_relationship(record.parent_id, record.object_id))
            return WorldStateSnapshotApplyError::CallbackFailed;
    }

    for (const std::size_t index : validation.topological_order) {
        const ObjectRecord& record = records[index];
        if (visitor.runtime &&
            !visitor.runtime(record.object_id, ByteView{record.runtime}))
            return WorldStateSnapshotApplyError::CallbackFailed;
        if (!visitor.runtime && visitor.on_runtime &&
            !visitor.on_runtime(record.object_id, ByteView{record.runtime}))
            return WorldStateSnapshotApplyError::CallbackFailed;
        for (const std::size_t property_index : validation.property_order[index]) {
            const PropertySnapshot& property = record.properties[property_index];
            if (visitor.property &&
                !visitor.property(record.object_id, property.property_id,
                                  ByteView{property.value}))
                return WorldStateSnapshotApplyError::CallbackFailed;
            if (!visitor.property && visitor.on_property &&
                !visitor.on_property(record.object_id, property.property_id,
                                     ByteView{property.value}))
                return WorldStateSnapshotApplyError::CallbackFailed;
        }
    }
    return WorldStateSnapshotApplyError::None;
}

WorldStateSnapshotApplyError apply_world_state_snapshot(
    const ByteView input, const WorldStateSnapshotVisitor& visitor)
{
    std::vector<ObjectRecord> records;
    if (!world_state_snapshot_codec_succeeded(
            decode_world_state_snapshot(input, records)))
        return WorldStateSnapshotApplyError::InvalidSnapshot;
    return apply_world_state_snapshot(records, visitor);
}

} // namespace kraken::net
