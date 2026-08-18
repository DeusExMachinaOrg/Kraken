#include "net/combat_presentation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace kraken::net {
namespace {

constexpr std::size_t kWeaponBodySize = 40;
constexpr std::size_t kAimBodySize = 64;
constexpr std::size_t kShotBodyPrefixSize = 72;
constexpr std::size_t kImpactBodyPrefixSize = 176;
constexpr std::size_t kDamageBodyPrefixSize = 48;
constexpr std::size_t kAttachmentIdentitySize = 16;
constexpr std::size_t kDeathBodyPrefixSize = 60;
constexpr std::size_t kHornBodyPrefixSize = 12;
constexpr std::size_t kJipBodyPrefixSize = 12;

struct Header {
    CombatPresentationPacketKind kind{};
    std::uint32_t session_epoch = 0;
    std::uint64_t id = 0;
    std::uint32_t server_tick = 0;
};

void put_u16(Byte* const destination, const std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* const destination, const std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index != 4; ++index)
        destination[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

void put_u64(Byte* const destination, const std::uint64_t value) noexcept
{
    for (std::size_t index = 0; index != 8; ++index)
        destination[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

void put_f32(Byte* const destination, const float value) noexcept
{
    put_u32(destination, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::uint16_t get_u16(const Byte* const source) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[0])) |
           static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[1]) << 8);
}

[[nodiscard]] std::uint32_t get_u32(const Byte* const source) noexcept
{
    std::uint32_t result = 0;
    for (std::size_t index = 0; index != 4; ++index)
        result |= static_cast<std::uint32_t>(
                      static_cast<std::uint8_t>(source[index])) <<
                  (index * 8);
    return result;
}

[[nodiscard]] std::uint64_t get_u64(const Byte* const source) noexcept
{
    std::uint64_t result = 0;
    for (std::size_t index = 0; index != 8; ++index)
        result |= static_cast<std::uint64_t>(
                      static_cast<std::uint8_t>(source[index])) <<
                  (index * 8);
    return result;
}

[[nodiscard]] float get_f32(const Byte* const source) noexcept
{
    return std::bit_cast<float>(get_u32(source));
}

void put_vector(Byte* const destination, const VehicleVector3& value) noexcept
{
    put_f32(destination + 0, value.x);
    put_f32(destination + 4, value.y);
    put_f32(destination + 8, value.z);
}

void put_quaternion(Byte* const destination,
                    const VehicleQuaternion& value) noexcept
{
    put_f32(destination + 0, value.x);
    put_f32(destination + 4, value.y);
    put_f32(destination + 8, value.z);
    put_f32(destination + 12, value.w);
}

class Writer final {
public:
    explicit Writer(std::vector<Byte>& output) : m_output(output) {}

    void u8(const std::uint8_t value)
    {
        m_output[m_offset++] = static_cast<Byte>(value);
    }
    void u16(const std::uint16_t value)
    {
        put_u16(m_output.data() + m_offset, value);
        m_offset += 2;
    }
    void u32(const std::uint32_t value)
    {
        put_u32(m_output.data() + m_offset, value);
        m_offset += 4;
    }
    void u64(const std::uint64_t value)
    {
        put_u64(m_output.data() + m_offset, value);
        m_offset += 8;
    }
    void f32(const float value)
    {
        put_f32(m_output.data() + m_offset, value);
        m_offset += 4;
    }
    void vector3(const VehicleVector3& value)
    {
        put_vector(m_output.data() + m_offset, value);
        m_offset += 12;
    }
    void quaternion(const VehicleQuaternion& value)
    {
        put_quaternion(m_output.data() + m_offset, value);
        m_offset += 16;
    }
    void bytes(const std::string& value)
    {
        if (!value.empty())
            std::memcpy(m_output.data() + m_offset, value.data(), value.size());
        m_offset += value.size();
    }
    void skip(const std::size_t count) { m_offset += count; }

private:
    std::vector<Byte>& m_output;
    std::size_t m_offset = 0;
};

class Reader final {
public:
    explicit Reader(const ByteView input) : m_input(input) {}

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept
    {
        if (!take(1))
            return false;
        value = static_cast<std::uint8_t>(m_input[m_offset - 1]);
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t& value) noexcept
    {
        if (!take(2))
            return false;
        value = get_u16(m_input.data() + m_offset - 2);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value) noexcept
    {
        if (!take(4))
            return false;
        value = get_u32(m_input.data() + m_offset - 4);
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& value) noexcept
    {
        if (!take(8))
            return false;
        value = get_u64(m_input.data() + m_offset - 8);
        return true;
    }
    [[nodiscard]] bool f32(float& value) noexcept
    {
        std::uint32_t bits = 0;
        if (!u32(bits))
            return false;
        value = std::bit_cast<float>(bits);
        return true;
    }
    [[nodiscard]] bool vector3(VehicleVector3& value) noexcept
    {
        return f32(value.x) && f32(value.y) && f32(value.z);
    }
    [[nodiscard]] bool quaternion(VehicleQuaternion& value) noexcept
    {
        return f32(value.x) && f32(value.y) && f32(value.z) && f32(value.w);
    }
    [[nodiscard]] bool string(std::string& value,
                              const std::size_t max_bytes) noexcept
    {
        std::uint16_t length = 0;
        if (!u16(length) || length > max_bytes || remaining() < length)
            return false;
        try {
            value.assign(length, '\0');
            if (length != 0)
                std::memcpy(value.data(), m_input.data() + m_offset, length);
        }
        catch (...) {
            return false;
        }
        m_offset += length;
        return true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return m_offset <= m_input.size() ? m_input.size() - m_offset : 0;
    }
    [[nodiscard]] bool finished() const noexcept
    {
        return m_offset == m_input.size();
    }
    [[nodiscard]] const Byte* data() const noexcept
    {
        return m_input.data() + m_offset;
    }
    void skip(const std::size_t count) noexcept
    {
        m_offset += count;
    }

private:
    [[nodiscard]] bool take(const std::size_t count) noexcept
    {
        if (m_offset > m_input.size() || count > m_input.size() - m_offset)
            return false;
        m_offset += count;
        return true;
    }

