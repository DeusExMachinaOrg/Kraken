#define LOGGER "multiplayer"

#include "net/runtime.hpp"

#include "config.hpp"
#include "ext/logger.hpp"
#include "net/session.hpp"
#include "net/transport.hpp"
#include "routines.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kraken::net::runtime {
namespace {

constexpr uintptr_t kServerUpdateCallSite = 0x005C809D;
constexpr uintptr_t kServerUpdateAddress = 0x005F4090;

using Clock = std::chrono::steady_clock;
using ServerUpdateFn = void(__fastcall*)(void*, void*, float);

struct RuntimeState {
    EnetTransport transport;
    std::unique_ptr<Session> session;
    std::vector<PeerId> peers;
    Clock::time_point next_ping{};
    bool hook_installed = false;
};

RuntimeState g_state;
ServerUpdateFn g_server_update =
    reinterpret_cast<ServerUpdateFn>(kServerUpdateAddress);

struct EffectiveConfig {
    bool enabled = false;
    bool host = true;
    std::string address = "127.0.0.1";
    std::uint16_t port = kDefaultPort;
    std::uint32_t max_peers = 16;
};

std::optional<std::string> environment(const char* name)
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value, length > 0 ? length - 1 : 0);
    std::free(value);
    return result;
}

std::uint32_t environment_uint(const char* name, std::uint32_t fallback,
                               std::uint32_t minimum,
                               std::uint32_t maximum)
{
    const auto value = environment(name);
    if (!value || value->empty())
        return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value->c_str(), &end, 10);
    if (end == value->c_str() || *end != '\0')
        return fallback;
    return (std::clamp)(static_cast<std::uint32_t>(parsed), minimum, maximum);
}

EffectiveConfig effective_config(const Config& config)
{
    EffectiveConfig result;
    result.enabled = environment_uint("KRAKEN_MP_ENABLED",
                                      config.multiplayer_enabled.value, 0, 1) != 0;
    result.host = environment_uint("KRAKEN_MP_HOST",
                                   config.multiplayer_host.value, 0, 1) != 0;
    result.address = environment("KRAKEN_MP_ADDRESS")
                         .value_or(config.multiplayer_address.value);
    result.port = static_cast<std::uint16_t>(environment_uint(
        "KRAKEN_MP_PORT", config.multiplayer_port.value, 1024, 65535));
    result.max_peers = environment_uint("KRAKEN_MP_MAX_PEERS",
                                        config.multiplayer_max_peers.value, 2, 16);
    return result;
}

void handle_event(SessionEvent&& event)
{
    switch (event.type) {
    case SessionEventType::PeerConnected:
        if (std::find(g_state.peers.begin(), g_state.peers.end(), event.peer) ==
            g_state.peers.end())
            g_state.peers.push_back(event.peer);
        LOG_INFO("peer=%u handshake complete", event.peer);
        (void)g_state.session->ping(event.peer);
        break;

    case SessionEventType::PeerDisconnected:
        std::erase(g_state.peers, event.peer);
        LOG_INFO("peer=%u disconnected", event.peer);
        break;

    case SessionEventType::RoundTripTime:
        LOG_INFO("peer=%u rtt=%u ms", event.peer, event.round_trip_time_ms);
        break;

    case SessionEventType::Message:
        LOG_DEBUG("peer=%u message=%u bytes=%u channel=%u", event.peer,
                  static_cast<unsigned>(event.message_type),
                  static_cast<unsigned>(event.payload.size()),
                  static_cast<unsigned>(event.channel));
        break;

    case SessionEventType::ProtocolError:
        LOG_ERROR("peer=%u protocol error=%u", event.peer,
                  static_cast<unsigned>(event.protocol_error));
        break;
    }
}

void pump()
{
    if (!g_state.session)
        return;

    const TransportResult result = g_state.session->pump();
    if (!result && result.code != TransportResultCode::WouldBlock)
        LOG_ERROR("network pump failed code=%u", static_cast<unsigned>(result.code));

    std::array<SessionEvent, 64> events{};
    for (;;) {
        const std::size_t count = g_state.session->drain_events(events);
        if (count == 0)
            break;
        for (std::size_t index = 0; index < count; ++index)
            handle_event(std::move(events[index]));
    }

    const Clock::time_point now = Clock::now();
    if (now < g_state.next_ping)
        return;
    g_state.next_ping = now + std::chrono::seconds(1);
    for (const PeerId peer : g_state.peers)
        (void)g_state.session->ping(peer);
}

// The original call is ai::CServer::Update(float): ECX=this, float on stack.
// A free __fastcall hook reserves EDX as the second dummy argument.
void __fastcall server_update_hook(void* server, void*, float elapsed_time)
{
    // Receive/apply packets before native gameplay and ODE advance.
    pump();
    g_server_update(server, nullptr, elapsed_time);
    // M1 snapshot capture will be added here, after the authoritative frame.
}

} // namespace

void Apply(const Config* config)
{
    if (!config)
        return;
    const EffectiveConfig effective = effective_config(*config);
    if (!effective.enabled)
        return;
    if (g_state.session)
        return;

    SessionConfig session_config{};
    session_config.role = effective.host
                              ? SessionRole::Server
                              : SessionRole::Client;
    session_config.transport.role = session_config.role == SessionRole::Server
                                        ? TransportRole::Server
                                        : TransportRole::Client;
    session_config.transport.bind_endpoint.host =
        session_config.role == SessionRole::Server ? "0.0.0.0" : "127.0.0.1";
    session_config.transport.bind_endpoint.port =
        effective.port;
    session_config.transport.max_peers = effective.max_peers;

    g_state.session =
        std::make_unique<Session>(g_state.transport, std::move(session_config));
    TransportResult result = g_state.session->start();
    if (!result) {
        LOG_ERROR("failed to start network role=%s port=%u code=%u",
                  effective.host ? "host" : "client",
                  effective.port,
                  static_cast<unsigned>(result.code));
        g_state.session.reset();
        return;
    }

    if (!effective.host) {
        Endpoint endpoint{effective.address, effective.port};
        result = g_state.session->connect(endpoint);
        if (!result) {
            LOG_ERROR("failed to connect to %s:%u code=%u", endpoint.host.c_str(),
                      endpoint.port, static_cast<unsigned>(result.code));
            g_state.session->stop();
            g_state.session.reset();
            return;
        }
    }

    try {
        routines::ChangeCall(reinterpret_cast<void*>(kServerUpdateCallSite),
                             &server_update_hook);
        g_state.hook_installed = true;
    }
    catch (const std::exception& error) {
        LOG_ERROR("failed to install server tick hook: %s", error.what());
        g_state.session->stop();
        g_state.session.reset();
        return;
    }

    g_state.next_ping = Clock::now() + std::chrono::seconds(1);
    LOG_INFO("network started role=%s endpoint=%s:%u max_peers=%u",
             effective.host ? "host" : "client",
             effective.host ? "0.0.0.0" : effective.address.c_str(),
             effective.port, effective.max_peers);
}

} // namespace kraken::net::runtime
