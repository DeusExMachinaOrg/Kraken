#include "net/lan_discovery.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kraken::net {
namespace {

constexpr std::array<char, 8> kRequest{{'K', 'R', 'N', 'L', 'A', 'N', '1', '\0'}};
constexpr std::array<char, 8> kResponse{{'K', 'R', 'N', 'H', 'O', 'S', 'T', '1'}};
constexpr std::size_t kResponseSize = kResponse.size() + sizeof(std::uint16_t);
constexpr std::size_t kCurrentMapLengthOffset = 14;
constexpr std::size_t kTargetMapLengthOffset = 16;
constexpr std::size_t kMapsOffset = kLanDiscoveryWireHeaderSize;

SOCKET as_socket(std::uintptr_t value) noexcept
{
    return static_cast<SOCKET>(value);
}

std::uintptr_t as_handle(SOCKET socket) noexcept
{
    return static_cast<std::uintptr_t>(socket);
}

bool set_nonblocking(SOCKET socket) noexcept
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

bool ensure_winsock() noexcept
{
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

void close_socket(SOCKET socket) noexcept
{
    if (socket != INVALID_SOCKET)
        closesocket(socket);
}

bool is_would_block() noexcept
{
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
}

void put_u16(Byte* const destination, const std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* const destination, const std::uint32_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
    destination[2] = static_cast<Byte>((value >> 16) & 0xffu);
    destination[3] = static_cast<Byte>((value >> 24) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(const Byte* const source) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(source[0]) |
        (static_cast<std::uint16_t>(source[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const Byte* const source) noexcept
{
    return static_cast<std::uint32_t>(source[0]) |
           (static_cast<std::uint32_t>(source[1]) << 8) |
           (static_cast<std::uint32_t>(source[2]) << 16) |
           (static_cast<std::uint32_t>(source[3]) << 24);
}

[[nodiscard]] bool valid_map_text(const std::string_view value) noexcept
{
    if (value.size() > kMaxLanDiscoveryMapSize)
        return false;
    return std::find(value.begin(), value.end(), '\0') == value.end();
}

[[nodiscard]] bool valid_state(const MatchState state) noexcept
{
    return state == MatchState::Forming || state == MatchState::Playing;
}

[[nodiscard]] bool valid_join_policy(const JoinPolicy policy) noexcept
{
    return policy == JoinPolicy::ClosedAfterStart ||
           policy == JoinPolicy::JoinInProgress;
}

[[nodiscard]] LanDiscoveryCodecError validate_advertisement(
    const LanSessionAdvertisement& advertisement) noexcept
{
    if (advertisement.game_port == 0)
        return LanDiscoveryCodecError::InvalidPort;
    if (!valid_state(advertisement.state))
        return LanDiscoveryCodecError::InvalidState;
    if (!valid_join_policy(advertisement.join_policy))
        return LanDiscoveryCodecError::InvalidJoinPolicy;
    if (!valid_map_text(advertisement.current_map) ||
        !valid_map_text(advertisement.target_map))
        return (advertisement.current_map.size() > kMaxLanDiscoveryMapSize ||
                advertisement.target_map.size() > kMaxLanDiscoveryMapSize)
            ? LanDiscoveryCodecError::MapTooLong
            : LanDiscoveryCodecError::InvalidMap;
    if (advertisement.target_map.empty() ||
        (advertisement.state == MatchState::Playing &&
         advertisement.current_map.empty()))
        return LanDiscoveryCodecError::InvalidMap;
    if (advertisement.max_players == 0 ||
        advertisement.max_players > kMaxSessionPlayers ||
        advertisement.current_players > advertisement.max_players)
        return LanDiscoveryCodecError::InvalidPlayerCount;
    if (!is_valid_session_identity(advertisement.identity))
        return LanDiscoveryCodecError::InvalidIdentity;
    return LanDiscoveryCodecError::None;
}

[[nodiscard]] LanDiscoveryCodecError map_identity_error(
    const SessionCompatibilityCodecError error) noexcept
{
    switch (error) {
    case SessionCompatibilityCodecError::None:
        return LanDiscoveryCodecError::None;
    case SessionCompatibilityCodecError::InputTooSmall:
        return LanDiscoveryCodecError::InputTooSmall;
    case SessionCompatibilityCodecError::BadMagic:
        return LanDiscoveryCodecError::BadMagic;
    case SessionCompatibilityCodecError::BadVersion:
        return LanDiscoveryCodecError::BadVersion;
    case SessionCompatibilityCodecError::BadPayloadSize:
        return LanDiscoveryCodecError::BadPayloadSize;
    default:
        return LanDiscoveryCodecError::InvalidIdentity;
    }
}

sockaddr_in ipv4_address(std::string_view host, std::uint16_t port) noexcept
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const std::string host_copy(host);
    if (InetPtonA(AF_INET, host_copy.c_str(), &address.sin_addr) != 1)
        address.sin_addr.s_addr = INADDR_NONE;
    return address;
}

void append_target(std::vector<sockaddr_in>& targets, const sockaddr_in& target)
{
    if (target.sin_addr.s_addr == INADDR_NONE)
        return;
    const auto found = std::find_if(targets.begin(), targets.end(),
        [&target](const sockaddr_in& existing) {
            return existing.sin_addr.s_addr == target.sin_addr.s_addr &&
                   existing.sin_port == target.sin_port;
        });
    if (found == targets.end())
        targets.push_back(target);
}

std::vector<sockaddr_in> discovery_targets(const std::uint16_t port,
                                           const std::string_view requested_target)
{
    // Explicit destinations are useful to tests and troubleshooting.  Normal
    // gameplay fans out to the limited broadcast plus every adapter's directed
    // broadcast address.  On Windows the former is commonly routed to the
    // wrong NIC when a VPN, Hyper-V or a virtual LAN adapter exists.
    std::vector<LanIpv4Adapter> adapters;
    ULONG bytes = 0;
    if (requested_target == "255.255.255.255" &&
        GetAdaptersInfo(nullptr, &bytes) == ERROR_BUFFER_OVERFLOW && bytes != 0) {
        std::vector<std::byte> buffer(bytes);
        auto* adapter_list = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
        if (GetAdaptersInfo(adapter_list, &bytes) == NO_ERROR) {
            for (PIP_ADAPTER_INFO adapter = adapter_list; adapter != nullptr; adapter = adapter->Next) {
                if (adapter->Type == MIB_IF_TYPE_LOOPBACK)
                    continue;
                for (IP_ADDR_STRING* item = &adapter->IpAddressList; item != nullptr; item = item->Next)
                    adapters.push_back({item->IpAddress.String, item->IpMask.String});
            }
        }
    }

    const std::vector<Endpoint> endpoint_targets =
        make_lan_discovery_targets(port, requested_target, adapters);
    std::vector<sockaddr_in> targets;
    for (const Endpoint& endpoint : endpoint_targets)
        append_target(targets, ipv4_address(endpoint.host, endpoint.port));
    return targets;
}

std::uint8_t local_ipv4_host_octet() noexcept
{
    ULONG bytes = 0;
    std::uint8_t best = 255;
    if (GetAdaptersInfo(nullptr, &bytes) != ERROR_BUFFER_OVERFLOW || bytes == 0)
        return best;
    std::vector<std::byte> buffer(bytes);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
    if (GetAdaptersInfo(adapters, &bytes) != NO_ERROR)
        return best;
    for (PIP_ADAPTER_INFO adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->Type == MIB_IF_TYPE_LOOPBACK)
            continue;
        for (IP_ADDR_STRING* item = &adapter->IpAddressList; item != nullptr; item = item->Next) {
            in_addr address{};
            if (InetPtonA(AF_INET, item->IpAddress.String, &address) != 1)
                continue;
            const std::uint32_t host = ntohl(address.s_addr);
            const std::uint8_t first = static_cast<std::uint8_t>(host >> 24);
            const std::uint8_t last = static_cast<std::uint8_t>(host);
            if (first != 127 && first != 169)
                best = last < best ? last : best;
        }
    }
    return best;
}

} // namespace

LanDiscoveryCodecError encode_lan_discovery_advertisement(
    const LanSessionAdvertisement& advertisement, std::vector<Byte>& output)
{
    const LanDiscoveryCodecError valid = validate_advertisement(advertisement);
    if (!lan_discovery_codec_succeeded(valid)) {
        output.clear();
        return valid;
    }

    const std::size_t identity_size =
        session_identity_wire_size(advertisement.identity);
    const std::size_t required = kMapsOffset + advertisement.current_map.size() +
                                 advertisement.target_map.size() + identity_size;
    if (required > kMaxLanDiscoveryDatagramSize ||
        required > (std::numeric_limits<std::uint16_t>::max)()) {
        output.clear();
        return LanDiscoveryCodecError::PayloadTooLarge;
    }

    output.assign(required, Byte{});
    put_u32(output.data(), kLanDiscoveryWireMagic);
    put_u16(output.data() + 4, kLanDiscoveryWireVersion);
    put_u16(output.data() + 6, 0);
    put_u16(output.data() + 8, advertisement.game_port);
    output[10] = static_cast<Byte>(advertisement.state);
    output[11] = static_cast<Byte>(advertisement.join_policy);
    output[12] = static_cast<Byte>(advertisement.current_players);
    output[13] = static_cast<Byte>(advertisement.max_players);
    put_u16(output.data() + kCurrentMapLengthOffset,
            static_cast<std::uint16_t>(advertisement.current_map.size()));
    put_u16(output.data() + kTargetMapLengthOffset,
            static_cast<std::uint16_t>(advertisement.target_map.size()));
    put_u16(output.data() + 18, 0);

    std::size_t offset = kMapsOffset;
    std::copy(advertisement.current_map.begin(), advertisement.current_map.end(),
              reinterpret_cast<char*>(output.data()) + offset);
    offset += advertisement.current_map.size();
    std::copy(advertisement.target_map.begin(), advertisement.target_map.end(),
              reinterpret_cast<char*>(output.data()) + offset);
    offset += advertisement.target_map.size();

    std::size_t identity_written = 0;
    const SessionCompatibilityCodecError identity_error =
        encode_session_identity(advertisement.identity,
                                MutableByteView{output}.subspan(offset),
                                identity_written);
    if (identity_error != SessionCompatibilityCodecError::None ||
        identity_written != identity_size) {
        output.clear();
        return LanDiscoveryCodecError::InvalidIdentity;
    }
    return LanDiscoveryCodecError::None;
}

LanDiscoveryCodecError decode_lan_discovery_advertisement(
    const ByteView input, LanSessionAdvertisement& advertisement) noexcept
{
    if (input.size() < kLanDiscoveryWireHeaderSize)
        return LanDiscoveryCodecError::InputTooSmall;
    if (input.size() > kMaxLanDiscoveryDatagramSize)
        return LanDiscoveryCodecError::InputTooLarge;

    const Byte* const data = input.data();
    if (get_u32(data) != kLanDiscoveryWireMagic)
        return LanDiscoveryCodecError::BadMagic;
    if (get_u16(data + 4) != kLanDiscoveryWireVersion)
        return LanDiscoveryCodecError::BadVersion;
    if (get_u16(data + 6) != 0)
        return LanDiscoveryCodecError::BadFlags;
    if (get_u16(data + 18) != 0)
        return LanDiscoveryCodecError::BadReserved;

    LanSessionAdvertisement decoded{};
    decoded.game_port = get_u16(data + 8);
    decoded.state = static_cast<MatchState>(static_cast<std::uint8_t>(data[10]));
    decoded.join_policy =
        static_cast<JoinPolicy>(static_cast<std::uint8_t>(data[11]));
    decoded.current_players = static_cast<std::uint8_t>(data[12]);
    decoded.max_players = static_cast<std::uint8_t>(data[13]);

    const std::size_t current_size = get_u16(data + kCurrentMapLengthOffset);
    const std::size_t target_size = get_u16(data + kTargetMapLengthOffset);
    if (current_size > kMaxLanDiscoveryMapSize ||
        target_size > kMaxLanDiscoveryMapSize)
        return LanDiscoveryCodecError::MapTooLong;
    if (input.size() - kMapsOffset < current_size ||
        input.size() - kMapsOffset - current_size < target_size)
        return LanDiscoveryCodecError::InputTooSmall;

    std::size_t offset = kMapsOffset;
    decoded.current_map.assign(
        reinterpret_cast<const char*>(data + offset), current_size);
    offset += current_size;
    decoded.target_map.assign(
        reinterpret_cast<const char*>(data + offset), target_size);
    offset += target_size;

    const SessionCompatibilityCodecError identity_error =
        decode_session_identity(input.subspan(offset), decoded.identity);
    const LanDiscoveryCodecError mapped_identity_error =
        map_identity_error(identity_error);
    if (!lan_discovery_codec_succeeded(mapped_identity_error))
        return mapped_identity_error;

    const LanDiscoveryCodecError valid = validate_advertisement(decoded);
    if (!lan_discovery_codec_succeeded(valid))
        return valid;
    advertisement = std::move(decoded);
    return LanDiscoveryCodecError::None;
}

bool is_compatible_lan_session(const LanSessionAdvertisement& advertisement,
                               const SessionIdentity& identity) noexcept
{
    return lan_discovery_codec_succeeded(validate_advertisement(advertisement)) &&
           advertisement.identity == identity;
}

bool is_joinable_lan_session(const LanSessionAdvertisement& advertisement,
                             const SessionIdentity& identity) noexcept
{
    if (!is_compatible_lan_session(advertisement, identity) ||
        advertisement.current_players >= advertisement.max_players)
        return false;
    return advertisement.state == MatchState::Forming ||
           (advertisement.state == MatchState::Playing &&
            advertisement.join_policy == JoinPolicy::JoinInProgress);
}

std::optional<LanDiscoveredSession> select_lan_session(
    const std::span<const LanDiscoveredSession> candidates,
    const SessionIdentity& identity) noexcept
{
    const LanDiscoveredSession* selected = nullptr;
    for (const LanDiscoveredSession& candidate : candidates) {
        if (!is_joinable_lan_session(candidate.advertisement, identity))
            continue;
        if (selected == nullptr) {
            selected = &candidate;
            continue;
        }

        const auto state_rank = [](const MatchState state) noexcept {
            return state == MatchState::Forming ? 0 : 1;
        };
        const int candidate_rank = state_rank(candidate.advertisement.state);
        const int selected_rank = state_rank(selected->advertisement.state);
        if (candidate_rank != selected_rank) {
            if (candidate_rank < selected_rank)
                selected = &candidate;
            continue;
        }
        if (candidate.endpoint.host != selected->endpoint.host) {
            if (candidate.endpoint.host < selected->endpoint.host)
                selected = &candidate;
            continue;
        }
        if (candidate.endpoint.port < selected->endpoint.port)
            selected = &candidate;
    }
    if (selected == nullptr)
        return std::nullopt;
    return *selected;
}

std::vector<Endpoint> make_lan_discovery_targets(
    const std::uint16_t port, const std::string_view requested_target,
    const std::vector<LanIpv4Adapter>& adapters)
{
    if (requested_target != "255.255.255.255")
        return {{std::string(requested_target), port}};

    std::vector<Endpoint> targets{{"255.255.255.255", port}};
    for (const LanIpv4Adapter& adapter : adapters) {
        in_addr address{};
        in_addr mask{};
        if (InetPtonA(AF_INET, adapter.address.c_str(), &address) != 1 ||
            InetPtonA(AF_INET, adapter.netmask.c_str(), &mask) != 1 ||
            address.s_addr == htonl(INADDR_ANY))
            continue;
        in_addr broadcast{};
        broadcast.s_addr = (address.s_addr & mask.s_addr) | ~mask.s_addr;
        char text[INET_ADDRSTRLEN]{};
        if (InetNtopA(AF_INET, &broadcast, text, sizeof(text)) == nullptr)
            continue;
        const auto duplicate = std::find_if(targets.begin(), targets.end(),
            [&text, port](const Endpoint& endpoint) {
                return endpoint.port == port && endpoint.host == text;
            });
        if (duplicate == targets.end())
            targets.push_back({text, port});
    }
    return targets;
}

LanDiscovery::~LanDiscovery()
{
    stop();
}

bool LanDiscovery::become_host(const std::uint16_t discovery_port,
                               const std::uint16_t game_port) noexcept
{
    stop();
    if (!ensure_winsock())
        return false;
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET)
        return false;
    BOOL exclusive = TRUE;
    (void)setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = htons(discovery_port);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&bind_address),
               sizeof(bind_address)) == SOCKET_ERROR || !set_nonblocking(socket)) {
        close_socket(socket);
        return false;
    }
    socket_ = as_handle(socket);
    game_port_ = game_port;
    return true;
}

