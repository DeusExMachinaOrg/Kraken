#ifndef KRAKEN_NET_LOADOUT_PROTOCOL_HPP
#define KRAKEN_NET_LOADOUT_PROTOCOL_HPP

#include "net/entity_registry.hpp"
#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kraken::net {

// Slot names and prototype names are engine data, not ObjIds.  This makes a
// loadout portable between processes where every created VehiclePart has a
// different local object identity.
struct LoadoutPart {
    std::string slot;
    std::string prototype;
};

struct LoadoutProfile {
    NetId entity_id = 0;
    std::uint32_t revision = 0;
    std::vector<LoadoutPart> parts;
    // Zero keeps the version-1 wire form for legacy player loadouts.  Host
    // authored entities always carry the generation so a delayed loadout
    // cannot mutate a replacement that reused the NetId.
    EntityGeneration generation = kInvalidEntityGeneration;
    // A client-owned PlayerVehicle publishes its actual base Vehicle
    // prototype so the host can create an independent authoritative object.
    // -1 preserves the NPC/legacy loadout form.
    std::int32_t base_prototype_id = -1;
};

inline constexpr std::uint32_t kLoadoutWireMagic = 0x31444f4cu; // LOD1
inline constexpr std::uint16_t kLoadoutWireVersion = 1;
inline constexpr std::uint16_t kLoadoutWireVersionWithGeneration = 2;
inline constexpr std::uint16_t kLoadoutWireVersionWithPrototype = 3;
inline constexpr std::size_t kMaxLoadoutParts = 64;
inline constexpr std::size_t kMaxLoadoutNameLength = 63;

enum class LoadoutCodecError : std::uint8_t {
    None, OutputTooSmall, InputSizeMismatch, BadMagic, BadVersion, BadFlags,
    InvalidEntity, InvalidRevision, InvalidGeneration, InvalidPrototype,
    TooManyParts,
    InvalidName, DuplicateSlot,
};

[[nodiscard]] constexpr bool loadout_codec_succeeded(LoadoutCodecError error) noexcept
{ return error == LoadoutCodecError::None; }
[[nodiscard]] LoadoutCodecError encode_loadout(const LoadoutProfile&, std::vector<Byte>&) noexcept;
[[nodiscard]] LoadoutCodecError decode_loadout(ByteView, LoadoutProfile&) noexcept;
} // namespace kraken::net
#endif
