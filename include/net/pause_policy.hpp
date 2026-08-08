#ifndef KRAKEN_NET_PAUSE_POLICY_HPP
#define KRAKEN_NET_PAUSE_POLICY_HPP

namespace kraken::net {

struct PauseSignals {
    bool session_active = false;
    bool application_paused = false;
    bool server_paused = false;
    bool cinematic = false;
};

// Network play never permits a local menu/focus pause to freeze the shared
// ODE world. Cinematics retain their native timing semantics.
[[nodiscard]] constexpr bool should_clear_network_pause(
    const PauseSignals signals) noexcept
{
    return signals.session_active && !signals.cinematic &&
           (signals.application_paused || signals.server_paused);
}

} // namespace kraken::net

#endif // KRAKEN_NET_PAUSE_POLICY_HPP