    ByteView m_input;
    std::size_t m_offset = 0;
};

[[nodiscard]] bool valid_utf8_text(const std::string_view value,
                                   const std::size_t max_bytes,
                                   std::uint32_t* const first_bad = nullptr) noexcept
{
    if (value.size() > max_bytes)
        return false;
    std::size_t index = 0;
    while (index < value.size()) {
        const auto byte = static_cast<std::uint8_t>(value[index]);
        std::uint32_t codepoint = 0;
        std::size_t width = 0;
        if (byte <= 0x7fu) {
            codepoint = byte;
            width = 1;
        }
        else if (byte >= 0xc2u && byte <= 0xdfu) {
            codepoint = byte & 0x1fu;
            width = 2;
        }
        else if (byte >= 0xe0u && byte <= 0xefu) {
            codepoint = byte & 0x0fu;
            width = 3;
        }
        else if (byte >= 0xf0u && byte <= 0xf4u) {
            codepoint = byte & 0x07u;
            width = 4;
        }
        else {
            if (first_bad != nullptr)
                *first_bad = byte;
            return false;
        }
        if (index + width > value.size()) {
            if (first_bad != nullptr)
                *first_bad = byte;
            return false;
        }
        for (std::size_t continuation = 1; continuation != width;
             ++continuation) {
            const auto next = static_cast<std::uint8_t>(
                value[index + continuation]);
            if ((next & 0xc0u) != 0x80u) {
                if (first_bad != nullptr)
                    *first_bad = next;
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if ((width == 3 && codepoint < 0x800u) ||
            (width == 4 && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint <= 0x1fu ||
            (codepoint >= 0x7fu && codepoint <= 0x9fu)) {
            if (first_bad != nullptr)
                *first_bad = codepoint;
            return false;
        }
        index += width;
    }
    return true;
}

[[nodiscard]] bool canonicalize_resource_name(const std::string_view input,
                                              std::string& output) noexcept
{
    output.clear();
    if (input.empty() || input.size() > kMaxResourceCueNameBytes)
        return false;
    try {
        output.reserve(input.size());
        for (const char character : input)
            output.push_back(character == '\\' ? '/' : character);
    }
    catch (...) {
        return false;
    }
    if (!valid_utf8_text(output, kMaxResourceCueNameBytes))
        return false;
    if (output.front() == '/' || output.back() == '/')
        return false;
    std::size_t segment_begin = 0;
    while (segment_begin <= output.size()) {
        const std::size_t separator = output.find('/', segment_begin);
        const std::size_t end = separator == std::string::npos
                                    ? output.size()
                                    : separator;
        if (end == segment_begin ||
            (end - segment_begin == 1 && output[segment_begin] == '.') ||
            (end - segment_begin == 2 && output[segment_begin] == '.' &&
             output[segment_begin + 1] == '.') ||
            output.find(':', segment_begin) < end)
            return false;
        if (separator == std::string::npos)
            break;
        segment_begin = separator + 1;
    }
    return true;
}

void initialize_header(std::vector<Byte>& output,
                       const CombatPresentationPacketKind kind,
                       const std::uint32_t session_epoch,
                       const std::uint64_t id,
                       const std::uint32_t server_tick,
                       const std::size_t body_size)
{
    output.assign(kCombatPresentationHeaderSize + body_size, Byte{});
    Byte* const data = output.data();
    put_u32(data + 0, kCombatPresentationWireMagic);
    put_u16(data + 4, kCombatPresentationWireVersion);
    data[6] = static_cast<Byte>(kind);
    data[7] = Byte{};
    put_u32(data + 8, session_epoch);
    put_u64(data + 12, id);
    put_u32(data + 20, server_tick);
}

[[nodiscard]] CombatPresentationCodecError read_header(
    const ByteView input, const CombatPresentationPacketKind expected,
    Header& header) noexcept
{
    if (input.size() < kCombatPresentationHeaderSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    const Byte* const data = input.data();
    if (get_u32(data + 0) != kCombatPresentationWireMagic)
        return CombatPresentationCodecError::BadMagic;
    if (get_u16(data + 4) != kCombatPresentationWireVersion)
        return CombatPresentationCodecError::BadVersion;
    const auto kind = static_cast<CombatPresentationPacketKind>(
        static_cast<std::uint8_t>(data[6]));
    if (!is_valid_combat_presentation_kind(kind) || kind != expected)
        return CombatPresentationCodecError::BadKind;
    if (data[7] != Byte{})
        return CombatPresentationCodecError::BadFlags;
    header.kind = kind;
    header.session_epoch = get_u32(data + 8);
    header.id = get_u64(data + 12);
    header.server_tick = get_u32(data + 20);
    return CombatPresentationCodecError::None;
}

[[nodiscard]] bool finite(const float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool valid_vector(const VehicleVector3& value) noexcept
{
    const auto valid = [](const float component) noexcept {
        return finite(component) && std::fabs(component) <= kMaxCombatCoordinate;
    };
    return valid(value.x) && valid(value.y) && valid(value.z);
}

[[nodiscard]] bool valid_direction(const VehicleVector3& value) noexcept
{
    if (!valid_vector(value))
        return false;
    const float norm = value.x * value.x + value.y * value.y + value.z * value.z;
    return finite(norm) && norm > 0.0f && std::fabs(norm - 1.0f) <= 0.01f;
}

[[nodiscard]] bool valid_quaternion(const VehicleQuaternion& value) noexcept
{
    if (!finite(value.x) || !finite(value.y) || !finite(value.z) ||
        !finite(value.w) || std::fabs(value.x) > 1.01f ||
        std::fabs(value.y) > 1.01f || std::fabs(value.z) > 1.01f ||
        std::fabs(value.w) > 1.01f)
        return false;
    const float norm = value.x * value.x + value.y * value.y +
                       value.z * value.z + value.w * value.w;
    return finite(norm) && norm > 0.0f && std::fabs(norm - 1.0f) <= 0.01f;
}

[[nodiscard]] bool valid_pose(const CombatPose& pose) noexcept
{
    return valid_vector(pose.position) && valid_quaternion(pose.rotation);
}

[[nodiscard]] CombatPresentationCodecError validate_entity_ref(
    const NetEntityRef& entity, const bool required) noexcept
{
    const bool has_id = entity.net_id != kInvalidNetId;
    const bool has_generation = entity.generation != kInvalidEntityGeneration;
    if (has_id != has_generation)
        return CombatPresentationCodecError::InvalidGeneration;
    if (required && !has_id)
        return CombatPresentationCodecError::InvalidEntity;
    return CombatPresentationCodecError::None;
}

[[nodiscard]] bool valid_gun(const GunAttachmentIdentity& gun) noexcept
{
    return gun.attachment_id != 0 && gun.path_hash != 0;
}

[[nodiscard]] bool valid_optional_attachment(const AttachmentIdentity& value) noexcept
{
    return (value.attachment_id == 0 && value.path_hash == 0) ||
           valid_gun(value);
}

[[nodiscard]] bool same_entity(const NetEntityRef& left,
                               const NetEntityRef& right) noexcept
{
    return left.net_id == right.net_id && left.generation == right.generation;
}

[[nodiscard]] bool conflicting_attachment_identity(
    const AttachmentIdentity& left, const AttachmentIdentity& right) noexcept
{
    return left.attachment_id == right.attachment_id ||
           left.path_hash == right.path_hash;
}

[[nodiscard]] bool same_weapon_identity(const WeaponTriggerState& left,
                                        const WeaponTriggerState& right) noexcept
{
    return same_entity(left.shooter, right.shooter) &&
           conflicting_attachment_identity(left.gun, right.gun);
}

[[nodiscard]] bool same_weapon_identity(const NetEntityRef& left_shooter,
                                        const GunAttachmentIdentity& left_gun,
                                        const NetEntityRef& right_shooter,
                                        const GunAttachmentIdentity& right_gun) noexcept
{
    return same_entity(left_shooter, right_shooter) &&
           conflicting_attachment_identity(left_gun, right_gun);
}

[[nodiscard]] bool same_weapon_identity(const WeaponAimState& left,
                                        const WeaponAimState& right) noexcept
{
    return same_weapon_identity(left.shooter, left.gun, right.shooter,
                                right.gun);
}

[[nodiscard]] CombatPresentationCodecError validate_common(
    const std::uint32_t epoch, const std::uint64_t id,
    const bool transition) noexcept
{
    if (epoch == 0)
        return CombatPresentationCodecError::InvalidEpoch;
    if (id == 0)
        return transition ? CombatPresentationCodecError::InvalidTransitionId
                          : CombatPresentationCodecError::InvalidEventId;
    return CombatPresentationCodecError::None;
}

[[nodiscard]] CombatPresentationCodecError validate_target(
    const ImpactTargetIdentity& target) noexcept
{
    if (!is_valid_impact_target_kind(target.kind))
        return CombatPresentationCodecError::InvalidEnum;
    if (target.kind == ImpactTargetKind::DynamicEntity) {
        if (target.environment_kind != EnvironmentKind::UnboundStatic)
            return CombatPresentationCodecError::InvalidTarget;
        if (const auto error = validate_entity_ref(target.dynamic, true);
            error != CombatPresentationCodecError::None)
            return error == CombatPresentationCodecError::InvalidEntity
                       ? CombatPresentationCodecError::InvalidTarget
                       : error;
        if (target.stable.stable_id != 0 || target.stable.path_hash != 0)
            return CombatPresentationCodecError::InvalidTarget;
        return CombatPresentationCodecError::None;
    }
    if (target.kind == ImpactTargetKind::StableStatic) {
        if (target.environment_kind != EnvironmentKind::UnboundStatic ||
            target.stable.stable_id == 0 || target.stable.path_hash == 0 ||
            target.dynamic.net_id != kInvalidNetId ||
            target.dynamic.generation != kInvalidEntityGeneration)
            return CombatPresentationCodecError::InvalidTarget;
        return CombatPresentationCodecError::None;
    }
    if (!is_valid_environment_kind(target.environment_kind) ||
        target.dynamic.net_id != kInvalidNetId ||
        target.dynamic.generation != kInvalidEntityGeneration ||
        target.stable.stable_id != 0 || target.stable.path_hash != 0)
        return CombatPresentationCodecError::InvalidTarget;
    return CombatPresentationCodecError::None;
}

[[nodiscard]] CombatPresentationCodecError validate_cue(
    const ResourceCue& cue, const bool required) noexcept
{
    return validate_resource_cue(cue, !required);
}

void write_entity(Writer& writer, const NetEntityRef& entity)
{
    writer.u32(entity.net_id);
    writer.u16(entity.generation);
    writer.u16(0);
}

void write_attachment(Writer& writer, const AttachmentIdentity& attachment)
{
    writer.u64(attachment.attachment_id);
    writer.u64(attachment.path_hash);
}

void write_pose(Writer& writer, const CombatPose& pose)
{
    writer.vector3(pose.position);
    writer.quaternion(pose.rotation);
}

void write_cue(Writer& writer, const ResourceCue& cue)
{
    writer.u16(static_cast<std::uint16_t>(cue.name.size()));
    writer.bytes(cue.name);
    writer.u64(cue.hash);
}

[[nodiscard]] bool read_entity(Reader& reader, NetEntityRef& entity) noexcept
{
    std::uint16_t generation = 0;
    std::uint16_t reserved = 0;
    if (!reader.u32(entity.net_id) || !reader.u16(generation) ||
        !reader.u16(reserved) || reserved != 0)
        return false;
    entity.generation = generation;
    return true;
}

[[nodiscard]] bool read_attachment(Reader& reader,
                                   AttachmentIdentity& attachment) noexcept
{
    return reader.u64(attachment.attachment_id) &&
           reader.u64(attachment.path_hash);
}

[[nodiscard]] bool read_pose(Reader& reader, CombatPose& pose) noexcept
{
    return reader.vector3(pose.position) && reader.quaternion(pose.rotation);
}

[[nodiscard]] CombatPresentationCodecError read_cue(
    Reader& reader, ResourceCue& cue) noexcept
{
    std::uint16_t length = 0;
    if (!reader.u16(length))
        return CombatPresentationCodecError::InputSizeMismatch;
    if (length > kMaxResourceCueNameBytes)
        return CombatPresentationCodecError::CueTooLong;
    if (reader.remaining() < static_cast<std::size_t>(length) + 8)
        return CombatPresentationCodecError::InputSizeMismatch;
    try {
        cue.name.assign(length, '\0');
        if (length != 0)
            std::memcpy(cue.name.data(), reader.data(), length);
    }
    catch (...) {
        return CombatPresentationCodecError::AllocationFailure;
    }
    reader.skip(length);
    if (!reader.u64(cue.hash))
        return CombatPresentationCodecError::InputSizeMismatch;
    return validate_resource_cue(cue, true);
}

void write_weapon_body(Writer& writer, const WeaponTriggerState& value)
{
    write_entity(writer, value.shooter);
    write_attachment(writer, value.gun);
    writer.u8(static_cast<std::uint8_t>((value.trigger_held ? 1u : 0u) |
                                        (value.reloading ? 2u : 0u)));
    writer.u8(0);
    writer.u16(0);
    writer.u32(value.shells_in_current_charge);
    writer.u32(value.shells_in_pool);
    writer.f32(value.reload_fraction);
}

[[nodiscard]] bool read_weapon_body(Reader& reader,
                                    WeaponTriggerState& value) noexcept
{
    std::uint8_t flags = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    if (!read_entity(reader, value.shooter) ||
        !read_attachment(reader, value.gun) || !reader.u8(flags) ||
        !reader.u8(reserved8) || !reader.u16(reserved16) || reserved8 != 0 ||
        reserved16 != 0 || (flags & static_cast<std::uint8_t>(~std::uint8_t{3})) != 0 ||
        !reader.u32(value.shells_in_current_charge) ||
        !reader.u32(value.shells_in_pool) || !reader.f32(value.reload_fraction))
        return false;
    value.trigger_held = (flags & 1u) != 0;
    value.reloading = (flags & 2u) != 0;
    return true;
}

void write_aim_body(Writer& writer, const WeaponAimState& value)
{
    write_entity(writer, value.shooter);
    write_attachment(writer, value.gun);
    writer.u8(value.has_target ? 1 : 0);
    writer.u8(0);
    writer.u16(0);
    write_entity(writer, value.target);
    writer.vector3(value.aim_point);
    writer.vector3(value.aim_direction);
    writer.f32(value.aim_speed);
}

[[nodiscard]] bool read_aim_body(Reader& reader, WeaponAimState& value) noexcept
{
    std::uint8_t has_target = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    if (!read_entity(reader, value.shooter) ||
        !read_attachment(reader, value.gun) || !reader.u8(has_target) ||
        !reader.u8(reserved8) || !reader.u16(reserved16) || has_target > 1 ||
        reserved8 != 0 || reserved16 != 0 || !read_entity(reader, value.target) ||
        !reader.vector3(value.aim_point) || !reader.vector3(value.aim_direction) ||
        !reader.f32(value.aim_speed))
        return false;
    value.has_target = has_target != 0;
    return true;
}

void write_shot_body(Writer& writer, const ShotConfirmed& value)
{
    write_entity(writer, value.shooter);
    write_attachment(writer, value.gun);
    writer.u32(value.burst_id);
    writer.u16(value.burst_index);
    writer.u16(value.burst_size);
    write_pose(writer, value.muzzle_pose);
    writer.u32(value.shells_in_current_charge);
    writer.u32(value.shells_in_pool);
    writer.u8(static_cast<std::uint8_t>(value.reload_state));
    writer.u8(0);
    writer.u16(0);
    write_cue(writer, value.presentation.muzzle_cue);
    write_cue(writer, value.presentation.projectile_cue);
    write_cue(writer, value.presentation.shot_cue);
    write_cue(writer, value.presentation.reload_cue);
}

[[nodiscard]] CombatPresentationCodecError read_shot_body(
    Reader& reader, ShotConfirmed& value) noexcept
{
    std::uint8_t reload_state = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    if (!read_entity(reader, value.shooter) || !read_attachment(reader, value.gun) ||
        !reader.u32(value.burst_id) || !reader.u16(value.burst_index) ||
        !reader.u16(value.burst_size) || !read_pose(reader, value.muzzle_pose) ||
        !reader.u32(value.shells_in_current_charge) ||
        !reader.u32(value.shells_in_pool) || !reader.u8(reload_state) ||
        !reader.u8(reserved8) || !reader.u16(reserved16) || reserved8 != 0 ||
        reserved16 != 0)
        return CombatPresentationCodecError::InputSizeMismatch;
    value.reload_state = static_cast<AmmoReloadState>(reload_state);
    if (auto error = read_cue(reader, value.presentation.muzzle_cue);
        error != CombatPresentationCodecError::None)
        return error;
    if (auto error = read_cue(reader, value.presentation.projectile_cue);
        error != CombatPresentationCodecError::None)
        return error;
    if (auto error = read_cue(reader, value.presentation.shot_cue);
        error != CombatPresentationCodecError::None)
        return error;
    return read_cue(reader, value.presentation.reload_cue);
}

void write_impact_body(Writer& writer, const ImpactPresentation& value)
{
    writer.u64(value.shot_id);
    write_entity(writer, value.shooter);
    write_attachment(writer, value.gun);
    writer.u8(static_cast<std::uint8_t>(value.target.kind));
    writer.u8(static_cast<std::uint8_t>(value.target.environment_kind));
    writer.u16(0);
    write_entity(writer, value.target.dynamic);
    writer.u64(value.target.stable.stable_id);
    writer.u64(value.target.stable.path_hash);
    write_attachment(writer, value.target_part);
    writer.u8(static_cast<std::uint8_t>(value.surface));
    writer.u8(static_cast<std::uint8_t>((value.did_damage ? 1u : 0u) |
                                         (value.remove_if_free ? 2u : 0u) |
                                         (value.has_incoming_direction ? 4u : 0u) |
                                         (value.has_decal_tangent ? 8u : 0u)));
    writer.u8(static_cast<std::uint8_t>(value.blocked_reason));
    writer.u8(0);
    writer.vector3(value.hit_position);
    writer.vector3(value.effect_position);
    writer.vector3(value.incoming_direction);
    writer.vector3(value.contact_normal);
    writer.vector3(value.decal_tangent);
    writer.quaternion(value.effect_rotation);
    writer.f32(value.effect_scale);
    writer.u64(value.mesh_id);
    writer.u32(value.material_id);
    writer.u32(0);
    write_cue(writer, value.effect_cue);
    write_cue(writer, value.decal_cue);
}

[[nodiscard]] CombatPresentationCodecError read_impact_body(
    Reader& reader, ImpactPresentation& value) noexcept
{
    std::uint8_t kind = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    std::uint8_t surface = 0;
    std::uint8_t flags = 0;
    std::uint8_t blocked_reason = 0;
    std::uint8_t status_reserved = 0;
    std::uint32_t tail_reserved = 0;
    if (!reader.u64(value.shot_id) || !read_entity(reader, value.shooter) ||
        !read_attachment(reader, value.gun) || !reader.u8(kind) ||
        !reader.u8(reserved8) || !reader.u16(reserved16) ||
        reserved16 != 0 || !read_entity(reader, value.target.dynamic) ||
        !reader.u64(value.target.stable.stable_id) ||
        !reader.u64(value.target.stable.path_hash) ||
        !read_attachment(reader, value.target_part) || !reader.u8(surface) ||
        !reader.u8(flags) || !reader.u8(blocked_reason) ||
        !reader.u8(status_reserved) || status_reserved != 0 ||
        (flags & static_cast<std::uint8_t>(~std::uint8_t{15})) != 0 ||
        !reader.vector3(value.hit_position) ||
        !reader.vector3(value.effect_position) ||
        !reader.vector3(value.incoming_direction) ||
        !reader.vector3(value.contact_normal) ||
        !reader.vector3(value.decal_tangent) ||
        !reader.quaternion(value.effect_rotation) ||
        !reader.f32(value.effect_scale) || !reader.u64(value.mesh_id) ||
        !reader.u32(value.material_id) || !reader.u32(tail_reserved) ||
        tail_reserved != 0)
        return CombatPresentationCodecError::InputSizeMismatch;
    value.target.kind = static_cast<ImpactTargetKind>(kind);
    value.target.environment_kind = static_cast<EnvironmentKind>(reserved8);
    value.surface = static_cast<SurfaceKind>(surface);
    value.did_damage = (flags & 1u) != 0;
    value.remove_if_free = (flags & 2u) != 0;
    value.has_incoming_direction = (flags & 4u) != 0;
    value.has_decal_tangent = (flags & 8u) != 0;
    value.blocked_reason = static_cast<ImpactBlockedReason>(blocked_reason);
    if (auto error = read_cue(reader, value.effect_cue);
        error != CombatPresentationCodecError::None)
        return error;
    return read_cue(reader, value.decal_cue);
}

void write_damage_body(Writer& writer, const DamageResult& value)
{
    writer.u64(value.shot_id);
    writer.u64(value.impact_event_id);
    write_entity(writer, value.shooter);
    write_entity(writer, value.target);
    writer.f32(value.damage);
    writer.f32(value.post_health);
    writer.u8(value.dead_transition ? 1 : 0);
    writer.u8(0);
    writer.u16(0);
    writer.u16(0);
    writer.u16(static_cast<std::uint16_t>(value.damaged_part.size()));
    writer.bytes(value.damaged_part);
}

[[nodiscard]] CombatPresentationCodecError read_damage_body(
    Reader& reader, DamageResult& value) noexcept
{
    std::uint8_t dead = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t body_reserved16 = 0;
    std::uint16_t part_reserved = 0;
    if (!reader.u64(value.shot_id) || !reader.u64(value.impact_event_id) ||
        !read_entity(reader, value.shooter) || !read_entity(reader, value.target) ||
        !reader.f32(value.damage) || !reader.f32(value.post_health) ||
        !reader.u8(dead) || !reader.u8(reserved8) || !reader.u16(body_reserved16) ||
        !reader.u16(part_reserved) || reserved8 != 0 || body_reserved16 != 0 ||
        part_reserved != 0 || dead > 1 ||
        !reader.string(value.damaged_part, kMaxCombatPresentationStringBytes))
        return CombatPresentationCodecError::InputSizeMismatch;
    value.dead_transition = dead != 0;
    return CombatPresentationCodecError::None;
}

void write_death_body(Writer& writer, const DeathWreckPresentation& value)
{
    write_entity(writer, value.entity);
    write_entity(writer, value.wreck_entity);
    writer.u8(static_cast<std::uint8_t>(value.reason));
    writer.u8(value.terminal ? 1 : 0);
    writer.u16(0);
    writer.u64(value.wreck_variant_id);
    writer.u64(value.wreck_archive_id);
    writer.u32(value.wreck_archive_revision);
    writer.u64(value.wreck_archive_digest);
    writer.u32(value.wreck_archive_size);
    writer.u16(value.wreck_archive_chunk_count);
    writer.u16(value.wreck_archive_chunk_size);
    writer.u16(static_cast<std::uint16_t>(value.broken_parts.size()));
    writer.u16(0);
    write_cue(writer, value.death_cue);
    write_cue(writer, value.wreck_cue);
    for (const BrokenPartIdentity& part : value.broken_parts)
        write_attachment(writer, part);
}

[[nodiscard]] CombatPresentationCodecError read_death_body(
    Reader& reader, DeathWreckPresentation& value) noexcept
{
    std::uint8_t reason = 0;
    std::uint8_t terminal = 0;
    std::uint16_t reserved = 0;
    std::uint16_t part_count = 0;
    if (!read_entity(reader, value.entity) ||
        !read_entity(reader, value.wreck_entity) || !reader.u8(reason) ||
        !reader.u8(terminal) || !reader.u16(reserved) || reserved != 0 ||
        terminal > 1 || !reader.u64(value.wreck_variant_id) ||
        !reader.u64(value.wreck_archive_id) ||
        !reader.u32(value.wreck_archive_revision) ||
        !reader.u64(value.wreck_archive_digest) ||
        !reader.u32(value.wreck_archive_size) ||
        !reader.u16(value.wreck_archive_chunk_count) ||
        !reader.u16(value.wreck_archive_chunk_size) ||
        !reader.u16(part_count) ||
        !reader.u16(reserved) || reserved != 0)
        return CombatPresentationCodecError::InputSizeMismatch;
    value.reason = static_cast<DeathWreckReason>(reason);
    value.terminal = terminal != 0;
    if (part_count > kMaxDeathWreckBrokenParts)
        return CombatPresentationCodecError::InvalidCount;
    if (auto error = read_cue(reader, value.death_cue);
        error != CombatPresentationCodecError::None)
        return error;
    if (auto error = read_cue(reader, value.wreck_cue);
        error != CombatPresentationCodecError::None)
        return error;
    try {
        value.broken_parts.reserve(part_count);
        for (std::size_t index = 0; index != part_count; ++index) {
            BrokenPartIdentity part{};
            if (!read_attachment(reader, part))
                return CombatPresentationCodecError::InputSizeMismatch;
            value.broken_parts.push_back(part);
        }
    }
    catch (...) {
        return CombatPresentationCodecError::AllocationFailure;
    }
    return CombatPresentationCodecError::None;
}

void write_horn_body(Writer& writer, const HornState& value)
{
    write_entity(writer, value.vehicle);
    writer.u8(value.active ? 1 : 0);
    writer.u8(0);
    writer.u16(0);
    write_cue(writer, value.horn_cue);
}

[[nodiscard]] CombatPresentationCodecError read_horn_body(
    Reader& reader, HornState& value) noexcept
{
    std::uint8_t active = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    if (!read_entity(reader, value.vehicle) || !reader.u8(active) ||
        !reader.u8(reserved8) || !reader.u16(reserved16) || reserved8 != 0 ||
        reserved16 != 0 || active > 1)
        return CombatPresentationCodecError::InputSizeMismatch;
    value.active = active != 0;
    return read_cue(reader, value.horn_cue);
}

template <typename Type>
CombatPresentationCodecError copy_encoded(
    const Type& value, MutableByteView output,
    CombatPresentationCodecError (*encoder)(const Type&, std::vector<Byte>&)) noexcept
{
    try {
        std::vector<Byte> encoded;
        const CombatPresentationCodecError error = encoder(value, encoded);
        if (!combat_presentation_codec_succeeded(error))
            return error;
        if (output.size() < encoded.size())
            return CombatPresentationCodecError::OutputTooSmall;
        std::copy(encoded.begin(), encoded.end(), output.begin());
        return CombatPresentationCodecError::None;
    }
    catch (...) {
        return CombatPresentationCodecError::AllocationFailure;
    }
}

[[nodiscard]] std::size_t cue_wire_size(const ResourceCue& cue) noexcept
{
    return kResourceCueWirePrefixSize + cue.name.size();
}

[[nodiscard]] bool checked_add(std::size_t& total,
                               const std::size_t value) noexcept
{
    if (value > static_cast<std::size_t>(-1) - total)
        return false;
    total += value;
    return true;
}

[[nodiscard]] std::size_t impact_body_size(const ImpactPresentation& value) noexcept
{
    return kImpactBodyPrefixSize + cue_wire_size(value.effect_cue) +
           cue_wire_size(value.decal_cue);
}

[[nodiscard]] std::size_t shot_body_size(const ShotConfirmed& value) noexcept
{
    return kShotBodyPrefixSize + cue_wire_size(value.presentation.muzzle_cue) +
           cue_wire_size(value.presentation.projectile_cue) +
           cue_wire_size(value.presentation.shot_cue) +
           cue_wire_size(value.presentation.reload_cue);
}

[[nodiscard]] std::size_t death_body_size(
    const DeathWreckPresentation& value) noexcept
{
    return kDeathBodyPrefixSize + cue_wire_size(value.death_cue) +
           cue_wire_size(value.wreck_cue) +
           value.broken_parts.size() * kAttachmentIdentitySize;
}

[[nodiscard]] std::size_t horn_body_size(const HornState& value) noexcept
{
    return kHornBodyPrefixSize + cue_wire_size(value.horn_cue);
}

} // namespace

std::uint64_t resource_cue_hash(const std::string_view canonical_name) noexcept
{
    if (canonical_name.empty())
        return 0;
    std::uint64_t hash = kResourceCueFnv1aOffset;
    for (const unsigned char byte : canonical_name) {
        hash ^= byte;
        hash *= kResourceCueFnv1aPrime;
    }
    return hash;
}

std::string normalize_resource_name(const std::string_view name)
{
    std::string normalized;
    if (!canonicalize_resource_name(name, normalized))
        return {};
    return normalized;
}

ResourceCue make_resource_cue(const std::string_view name)
{
    ResourceCue cue{};
    (void)try_make_resource_cue(name, cue);
    return cue;
}

bool try_make_resource_cue(const std::string_view name,
                          ResourceCue& output) noexcept
{
    output = {};
    try {
        const std::string normalized = normalize_resource_name(name);
        if (normalized.empty())
            return false;
        output.name = normalized;
        output.hash = resource_cue_hash(output.name);
        return output.hash != 0;
    }
    catch (...) {
        return false;
    }
}

CombatPresentationCodecError validate_resource_cue(
    const ResourceCue& cue, const bool allow_empty) noexcept
{
    if (cue.name.empty())
        return cue.hash == 0 && allow_empty
                   ? CombatPresentationCodecError::None
                   : CombatPresentationCodecError::InvalidCue;
    if (cue.name.size() > kMaxResourceCueNameBytes)
        return CombatPresentationCodecError::CueTooLong;
    if (!valid_utf8_text(cue.name, kMaxResourceCueNameBytes))
        return CombatPresentationCodecError::InvalidUtf8;
    try {
        const std::string normalized = normalize_resource_name(cue.name);
        if (normalized.empty() || normalized != cue.name)
            return CombatPresentationCodecError::InvalidPath;
    }
    catch (...) {
        return CombatPresentationCodecError::AllocationFailure;
    }
    if (cue.hash == 0 || resource_cue_hash(cue.name) != cue.hash)
        return CombatPresentationCodecError::InvalidCue;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_weapon_trigger_state(
    const WeaponTriggerState& value) noexcept
{
    if (const auto error = validate_common(value.session_epoch,
                                           value.transition_id, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_entity_ref(value.shooter, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (!valid_gun(value.gun))
        return CombatPresentationCodecError::InvalidAttachment;
    if (value.shells_in_current_charge > kMaxCombatAmmoCount ||
        value.shells_in_pool > kMaxCombatAmmoCount)
        return CombatPresentationCodecError::ValueOutOfBounds;
    if (!finite(value.reload_fraction))
        return CombatPresentationCodecError::NonFiniteValue;
    if (value.reload_fraction < 0.0f || value.reload_fraction > 1.0f)
        return CombatPresentationCodecError::ValueOutOfBounds;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_weapon_aim_state(
    const WeaponAimState& value) noexcept
{
    if (value.session_epoch == 0)
        return CombatPresentationCodecError::InvalidEpoch;
    if (value.update_sequence == 0)
        return CombatPresentationCodecError::InvalidUpdateSequence;
    if (const auto error = validate_entity_ref(value.shooter, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (!valid_gun(value.gun))
        return CombatPresentationCodecError::InvalidAttachment;
    if (const auto error = validate_entity_ref(value.target, value.has_target);
        error != CombatPresentationCodecError::None)
        return error == CombatPresentationCodecError::InvalidEntity
                   ? CombatPresentationCodecError::InvalidTarget
                   : error;
    if (!value.has_target &&
        (value.target.net_id != kInvalidNetId ||
         value.target.generation != kInvalidEntityGeneration))
        return CombatPresentationCodecError::InvalidTarget;
    if (!valid_vector(value.aim_point) || !valid_direction(value.aim_direction) ||
        !finite(value.aim_speed))
        return CombatPresentationCodecError::NonFiniteValue;
    if (value.aim_speed < 0.0f || value.aim_speed > kMaxCombatAimSpeed)
        return CombatPresentationCodecError::ValueOutOfBounds;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_shot_confirmed(
    const ShotConfirmed& value) noexcept
{
    if (const auto error = validate_common(value.session_epoch, value.shot_id,
                                           false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_entity_ref(value.shooter, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (!valid_gun(value.gun))
        return CombatPresentationCodecError::InvalidAttachment;
    if (value.burst_id == 0 || value.burst_size == 0 ||
        value.burst_index >= value.burst_size)
        return CombatPresentationCodecError::InvalidBurst;
    if (!valid_pose(value.muzzle_pose))
        return CombatPresentationCodecError::InvalidQuaternion;
    if (value.shells_in_current_charge > kMaxCombatAmmoCount ||
        value.shells_in_pool > kMaxCombatAmmoCount)
        return CombatPresentationCodecError::ValueOutOfBounds;
    if (!is_valid_ammo_reload_state(value.reload_state))
        return CombatPresentationCodecError::InvalidEnum;
    if (const auto error = validate_cue(value.presentation.muzzle_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_cue(value.presentation.projectile_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_cue(value.presentation.shot_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_cue(value.presentation.reload_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_impact_presentation(
    const ImpactPresentation& value) noexcept
{
    if (const auto error = validate_common(value.session_epoch, value.event_id,
                                           false);
        error != CombatPresentationCodecError::None)
        return error;
    if (value.shot_id == 0)
        return CombatPresentationCodecError::InvalidShotReference;
    if (const auto error = validate_entity_ref(value.shooter, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (!valid_gun(value.gun))
        return CombatPresentationCodecError::InvalidAttachment;
    if (const auto error = validate_target(value.target);
        error != CombatPresentationCodecError::None)
        return error;
    if (!valid_optional_attachment(value.target_part))
        return CombatPresentationCodecError::InvalidAttachment;
    if (!is_valid_surface_kind(value.surface) ||
        !is_valid_impact_blocked_reason(value.blocked_reason))
        return CombatPresentationCodecError::InvalidEnum;
    if (!valid_vector(value.hit_position) ||
        !valid_vector(value.effect_position) ||
        !valid_direction(value.contact_normal))
        return CombatPresentationCodecError::NonFiniteValue;
    if (value.has_incoming_direction) {
        if (!valid_direction(value.incoming_direction))
            return CombatPresentationCodecError::NonFiniteValue;
    }
    else if (value.incoming_direction.x != 0.0f ||
             value.incoming_direction.y != 0.0f ||
             value.incoming_direction.z != 0.0f)
        return CombatPresentationCodecError::InvalidTarget;
    if (value.has_decal_tangent) {
        if (!valid_direction(value.decal_tangent))
            return CombatPresentationCodecError::NonFiniteValue;
        const float dot = value.contact_normal.x * value.decal_tangent.x +
                          value.contact_normal.y * value.decal_tangent.y +
                          value.contact_normal.z * value.decal_tangent.z;
        if (!finite(dot) || std::fabs(dot) > 0.1f)
            return CombatPresentationCodecError::ValueOutOfBounds;
    }
    else if (value.decal_tangent.x != 0.0f ||
             value.decal_tangent.y != 0.0f ||
             value.decal_tangent.z != 0.0f)
        return CombatPresentationCodecError::InvalidTarget;
    if (!valid_quaternion(value.effect_rotation))
        return CombatPresentationCodecError::InvalidQuaternion;
    if (!finite(value.effect_scale) || value.effect_scale < 0.0f ||
        value.effect_scale > kMaxCombatEffectScale)
        return finite(value.effect_scale)
                   ? CombatPresentationCodecError::ValueOutOfBounds
                   : CombatPresentationCodecError::NonFiniteValue;
    if (value.did_damage && value.blocked_reason != ImpactBlockedReason::None)
        return CombatPresentationCodecError::InvalidEnum;
    if (!value.did_damage && value.blocked_reason == ImpactBlockedReason::None)
        return CombatPresentationCodecError::InvalidEnum;
    if (const auto error = validate_cue(value.effect_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_cue(value.decal_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (!value.decal_cue.empty()) {
        if (!value.has_decal_tangent)
            return CombatPresentationCodecError::InvalidTarget;
        if (value.target.kind == ImpactTargetKind::DynamicEntity &&
            (value.target_part.attachment_id == 0 ||
             value.target_part.path_hash == 0))
            return CombatPresentationCodecError::InvalidAttachment;
    }
    if (value.effect_cue.empty() && value.decal_cue.empty())
        return CombatPresentationCodecError::InvalidCue;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_damage_result(
    const DamageResult& value) noexcept
{
    if (const auto error = validate_common(value.session_epoch, value.event_id,
                                           false);
        error != CombatPresentationCodecError::None)
        return error;
    if (value.shot_id == 0)
        return CombatPresentationCodecError::InvalidCorrelation;
    if (const auto error = validate_entity_ref(value.shooter, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_entity_ref(value.target, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (!finite(value.damage) || !finite(value.post_health))
        return CombatPresentationCodecError::NonFiniteValue;
    if (value.damage < 0.0f || value.damage > kMaxCombatHealth ||
        value.post_health < 0.0f || value.post_health > kMaxCombatHealth)
        return CombatPresentationCodecError::ValueOutOfBounds;
    if (value.damaged_part.size() > kMaxCombatPresentationStringBytes)
        return CombatPresentationCodecError::StringTooLong;
    if (!valid_utf8_text(value.damaged_part, kMaxCombatPresentationStringBytes))
        return CombatPresentationCodecError::InvalidUtf8;
    if (value.dead_transition && value.post_health != 0.0f)
        return CombatPresentationCodecError::ValueOutOfBounds;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_death_wreck_presentation(
    const DeathWreckPresentation& value) noexcept
{
    if (const auto error = validate_common(value.session_epoch,
                                           value.transition_id, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_entity_ref(value.entity, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_entity_ref(value.wreck_entity, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (value.entity.net_id == value.wreck_entity.net_id &&
        value.entity.generation == value.wreck_entity.generation)
        return CombatPresentationCodecError::InvalidEntity;
    if (!is_valid_death_wreck_reason(value.reason))
        return CombatPresentationCodecError::InvalidEnum;
    if (!value.terminal)
        return CombatPresentationCodecError::InvalidTerminal;
    if (value.wreck_variant_id == 0 || value.wreck_archive_id == 0 ||
        value.wreck_archive_revision == 0 || value.wreck_archive_digest == 0 ||
        value.wreck_archive_size == 0 ||
        value.wreck_archive_size > kMaxDeathWreckArchiveBytes ||
        value.wreck_archive_chunk_count == 0 ||
        value.wreck_archive_chunk_count > kMaxDeathWreckArchiveChunks ||
        value.wreck_archive_chunk_size == 0 ||
        value.wreck_archive_chunk_size > kMaxDeathWreckArchiveChunkBytes)
        return value.wreck_variant_id == 0
                   ? CombatPresentationCodecError::InvalidWreckVariant
                   : CombatPresentationCodecError::InvalidArchive;
    const std::size_t expected_chunks =
        (static_cast<std::size_t>(value.wreck_archive_size) +
         value.wreck_archive_chunk_size - 1) / value.wreck_archive_chunk_size;
    if (expected_chunks != value.wreck_archive_chunk_count)
        return CombatPresentationCodecError::InvalidArchive;
    if (const auto error = validate_cue(value.death_cue, false);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_cue(value.wreck_cue,
                                        value.wreck_archive_id == 0);
        error != CombatPresentationCodecError::None)
        return error;
    if (value.broken_parts.size() > kMaxDeathWreckBrokenParts)
        return CombatPresentationCodecError::InvalidCount;
    for (std::size_t index = 0; index != value.broken_parts.size(); ++index) {
        if (!valid_gun(value.broken_parts[index]))
            return CombatPresentationCodecError::InvalidAttachment;
        for (std::size_t prior = 0; prior != index; ++prior) {
            if (conflicting_attachment_identity(value.broken_parts[prior],
                                                value.broken_parts[index]))
                return CombatPresentationCodecError::DuplicateIdentity;
        }
    }
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_horn_state(const HornState& value) noexcept
{
    if (const auto error = validate_common(value.session_epoch,
                                           value.transition_id, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_entity_ref(value.vehicle, true);
        error != CombatPresentationCodecError::None)
        return error;
    if (const auto error = validate_cue(value.horn_cue, value.active);
        error != CombatPresentationCodecError::None)
        return error;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError validate_presentation_jip_state(
    const PresentationJipState& value) noexcept
{
    if (value.session_epoch == 0)
        return CombatPresentationCodecError::InvalidEpoch;
    if (value.state_revision == 0)
        return CombatPresentationCodecError::InvalidStateRevision;
    if (value.chunk_count == 0 || value.chunk_count > kMaxPresentationJipChunks ||
        value.chunk_index >= value.chunk_count)
        return CombatPresentationCodecError::InvalidChunk;
    if (value.chunk_count > 1 && value.weapon_triggers.empty() &&
        value.weapon_aims.empty() && value.horn_states.empty() &&
        value.terminal_deaths.empty())
        return CombatPresentationCodecError::InvalidChunk;
    if (value.weapon_triggers.size() > kMaxPresentationJipWeaponStatesPerChunk ||
        value.weapon_aims.size() > kMaxPresentationJipAimStatesPerChunk ||
        value.horn_states.size() > kMaxPresentationJipHornStatesPerChunk ||
        value.terminal_deaths.size() > kMaxPresentationJipDeathStatesPerChunk)
        return CombatPresentationCodecError::InvalidCount;
    for (std::size_t index = 0; index != value.weapon_triggers.size(); ++index) {
        const auto& state = value.weapon_triggers[index];
        if (state.session_epoch != value.session_epoch)
            return CombatPresentationCodecError::InvalidEpoch;
        if (const auto error = validate_weapon_trigger_state(state);
            error != CombatPresentationCodecError::None)
            return error;
        for (std::size_t prior = 0; prior != index; ++prior)
            if (same_weapon_identity(value.weapon_triggers[prior], state))
                return CombatPresentationCodecError::DuplicateIdentity;
    }
    for (std::size_t index = 0; index != value.weapon_aims.size(); ++index) {
        const auto& state = value.weapon_aims[index];
        if (state.session_epoch != value.session_epoch)
            return CombatPresentationCodecError::InvalidEpoch;
        if (const auto error = validate_weapon_aim_state(state);
            error != CombatPresentationCodecError::None)
            return error;
        for (std::size_t prior = 0; prior != index; ++prior)
            if (same_weapon_identity(value.weapon_aims[prior], state))
                return CombatPresentationCodecError::DuplicateIdentity;
    }
    for (std::size_t index = 0; index != value.horn_states.size(); ++index) {
        const auto& state = value.horn_states[index];
        if (state.session_epoch != value.session_epoch)
            return CombatPresentationCodecError::InvalidEpoch;
        if (const auto error = validate_horn_state(state);
            error != CombatPresentationCodecError::None)
            return error;
        for (std::size_t prior = 0; prior != index; ++prior)
            if (same_entity(value.horn_states[prior].vehicle, state.vehicle))
                return CombatPresentationCodecError::DuplicateIdentity;
    }
    for (std::size_t index = 0; index != value.terminal_deaths.size(); ++index) {
        const auto& state = value.terminal_deaths[index];
        if (state.session_epoch != value.session_epoch)
            return CombatPresentationCodecError::InvalidEpoch;
        if (const auto error = validate_death_wreck_presentation(state);
            error != CombatPresentationCodecError::None)
            return error;
        for (std::size_t prior = 0; prior != index; ++prior)
            if (same_entity(value.terminal_deaths[prior].entity, state.entity))
                return CombatPresentationCodecError::DuplicateIdentity;
    }
    std::size_t body_size = kJipBodyPrefixSize;
    for (const auto& state : value.weapon_triggers)
        if (!checked_add(body_size, 12 + kWeaponBodySize))
            return CombatPresentationCodecError::PayloadTooLarge;
    for (const auto& state : value.weapon_aims)
        if (!checked_add(body_size, 12 + kAimBodySize))
            return CombatPresentationCodecError::PayloadTooLarge;
    for (const auto& state : value.horn_states)
        if (!checked_add(body_size, 12 + horn_body_size(state)))
            return CombatPresentationCodecError::PayloadTooLarge;
    for (const auto& state : value.terminal_deaths)
        if (!checked_add(body_size, 12 + death_body_size(state)))
            return CombatPresentationCodecError::PayloadTooLarge;
    if (body_size > kMaxPresentationJipChunkWireSize -
                        kCombatPresentationHeaderSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_weapon_trigger_state(
    const WeaponTriggerState& value, std::vector<Byte>& output)
{
    if (const auto error = validate_weapon_trigger_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output, CombatPresentationPacketKind::WeaponTriggerState,
                      value.session_epoch, value.transition_id, value.server_tick,
                      kWeaponBodySize);
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_weapon_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_weapon_trigger_state(
    const WeaponTriggerState& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_weapon_trigger_state);
}

CombatPresentationCodecError decode_weapon_trigger_state(
    const ByteView input, WeaponTriggerState& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::WeaponTriggerState, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() != kWeaponTriggerStateWireSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    WeaponTriggerState value{};
    value.session_epoch = header.session_epoch;
    value.transition_id = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (!read_weapon_body(reader, value) || !reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_weapon_trigger_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = value;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_weapon_aim_state(
    const WeaponAimState& value, std::vector<Byte>& output)
{
    if (const auto error = validate_weapon_aim_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output, CombatPresentationPacketKind::WeaponAimState,
                      value.session_epoch, value.update_sequence, value.server_tick,
                      kAimBodySize);
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_aim_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_weapon_aim_state(
    const WeaponAimState& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_weapon_aim_state);
}

CombatPresentationCodecError decode_weapon_aim_state(
    const ByteView input, WeaponAimState& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::WeaponAimState, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() != kWeaponAimStateWireSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    WeaponAimState value{};
    value.session_epoch = header.session_epoch;
    value.update_sequence = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (!read_aim_body(reader, value) || !reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_weapon_aim_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = value;
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_shot_confirmed(
    const ShotConfirmed& value, std::vector<Byte>& output)
{
    if (const auto error = validate_shot_confirmed(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output, CombatPresentationPacketKind::ShotConfirmed,
                      value.session_epoch, value.shot_id, value.server_tick,
                      shot_body_size(value));
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_shot_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_shot_confirmed(
    const ShotConfirmed& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_shot_confirmed);
}

CombatPresentationCodecError decode_shot_confirmed(
    const ByteView input, ShotConfirmed& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::ShotConfirmed, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() < kShotConfirmedWireSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    if (input.size() > kMaxShotConfirmedWireSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    ShotConfirmed value{};
    value.session_epoch = header.session_epoch;
    value.shot_id = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (const auto error = read_shot_body(reader, value);
        error != CombatPresentationCodecError::None)
        return error;
    if (!reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_shot_confirmed(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = std::move(value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_impact_presentation(
    const ImpactPresentation& value, std::vector<Byte>& output)
{
    if (const auto error = validate_impact_presentation(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output, CombatPresentationPacketKind::ImpactPresentation,
                      value.session_epoch, value.event_id, value.server_tick,
                      impact_body_size(value));
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_impact_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_impact_presentation(
    const ImpactPresentation& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_impact_presentation);
}

CombatPresentationCodecError decode_impact_presentation(
    const ByteView input, ImpactPresentation& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::ImpactPresentation, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() < kImpactPresentationWireSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    if (input.size() > kMaxImpactPresentationWireSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    ImpactPresentation value{};
    value.session_epoch = header.session_epoch;
    value.event_id = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (const auto error = read_impact_body(reader, value);
        error != CombatPresentationCodecError::None)
        return error;
    if (!reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_impact_presentation(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = std::move(value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_damage_result(
    const DamageResult& value, std::vector<Byte>& output)
{
    if (const auto error = validate_damage_result(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output, CombatPresentationPacketKind::DamageResult,
                      value.session_epoch, value.event_id, value.server_tick,
                      kDamageBodyPrefixSize + value.damaged_part.size());
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_damage_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_damage_result(
    const DamageResult& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_damage_result);
}

CombatPresentationCodecError decode_damage_result(
    const ByteView input, DamageResult& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::DamageResult, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() < kDamageResultWirePrefixSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    if (input.size() > kMaxDamageResultWireSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    DamageResult value{};
    value.session_epoch = header.session_epoch;
    value.event_id = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (const auto error = read_damage_body(reader, value);
        error != CombatPresentationCodecError::None)
        return error;
    if (!reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_damage_result(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = std::move(value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_death_wreck_presentation(
    const DeathWreckPresentation& value, std::vector<Byte>& output)
{
    if (const auto error = validate_death_wreck_presentation(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output,
                      CombatPresentationPacketKind::DeathWreckPresentation,
                      value.session_epoch, value.transition_id, value.server_tick,
                      death_body_size(value));
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_death_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_death_wreck_presentation(
    const DeathWreckPresentation& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_death_wreck_presentation);
}

CombatPresentationCodecError decode_death_wreck_presentation(
    const ByteView input, DeathWreckPresentation& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::DeathWreckPresentation, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() < kDeathWreckPresentationWirePrefixSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    if (input.size() > kMaxDeathWreckPresentationWireSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    DeathWreckPresentation value{};
    value.session_epoch = header.session_epoch;
    value.transition_id = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (const auto error = read_death_body(reader, value);
        error != CombatPresentationCodecError::None)
        return error;
    if (!reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_death_wreck_presentation(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = std::move(value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_horn_state(
    const HornState& value, std::vector<Byte>& output)
{
    if (const auto error = validate_horn_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    initialize_header(output, CombatPresentationPacketKind::HornState,
                      value.session_epoch, value.transition_id, value.server_tick,
                      horn_body_size(value));
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    write_horn_body(writer, value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_horn_state(
    const HornState& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_horn_state);
}

CombatPresentationCodecError decode_horn_state(
    const ByteView input, HornState& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::HornState, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() < kHornStateWireSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    if (input.size() > kMaxHornStateWireSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    HornState value{};
    value.session_epoch = header.session_epoch;
    value.transition_id = header.id;
    value.server_tick = header.server_tick;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    if (const auto error = read_horn_body(reader, value);
        error != CombatPresentationCodecError::None)
        return error;
    if (!reader.finished())
        return CombatPresentationCodecError::InputSizeMismatch;
    if (const auto error = validate_horn_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    output = std::move(value);
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_presentation_jip_state(
    const PresentationJipState& value, std::vector<Byte>& output)
{
    if (const auto error = validate_presentation_jip_state(value);
        error != CombatPresentationCodecError::None)
        return error;
    std::size_t body_size = kJipBodyPrefixSize;
    for (const auto& state : value.weapon_triggers)
        (void)checked_add(body_size, 12 + kWeaponBodySize);
    for (const auto& state : value.weapon_aims)
        (void)checked_add(body_size, 12 + kAimBodySize);
    for (const auto& state : value.horn_states)
        (void)checked_add(body_size, 12 + horn_body_size(state));
    for (const auto& state : value.terminal_deaths)
        (void)checked_add(body_size, 12 + death_body_size(state));
    initialize_header(output, CombatPresentationPacketKind::PresentationJipState,
                      value.session_epoch, value.state_revision, value.server_tick,
                      body_size);
    Writer writer(output);
    writer.skip(kCombatPresentationHeaderSize);
    writer.u16(value.chunk_index);
    writer.u16(value.chunk_count);
    writer.u16(static_cast<std::uint16_t>(value.weapon_triggers.size()));
    writer.u16(static_cast<std::uint16_t>(value.weapon_aims.size()));
    writer.u16(static_cast<std::uint16_t>(value.horn_states.size()));
    writer.u16(static_cast<std::uint16_t>(value.terminal_deaths.size()));
    for (const auto& state : value.weapon_triggers) {
        writer.u64(state.transition_id);
        writer.u32(state.server_tick);
        write_weapon_body(writer, state);
    }
    for (const auto& state : value.weapon_aims) {
        writer.u64(state.update_sequence);
        writer.u32(state.server_tick);
        write_aim_body(writer, state);
    }
    for (const auto& state : value.horn_states) {
        writer.u64(state.transition_id);
        writer.u32(state.server_tick);
        write_horn_body(writer, state);
    }
    for (const auto& state : value.terminal_deaths) {
        writer.u64(state.transition_id);
        writer.u32(state.server_tick);
        write_death_body(writer, state);
    }
    return CombatPresentationCodecError::None;
}

CombatPresentationCodecError encode_presentation_jip_state(
    const PresentationJipState& value, MutableByteView output) noexcept
{
    return copy_encoded(value, output, &encode_presentation_jip_state);
}

CombatPresentationCodecError decode_presentation_jip_state(
    const ByteView input, PresentationJipState& output) noexcept
{
    Header header{};
    if (const auto error = read_header(
            input, CombatPresentationPacketKind::PresentationJipState, header);
        error != CombatPresentationCodecError::None)
        return error;
    if (input.size() < kCombatPresentationHeaderSize + kJipBodyPrefixSize)
        return CombatPresentationCodecError::InputSizeMismatch;
    if (input.size() > kMaxPresentationJipChunkWireSize)
        return CombatPresentationCodecError::PayloadTooLarge;
    Reader reader(input.subspan(kCombatPresentationHeaderSize));
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 0;
    std::uint16_t weapon_count = 0;
    std::uint16_t aim_count = 0;
    std::uint16_t horn_count = 0;
    std::uint16_t death_count = 0;
    if (!reader.u16(chunk_index) || !reader.u16(chunk_count) ||
        !reader.u16(weapon_count) || !reader.u16(aim_count) ||
        !reader.u16(horn_count) || !reader.u16(death_count))
        return CombatPresentationCodecError::InputSizeMismatch;
    if (weapon_count > kMaxPresentationJipWeaponStatesPerChunk ||
        aim_count > kMaxPresentationJipAimStatesPerChunk ||
        horn_count > kMaxPresentationJipHornStatesPerChunk ||
        death_count > kMaxPresentationJipDeathStatesPerChunk)
        return CombatPresentationCodecError::InvalidCount;
    try {
        PresentationJipState value{};
        value.session_epoch = header.session_epoch;
        value.state_revision = header.id;
        value.server_tick = header.server_tick;
        value.chunk_index = chunk_index;
        value.chunk_count = chunk_count;
        value.weapon_triggers.reserve(weapon_count);
        value.weapon_aims.reserve(aim_count);
        value.horn_states.reserve(horn_count);
        value.terminal_deaths.reserve(death_count);
        for (std::size_t index = 0; index != weapon_count; ++index) {
            WeaponTriggerState state{};
            state.session_epoch = header.session_epoch;
            if (!reader.u64(state.transition_id) || !reader.u32(state.server_tick) ||
                !read_weapon_body(reader, state))
                return CombatPresentationCodecError::InputSizeMismatch;
            if (const auto error = validate_weapon_trigger_state(state);
                error != CombatPresentationCodecError::None)
                return error;
            value.weapon_triggers.push_back(std::move(state));
        }
        for (std::size_t index = 0; index != aim_count; ++index) {
            WeaponAimState state{};
            state.session_epoch = header.session_epoch;
            if (!reader.u64(state.update_sequence) || !reader.u32(state.server_tick) ||
                !read_aim_body(reader, state))
                return CombatPresentationCodecError::InputSizeMismatch;
            if (const auto error = validate_weapon_aim_state(state);
                error != CombatPresentationCodecError::None)
                return error;
            value.weapon_aims.push_back(std::move(state));
        }
        for (std::size_t index = 0; index != horn_count; ++index) {
            HornState state{};
            state.session_epoch = header.session_epoch;
            if (!reader.u64(state.transition_id) || !reader.u32(state.server_tick) ||
                read_horn_body(reader, state) != CombatPresentationCodecError::None)
                return CombatPresentationCodecError::InputSizeMismatch;
            if (const auto error = validate_horn_state(state);
                error != CombatPresentationCodecError::None)
                return error;
            value.horn_states.push_back(std::move(state));
        }
        for (std::size_t index = 0; index != death_count; ++index) {
            DeathWreckPresentation state{};
            state.session_epoch = header.session_epoch;
            if (!reader.u64(state.transition_id) || !reader.u32(state.server_tick))
                return CombatPresentationCodecError::InputSizeMismatch;
            if (const auto error = read_death_body(reader, state);
                error != CombatPresentationCodecError::None)
                return error;
            if (const auto error = validate_death_wreck_presentation(state);
                error != CombatPresentationCodecError::None)
                return error;
            value.terminal_deaths.push_back(std::move(state));
        }
        if (!reader.finished())
            return CombatPresentationCodecError::InputSizeMismatch;
        if (const auto error = validate_presentation_jip_state(value);
            error != CombatPresentationCodecError::None)
            return error;
        output = std::move(value);
        return CombatPresentationCodecError::None;
    }
    catch (...) {
        return CombatPresentationCodecError::AllocationFailure;
    }
}

CombatEventDeduplicator::CombatEventDeduplicator(const std::size_t capacity)
    : m_capacity(capacity)
{
}

bool CombatEventDeduplicator::accept(const std::uint32_t session_epoch,
                                     const CombatEventId event_id)
{
    if (session_epoch == 0 || event_id == 0 || m_capacity == 0)
        return false;
    if (!m_have_high_water) {
        m_epoch = session_epoch;
        m_high_water = event_id;
        m_have_high_water = true;
    }
    else if (session_epoch != m_epoch) {
        const std::uint32_t delta = session_epoch - m_epoch;
        if (delta == 0 || delta >= 0x80000000u)
            return false;
        clear();
        m_epoch = session_epoch;
        m_high_water = event_id;
        m_have_high_water = true;
    }
    else if (!combat_event_id_is_newer(m_high_water, event_id)) {
        return false;
    }
    if (m_recent.size() >= m_capacity)
        m_recent.pop_front();
    m_recent.push_back({session_epoch, event_id});
    m_high_water = event_id;
    return true;
}

bool CombatEventDeduplicator::contains(const std::uint32_t session_epoch,
                                       const CombatEventId event_id) const noexcept
{
    return std::any_of(m_recent.begin(), m_recent.end(),
                       [session_epoch, event_id](const Key& key) {
                           return key.session_epoch == session_epoch &&
                                  key.event_id == event_id;
                       });
}

void CombatEventDeduplicator::clear() noexcept
{
    m_recent.clear();
    m_epoch = 0;
    m_high_water = 0;
    m_have_high_water = false;
}

WeaponAimStateTracker::WeaponAimStateTracker(const std::size_t capacity)
    : m_capacity(capacity)
{
}

bool WeaponAimStateTracker::accept(const WeaponAimState& value)
{
    if (validate_weapon_aim_state(value) !=
            CombatPresentationCodecError::None ||
        m_capacity == 0)
        return false;
    if (m_have_epoch && value.session_epoch != m_epoch) {
        const std::uint32_t delta = value.session_epoch - m_epoch;
        if (delta == 0 || delta >= 0x80000000u)
            return false;
        clear();
    }
    if (!m_have_epoch) {
        m_epoch = value.session_epoch;
        m_have_epoch = true;
    }
    const auto found = std::find_if(
        m_latest.begin(), m_latest.end(), [&value](const Entry& entry) {
            return entry.session_epoch == value.session_epoch &&
                   same_weapon_identity(entry.shooter, entry.gun,
                                        value.shooter, value.gun);
        });
    if (found != m_latest.end()) {
        if (found->gun.attachment_id != value.gun.attachment_id ||
            found->gun.path_hash != value.gun.path_hash ||
            !combat_event_id_is_newer(found->current.update_sequence,
                                      value.update_sequence))
            return false;
        found->previous = found->current;
        found->current = value;
        found->have_previous = true;
        return true;
    }
    if (m_latest.size() >= m_capacity)
        return false;
    Entry entry{};
    entry.session_epoch = value.session_epoch;
    entry.shooter = value.shooter;
    entry.gun = value.gun;
    entry.current = value;
    m_latest.push_back(std::move(entry));
    return true;
}

bool WeaponAimStateTracker::interpolation_input(
    const std::uint32_t session_epoch, const NetEntityRef& shooter,
    const GunAttachmentIdentity& gun,
    WeaponAimInterpolationInput& output) const noexcept
{
    const auto found = std::find_if(
        m_latest.begin(), m_latest.end(),
        [&shooter, &gun, session_epoch](const Entry& entry) {
            return entry.session_epoch == session_epoch &&
                   same_entity(entry.shooter, shooter) &&
                   entry.gun.attachment_id == gun.attachment_id &&
                   entry.gun.path_hash == gun.path_hash;
        });
    if (found == m_latest.end() || !found->have_previous)
        return false;
    output.previous = found->previous;
    output.current = found->current;
    return true;
}

void WeaponAimStateTracker::clear() noexcept
{
    m_latest.clear();
    m_epoch = 0;
    m_have_epoch = false;
}

DeathWreckDeduplicator::DeathWreckDeduplicator(const std::size_t capacity)
    : m_capacity(capacity)
{
}

bool DeathWreckDeduplicator::accept(const DeathWreckPresentation& value)
{
    if (validate_death_wreck_presentation(value) !=
        CombatPresentationCodecError::None)
        return false;
    if (m_have_epoch && value.session_epoch != m_epoch) {
        const std::uint32_t delta = value.session_epoch - m_epoch;
        if (delta == 0 || delta >= 0x80000000u)
            return false;
        clear();
    }
    if (!m_have_epoch) {
        m_epoch = value.session_epoch;
        m_have_epoch = true;
    }
    if (contains(value.session_epoch, value.entity) ||
        m_terminal.size() >= m_capacity)
        return false;
    m_terminal.push_back({value.session_epoch, value.entity});
    return true;
}

bool DeathWreckDeduplicator::contains(const std::uint32_t session_epoch,
                                      const NetEntityRef& entity) const noexcept
{
    return std::any_of(m_terminal.begin(), m_terminal.end(),
                       [&entity, session_epoch](const Entry& entry) {
                           return entry.session_epoch == session_epoch &&
                                  same_entity(entry.entity, entity);
                       });
}

void DeathWreckDeduplicator::clear() noexcept
{
    m_terminal.clear();
    m_epoch = 0;
    m_have_epoch = false;
}

HornTransitionDeduplicator::HornTransitionDeduplicator(
    const std::size_t capacity)
    : m_capacity(capacity)
{
}

bool HornTransitionDeduplicator::accept(const HornState& value)
{
    if (validate_horn_state(value) != CombatPresentationCodecError::None)
        return false;
    if (m_have_epoch && value.session_epoch != m_epoch) {
        const std::uint32_t delta = value.session_epoch - m_epoch;
        if (delta == 0 || delta >= 0x80000000u)
            return false;
        clear();
    }
    if (!m_have_epoch) {
        m_epoch = value.session_epoch;
        m_have_epoch = true;
    }
    const auto found = std::find_if(
        m_latest.begin(), m_latest.end(), [&value](const Entry& entry) {
            return entry.session_epoch == value.session_epoch &&
                   same_entity(entry.vehicle, value.vehicle);
        });
    if (found != m_latest.end()) {
        if (!combat_event_id_is_newer(found->transition_id,
                                      value.transition_id))
            return false;
        found->transition_id = value.transition_id;
        return true;
    }
    if (m_latest.size() >= m_capacity)
        return false;
    m_latest.push_back({value.session_epoch, value.vehicle, value.transition_id});
    return true;
}

bool HornTransitionDeduplicator::contains(
    const std::uint32_t session_epoch, const NetEntityRef& vehicle,
    const CombatTransitionId transition_id) const noexcept
{
    return std::any_of(m_latest.begin(), m_latest.end(),
                       [&vehicle, session_epoch, transition_id](
                           const Entry& entry) {
                           return entry.session_epoch == session_epoch &&
                                  same_entity(entry.vehicle, vehicle) &&
                                  entry.transition_id == transition_id;
                       });
}

void HornTransitionDeduplicator::clear() noexcept
{
    m_latest.clear();
    m_epoch = 0;
    m_have_epoch = false;
}

bool PresentationJipReassembler::duplicates_existing_identity(
    const PresentationJipState& chunk) const noexcept
{
    for (const StoredChunk& stored : m_chunks) {
        if (!stored.present)
            continue;
        for (const auto& incoming : chunk.weapon_triggers)
            for (const auto& existing : stored.value.weapon_triggers)
                if (same_weapon_identity(incoming, existing))
                    return true;
        for (const auto& incoming : chunk.weapon_aims)
            for (const auto& existing : stored.value.weapon_aims)
                if (same_weapon_identity(incoming, existing))
                    return true;
        for (const auto& incoming : chunk.horn_states)
            for (const auto& existing : stored.value.horn_states)
                if (same_entity(incoming.vehicle, existing.vehicle))
                    return true;
        for (const auto& incoming : chunk.terminal_deaths)
            for (const auto& existing : stored.value.terminal_deaths)
                if (same_entity(incoming.entity, existing.entity))
                    return true;
    }
    return false;
}

PresentationJipAssemblyResult PresentationJipReassembler::accept(
    const PresentationJipState& chunk)
{
    const auto validation = validate_presentation_jip_state(chunk);
    if (validation == CombatPresentationCodecError::DuplicateIdentity)
        return PresentationJipAssemblyResult::DuplicateIdentity;
    if (validation != CombatPresentationCodecError::None)
        return PresentationJipAssemblyResult::InvalidChunk;
    try {
        std::vector<Byte> canonical_wire;
        if (encode_presentation_jip_state(chunk, canonical_wire) !=
            CombatPresentationCodecError::None)
            return PresentationJipAssemblyResult::InvalidChunk;
        bool new_snapshot = m_chunks.empty();
        if (!new_snapshot && chunk.session_epoch != m_epoch) {
            const std::uint32_t delta = chunk.session_epoch - m_epoch;
            if (delta == 0 || delta >= 0x80000000u)
                return PresentationJipAssemblyResult::Stale;
            new_snapshot = true;
        }
        else if (!new_snapshot && chunk.state_revision != m_revision) {
            if (!combat_event_id_is_newer(m_revision, chunk.state_revision))
                return PresentationJipAssemblyResult::Stale;
            new_snapshot = true;
        }
        if (new_snapshot) {
            m_chunks.assign(chunk.chunk_count, StoredChunk{});
            m_epoch = chunk.session_epoch;
            m_revision = chunk.state_revision;
            m_server_tick = chunk.server_tick;
            m_received = 0;
        }
        else if (chunk.chunk_count != m_chunks.size() ||
                 chunk.server_tick != m_server_tick)
            return PresentationJipAssemblyResult::Inconsistent;
        StoredChunk& destination = m_chunks[chunk.chunk_index];
        if (destination.present)
            return destination.canonical_wire == canonical_wire
                       ? PresentationJipAssemblyResult::Duplicate
                       : PresentationJipAssemblyResult::Inconsistent;
        if (duplicates_existing_identity(chunk))
            return PresentationJipAssemblyResult::DuplicateIdentity;
        destination.value = chunk;
        destination.canonical_wire = std::move(canonical_wire);
        destination.present = true;
        ++m_received;
        return complete() ? PresentationJipAssemblyResult::Complete
                          : PresentationJipAssemblyResult::Accepted;
    }
    catch (...) {
        return PresentationJipAssemblyResult::AllocationFailure;
    }
}

bool PresentationJipReassembler::assemble(
    ReassembledPresentationJipState& output) const
{
    if (!complete())
        return false;
    try {
        std::size_t weapon_count = 0;
        std::size_t aim_count = 0;
        std::size_t horn_count = 0;
        std::size_t death_count = 0;
        for (const auto& chunk : m_chunks) {
            weapon_count += chunk.value.weapon_triggers.size();
            aim_count += chunk.value.weapon_aims.size();
            horn_count += chunk.value.horn_states.size();
            death_count += chunk.value.terminal_deaths.size();
        }
        if (weapon_count > kMaxPresentationJipWeaponStates ||
            aim_count > kMaxPresentationJipAimStates ||
            horn_count > kMaxPresentationJipHornStates ||
            death_count > kMaxPresentationJipDeathStates)
            return false;
        ReassembledPresentationJipState assembled{};
        assembled.session_epoch = m_epoch;
        assembled.state_revision = m_revision;
        assembled.server_tick = m_server_tick;
        assembled.weapon_triggers.reserve(weapon_count);
        assembled.weapon_aims.reserve(aim_count);
        assembled.horn_states.reserve(horn_count);
        assembled.terminal_deaths.reserve(death_count);
        for (const auto& chunk : m_chunks) {
            assembled.weapon_triggers.insert(assembled.weapon_triggers.end(),
                                             chunk.value.weapon_triggers.begin(),
                                             chunk.value.weapon_triggers.end());
            assembled.weapon_aims.insert(assembled.weapon_aims.end(),
                                         chunk.value.weapon_aims.begin(),
                                         chunk.value.weapon_aims.end());
            assembled.horn_states.insert(assembled.horn_states.end(),
                                         chunk.value.horn_states.begin(),
                                         chunk.value.horn_states.end());
            assembled.terminal_deaths.insert(assembled.terminal_deaths.end(),
                                             chunk.value.terminal_deaths.begin(),
                                             chunk.value.terminal_deaths.end());
        }
        output = std::move(assembled);
        return true;
    }
    catch (...) {
        return false;
    }
}

void PresentationJipReassembler::clear() noexcept
{
    m_chunks.clear();
    m_epoch = 0;
    m_revision = 0;
    m_server_tick = 0;
    m_received = 0;
}

bool PresentationJipReassembler::complete() const noexcept
{
    return !m_chunks.empty() && m_received == m_chunks.size();
}

std::size_t PresentationJipReassembler::received_chunk_count() const noexcept
{
    return m_received;
}

std::uint32_t PresentationJipReassembler::session_epoch() const noexcept
{
    return m_epoch;
}

PresentationStateRevision PresentationJipReassembler::state_revision() const noexcept
{
    return m_revision;
}

} // namespace kraken::net
