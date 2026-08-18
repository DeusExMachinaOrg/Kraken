#include "net/match_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace kraken::net {
namespace {

constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kFixedControlSize = kHeaderSize + 8;
constexpr std::size_t kReadyRequestJipSize = kFixedControlSize + 4;
constexpr std::size_t kMapReadySize = kHeaderSize + 16;
constexpr std::size_t kRosterHeaderSize = kHeaderSize + 16;
constexpr std::size_t kRosterPlayerSize = 24;
constexpr std::size_t kRosterSpawnSize = 20;
constexpr std::size_t kLoadHeaderSize = kHeaderSize + 16;
constexpr std::size_t kMaxEncodedPayload = 64u * 1024u;

void put_u16(Byte* const dst, const std::uint16_t value) noexcept
{
    dst[0] = static_cast<Byte>(value & 0xffu);
    dst[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* const dst, const std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < 4; ++index)
        dst[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

void put_u64(Byte* const dst, const std::uint64_t value) noexcept
{
    for (unsigned index = 0; index < 8; ++index)
        dst[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* const src) noexcept
{
    return static_cast<std::uint16_t>(std::uint8_t(src[0])) |
           static_cast<std::uint16_t>(std::uint8_t(src[1])) << 8;
}

[[nodiscard]] std::uint32_t get_u32(const Byte* const src) noexcept
{
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index)
        result |= static_cast<std::uint32_t>(std::uint8_t(src[index]))
                  << (index * 8);
    return result;
}

[[nodiscard]] std::uint64_t get_u64(const Byte* const src) noexcept
{
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index)
        result |= static_cast<std::uint64_t>(std::uint8_t(src[index]))
                  << (index * 8);
    return result;
}

void put_float(Byte* const dst, const float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(dst, bits);
}

[[nodiscard]] float get_float(const Byte* const src) noexcept
{
    const std::uint32_t bits = get_u32(src);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void initialize_header(std::vector<Byte>& output, const std::size_t size)
{
    output.assign(size, Byte{});
    put_u32(output.data(), kMatchProtocolMagic);
    put_u16(output.data() + 4, kMatchProtocolVersion);
    put_u16(output.data() + 6, 0);
}

[[nodiscard]] MatchCodecError decode_header(ByteView input) noexcept
{
    if (input.size() < kHeaderSize)
        return MatchCodecError::InputSizeMismatch;
    if (get_u32(input.data()) != kMatchProtocolMagic)
        return MatchCodecError::BadMagic;
    if (get_u16(input.data() + 4) != kMatchProtocolVersion)
        return MatchCodecError::BadVersion;
    if (get_u16(input.data() + 6) != 0)
        return MatchCodecError::BadFlags;
    return MatchCodecError::None;
}

[[nodiscard]] MatchCodecError validate_epoch(const std::uint32_t epoch)
{
    return epoch == 0 ? MatchCodecError::InvalidEpoch : MatchCodecError::None;
}

[[nodiscard]] MatchCodecError validate_player(const MatchPlayerId player)
{
    return player == kInvalidMatchPlayerId
        ? MatchCodecError::InvalidPlayer : MatchCodecError::None;
}

[[nodiscard]] MatchCodecError validate_entity(const NetId entity)
{
    return entity == kInvalidNetId
        ? MatchCodecError::InvalidEntity : MatchCodecError::None;
}

[[nodiscard]] MatchCodecError validate_revision(const std::uint32_t revision)
{
    return revision == 0 ? MatchCodecError::InvalidRevision
                         : MatchCodecError::None;
}

[[nodiscard]] bool valid_policy(const JoinPolicy policy) noexcept
{
    return policy == JoinPolicy::ClosedAfterStart ||
           policy == JoinPolicy::JoinInProgress;
}

[[nodiscard]] bool valid_spawn(const MatchSpawn& spawn) noexcept
{
    return std::isfinite(spawn.x) && std::isfinite(spawn.y) &&
           std::isfinite(spawn.z) && std::isfinite(spawn.yaw);
}

[[nodiscard]] MatchCodecError validate_roster(const MatchRosterLock& roster)
{
    if (const MatchCodecError error = validate_epoch(roster.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(roster.roster_revision);
        !match_codec_succeeded(error))
        return error;
    if (roster.required_players == 0 || roster.max_players == 0 ||
        roster.required_players > roster.max_players ||
        roster.max_players > kMaxSessionPlayers || !valid_policy(roster.join_policy))
        return MatchCodecError::InvalidRoster;
    if (roster.players.empty() || roster.players.size() > kMaxMatchRosterEntries)
        return MatchCodecError::InvalidRoster;
    if (roster.spawns.size() > roster.players.size() ||
        roster.spawns.size() > kMaxMatchRosterEntries)
        return MatchCodecError::TooManySpawns;
    for (const MatchRosterPlayer& player : roster.players) {
        if (validate_player(player.player_id) != MatchCodecError::None ||
            validate_entity(player.entity_id) != MatchCodecError::None ||
            player.join_order == 0)
            return MatchCodecError::InvalidRoster;
    }
    for (const MatchSpawn& spawn : roster.spawns)
        if (!valid_spawn(spawn))
            return MatchCodecError::InvalidSpawn;
    return MatchCodecError::None;
}

[[nodiscard]] MatchCodecError validate_map(const std::string& value,
                                            const bool required)
{
    if (required && value.empty())
        return MatchCodecError::InvalidMap;
    return value.size() > kMaxMatchMapName ? MatchCodecError::MapTooLong
                                           : MatchCodecError::None;
}

} // namespace

bool begin_match_jip_map_load(
    MatchJipBarrier& barrier, const std::uint32_t session_epoch,
    const std::uint32_t roster_revision, const MatchPlayerId player_id,
    const NetId entity_id) noexcept
{
    if (barrier.state != MatchJipBarrierState::Idle || session_epoch == 0 ||
        roster_revision == 0 || player_id == kInvalidMatchPlayerId ||
        entity_id == kInvalidNetId)
        return false;
    barrier.session_epoch = session_epoch;
    barrier.roster_revision = roster_revision;
    barrier.player_id = player_id;
    barrier.entity_id = entity_id;
    barrier.state = MatchJipBarrierState::MapLoadSent;
    return true;
}

MatchJipBarrierResult accept_match_jip_map_ready(
    MatchJipBarrier& barrier, const MatchMapReady& acknowledgement) noexcept
{
    const bool same_epoch =
        acknowledgement.session_epoch == barrier.session_epoch &&
        acknowledgement.roster_revision == barrier.roster_revision;
    const bool same_identity =
        acknowledgement.player_id == barrier.player_id &&
        acknowledgement.entity_id == barrier.entity_id;
    if (!same_epoch)
        return MatchJipBarrierResult::Stale;
    if (!same_identity)
        return MatchJipBarrierResult::WrongIdentity;
    if (barrier.state == MatchJipBarrierState::MapReady ||
        barrier.state == MatchJipBarrierState::SnapshotStarted)
        return MatchJipBarrierResult::Duplicate;
    if (barrier.state != MatchJipBarrierState::MapLoadSent)
        return MatchJipBarrierResult::NotAwaiting;
    barrier.state = MatchJipBarrierState::MapReady;
    return MatchJipBarrierResult::Accepted;
}

bool mark_match_jip_snapshot_started(MatchJipBarrier& barrier) noexcept
{
    if (barrier.state != MatchJipBarrierState::MapReady)
        return false;
    barrier.state = MatchJipBarrierState::SnapshotStarted;
    return true;
}

MatchCodecError encode_match_ready_request(const MatchReadyRequest& value,
                                           std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    initialize_header(output, value.join_in_progress
                               ? kReadyRequestJipSize : kFixedControlSize);
    put_u32(output.data() + 8, value.session_epoch);
    put_u32(output.data() + 12, value.roster_revision);
    if (value.join_in_progress) {
        output[16] = Byte{1};
        output[17] = Byte{};
        output[18] = Byte{};
        output[19] = Byte{};
    }
    return MatchCodecError::None;
}

MatchCodecError decode_match_ready_request(ByteView input,
                                           MatchReadyRequest& output) noexcept
{
    if (input.size() != kFixedControlSize && input.size() != kReadyRequestJipSize)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    bool join_in_progress = false;
    if (input.size() == kReadyRequestJipSize) {
        if (std::uint8_t(input[16]) > 1 || std::uint8_t(input[17]) != 0 ||
            std::uint8_t(input[18]) != 0 || std::uint8_t(input[19]) != 0)
            return MatchCodecError::BadFlags;
        join_in_progress = std::uint8_t(input[16]) != 0;
    }
    MatchReadyRequest value{get_u32(input.data() + 8),
                            get_u32(input.data() + 12), join_in_progress};
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    output = value;
    return MatchCodecError::None;
}

MatchCodecError encode_match_ready(const MatchReady& value,
                                   std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_player(value.player_id);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_entity(value.entity_id);
        !match_codec_succeeded(error))
        return error;
    initialize_header(output, 24);
    put_u32(output.data() + 8, value.session_epoch);
    put_u32(output.data() + 12, value.player_id);
    put_u32(output.data() + 16, value.entity_id);
    output[20] = static_cast<Byte>(value.ready ? 1 : 0);
    return MatchCodecError::None;
}

MatchCodecError decode_match_ready(ByteView input, MatchReady& output) noexcept
{
    if (input.size() != 24)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    if (std::uint8_t(input[21]) != 0 || std::uint8_t(input[22]) != 0 ||
        std::uint8_t(input[23]) != 0)
        return MatchCodecError::BadFlags;
    MatchReady value{get_u32(input.data() + 8), get_u32(input.data() + 12),
                     get_u32(input.data() + 16), std::uint8_t(input[20]) != 0};
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_player(value.player_id);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_entity(value.entity_id);
        !match_codec_succeeded(error))
        return error;
    if (std::uint8_t(input[20]) > 1)
        return MatchCodecError::BadFlags;
    output = value;
    return MatchCodecError::None;
}

MatchCodecError encode_match_reject(const MatchReject& value,
                                    std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (value.reason == MatchRejectReason::Unknown ||
        static_cast<std::uint8_t>(value.reason) >
            static_cast<std::uint8_t>(MatchRejectReason::NotReady))
        return MatchCodecError::InvalidReason;
    initialize_header(output, kFixedControlSize);
    put_u32(output.data() + 8, value.session_epoch);
    output[12] = static_cast<Byte>(value.reason);
    return MatchCodecError::None;
}

MatchCodecError decode_match_reject(ByteView input, MatchReject& output) noexcept
{
    if (input.size() != kFixedControlSize)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    if (std::uint8_t(input[13]) != 0 || std::uint8_t(input[14]) != 0 ||
        std::uint8_t(input[15]) != 0)
        return MatchCodecError::BadFlags;
    MatchReject value{get_u32(input.data() + 8),
                      static_cast<MatchRejectReason>(std::uint8_t(input[12]))};
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (value.reason == MatchRejectReason::Unknown ||
        static_cast<std::uint8_t>(value.reason) >
            static_cast<std::uint8_t>(MatchRejectReason::NotReady))
        return MatchCodecError::InvalidReason;
    output = value;
    return MatchCodecError::None;
}

MatchCodecError encode_match_roster_lock(const MatchRosterLock& value,
                                         std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_roster(value);
        !match_codec_succeeded(error))
        return error;
    const std::size_t size = kRosterHeaderSize +
        value.players.size() * kRosterPlayerSize +
        value.spawns.size() * kRosterSpawnSize;
    if (size > kMaxEncodedPayload)
        return MatchCodecError::TooManyPlayers;
    initialize_header(output, size);
    Byte* cursor = output.data() + kHeaderSize;
    put_u32(cursor, value.session_epoch);
    put_u32(cursor + 4, value.roster_revision);
    cursor[8] = static_cast<Byte>(value.required_players);
    cursor[9] = static_cast<Byte>(value.max_players);
    cursor[10] = static_cast<Byte>(value.join_policy);
    cursor[11] = static_cast<Byte>(value.friendly_fire ? 1 : 0);
    put_u16(cursor + 12, static_cast<std::uint16_t>(value.players.size()));
    put_u16(cursor + 14, static_cast<std::uint16_t>(value.spawns.size()));
    cursor += 16;
    for (const MatchRosterPlayer& player : value.players) {
        put_u32(cursor, player.player_id);
        put_u32(cursor + 4, player.peer);
        put_u32(cursor + 8, player.entity_id);
        put_u32(cursor + 12, player.join_order);
        cursor[16] = static_cast<Byte>(player.host ? 1 : 0);
        cursor[17] = static_cast<Byte>(player.ready ? 1 : 0);
        cursor += kRosterPlayerSize;
    }
    for (const MatchSpawn& spawn : value.spawns) {
        put_float(cursor, spawn.x);
        put_float(cursor + 4, spawn.y);
        put_float(cursor + 8, spawn.z);
        put_float(cursor + 12, spawn.yaw);
        put_u32(cursor + 16, static_cast<std::uint32_t>(spawn.belong));
        cursor += kRosterSpawnSize;
    }
    return MatchCodecError::None;
}

