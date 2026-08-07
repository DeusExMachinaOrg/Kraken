#ifndef KRAKEN_NET_TRANSPORT_HPP
#define KRAKEN_NET_TRANSPORT_HPP

#include "net/net_types.hpp"

#include <memory>

namespace kraken::net {

// Define to 1 only in a build that supplies ENet headers and libraries.
#ifndef KRAKEN_NET_ENABLE_ENET
#define KRAKEN_NET_ENABLE_ENET 0
#endif

inline constexpr bool kEnetBackendCompiled = KRAKEN_NET_ENABLE_ENET != 0;

class EnetTransport final : public ITransport {
public:
    EnetTransport();
    ~EnetTransport() override;

    EnetTransport(EnetTransport&&) = delete;
    EnetTransport& operator=(EnetTransport&&) = delete;
    EnetTransport(const EnetTransport&) = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;

    TransportResult start(const TransportConfig& config) override;
    TransportResult listen() override;
    TransportResult connect(const Endpoint& endpoint) override;
    TransportResult poll(std::span<TransportEvent> events,
                         std::size_t& event_count) override;
    TransportResult send(PeerId peer, Channel channel,
                         ByteView payload) override;
    TransportResult disconnect(PeerId peer) override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

static_assert(kEnetBackendCompiled == (KRAKEN_NET_ENABLE_ENET != 0));
static_assert(!std::is_copy_constructible_v<EnetTransport>);
static_assert(!std::is_move_constructible_v<EnetTransport>);

} // namespace kraken::net

#endif // KRAKEN_NET_TRANSPORT_HPP
