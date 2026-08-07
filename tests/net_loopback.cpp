#include "net/session.hpp"
#include "net/transport.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool pump(kraken::net::Session& session)
{
    const kraken::net::TransportResult result = session.pump();
    return result.succeeded();
}

} // namespace

int main()
{
    using namespace kraken::net;

    constexpr std::uint16_t port = 39051;

    EnetTransport host_transport;
    SessionConfig host_config{};
    host_config.role = SessionRole::Server;
    host_config.transport.bind_endpoint = {"127.0.0.1", port};
    host_config.transport.max_peers = 2;
    Session host(host_transport, host_config);

    EnetTransport client_transport;
    SessionConfig client_config{};
    client_config.role = SessionRole::Client;
    Session client(client_transport, client_config);

    if (!host.start() || !client.start() ||
        !client.connect(Endpoint{"127.0.0.1", port}))
        return 1;

    PeerId client_peer = kInvalidPeer;
    bool host_connected = false;
    bool client_connected = false;
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + 5s;

    std::array<SessionEvent, 16> events{};
    while (std::chrono::steady_clock::now() < handshake_deadline &&
           (!host_connected || !client_connected)) {
        if (!pump(host) || !pump(client))
            return 2;

        std::size_t count = host.drain_events(events);
        for (std::size_t index = 0; index < count; ++index)
            host_connected |= events[index].type ==
                              SessionEventType::PeerConnected;

        count = client.drain_events(events);
        for (std::size_t index = 0; index < count; ++index) {
            if (events[index].type == SessionEventType::PeerConnected) {
                client_connected = true;
                client_peer = events[index].peer;
            }
        }
        std::this_thread::sleep_for(1ms);
    }

    if (!host_connected || !client_connected || client_peer == kInvalidPeer)
        return 3;
    if (!client.ping(client_peer))
        return 4;

    bool received_rtt = false;
    const auto ping_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < ping_deadline && !received_rtt) {
        if (!pump(host) || !pump(client))
            return 5;
        (void)host.drain_events(events);
        const std::size_t count = client.drain_events(events);
        for (std::size_t index = 0; index < count; ++index)
            received_rtt |= events[index].type ==
                            SessionEventType::RoundTripTime;
        std::this_thread::sleep_for(1ms);
    }

    client.stop();
    host.stop();
    return received_rtt ? 0 : 6;
}
