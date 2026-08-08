#include "net/lan_discovery.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

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

} // namespace

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
    const sockaddr_in target = ipv4_address(target_address, discovery_port);
    if (target.sin_addr.s_addr == INADDR_NONE ||
        sendto(socket, kRequest.data(), static_cast<int>(kRequest.size()), 0,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == SOCKET_ERROR) {
        close_socket(socket);
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 32> reply{};
    while (std::chrono::steady_clock::now() < deadline) {
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