MatchCodecError decode_match_roster_lock(ByteView input,
                                         MatchRosterLock& output)
{
    if (input.size() < kRosterHeaderSize)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    const Byte* cursor = input.data() + kHeaderSize;
    MatchRosterLock value{};
    value.session_epoch = get_u32(cursor);
    value.roster_revision = get_u32(cursor + 4);
    value.required_players = std::uint8_t(cursor[8]);
    value.max_players = std::uint8_t(cursor[9]);
    value.join_policy = static_cast<JoinPolicy>(std::uint8_t(cursor[10]));
    value.friendly_fire = std::uint8_t(cursor[11]) != 0;
    const std::size_t player_count = get_u16(cursor + 12);
    const std::size_t spawn_count = get_u16(cursor + 14);
    if (std::uint8_t(cursor[11]) > 1 || player_count == 0 ||
        player_count > kMaxMatchRosterEntries ||
        spawn_count > kMaxMatchRosterEntries)
        return MatchCodecError::InvalidRoster;
    const std::size_t expected = kRosterHeaderSize +
        player_count * kRosterPlayerSize + spawn_count * kRosterSpawnSize;
    if (input.size() != expected)
        return MatchCodecError::InputSizeMismatch;
    cursor += 16;
    value.players.reserve(player_count);
    for (std::size_t index = 0; index < player_count; ++index) {
        if (std::uint8_t(cursor[17]) > 1 || std::uint8_t(cursor[16]) > 1)
            return MatchCodecError::InvalidRoster;
        value.players.push_back({get_u32(cursor), get_u32(cursor + 4),
                                 get_u32(cursor + 8), get_u32(cursor + 12),
                                 std::uint8_t(cursor[16]) != 0,
                                 std::uint8_t(cursor[17]) != 0});
        cursor += kRosterPlayerSize;
    }
    value.spawns.reserve(spawn_count);
    for (std::size_t index = 0; index < spawn_count; ++index) {
        value.spawns.push_back({get_float(cursor), get_float(cursor + 4),
                                 get_float(cursor + 8), get_float(cursor + 12),
                                 static_cast<std::int32_t>(get_u32(cursor + 16))});
        cursor += kRosterSpawnSize;
    }
    if (const MatchCodecError error = validate_roster(value);
        !match_codec_succeeded(error))
        return error;
    output = std::move(value);
    return MatchCodecError::None;
}

