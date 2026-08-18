#ifndef KRAKEN_NET_WORLD_MUTATION_PROTOCOL_HPP
#define KRAKEN_NET_WORLD_MUTATION_PROTOCOL_HPP

#include "net/world_observer.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kraken::net {

inline constexpr std::uint32_t kWorldMutationWireMagic = 0x31444D57u; // WMD1
inline constexpr std::uint16_t kWorldMutationWireVersion = 1;
inline constexpr std::size_t kMaxWorldMutationPayload = 64u * 1024u;

enum class WorldMutationCodecError : std::uint8_t {
    None,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidKind,
    InvalidIdentity,
    InvalidPayload,
    PayloadTooLarge,
};

[[nodiscard]] constexpr bool world_mutation_codec_succeeded(
    const WorldMutationCodecError error) noexcept
{ return error == WorldMutationCodecError::None; }

[[nodiscard]] WorldMutationCodecError encode_world_mutation(
    const WorldMutationEvent&, std::vector<Byte>&);
[[nodiscard]] WorldMutationCodecError decode_world_mutation(
    ByteView, WorldMutationEvent&) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_WORLD_MUTATION_PROTOCOL_HPP
