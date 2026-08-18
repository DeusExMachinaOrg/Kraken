#include "net/lan_discovery.hpp"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

kraken::net::SessionIdentity test_identity()
{
    return {"wire-1", "kraken-1", "game-1", "mod-1", "resources-1"};
}

kraken::net::LanSessionAdvertisement test_advertisement(
    const kraken::net::MatchState state = kraken::net::MatchState::Forming)
{
    kraken::net::LanSessionAdvertisement advertisement{};
    advertisement.game_port = 28915;
    advertisement.state = state;
    advertisement.join_policy = kraken::net::JoinPolicy::ClosedAfterStart;
    advertisement.current_map = "r1m1";
    advertisement.target_map = "r1m1";
    advertisement.current_players = 1;
    advertisement.max_players = 4;
    advertisement.identity = test_identity();
    return advertisement;
}

bool test_advertisement_codec()
{
    using namespace kraken::net;
    const LanSessionAdvertisement expected = test_advertisement();
    std::vector<Byte> encoded;
    if (encode_lan_discovery_advertisement(expected, encoded) !=
        LanDiscoveryCodecError::None)
        return false;
    if (encoded.size() > kMaxLanDiscoveryDatagramSize)
        return false;

    LanSessionAdvertisement decoded{};
    if (decode_lan_discovery_advertisement(ByteView{encoded}, decoded) !=
            LanDiscoveryCodecError::None ||
        decoded.game_port != expected.game_port ||
        decoded.state != expected.state ||
        decoded.join_policy != expected.join_policy ||
        decoded.current_map != expected.current_map ||
        decoded.target_map != expected.target_map ||
        decoded.current_players != expected.current_players ||
        decoded.max_players != expected.max_players ||
        decoded.identity != expected.identity)
        return false;

    std::vector<Byte> reencoded;
    if (encode_lan_discovery_advertisement(decoded, reencoded) !=
            LanDiscoveryCodecError::None ||
        reencoded != encoded)
        return false;

    std::vector<Byte> legacy{
        static_cast<Byte>('K'), static_cast<Byte>('R'), static_cast<Byte>('N'),
        static_cast<Byte>('H'), static_cast<Byte>('O'), static_cast<Byte>('S'),
        static_cast<Byte>('T'), static_cast<Byte>('1'),
    };
    if (decode_lan_discovery_advertisement(ByteView{legacy}, decoded) !=
        LanDiscoveryCodecError::InputTooSmall)
        return false;

    std::vector<Byte> bad_version = encoded;
    bad_version[4] = static_cast<Byte>(2);
    if (decode_lan_discovery_advertisement(ByteView{bad_version}, decoded) !=
        LanDiscoveryCodecError::BadVersion)
        return false;

    std::vector<Byte> truncated = encoded;
    truncated.pop_back();
    if (decode_lan_discovery_advertisement(ByteView{truncated}, decoded) ==
        LanDiscoveryCodecError::None)
        return false;

    const std::vector<Byte> oversized(
        kMaxLanDiscoveryDatagramSize + 1, Byte{});
    if (decode_lan_discovery_advertisement(ByteView{oversized}, decoded) !=
        LanDiscoveryCodecError::InputTooLarge)
        return false;

    LanSessionAdvertisement invalid = expected;
    invalid.state = static_cast<MatchState>(99);
    if (encode_lan_discovery_advertisement(invalid, encoded) !=
        LanDiscoveryCodecError::InvalidState)
        return false;
    invalid = expected;
    invalid.current_players = invalid.max_players;
    if (is_joinable_lan_session(invalid, expected.identity))
        return false;
    if (!is_compatible_lan_session(invalid, expected.identity))
        return false;

    LanSessionAdvertisement max_capacity = expected;
    max_capacity.current_players = 63;
    max_capacity.max_players = 64;
    if (encode_lan_discovery_advertisement(max_capacity, encoded) !=
        LanDiscoveryCodecError::None)
        return false;
    max_capacity.max_players = 65;
    if (encode_lan_discovery_advertisement(max_capacity, encoded) !=
        LanDiscoveryCodecError::InvalidPlayerCount)
        return false;
    max_capacity.max_players = 0;
    return encode_lan_discovery_advertisement(max_capacity, encoded) ==
           LanDiscoveryCodecError::InvalidPlayerCount;
}

bool test_advertisement_selection()
{
    using namespace kraken::net;
    const SessionIdentity identity = test_identity();
    LanDiscoveredSession playing{
        {"192.168.2.20", 28920},
        test_advertisement(MatchState::Playing)};
    playing.advertisement.join_policy = JoinPolicy::JoinInProgress;

    LanDiscoveredSession closed_playing{
        {"192.168.2.1", 28911},
        test_advertisement(MatchState::Playing)};
    LanDiscoveredSession full_forming{
        {"192.168.2.2", 28912},
        test_advertisement()};
    full_forming.advertisement.current_players =
        full_forming.advertisement.max_players;
    LanDiscoveredSession incompatible{
        {"192.168.2.3", 28913},
        test_advertisement()};
    incompatible.advertisement.identity.game_version = "other-game";

    LanDiscoveredSession forming_high{
        {"192.168.2.20", 28915}, test_advertisement()};
    LanDiscoveredSession forming_low{
        {"192.168.2.20", 28914}, test_advertisement()};
    const std::vector<LanDiscoveredSession> candidates{
        playing, closed_playing, full_forming, incompatible,
        forming_high, forming_low};
    const auto selected = select_lan_session(candidates, identity);
    if (!selected || selected->endpoint.host != "192.168.2.20" ||
        selected->endpoint.port != 28914 ||
        selected->advertisement.state != MatchState::Forming)
        return false;

    const std::vector<LanDiscoveredSession> playing_only{playing};
    const auto playing_selected = select_lan_session(playing_only, identity);
    if (!playing_selected || playing_selected->endpoint.port != 28920)
        return false;

    const std::vector<LanDiscoveredSession> rejected{
        closed_playing, full_forming, incompatible};
    return !select_lan_session(rejected, identity);
}

} // namespace

int main()
{
    if (!test_advertisement_codec()) {
        std::cerr << "LAN discovery advertisement codec test failed\n";
        return 30;
    }
    if (!test_advertisement_selection()) {
        std::cerr << "LAN discovery advertisement selection test failed\n";
        return 31;
    }

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

    // Metadata-aware responses use the same request fan-out but are selected
    // only after decoding and compatibility filtering.
    kraken::net::LanSessionAdvertisement advertisement = test_advertisement();
    advertisement.game_port = 28917;
    if (!host.become_host(28918, advertisement)) {
        std::cerr << "could not bind metadata LAN discovery host\n";
        return 3;
    }
    std::atomic<bool> metadata_pumping = true;
    std::thread metadata_host_thread([&] {
        while (metadata_pumping.load()) {
            host.pump();
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto discovered = kraken::net::LanDiscovery::discover(
        28918, 500ms, test_identity(), "127.0.0.1");
    metadata_pumping = false;
    metadata_host_thread.join();
    host.stop();
    if (!discovered || discovered->endpoint.host != "127.0.0.1" ||
        discovered->endpoint.port != advertisement.game_port ||
        discovered->advertisement.identity != advertisement.identity ||
        discovered->advertisement.target_map != advertisement.target_map) {
        std::cerr << "metadata LAN discovery response invalid\n";
        return 4;
    }
    std::cout << "LAN discovery tests passed\n";
    WSACleanup();
    return 0;
}
