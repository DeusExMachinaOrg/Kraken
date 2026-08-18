#ifndef KRAKEN_NET_SESSION_COMPATIBILITY_HPP
#define KRAKEN_NET_SESSION_COMPATIBILITY_HPP

#include "net/net_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace kraken::net {

// The compatibility payload is intentionally independent from the wire
// header.  This lets the framing protocol evolve without silently accepting
// a peer with a different game/mod/resource identity.
inline constexpr std::uint32_t kSessionCompatibilityMagic = 0x4B434D31u;
inline constexpr std::uint16_t kSessionCompatibilityVersion = 1;
inline constexpr std::uint16_t kSessionCompatibilityFieldCount = 5;
inline constexpr std::size_t kSessionCompatibilityHeaderSize = 12;
inline constexpr std::size_t kSessionCompatibilityMaxFieldSize = 128;
inline constexpr std::size_t kSessionCompatibilityFieldLengthSize = 2;
inline constexpr std::size_t kSessionCompatibilityMaxWireSize =
    kSessionCompatibilityHeaderSize +
    kSessionCompatibilityFieldCount *
        (kSessionCompatibilityFieldLengthSize +
         kSessionCompatibilityMaxFieldSize);

struct SessionIdentity {
    std::string protocol_version{};
    std::string kraken_version{};
    std::string game_version{};
    std::string mod_version{};
    std::string resource_fingerprint{};

    friend bool operator==(const SessionIdentity&, const SessionIdentity&) =
        default;
};

enum class SessionCompatibilityCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputTooSmall,
    BadMagic,
    BadVersion,
    BadFlags,
    BadFieldCount,
    BadReserved,
    FieldTooLarge,
    MalformedField,
    BadPayloadSize,
};

[[nodiscard]] inline constexpr bool session_compatibility_codec_succeeded(
    SessionCompatibilityCodecError error) noexcept
{
    return error == SessionCompatibilityCodecError::None;
}

