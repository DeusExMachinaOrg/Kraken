#include "net/match_session.hpp"

#include <algorithm>
#include <limits>

namespace kraken::net {

bool MatchCoordinator::start(MatchConfig config, const MatchPlayerId host_id,
                             const NetId host_entity_id, const TimePoint now)
{
    if (state_ != MatchState::Offline || host_id == kInvalidMatchPlayerId ||
        host_entity_id == kInvalidNetId || config.required_players == 0 ||
        config.max_players == 0 || config.max_players > kMaxSessionPlayers ||
        config.required_players > config.max_players ||
        (config.wait_timeout && config.wait_timeout->count() <= 0) ||
        config.target_map.empty())
        return false;

    config_ = std::move(config);
    state_ = MatchState::Forming;
    forming_started_ = now;
    players_.clear();
    spawns_.clear();
    next_join_order_ = 1;
    roster_locked_ = false;
    players_.push_back({host_id, kInvalidPeer, host_entity_id,
                        next_join_order_++, true, true, false});
    return true;
}

bool MatchCoordinator::add_player(const MatchPlayerId id, const PeerId peer,
                                  const NetId entity_id)
{
    if (!can_join() || id == kInvalidMatchPlayerId ||
        peer == kInvalidPeer || entity_id == kInvalidNetId || find(id) ||
        players_.size() >= config_.max_players)
        return false;
    const auto duplicate = std::find_if(players_.begin(), players_.end(),
        [peer, entity_id](const MatchPlayer& player) {
            return player.peer == peer || player.entity_id == entity_id;
        });
    if (duplicate != players_.end())
        return false;
    players_.push_back({id, peer, entity_id, next_join_order_++, false,
                        false, false});
    return true;
}

bool MatchCoordinator::set_ready(const MatchPlayerId id, const bool ready)
{
    MatchPlayer* player = find(id);
    const bool forming = state_ == MatchState::Forming && !roster_locked_;
    const bool joining_live_session =
        state_ == MatchState::Playing &&
        config_.join_policy == JoinPolicy::JoinInProgress && !player->host;
    if (!player || (!forming && !joining_live_session))
        return false;
    player->ready = ready;
    return true;
}

bool MatchCoordinator::remove_player(const MatchPlayerId id)
{
    const auto found = std::find_if(players_.begin(), players_.end(),
        [id](const MatchPlayer& player) { return player.id == id; });
    if (found == players_.end() || found->host)
        return false;
    players_.erase(found);
    return true;
}

MatchAction MatchCoordinator::update(const TimePoint now)
{
    if (state_ != MatchState::Forming || roster_locked_)
        return MatchAction::None;
    const bool enough_players = ready_count() >= config_.required_players;
    const bool timed_out = config_.wait_timeout &&
        now - forming_started_ >= *config_.wait_timeout;
    if (!enough_players && !timed_out)
        return MatchAction::None;
    lock_ready_roster();
    state_ = MatchState::Loading;
    return MatchAction::BeginLoading;
}

bool MatchCoordinator::begin_synchronizing()
{
    if (state_ != MatchState::Loading)
        return false;
    state_ = MatchState::Synchronizing;
    return true;
}

bool MatchCoordinator::set_synchronized(const MatchPlayerId id,
                                        const bool synchronized)
{
    MatchPlayer* player = find(id);
    const bool synchronizing_roster = state_ == MatchState::Synchronizing;
    const bool joining_live_session =
        state_ == MatchState::Playing &&
        config_.join_policy == JoinPolicy::JoinInProgress && !player->host &&
        player->ready;
    if (!player || (!synchronizing_roster && !joining_live_session))
        return false;
    player->synchronized = synchronized;
    return true;
}

bool MatchCoordinator::begin_playing()
{
    if (state_ != MatchState::Synchronizing || players_.empty() ||
        std::any_of(players_.begin(), players_.end(),
            [](const MatchPlayer& player) { return !player.synchronized; }))
        return false;
    state_ = MatchState::Playing;
    return true;
}

bool MatchCoordinator::begin_leaving()
{
    if (state_ == MatchState::Offline || state_ == MatchState::Leaving)
        return false;
    state_ = MatchState::Leaving;
    roster_locked_ = true;
    return true;
}

void MatchCoordinator::reset() noexcept
{
    config_ = {};
    state_ = MatchState::Offline;
    forming_started_ = {};
    players_.clear();
    spawns_.clear();
    next_join_order_ = 1;
    roster_locked_ = false;
}

bool MatchCoordinator::add_spawn(MatchSpawn spawn)
{
    if (state_ != MatchState::Forming || roster_locked_ ||
        spawns_.size() >= config_.max_players)
        return false;
    spawns_.push_back(spawn);
    return true;
}

std::optional<MatchSpawn> MatchCoordinator::spawn_for(
    const MatchPlayerId id) const
{
    const MatchPlayer* player = find(id);
    if (!player)
        return std::nullopt;
    const auto ordered = std::count_if(players_.begin(), players_.end(),
        [player](const MatchPlayer& candidate) {
            return candidate.join_order < player->join_order;
        });
    if (ordered >= static_cast<std::ptrdiff_t>(spawns_.size()))
        return std::nullopt;
    return spawns_[static_cast<std::size_t>(ordered)];
}

bool MatchCoordinator::can_join() const noexcept
{
    if (players_.size() >= config_.max_players)
        return false;
    if (state_ == MatchState::Forming && !roster_locked_)
        return true;
    return state_ == MatchState::Playing &&
        config_.join_policy == JoinPolicy::JoinInProgress;
}

MatchStatus MatchCoordinator::status(const TimePoint now) const noexcept
{
    MatchStatus result;
    result.state = state_;
    result.connected_players = static_cast<std::uint8_t>(players_.size());
    result.ready_players = static_cast<std::uint8_t>(ready_count());
    result.required_players = config_.required_players;
    result.infinite_wait = !config_.wait_timeout.has_value();
    result.roster_locked = roster_locked_;
    if (state_ == MatchState::Forming && config_.wait_timeout) {
        const auto elapsed = now - forming_started_;
        if (elapsed < *config_.wait_timeout)
            result.remaining_wait_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *config_.wait_timeout - elapsed).count());
    }
    return result;
}

