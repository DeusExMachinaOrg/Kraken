#include "net/weapon_command.hpp"

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

WeaponCommandCodecError validate(const WeaponCommand& command) noexcept
{
    if (command.entity_id == 0)
        return WeaponCommandCodecError::InvalidEntityId;
    if (command.gun_id < 0 || command.gun_id > kMaxNetworkGunId)
        return WeaponCommandCodecError::InvalidGunId;
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
    // trigger is carried by the reserved flag word's low bit to preserve a
    // fixed, explicitly little-endian command layout.
    data[6] = static_cast<Byte>(command.trigger_held ? 1 : 0);
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
    if (data[6] != Byte{} && data[6] != static_cast<Byte>(1) || data[7] != Byte{})
        return WeaponCommandCodecError::BadFlags;

    WeaponCommand decoded{};
    decoded.entity_id = get_u32(data + 8);
    decoded.sequence = get_u32(data + 12);
    decoded.client_tick = get_u32(data + 16);
    decoded.gun_id = static_cast<std::int32_t>(get_u32(data + 20));
    decoded.trigger_held = data[6] == static_cast<Byte>(1);
    const WeaponCommandCodecError validation = validate(decoded);
    if (!weapon_command_codec_succeeded(validation))
        return validation;
    command = decoded;
    return WeaponCommandCodecError::None;
}

} // namespace kraken::net
