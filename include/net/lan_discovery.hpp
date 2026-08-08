#ifndef KRAKEN_NET_LAN_DISCOVERY_HPP
#define KRAKEN_NET_LAN_DISCOVERY_HPP

#include "net/transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kraken::net {

enum class LanSessionRole : std::uint8_t {
    None,
    Host,
    Client,
};

struct LanSessionSelection {
    LanSessionRole role = LanSessionRole::None;
    std::optional<Endpoint> host;
};

// A LAN broadcast reply always wins over the ability to host locally: unlike
// TCP, every machine on a LAN can bind the same UDP port independently.
[[nodiscard]] inline LanSessionSelection select_lan_session(
    const std::optional<Endpoint>& discovered_host, const bool can_host) noexcept
{
    if (discovered_host)
        return {LanSessionRole::Client, discovered_host};
    return {can_host ? LanSessionRole::Host : LanSessionRole::None, std::nullopt};
}

// Hosts answer small UDP broadcasts with their game endpoint.
class LanDiscovery final {
public:
    LanDiscovery() = default;
    ~LanDiscovery();

    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;

    [[nodiscard]] bool become_host(std::uint16_t discovery_port,
                                   std::uint16_t game_port) noexcept;
    [[nodiscard]] static std::optional<Endpoint> discover(
        std::uint16_t discovery_port,
        std::chrono::milliseconds timeout,
        std::string_view target_address = "255.255.255.255") noexcept;
    void pump() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool hosting() const noexcept;

private:
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
    std::uint16_t game_port_ = 0;
};

} // namespace kraken::net

#endif // KRAKEN_NET_LAN_DISCOVERY_HPP
