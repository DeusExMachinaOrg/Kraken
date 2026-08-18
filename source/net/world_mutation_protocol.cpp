#include "net/world_mutation_protocol.hpp"

#include <cstring>
#include <utility>
#include <type_traits>

namespace kraken::net {
namespace {

constexpr std::size_t kHeaderSize = 8;

void put_u16(Byte* const dst, const std::uint16_t value) noexcept
{ dst[0] = Byte(value & 0xffu); dst[1] = Byte((value >> 8) & 0xffu); }
void put_u32(Byte* const dst, const std::uint32_t value) noexcept
{ for (unsigned i = 0; i < 4; ++i) dst[i] = Byte((value >> (i * 8)) & 0xffu); }
void put_u64(Byte* const dst, const std::uint64_t value) noexcept
{ for (unsigned i = 0; i < 8; ++i) dst[i] = Byte((value >> (i * 8)) & 0xffu); }
void put_f32(Byte* const dst, const float value) noexcept
{ std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); put_u32(dst, bits); }

[[nodiscard]] std::uint16_t get_u16(const Byte* const src) noexcept
{ return std::uint16_t(std::uint8_t(src[0])) | std::uint16_t(std::uint8_t(src[1])) << 8; }
[[nodiscard]] std::uint32_t get_u32(const Byte* const src) noexcept
{ std::uint32_t r=0; for(unsigned i=0;i<4;++i) r|=std::uint32_t(std::uint8_t(src[i]))<<(i*8); return r; }
[[nodiscard]] std::uint64_t get_u64(const Byte* const src) noexcept
{ std::uint64_t r=0; for(unsigned i=0;i<8;++i) r|=std::uint64_t(std::uint8_t(src[i]))<<(i*8); return r; }
[[nodiscard]] float get_f32(const Byte* const src) noexcept
{ const std::uint32_t bits=get_u32(src); float value=0.0f; std::memcpy(&value,&bits,sizeof(value)); return value; }

enum class WireKind : std::uint8_t {
    Created, Despawned, ParentAdded, ParentRemoved, Runtime, Property,
    Damage, Destroyed, Fx,
};

void init(std::vector<Byte>& out, const WireKind kind, const std::size_t body)
{
    out.assign(kHeaderSize + body, Byte{});
    put_u32(out.data(), kWorldMutationWireMagic);
    put_u16(out.data() + 4, kWorldMutationWireVersion);
    out[6] = Byte(kind);
    out[7] = Byte{};
}

[[nodiscard]] WorldMutationCodecError header(ByteView input, WireKind& kind) noexcept
{
    if (input.size() < kHeaderSize) return WorldMutationCodecError::InputSizeMismatch;
    if (get_u32(input.data()) != kWorldMutationWireMagic) return WorldMutationCodecError::BadMagic;
    if (get_u16(input.data()+4) != kWorldMutationWireVersion) return WorldMutationCodecError::BadVersion;
    if (std::uint8_t(input[7]) != 0) return WorldMutationCodecError::BadFlags;
    if (std::uint8_t(input[6]) > static_cast<std::uint8_t>(WireKind::Fx))
        return WorldMutationCodecError::InvalidKind;
    kind = static_cast<WireKind>(std::uint8_t(input[6]));
    return WorldMutationCodecError::None;
}

[[nodiscard]] bool valid_id(const HostObjectId id) noexcept { return id != kInvalidHostObjectId; }
[[nodiscard]] bool valid_size(const std::size_t size) noexcept { return size <= kMaxWorldMutationPayload; }

} // namespace

WorldMutationCodecError encode_world_mutation(const WorldMutationEvent& event,
                                              std::vector<Byte>& output)
{
    return std::visit([&output](const auto& value) -> WorldMutationCodecError {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ObjectCreatedEvent>) {
            if (!valid_id(value.object_id)) return WorldMutationCodecError::InvalidIdentity;
            init(output, WireKind::Created, 12); put_u64(output.data()+8,value.object_id); put_u32(output.data()+16,value.type_id); return WorldMutationCodecError::None;
        } else if constexpr (std::is_same_v<T, ObjectDespawnedEvent>) {
            if (!valid_id(value.object_id)) return WorldMutationCodecError::InvalidIdentity;
            init(output, WireKind::Despawned, 8); put_u64(output.data()+8,value.object_id); return WorldMutationCodecError::None;
        } else if constexpr (std::is_same_v<T, ParentChildAddedEvent> || std::is_same_v<T, ParentChildRemovedEvent>) {
            if (!valid_id(value.parent_id) || !valid_id(value.child_id)) return WorldMutationCodecError::InvalidIdentity;
            init(output, std::is_same_v<T, ParentChildAddedEvent> ? WireKind::ParentAdded : WireKind::ParentRemoved, 16); put_u64(output.data()+8,value.parent_id); put_u64(output.data()+16,value.child_id); return WorldMutationCodecError::None;
        } else if constexpr (std::is_same_v<T, RuntimeChangedEvent>) {
            if (!valid_id(value.object_id)) return WorldMutationCodecError::InvalidIdentity;
            if (!valid_size(value.value.size())) return WorldMutationCodecError::PayloadTooLarge;
            init(output, WireKind::Runtime, 12 + value.value.size()); put_u64(output.data()+8,value.object_id); put_u32(output.data()+16,static_cast<std::uint32_t>(value.value.size())); if(!value.value.empty()) std::memcpy(output.data()+20,value.value.data(),value.value.size()); return WorldMutationCodecError::None;
        } else if constexpr (std::is_same_v<T, PropertyChangedEvent>) {
            if (!valid_id(value.object_id)) return WorldMutationCodecError::InvalidIdentity;
            if (!valid_size(value.value.size())) return WorldMutationCodecError::PayloadTooLarge;
            init(output, WireKind::Property, 20 + value.value.size()); put_u64(output.data()+8,value.object_id); put_u32(output.data()+16,value.property_id); output[20]=Byte(value.removed ? 1 : 0); put_u32(output.data()+24,static_cast<std::uint32_t>(value.value.size())); if(!value.value.empty()) std::memcpy(output.data()+28,value.value.data(),value.value.size()); return WorldMutationCodecError::None;
        } else if constexpr (std::is_same_v<T, DamageEvent>) {
            if (!valid_id(value.target_id) || !valid_id(value.source_id)) return WorldMutationCodecError::InvalidIdentity;
            init(output, WireKind::Damage, 24); put_u64(output.data()+8,value.target_id); put_u64(output.data()+16,value.source_id); put_u32(output.data()+24,value.damage_type); put_f32(output.data()+28,value.amount); return WorldMutationCodecError::None;
        } else if constexpr (std::is_same_v<T, DestroyedEvent>) {
            if (!valid_id(value.object_id)) return WorldMutationCodecError::InvalidIdentity;
            init(output, WireKind::Destroyed, 12); put_u64(output.data()+8,value.object_id); put_u32(output.data()+16,value.reason); return WorldMutationCodecError::None;
        } else {
            if (!valid_id(value.object_id)) return WorldMutationCodecError::InvalidIdentity;
            if (!valid_size(value.payload.size())) return WorldMutationCodecError::PayloadTooLarge;
            init(output, WireKind::Fx, 16 + value.payload.size()); put_u64(output.data()+8,value.object_id); put_u32(output.data()+16,value.effect_id); put_u32(output.data()+20,static_cast<std::uint32_t>(value.payload.size())); if(!value.payload.empty()) std::memcpy(output.data()+24,value.payload.data(),value.payload.size()); return WorldMutationCodecError::None;
        }
    }, event);
}