MatchCodecError encode_match_load(const MatchLoad& value,
                                  std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_map(value.target_map, true);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_map(value.exit_map, false);
        !match_codec_succeeded(error))
        return error;
    if (value.target_map.size() > (std::numeric_limits<std::uint16_t>::max)() ||
        value.exit_map.size() > (std::numeric_limits<std::uint16_t>::max)())
        return MatchCodecError::MapTooLong;
    const std::size_t size = kLoadHeaderSize + value.target_map.size() +
                             value.exit_map.size();
    if (size > kMaxEncodedPayload)
        return MatchCodecError::MapTooLong;
    initialize_header(output, size);
    Byte* cursor = output.data() + kHeaderSize;
    put_u32(cursor, value.session_epoch);
    put_u32(cursor + 4, value.roster_revision);
    cursor[8] = static_cast<Byte>(value.friendly_fire ? 1 : 0);
    put_u16(cursor + 10, static_cast<std::uint16_t>(value.target_map.size()));
    put_u16(cursor + 12, static_cast<std::uint16_t>(value.exit_map.size()));
    cursor += 16;
    std::memcpy(cursor, value.target_map.data(), value.target_map.size());
    cursor += value.target_map.size();
    std::memcpy(cursor, value.exit_map.data(), value.exit_map.size());
    return MatchCodecError::None;
}

