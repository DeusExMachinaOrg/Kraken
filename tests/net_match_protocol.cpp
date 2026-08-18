#include "net/match_protocol.hpp"
#include "net/wire_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>

using namespace kraken::net;

namespace {

void test_reliable_match_message_types()
{
    for (const MessageType type : {MessageType::MatchReadyRequest,
                                   MessageType::MatchReady,
                                   MessageType::MatchReject,
                                   MessageType::MatchRosterLock,
                                   MessageType::MatchLoad,
                                   MessageType::MatchSync,
                                   MessageType::MatchPlay,
                                   MessageType::MatchLeave,
                                   MessageType::MatchMapReady}) {
        assert(is_valid_message_type(type));
        assert(requires_reliable_channel(type));
    }
}

void test_ready_and_sync_round_trip()
{
    std::vector<Byte> bytes;
    MatchReadyRequest request{9, 3, true};
    assert(encode_match_ready_request(request, bytes) ==
           MatchCodecError::None);
    MatchReadyRequest decoded_request{};
    assert(decode_match_ready_request(bytes, decoded_request) ==
           MatchCodecError::None);
    assert(decoded_request.session_epoch == request.session_epoch);
    assert(decoded_request.roster_revision == request.roster_revision);
    assert(decoded_request.join_in_progress);

    // The legacy short request remains a Forming request.
    request.join_in_progress = false;
    assert(encode_match_ready_request(request, bytes) ==
           MatchCodecError::None);
    assert(bytes.size() == 16);
    assert(decode_match_ready_request(bytes, decoded_request) ==
           MatchCodecError::None);
    assert(!decoded_request.join_in_progress);

    MatchReady expected{9, 7, 11, true};
    assert(encode_match_ready(expected, bytes) == MatchCodecError::None);
    MatchReady actual{};
    assert(decode_match_ready(bytes, actual) == MatchCodecError::None);
    assert(actual.session_epoch == expected.session_epoch);
    assert(actual.player_id == expected.player_id);
    assert(actual.entity_id == expected.entity_id);
    assert(actual.ready);

    MatchSync sync{9, 3, 7, 12, true};
    assert(encode_match_sync(sync, bytes) == MatchCodecError::None);
    MatchSync decoded{};
    assert(decode_match_sync(bytes, decoded) == MatchCodecError::None);
    assert(decoded.snapshot_revision == sync.snapshot_revision);
    assert(decoded.request == sync.request);

    MatchSync initial{9, 3, 7, 0, false};
    assert(encode_match_sync(initial, bytes) == MatchCodecError::None);
    assert(decode_match_sync(bytes, decoded) == MatchCodecError::None);
    assert(decoded.snapshot_revision == 0);

    assert(encode_match_sync(initial, bytes) == MatchCodecError::None);
    bytes[0] = Byte{0};
    assert(decode_match_sync(bytes, decoded) == MatchCodecError::BadMagic);

    MatchMapReady map_ready{9, 3, 7, 11};
    assert(encode_match_map_ready(map_ready, bytes) ==
           MatchCodecError::None);
    MatchMapReady decoded_map_ready{};
    assert(decode_match_map_ready(bytes, decoded_map_ready) ==
           MatchCodecError::None);
    assert(decoded_map_ready.session_epoch == map_ready.session_epoch);
    assert(decoded_map_ready.roster_revision == map_ready.roster_revision);
    assert(decoded_map_ready.player_id == map_ready.player_id);
    assert(decoded_map_ready.entity_id == map_ready.entity_id);
}

void test_jip_map_barrier()
{
    const MatchMapReady valid{9, 3, 7, 11};
    MatchJipBarrier not_loaded{9, 3, 7, 11,
                               MatchJipBarrierState::Idle};
    assert(accept_match_jip_map_ready(not_loaded, valid) ==
           MatchJipBarrierResult::NotAwaiting);
    assert(!match_jip_snapshot_permitted(not_loaded));

    MatchJipBarrier barrier{};
    assert(!match_jip_snapshot_permitted(barrier));
    assert(begin_match_jip_map_load(barrier, 9, 3, 7, 11));
    assert(!match_jip_snapshot_permitted(barrier));

    const MatchMapReady stale{8, 3, 7, 11};
    assert(accept_match_jip_map_ready(barrier, stale) ==
           MatchJipBarrierResult::Stale);
    assert(!match_jip_snapshot_permitted(barrier));

    const MatchMapReady wrong_peer_identity{9, 3, 8, 11};
    assert(accept_match_jip_map_ready(barrier, wrong_peer_identity) ==
           MatchJipBarrierResult::WrongIdentity);
    assert(!match_jip_snapshot_permitted(barrier));

    assert(accept_match_jip_map_ready(barrier, valid) ==
           MatchJipBarrierResult::Accepted);
    assert(match_jip_snapshot_permitted(barrier));
    assert(accept_match_jip_map_ready(barrier, valid) ==
           MatchJipBarrierResult::Duplicate);
    assert(mark_match_jip_snapshot_started(barrier));
    assert(match_jip_snapshot_permitted(barrier));
    assert(accept_match_jip_map_ready(barrier, valid) ==
           MatchJipBarrierResult::Duplicate);
    assert(!mark_match_jip_snapshot_started(barrier));

    // The forming wire shape remains closed-after-start by default; a JIP
    // barrier never changes the roster policy carried by the lock.
    MatchRosterLock closed_roster{};
    closed_roster.session_epoch = 9;
    closed_roster.roster_revision = 3;
    closed_roster.required_players = 1;
    closed_roster.max_players = 64;
    closed_roster.join_policy = JoinPolicy::ClosedAfterStart;
    closed_roster.players = {{1, kInvalidPeer, 1, 1, true, true}};
    std::vector<Byte> bytes;
    assert(encode_match_roster_lock(closed_roster, bytes) ==
           MatchCodecError::None);
    MatchRosterLock decoded_closed_roster{};
    assert(decode_match_roster_lock(bytes, decoded_closed_roster) ==
           MatchCodecError::None);
    assert(decoded_closed_roster.join_policy == JoinPolicy::ClosedAfterStart);
}

void test_roster_and_load_round_trip()
{
    MatchRosterLock expected{};
    expected.session_epoch = 4;
    expected.roster_revision = 8;
    expected.required_players = 2;
    expected.max_players = 64;
    expected.join_policy = JoinPolicy::JoinInProgress;
    expected.friendly_fire = true;
    expected.players = {{1, kInvalidPeer, 1, 1, true, true},
                        {2, 12, 2, 2, false, true}};
    expected.spawns = {{1.0f, 2.0f, 3.0f, 4.0f, 1100}};

    std::vector<Byte> bytes;
    assert(encode_match_roster_lock(expected, bytes) == MatchCodecError::None);
    MatchRosterLock actual{};
    assert(decode_match_roster_lock(bytes, actual) == MatchCodecError::None);
    assert(actual.max_players == 64);
    assert(actual.join_policy == JoinPolicy::JoinInProgress);
    assert(actual.players.size() == 2);
    assert(actual.players[1].peer == 12);
    assert(actual.spawns[0].belong == 1100);

    MatchLoad load{4, 8, "coop_map", "shelter", true};
    assert(encode_match_load(load, bytes) == MatchCodecError::None);
    MatchLoad decoded{};
    assert(decode_match_load(bytes, decoded) == MatchCodecError::None);
    assert(decoded.target_map == load.target_map);
    assert(decoded.exit_map == load.exit_map);
    assert(decoded.friendly_fire);

    load.target_map.clear();
    assert(encode_match_load(load, bytes) == MatchCodecError::InvalidMap);
    expected.spawns[0].yaw = std::nanf("");
    assert(encode_match_roster_lock(expected, bytes) ==
           MatchCodecError::InvalidSpawn);
}

void test_reason_bearing_leave_round_trip()
{
    std::vector<Byte> bytes;
    const MatchLeave expected{9, MatchLeaveReason::HostTerminated, true};
    assert(encode_match_leave(expected, bytes) == MatchCodecError::None);
    MatchLeave actual{};
    assert(decode_match_leave(bytes, actual) == MatchCodecError::None);
    assert(actual.session_epoch == expected.session_epoch);
    assert(actual.reason == MatchLeaveReason::HostTerminated);
    assert(actual.terminate_match);

    // Preserve compatibility with the four-byte leave emitted by older
    // clients, whose only meaningful interpretation is an ordinary leave.
    bytes.resize(12);
    assert(decode_match_leave(bytes, actual) == MatchCodecError::None);
    assert(actual.reason == MatchLeaveReason::User);
    assert(!actual.terminate_match);
}

} // namespace

int main()
{
    test_reliable_match_message_types();
    test_ready_and_sync_round_trip();
    test_jip_map_barrier();
    test_roster_and_load_round_trip();
    test_reason_bearing_leave_round_trip();
    return 0;
}
