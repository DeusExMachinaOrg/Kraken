#ifndef KRAKEN_NET_WORLD_LOOT_HPP
#define KRAKEN_NET_WORLD_LOOT_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kraken::net {

// World loot is a host-authored presentation object.  Engine ObjIds and local
// repository pointers never cross this interface.
using WorldLootId = std::uint32_t;
using WorldLootGeneration = std::uint16_t;
using WorldLootRevision = std::uint32_t;
using WorldLootSessionEpoch = std::uint32_t;

inline constexpr std::uint32_t kWorldLootSpawnMagic = 0x31534c57u; // WLS1
inline constexpr std::uint32_t kWorldLootBaselineMagic = 0x31424c57u; // WLB1
inline constexpr std::uint32_t kWorldLootDeltaMagic = 0x31444c57u; // WLD1
inline constexpr std::uint32_t kWorldLootRemoveMagic = 0x31524c57u; // WLR1
inline constexpr std::uint32_t kWorldLootPickupRequestMagic = 0x31504c57u; // WLP1
inline constexpr std::uint32_t kWorldLootPickupResultMagic = 0x31544357u; // WTC1
inline constexpr std::uint16_t kWorldLootWireVersion = 1;
inline constexpr std::size_t kWorldLootRecordWireSize = 76;
inline constexpr std::size_t kWorldLootSpawnWireSize = kWorldLootRecordWireSize;
inline constexpr std::size_t kWorldLootBaselineHeaderWireSize = 20;
inline constexpr std::size_t kWorldLootDeltaWireSize = 28;
inline constexpr std::size_t kWorldLootRemoveWireSize = 28;
inline constexpr std::size_t kWorldLootPickupRequestWireSize = 32;
inline constexpr std::size_t kWorldLootPickupResultWireSize = 48;
inline constexpr std::size_t kMaxWorldLootBaselineRecords = 1024;

struct WorldLootTransform {
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float rotation_x = 0.0f;
    float rotation_y = 0.0f;
    float rotation_z = 0.0f;
    float rotation_w = 1.0f;
};

struct WorldLootRecord {
    WorldLootSessionEpoch session_epoch = 0;
    WorldLootId loot_id = 0;
    WorldLootGeneration generation = 0;
    WorldLootRevision revision = 0;
    std::uint32_t container_id = 0;
    std::int32_t container_prototype_id = -1;
    std::uint32_t owner_entity_id = 0;
    WorldLootTransform transform{};
    std::int32_t item_prototype_id = -1;
    // -1 means the current engine API exposed no portable item instance id.
    std::int32_t item_instance_id = -1;
    std::uint32_t amount = 0;
};

struct WorldLootSpawn {
    WorldLootRecord record{};
};

struct WorldLootBaseline {
    WorldLootSessionEpoch session_epoch = 0;
    WorldLootRevision revision = 0;
    std::vector<WorldLootRecord> records;
};

struct WorldLootDelta {
    WorldLootSessionEpoch session_epoch = 0;
    WorldLootId loot_id = 0;
    WorldLootGeneration generation = 0;
    WorldLootRevision revision = 0;
    std::uint32_t amount = 0;
};

struct WorldLootRemove {
    WorldLootSessionEpoch session_epoch = 0;
    WorldLootId loot_id = 0;
    WorldLootGeneration generation = 0;
    WorldLootRevision revision = 0;
    std::uint8_t reason = 0;
};

struct WorldLootPickupRequest {
    WorldLootSessionEpoch session_epoch = 0;
    std::uint32_t entity_id = 0;
    WorldLootId loot_id = 0;
    WorldLootGeneration generation = 0;
    std::uint32_t transaction_id = 0;
    std::uint32_t amount = 0;
};

enum class WorldLootPickupCode : std::uint8_t {
    Granted = 0,
    NotFound,
    NotOwner,
    TooFar,
    InventoryFull,
    Exhausted,
    InvalidRequest,
    StaleSession,
    StaleGeneration,
};

struct WorldLootPickupResult {
    WorldLootSessionEpoch session_epoch = 0;
    WorldLootId loot_id = 0;
    WorldLootGeneration generation = 0;
    std::uint32_t transaction_id = 0;
    WorldLootPickupCode code = WorldLootPickupCode::InvalidRequest;
    std::int32_t item_prototype_id = -1;
    std::int32_t item_instance_id = -1;
    std::uint32_t granted_amount = 0;
    std::uint32_t remaining_amount = 0;
    WorldLootRevision revision = 0;
};

