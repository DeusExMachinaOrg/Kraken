#ifndef KRAKEN_NET_RUNTIME_HPP
#define KRAKEN_NET_RUNTIME_HPP

namespace kraken {
class Config;
}

namespace hta::ai { struct Vehicle; }

namespace kraken::net::runtime {

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
[[nodiscard]] bool IsSessionActive();

// Called at Vehicle::_KeepThrottle, after native controller polling and just
// before drivetrain consumption.  Returns true only for a live remote player
// vehicle controlled by the session host.
bool ApplyAuthoritativeRemoteInput(::hta::ai::Vehicle* vehicle);

} // namespace kraken::net::runtime

#endif
