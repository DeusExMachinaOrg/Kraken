#include "net/entity_registry.hpp"
#include "net/match_protocol.hpp"
#include "net/match_session.hpp"
#include "net/player_slots.hpp"
#include "net/session.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace kraken::net;

namespace {

class VirtualTransport;

class VirtualBus final {
public:
    void attach(std::uint32_t id, VirtualTransport* transport)
    {
        assert(id < endpoints.size());
        endpoints[id] = transport;
    }

    void detach(std::uint32_t id, VirtualTransport* transport)
    {
        if (id < endpoints.size() && endpoints[id] == transport)
            endpoints[id] = nullptr;
    }

    VirtualTransport* endpoint(std::uint32_t id) const
    {
        return id < endpoints.size() ? endpoints[id] : nullptr;
    }

    std::size_t attached() const
    {
        std::size_t count = 0;
        for (const auto* endpoint : endpoints)
            count += endpoint != nullptr;
        return count;
    }

private:
    std::array<VirtualTransport*, kMaxSessionPlayers> endpoints{};
};

class VirtualTransport final : public ITransport {
public:
    VirtualTransport(VirtualBus& bus, std::uint32_t id)
        : bus_(&bus), id_(id)
    {
        bus_->attach(id_, this);
    }

    ~VirtualTransport() override
    {
        stop();
        bus_->detach(id_, this);
    }

    TransportResult start(const TransportConfig& config) override
    {
        if (running_)
            return {TransportResultCode::AlreadyRunning};
        if (config.max_peers == 0 || config.max_peers > kMaxSessionPlayers)
            return {TransportResultCode::InvalidArgument};
        role_ = config.role;
        running_ = true;
        return {};
    }

    TransportResult listen() override
    {
        return running_ && role_ == TransportRole::Server
            ? TransportResult{} : TransportResult{TransportResultCode::NotRunning};
    }

    TransportResult connect(const Endpoint&) override
    {
        VirtualTransport* host = bus_->endpoint(0);
        if (!running_ || role_ != TransportRole::Client || host == nullptr ||
            !host->running_)
            return {TransportResultCode::NotRunning};
        connected_ = true;
        host->connected_clients_[id_] = true;
        queue_.push_back({TransportEventType::Connected, 1});
        host->queue_.push_back({TransportEventType::Connected, id_});
        return {};
    }

    TransportResult poll(std::span<TransportEvent> events,
                         std::size_t& event_count) override
    {
        if (!running_)
            return {TransportResultCode::NotRunning};
        event_count = (std::min)(events.size(), queue_.size());
        for (std::size_t index = 0; index < event_count; ++index) {
            events[index] = std::move(queue_.front());
            queue_.pop_front();
        }
        return {};
    }

    TransportResult send(PeerId peer, Channel channel, ByteView payload) override
    {
        if (!running_)
            return {TransportResultCode::NotRunning};
        VirtualTransport* target = nullptr;
        PeerId target_peer = kInvalidPeer;
        if (role_ == TransportRole::Server) {
            if (peer >= connected_clients_.size() || !connected_clients_[peer])
                return {TransportResultCode::Disconnected};
            target = bus_->endpoint(peer);
            target_peer = 1;
        } else {
            if (!connected_ || peer != 1)
                return {TransportResultCode::Disconnected};
            target = bus_->endpoint(0);
            target_peer = id_;
        }
        if (target == nullptr || !target->running_)
            return {TransportResultCode::Disconnected};
        TransportEvent event{};
        event.type = TransportEventType::Packet;
        event.peer = target_peer;
        event.channel = channel;
        event.payload.assign(payload.begin(), payload.end());
        target->queue_.push_back(std::move(event));
        return {};
    }

