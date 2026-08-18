#ifndef KRAKEN_NET_VEHICLE_TRANSFER_HPP
#define KRAKEN_NET_VEHICLE_TRANSFER_HPP

#include "net/net_types.hpp"
#include "net/entity_registry.hpp"
#include "net/vehicle_descriptor.hpp"
#include "net/world_replication.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace kraken::net {

// Transfer packets are reliable envelopes around the existing opaque world
// replication payload and rich vehicle descriptor codec.  Match control
// remains in match_protocol; this layer only carries the state needed before
// a client may acknowledge MatchSync.
inline constexpr std::uint32_t kVehicleTransferWireMagic = 0x3154564Du; // MVT1
inline constexpr std::uint16_t kVehicleTransferWireVersion = 1;
inline constexpr std::size_t kMaxVehicleTransferPayload = 1024u * 1024u;

enum class ReplicationSourceContext : std::uint8_t {
    LocalAuthoritative,
    NetworkReplay,
    MapLoad,
    Teardown,
};

enum class RuntimeAuthority : std::uint8_t {
    Local,
    Host,
    ClientReplica,
};

[[nodiscard]] constexpr RuntimeAuthority runtime_authority(
    const bool session_active, const bool host) noexcept
{
    if (!session_active)
        return RuntimeAuthority::Local;
    return host ? RuntimeAuthority::Host : RuntimeAuthority::ClientReplica;
}

[[nodiscard]] constexpr bool may_authoritatively_mutate_world(
    const RuntimeAuthority authority) noexcept
{ return authority == RuntimeAuthority::Host; }

[[nodiscard]] constexpr bool is_gameplay_level_name(
    const std::string_view name) noexcept
{
    return !name.empty() && name != "main" && name != "menu" &&
           name != "main_menu" && name != "loading" && name != "shelter";
}

[[nodiscard]] constexpr bool suppress_same_belong_damage(
    const bool friendly_fire, const std::int32_t attacker_belong,
    const std::int32_t target_belong) noexcept
{ return !friendly_fire && attacker_belong == target_belong; }

// Runtime sinks use this small RAII scope to prevent replayed state from
// re-entering the authoritative journal.  It is intentionally independent of
// any engine detour or ABI-specific object type.
class ScopedReplaySuppression final {
public:
    explicit ScopedReplaySuppression(std::size_t& depth) noexcept
        : m_depth(&depth) { ++*m_depth; }
    ScopedReplaySuppression(const ScopedReplaySuppression&) = delete;
    ScopedReplaySuppression& operator=(const ScopedReplaySuppression&) = delete;
    ScopedReplaySuppression(ScopedReplaySuppression&& other) noexcept
        : m_depth(other.m_depth) { other.m_depth = nullptr; }
    ~ScopedReplaySuppression() noexcept
    { if (m_depth != nullptr && *m_depth != 0) --*m_depth; }

private:
    std::size_t* m_depth = nullptr;
};

[[nodiscard]] constexpr bool replication_source_emits_delta(
    const ReplicationSourceContext source) noexcept
{
    return source == ReplicationSourceContext::LocalAuthoritative;
}

struct WorldSnapshotTransfer {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    WorldSnapshot snapshot{};
};

struct WorldDeltaTransfer {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    WorldDelta delta{};
};

struct VehicleDescriptorTransfer {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    std::uint32_t player_id = 0;
    NetId entity_id = 0;
    std::uint16_t generation = 0;
    VehicleDescriptor descriptor{};
};

struct WorldTransferReady {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    std::uint32_t player_id = 0;
    WorldEpoch world_epoch = kInvalidWorldEpoch;
    WorldRevision world_revision = kInvalidWorldRevision;
    std::uint16_t descriptor_count = 0;
};

using MatchWorldSnapshot = WorldSnapshotTransfer;
using MatchWorldDelta = WorldDeltaTransfer;
using MatchVehicleDescriptor = VehicleDescriptorTransfer;
using MatchWorldReady = WorldTransferReady;

enum class VehicleTransferCodecError : std::uint8_t {
    None,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidEpoch,
    InvalidRosterRevision,
    InvalidWorldEpoch,
    InvalidPlayer,
    InvalidEntity,
    InvalidGeneration,
    InvalidRevision,
    PayloadTooLarge,
    DescriptorInvalid,
    TooManyDescriptors,
};

[[nodiscard]] constexpr bool vehicle_transfer_codec_succeeded(
    const VehicleTransferCodecError error) noexcept
{
    return error == VehicleTransferCodecError::None;
}

[[nodiscard]] VehicleTransferCodecError encode_world_snapshot_transfer(
    const WorldSnapshotTransfer&, std::vector<Byte>&);
[[nodiscard]] VehicleTransferCodecError decode_world_snapshot_transfer(
    ByteView, WorldSnapshotTransfer&) noexcept;

[[nodiscard]] VehicleTransferCodecError encode_world_delta_transfer(
    const WorldDeltaTransfer&, std::vector<Byte>&);
[[nodiscard]] VehicleTransferCodecError decode_world_delta_transfer(
    ByteView, WorldDeltaTransfer&) noexcept;

[[nodiscard]] VehicleTransferCodecError encode_vehicle_descriptor_transfer(
    const VehicleDescriptorTransfer&, std::vector<Byte>&);
[[nodiscard]] VehicleTransferCodecError decode_vehicle_descriptor_transfer(
    ByteView, VehicleDescriptorTransfer&) noexcept;

[[nodiscard]] VehicleTransferCodecError encode_world_transfer_ready(
    const WorldTransferReady&, std::vector<Byte>&);
[[nodiscard]] VehicleTransferCodecError decode_world_transfer_ready(
    ByteView, WorldTransferReady&) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_VEHICLE_TRANSFER_HPP