MatchCodecError decode_match_load(ByteView input, MatchLoad& output)
{
    if (input.size() < kLoadHeaderSize)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    const Byte* cursor = input.data() + kHeaderSize;
    MatchLoad value{};
    value.session_epoch = get_u32(cursor);
    value.roster_revision = get_u32(cursor + 4);
    if (std::uint8_t(cursor[8]) > 1 || get_u16(cursor + 14) != 0)
        return MatchCodecError::BadFlags;
    value.friendly_fire = std::uint8_t(cursor[8]) != 0;
    const std::size_t target_size = get_u16(cursor + 10);
    const std::size_t exit_size = get_u16(cursor + 12);
    if (target_size == 0 || target_size > kMaxMatchMapName ||
        exit_size > kMaxMatchMapName)
        return target_size == 0 ? MatchCodecError::InvalidMap
                                : MatchCodecError::MapTooLong;
    if (input.size() != kLoadHeaderSize + target_size + exit_size)
        return MatchCodecError::InputSizeMismatch;
    cursor += 16;
    value.target_map.assign(reinterpret_cast<const char*>(cursor), target_size);
    cursor += target_size;
    value.exit_map.assign(reinterpret_cast<const char*>(cursor), exit_size);
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    output = std::move(value);
    return MatchCodecError::None;
}

