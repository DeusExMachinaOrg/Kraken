#include "net/session.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace kraken::net {
namespace {

TransportResult result(TransportResultCode code)
{
    return {code};
}

bool is_control_message(MessageType type)
{
    return type == MessageType::Hello || type == MessageType::Welcome ||
           type == MessageType::Ping || type == MessageType::Pong ||
           type == MessageType::Disconnect;
}

SessionIdentity identity_from_config(const SessionConfig& config)
{
    return {config.protocol_version, config.kraken_version,
            config.game_version, config.mod_version,
            config.resource_fingerprint};
}

SessionCompatibilityCodecError encode_identity(
    const SessionIdentity& identity, std::vector<Byte>& payload)
{
    payload.resize(session_identity_wire_size(identity));
    std::size_t bytes_written = 0;
    const SessionCompatibilityCodecError error = encode_session_identity(
        identity, MutableByteView{payload}, bytes_written);
    if (error == SessionCompatibilityCodecError::None)
        payload.resize(bytes_written);
    else
        payload.clear();
    return error;
}

WireDecodeError compatibility_error_to_wire_error(
    SessionCompatibilityCodecError error)
{
    switch (error) {
    case SessionCompatibilityCodecError::BadVersion:
        return WireDecodeError::BadVersion;
    case SessionCompatibilityCodecError::FieldTooLarge:
        return WireDecodeError::PayloadTooLarge;
    case SessionCompatibilityCodecError::None:
        return WireDecodeError::None;
    default:
        return WireDecodeError::BadPayloadSize;
    }
}

} // namespace

Session::Session(ITransport& transport, SessionConfig config)
    : m_transport(&transport), m_config(std::move(config))
{
    m_config.transport.role =
        m_config.role == SessionRole::Server ? TransportRole::Server
                                             : TransportRole::Client;
    if (m_config.max_payload > kMaxWirePayload)
        m_config.max_payload = kMaxWirePayload;
}

TransportResult Session::start()
{
    if (!m_transport)
        return result(TransportResultCode::InvalidArgument);
    if (m_state != SessionState::Idle && m_state != SessionState::Closed)
        return result(TransportResultCode::AlreadyRunning);
    if (!is_valid_session_identity(identity()))
        return result(TransportResultCode::InvalidArgument);

    if (m_state == SessionState::Closed)
        m_state = SessionState::Idle;

    const TransportResult started = m_transport->start(m_config.transport);
    if (!started)
        return started;

    m_peers.clear();
    m_events.clear();
    m_default_peer = kInvalidPeer;
    const SessionState initial = m_config.role == SessionRole::Server
                                     ? SessionState::Listening
                                     : SessionState::Ready;
    if (!transition(initial)) {
        m_transport->stop();
        mark_failed();
        return result(TransportResultCode::BackendError);
    }
    if (m_config.role == SessionRole::Server) {
        const TransportResult listening = m_transport->listen();
        if (!listening) {
            m_transport->stop();
            mark_failed();
            return listening;
        }
    }
    return {};
}

TransportResult Session::connect(const Endpoint& endpoint)
{
    if (!m_transport || !m_transport->running())
        return result(TransportResultCode::NotRunning);
    if (m_config.role != SessionRole::Client || m_state != SessionState::Ready)
        return result(TransportResultCode::InvalidArgument);

    const TransportResult connecting = m_transport->connect(endpoint);
    if (!connecting)
        return connecting;
    if (!transition(SessionState::Connecting))
        return result(TransportResultCode::BackendError);
    return {};
}

TransportResult Session::pump(std::size_t max_events)
{
    if (!m_transport || !m_transport->running())
        return result(TransportResultCode::NotRunning);
    if (max_events == 0)
        return {};

    std::vector<TransportEvent> events(max_events);
    std::size_t event_count = 0;
    const TransportResult polled =
        m_transport->poll(std::span<TransportEvent>{events}, event_count);
    if (!polled)
        return polled;

    TransportResult final_result{};
    for (std::size_t index = 0; index < event_count; ++index) {
        const TransportResult handled =
            handle_transport_event(std::move(events[index]));
        if (!handled)
            final_result = handled;
    }
    return final_result;
}

