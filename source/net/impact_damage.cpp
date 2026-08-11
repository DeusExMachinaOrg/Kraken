#include "net/impact_damage.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace kraken::net {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kFlagsOffset = 6;
constexpr std::size_t kEventIdOffset = 8;
constexpr std::size_t kServerTickOffset = 12;
constexpr std::size_t kAttackerIdOffset = 16;
constexpr std::size_t kTargetIdOffset = 20;
constexpr std::size_t kAttackerGenerationOffset = 24;
constexpr std::size_t kTargetGenerationOffset = 26;
constexpr std::size_t kGunIdOffset = 28;
constexpr std::size_t kDamageTypeOffset = 32;
constexpr std::size_t kDamageOffset = 36;
constexpr std::size_t kPostHealthOffset = 40;
constexpr std::size_t kHitPositionOffset = 44;
constexpr std::size_t kDirectionOffset = 56;
constexpr std::size_t kNormalOffset = 68;
constexpr std::size_t kPartLengthOffset = 80;
constexpr std::size_t kPartOffset = 84;

void put_u16(Byte* destination, std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* source) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[0])) |
           static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[1]) << 8);
}

void put_u32(Byte* destination, std::uint32_t value) noexcept
{
    for (int index = 0; index != 4; ++index)
        destination[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

[[nodiscard]] std::uint32_t get_u32(const Byte* source) noexcept
{
    std::uint32_t value = 0;
    for (int index = 0; index != 4; ++index)
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[index]))
                 << (index * 8);
    return value;
}

void put_i32(Byte* destination, std::int32_t value) noexcept
{
    put_u32(destination, static_cast<std::uint32_t>(value));
}

[[nodiscard]] std::int32_t get_i32(const Byte* source) noexcept
{
    return static_cast<std::int32_t>(get_u32(source));
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

[[nodiscard]] bool finite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool bounded_vector(const VehicleVector3& value) noexcept
{
    const auto valid = [](float component) noexcept {
        return finite(component) &&
               std::fabs(component) <= kMaxImpactDamageVectorComponent;
    };
    return valid(value.x) && valid(value.y) && valid(value.z);
}

[[nodiscard]] bool valid_part(const std::string& part) noexcept
{
    if (part.size() > kImpactDamagePartMaxBytes)
        return false;
    for (const unsigned char character : part) {
        // Engine part names are text. Reject embedded NUL/control bytes so a
        // peer cannot smuggle a truncated or log-injecting name.
        if (character < 0x20u || character > 0x7eu)
            return false;
    }
    return true;
}

[[nodiscard]] ImpactDamageCodecError validate(const ImpactDamage& event) noexcept
{
    if (event.event_id == 0)
        return ImpactDamageCodecError::InvalidEventId;
    const bool has_attacker_id =
        event.attacker_entity_id != kInvalidNetId;
    const bool has_attacker_generation =
        event.attacker_generation != kInvalidEntityGeneration;
    if (has_attacker_id != has_attacker_generation)
        return ImpactDamageCodecError::InvalidAttacker;
    if (event.target_entity_id == kInvalidNetId)
        return ImpactDamageCodecError::InvalidTarget;
    if (event.target_generation == 0)
        return ImpactDamageCodecError::InvalidGeneration;
    if (event.gun_id < 0 || event.gun_id > kMaxImpactDamageGunId)
        return ImpactDamageCodecError::InvalidGunId;
    if (event.damage_type < 0 || event.damage_type > kMaxImpactDamageType)
        return ImpactDamageCodecError::InvalidDamageType;
    if (!finite(event.damage) || !finite(event.post_health))
        return ImpactDamageCodecError::NonFiniteValue;
    if (event.damage < 0.0f || event.damage > kMaxImpactDamage ||
        event.post_health < 0.0f || event.post_health > kMaxImpactDamageHealth ||
        !bounded_vector(event.hit_position) || !bounded_vector(event.direction) ||
        !bounded_vector(event.normal))
        return ImpactDamageCodecError::ValueOutOfBounds;
    if (!valid_part(event.damaged_part))
        return ImpactDamageCodecError::InvalidPart;
    return ImpactDamageCodecError::None;
}

} // namespace

ImpactDamageCodecError encode_impact_damage(const ImpactDamage& event,
                                            MutableByteView output) noexcept
{
    if (output.size() < kImpactDamageWireSize)
        return ImpactDamageCodecError::OutputTooSmall;

    const ImpactDamageCodecError validation = validate(event);
    if (!impact_damage_codec_succeeded(validation))
        return validation;

    Byte* const data = output.data();
    std::fill_n(data, kImpactDamageWireSize, Byte{});
    put_u32(data + kMagicOffset, kImpactDamageWireMagic);
    put_u16(data + kVersionOffset, kImpactDamageWireVersion);
    put_u16(data + kFlagsOffset,
            event.target_dead ? kImpactDamageFlagTargetDead : 0u);
    put_u32(data + kEventIdOffset, event.event_id);
    put_u32(data + kServerTickOffset, event.server_tick);
    put_u32(data + kAttackerIdOffset, event.attacker_entity_id);
    put_u32(data + kTargetIdOffset, event.target_entity_id);
    put_u16(data + kAttackerGenerationOffset, event.attacker_generation);
    put_u16(data + kTargetGenerationOffset, event.target_generation);
    put_i32(data + kGunIdOffset, event.gun_id);
    put_i32(data + kDamageTypeOffset, event.damage_type);
    put_f32(data + kDamageOffset, event.damage);
    put_f32(data + kPostHealthOffset, event.post_health);
    put_vector(data + kHitPositionOffset, event.hit_position);
    put_vector(data + kDirectionOffset, event.direction);
    put_vector(data + kNormalOffset, event.normal);
    data[kPartLengthOffset] = static_cast<Byte>(event.damaged_part.size());
    std::memcpy(data + kPartOffset, event.damaged_part.data(),
                event.damaged_part.size());
    return ImpactDamageCodecError::None;
}

