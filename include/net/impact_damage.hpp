#ifndef KRAKEN_NET_IMPACT_DAMAGE_HPP
#define KRAKEN_NET_IMPACT_DAMAGE_HPP

#include "net/entity_registry.hpp"
#include "net/net_types.hpp"
#include "net/vehicle_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace kraken::net {

// ImpactDamage is a host result, never a client intent.  The wire schema is
// fixed-size so decoding cannot consume bytes from a following frame.
inline constexpr std::uint32_t kImpactDamageWireMagic = 0x31474449u; // IDG1
inline constexpr std::uint16_t kImpactDamageWireVersion = 1;
inline constexpr std::uint16_t kImpactDamageWireFlags = 0x0001u;
inline constexpr std::size_t kImpactDamagePartMaxBytes = 32;
inline constexpr std::size_t kImpactDamageWireSize = 116;
// Gun prototype ids are game-data ids, not player loadout indices.  Current
// EFA content already uses values outside the former 0..4095 command range.
inline constexpr std::int32_t kMaxImpactDamageGunId = INT32_MAX;
inline constexpr std::int32_t kMaxImpactDamageType = 3;
inline constexpr float kMaxImpactDamage = 1'000'000.0f;
inline constexpr float kMaxImpactDamageHealth = 1'000'000.0f;
inline constexpr float kMaxImpactDamageVectorComponent = 1'000'000.0f;

inline constexpr std::uint16_t kImpactDamageFlagTargetDead = 0x0001u;

struct ImpactDamage {
    std::uint32_t event_id = 0;
    std::uint32_t server_tick = 0;
    NetId attacker_entity_id = kInvalidNetId;
    NetId target_entity_id = kInvalidNetId;
    std::uint16_t attacker_generation = 0;
    std::uint16_t target_generation = 0;
    std::int32_t gun_id = 0;
    std::int32_t damage_type = 0;
    float damage = 0.0f;
    float post_health = 0.0f;
    VehicleVector3 hit_position{};
    VehicleVector3 direction{};
    VehicleVector3 normal{};
    bool target_dead = false;
    std::string damaged_part;
};

enum class ImpactDamageCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidEventId,
    InvalidAttacker,
    InvalidTarget,
    InvalidGeneration,
    InvalidGunId,
    InvalidDamageType,
    NonFiniteValue,
    ValueOutOfBounds,
    InvalidPart,
    AllocationFailure,
};

[[nodiscard]] constexpr bool impact_damage_codec_succeeded(
    ImpactDamageCodecError error) noexcept
{
    return error == ImpactDamageCodecError::None;
}

[[nodiscard]] ImpactDamageCodecError encode_impact_damage(
    const ImpactDamage&, MutableByteView) noexcept;
[[nodiscard]] ImpactDamageCodecError decode_impact_damage(
    ByteView, ImpactDamage&) noexcept;

// Reliable ImpactDamage events are emitted in monotonically increasing host
// order.  Keep a bounded recent window for diagnostics while a serial-number
// high-water mark rejects duplicates and old events even after window eviction.
class ImpactDamageDeduplicator final {
public:
    explicit ImpactDamageDeduplicator(std::size_t capacity = 4096);

    [[nodiscard]] bool accept(std::uint32_t event_id);
    [[nodiscard]] bool contains(std::uint32_t event_id) const noexcept;
    void clear() noexcept;

private:
    std::deque<std::uint32_t> m_event_ids;
    std::size_t m_capacity = 0;
    std::uint32_t m_high_water = 0;
    bool m_has_high_water = false;
};

} // namespace kraken::net

#endif // KRAKEN_NET_IMPACT_DAMAGE_HPP
