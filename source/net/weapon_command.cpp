#include "net/weapon_command.hpp"

#include <cmath>
#include <cstring>

namespace kraken::net {
namespace {

void put_u16(Byte* data, std::uint16_t value) noexcept
{
    data[0] = static_cast<Byte>(value);
    data[1] = static_cast<Byte>(value >> 8);
}

void put_u32(Byte* data, std::uint32_t value) noexcept
{
    for (int byte = 0; byte != 4; ++byte)
        data[byte] = static_cast<Byte>(value >> (8 * byte));
}

void put_f32(Byte* data, float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(data, bits);
}

std::uint16_t get_u16(const Byte* data) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[0])) |
           (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[1])) << 8);
}

std::uint32_t get_u32(const Byte* data) noexcept
{
    std::uint32_t value = 0;
    for (int byte = 0; byte != 4; ++byte)
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[byte])) <<
                 (8 * byte);
    return value;
}

float get_f32(const Byte* data) noexcept
{
    const std::uint32_t bits = get_u32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool valid_aim_point(const VehicleVector3& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z) &&
           std::abs(point.x) <= kMaxNetworkAimPointComponent &&
           std::abs(point.y) <= kMaxNetworkAimPointComponent &&
           std::abs(point.z) <= kMaxNetworkAimPointComponent;
}

bool is_zero(const VehicleVector3& point) noexcept
{
    return point.x == 0.0f && point.y == 0.0f && point.z == 0.0f;
}

WeaponCommandCodecError validate(const WeaponCommand& command) noexcept
{
    if (command.entity_id == 0)
        return WeaponCommandCodecError::InvalidEntityId;
    if (command.gun_id < 0 || command.gun_id > kMaxNetworkGunId)
        return WeaponCommandCodecError::InvalidGunId;
    if (command.target_entity_id == command.entity_id)
        return WeaponCommandCodecError::InvalidTargetEntityId;
    if (command.shot_id == 0)
        return WeaponCommandCodecError::InvalidShotId;
    if (!command.has_aim_point && !is_zero(command.aim_point))
        return WeaponCommandCodecError::InvalidAimPoint;
    if (command.has_aim_point && !valid_aim_point(command.aim_point))
        return WeaponCommandCodecError::InvalidAimPoint;
    if (!std::isfinite(command.aim_speed) || command.aim_speed < 0.0f ||
        command.aim_speed > 100.0f)
        return WeaponCommandCodecError::InvalidAimSpeed;
    return WeaponCommandCodecError::None;
}

} // namespace

WeaponCommandCodecError encode_weapon_command(const WeaponCommand& command,
                                               MutableByteView output) noexcept
{
    if (output.size() < kWeaponCommandWireSize)
        return WeaponCommandCodecError::OutputTooSmall;
    const WeaponCommandCodecError validation = validate(command);
    if (!weapon_command_codec_succeeded(validation))
        return validation;

    Byte* const data = output.data();
    put_u32(data + 0, kWeaponCommandWireMagic);
    put_u16(data + 4, kWeaponCommandWireVersion);
    put_u16(data + 6, 0);
    put_u32(data + 8, command.entity_id);
    put_u32(data + 12, command.sequence);
    put_u32(data + 16, command.client_tick);
    put_u32(data + 20, static_cast<std::uint32_t>(command.gun_id));
    put_u32(data + 24, command.target_entity_id);
    put_u32(data + 28, command.shot_id);
    put_f32(data + 32, command.aim_point.x);
    put_f32(data + 36, command.aim_point.y);
    put_f32(data + 40, command.aim_point.z);
    put_f32(data + 44, command.aim_speed);
    put_u32(data + 48, command.shells_in_current_charge);
    put_u32(data + 52, command.shells_in_pool);
    // The low flag byte carries trigger state. The high flag byte carries
    // aim-point presence; both bytes remain explicitly bounded.
    data[6] = static_cast<Byte>(command.trigger_held ? 1 : 0);
    data[7] = static_cast<Byte>((command.has_aim_point ? 1 : 0) |
                                (command.has_ammo_state ? 2 : 0));
    return WeaponCommandCodecError::None;
}

WeaponCommandCodecError decode_weapon_command(ByteView input,
                                               WeaponCommand& command) noexcept
{
    if (input.size() != kWeaponCommandWireSize)
        return WeaponCommandCodecError::InputSizeMismatch;
    const Byte* const data = input.data();
    if (get_u32(data + 0) != kWeaponCommandWireMagic)
        return WeaponCommandCodecError::BadMagic;
    if (get_u16(data + 4) != kWeaponCommandWireVersion)
        return WeaponCommandCodecError::BadVersion;
    if ((data[6] != Byte{} && data[6] != static_cast<Byte>(1)) ||
        (static_cast<std::uint8_t>(data[7]) & ~std::uint8_t{3}) != 0)
        return WeaponCommandCodecError::BadFlags;

    WeaponCommand decoded{};
    decoded.entity_id = get_u32(data + 8);
    decoded.sequence = get_u32(data + 12);
    decoded.client_tick = get_u32(data + 16);
    decoded.gun_id = static_cast<std::int32_t>(get_u32(data + 20));
    decoded.target_entity_id = get_u32(data + 24);
    decoded.shot_id = get_u32(data + 28);
    decoded.aim_point = {get_f32(data + 32), get_f32(data + 36),
                        get_f32(data + 40)};
    decoded.aim_speed = get_f32(data + 44);
    decoded.shells_in_current_charge = get_u32(data + 48);
    decoded.shells_in_pool = get_u32(data + 52);
    decoded.trigger_held = data[6] == static_cast<Byte>(1);
    decoded.has_aim_point = (static_cast<std::uint8_t>(data[7]) & 1u) != 0;
    decoded.has_ammo_state = (static_cast<std::uint8_t>(data[7]) & 2u) != 0;
    const WeaponCommandCodecError validation = validate(decoded);
    if (!weapon_command_codec_succeeded(validation))
        return validation;
    command = decoded;
    return WeaponCommandCodecError::None;
}

} // namespace kraken::net
