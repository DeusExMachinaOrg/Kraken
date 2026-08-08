#ifndef KRAKEN_NET_INPUT_COMMAND_HPP
#define KRAKEN_NET_INPUT_COMMAND_HPP

#include "net/net_types.hpp"
#include <cstddef>
#include <cstdint>
namespace kraken::net {
inline constexpr std::uint32_t kInputCommandWireMagic=0x31504E49u;
inline constexpr std::uint16_t kInputCommandWireVersion=2;
inline constexpr std::size_t kInputCommandWireSize=36;
struct InputCommand { std::uint32_t entity_id=0,sequence=0,client_tick=0; float throttle=0,steer=0,brake=0; bool handbrake=false; bool request_unstuck=false; };
enum class InputCommandCodecError:std::uint8_t { None,OutputTooSmall,InputSizeMismatch,BadMagic,BadVersion,BadFlags,InvalidEntityId,InvalidHandbrake,InvalidUnstuckRequest,NonFiniteValue,ValueOutOfRange };
[[nodiscard]] constexpr bool input_command_codec_succeeded(InputCommandCodecError e) noexcept{return e==InputCommandCodecError::None;}
[[nodiscard]] InputCommandCodecError encode_input_command(const InputCommand&,MutableByteView) noexcept;
[[nodiscard]] InputCommandCodecError decode_input_command(ByteView,InputCommand&) noexcept;
}
#endif