    TransportResult disconnect(PeerId peer) override
    {
        if (!running_)
            return {TransportResultCode::NotRunning};
        if (role_ == TransportRole::Server) {
            if (peer >= connected_clients_.size() || !connected_clients_[peer])
                return {TransportResultCode::Disconnected};
            connected_clients_[peer] = false;
            if (VirtualTransport* client = bus_->endpoint(peer)) {
                client->connected_ = false;
                client->queue_.push_back({TransportEventType::Disconnected, 1});
            }
            queue_.push_back({TransportEventType::Disconnected, peer});
        } else {
            if (!connected_ || peer != 1)
                return {TransportResultCode::Disconnected};
            connected_ = false;
            if (VirtualTransport* host = bus_->endpoint(0)) {
                host->connected_clients_[id_] = false;
                host->queue_.push_back({TransportEventType::Disconnected, id_});
            }
            queue_.push_back({TransportEventType::Disconnected, 1});
        }
        return {};
    }

    void stop() noexcept override
    {
        if (!running_)
            return;
        if (role_ == TransportRole::Client && connected_)
            (void)disconnect(1);
        if (role_ == TransportRole::Server) {
            for (PeerId peer = 1; peer < connected_clients_.size(); ++peer) {
                if (connected_clients_[peer])
                    (void)disconnect(peer);
            }
        }
        queue_.clear();
        running_ = false;
    }