MatchPlayer* MatchCoordinator::find(const MatchPlayerId id) noexcept
{
    const auto found = std::find_if(players_.begin(), players_.end(),
        [id](const MatchPlayer& player) { return player.id == id; });
    return found == players_.end() ? nullptr : &*found;
}

const MatchPlayer* MatchCoordinator::find(const MatchPlayerId id) const noexcept
{
    const auto found = std::find_if(players_.begin(), players_.end(),
        [id](const MatchPlayer& player) { return player.id == id; });
    return found == players_.end() ? nullptr : &*found;
}

std::size_t MatchCoordinator::ready_count() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        players_.begin(), players_.end(),
        [](const MatchPlayer& player) { return player.ready; }));
}

void MatchCoordinator::lock_ready_roster()
{
    players_.erase(std::remove_if(players_.begin(), players_.end(),
        [](const MatchPlayer& player) { return !player.ready; }), players_.end());
    roster_locked_ = true;
}

std::optional<JoinPolicy> parse_join_policy(const std::string& value) noexcept
{
    if (value == "closed_after_start")
        return JoinPolicy::ClosedAfterStart;
    if (value == "join_in_progress")
        return JoinPolicy::JoinInProgress;
    return std::nullopt;
}

const char* to_string(const JoinPolicy policy) noexcept
{
    return policy == JoinPolicy::JoinInProgress
        ? "join_in_progress" : "closed_after_start";
}

const char* to_string(const MatchState state) noexcept
{
    switch (state) {
    case MatchState::Offline: return "offline";
    case MatchState::Forming: return "forming";
    case MatchState::Loading: return "loading";
    case MatchState::Synchronizing: return "synchronizing";
    case MatchState::Playing: return "playing";
    case MatchState::Leaving: return "leaving";
    }
    return "offline";
}

} // namespace kraken::net