namespace session_compatibility_detail {

inline void put_u16(Byte* destination, const std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>((value >> 0) & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

inline void put_u32(Byte* destination, const std::uint32_t value) noexcept
{
    destination[0] = static_cast<Byte>((value >> 0) & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
    destination[2] = static_cast<Byte>((value >> 16) & 0xffu);
    destination[3] = static_cast<Byte>((value >> 24) & 0xffu);
}

[[nodiscard]] inline std::uint16_t get_u16(const Byte* source) noexcept
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(source[0]) << 0) |
        (static_cast<std::uint16_t>(source[1]) << 8));
}

[[nodiscard]] inline std::uint32_t get_u32(const Byte* source) noexcept
{
    return (static_cast<std::uint32_t>(source[0]) << 0) |
           (static_cast<std::uint32_t>(source[1]) << 8) |
           (static_cast<std::uint32_t>(source[2]) << 16) |
           (static_cast<std::uint32_t>(source[3]) << 24);
}

[[nodiscard]] inline bool is_continuation(const std::uint8_t value) noexcept
{
    return value >= 0x80u && value <= 0xbfu;
}

[[nodiscard]] inline bool valid_field(std::string_view field) noexcept
{
    if (field.size() > kSessionCompatibilityMaxFieldSize)
        return false;

    // Identity values are UTF-8 strings.  Reject NULs and overlong,
    // surrogate, or out-of-range encodings so malformed values cannot compare
    // differently across implementations.
    for (std::size_t index = 0; index < field.size();) {
        const std::uint8_t first = static_cast<std::uint8_t>(field[index]);
        if (first == 0)
            return false;
        if (first <= 0x7fu) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xc2u && first <= 0xdfu)
            length = 2;
        else if (first >= 0xe0u && first <= 0xefu)
            length = 3;
        else if (first >= 0xf0u && first <= 0xf4u)
            length = 4;
        else
            return false;

        if (index + length > field.size())
            return false;
        const std::uint8_t second =
            static_cast<std::uint8_t>(field[index + 1]);
        if (!is_continuation(second))
            return false;
        if ((first == 0xe0u && second < 0xa0u) ||
            (first == 0xedu && second > 0x9fu) ||
            (first == 0xf0u && second < 0x90u) ||
            (first == 0xf4u && second > 0x8fu))
            return false;
        for (std::size_t offset = 2; offset < length; ++offset) {
            if (!is_continuation(
                    static_cast<std::uint8_t>(field[index + offset])))
                return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] inline std::array<std::string_view,
                                kSessionCompatibilityFieldCount>
fields(const SessionIdentity& identity) noexcept
{
    return {identity.protocol_version, identity.kraken_version,
            identity.game_version, identity.mod_version,
            identity.resource_fingerprint};
}

[[nodiscard]] inline std::size_t encoded_size(
    const SessionIdentity& identity) noexcept
{
    const auto values = fields(identity);
    std::size_t size = kSessionCompatibilityHeaderSize;
    for (const std::string_view value : values)
        size += kSessionCompatibilityFieldLengthSize + value.size();
    return size;
}

} // namespace session_compatibility_detail

[[nodiscard]] inline bool is_valid_session_identity(
    const SessionIdentity& identity) noexcept
{
    const auto values = session_compatibility_detail::fields(identity);
    for (const std::string_view value : values) {
        if (!session_compatibility_detail::valid_field(value))
            return false;
    }
    return true;
}

[[nodiscard]] inline std::size_t session_identity_wire_size(
    const SessionIdentity& identity) noexcept
{
    return session_compatibility_detail::encoded_size(identity);
}

[[nodiscard]] inline SessionCompatibilityCodecError encode_session_identity(
    const SessionIdentity& identity, MutableByteView output,
    std::size_t& bytes_written) noexcept
{
    bytes_written = 0;
    for (const std::string_view value :
         session_compatibility_detail::fields(identity)) {
        if (value.size() > kSessionCompatibilityMaxFieldSize)
            return SessionCompatibilityCodecError::FieldTooLarge;
        if (!session_compatibility_detail::valid_field(value))
            return SessionCompatibilityCodecError::MalformedField;
    }

    const std::size_t required = session_identity_wire_size(identity);
    if (output.size() < required)
        return SessionCompatibilityCodecError::OutputTooSmall;

    Byte* data = output.data();
    session_compatibility_detail::put_u32(data + 0,
                                           kSessionCompatibilityMagic);
    session_compatibility_detail::put_u16(
        data + 4, kSessionCompatibilityVersion);
    session_compatibility_detail::put_u16(data + 6, 0);
    session_compatibility_detail::put_u16(
        data + 8, kSessionCompatibilityFieldCount);
    session_compatibility_detail::put_u16(data + 10, 0);

    std::size_t offset = kSessionCompatibilityHeaderSize;
    for (const std::string_view value :
         session_compatibility_detail::fields(identity)) {
        session_compatibility_detail::put_u16(
            data + offset, static_cast<std::uint16_t>(value.size()));
        offset += kSessionCompatibilityFieldLengthSize;
        for (const char character : value)
            data[offset++] = static_cast<Byte>(character);
    }
    bytes_written = offset;
    return SessionCompatibilityCodecError::None;
}

[[nodiscard]] inline SessionCompatibilityCodecError decode_session_identity(
    ByteView input, SessionIdentity& identity) noexcept
{
    if (input.size() < kSessionCompatibilityHeaderSize)
        return SessionCompatibilityCodecError::InputTooSmall;

    const Byte* data = input.data();
    if (session_compatibility_detail::get_u32(data + 0) !=
        kSessionCompatibilityMagic)
        return SessionCompatibilityCodecError::BadMagic;
    if (session_compatibility_detail::get_u16(data + 4) !=
        kSessionCompatibilityVersion)
        return SessionCompatibilityCodecError::BadVersion;
    if (session_compatibility_detail::get_u16(data + 6) != 0)
        return SessionCompatibilityCodecError::BadFlags;
    if (session_compatibility_detail::get_u16(data + 8) !=
        kSessionCompatibilityFieldCount)
        return SessionCompatibilityCodecError::BadFieldCount;
    if (session_compatibility_detail::get_u16(data + 10) != 0)
        return SessionCompatibilityCodecError::BadReserved;

    std::array<std::string, kSessionCompatibilityFieldCount> values{};
    std::size_t offset = kSessionCompatibilityHeaderSize;
    for (std::string& value : values) {
        if (input.size() - offset < kSessionCompatibilityFieldLengthSize)
            return SessionCompatibilityCodecError::InputTooSmall;
        const std::size_t length =
            session_compatibility_detail::get_u16(data + offset);
        offset += kSessionCompatibilityFieldLengthSize;
        if (length > kSessionCompatibilityMaxFieldSize)
            return SessionCompatibilityCodecError::FieldTooLarge;
        if (input.size() - offset < length)
            return SessionCompatibilityCodecError::InputTooSmall;
        const auto field = input.subspan(offset, length);
        value.assign(reinterpret_cast<const char*>(field.data()), length);
        if (!session_compatibility_detail::valid_field(value))
            return SessionCompatibilityCodecError::MalformedField;
        offset += length;
    }
    if (offset != input.size())
        return SessionCompatibilityCodecError::BadPayloadSize;

    identity.protocol_version = std::move(values[0]);
    identity.kraken_version = std::move(values[1]);
    identity.game_version = std::move(values[2]);
    identity.mod_version = std::move(values[3]);
    identity.resource_fingerprint = std::move(values[4]);
    return SessionCompatibilityCodecError::None;
}

static_assert(kSessionCompatibilityMaxWireSize == 662);
static_assert(kSessionCompatibilityFieldCount == 5);

} // namespace kraken::net

#endif // KRAKEN_NET_SESSION_COMPATIBILITY_HPP