bool LanDiscovery::become_host(
    const std::uint16_t discovery_port, const std::uint16_t game_port,
    const LanSessionAdvertisement& advertisement)
{
    LanSessionAdvertisement prepared = advertisement;
    prepared.game_port = game_port;
    if (!lan_discovery_codec_succeeded(
            validate_advertisement(prepared)))
        return false;
    if (!become_host(discovery_port, game_port))
        return false;
    if (!set_advertisement(prepared)) {
        stop();
        return false;
    }
    return true;
}

bool LanDiscovery::become_host(
    const std::uint16_t discovery_port,
    const LanSessionAdvertisement& advertisement)
{
    if (advertisement.game_port == 0)
        return false;
    return become_host(discovery_port, advertisement.game_port, advertisement);
}

bool LanDiscovery::set_advertisement(
    const LanSessionAdvertisement& advertisement)
{
    if (!hosting() || advertisement.game_port != game_port_)
        return false;

    std::vector<Byte> encoded;
    if (!lan_discovery_codec_succeeded(
            encode_lan_discovery_advertisement(advertisement, encoded)))
        return false;
    advertisement_ = advertisement;
    advertisement_packet_ = std::move(encoded);
    return true;
}

std::optional<Endpoint> LanDiscovery::discover(
    const std::uint16_t discovery_port, const std::chrono::milliseconds timeout,
    const std::string_view target_address) noexcept
{
    if (!ensure_winsock())
        return std::nullopt;
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET)
        return std::nullopt;
    BOOL broadcast = TRUE;
    (void)setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
    if (!set_nonblocking(socket)) {
        close_socket(socket);
        return std::nullopt;
    }
    const std::vector<sockaddr_in> targets = discovery_targets(discovery_port, target_address);
    bool request_sent = false;
    const auto send_request = [&] {
        for (const sockaddr_in& target : targets) {
            if (sendto(socket, kRequest.data(), static_cast<int>(kRequest.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != SOCKET_ERROR)
                request_sent = true;
        }
    };
    send_request();
    if (!request_sent) {
        close_socket(socket);
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto next_request = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    std::array<char, 32> reply{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= next_request) {
            send_request();
            next_request += std::chrono::milliseconds(250);
        }
        sockaddr_in sender{};
        int sender_size = sizeof(sender);
        const int received = recvfrom(socket, reply.data(), static_cast<int>(reply.size()), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (received == static_cast<int>(kResponseSize) &&
            std::memcmp(reply.data(), kResponse.data(), kResponse.size()) == 0) {
            std::uint16_t network_port = 0;
            std::memcpy(&network_port, reply.data() + kResponse.size(), sizeof(network_port));
            char address[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &sender.sin_addr, address, sizeof(address)) != nullptr) {
                close_socket(socket);
                return Endpoint{address, ntohs(network_port)};
            }
        }
        else if (received == SOCKET_ERROR && !is_would_block())
            break;
        Sleep(5);
    }
    close_socket(socket);
    return std::nullopt;
}

std::optional<LanDiscoveredSession> LanDiscovery::discover(
    const std::uint16_t discovery_port, const std::chrono::milliseconds timeout,
    const SessionIdentity& identity, const std::string_view target_address) noexcept
{
    if (!is_valid_session_identity(identity) || !ensure_winsock())
        return std::nullopt;
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET)
        return std::nullopt;
    BOOL broadcast = TRUE;
    (void)setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
    if (!set_nonblocking(socket)) {
        close_socket(socket);
        return std::nullopt;
    }

    const std::vector<sockaddr_in> targets =
        discovery_targets(discovery_port, target_address);
    bool request_sent = false;
    const auto send_request = [&] {
        for (const sockaddr_in& target : targets) {
            if (sendto(socket, kRequest.data(), static_cast<int>(kRequest.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target)) !=
                SOCKET_ERROR)
                request_sent = true;
        }
    };
    send_request();
    if (!request_sent) {
        close_socket(socket);
        return std::nullopt;
    }

    std::vector<LanDiscoveredSession> candidates;
    candidates.reserve(kMaxLanDiscoveryCandidates);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto next_request = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(250);
    std::array<char, kMaxLanDiscoveryDatagramSize> reply{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= next_request) {
            send_request();
            next_request += std::chrono::milliseconds(250);
        }

        sockaddr_in sender{};
        int sender_size = sizeof(sender);
        const int received = recvfrom(
            socket, reply.data(), static_cast<int>(reply.size()), 0,
            reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (received == SOCKET_ERROR) {
            if (!is_would_block())
                break;
            Sleep(5);
            continue;
        }
        if (received <= 0 || received > static_cast<int>(reply.size()))
            continue;

        LanSessionAdvertisement advertisement{};
        const ByteView packet{
            reinterpret_cast<const Byte*>(reply.data()),
            static_cast<std::size_t>(received)};
        if (!lan_discovery_codec_succeeded(
                decode_lan_discovery_advertisement(packet, advertisement)) ||
            !is_joinable_lan_session(advertisement, identity))
            continue;

        char address[INET_ADDRSTRLEN]{};
        if (InetNtopA(AF_INET, &sender.sin_addr, address, sizeof(address)) == nullptr)
            continue;
        LanDiscoveredSession candidate{
            Endpoint{address, advertisement.game_port}, std::move(advertisement)};
        const auto duplicate = std::find_if(
            candidates.begin(), candidates.end(),
            [&candidate](const LanDiscoveredSession& existing) {
                return existing.endpoint.host == candidate.endpoint.host &&
                       existing.endpoint.port == candidate.endpoint.port;
            });
        if (duplicate != candidates.end())
            *duplicate = std::move(candidate);
        else if (candidates.size() < kMaxLanDiscoveryCandidates)
            candidates.push_back(std::move(candidate));
    }
    close_socket(socket);
    return select_lan_session(candidates, identity);
}

