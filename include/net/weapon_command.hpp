#ifndef KRAKEN_NET_WEAPON_COMMAND_HPP
#define KRAKEN_NET_WEAPON_COMMAND_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>

namespace kraken::net {

// A command is intent only.  The server resolves it against the vehicle's
// real weapon parts and runs the original Gun code in its authoritative ODE
// world; clients must never instantiate a projectile from this packet.
inline constexpr std::uint32_t kWeaponCommandWireMagic = 0x324E5057u; // WPN2
inline constexpr std::uint16_t kWeaponCommandWireVersion = 2;
inline constexpr std::size_t kWeaponCommandWireSize = 28;
inline constexpr std::int32_t kMaxNetworkGunId = 4095;

struct WeaponCommand {
    std::uint32_t entity_id = 0;
    std::uint32_t sequence = 0;
    std::uint32_t client_tick = 0;
    std::int32_t gun_id = -1;
    bool trigger_held = false;
    // objIds are process-local. The host maps this stable id back to its own
    // target object before it enters the original weapon implementation.
    std::uint32_t target_entity_id = 0;
};

enum class WeaponCommandCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidEntityId,
    InvalidGunId,
    InvalidTrigger,
    InvalidTargetEntityId,
};

[[nodiscard]] constexpr bool weapon_command_codec_succeeded(
    WeaponCommandCodecError error) noexcept
{
    return error == WeaponCommandCodecError::None;
}

[[nodiscard]] WeaponCommandCodecError encode_weapon_command(
    const WeaponCommand& command, MutableByteView output) noexcept;
[[nodiscard]] WeaponCommandCodecError decode_weapon_command(
    ByteView input, WeaponCommand& command) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_WEAPON_COMMAND_HPP