MatchCodecError encode_match_map_ready(const MatchMapReady& value,
                                       std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_player(value.player_id);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_entity(value.entity_id);
        !match_codec_succeeded(error))
        return error;
    initialize_header(output, kMapReadySize);
    put_u32(output.data() + 8, value.session_epoch);
    put_u32(output.data() + 12, value.roster_revision);
    put_u32(output.data() + 16, value.player_id);
    put_u32(output.data() + 20, value.entity_id);
    return MatchCodecError::None;
}

MatchCodecError decode_match_map_ready(ByteView input,
                                       MatchMapReady& output) noexcept
{
    if (input.size() != kMapReadySize)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    const MatchMapReady value{get_u32(input.data() + 8),
                              get_u32(input.data() + 12),
                              get_u32(input.data() + 16),
                              get_u32(input.data() + 20)};
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_player(value.player_id);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_entity(value.entity_id);
        !match_codec_succeeded(error))
        return error;
    output = value;
    return MatchCodecError::None;
}

MatchCodecError encode_match_sync(const MatchSync& value,
                                  std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_player(value.player_id);
        !match_codec_succeeded(error))
        return error;
    // Revision zero is the valid initial empty snapshot.  Ordering/stale
    // checks belong to WorldJoinBarrier, not this wire codec.
    initialize_header(output, 32);
    put_u32(output.data() + 8, value.session_epoch);
    put_u32(output.data() + 12, value.roster_revision);
    put_u32(output.data() + 16, value.player_id);
    put_u64(output.data() + 20, value.snapshot_revision);
    output[28] = static_cast<Byte>(value.request ? 1 : 0);
    return MatchCodecError::None;
}

