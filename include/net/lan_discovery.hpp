#ifndef KRAKEN_NET_LAN_DISCOVERY_HPP
#define KRAKEN_NET_LAN_DISCOVERY_HPP

#include "net/entity_registry.hpp"
#include "net/match_session.hpp"
#include "net/session_compatibility.hpp"
#include "net/transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// Discovery responses deliberately carry only the two states in which a new
// peer can be admitted.  Loading/synchronizing/leaving sessions are not
// advertised because their admission policy is not stable at discovery time.
struct LanSessionAdvertisement {
    std::uint16_t game_port = kDefaultPort;
    MatchState state = MatchState::Forming;
    JoinPolicy join_policy = JoinPolicy::ClosedAfterStart;
    std::string current_map;
    std::string target_map;
    std::uint8_t current_players = 0;
    std::uint8_t max_players = 0;
    SessionIdentity identity{};
};

using LanDiscoveryAdvertisement = LanSessionAdvertisement;

struct LanDiscoveredSession {
    Endpoint endpoint;
    LanSessionAdvertisement advertisement;
};

inline constexpr std::uint32_t kLanDiscoveryWireMagic = 0x32444C4Bu;
inline constexpr std::uint16_t kLanDiscoveryWireVersion = 1;
inline constexpr std::size_t kLanDiscoveryWireHeaderSize = 20;
inline constexpr std::size_t kMaxLanDiscoveryMapSize = 128;
inline constexpr std::size_t kMaxLanDiscoveryDatagramSize =
    kLanDiscoveryWireHeaderSize + 2 * kMaxLanDiscoveryMapSize +
    kSessionCompatibilityMaxWireSize;
inline constexpr std::size_t kMaxLanDiscoveryCandidates = 64;

enum class LanDiscoveryCodecError : std::uint8_t {
    None,
    InputTooSmall,
    InputTooLarge,
    BadMagic,
    BadVersion,
    BadFlags,
    BadReserved,
    OutputTooSmall,
    InvalidPort,
    InvalidState,
    InvalidJoinPolicy,
    InvalidMap,
    MapTooLong,
    InvalidPlayerCount,
    InvalidIdentity,
    PayloadTooLarge,
    BadPayloadSize,
};

[[nodiscard]] constexpr bool lan_discovery_codec_succeeded(
    const LanDiscoveryCodecError error) noexcept
{
    return error == LanDiscoveryCodecError::None;
}

[[nodiscard]] LanDiscoveryCodecError encode_lan_discovery_advertisement(
    const LanSessionAdvertisement&, std::vector<Byte>&);
[[nodiscard]] LanDiscoveryCodecError decode_lan_discovery_advertisement(
    ByteView, LanSessionAdvertisement&) noexcept;

// A candidate is compatible only when every advertised identity field is
// equal.  Joinability additionally excludes closed, full, or transitional
// sessions.  These pure predicates are also used by the socket discovery
// path, keeping packet validation and selection rules identical in tests and
// production.
[[nodiscard]] bool is_compatible_lan_session(
    const LanSessionAdvertisement&, const SessionIdentity&) noexcept;
[[nodiscard]] bool is_joinable_lan_session(
    const LanSessionAdvertisement&, const SessionIdentity&) noexcept;

[[nodiscard]] std::optional<LanDiscoveredSession> select_lan_session(
    std::span<const LanDiscoveredSession>,
    const SessionIdentity&) noexcept;

// IPv4 data required to derive the directed broadcast for one active adapter.
// Kept independent of Win32 adapter structures so the discovery policy has a
// deterministic regression test.
struct LanIpv4Adapter {
    std::string address;
    std::string netmask;
};

// Normal automatic discovery sends to the limited broadcast and to every
// adapter's directed broadcast.  An explicit target deliberately bypasses
// that fan-out for tests and troubleshooting.
[[nodiscard]] std::vector<Endpoint> make_lan_discovery_targets(
    std::uint16_t port, std::string_view requested_target,
    const std::vector<LanIpv4Adapter>& adapters);

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
    // Metadata-aware hosting is opt-in so the existing runtime endpoint API
    // remains source-compatible while the session lifecycle is integrated.
    [[nodiscard]] bool become_host(std::uint16_t discovery_port,
                                   std::uint16_t game_port,
                                   const LanSessionAdvertisement&);
    [[nodiscard]] bool become_host(std::uint16_t discovery_port,
                                   const LanSessionAdvertisement& advertisement);
    [[nodiscard]] bool set_advertisement(
        const LanSessionAdvertisement& advertisement);
    [[nodiscard]] static std::optional<Endpoint> discover(
        std::uint16_t discovery_port,
        std::chrono::milliseconds timeout,
        std::string_view target_address = "255.255.255.255") noexcept;
    [[nodiscard]] static std::optional<LanDiscoveredSession> discover(
        std::uint16_t discovery_port,
        std::chrono::milliseconds timeout,
        const SessionIdentity& identity,
        std::string_view target_address = "255.255.255.255") noexcept;
    // A stable local delay used only after an unanswered discovery round.
    // It prevents two simultaneously-started LAN peers from both becoming
    // hosts before either one can answer a broadcast.
    [[nodiscard]] static std::chrono::milliseconds host_election_delay() noexcept;
    void pump() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool hosting() const noexcept;

private:
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
    std::uint16_t game_port_ = 0;
    std::optional<LanSessionAdvertisement> advertisement_;
    std::vector<Byte> advertisement_packet_;
};

} // namespace kraken::net

#endif // KRAKEN_NET_LAN_DISCOVERY_HPP
