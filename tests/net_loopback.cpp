#include "config.hpp"
#include "net/session.hpp"
#include "net/transport.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
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

    if (kraken::clamp_multiplayer_max_players(64) != 64 ||
        kraken::clamp_multiplayer_max_players(65) != 64 ||
        kraken::clamp_multiplayer_max_players(0) != 1 ||
        kraken::multiplayer_host_peer_capacity(64) < 63)
        return 13;

    TransportConfig invalid_capacity{};
    invalid_capacity.role = TransportRole::Server;
    invalid_capacity.bind_endpoint = {"127.0.0.1", 39050};
    invalid_capacity.max_peers = kMaxTransportPeerCapacity + 1;
    EnetTransport invalid_capacity_transport;
    if (invalid_capacity_transport.start(invalid_capacity).code !=
        TransportResultCode::InvalidArgument)
        return 14;
    invalid_capacity.max_peers = 0;
    if (invalid_capacity_transport.start(invalid_capacity).code !=
        TransportResultCode::InvalidArgument)
        return 15;

    SessionIdentity default_identity{};
    std::array<Byte, kSessionCompatibilityMaxWireSize> default_encoding_a{};
    std::array<Byte, kSessionCompatibilityMaxWireSize> default_encoding_b{};
    std::size_t default_size_a = 0;
    std::size_t default_size_b = 0;
    if (encode_session_identity(default_identity, default_encoding_a,
                                default_size_a) !=
            SessionCompatibilityCodecError::None ||
        encode_session_identity(default_identity, default_encoding_b,
                                default_size_b) !=
            SessionCompatibilityCodecError::None ||
        default_size_a != default_size_b)
        return 12;
    for (std::size_t index = 0; index < default_size_a; ++index) {
        if (default_encoding_a[index] != default_encoding_b[index])
            return 12;
    }

    SessionIdentity exact_identity{};
    exact_identity.protocol_version = "session-protocol-1";
    exact_identity.kraken_version = "kraken-0.1";
    exact_identity.game_version = "game-0.1";
    exact_identity.mod_version = "mod-0.1";
    exact_identity.resource_fingerprint = "sha256:loopback-resources";

    std::array<Byte, kSessionCompatibilityMaxWireSize> encoded_identity{};
    std::size_t identity_size = 0;
    const SessionCompatibilityCodecError encoded = encode_session_identity(
        exact_identity, encoded_identity, identity_size);
    if (encoded != SessionCompatibilityCodecError::None || identity_size == 0) {
        std::cerr << "compat encode failed code="
                  << static_cast<unsigned>(encoded)
                  << " size=" << identity_size << '\n';
        return 1;
    }
    SessionIdentity decoded_identity{};
    if (decode_session_identity(
            ByteView{encoded_identity}.first(identity_size), decoded_identity) !=
            SessionCompatibilityCodecError::None ||
        decoded_identity != exact_identity)
        return 2;

    auto malformed_identity = encoded_identity;
    malformed_identity[4] = static_cast<Byte>(
        kSessionCompatibilityVersion + static_cast<std::uint16_t>(1));
    if (decode_session_identity(
            ByteView{malformed_identity}.first(identity_size), decoded_identity) !=
        SessionCompatibilityCodecError::BadVersion)
        return 3;

    malformed_identity = encoded_identity;
    malformed_identity[12] = static_cast<Byte>(
        kSessionCompatibilityMaxFieldSize + 1);
    malformed_identity[13] = Byte{};
    if (decode_session_identity(
            ByteView{malformed_identity}.first(identity_size), decoded_identity) !=
        SessionCompatibilityCodecError::FieldTooLarge)
        return 4;

    SessionIdentity oversized_identity{};
    oversized_identity.mod_version.assign(kSessionCompatibilityMaxFieldSize + 1,
                                          'x');
    if (is_valid_session_identity(oversized_identity))
        return 5;

    constexpr std::uint16_t port = 39051;

    EnetTransport host_transport;
    SessionConfig host_config{};
    host_config.role = SessionRole::Server;
    host_config.transport.bind_endpoint = {"127.0.0.1", port};
    // A 64-player session needs room for 63 remote clients. Starting this
    // host exercises the real ENet allocation without creating 63 clients.
    host_config.transport.max_peers = kMaxTransportPeerCapacity - 1;
    host_config.protocol_version = exact_identity.protocol_version;
    host_config.kraken_version = exact_identity.kraken_version;
    host_config.game_version = exact_identity.game_version;
    host_config.mod_version = exact_identity.mod_version;
    host_config.resource_fingerprint = exact_identity.resource_fingerprint;
    Session host(host_transport, host_config);

    EnetTransport client_transport;
    SessionConfig client_config{};
    client_config.role = SessionRole::Client;
    client_config.protocol_version = exact_identity.protocol_version;
    client_config.kraken_version = exact_identity.kraken_version;
    client_config.game_version = exact_identity.game_version;
    client_config.mod_version = exact_identity.mod_version;
    client_config.resource_fingerprint = exact_identity.resource_fingerprint;
    Session client(client_transport, client_config);

    const TransportResult host_started = host.start();
    const TransportResult client_started = client.start();
    const TransportResult client_connected_to_host =
        client.connect(Endpoint{"127.0.0.1", port});
    if (!host_started || !client_started || !client_connected_to_host) {
        std::cerr << "exact setup failed host="
                  << static_cast<unsigned>(host_started.code)
                  << " client=" << static_cast<unsigned>(client_started.code)
                  << " connect="
                  << static_cast<unsigned>(client_connected_to_host.code)
                  << '\n';
        return 1;
    }

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
        return 5;
    if (!client.ping(client_peer))
        return 6;

    bool received_rtt = false;
    const auto ping_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < ping_deadline && !received_rtt) {
        if (!pump(host) || !pump(client))
            return 7;
        (void)host.drain_events(events);
        const std::size_t count = client.drain_events(events);
        for (std::size_t index = 0; index < count; ++index)
            received_rtt |= events[index].type ==
                            SessionEventType::RoundTripTime;
        std::this_thread::sleep_for(1ms);
    }

    client.stop();
    host.stop();
    if (!received_rtt)
        return 8;

    constexpr std::uint16_t mismatch_port = 39052;
    EnetTransport mismatch_host_transport;
    SessionConfig mismatch_host_config = host_config;
    mismatch_host_config.transport.bind_endpoint.port = mismatch_port;
    Session mismatch_host(mismatch_host_transport, mismatch_host_config);

    EnetTransport mismatch_client_transport;
    SessionConfig mismatch_client_config = client_config;
    mismatch_client_config.resource_fingerprint = "sha256:different";
    Session mismatch_client(mismatch_client_transport, mismatch_client_config);

    if (!mismatch_host.start() || !mismatch_client.start() ||
        !mismatch_client.connect(
            Endpoint{"127.0.0.1", mismatch_port}))
        return 9;

    bool mismatch_host_connected = false;
    bool mismatch_client_connected = false;
    bool mismatch_rejected = false;
    bool mismatch_client_disconnected = false;
    const auto mismatch_deadline =
        std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < mismatch_deadline &&
           !mismatch_client_disconnected) {
        const TransportResult host_result = mismatch_host.pump();
        const TransportResult client_result = mismatch_client.pump();
        if ((!host_result && host_result.code != TransportResultCode::ProtocolError) ||
            (!client_result &&
             client_result.code != TransportResultCode::ProtocolError))
            return 10;

        std::size_t count = mismatch_host.drain_events(events);
        for (std::size_t index = 0; index < count; ++index) {
            mismatch_host_connected |=
                events[index].type == SessionEventType::PeerConnected;
            mismatch_rejected |=
                events[index].type == SessionEventType::ProtocolError;
        }
        count = mismatch_client.drain_events(events);
        for (std::size_t index = 0; index < count; ++index) {
            mismatch_client_connected |=
                events[index].type == SessionEventType::PeerConnected;
            mismatch_client_disconnected |=
                events[index].type == SessionEventType::PeerDisconnected;
        }
        std::this_thread::sleep_for(1ms);
    }

    mismatch_client.stop();
    mismatch_host.stop();
    if (mismatch_host_connected || mismatch_client_connected ||
        !mismatch_rejected || !mismatch_client_disconnected)
        return 11;
    return 0;
}
