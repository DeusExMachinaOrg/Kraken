#include "net/match_session.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>

using namespace kraken::net;
using namespace std::chrono_literals;

namespace {

MatchConfig config(std::uint8_t required,
                   std::optional<std::chrono::milliseconds> timeout = std::nullopt)
{
    MatchConfig value;
    value.required_players = required;
    value.wait_timeout = timeout;
    value.target_map = "r1m1";
    return value;
}

void test_infinite_wait_and_required_count()
{
    MatchCoordinator match;
    const auto start = MatchCoordinator::TimePoint{};
    assert(match.start(config(3), 1, 1, start));
    const MatchStatus initial = match.status(start + 24h);
    assert(initial.state == MatchState::Forming);
    assert(initial.connected_players == 1);
    assert(initial.ready_players == 1);
    assert(initial.required_players == 3);
    assert(initial.infinite_wait);
    assert(initial.remaining_wait_ms == 0);
    assert(!initial.roster_locked);
    assert(match.update(start + 24h) == MatchAction::None);
    assert(match.add_player(2, 12, 2));
    assert(match.set_ready(2));
    assert(match.update(start + 48h) == MatchAction::None);
    const MatchStatus below_required = match.status(start + 48h);
    assert(below_required.state == MatchState::Forming);
    assert(below_required.connected_players == 2);
    assert(below_required.ready_players == 2);
    assert(below_required.required_players == 3);
    assert(below_required.infinite_wait);
    assert(below_required.remaining_wait_ms == 0);
    assert(!below_required.roster_locked);
    assert(match.add_player(3, 13, 3));
    assert(match.set_ready(3));
    assert(match.update(start + 48h) == MatchAction::BeginLoading);
    assert(match.state() == MatchState::Loading);
    assert(!match.can_join());
}

void test_required_count_starts_before_timeout()
{
    MatchCoordinator match;
    const auto start = MatchCoordinator::TimePoint{};
    assert(match.start(config(3, 30s), 1, 1, start));
    assert(match.add_player(2, 12, 2));
    assert(match.set_ready(2));
    assert(match.add_player(3, 13, 3));
    assert(match.set_ready(3));

    const MatchStatus before_deadline = match.status(start + 1s);
    assert(before_deadline.state == MatchState::Forming);
    assert(!before_deadline.infinite_wait);
    assert(before_deadline.connected_players == 3);
    assert(before_deadline.ready_players == 3);
    assert(before_deadline.required_players == 3);
    assert(before_deadline.remaining_wait_ms == 29000);
    assert(match.update(start + 1s) == MatchAction::BeginLoading);
    assert(match.state() == MatchState::Loading);
    assert(match.players().size() == 3);
    assert(match.status(start + 1s).roster_locked);
}

void test_timeout_starts_with_host_and_reports_remaining_time()
{
    MatchCoordinator match;
    const auto start = MatchCoordinator::TimePoint{};
    assert(match.start(config(4, 30s), 1, 1, start));
    const MatchStatus early = match.status(start + 1250ms);
    assert(early.state == MatchState::Forming);
    assert(early.connected_players == 1);
    assert(!early.infinite_wait);
    assert(early.ready_players == 1);
    assert(early.required_players == 4);
    assert(early.remaining_wait_ms == 28750);
    assert(!early.roster_locked);
    assert(match.update(start + 29999ms) == MatchAction::None);
    const MatchStatus at_deadline = match.status(start + 30s);
    assert(at_deadline.state == MatchState::Forming);
    assert(at_deadline.connected_players == 1);
    assert(at_deadline.ready_players == 1);
    assert(at_deadline.remaining_wait_ms == 0);
    assert(match.update(start + 30s) == MatchAction::BeginLoading);
    assert(match.players().size() == 1);
    assert(match.players().front().host);
    assert(match.players().front().ready);
    assert(match.status(start + 30s).roster_locked);
}

void test_timeout_excludes_unready_and_disconnects_cleanly()
{
    MatchCoordinator match;
    const auto start = MatchCoordinator::TimePoint{};
    assert(match.start(config(4, 5s), 1, 1, start));
    assert(match.add_player(2, 22, 2));
    assert(match.add_player(3, 23, 3));
    assert(match.add_player(4, 24, 4));
    assert(match.set_ready(2));
    assert(match.set_ready(3));
    // A peer with no ready acknowledgement represents an incomplete forming
    // handshake.  Synchronization is not accepted until the load barrier.
    assert(!match.set_synchronized(4));
    assert(!match.players().back().ready);
    assert(!match.players().back().synchronized);
    assert(match.update(start + 4s) == MatchAction::None);
    assert(match.remove_player(2));
    assert(match.update(start + 5s) == MatchAction::BeginLoading);
    assert(match.players().size() == 2);
    assert(match.players().front().id == 1);
    assert(match.players().front().ready);
    assert(match.players().back().id == 3);
    assert(match.players().back().ready);
    const MatchStatus locked = match.status(start + 5s);
    assert(locked.connected_players == 2);
    assert(locked.ready_players == 2);
    assert(locked.roster_locked);
}

void test_join_in_progress_and_sync_barrier()
{
    MatchCoordinator match;
    MatchConfig value = config(1);
    value.join_policy = JoinPolicy::JoinInProgress;
    const auto start = MatchCoordinator::TimePoint{};
    assert(match.start(value, 1, 1, start));
    assert(match.update(start) == MatchAction::BeginLoading);
    assert(match.begin_synchronizing());
    assert(!match.begin_playing());
    assert(match.set_synchronized(1));
    assert(match.begin_playing());
    assert(match.can_join());
    assert(match.add_player(2, 42, 2));
    assert(!match.set_synchronized(2));
    assert(match.set_ready(2));
    assert(match.set_synchronized(2));
    assert(match.players().back().ready);
    assert(match.players().back().synchronized);
}

void test_64_player_limit_and_spawn_order()
{
    MatchCoordinator match;
    MatchConfig value = config(64);
    const auto start = MatchCoordinator::TimePoint{};
    assert(match.start(value, 1, 1, start));
    assert(match.add_spawn({1, 2, 3, 4, 1091}));
    for (MatchPlayerId id = 2; id <= 64; ++id) {
        assert(match.add_player(id, id + 100, id));
        assert(match.set_ready(id));
    }
    assert(match.players().size() == kMaxSessionPlayers);
    assert(!match.add_player(65, 165, 65));
    const auto host_spawn = match.spawn_for(1);
    assert(host_spawn && host_spawn->belong == 1091);
    assert(!match.spawn_for(2).has_value());
    assert(match.update(start) == MatchAction::BeginLoading);
}

void test_policy_parser()
{
    assert(parse_join_policy("closed_after_start") ==
           JoinPolicy::ClosedAfterStart);
    assert(parse_join_policy("join_in_progress") == JoinPolicy::JoinInProgress);
    assert(!parse_join_policy("open").has_value());
}

} // namespace

int main()
{
    test_infinite_wait_and_required_count();
    test_required_count_starts_before_timeout();
    test_timeout_starts_with_host_and_reports_remaining_time();
    test_timeout_excludes_unready_and_disconnects_cleanly();
    test_join_in_progress_and_sync_barrier();
    test_64_player_limit_and_spawn_order();
    test_policy_parser();
    return 0;
}
