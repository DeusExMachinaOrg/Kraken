#include "net/loadout_protocol.hpp"

#include <algorithm>

namespace kraken::net { namespace {
void put16(std::vector<Byte>& out, std::uint16_t value) { out.push_back(Byte(value)); out.push_back(Byte(value >> 8)); }
void put32(std::vector<Byte>& out, std::uint32_t value) { for (int i=0;i<4;++i) out.push_back(Byte(value >> (i*8))); }
std::uint16_t get16(const Byte* data) { return std::uint16_t(std::uint8_t(data[0])) | std::uint16_t(std::uint8_t(data[1])) << 8; }
std::uint32_t get32(const Byte* data) { std::uint32_t v=0; for(int i=0;i<4;++i) v|=std::uint32_t(std::uint8_t(data[i])) << (i*8); return v; }
LoadoutCodecError validate(const LoadoutProfile& p) {
    if (!p.entity_id) return LoadoutCodecError::InvalidEntity;
    if (!p.revision) return LoadoutCodecError::InvalidRevision;
    if (p.generation == kInvalidEntityGeneration && p.entity_id >= 1000)
        return LoadoutCodecError::InvalidGeneration;
    if (p.base_prototype_id < -1)
        return LoadoutCodecError::InvalidPrototype;
    if (p.base_prototype_id >= 0 &&
        p.generation == kInvalidEntityGeneration)
        return LoadoutCodecError::InvalidGeneration;
    if (p.parts.size()>kMaxLoadoutParts) return LoadoutCodecError::TooManyParts;
    for (std::size_t i=0;i<p.parts.size();++i) {
        const auto& x=p.parts[i];
        if (x.slot.empty() || x.prototype.empty() || x.slot.size()>kMaxLoadoutNameLength || x.prototype.size()>kMaxLoadoutNameLength) return LoadoutCodecError::InvalidName;
        for (std::size_t j=0;j<i;++j) if (p.parts[j].slot==x.slot) return LoadoutCodecError::DuplicateSlot;
    }
    return LoadoutCodecError::None;
}
}
LoadoutCodecError encode_loadout(const LoadoutProfile& p, std::vector<Byte>& out) noexcept {
    const auto error=validate(p); if(!loadout_codec_succeeded(error)) return error;
    const bool has_generation = p.generation != kInvalidEntityGeneration;
    const bool has_prototype = p.base_prototype_id >= 0;
    out.clear(); out.reserve((has_prototype ? 26u : has_generation ? 22u : 20u)+p.parts.size()*16);
    put32(out,kLoadoutWireMagic);
    put16(out,has_prototype ? kLoadoutWireVersionWithPrototype
                             : has_generation ? kLoadoutWireVersionWithGeneration
                             : kLoadoutWireVersion);
    put16(out,0);
    put32(out,p.entity_id);
    put32(out,p.revision);
    if (has_prototype) {
        put32(out, static_cast<std::uint32_t>(p.base_prototype_id));
        put16(out, p.generation);
        out.push_back(Byte(p.parts.size()));
        out.push_back(Byte{});
        out.push_back(Byte{});
        out.push_back(Byte{});
    }
    else if (has_generation) {
        put16(out, p.generation);
        out.push_back(Byte(p.parts.size()));
        out.push_back(Byte{});
        out.push_back(Byte{});
        out.push_back(Byte{});
    }
    else {
        out.push_back(Byte(p.parts.size()));
        out.push_back(Byte{});
        out.push_back(Byte{});
        out.push_back(Byte{});
    }
    for(const auto& x:p.parts) {
        out.push_back(Byte(x.slot.size()));
        out.insert(out.end(),reinterpret_cast<const Byte*>(x.slot.data()),reinterpret_cast<const Byte*>(x.slot.data()+x.slot.size()));
        out.push_back(Byte(x.prototype.size()));
        out.insert(out.end(),reinterpret_cast<const Byte*>(x.prototype.data()),reinterpret_cast<const Byte*>(x.prototype.data()+x.prototype.size()));
    }
    return LoadoutCodecError::None;
}
LoadoutCodecError decode_loadout(ByteView in, LoadoutProfile& out) noexcept {
    if(in.size()<20) return LoadoutCodecError::InputSizeMismatch;
    const Byte* d=in.data();
    if(get32(d)!=kLoadoutWireMagic)return LoadoutCodecError::BadMagic;
    const std::uint16_t version = get16(d+4);
    if(version != kLoadoutWireVersion &&
       version != kLoadoutWireVersionWithGeneration &&
       version != kLoadoutWireVersionWithPrototype)
        return LoadoutCodecError::BadVersion;
    if(get16(d+6)!=0)return LoadoutCodecError::BadFlags;
    LoadoutProfile p{};
    p.entity_id = get32(d+8);
    p.revision = get32(d+12);
    std::size_t pos = 20;
    std::size_t count = 0;
    if (version == kLoadoutWireVersionWithPrototype) {
        if (in.size() < 26u)return LoadoutCodecError::InputSizeMismatch;
        p.base_prototype_id = static_cast<std::int32_t>(get32(d+16));
        if (p.base_prototype_id < 0)return LoadoutCodecError::InvalidPrototype;
        p.generation = get16(d+20);
        if (p.generation == kInvalidEntityGeneration)return LoadoutCodecError::InvalidGeneration;
        count=std::uint8_t(d[22]);
        if(std::uint8_t(d[23])||std::uint8_t(d[24])||std::uint8_t(d[25]))return LoadoutCodecError::BadFlags;
        pos = 26;
    }
    else if (version == kLoadoutWireVersionWithGeneration) {
        if (in.size() < 22u)return LoadoutCodecError::InputSizeMismatch;
        p.generation = get16(d+16);
        if (p.generation == kInvalidEntityGeneration)return LoadoutCodecError::InvalidGeneration;
        count=std::uint8_t(d[18]);
        if(std::uint8_t(d[19])||std::uint8_t(d[20])||std::uint8_t(d[21]))return LoadoutCodecError::BadFlags;
        pos = 22;
    }
    else {
        count=std::uint8_t(d[16]);
        if(std::uint8_t(d[17])||std::uint8_t(d[18])||std::uint8_t(d[19]))return LoadoutCodecError::BadFlags;
    }
    if(count>kMaxLoadoutParts)return LoadoutCodecError::TooManyParts;
    for(std::size_t i=0;i<count;++i){if(pos>=in.size())return LoadoutCodecError::InputSizeMismatch;const auto a=std::uint8_t(d[pos++]);if(!a||a>kMaxLoadoutNameLength||pos+a>=in.size())return LoadoutCodecError::InvalidName;std::string slot(reinterpret_cast<const char*>(d+pos),a);pos+=a;const auto b=std::uint8_t(d[pos++]);if(!b||b>kMaxLoadoutNameLength||pos+b>in.size())return LoadoutCodecError::InvalidName;std::string prototype(reinterpret_cast<const char*>(d+pos),b);pos+=b;p.parts.push_back({std::move(slot),std::move(prototype)});}
    if(pos!=in.size())return LoadoutCodecError::InputSizeMismatch;const auto e=validate(p);if(loadout_codec_succeeded(e))out=std::move(p);return e;
}
}
