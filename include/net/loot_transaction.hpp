#ifndef KRAKEN_NET_LOOT_TRANSACTION_HPP
#define KRAKEN_NET_LOOT_TRANSACTION_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>

namespace kraken::net {

using LootId = std::uint32_t;
using LootTransactionId = std::uint32_t;

inline constexpr std::uint32_t kLootRequestWireMagic = 0x3151454Cu; // LEQ1
inline constexpr std::uint32_t kLootResultWireMagic = 0x3152454Cu;  // LER1
inline constexpr std::uint16_t kLootWireVersion = 1;
inline constexpr std::size_t kLootRequestWireSize = 24;
inline constexpr std::size_t kLootResultWireSize = 32;

struct LootRequest {
    std::uint32_t entity_id = 0;
    LootId loot_id = 0;
    LootTransactionId transaction_id = 0;
    std::uint32_t amount = 0;
};

enum class LootResultCode : std::uint8_t {
    Granted = 0,
    NotFound,
    NotOwner,
    TooFar,
    InventoryFull,
    Exhausted,
    InvalidRequest,
};

struct LootResult {
    LootTransactionId transaction_id = 0;
    LootId loot_id = 0;
    std::int32_t prototype_id = -1;
    std::uint32_t granted_amount = 0;
    std::uint32_t remaining_amount = 0;
    LootResultCode code = LootResultCode::InvalidRequest;
};

enum class LootCodecError : std::uint8_t {
    None, OutputTooSmall, InputSizeMismatch, BadMagic, BadVersion, BadFlags,
    InvalidEntity, InvalidLootId, InvalidTransactionId, InvalidAmount,
    InvalidResultCode,
};

[[nodiscard]] constexpr bool loot_codec_succeeded(LootCodecError error) noexcept
{ return error == LootCodecError::None; }

[[nodiscard]] LootCodecError encode_loot_request(const LootRequest&, MutableByteView) noexcept;
[[nodiscard]] LootCodecError decode_loot_request(ByteView, LootRequest&) noexcept;
[[nodiscard]] LootCodecError encode_loot_result(const LootResult&, MutableByteView) noexcept;
[[nodiscard]] LootCodecError decode_loot_result(ByteView, LootResult&) noexcept;

} // namespace kraken::net

#endif
