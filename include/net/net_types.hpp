#ifndef KRAKEN_NET_NET_TYPES_HPP
#define KRAKEN_NET_NET_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace kraken::net {

using Byte = std::byte;
using ByteView = std::span<const Byte>;
using MutableByteView = std::span<Byte>;
using PeerId = std::uint32_t;

inline constexpr PeerId kInvalidPeer = 0;
inline constexpr std::uint16_t kDefaultPort = 27015;
inline constexpr std::uint32_t kDefaultMaxPacketSize = 1024u * 1024u + 16u;

enum class Channel : std::uint8_t {
    Reliable = 0,
    Unreliable = 1,
};

[[nodiscard]] constexpr bool is_valid_channel(Channel channel) noexcept
{
    return channel == Channel::Reliable || channel == Channel::Unreliable;
}

enum class TransportRole : std::uint8_t {
    Client,
    Server,
};

struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = kDefaultPort;
};

struct TransportConfig {
    TransportRole role = TransportRole::Client;
    Endpoint bind_endpoint{};
    std::uint32_t max_peers = 16;
    std::uint32_t receive_limit = 64;
    std::uint32_t max_packet_size = kDefaultMaxPacketSize;
};

enum class TransportResultCode : std::uint8_t {
    Ok,
    AlreadyRunning,
    NotRunning,
    InvalidArgument,
    WouldBlock,
    Disconnected,
    BufferTooSmall,
    ProtocolError,
    BackendUnavailable,
    BackendError,
};

struct TransportResult {
    TransportResultCode code = TransportResultCode::Ok;

    [[nodiscard]] constexpr bool succeeded() const noexcept
    {
        return code == TransportResultCode::Ok;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return succeeded();
    }
};

enum class TransportEventType : std::uint8_t {
    Connected,
    Disconnected,
    Packet,
    Error,
};

struct TransportEvent {
    TransportEventType type = TransportEventType::Error;
    PeerId peer = kInvalidPeer;
    Channel channel = Channel::Reliable;
    TransportResult result{};
    std::vector<Byte> payload;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual TransportResult start(const TransportConfig& config) = 0;
    virtual TransportResult listen() = 0;
    virtual TransportResult connect(const Endpoint& endpoint) = 0;
    virtual TransportResult poll(std::span<TransportEvent> events,
                                 std::size_t& event_count) = 0;
    virtual TransportResult send(PeerId peer, Channel channel,
                                 ByteView payload) = 0;
    virtual TransportResult disconnect(PeerId peer) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
};

static_assert(sizeof(PeerId) == sizeof(std::uint32_t));
static_assert(std::is_enum_v<Channel>);
static_assert(static_cast<std::uint8_t>(Channel::Reliable) == 0);
static_assert(static_cast<std::uint8_t>(Channel::Unreliable) == 1);
static_assert(!std::is_copy_constructible_v<ITransport>);

} // namespace kraken::net

#endif // KRAKEN_NET_NET_TYPES_HPP
