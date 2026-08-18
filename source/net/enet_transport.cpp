#include "net/transport.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

#if KRAKEN_NET_ENABLE_ENET
#include <enet/enet.h>
#endif

namespace kraken::net {

struct EnetTransport::Impl {
    TransportConfig config{};
    bool running = false;

#if KRAKEN_NET_ENABLE_ENET
    ENetHost* host = nullptr;
    PeerId next_peer = 1;
#endif
};

#if KRAKEN_NET_ENABLE_ENET
namespace {

std::mutex g_enet_mutex;
std::uint32_t g_enet_users = 0;

bool acquire_enet()
{
    std::lock_guard lock(g_enet_mutex);
    if (g_enet_users == 0 && enet_initialize() != 0)
        return false;
    ++g_enet_users;
    return true;
}

void release_enet() noexcept
{
    std::lock_guard lock(g_enet_mutex);
    if (g_enet_users == 0)
        return;
    --g_enet_users;
    if (g_enet_users == 0)
        enet_deinitialize();
}

PeerId peer_id(ENetPeer* peer, PeerId& next_peer, bool allocate)
{
    if (!peer)
        return kInvalidPeer;

    const auto existing = static_cast<PeerId>(
        reinterpret_cast<std::uintptr_t>(peer->data));
    if (existing != kInvalidPeer || !allocate)
        return existing;

    PeerId id = next_peer++;
    if (id == kInvalidPeer)
        id = next_peer++;
    peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    return id;
}

ENetPeer* find_peer(ENetHost* host, PeerId id)
{
    if (!host || id == kInvalidPeer)
        return nullptr;

    for (std::size_t index = 0; index < host->peerCount; ++index) {
        ENetPeer& peer = host->peers[index];
        if (static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(peer.data)) ==
            id)
            return &peer;
    }
    return nullptr;
}

TransportResult backend_error()
{
    return {TransportResultCode::BackendError};
}

} // namespace
#endif

EnetTransport::EnetTransport() : m_impl(std::make_unique<Impl>()) {}

EnetTransport::~EnetTransport()
{
    stop();
}

TransportResult EnetTransport::start(const TransportConfig& config)
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
    if (m_impl->running)
        return {TransportResultCode::AlreadyRunning};
    if (config.max_peers == 0 ||
        config.max_peers > kMaxTransportPeerCapacity ||
        config.receive_limit == 0 ||
        config.max_packet_size == 0 ||
        (config.role == TransportRole::Server &&
         config.bind_endpoint.port == 0))
        return {TransportResultCode::InvalidArgument};

#if !KRAKEN_NET_ENABLE_ENET
    (void)config;
    return {TransportResultCode::BackendUnavailable};
#else
    if (!acquire_enet())
        return backend_error();

    ENetHost* host = nullptr;
    if (config.role == TransportRole::Server) {
        ENetAddress address{};
        if (enet_address_set_host(&address, config.bind_endpoint.host.c_str()) !=
            0) {
            release_enet();
            return {TransportResultCode::InvalidArgument};
        }
        address.port = config.bind_endpoint.port;
        host = enet_host_create(&address, config.max_peers, 2, 0, 0);
    }
    else {
        // A client host binds an ephemeral local port and needs only its one
        // server peer; max_peers is intentionally ignored by ENet here.
        host = enet_host_create(nullptr, 1, 2, 0, 0);
    }

    if (!host) {
        release_enet();
        return backend_error();
    }

    m_impl->config = config;
    m_impl->host = host;
    m_impl->next_peer = 1;
    m_impl->running = true;
    return {};
#endif
}

TransportResult EnetTransport::listen()
{
    if (!m_impl || !m_impl->running)
        return {TransportResultCode::NotRunning};
#if !KRAKEN_NET_ENABLE_ENET
    return {TransportResultCode::BackendUnavailable};
#else
    if (m_impl->config.role != TransportRole::Server)
        return {TransportResultCode::InvalidArgument};
    return {};
#endif
}

