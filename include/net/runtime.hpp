#ifndef KRAKEN_NET_RUNTIME_HPP
#define KRAKEN_NET_RUNTIME_HPP

namespace kraken {
class Config;
}

namespace kraken::net::runtime {

// Starts the listen-server/client bootstrap and installs the main-thread pump.
// Does nothing when [multiplayer] enabled=0.
void Apply(const Config* config);

} // namespace kraken::net::runtime

#endif