TransportResult Session::send(PeerId peer, MessageType type, Channel channel,
                              ByteView payload)
{
    if (!is_valid_message_type(type) || !is_valid_channel(channel))
        return result(TransportResultCode::InvalidArgument);
    if (requires_reliable_channel(type) && channel != Channel::Reliable)
        return result(TransportResultCode::InvalidArgument);
    if (is_control_message(type))
        return result(TransportResultCode::InvalidArgument);
    return send_frame(peer, type, channel, payload, true);
}

TransportResult Session::ping(PeerId peer)
{
    PeerState* state = find_peer(peer);
    if (!state || state->state != SessionState::Connected)
        return result(TransportResultCode::Disconnected);
    if (state->ping_pending)
        return result(TransportResultCode::WouldBlock);

    const TransportResult sent = send_frame(
        peer, MessageType::Ping, Channel::Reliable, {}, false);
    if (sent) {
        state->ping_sent = std::chrono::steady_clock::now();
        state->ping_pending = true;
    }
    return sent;
}

void Session::stop() noexcept
{
    if (m_transport)
        m_transport->stop();
    m_peers.clear();
    m_events.clear();
    m_default_peer = kInvalidPeer;
    if (m_state != SessionState::Closed) {
        if (can_transition(m_state, SessionState::Closing))
            m_state = SessionState::Closing;
        if (can_transition(m_state, SessionState::Closed))
            m_state = SessionState::Closed;
        else
            m_state = SessionState::Closed;
    }
}

bool Session::running() const noexcept
{
    return m_transport != nullptr && m_transport->running() &&
           m_state != SessionState::Closed && m_state != SessionState::Idle &&
           m_state != SessionState::Failed;
}

std::size_t Session::drain_events(std::span<SessionEvent> output)
{
    const std::size_t count = (std::min)(output.size(), m_events.size());
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = std::move(m_events.front());
        m_events.pop_front();
    }
    return count;
}

TransportResult Session::handle_transport_event(TransportEvent&& event)
{
    switch (event.type) {
    case TransportEventType::Connected: {
        if (event.peer == kInvalidPeer)
            return result(TransportResultCode::ProtocolError);

        m_peers[event.peer] = PeerState{};
        if (m_config.role == SessionRole::Client) {
            if (m_state != SessionState::Connecting)
                return result(TransportResultCode::ProtocolError);
            m_default_peer = event.peer;
            if (!transition(SessionState::Handshaking))
                return result(TransportResultCode::ProtocolError);
            std::vector<Byte> payload;
            if (const SessionCompatibilityCodecError error =
                    encode_identity(identity(), payload);
                error != SessionCompatibilityCodecError::None) {
                (void)m_transport->disconnect(event.peer);
                return result(TransportResultCode::InvalidArgument);
            }
            const TransportResult sent = send_frame(
                event.peer, MessageType::Hello, Channel::Reliable, payload,
                false);
            if (!sent)
                (void)m_transport->disconnect(event.peer);
            return sent;
        }
        return {};
    }
    case TransportEventType::Disconnected: {
        if (event.peer == kInvalidPeer)
            return result(TransportResultCode::ProtocolError);
        m_peers.erase(event.peer);
        emit(SessionEvent{SessionEventType::PeerDisconnected, event.peer});
        if (m_config.role == SessionRole::Client &&
            event.peer == m_default_peer) {
            m_default_peer = kInvalidPeer;
            if (m_state == SessionState::Connected ||
                m_state == SessionState::Handshaking ||
                m_state == SessionState::Connecting)
                (void)transition(SessionState::Ready);
        }
        return {};
    }
    case TransportEventType::Packet:
        return handle_packet(std::move(event));
    case TransportEventType::Error:
        return event.result.succeeded() ? result(TransportResultCode::BackendError)
                                        : event.result;
    }
    return result(TransportResultCode::BackendError);
}

