#ifndef KRAKEN_NET_RUNTIME_HPP
#define KRAKEN_NET_RUNTIME_HPP

#include "net/entity_registry.hpp"
#include "net/match_session.hpp"
#include "net/world_loot.hpp"
#include "net/world_authority.hpp"

#include <cstdint>

namespace kraken {
class Config;
}

namespace hta::ai { struct Vehicle; }

namespace kraken::net::runtime {

enum class SessionLeaveReason : std::uint8_t {
    User = 0,
    Death = 1,
    Extract = 2,
    HostTerminated = 3,
    MapUnload = 4,
};

namespace detail {

// MatchCoordinator is authoritative on the host; replicas receive the same
// lifecycle through visible_match_state as reliable match messages arrive.
// Forming is deliberately treated as Loading so native map/static setup is
// never filtered before the active-world barrier opens.
[[nodiscard]] constexpr world_authority::WorldExecutionPhase
world_phase_for_match_state(const MatchState state) noexcept
{
    switch (state) {
    case MatchState::Offline:
        return world_authority::WorldExecutionPhase::Offline;
    case MatchState::Forming:
    case MatchState::Loading:
        return world_authority::WorldExecutionPhase::Loading;
    case MatchState::Synchronizing:
        return world_authority::WorldExecutionPhase::Synchronizing;
    case MatchState::Playing:
        return world_authority::WorldExecutionPhase::Playing;
    case MatchState::Leaving:
        return world_authority::WorldExecutionPhase::Teardown;
    default:
        return world_authority::WorldExecutionPhase::Unknown;
    }
}

[[nodiscard]] constexpr world_authority::WorldExecutionContext
derive_world_execution_context(const bool session_active, const bool is_host,
                               const MatchState host_state,
                               const MatchState replica_state,
                               const bool replay = false,
                               const bool presentation = false) noexcept
{
    const world_authority::RuntimeAuthority authority = !session_active
        ? world_authority::RuntimeAuthority::Offline
        : (is_host ? world_authority::RuntimeAuthority::Host
                   : world_authority::RuntimeAuthority::Replica);
    if (presentation)
        return {world_authority::WorldExecutionPhase::Presentation, authority};
    if (replay)
        return {world_authority::WorldExecutionPhase::Replay, authority};
    if (!session_active)
        return {world_authority::WorldExecutionPhase::Offline, authority};
    return {world_phase_for_match_state(is_host ? host_state : replica_state),
            authority};
}

} // namespace detail

// Starts the listen-server/client bootstrap and installs the main-thread pump.
// Does nothing when [multiplayer] enabled=0.
void Apply(const Config* config);

// Mod-facing native seam.  This transmits intent; it never creates a local
// projectile.  The host validates and executes the matching original engine
// weapon call in its ODE world.
bool SubmitLocalWeaponCommand(int gun_id, bool trigger_held);
// Selects the LAN listen-server or client endpoint before BeginSession().
// Keeping this separate from session start lets Lua menus configure a raid
// without changing process environment variables.
bool ConfigureSession(bool host, const char* address, unsigned short port,
                      unsigned int max_peers);
bool BeginSession();
bool EndSession();
// Starts the generic host-authoritative match controller.  A client may call
// this before connecting; the request is retained and its Ready is sent after
// the transport handshake completes.
bool StartMatchmaking(std::uint8_t required_players, const char* target_map,
                      const char* exit_map, std::int32_t wait_timeout_seconds,
                      bool friendly_fire);
bool AddSpawn(float x, float y, float z, float yaw, std::int32_t belong);
[[nodiscard]] const char* GetSessionState();
[[nodiscard]] MatchStatus GetSessionStatus();
bool LeaveSession(SessionLeaveReason reason = SessionLeaveReason::User);
[[nodiscard]] bool IsSessionActive();
[[nodiscard]] bool IsHost();
[[nodiscard]] bool IsAuthority();
// True while offline or on the listen-server authority.  Mods can use this
// as a generic host guard without encoding a raid-specific topology.
[[nodiscard]] bool IsAuthorityOrOffline();
[[nodiscard]] NetId LocalEntityId();

// Generic shared-world loot seam.  The host owns the engine repository; a
// client only publishes a pickup intent and reads presentation records.
[[nodiscard]] WorldLootId PublishHostWorldLoot(
    std::int32_t container_prototype_id, std::int32_t item_prototype_id,
    std::uint32_t amount, NetId owner_entity_id = kInvalidNetId);
[[nodiscard]] WorldLootId PublishHostWorldLootObject(
    std::int32_t object_id, std::int32_t item_prototype_id,
    std::uint32_t amount, NetId owner_entity_id = kInvalidNetId);
[[nodiscard]] bool RequestWorldLootPickup(
    WorldLootId loot_id, WorldLootGeneration generation,
    std::uint32_t transaction_id, std::uint32_t amount);
[[nodiscard]] bool QueryWorldLootAuthority();

// Registers a host-created vehicle for generic publication.  The returned
// NetId is stable for the lifetime of the host object, or zero on rejection.
// The native runtime remains the only publisher; clients never call this.
[[nodiscard]] NetId PublishHostEntity(std::int32_t object_id,
                                      std::int32_t kind);

// Called at Vehicle::_KeepThrottle, after native controller polling and just
// before drivetrain consumption.  Returns true only for a live remote player
// vehicle controlled by the session host.
bool ApplyAuthoritativeRemoteInput(::hta::ai::Vehicle* vehicle);

} // namespace kraken::net::runtime

#endif