TransportResult EnetTransport::connect(const Endpoint& endpoint)
{
    if (!m_impl || !m_impl->running)
        return {TransportResultCode::NotRunning};
    if (endpoint.host.empty() || endpoint.port == 0)
        return {TransportResultCode::InvalidArgument};
#if !KRAKEN_NET_ENABLE_ENET
    (void)endpoint;
    return {TransportResultCode::BackendUnavailable};
#else
    if (m_impl->config.role != TransportRole::Client)
        return {TransportResultCode::InvalidArgument};

    ENetAddress address{};
    if (enet_address_set_host(&address, endpoint.host.c_str()) != 0)
        return {TransportResultCode::InvalidArgument};
    address.port = endpoint.port;

    ENetPeer* peer = enet_host_connect(m_impl->host, &address, 2, 0);
    if (!peer)
        return backend_error();
    (void)peer_id(peer, m_impl->next_peer, true);
    return {};
#endif
}

TransportResult EnetTransport::poll(std::span<TransportEvent> events,
                                    std::size_t& event_count)
{
    event_count = 0;
    if (!m_impl || !m_impl->running)
        return {TransportResultCode::NotRunning};
    if (events.empty())
        return {TransportResultCode::BufferTooSmall};
#if !KRAKEN_NET_ENABLE_ENET
    return {TransportResultCode::BackendUnavailable};
#else
    ENetEvent event{};
    while (event_count < events.size()) {
        const int service_result = enet_host_service(m_impl->host, &event, 0);
        if (service_result < 0)
            return backend_error();
        if (service_result == 0)
            break;

        TransportEvent& output = events[event_count++];
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            output.type = TransportEventType::Connected;
            output.peer = peer_id(event.peer, m_impl->next_peer, true);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            output.type = TransportEventType::Disconnected;
            output.peer = peer_id(event.peer, m_impl->next_peer, false);
            event.peer->data = nullptr;
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            output.type = TransportEventType::Packet;
            output.peer = peer_id(event.peer, m_impl->next_peer, false);
            if (event.channelID > 1) {
                enet_packet_destroy(event.packet);
                output.type = TransportEventType::Error;
                output.result = {TransportResultCode::ProtocolError};
                break;
            }
            if (event.packet->dataLength > m_impl->config.max_packet_size) {
                enet_packet_destroy(event.packet);
                output.type = TransportEventType::Error;
                output.result = {TransportResultCode::ProtocolError};
                break;
            }
            output.channel = event.channelID == 0 ? Channel::Reliable
                                                  : Channel::Unreliable;
            output.payload.resize(event.packet->dataLength);
            if (!output.payload.empty()) {
                std::memcpy(output.payload.data(), event.packet->data,
                            event.packet->dataLength);
            }
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            --event_count;
            break;
        }
    }
    return {};
#endif
}

TransportResult EnetTransport::send(PeerId peer, Channel channel,
                                    ByteView payload)
{
    if (!m_impl || !m_impl->running)
        return {TransportResultCode::NotRunning};
    if (!is_valid_channel(channel))
        return {TransportResultCode::InvalidArgument};
#if !KRAKEN_NET_ENABLE_ENET
    (void)peer;
    (void)payload;
    return {TransportResultCode::BackendUnavailable};
#else
    ENetPeer* target = find_peer(m_impl->host, peer);
    if (!target)
        return {TransportResultCode::Disconnected};

    const enet_uint32 flags = channel == Channel::Reliable
                                  ? ENET_PACKET_FLAG_RELIABLE
                                  : 0;
    ENetPacket* packet = enet_packet_create(payload.data(), payload.size(), flags);
    if (!packet)
        return backend_error();
    if (enet_peer_send(target, static_cast<enet_uint8>(channel), packet) != 0) {
        enet_packet_destroy(packet);
        return backend_error();
    }
    enet_host_flush(m_impl->host);
    return {};
#endif
}

TransportResult EnetTransport::disconnect(PeerId peer)
{
    if (!m_impl || !m_impl->running)
        return {TransportResultCode::NotRunning};
#if !KRAKEN_NET_ENABLE_ENET
    (void)peer;
    return {TransportResultCode::BackendUnavailable};
#else
    ENetPeer* target = find_peer(m_impl->host, peer);
    if (!target)
        return {TransportResultCode::Disconnected};
    enet_peer_disconnect(target, 0);
    return {};
#endif
}

void EnetTransport::stop() noexcept
{
    if (!m_impl || !m_impl->running)
        return;
#if KRAKEN_NET_ENABLE_ENET
    if (m_impl->host) {
        enet_host_destroy(m_impl->host);
        m_impl->host = nullptr;
    }
    release_enet();
#endif
    m_impl->running = false;
}

bool EnetTransport::running() const noexcept
{
    return m_impl != nullptr && m_impl->running;
}

} // namespace kraken::net