TransportResult Session::handle_packet(TransportEvent&& event)
{
    PeerState* peer = find_peer(event.peer);
    if (!peer)
        return result(TransportResultCode::ProtocolError);

    WireHeader header{};
    ByteView payload{};
    const WireDecodeError error =
        decode_frame(ByteView{event.payload}, header, payload);
    if (error != WireDecodeError::None) {
        emit(SessionEvent{SessionEventType::ProtocolError, event.peer,
                          MessageType::Disconnect, event.channel, error});
        if (header.message_type == MessageType::Hello ||
            header.message_type == MessageType::Welcome)
            (void)m_transport->disconnect(event.peer);
        return result(TransportResultCode::ProtocolError);
    }
    if (header.channel != event.channel ||
        header.payload_size > m_config.max_payload) {
        emit(SessionEvent{SessionEventType::ProtocolError, event.peer,
                          header.message_type, header.channel,
                          WireDecodeError::BadChannel});
        if (header.message_type == MessageType::Hello ||
            header.message_type == MessageType::Welcome)
            (void)m_transport->disconnect(event.peer);
        return result(TransportResultCode::ProtocolError);
    }
    if (requires_reliable_channel(header.message_type) &&
        header.channel != Channel::Reliable) {
        emit(SessionEvent{SessionEventType::ProtocolError, event.peer,
                          header.message_type, header.channel,
                          WireDecodeError::BadChannel});
        return result(TransportResultCode::ProtocolError);
    }

    if (is_control_message(header.message_type)) {
        const bool identity_message =
            header.message_type == MessageType::Hello ||
            header.message_type == MessageType::Welcome;
        if (!identity_message && !payload.empty()) {
            (void)m_transport->disconnect(event.peer);
            return result(TransportResultCode::ProtocolError);
        }
        return handle_control(event.peer, header, payload);
    }
    if (peer->state != SessionState::Connected)
        return result(TransportResultCode::ProtocolError);

    SessionEvent message{SessionEventType::Message, event.peer,
                         header.message_type, header.channel};
    message.payload.assign(payload.begin(), payload.end());
    emit(std::move(message));
    return {};
}

TransportResult Session::handle_control(PeerId peer_id,
                                         const WireHeader& header,
                                         ByteView payload)
{
    PeerState* peer = find_peer(peer_id);
    if (!peer)
        return result(TransportResultCode::ProtocolError);

    switch (header.message_type) {
    case MessageType::Hello: {
        if (m_config.role != SessionRole::Server ||
            peer->state != SessionState::Handshaking ||
            header.channel != Channel::Reliable)
            return reject_peer(peer_id, MessageType::Hello, header.channel,
                               WireDecodeError::BadChannel);

        {
            SessionIdentity received{};
            const SessionCompatibilityCodecError decoded =
                decode_session_identity(payload, received);
            if (decoded != SessionCompatibilityCodecError::None)
                return reject_peer(
                    peer_id, MessageType::Hello, header.channel,
                    compatibility_error_to_wire_error(decoded));
            if (received != identity())
                return reject_peer(peer_id, MessageType::Hello,
                                   header.channel, WireDecodeError::BadVersion);
        }

        std::vector<Byte> welcome_payload;
        if (const SessionCompatibilityCodecError error =
                encode_identity(identity(), welcome_payload);
            error != SessionCompatibilityCodecError::None)
            return reject_peer(peer_id, MessageType::Welcome,
                               header.channel,
                               compatibility_error_to_wire_error(error));
        if (const TransportResult sent =
                send_frame(peer_id, MessageType::Welcome, Channel::Reliable,
                           welcome_payload, false);
            !sent) {
            (void)m_transport->disconnect(peer_id);
            return sent;
        }
        peer->state = SessionState::Connected;
        emit(SessionEvent{SessionEventType::PeerConnected, peer_id});
        return {};
    }

    case MessageType::Welcome: {
        if (m_config.role != SessionRole::Client ||
            peer->state != SessionState::Handshaking ||
            header.channel != Channel::Reliable)
            return reject_peer(peer_id, MessageType::Welcome,
                               header.channel, WireDecodeError::BadChannel);
        {
            SessionIdentity received{};
            const SessionCompatibilityCodecError decoded =
                decode_session_identity(payload, received);
            if (decoded != SessionCompatibilityCodecError::None)
                return reject_peer(
                    peer_id, MessageType::Welcome, header.channel,
                    compatibility_error_to_wire_error(decoded));
            if (received != identity())
                return reject_peer(peer_id, MessageType::Welcome,
                                   header.channel, WireDecodeError::BadVersion);
        }
        peer->state = SessionState::Connected;
        if (!transition(SessionState::Connected))
            return result(TransportResultCode::ProtocolError);
        emit(SessionEvent{SessionEventType::PeerConnected, peer_id});
        return {};
    }

    case MessageType::Ping:
        if (peer->state != SessionState::Connected ||
            header.channel != Channel::Reliable)
            return result(TransportResultCode::ProtocolError);
        return send_frame(peer_id, MessageType::Pong, Channel::Reliable, {},
                          false);

    case MessageType::Pong:
        if (peer->state != SessionState::Connected)
            return result(TransportResultCode::ProtocolError);
        if (header.channel != Channel::Reliable)
            return result(TransportResultCode::ProtocolError);
        if (peer->ping_pending) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - peer->ping_sent);
            peer->ping_pending = false;
            SessionEvent event{SessionEventType::RoundTripTime, peer_id};
            event.round_trip_time_ms = static_cast<std::uint32_t>(
                (std::min)(elapsed.count(),
                           static_cast<decltype(elapsed.count())>(
                               (std::numeric_limits<std::uint32_t>::max)())));
            emit(std::move(event));
        }
        return {};

    case MessageType::Disconnect:
        (void)m_transport->disconnect(peer_id);
        return {};

    case MessageType::Input:
    case MessageType::Snapshot:
    case MessageType::EntityAssign:
    case MessageType::WeaponCommand:
    case MessageType::LootRequest:
    case MessageType::LootResult:
    case MessageType::ImpactDamage:
    case MessageType::WorldLootSpawn:
    case MessageType::WorldLootBaseline:
    case MessageType::WorldLootDelta:
    case MessageType::WorldLootRemove:
    case MessageType::WorldLootPickupRequest:
    case MessageType::WorldLootPickupResult:
        break;
    }
    return result(TransportResultCode::ProtocolError);
}

