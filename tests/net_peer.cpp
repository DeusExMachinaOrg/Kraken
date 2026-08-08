#include "net/session.hpp"
#include "net/transport.hpp"
#include "net/vehicle_snapshot.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    using namespace kraken::net;

    if (argc < 2 || (std::string(argv[1]) != "host" &&
                     std::string(argv[1]) != "client")) {
        std::cerr << "usage: kraken_net_peer_test <host|client> [port] [address] [--scripted-snapshots]\n";
        return 64;
    }

    const bool is_host = std::string(argv[1]) == "host";
    const std::uint16_t port = argc >= 3
        ? static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10))
        : kDefaultPort;
    const std::string address = argc >= 4 ? argv[3] : "127.0.0.1";
    const bool scripted_snapshots = argc >= 5 &&
        std::string(argv[4]) == "--scripted-snapshots";

    EnetTransport transport;
    SessionConfig config{};
    config.role = is_host ? SessionRole::Server : SessionRole::Client;
    config.transport.bind_endpoint = {is_host ? "0.0.0.0" : "127.0.0.1", port};
    config.transport.max_peers = 16;
    Session session(transport, config);

    if (!session.start()) {
        std::cerr << "start failed\n";
        return 1;
    }
    if (!is_host && !session.connect(Endpoint{address, port})) {
        std::cerr << "connect failed\n";
        return 2;
    }

    std::vector<PeerId> peers;
    bool connected = false;
    bool received_rtt = false;
    unsigned rtt_samples = 0;
    unsigned snapshot_samples = 0;
    auto next_ping = std::chrono::steady_clock::now();
    auto next_snapshot = std::chrono::steady_clock::now();
    std::uint32_t snapshot_sequence = 1;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    std::array<SessionEvent, 32> events{};

    std::cout << (is_host ? "listening" : "connecting") << " port=" << port
              << " address=" << address << std::endl;

    while (std::chrono::steady_clock::now() < deadline) {
        const TransportResult result = session.pump();
        if (!result && result.code != TransportResultCode::WouldBlock) {
            std::cerr << "pump failed code=" << static_cast<unsigned>(result.code) << '\n';
            return 3;
        }

        const std::size_t count = session.drain_events(events);
        for (std::size_t index = 0; index < count; ++index) {
            const SessionEvent& event = events[index];
            switch (event.type) {
            case SessionEventType::PeerConnected:
                connected = true;
                peers.push_back(event.peer);
                std::cout << "connected peer=" << event.peer << std::endl;
                break;
            case SessionEventType::PeerDisconnected:
                std::cout << "disconnected peer=" << event.peer << std::endl;
                break;
            case SessionEventType::RoundTripTime:
                received_rtt = true;
                ++rtt_samples;
                std::cout << "rtt peer=" << event.peer
                          << " ms=" << event.round_trip_time_ms << std::endl;
                break;
            case SessionEventType::ProtocolError:
                std::cerr << "protocol_error peer=" << event.peer
                          << " code=" << static_cast<unsigned>(event.protocol_error)
                          << std::endl;
                break;
            case SessionEventType::Message:
                if (event.message_type == MessageType::Snapshot) {
                    VehicleSnapshot snapshot{};
                    const VehicleSnapshotCodecError decoded =
                        decode_vehicle_snapshot(event.payload, snapshot);
                    if (!vehicle_snapshot_codec_succeeded(decoded)) {
                        std::cerr << "snapshot decode failed code="
                                  << static_cast<unsigned>(decoded) << '\n';
                        return 6;
                    }
                    ++snapshot_samples;
                    std::cout << "snapshot entity=" << snapshot.entity_id
                              << " sequence=" << snapshot.sequence
                              << " tick=" << snapshot.server_tick << std::endl;
                }
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (connected && now >= next_ping) {
            next_ping = now + 1s;
            for (PeerId peer : peers)
                (void)session.ping(peer);
        }
        if (is_host && scripted_snapshots && connected && now >= next_snapshot) {
            next_snapshot = now + 50ms;
            VehicleSnapshot snapshot{};
            snapshot.entity_id = 42;
            snapshot.sequence = snapshot_sequence;
            snapshot.server_tick = snapshot_sequence++;
            snapshot.position = {50.0f + 0.05f * snapshot.sequence, 0.0f, 50.0f};
            snapshot.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            snapshot.linear_velocity = {1.0f, 0.0f, 0.0f};
            std::array<Byte, kVehicleSnapshotWireSize> payload{};
            if (encode_vehicle_snapshot(snapshot, payload) !=
                VehicleSnapshotCodecError::None)
                return 7;
            for (PeerId peer : peers)
                (void)session.send(peer, MessageType::Snapshot,
                                   Channel::Unreliable, payload);
        }
        if (received_rtt && rtt_samples >= 10)
            break;
        std::this_thread::sleep_for(1ms);
    }

    session.stop();
    if (!connected) {
        std::cerr << "timeout waiting for handshake\n";
        return 4;
    }
    if (!received_rtt) {
        std::cerr << "timeout waiting for RTT\n";
        return 5;
    }
    return 0;
}