MatchCodecError decode_match_sync(ByteView input, MatchSync& output) noexcept
{
    if (input.size() != 32)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    if (std::uint8_t(input[29]) != 0 || std::uint8_t(input[30]) != 0 ||
        std::uint8_t(input[31]) != 0 || std::uint8_t(input[28]) > 1)
        return MatchCodecError::BadFlags;
    MatchSync value{get_u32(input.data() + 8), get_u32(input.data() + 12),
                    get_u32(input.data() + 16), get_u64(input.data() + 20),
                    std::uint8_t(input[28]) != 0};
    if (validate_epoch(value.session_epoch) != MatchCodecError::None ||
        validate_revision(value.roster_revision) != MatchCodecError::None ||
        validate_player(value.player_id) != MatchCodecError::None)
        return MatchCodecError::InvalidRevision;
    output = value;
    return MatchCodecError::None;
}

MatchCodecError encode_match_play(const MatchPlay& value,
                                  std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (const MatchCodecError error = validate_revision(value.roster_revision);
        !match_codec_succeeded(error))
        return error;
    initialize_header(output, kFixedControlSize);
    put_u32(output.data() + 8, value.session_epoch);
    put_u32(output.data() + 12, value.roster_revision);
    return MatchCodecError::None;
}

MatchCodecError decode_match_play(ByteView input, MatchPlay& output) noexcept
{
    if (input.size() != kFixedControlSize)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    MatchPlay value{get_u32(input.data() + 8), get_u32(input.data() + 12)};
    if (validate_epoch(value.session_epoch) != MatchCodecError::None ||
        validate_revision(value.roster_revision) != MatchCodecError::None)
        return MatchCodecError::InvalidRevision;
    output = value;
    return MatchCodecError::None;
}

MatchCodecError encode_match_leave(const MatchLeave& value,
                                   std::vector<Byte>& output)
{
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    if (!is_valid_match_leave_reason(value.reason))
        return MatchCodecError::InvalidReason;
    initialize_header(output, kHeaderSize + 8);
    put_u32(output.data() + 8, value.session_epoch);
    output[12] = static_cast<Byte>(value.reason);
    output[13] = value.terminate_match ? Byte{1} : Byte{0};
    output[14] = Byte{};
    output[15] = Byte{};
    return MatchCodecError::None;
}

MatchCodecError decode_match_leave(ByteView input, MatchLeave& output) noexcept
{
    // Four-byte leaves were emitted by the first runtime revision.  They are
    // still accepted as a user leave; new peers use the reason-bearing form.
    if (input.size() != kHeaderSize + 4 && input.size() != kHeaderSize + 8)
        return MatchCodecError::InputSizeMismatch;
    if (const MatchCodecError error = decode_header(input);
        !match_codec_succeeded(error))
        return error;
    MatchLeave value{get_u32(input.data() + 8)};
    if (input.size() == kHeaderSize + 8) {
        value.reason = static_cast<MatchLeaveReason>(
            std::uint8_t(input[12]));
        value.terminate_match = std::uint8_t(input[13]) != 0;
        if (std::uint8_t(input[14]) != 0 || std::uint8_t(input[15]) != 0)
            return MatchCodecError::BadFlags;
        if (!is_valid_match_leave_reason(value.reason))
            return MatchCodecError::InvalidReason;
    }
    if (const MatchCodecError error = validate_epoch(value.session_epoch);
        !match_codec_succeeded(error))
        return error;
    output = value;
    return MatchCodecError::None;
}

} // namespace kraken::net
