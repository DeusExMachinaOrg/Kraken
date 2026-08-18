#ifndef KRAKEN_NET_MATCH_SESSION_HPP
#define KRAKEN_NET_MATCH_SESSION_HPP

#include "net/entity_registry.hpp"
#include "net/net_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kraken::net {

inline constexpr std::size_t kMaxSessionPlayers = 64;
using MatchPlayerId = std::uint32_t;
inline constexpr MatchPlayerId kInvalidMatchPlayerId = 0;

enum class MatchState : std::uint8_t {
    Offline,
    Forming,
    Loading,
    Synchronizing,
    Playing,
    Leaving,
};

enum class JoinPolicy : std::uint8_t {
    ClosedAfterStart,
    JoinInProgress,
};

enum class MatchAction : std::uint8_t {
    None,
    BeginLoading,
};

struct MatchConfig {
    std::uint8_t required_players = 1;
    std::uint8_t max_players = static_cast<std::uint8_t>(kMaxSessionPlayers);
    JoinPolicy join_policy = JoinPolicy::ClosedAfterStart;
    std::optional<std::chrono::milliseconds> wait_timeout;
    std::string target_map;
    std::string exit_map;
    bool friendly_fire = false;
};

struct MatchPlayer {
    MatchPlayerId id = kInvalidMatchPlayerId;
    PeerId peer = kInvalidPeer;
    NetId entity_id = kInvalidNetId;
    std::uint32_t join_order = 0;
    bool host = false;
    bool ready = false;
    bool synchronized = false;
};

struct MatchSpawn {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    std::int32_t belong = 0;
};

struct MatchStatus {
    MatchState state = MatchState::Offline;
    std::uint8_t connected_players = 0;
    std::uint8_t ready_players = 0;
    std::uint8_t required_players = 1;
    bool infinite_wait = true;
    std::uint64_t remaining_wait_ms = 0;
    bool roster_locked = false;
};

class MatchCoordinator final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] bool start(MatchConfig config, MatchPlayerId host_id,
                             NetId host_entity_id, TimePoint now);
    [[nodiscard]] bool add_player(MatchPlayerId id, PeerId peer,
                                  NetId entity_id);
    [[nodiscard]] bool set_ready(MatchPlayerId id, bool ready = true);
    [[nodiscard]] bool remove_player(MatchPlayerId id);
    [[nodiscard]] MatchAction update(TimePoint now);
    [[nodiscard]] bool begin_synchronizing();
    [[nodiscard]] bool set_synchronized(MatchPlayerId id,
                                        bool synchronized = true);
    [[nodiscard]] bool begin_playing();
    [[nodiscard]] bool begin_leaving();
    void reset() noexcept;

    [[nodiscard]] bool add_spawn(MatchSpawn spawn);
    [[nodiscard]] std::optional<MatchSpawn> spawn_for(MatchPlayerId id) const;
    [[nodiscard]] bool can_join() const noexcept;
    [[nodiscard]] MatchStatus status(TimePoint now) const noexcept;
    [[nodiscard]] const MatchConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<MatchPlayer>& players() const noexcept
    { return players_; }
    [[nodiscard]] MatchState state() const noexcept { return state_; }

private:
    [[nodiscard]] MatchPlayer* find(MatchPlayerId id) noexcept;
    [[nodiscard]] const MatchPlayer* find(MatchPlayerId id) const noexcept;
    [[nodiscard]] std::size_t ready_count() const noexcept;
    void lock_ready_roster();

    MatchConfig config_{};
    MatchState state_ = MatchState::Offline;
    TimePoint forming_started_{};
    std::vector<MatchPlayer> players_;
    std::vector<MatchSpawn> spawns_;
    std::uint32_t next_join_order_ = 1;
    bool roster_locked_ = false;
};

[[nodiscard]] std::optional<JoinPolicy> parse_join_policy(
    const std::string& value) noexcept;
[[nodiscard]] const char* to_string(JoinPolicy policy) noexcept;
[[nodiscard]] const char* to_string(MatchState state) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_MATCH_SESSION_HPP