std::chrono::milliseconds LanDiscovery::host_election_delay() noexcept
{
    // 10 ms keeps the extra join time short while leaving a deterministic
    // lead for the usual /24 LAN.  The final second discovery window below
    // covers neighbours whose last octets are close together.
    return std::chrono::milliseconds(10u * local_ipv4_host_octet());
}

void LanDiscovery::pump() noexcept
{
    const SOCKET socket = as_socket(socket_);
    if (socket == INVALID_SOCKET)
        return;
    std::array<char, 32> request{};
    for (;;) {
        sockaddr_in sender{};
        int sender_size = sizeof(sender);
        const int received = recvfrom(socket, request.data(), static_cast<int>(request.size()), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (received == SOCKET_ERROR) {
            if (!is_would_block())
                stop();
            return;
        }
        if (received != static_cast<int>(kRequest.size()) ||
            std::memcmp(request.data(), kRequest.data(), kRequest.size()) != 0)
            continue;
        if (!advertisement_packet_.empty()) {
            (void)sendto(
                socket,
                reinterpret_cast<const char*>(advertisement_packet_.data()),
                static_cast<int>(advertisement_packet_.size()), 0,
                reinterpret_cast<const sockaddr*>(&sender), sender_size);
        } else {
            std::array<char, kResponseSize> response{};
            std::memcpy(response.data(), kResponse.data(), kResponse.size());
            const std::uint16_t network_port = htons(game_port_);
            std::memcpy(response.data() + kResponse.size(), &network_port,
                        sizeof(network_port));
            (void)sendto(socket, response.data(), static_cast<int>(response.size()), 0,
                         reinterpret_cast<const sockaddr*>(&sender), sender_size);
        }
    }
}

void LanDiscovery::stop() noexcept
{
    const SOCKET socket = as_socket(socket_);
    socket_ = as_handle(INVALID_SOCKET);
    game_port_ = 0;
    advertisement_.reset();
    advertisement_packet_.clear();
    close_socket(socket);
}

bool LanDiscovery::hosting() const noexcept
{
    return as_socket(socket_) != INVALID_SOCKET;
}

} // namespace kraken::net
