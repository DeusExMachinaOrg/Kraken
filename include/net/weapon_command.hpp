#ifndef KRAKEN_NET_WEAPON_COMMAND_HPP
#define KRAKEN_NET_WEAPON_COMMAND_HPP

#include "net/net_types.hpp"
#include "net/vehicle_snapshot.hpp"

#include <cstddef>
#include <cstdint>

namespace kraken::net {

// Client-to-host packets are trigger/aim intent.  Host-to-client packets are
// either replicated trigger state or a confirmed shot (has_ammo_state).  The
// distinction is intentional: an automatic weapon must be held locally, not
// reconstructed from a series of delayed one-shot events.
inline constexpr std::uint32_t kWeaponCommandWireMagic = 0x364E5057u; // WPN6
inline constexpr std::uint16_t kWeaponCommandWireVersion = 6;
inline constexpr std::size_t kWeaponCommandWireSize = 56;
inline constexpr std::int32_t kMaxNetworkGunId = 4095;
inline constexpr float kMaxNetworkAimPointComponent = 1'000'000.0f;

struct WeaponCommand {
    std::uint32_t entity_id = 0;
    std::uint32_t sequence = 0;
    std::uint32_t client_tick = 0;
    std::int32_t gun_id = -1;
    bool trigger_held = false;
    // Engine objIds are process-local. The host maps this stable id back to
    // its own target object before it enters the original weapon path.
    std::uint32_t target_entity_id = 0;
    // Stable identity for a shot/presentation event. It is separate from the
    // client tick so duplicate presentation events can be suppressed.
    std::uint32_t shot_id = 1;
    VehicleVector3 aim_point{};
    bool has_aim_point = false;
    // This is the engine's own WeaponLookAtPoint speed argument, captured at
    // the local input boundary.  It is not wall-clock frame time.
    float aim_speed = 1.0f;
    // Only host-confirmed fire events contain this state.  It reconciles the
    // shooting client's HUD with the gun that actually fired on the host.
    std::uint32_t shells_in_current_charge = 0;
    std::uint32_t shells_in_pool = 0;
    bool has_ammo_state = false;
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
    InvalidShotId,
    InvalidAimPoint,
    InvalidAimSpeed,
    InvalidAmmoState,
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
