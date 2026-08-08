#ifndef KRAKEN_NET_RUNTIME_HPP
#define KRAKEN_NET_RUNTIME_HPP

namespace kraken {
class Config;
}

namespace kraken::net::runtime {

// Starts the listen-server/client bootstrap and installs the main-thread pump.
// Does nothing when [multiplayer] enabled=0.
void Apply(const Config* config);

// Mod-facing native seam.  This transmits intent; it never creates a local
// projectile.  The host validates and executes the matching original engine
// weapon call in its ODE world.
bool SubmitLocalWeaponCommand(int gun_id, bool trigger_held);

} // namespace kraken::net::runtime

#endif
