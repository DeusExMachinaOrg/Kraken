#include "net/lan_discovery.hpp"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main()
{
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::cerr << "could not initialize Winsock for LAN target test\n";
        return 19;
    }

    // Regression for the two-PC failure observed on 192.168.2.0/24: a global
    // 255.255.255.255 packet may be routed to a VPN/virtual NIC by Windows.
    // The production policy must also target the LAN's directed broadcast.
    const std::vector<kraken::net::LanIpv4Adapter> adapters{
        {"192.168.2.118", "255.255.255.0"},
        {"10.10.0.7", "255.255.0.0"},
    };
    const auto targets = kraken::net::make_lan_discovery_targets(
        27016, "255.255.255.255", adapters);
    const auto has_target = [&targets](const char* host) {
        for (const kraken::net::Endpoint& endpoint : targets)
            if (endpoint.host == host && endpoint.port == 27016)
                return true;
        return false;
    };
    if (!has_target("255.255.255.255") || !has_target("192.168.2.255") ||
        !has_target("10.10.255.255") || targets.size() != 3) {
        std::cerr << "LAN discovery did not fan out to directed broadcasts\n";
        return 20;
    }
    const auto explicit_target = kraken::net::make_lan_discovery_targets(
        27016, "192.168.2.80", adapters);
    if (explicit_target.size() != 1 || explicit_target.front().host != "192.168.2.80" ||
        explicit_target.front().port != 27016) {
        std::cerr << "explicit LAN discovery target was unexpectedly expanded\n";
        return 21;
    }

    // Regression: on separate LAN computers both can bind UDP/27016.  A reply
    // from an existing host must therefore force the second peer to be client.
    const kraken::net::Endpoint existing_host{"192.168.2.118", 27015};
    const auto second_peer = kraken::net::select_lan_session(existing_host, true);
    if (second_peer.role != kraken::net::LanSessionRole::Client ||
        !second_peer.host || second_peer.host->host != existing_host.host ||
        second_peer.host->port != existing_host.port) {
        std::cerr << "existing LAN host was not preferred over local UDP bind\n";
        return 10;
    }
    if (kraken::net::select_lan_session(std::nullopt, true).role !=
            kraken::net::LanSessionRole::Host ||
        kraken::net::select_lan_session(std::nullopt, false).role !=
            kraken::net::LanSessionRole::None) {
        std::cerr << "LAN host election fallback is invalid\n";
        return 11;
    }

    constexpr std::uint16_t discovery_port = 28916;
    constexpr std::uint16_t game_port = 28915;
    kraken::net::LanDiscovery host;
    if (!host.become_host(discovery_port, game_port)) {
        std::cerr << "could not bind LAN discovery host\n";
        return 1;
    }
    std::atomic<bool> pumping = true;
    std::thread host_thread([&] {
        while (pumping.load()) {
            host.pump();
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto endpoint = kraken::net::LanDiscovery::discover(
        discovery_port, 1000ms, "127.0.0.1");
    pumping = false;
    host_thread.join();
    host.stop();
    if (!endpoint || endpoint->host != "127.0.0.1" || endpoint->port != game_port) {
        std::cerr << "LAN discovery response invalid\n";
        return 2;
    }
    std::cout << "LAN discovery tests passed\n";
    WSACleanup();
    return 0;
}