ImpactDamageCodecError decode_impact_damage(ByteView input,
                                            ImpactDamage& output) noexcept
{
    if (input.size() != kImpactDamageWireSize)
        return ImpactDamageCodecError::InputSizeMismatch;

    const Byte* const data = input.data();
    if (get_u32(data + kMagicOffset) != kImpactDamageWireMagic)
        return ImpactDamageCodecError::BadMagic;
    if (get_u16(data + kVersionOffset) != kImpactDamageWireVersion)
        return ImpactDamageCodecError::BadVersion;
    const std::uint16_t flags = get_u16(data + kFlagsOffset);
    if ((flags & static_cast<std::uint16_t>(~kImpactDamageWireFlags)) != 0 ||
        (data[kPartLengthOffset] == Byte{} &&
         std::any_of(data + kPartOffset,
                     data + kPartOffset + kImpactDamagePartMaxBytes,
                     [](Byte value) { return value != Byte{}; })))
        return ImpactDamageCodecError::BadFlags;

    const std::size_t part_length =
        static_cast<std::size_t>(static_cast<std::uint8_t>(data[kPartLengthOffset]));
    if (part_length > kImpactDamagePartMaxBytes)
        return ImpactDamageCodecError::InvalidPart;
    for (std::size_t index = part_length; index < kImpactDamagePartMaxBytes; ++index)
        if (data[kPartOffset + index] != Byte{})
            return ImpactDamageCodecError::BadFlags;
    if (data[81] != Byte{} || data[82] != Byte{} || data[83] != Byte{})
        return ImpactDamageCodecError::BadFlags;

    ImpactDamage decoded{};
    decoded.event_id = get_u32(data + kEventIdOffset);
    decoded.server_tick = get_u32(data + kServerTickOffset);
    decoded.attacker_entity_id = get_u32(data + kAttackerIdOffset);
    decoded.target_entity_id = get_u32(data + kTargetIdOffset);
    decoded.attacker_generation = get_u16(data + kAttackerGenerationOffset);
    decoded.target_generation = get_u16(data + kTargetGenerationOffset);
    decoded.gun_id = get_i32(data + kGunIdOffset);
    decoded.damage_type = get_i32(data + kDamageTypeOffset);
    decoded.damage = get_f32(data + kDamageOffset);
    decoded.post_health = get_f32(data + kPostHealthOffset);
    decoded.hit_position = get_vector(data + kHitPositionOffset);
    decoded.direction = get_vector(data + kDirectionOffset);
    decoded.normal = get_vector(data + kNormalOffset);
    decoded.target_dead = (flags & kImpactDamageFlagTargetDead) != 0;
    try {
        decoded.damaged_part.assign(
            reinterpret_cast<const char*>(data + kPartOffset), part_length);
    }
    catch (...) {
        return ImpactDamageCodecError::AllocationFailure;
    }

    const ImpactDamageCodecError validation = validate(decoded);
    if (!impact_damage_codec_succeeded(validation))
        return validation;
    output = std::move(decoded);
    return ImpactDamageCodecError::None;
}

ImpactDamageDeduplicator::ImpactDamageDeduplicator(std::size_t capacity)
    : m_capacity(capacity)
{
    m_event_ids.clear();
}

bool ImpactDamageDeduplicator::accept(const std::uint32_t event_id)
{
    if (event_id == 0 || m_capacity == 0)
        return false;
    if (m_has_high_water) {
        // RFC-1982-style 32-bit serial arithmetic.  The host skips zero when
        // wrapping, so max -> 1 is forward by two.  Equal, backward, and the
        // exactly-half-range ambiguous value are all rejected explicitly.
        const std::uint32_t delta = event_id - m_high_water;
        if (delta == 0 || delta >= 0x80000000u)
            return false;
    }
    if (m_event_ids.size() >= m_capacity)
        m_event_ids.pop_front();
    m_event_ids.push_back(event_id);
    m_high_water = event_id;
    m_has_high_water = true;
    return true;
}

bool ImpactDamageDeduplicator::contains(const std::uint32_t event_id) const noexcept
{
    return event_id != 0 &&
           std::find(m_event_ids.begin(), m_event_ids.end(), event_id) !=
               m_event_ids.end();
}

void ImpactDamageDeduplicator::clear() noexcept
{
    m_event_ids.clear();
    m_high_water = 0;
    m_has_high_water = false;
}

} // namespace kraken::net
