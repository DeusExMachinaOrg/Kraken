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
#include <mutex>
#include <string>
#include <vector>

namespace kraken::net {
namespace {

constexpr std::array<char, 8> kRequest{{'K', 'R', 'N', 'L', 'A', 'N', '1', '\0'}};
constexpr std::array<char, 8> kResponse{{'K', 'R', 'N', 'H', 'O', 'S', 'T', '1'}};
constexpr std::size_t kResponseSize = kResponse.size() + sizeof(std::uint16_t);

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
        std::array<char, kResponseSize> response{};
        std::memcpy(response.data(), kResponse.data(), kResponse.size());
        const std::uint16_t network_port = htons(game_port_);
        std::memcpy(response.data() + kResponse.size(), &network_port,
                    sizeof(network_port));
        (void)sendto(socket, response.data(), static_cast<int>(response.size()), 0,
                     reinterpret_cast<const sockaddr*>(&sender), sender_size);
    }
}

void LanDiscovery::stop() noexcept
{
    const SOCKET socket = as_socket(socket_);
    socket_ = as_handle(INVALID_SOCKET);
    game_port_ = 0;
    close_socket(socket);
}

bool LanDiscovery::hosting() const noexcept
{
    return as_socket(socket_) != INVALID_SOCKET;
}

} // namespace kraken::net
