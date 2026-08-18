#ifndef KRAKEN_NET_WIRE_PROTOCOL_HPP
#define KRAKEN_NET_WIRE_PROTOCOL_HPP

#include "net/net_types.hpp"

#include <array>
#include <cstdint>

namespace kraken::net {

inline constexpr std::uint32_t kWireMagic = 0x4B524E31u; // "KRN1"
// The envelope version advances with the typed combat presentation schema.
// Peers that still emit the v1 envelope are intentionally incompatible.
inline constexpr std::uint8_t kWireVersion = 4;
inline constexpr std::size_t kWireHeaderSize = 16;
inline constexpr std::uint32_t kMaxWirePayload = 1024u * 1024u;

enum class MessageType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Input = 3,
    Snapshot = 4,
    Ping = 5,
    Pong = 6,
    Disconnect = 7,
    EntityAssign = 8,
    WeaponCommand = 9,
    LootRequest = 10,
    LootResult = 11,
    EntityDespawn = 12,
    EntitySpawn = 13,
    Loadout = 14,
    ImpactDamage = 15,
    WorldLootSpawn = 16,
    WorldLootBaseline = 17,
    WorldLootDelta = 18,
    WorldLootRemove = 19,
    WorldLootPickupRequest = 20,
    WorldLootPickupResult = 21,
    MatchReadyRequest = 22,
    MatchReady = 23,
    MatchReject = 24,
    MatchRosterLock = 25,
    MatchLoad = 26,
    MatchSync = 27,
    MatchPlay = 28,
    MatchLeave = 29,
    MatchWorldSnapshot = 30,
    MatchWorldDelta = 31,
    MatchVehicleDescriptor = 32,
    MatchWorldReady = 33,
    MatchMapReady = 34,
    CombatWeaponTriggerState = 35,
    CombatShotConfirmed = 36,
    CombatImpactPresentation = 37,
    CombatDamageResult = 38,
    CombatDeathWreckPresentation = 39,
    CombatHornState = 40,
    CombatPresentationJipState = 41,
    CombatWeaponAimState = 42,
    CombatWreckArchiveChunk = 43,
    MatchQuestSnapshot = 44,
    MatchQuestDelta = 45,
};

[[nodiscard]] constexpr bool is_valid_message_type(MessageType type) noexcept
{
    switch (type) {
    case MessageType::Hello:
    case MessageType::Welcome:
    case MessageType::Input:
    case MessageType::Snapshot:
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::Disconnect:
    case MessageType::EntityAssign:
    case MessageType::WeaponCommand:
    case MessageType::LootRequest:
    case MessageType::LootResult:
    case MessageType::EntityDespawn:
    case MessageType::EntitySpawn:
    case MessageType::Loadout:
    case MessageType::ImpactDamage:
    case MessageType::WorldLootSpawn:
    case MessageType::WorldLootBaseline:
    case MessageType::WorldLootDelta:
    case MessageType::WorldLootRemove:
    case MessageType::WorldLootPickupRequest:
    case MessageType::WorldLootPickupResult:
    case MessageType::MatchReadyRequest:
    case MessageType::MatchReady:
    case MessageType::MatchReject:
    case MessageType::MatchRosterLock:
    case MessageType::MatchLoad:
    case MessageType::MatchSync:
    case MessageType::MatchPlay:
    case MessageType::MatchLeave:
    case MessageType::MatchWorldSnapshot:
    case MessageType::MatchWorldDelta:
    case MessageType::MatchVehicleDescriptor:
    case MessageType::MatchWorldReady:
    case MessageType::MatchMapReady:
    case MessageType::CombatWeaponTriggerState:
    case MessageType::CombatShotConfirmed:
    case MessageType::CombatImpactPresentation:
    case MessageType::CombatDamageResult:
    case MessageType::CombatDeathWreckPresentation:
    case MessageType::CombatHornState:
    case MessageType::CombatPresentationJipState:
    case MessageType::CombatWeaponAimState:
    case MessageType::CombatWreckArchiveChunk:
    case MessageType::MatchQuestSnapshot:
    case MessageType::MatchQuestDelta:
        return true;
    }
    return false;
}

// ENet packets without RELIABLE or UNSEQUENCED flags are unreliable and
// sequenced within their channel.  Aim updates deliberately use that policy so
// stale high-rate samples do not delay newer presentation state.
[[nodiscard]] constexpr bool uses_unreliable_sequenced_channel(
    const MessageType type) noexcept
{
    return type == MessageType::CombatWeaponAimState;
}

[[nodiscard]] constexpr bool requires_reliable_channel(
    MessageType type) noexcept
{
    switch (type) {
    case MessageType::WorldLootSpawn:
    case MessageType::WorldLootBaseline:
    case MessageType::WorldLootDelta:
    case MessageType::WorldLootRemove:
    case MessageType::WorldLootPickupRequest:
    case MessageType::WorldLootPickupResult:
    case MessageType::MatchReadyRequest:
    case MessageType::MatchReady:
    case MessageType::MatchReject:
    case MessageType::MatchRosterLock:
    case MessageType::MatchLoad:
    case MessageType::MatchSync:
    case MessageType::MatchPlay:
    case MessageType::MatchLeave:
    case MessageType::MatchWorldSnapshot:
    case MessageType::MatchWorldDelta:
    case MessageType::MatchVehicleDescriptor:
    case MessageType::MatchWorldReady:
    case MessageType::MatchMapReady:
    case MessageType::CombatWeaponTriggerState:
    case MessageType::CombatShotConfirmed:
    case MessageType::CombatImpactPresentation:
    case MessageType::CombatDamageResult:
    case MessageType::CombatDeathWreckPresentation:
    case MessageType::CombatHornState:
    case MessageType::CombatPresentationJipState:
    case MessageType::CombatWreckArchiveChunk:
    case MessageType::MatchQuestSnapshot:
    case MessageType::MatchQuestDelta:
        return true;
    default:
        return false;
    }
}