WorldMutationCodecError decode_world_mutation(ByteView input,
                                              WorldMutationEvent& output) noexcept
{
    WireKind kind{};
    if (const auto error=header(input,kind); !world_mutation_codec_succeeded(error)) return error;
    const Byte* data=input.data()+8;
    switch(kind) {
    case WireKind::Created:
        if(input.size()!=20 || !valid_id(get_u64(data))) return WorldMutationCodecError::InvalidIdentity;
        output=ObjectCreatedEvent{get_u64(data),get_u32(data+8)}; return WorldMutationCodecError::None;
    case WireKind::Despawned:
        if(input.size()!=16 || !valid_id(get_u64(data))) return WorldMutationCodecError::InvalidIdentity;
        output=ObjectDespawnedEvent{get_u64(data)}; return WorldMutationCodecError::None;
    case WireKind::ParentAdded:
    case WireKind::ParentRemoved:
        if(input.size()!=24 || !valid_id(get_u64(data)) || !valid_id(get_u64(data+8))) return WorldMutationCodecError::InvalidIdentity;
        if(kind==WireKind::ParentAdded) output=ParentChildAddedEvent{get_u64(data),get_u64(data+8)}; else output=ParentChildRemovedEvent{get_u64(data),get_u64(data+8)}; return WorldMutationCodecError::None;
    case WireKind::Runtime: {
        if(input.size()<20 || !valid_id(get_u64(data))) return WorldMutationCodecError::InvalidIdentity; const auto size=get_u32(data+8); if(!valid_size(size)||input.size()!=20+size) return size>kMaxWorldMutationPayload?WorldMutationCodecError::PayloadTooLarge:WorldMutationCodecError::InputSizeMismatch; output=RuntimeChangedEvent{get_u64(data),std::vector<Byte>(input.begin()+20,input.end())}; return WorldMutationCodecError::None; }
    case WireKind::Property: {
        if(input.size()<28 || !valid_id(get_u64(data)) || std::uint8_t(data[12])>1) return WorldMutationCodecError::InvalidPayload; const auto size=get_u32(data+16); if(!valid_size(size)||input.size()!=28+size) return size>kMaxWorldMutationPayload?WorldMutationCodecError::PayloadTooLarge:WorldMutationCodecError::InputSizeMismatch; output=PropertyChangedEvent{get_u64(data),get_u32(data+8),std::vector<Byte>(input.begin()+28,input.end()),std::uint8_t(data[12])!=0}; return WorldMutationCodecError::None; }
    case WireKind::Damage:
        if(input.size()!=32 || !valid_id(get_u64(data)) || !valid_id(get_u64(data+8))) return WorldMutationCodecError::InvalidIdentity; output=DamageEvent{get_u64(data),get_u64(data+8),get_u32(data+16),get_f32(data+20)}; return WorldMutationCodecError::None;
    case WireKind::Destroyed:
        if(input.size()!=20 || !valid_id(get_u64(data))) return WorldMutationCodecError::InvalidIdentity; output=DestroyedEvent{get_u64(data),get_u32(data+8)}; return WorldMutationCodecError::None;
    case WireKind::Fx: {
        if(input.size()<24 || !valid_id(get_u64(data))) return WorldMutationCodecError::InvalidIdentity; const auto size=get_u32(data+12); if(!valid_size(size)||input.size()!=24+size) return size>kMaxWorldMutationPayload?WorldMutationCodecError::PayloadTooLarge:WorldMutationCodecError::InputSizeMismatch; output=FxEvent{get_u64(data),get_u32(data+8),std::vector<Byte>(input.begin()+24,input.end())}; return WorldMutationCodecError::None; }
    }
    return WorldMutationCodecError::InvalidKind;
}

} // namespace kraken::net