SessionIdentity Session::identity() const
{
    return identity_from_config(m_config);
}

TransportResult Session::reject_peer(PeerId peer, MessageType message,
                                      Channel channel,
                                      WireDecodeError error)
{
    emit(SessionEvent{SessionEventType::ProtocolError, peer, message, channel,
                      error});
    if (m_transport)
        (void)m_transport->disconnect(peer);
    return result(TransportResultCode::ProtocolError);
}

TransportResult Session::send_frame(PeerId peer, MessageType type,
                                     Channel channel, ByteView payload,
                                     bool require_connected)
{
    if (!m_transport || !m_transport->running() || peer == kInvalidPeer ||
        !is_valid_message_type(type) || !is_valid_channel(channel) ||
        payload.size() > m_config.max_payload ||
        payload.size() > kMaxWirePayload)
        return result(TransportResultCode::InvalidArgument);

    PeerState* peer_state = find_peer(peer);
    if (!peer_state || (require_connected &&
                        peer_state->state != SessionState::Connected))
        return result(TransportResultCode::Disconnected);

    if (peer_state->next_sequence == 0)
        peer_state->next_sequence = 1;
    WireHeader header{kWireMagic,
                      kWireVersion,
                      type,
                      channel,
                      0,
                      peer_state->next_sequence++,
                      static_cast<std::uint32_t>(payload.size())};

    std::vector<Byte> frame(kWireHeaderSize + payload.size());
    encode_header(header, MutableByteView{frame}.first(kWireHeaderSize));
    std::copy(payload.begin(), payload.end(),
              frame.begin() + static_cast<std::ptrdiff_t>(kWireHeaderSize));
    return m_transport->send(peer, channel, ByteView{frame});
}

void Session::emit(SessionEvent event)
{
    m_events.push_back(std::move(event));
}

void Session::mark_failed()
{
    if (can_transition(m_state, SessionState::Failed))
        m_state = SessionState::Failed;
    else
        m_state = SessionState::Failed;
}

bool Session::transition(SessionState next) noexcept
{
    if (!can_transition(m_state, next))
        return false;
    m_state = next;
    return true;
}

Session::PeerState* Session::find_peer(PeerId peer) noexcept
{
    const auto iterator = m_peers.find(peer);
    return iterator == m_peers.end() ? nullptr : &iterator->second;
}

const Session::PeerState* Session::find_peer(PeerId peer) const noexcept
{
    const auto iterator = m_peers.find(peer);
    return iterator == m_peers.end() ? nullptr : &iterator->second;
}

} // namespace kraken::net
