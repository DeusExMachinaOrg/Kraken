#include "net/lan_discovery.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

int main()
{
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
    return 0;
}
