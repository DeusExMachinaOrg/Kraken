#ifndef KRAKEN_NET_MATCH_PROTOCOL_HPP
#define KRAKEN_NET_MATCH_PROTOCOL_HPP

#include "net/entity_registry.hpp"
#include "net/match_session.hpp"
#include "net/net_types.hpp"
#include "net/world_replication.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kraken::net {

// Match control is deliberately a data protocol.  The transport MessageType
// selects the operation, while every payload carries its own magic/version so
// malformed or stale packets cannot be mistaken for a valid control event.
inline constexpr std::uint32_t kMatchProtocolMagic = 0x314D504Du; // MPM1
inline constexpr std::uint16_t kMatchProtocolVersion = 1;
inline constexpr std::size_t kMaxMatchMapName = 1024;
inline constexpr std::size_t kMaxMatchRosterEntries = kMaxSessionPlayers;

struct MatchReadyRequest {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    // A legacy 16-byte request decodes as false.  Hosts set this only when a
    // peer is joining an already Playing match and must load the active map
    // before any world/entity baseline is released.
    bool join_in_progress = false;
};

struct MatchReady {
    std::uint32_t session_epoch = 0;
    MatchPlayerId player_id = kInvalidMatchPlayerId;
    NetId entity_id = kInvalidNetId;
    bool ready = true;
};

enum class MatchRejectReason : std::uint8_t {
    Unknown = 0,
    ClosedAfterStart = 1,
    RosterFull = 2,
    InvalidRequest = 3,
    NotReady = 4,
};

struct MatchReject {
    std::uint32_t session_epoch = 0;
    MatchRejectReason reason = MatchRejectReason::Unknown;
};

struct MatchRosterPlayer {
    MatchPlayerId player_id = kInvalidMatchPlayerId;
    PeerId peer = kInvalidPeer;
    NetId entity_id = kInvalidNetId;
    std::uint32_t join_order = 0;
    bool host = false;
    bool ready = false;
};

struct MatchRosterLock {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    std::uint8_t required_players = 1;
    std::uint8_t max_players = static_cast<std::uint8_t>(kMaxSessionPlayers);
    JoinPolicy join_policy = JoinPolicy::ClosedAfterStart;
    bool friendly_fire = false;
    std::vector<MatchRosterPlayer> players;
    std::vector<MatchSpawn> spawns;
};

struct MatchLoad {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    std::string target_map;
    std::string exit_map;
    bool friendly_fire = false;
};

// Sent by a client only after MatchLoad's target map is observable through the
// native Level boundary.  The host validates the transport peer separately;
// these identities make stale/replayed acknowledgements harmless.
struct MatchMapReady {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    MatchPlayerId player_id = kInvalidMatchPlayerId;
    NetId entity_id = kInvalidNetId;
};

struct MatchSync {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    MatchPlayerId player_id = kInvalidMatchPlayerId;
    // WorldRevision is intentionally 64-bit.  Revision zero is the valid
    // initial empty snapshot and stale/gap checks belong to WorldJoinBarrier.
    WorldRevision snapshot_revision = kInvalidWorldRevision;
    bool request = true;
};

struct MatchPlay {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
};

enum class MatchLeaveReason : std::uint8_t {
    User = 0,
    Death = 1,
    Extract = 2,
    HostTerminated = 3,
    MapUnload = 4,
};

[[nodiscard]] constexpr bool is_valid_match_leave_reason(
    const MatchLeaveReason reason) noexcept
{
    return static_cast<std::uint8_t>(reason) <=
           static_cast<std::uint8_t>(MatchLeaveReason::MapUnload);
}

struct MatchLeave {
    std::uint32_t session_epoch = 0;
    MatchLeaveReason reason = MatchLeaveReason::User;
    bool terminate_match = false;
};

enum class MatchJipBarrierState : std::uint8_t {
    Idle,
    MapLoadSent,
    MapReady,
    SnapshotStarted,
};

enum class MatchJipBarrierResult : std::uint8_t {
    Accepted,
    Duplicate,
    Stale,
    WrongIdentity,
    NotAwaiting,
};

struct MatchJipBarrier {
    std::uint32_t session_epoch = 0;
    std::uint32_t roster_revision = 0;
    MatchPlayerId player_id = kInvalidMatchPlayerId;
    NetId entity_id = kInvalidNetId;
    MatchJipBarrierState state = MatchJipBarrierState::Idle;
};

[[nodiscard]] bool begin_match_jip_map_load(
    MatchJipBarrier&, std::uint32_t session_epoch,
    std::uint32_t roster_revision, MatchPlayerId player_id,
    NetId entity_id) noexcept;
[[nodiscard]] MatchJipBarrierResult accept_match_jip_map_ready(
    MatchJipBarrier&, const MatchMapReady&) noexcept;
[[nodiscard]] constexpr bool match_jip_snapshot_permitted(
    const MatchJipBarrier& barrier) noexcept
{
    return barrier.state == MatchJipBarrierState::MapReady ||
           barrier.state == MatchJipBarrierState::SnapshotStarted;
}
[[nodiscard]] bool mark_match_jip_snapshot_started(
    MatchJipBarrier&) noexcept;

enum class MatchCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidEpoch,
    InvalidPlayer,
    InvalidEntity,
    InvalidRevision,
    InvalidReason,
    InvalidPolicy,
    InvalidRoster,
    TooManyPlayers,
    TooManySpawns,
    InvalidSpawn,
    InvalidMap,
    MapTooLong,
};

[[nodiscard]] constexpr bool match_codec_succeeded(
    MatchCodecError error) noexcept
{
    return error == MatchCodecError::None;
}

[[nodiscard]] MatchCodecError encode_match_ready_request(
    const MatchReadyRequest&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_ready_request(
    ByteView, MatchReadyRequest&) noexcept;
[[nodiscard]] MatchCodecError encode_match_ready(
    const MatchReady&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_ready(ByteView, MatchReady&) noexcept;
[[nodiscard]] MatchCodecError encode_match_reject(
    const MatchReject&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_reject(ByteView, MatchReject&) noexcept;
[[nodiscard]] MatchCodecError encode_match_roster_lock(
    const MatchRosterLock&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_roster_lock(
    ByteView, MatchRosterLock&);
[[nodiscard]] MatchCodecError encode_match_load(
    const MatchLoad&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_load(ByteView, MatchLoad&);
[[nodiscard]] MatchCodecError encode_match_map_ready(
    const MatchMapReady&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_map_ready(
    ByteView, MatchMapReady&) noexcept;
[[nodiscard]] MatchCodecError encode_match_sync(
    const MatchSync&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_sync(ByteView, MatchSync&) noexcept;
[[nodiscard]] MatchCodecError encode_match_play(
    const MatchPlay&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_play(ByteView, MatchPlay&) noexcept;
[[nodiscard]] MatchCodecError encode_match_leave(
    const MatchLeave&, std::vector<Byte>&);
[[nodiscard]] MatchCodecError decode_match_leave(ByteView, MatchLeave&) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_MATCH_PROTOCOL_HPP