enum class WorldLootCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidSessionEpoch,
    InvalidLootId,
    InvalidGeneration,
    InvalidRevision,
    InvalidContainer,
    InvalidPrototype,
    InvalidOwner,
    InvalidAmount,
    InvalidTransform,
    TooManyRecords,
    DuplicateRecord,
    InvalidReason,
    InvalidEntity,
    InvalidTransactionId,
    InvalidPickupCode,
};

[[nodiscard]] constexpr bool world_loot_codec_succeeded(
    WorldLootCodecError error) noexcept
{
    return error == WorldLootCodecError::None;
}

[[nodiscard]] WorldLootCodecError encode_world_loot_spawn(
    const WorldLootSpawn&, MutableByteView) noexcept;
[[nodiscard]] WorldLootCodecError decode_world_loot_spawn(
    ByteView, WorldLootSpawn&) noexcept;
[[nodiscard]] WorldLootCodecError encode_world_loot_baseline(
    const WorldLootBaseline&, std::vector<Byte>&);
[[nodiscard]] WorldLootCodecError decode_world_loot_baseline(
    ByteView, WorldLootBaseline&) noexcept;
[[nodiscard]] WorldLootCodecError encode_world_loot_delta(
    const WorldLootDelta&, MutableByteView) noexcept;
[[nodiscard]] WorldLootCodecError decode_world_loot_delta(
    ByteView, WorldLootDelta&) noexcept;
[[nodiscard]] WorldLootCodecError encode_world_loot_remove(
    const WorldLootRemove&, MutableByteView) noexcept;
[[nodiscard]] WorldLootCodecError decode_world_loot_remove(
    ByteView, WorldLootRemove&) noexcept;
[[nodiscard]] WorldLootCodecError encode_world_loot_pickup_request(
    const WorldLootPickupRequest&, MutableByteView) noexcept;
[[nodiscard]] WorldLootCodecError decode_world_loot_pickup_request(
    ByteView, WorldLootPickupRequest&) noexcept;
[[nodiscard]] WorldLootCodecError encode_world_loot_pickup_result(
    const WorldLootPickupResult&, MutableByteView) noexcept;
[[nodiscard]] WorldLootCodecError decode_world_loot_pickup_result(
    ByteView, WorldLootPickupResult&) noexcept;

enum class WorldLootApplyResult : std::uint8_t {
    Applied,
    Duplicate,
    Stale,
    WrongSessionEpoch,
    Invalid,
};

// Applies reliable packets without allowing delayed or duplicated packets to
// resurrect a removed generation.  It is also the client-side presentation
// store; it has no inventory or engine-object mutation hooks.
class WorldLootReplica final {
public:
    [[nodiscard]] WorldLootApplyResult apply_spawn(
        const WorldLootSpawn&);
    [[nodiscard]] WorldLootApplyResult apply_baseline(
        const WorldLootBaseline&);
    [[nodiscard]] WorldLootApplyResult apply_delta(
        const WorldLootDelta&);
    [[nodiscard]] WorldLootApplyResult apply_remove(
        const WorldLootRemove&);

    [[nodiscard]] WorldLootSessionEpoch session_epoch() const noexcept
    { return m_session_epoch; }
    [[nodiscard]] WorldLootRevision revision() const noexcept
    { return m_revision; }
    [[nodiscard]] const std::vector<WorldLootRecord>& records() const noexcept
    { return m_records; }
    [[nodiscard]] const WorldLootRecord* find(WorldLootId) const noexcept;
    void clear() noexcept;

private:
    struct Tombstone {
        WorldLootId loot_id = 0;
        WorldLootGeneration generation = 0;
        WorldLootRevision revision = 0;
    };

    [[nodiscard]] WorldLootApplyResult accept_epoch(
        WorldLootSessionEpoch) noexcept;
    [[nodiscard]] static bool serial_newer(std::uint32_t,
                                           std::uint32_t) noexcept;
    [[nodiscard]] std::size_t find_index(WorldLootId) const noexcept;
    [[nodiscard]] std::size_t find_tombstone(WorldLootId) const noexcept;
    void remember_tombstone(const WorldLootRemove&);

    WorldLootSessionEpoch m_session_epoch = 0;
    WorldLootRevision m_revision = 0;
    std::vector<WorldLootRecord> m_records;
    std::vector<Tombstone> m_tombstones;
};

using WorldLootState = WorldLootReplica;

} // namespace kraken::net

#endif // KRAKEN_NET_WORLD_LOOT_HPP
