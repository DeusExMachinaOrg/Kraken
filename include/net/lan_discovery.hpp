#ifndef KRAKEN_NET_LAN_DISCOVERY_HPP
#define KRAKEN_NET_LAN_DISCOVERY_HPP

#include "net/transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kraken::net {

// The first peer to bind the discovery port becomes the LAN listen server.
// Later peers receive the host's endpoint through a small UDP broadcast reply.
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