    bool running() const noexcept override { return running_; }

private:
    VirtualBus* bus_ = nullptr;
    std::uint32_t id_ = 0;
    TransportRole role_ = TransportRole::Client;
    bool running_ = false;
    bool connected_ = false;
    std::array<bool, kMaxSessionPlayers> connected_clients_{};
    std::deque<TransportEvent> queue_;
};

SessionConfig session_config(SessionRole role, std::uint32_t max_peers = 64)
{
    SessionConfig config{};
    config.role = role;
    config.transport.max_peers = max_peers;
    config.protocol_version = "stress-protocol-1";
    config.kraken_version = "stress-kraken-1";
    config.game_version = "stress-game-1";
    config.mod_version = "stress-mod-1";
    config.resource_fingerprint = "sha256:stress-resource";
    return config;
}

void pump_all(Session& host,
              const std::vector<std::unique_ptr<Session>>& clients)
{
    assert(host.pump(256));
    for (const auto& client : clients)
        assert(client->pump(32));
}

bool all_connected(Session& host,
                   const std::vector<std::unique_ptr<Session>>& clients)
{
    std::array<SessionEvent, 128> events{};
    std::size_t host_connected = 0;
    std::vector<bool> client_connected(clients.size(), false);
    for (std::size_t iteration = 0; iteration < 256; ++iteration) {
        pump_all(host, clients);
        const std::size_t host_count = host.drain_events(events);
        for (std::size_t index = 0; index < host_count; ++index)
            host_connected += events[index].type == SessionEventType::PeerConnected;
        for (std::size_t client_index = 0; client_index < clients.size(); ++client_index) {
            const std::size_t count = clients[client_index]->drain_events(events);
            for (std::size_t index = 0; index < count; ++index)
                client_connected[client_index] = client_connected[client_index] ||
                    events[index].type == SessionEventType::PeerConnected;
        }
        bool clients_ready = true;
        for (const bool ready : client_connected)
            clients_ready = clients_ready && ready;
        if (host_connected == clients.size() && clients_ready)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    static_assert(kMaxSessionPlayers == 64);
    static_assert(kPlayerSlotCount == 64);

    VirtualBus bus;
    VirtualTransport host_transport(bus, 0);
    Session host(host_transport, session_config(SessionRole::Server));
    assert(host.start());

    // A mismatched participant exercises the real compatibility handshake and
    // is disconnected before the roster can be locked.
    {
        VirtualTransport mismatch_transport(bus, 1);
        SessionConfig mismatch_config = session_config(SessionRole::Client);
        mismatch_config.resource_fingerprint = "sha256:wrong-resource";
        Session mismatch(mismatch_transport, mismatch_config);
        assert(mismatch.start());
        assert(mismatch.connect({"virtual", 1}));
        bool rejected = false;
        std::array<SessionEvent, 8> events{};
        for (std::size_t iteration = 0; iteration < 16 && !rejected; ++iteration) {
            const TransportResult host_result = host.pump();
            assert(host_result || host_result.code == TransportResultCode::ProtocolError);
            const TransportResult client_result = mismatch.pump();
            assert(client_result || client_result.code == TransportResultCode::ProtocolError);
            const std::size_t count = host.drain_events(events);
            for (std::size_t index = 0; index < count; ++index)
                rejected = rejected || events[index].type == SessionEventType::ProtocolError;
        }
        assert(rejected);
        mismatch.stop();
    }

    std::vector<std::unique_ptr<VirtualTransport>> client_transports;
    std::vector<std::unique_ptr<Session>> clients;
    client_transports.reserve(63);
    clients.reserve(63);
    for (std::uint32_t id = 1; id <= 63; ++id) {
        client_transports.push_back(std::make_unique<VirtualTransport>(bus, id));
        clients.push_back(std::make_unique<Session>(
            *client_transports.back(), session_config(SessionRole::Client)));
        assert(clients.back()->start());
        assert(clients.back()->connect({"virtual", 1}));
    }
    assert(all_connected(host, clients));

    MatchCoordinator match;
    MatchConfig config{};
    config.required_players = 64;
    config.max_players = 64;
    config.join_policy = JoinPolicy::JoinInProgress;
    config.target_map = "stress-map";
    config.exit_map = "stress-exit";
    const auto now = MatchCoordinator::TimePoint{};
    assert(match.start(config, 1, 1, now));

    // Disconnect one valid participant before lock, remove it from the forming
    // roster, then reconnect it under the same deterministic player identity.
    for (MatchPlayerId player = 2; player <= 64; ++player)
        assert(match.add_player(player, player - 1, player));
    assert(client_transports.back()->disconnect(1));
    assert(match.remove_player(64));
    clients.back()->stop();
    clients.pop_back();
    client_transports.pop_back();
    client_transports.push_back(std::make_unique<VirtualTransport>(bus, 63));
    clients.push_back(std::make_unique<Session>(
        *client_transports.back(), session_config(SessionRole::Client)));
    assert(clients.back()->start());
    assert(clients.back()->connect({"virtual", 1}));
    // Pump the rejoin handshake without consuming the established clients.
    bool rejoined = false;
    std::array<SessionEvent, 128> events{};
    for (std::size_t iteration = 0; iteration < 32 && !rejoined; ++iteration) {
        assert(host.pump(256));
        assert(clients.back()->pump(32));
        const std::size_t count = clients.back()->drain_events(events);
        for (std::size_t index = 0; index < count; ++index)
            rejoined = rejoined || events[index].type == SessionEventType::PeerConnected;
        (void)host.drain_events(events);
    }
    assert(rejoined);
    assert(match.add_player(64, 63, 64));

    for (std::size_t index = 0; index < clients.size(); ++index) {
        const MatchPlayerId player = static_cast<MatchPlayerId>(index + 2);
        std::vector<Byte> ready_payload;
        assert(encode_match_ready({77, player, player, true}, ready_payload) ==
               MatchCodecError::None);
        assert(clients[index]->send(1, MessageType::MatchReady,
                                    Channel::Reliable, ready_payload));
    }
    assert(host.pump(256));
    std::size_t ready_receipts = 0;
    {
        const std::size_t count = host.drain_events(events);
        for (std::size_t index = 0; index < count; ++index) {
            if (events[index].type != SessionEventType::Message ||
                events[index].message_type != MessageType::MatchReady)
                continue;
            MatchReady ready{};
            assert(decode_match_ready(events[index].payload, ready) ==
                   MatchCodecError::None);
            assert(match.set_ready(ready.player_id, ready.ready));
            ++ready_receipts;
        }
    }
    assert(ready_receipts == 63);

    PlayerSlotAllocator slots;
    EntityRegistry entities(64);
    std::array<PlayerSlotLease, 64> leases{};
    for (MatchPlayerId player = 1; player <= 64; ++player) {
        assert(match.add_spawn({static_cast<float>(player), 0.0f,
                                static_cast<float>(player * 2), 0.0f,
                                static_cast<std::int32_t>(1000 + player)}));
        const PlayerSlotIndex slot = player_slot_for_entity(player);
        assert(slot != kInvalidPlayerSlot);
        const auto lease = slots.reserve(slot, player);
        assert(lease.has_value());
        assert(slots.bind(*lease, player));
        leases[slot] = *lease;
        assert(entities.bind(player, static_cast<ObjId>(10000 + player),
                             lease->generation) == EntityRegistryBindResult::Inserted);
    }
    assert(match.update(now) == MatchAction::BeginLoading);
    const MatchStatus locked = match.status(now);
    assert(locked.roster_locked && locked.connected_players == 64 &&
           locked.ready_players == 64);
    for (MatchPlayerId player = 1; player <= 64; ++player) {
        const auto spawn = match.spawn_for(player);
        assert(spawn && spawn->belong == static_cast<std::int32_t>(1000 + player));
    }

    MatchRosterLock roster{};
    roster.session_epoch = 77;
    roster.roster_revision = 9;
    roster.required_players = 64;
    roster.max_players = 64;
    roster.join_policy = JoinPolicy::JoinInProgress;
    for (const MatchPlayer& player : match.players()) {
        roster.players.push_back({player.id, player.peer, player.entity_id,
                                  player.join_order, player.host, player.ready});
        roster.spawns.push_back(*match.spawn_for(player.id));
    }
    std::vector<Byte> roster_payload;
    assert(encode_match_roster_lock(roster, roster_payload) == MatchCodecError::None);
    for (PeerId peer = 1; peer <= 63; ++peer)
        assert(host.send(peer, MessageType::MatchRosterLock, Channel::Reliable,
                         roster_payload));
    for (const auto& client : clients)
        assert(client->pump(32));
    std::size_t roster_receipts = 0;
    for (const auto& client : clients) {
        const std::size_t count = client->drain_events(events);
        for (std::size_t index = 0; index < count; ++index) {
            if (events[index].type != SessionEventType::Message ||
                events[index].message_type != MessageType::MatchRosterLock)
                continue;
            MatchRosterLock decoded{};
            assert(decode_match_roster_lock(events[index].payload, decoded) ==
                   MatchCodecError::None);
            assert(decoded.players.size() == 64 && decoded.spawns.size() == 64);
            ++roster_receipts;
        }
    }
    assert(roster_receipts == 63);

    assert(match.begin_synchronizing());
    for (MatchPlayerId player = 1; player <= 64; ++player)
        assert(match.set_synchronized(player));
    assert(match.begin_playing());
    assert(!match.can_join()); // JIP policy is open, but the 64-slot roster is full.

    // Leave/rejoin is legal under JIP after Playing, but synchronization is
    // still a separate barrier and does not silently inherit old readiness.
    assert(match.remove_player(64));
    assert(match.can_join());
    assert(match.add_player(64, 63, 64));
    assert(!match.set_synchronized(64));
    assert(match.set_ready(64));
    assert(match.set_synchronized(64));

    for (std::size_t index = 0; index < leases.size(); ++index)
        assert(slots.release(leases[index], static_cast<NetId>(index + 1)));
    entities.clear();
    for (auto& client : clients)
        client->stop();
    host.stop();
    clients.clear();
    client_transports.clear();
    assert(entities.empty());
    assert(!host_transport.running());
    assert(bus.attached() == 1); // the stopped stack-owned host transport remains
    return 0;
}
