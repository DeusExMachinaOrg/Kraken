#ifndef KRAKEN_NET_ENTITY_PROTOCOL_HPP
#define KRAKEN_NET_ENTITY_PROTOCOL_HPP

#include "net/net_types.hpp"
#include "net/vehicle_snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kraken::net {

// Stable identity for every host-authored object. Engine ObjId is deliberately
// excluded: it is allocated independently on every process.
enum class EntityKind : std::uint8_t { PlayerVehicle = 1, NpcVehicle = 2, WorldObject = 3, LootContainer = 4, Wreck = 5 };
inline constexpr std::size_t kEntitySpawnWireSize = 68;
inline constexpr std::size_t kEntityDespawnWireSize = 16;
inline constexpr std::uint32_t kEntitySpawnWireMagic = 0x31505345u; // ESP1
inline constexpr std::uint32_t kEntityDespawnWireMagic = 0x31504445u; // EDP1

struct EntitySpawn {
    std::uint32_t entity_id = 0;
    std::uint16_t generation = 0;
    EntityKind kind = EntityKind::NpcVehicle;
    std::uint32_t prototype_id = 0;
    std::uint32_t owner_entity_id = 0;
    std::int32_t belong = 0;
    VehicleVector3 position{};
    VehicleQuaternion rotation{};
    float health_fraction = 1.0f;
};

struct EntityDespawn { std::uint32_t entity_id = 0; std::uint16_t generation = 0; std::uint8_t reason = 0; };

enum class EntityCodecError : std::uint8_t { None, OutputTooSmall, InputSizeMismatch, BadMagic, BadVersion, BadFlags, InvalidEntity, InvalidKind, InvalidGeneration, NonFiniteValue, InvalidQuaternion, InvalidHealth };
[[nodiscard]] constexpr bool entity_codec_succeeded(EntityCodecError value) noexcept { return value == EntityCodecError::None; }
[[nodiscard]] EntityCodecError encode_entity_spawn(const EntitySpawn&, MutableByteView) noexcept;
[[nodiscard]] EntityCodecError decode_entity_spawn(ByteView, EntitySpawn&) noexcept;
[[nodiscard]] EntityCodecError encode_entity_despawn(const EntityDespawn&, MutableByteView) noexcept;
[[nodiscard]] EntityCodecError decode_entity_despawn(ByteView, EntityDespawn&) noexcept;
[[nodiscard]] constexpr bool is_valid_entity_kind(EntityKind kind) noexcept { return kind >= EntityKind::PlayerVehicle && kind <= EntityKind::Wreck; }
} // namespace kraken::net
#endif
