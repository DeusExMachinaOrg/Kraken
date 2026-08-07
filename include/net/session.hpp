#ifndef KRAKEN_NET_SESSION_HPP
#define KRAKEN_NET_SESSION_HPP

#include "net/transport.hpp"
#include "net/wire_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

namespace kraken::net {

enum class SessionRole : std::uint8_t {
    Client,
    Server,
};

enum class SessionState : std::uint8_t {
    Idle,
    Ready,
    Listening,
    Connecting,
    Handshaking,
    Connected,
    Closing,
    Closed,
    Failed,
};

enum class SessionEventType : std::uint8_t {
    PeerConnected,
    PeerDisconnected,
    RoundTripTime,
    Message,
    ProtocolError,
};

struct SessionConfig {
    SessionRole role = SessionRole::Client;
    TransportConfig transport{};
    std::uint32_t max_payload = kMaxWirePayload;
};

struct SessionEvent {
    SessionEventType type = SessionEventType::ProtocolError;
    PeerId peer = kInvalidPeer;
    MessageType message_type = MessageType::Disconnect;
    Channel channel = Channel::Reliable;
    WireDecodeError protocol_error = WireDecodeError::None;
    std::uint32_t round_trip_time_ms = 0;
    std::vector<Byte> payload;
};

[[nodiscard]] constexpr bool can_transition(SessionState from,
                                             SessionState to) noexcept
{
    if (from == to)
        return true;

    switch (from) {
    case SessionState::Idle:
        return to == SessionState::Ready || to == SessionState::Listening ||
               to == SessionState::Closed || to == SessionState::Failed;
    case SessionState::Ready:
        return to == SessionState::Connecting || to == SessionState::Closing ||
               to == SessionState::Closed || to == SessionState::Failed;
    case SessionState::Listening:
        return to == SessionState::Closing || to == SessionState::Closed ||
               to == SessionState::Failed;
    case SessionState::Connecting:
        return to == SessionState::Handshaking || to == SessionState::Ready ||
               to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::Handshaking:
        return to == SessionState::Connected || to == SessionState::Ready ||
               to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::Connected:
        return to == SessionState::Ready || to == SessionState::Closing ||
               to == SessionState::Failed;
    case SessionState::Closing:
        return to == SessionState::Closed;
    case SessionState::Closed:
        return to == SessionState::Ready || to == SessionState::Listening;
    case SessionState::Failed:
        return to == SessionState::Closing || to == SessionState::Closed;
    }
    return false;
}

class Session final {
public:
    Session(ITransport& transport, SessionConfig config = {});
    ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    TransportResult start();
    TransportResult connect(const Endpoint& endpoint);
    TransportResult pump(std::size_t max_events = 64);
    TransportResult send(PeerId peer, MessageType type, Channel channel,
                         ByteView payload = {});
    TransportResult ping(PeerId peer);
    void stop() noexcept;

    [[nodiscard]] SessionState state() const noexcept { return m_state; }
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t drain_events(std::span<SessionEvent> output);

private:
    struct PeerState {
        SessionState state = SessionState::Handshaking;
        std::uint32_t next_sequence = 1;
        std::chrono::steady_clock::time_point ping_sent{};
        bool ping_pending = false;
    };

    TransportResult handle_transport_event(TransportEvent&& event);
    TransportResult handle_packet(TransportEvent&& event);
    TransportResult handle_control(PeerId peer, const WireHeader& header);
    TransportResult send_frame(PeerId peer, MessageType type, Channel channel,
                                ByteView payload, bool require_connected);
    void emit(SessionEvent event);
    void mark_failed();
    bool transition(SessionState next) noexcept;
    PeerState* find_peer(PeerId peer) noexcept;
    const PeerState* find_peer(PeerId peer) const noexcept;

    ITransport* m_transport = nullptr;
    SessionConfig m_config{};
    SessionState m_state = SessionState::Idle;
    PeerId m_default_peer = kInvalidPeer;
    std::unordered_map<PeerId, PeerState> m_peers;
    std::deque<SessionEvent> m_events;
};

static_assert(can_transition(SessionState::Idle, SessionState::Ready));
static_assert(can_transition(SessionState::Connecting,
                             SessionState::Handshaking));
static_assert(can_transition(SessionState::Handshaking,
                             SessionState::Connected));
static_assert(!can_transition(SessionState::Idle, SessionState::Connected));
static_assert(!can_transition(SessionState::Connected,
                              SessionState::Handshaking));

} // namespace kraken::net

#endif // KRAKEN_NET_SESSION_HPP