static_assert(uses_unreliable_sequenced_channel(
    MessageType::CombatWeaponAimState));
static_assert(!requires_reliable_channel(MessageType::CombatWeaponAimState));
static_assert(requires_reliable_channel(
    MessageType::CombatWeaponTriggerState));
static_assert(requires_reliable_channel(MessageType::CombatShotConfirmed));

struct WireHeader {
    std::uint32_t magic = kWireMagic;
    std::uint8_t version = kWireVersion;
    MessageType message_type = MessageType::Input;
    Channel channel = Channel::Reliable;
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t payload_size = 0;
};

enum class WireDecodeError : std::uint8_t {
    None,
    BufferTooSmall,
    BadMagic,
    BadVersion,
    BadMessageType,
    BadChannel,
    BadFlags,
    BadPayloadSize,
    PayloadTooLarge,
};

namespace detail {

constexpr void put_u32(Byte* dst, std::uint32_t value) noexcept
{
    dst[0] = static_cast<Byte>((value >> 0) & 0xffu);
    dst[1] = static_cast<Byte>((value >> 8) & 0xffu);
    dst[2] = static_cast<Byte>((value >> 16) & 0xffu);
    dst[3] = static_cast<Byte>((value >> 24) & 0xffu);
}

[[nodiscard]] constexpr std::uint32_t get_u32(const Byte* src) noexcept
{
    return (static_cast<std::uint32_t>(src[0]) << 0) |
           (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}

} // namespace detail

[[nodiscard]] constexpr WireDecodeError decode_header(
    ByteView bytes, WireHeader& header) noexcept
{
    if (bytes.size() < kWireHeaderSize)
        return WireDecodeError::BufferTooSmall;

    const Byte* data = bytes.data();
    header.magic = detail::get_u32(data + 0);
    header.version = static_cast<std::uint8_t>(data[4]);
    header.message_type = static_cast<MessageType>(data[5]);
    header.channel = static_cast<Channel>(data[6]);
    header.flags = static_cast<std::uint8_t>(data[7]);
    header.sequence = detail::get_u32(data + 8);
    header.payload_size = detail::get_u32(data + 12);

    if (header.magic != kWireMagic)
        return WireDecodeError::BadMagic;
    if (header.version != kWireVersion)
        return WireDecodeError::BadVersion;
    if (!is_valid_message_type(header.message_type))
        return WireDecodeError::BadMessageType;
    if (!is_valid_channel(header.channel))
        return WireDecodeError::BadChannel;
    if (header.flags != 0)
        return WireDecodeError::BadFlags;
    if (header.payload_size > kMaxWirePayload)
        return WireDecodeError::PayloadTooLarge;
    return WireDecodeError::None;
}

constexpr void encode_header(const WireHeader& header,
                             MutableByteView bytes) noexcept
{
    if (bytes.size() < kWireHeaderSize)
        return;

    Byte* data = bytes.data();
    detail::put_u32(data + 0, header.magic);
    data[4] = static_cast<Byte>(header.version);
    data[5] = static_cast<Byte>(header.message_type);
    data[6] = static_cast<Byte>(header.channel);
    data[7] = static_cast<Byte>(header.flags);
    detail::put_u32(data + 8, header.sequence);
    detail::put_u32(data + 12, header.payload_size);
}

[[nodiscard]] constexpr WireDecodeError decode_frame(
    ByteView frame, WireHeader& header, ByteView& payload) noexcept
{
    const WireDecodeError error = decode_header(frame, header);
    if (error != WireDecodeError::None)
        return error;
    const std::size_t expected_size = kWireHeaderSize + header.payload_size;
    if (frame.size() < expected_size)
        return WireDecodeError::BufferTooSmall;
    if (frame.size() != expected_size)
        return WireDecodeError::BadPayloadSize;

    payload = frame.subspan(kWireHeaderSize, header.payload_size);
    return WireDecodeError::None;
}

consteval bool wire_header_self_test()
{
    std::array<Byte, kWireHeaderSize> bytes{};
    constexpr WireHeader expected{
        kWireMagic, kWireVersion, MessageType::Snapshot, Channel::Unreliable,
        0, 42, 3};
    encode_header(expected, MutableByteView{bytes});

    WireHeader actual{};
    return decode_header(ByteView{bytes}, actual) == WireDecodeError::None &&
           actual.magic == expected.magic &&
           actual.message_type == expected.message_type &&
           actual.channel == expected.channel &&
           actual.sequence == expected.sequence &&
           actual.payload_size == expected.payload_size;
}

static_assert(kWireHeaderSize == 16);
static_assert(wire_header_self_test());
static_assert(sizeof(MessageType) == sizeof(std::uint8_t));

} // namespace kraken::net

#endif // KRAKEN_NET_WIRE_PROTOCOL_HPP
