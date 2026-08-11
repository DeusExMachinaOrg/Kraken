#include "net/loadout_protocol.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

using namespace kraken::net;

namespace {

LoadoutProfile make_npc_loadout()
{
    return LoadoutProfile{
        1007,
        31,
        {{"CABIN_SMALL_GUN", "hornet01"},
         {"BASKET_BIG_GUN_0", "rapier01"}},
        9};
}

void assert_same_parts(const LoadoutProfile& expected,
                       const LoadoutProfile& actual)
{
    assert(actual.parts.size() == expected.parts.size());
    for (std::size_t index = 0; index < expected.parts.size(); ++index) {
        assert(actual.parts[index].slot == expected.parts[index].slot);
        assert(actual.parts[index].prototype == expected.parts[index].prototype);
    }
}

void test_generation_aware_round_trip_and_legacy_players()
{
    const LoadoutProfile expected = make_npc_loadout();
    std::vector<Byte> bytes;
    assert(encode_loadout(expected, bytes) == LoadoutCodecError::None);
    assert(bytes.size() >= 22);
    assert(static_cast<std::uint8_t>(bytes[4]) ==
           static_cast<std::uint8_t>(kLoadoutWireVersionWithGeneration));

    LoadoutProfile actual{};
    assert(decode_loadout(bytes, actual) == LoadoutCodecError::None);
    assert(actual.entity_id == expected.entity_id);
    assert(actual.revision == expected.revision);
    assert(actual.generation == expected.generation);
    assert_same_parts(expected, actual);

    const LoadoutProfile legacy{
        7, 3, {{"CABIN_SMALL_GUN", "hornet01"}}, kInvalidEntityGeneration};
    assert(encode_loadout(legacy, bytes) == LoadoutCodecError::None);
    assert(static_cast<std::uint8_t>(bytes[4]) ==
           static_cast<std::uint8_t>(kLoadoutWireVersion));
    assert(decode_loadout(bytes, actual) == LoadoutCodecError::None);
    assert(actual.generation == kInvalidEntityGeneration);
    assert_same_parts(legacy, actual);
}

void test_client_base_prototype_round_trip()
{
    LoadoutProfile expected{
        7, 41, {{"CABIN_SMALL_GUN", "hornet01"}}, 3, 1133};
    std::vector<Byte> bytes;
    assert(encode_loadout(expected, bytes) == LoadoutCodecError::None);
    assert(static_cast<std::uint8_t>(bytes[4]) ==
           static_cast<std::uint8_t>(kLoadoutWireVersionWithPrototype));

    LoadoutProfile actual{};
    assert(decode_loadout(bytes, actual) == LoadoutCodecError::None);
    assert(actual.entity_id == expected.entity_id);
    assert(actual.generation == expected.generation);
    assert(actual.base_prototype_id == expected.base_prototype_id);
    assert_same_parts(expected, actual);

    expected.base_prototype_id = -2;
    assert(encode_loadout(expected, bytes) == LoadoutCodecError::InvalidPrototype);
}

void test_revision_entity_generation_and_name_bounds()
{
    LoadoutProfile profile = make_npc_loadout();
    std::vector<Byte> bytes;

    profile.entity_id = 0;
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::InvalidEntity);
    profile = make_npc_loadout();
    profile.revision = 0;
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::InvalidRevision);
    profile = make_npc_loadout();
    profile.generation = kInvalidEntityGeneration;
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::InvalidGeneration);

    profile = make_npc_loadout();
    profile.parts = {{std::string(kMaxLoadoutNameLength, 's'),
                      std::string(kMaxLoadoutNameLength, 'p')}};
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::None);
    profile.parts[0].slot.push_back('x');
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::InvalidName);
    profile = make_npc_loadout();
    profile.parts[0].prototype.push_back('x');
    profile.parts[0].prototype.append(kMaxLoadoutNameLength, 'p');
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::InvalidName);

    profile = make_npc_loadout();
    profile.parts.clear();
    for (std::size_t index = 0; index < kMaxLoadoutParts; ++index)
        profile.parts.push_back({"slot" + std::to_string(index), "proto"});
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::None);
    profile.parts.push_back({"slot-overflow", "proto"});
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::TooManyParts);
}

void test_duplicate_and_malformed_wire_inputs_are_rejected()
{
    LoadoutProfile profile = make_npc_loadout();
    std::vector<Byte> bytes;
    LoadoutProfile output{};
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::None);

    std::vector<Byte> malformed = bytes;
    malformed[0] = Byte{0};
    assert(decode_loadout(malformed, output) == LoadoutCodecError::BadMagic);
    malformed = bytes;
    malformed[4] = Byte{4};
    assert(decode_loadout(malformed, output) == LoadoutCodecError::BadVersion);
    malformed = bytes;
    malformed[6] = Byte{1};
    assert(decode_loadout(malformed, output) == LoadoutCodecError::BadFlags);
    malformed = bytes;
    malformed[16] = Byte{};
    malformed[17] = Byte{};
    assert(decode_loadout(malformed, output) == LoadoutCodecError::InvalidGeneration);
    malformed = bytes;
    malformed.pop_back();
    assert(decode_loadout(malformed, output) == LoadoutCodecError::InvalidName);
    malformed = bytes;
    malformed.push_back(Byte{});
    assert(decode_loadout(malformed, output) == LoadoutCodecError::InputSizeMismatch);
    malformed = bytes;
    malformed[22] = Byte{};
    assert(decode_loadout(malformed, output) == LoadoutCodecError::InvalidName);

    profile.parts.push_back({profile.parts[0].slot, "other-prototype"});
    assert(encode_loadout(profile, bytes) == LoadoutCodecError::DuplicateSlot);
    assert(decode_loadout(ByteView{bytes}.first(21), output) ==
           LoadoutCodecError::InputSizeMismatch);
}

} // namespace

int main()
{
    test_generation_aware_round_trip_and_legacy_players();
    test_client_base_prototype_round_trip();
    test_revision_entity_generation_and_name_bounds();
    test_duplicate_and_malformed_wire_inputs_are_rejected();
    return 0;
}
