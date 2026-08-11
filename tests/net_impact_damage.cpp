#include "net/impact_damage.hpp"
#include "net/wire_protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {

using namespace kraken::net;

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

ImpactDamage make_event()
{
    ImpactDamage event{};
    event.event_id = 0x10203040u;
    event.server_tick = 0x50607080u;
    event.attacker_entity_id = 0x11223344u;
    event.target_entity_id = 0x55667788u;
    event.attacker_generation = 0x1234u;
    event.target_generation = 0x5678u;
    event.gun_id = kMaxImpactDamageGunId;
    event.damage_type = kMaxImpactDamageType;
    event.damage = 1234.5f;
    event.post_health = 98765.25f;
    event.hit_position = {1.25f, -2.5f, 3.75f};
    event.direction = {-4.5f, 5.25f, -6.75f};
    event.normal = {7.0f, -8.125f, 9.5f};
    event.target_dead = true;
    event.damaged_part = "wheel_front_left";
    return event;
}

bool impact_damage_is_noop(const ImpactDamage& event)
{
    // Keep this pure contract aligned with runtime.cpp: the engine's native
    // InflictDamage path ignores damage below approximately 0.01. A dead
    // result remains meaningful even when its final damage is zero.
    constexpr float kEngineMinimumImpactDamage = 0.01f;
    return !event.target_dead && event.damage < kEngineMinimumImpactDamage;
}

std::uint16_t get_u16(const Byte* source)
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[0])) |
           static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[1]) << 8);
}

void set_u32(Byte* destination, const std::uint32_t value)
{
    for (int index = 0; index != 4; ++index)
        destination[index] = static_cast<Byte>((value >> (index * 8)) & 0xffu);
}

void set_u16(Byte* destination, const std::uint16_t value)
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

ImpactDamageCodecError encode_error(const ImpactDamage& event)
{
    std::array<Byte, kImpactDamageWireSize> bytes{};
    bytes.fill(Byte{0xab});
    const ImpactDamageCodecError error = encode_impact_damage(event, bytes);
    if (error != ImpactDamageCodecError::None) {
        CHECK(std::all_of(bytes.begin(), bytes.end(),
                          [](const Byte value) { return value == Byte{0xab}; }));
    }
    return error;
}

void check_vector(const VehicleVector3& actual, const VehicleVector3& expected)
{
    CHECK(actual.x == expected.x);
    CHECK(actual.y == expected.y);
    CHECK(actual.z == expected.z);
}

void check_event(const ImpactDamage& actual, const ImpactDamage& expected)
{
    CHECK(actual.event_id == expected.event_id);
    CHECK(actual.server_tick == expected.server_tick);
    CHECK(actual.attacker_entity_id == expected.attacker_entity_id);
    CHECK(actual.target_entity_id == expected.target_entity_id);
    CHECK(actual.attacker_generation == expected.attacker_generation);
    CHECK(actual.target_generation == expected.target_generation);
    CHECK(actual.gun_id == expected.gun_id);
    CHECK(actual.damage_type == expected.damage_type);
    CHECK(actual.damage == expected.damage);
    CHECK(actual.post_health == expected.post_health);
    check_vector(actual.hit_position, expected.hit_position);
    check_vector(actual.direction, expected.direction);
    check_vector(actual.normal, expected.normal);
    CHECK(actual.target_dead == expected.target_dead);
    CHECK(actual.damaged_part == expected.damaged_part);
}

void test_message_type_registration()
{
    CHECK(is_valid_message_type(MessageType::ImpactDamage));
}

void test_idg1_roundtrip_all_fields()
{
    const ImpactDamage expected = make_event();
    std::array<Byte, kImpactDamageWireSize + 4> bytes{};
    bytes.fill(Byte{0xcd});

    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    CHECK(get_u16(bytes.data() + 4) == kImpactDamageWireVersion);
    CHECK(get_u16(bytes.data() + 6) == kImpactDamageFlagTargetDead);
    CHECK(static_cast<std::uint8_t>(bytes[80]) == expected.damaged_part.size());
    CHECK(bytes[81] == Byte{} && bytes[82] == Byte{} && bytes[83] == Byte{});
    for (std::size_t index = 84 + expected.damaged_part.size();
         index < kImpactDamageWireSize; ++index)
        CHECK(bytes[index] == Byte{});
    CHECK(bytes[kImpactDamageWireSize] == Byte{0xcd});
    CHECK(bytes[kImpactDamageWireSize + 1] == Byte{0xcd});
    CHECK(bytes[kImpactDamageWireSize + 2] == Byte{0xcd});
    CHECK(bytes[kImpactDamageWireSize + 3] == Byte{0xcd});

    ImpactDamage actual{};
    CHECK(decode_impact_damage(
              ByteView{bytes.data(), kImpactDamageWireSize}, actual) ==
          ImpactDamageCodecError::None);
    check_event(actual, expected);

    ImpactDamage alive = expected;
    alive.target_dead = false;
    CHECK(encode_impact_damage(alive, bytes) == ImpactDamageCodecError::None);
    CHECK(get_u16(bytes.data() + 6) == 0);
    CHECK(decode_impact_damage(
              ByteView{bytes.data(), kImpactDamageWireSize}, actual) ==
          ImpactDamageCodecError::None);
    check_event(actual, alive);
}

void test_nullable_attacker_environment()
{
    ImpactDamage expected = make_event();
    expected.attacker_entity_id = kInvalidNetId;
    expected.attacker_generation = kInvalidEntityGeneration;
    expected.damaged_part.clear();

    std::array<Byte, kImpactDamageWireSize> bytes{};
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);

    ImpactDamage actual{};
    CHECK(decode_impact_damage(bytes, actual) == ImpactDamageCodecError::None);
    check_event(actual, expected);

    // A null attacker must not carry a generation, and a present attacker
    // must carry one. These are distinct malformed states.
    expected.attacker_generation = 1;
    CHECK(encode_error(expected) == ImpactDamageCodecError::InvalidAttacker);
    expected.attacker_entity_id = 1;
    expected.attacker_generation = kInvalidEntityGeneration;
    CHECK(encode_error(expected) == ImpactDamageCodecError::InvalidAttacker);
}

void test_finite_and_bounds_validation()
{
    ImpactDamage event = make_event();

    event.event_id = 0;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidEventId);
    event = make_event();
    event.target_entity_id = kInvalidNetId;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidTarget);
    event = make_event();
    event.target_generation = kInvalidEntityGeneration;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidGeneration);

    event = make_event();
    event.gun_id = -1;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidGunId);
    event = make_event();
    event.gun_id = kMaxImpactDamageGunId + 1;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidGunId);
    event = make_event();
    event.damage_type = -1;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidDamageType);
    event = make_event();
    event.damage_type = kMaxImpactDamageType + 1;
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidDamageType);

    event = make_event();
    event.damage = std::numeric_limits<float>::quiet_NaN();
    CHECK(encode_error(event) == ImpactDamageCodecError::NonFiniteValue);
    event = make_event();
    event.post_health = std::numeric_limits<float>::infinity();
    CHECK(encode_error(event) == ImpactDamageCodecError::NonFiniteValue);
    event = make_event();
    event.direction.y = -std::numeric_limits<float>::infinity();
    CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);

    event = make_event();
    event.damage = -0.01f;
    CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);
    event = make_event();
    event.damage = kMaxImpactDamage + 1.0f;
    CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);
    event = make_event();
    event.post_health = -0.01f;
    CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);
    event = make_event();
    event.post_health = kMaxImpactDamageHealth + 1.0f;
    CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);

    const std::array<float VehicleVector3::*, 3> components{
        &VehicleVector3::x, &VehicleVector3::y, &VehicleVector3::z};
    for (float VehicleVector3::* const component : components) {
        event = make_event();
        event.hit_position.*component = kMaxImpactDamageVectorComponent + 1.0f;
        CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);
        event = make_event();
        event.direction.*component = -kMaxImpactDamageVectorComponent - 1.0f;
        CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);
        event = make_event();
        event.normal.*component = std::numeric_limits<float>::infinity();
        CHECK(encode_error(event) == ImpactDamageCodecError::ValueOutOfBounds);
    }

    event = make_event();
    event.damaged_part.assign(kImpactDamagePartMaxBytes, 'p');
    CHECK(encode_error(event) == ImpactDamageCodecError::None);
    event.damaged_part.push_back('x');
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidPart);
    event = make_event();
    event.damaged_part = "bad\npart";
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidPart);
    event = make_event();
    event.damaged_part = std::string("bad\x7fpart", 8);
    CHECK(encode_error(event) == ImpactDamageCodecError::InvalidPart);
}

void test_decode_validation_of_untrusted_values()
{
    const ImpactDamage expected = make_event();
    std::array<Byte, kImpactDamageWireSize> bytes{};
    ImpactDamage decoded{};

    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u32(bytes.data() + 36, 0x7fc00000u); // quiet NaN damage
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::NonFiniteValue);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u32(bytes.data() + 40, 0x7f800000u); // +infinity post-health
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::NonFiniteValue);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u32(bytes.data() + 44, 0x49742410u); // 1,000,001.0f
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::ValueOutOfBounds);

    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u32(bytes.data() + 28, 0xffffffffu);
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::InvalidGunId);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u32(bytes.data() + 32, static_cast<std::uint32_t>(kMaxImpactDamageType + 1));
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::InvalidDamageType);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    bytes[80] = Byte{static_cast<unsigned char>(kImpactDamagePartMaxBytes + 1)};
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::InvalidPart);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    bytes[84] = Byte{1};
    CHECK(decode_impact_damage(bytes, decoded) ==
          ImpactDamageCodecError::InvalidPart);
}

void test_wire_size_magic_version_flags_and_padding()
{
    const ImpactDamage expected = make_event();
    std::array<Byte, kImpactDamageWireSize> bytes{};
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);

    ImpactDamage decoded{};
    CHECK(encode_impact_damage(expected,
                               MutableByteView{bytes.data(), kImpactDamageWireSize - 1}) ==
          ImpactDamageCodecError::OutputTooSmall);
    CHECK(decode_impact_damage(
              ByteView{bytes.data(), kImpactDamageWireSize - 1}, decoded) ==
          ImpactDamageCodecError::InputSizeMismatch);
    CHECK(decode_impact_damage(
              ByteView{bytes.data(), kImpactDamageWireSize + 1}, decoded) ==
          ImpactDamageCodecError::InputSizeMismatch);

    bytes[0] = Byte{0};
    CHECK(decode_impact_damage(bytes, decoded) == ImpactDamageCodecError::BadMagic);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u16(bytes.data() + 4, static_cast<std::uint16_t>(kImpactDamageWireVersion + 1));
    CHECK(decode_impact_damage(bytes, decoded) == ImpactDamageCodecError::BadVersion);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    set_u16(bytes.data() + 6, static_cast<std::uint16_t>(kImpactDamageFlagTargetDead | 0x0002u));
    CHECK(decode_impact_damage(bytes, decoded) == ImpactDamageCodecError::BadFlags);

    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    bytes[81] = Byte{1};
    CHECK(decode_impact_damage(bytes, decoded) == ImpactDamageCodecError::BadFlags);
    CHECK(encode_impact_damage(expected, bytes) == ImpactDamageCodecError::None);
    bytes[84 + expected.damaged_part.size()] = Byte{1};
    CHECK(decode_impact_damage(bytes, decoded) == ImpactDamageCodecError::BadFlags);

    ImpactDamage empty_part = expected;
    empty_part.damaged_part.clear();
    CHECK(encode_impact_damage(empty_part, bytes) == ImpactDamageCodecError::None);
    bytes[84] = Byte{1};
    CHECK(decode_impact_damage(bytes, decoded) == ImpactDamageCodecError::BadFlags);
}

void test_generation_boundaries()
{
    ImpactDamage event = make_event();
    event.attacker_generation = std::numeric_limits<EntityGeneration>::max();
    event.target_generation = std::numeric_limits<EntityGeneration>::max();
    CHECK(encode_error(event) == ImpactDamageCodecError::None);

    event = make_event();
    event.attacker_generation = std::numeric_limits<EntityGeneration>::max();
    event.target_generation = std::numeric_limits<EntityGeneration>::max();
    CHECK(encode_error(event) == ImpactDamageCodecError::None);
}

void test_deduplicator_wrap_capacity_duplicate()
{
    ImpactDamageDeduplicator deduplicator(3);
    CHECK(!deduplicator.accept(0));
    CHECK(deduplicator.accept(std::numeric_limits<std::uint32_t>::max() - 1));
    CHECK(deduplicator.accept(std::numeric_limits<std::uint32_t>::max()));
    CHECK(deduplicator.accept(1));
    CHECK(!deduplicator.accept(std::numeric_limits<std::uint32_t>::max()));
    CHECK(!deduplicator.accept(1));
    CHECK(deduplicator.contains(std::numeric_limits<std::uint32_t>::max() - 1));
    CHECK(deduplicator.contains(std::numeric_limits<std::uint32_t>::max()));
    CHECK(deduplicator.contains(1));

    CHECK(deduplicator.accept(2));
    CHECK(!deduplicator.contains(std::numeric_limits<std::uint32_t>::max() - 1));
    CHECK(deduplicator.contains(std::numeric_limits<std::uint32_t>::max()));
    CHECK(deduplicator.contains(1));
    CHECK(deduplicator.contains(2));

    deduplicator.clear();
    CHECK(!deduplicator.contains(1));
    CHECK(deduplicator.accept(1));

    ImpactDamageDeduplicator zero_capacity(0);
    CHECK(!zero_capacity.accept(1));
    CHECK(!zero_capacity.contains(1));
}

void test_noop_impact_is_not_presented_or_relayed()
{
    ImpactDamage event = make_event();
    event.target_dead = false;
    event.damage = 0.0f;
    event.post_health = 600.0f;
    event.damaged_part.clear();
    CHECK(impact_damage_is_noop(event));

    event.damage = 0.009f;
    CHECK(impact_damage_is_noop(event));

    // A real engine impact remains eligible for the native FX and health
    // reconciliation path, including the exact threshold boundary.
    event.damage = 0.01f;
    CHECK(!impact_damage_is_noop(event));
    event.damage = 25.0f;
    CHECK(!impact_damage_is_noop(event));

    // Zero final damage must not suppress an authoritative death transition.
    event.damage = 0.0f;
    event.target_dead = true;
    CHECK(!impact_damage_is_noop(event));
}

void test_runtime_replay_does_not_overwrite_native_health()
{
    // No runtime ABI seam is available to unit-test Vehicle::InflictDamage;
    // keep this source-level guard deterministic so a health workaround cannot
    // return unnoticed in the authoritative replay path.
    const std::filesystem::path test_path(__FILE__);
    const std::array<std::filesystem::path, 3> candidates{
        test_path.parent_path().parent_path() / "source" / "net" /
            "runtime.cpp",
        std::filesystem::current_path() / ".." / "source" / "net" /
            "runtime.cpp",
        std::filesystem::current_path() / "source" / "net" / "runtime.cpp"};
    std::string source;
    for (const std::filesystem::path& candidate : candidates) {
        std::ifstream input(candidate, std::ios::binary);
        if (input) {
            source.assign(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>());
            break;
        }
    }
    CHECK(!source.empty());
    if (source.empty())
        return;
    CHECK(source.find("target->Health().m_value.set(event.post_health)") ==
          std::string::npos);
    CHECK(source.find("present_authoritative_impact_damage(*target, event)") !=
          std::string::npos);
    CHECK(source.find("target->_EvaluateToDead()") != std::string::npos);
    // A death packet represents a one-shot native transition.  Replaying it
    // for every post-destruction collision grows ODE joints without bound.
    CHECK(source.find("const bool was_dead = vehicle->_GetDeadStatus();") !=
          std::string::npos);
    CHECK(source.find("if (!input_captured || was_dead)") !=
          std::string::npos);
    CHECK(source.find("skip duplicate dead impact") != std::string::npos);
    // The native death transition fires vehicle weapons before it marks the
    // object dead.  It must be scoped away from input/relay hooks rather than
    // treating those internal shots as player commands.
    CHECK(source.find("g_presenting_authoritative_death = true;") !=
          std::string::npos);
    CHECK(source.find("if (g_presenting_authoritative_death)") !=
          std::string::npos);
}

} // namespace

int main()
{
    test_message_type_registration();
    test_idg1_roundtrip_all_fields();
    test_nullable_attacker_environment();
    test_finite_and_bounds_validation();
    test_decode_validation_of_untrusted_values();
    test_wire_size_magic_version_flags_and_padding();
    test_generation_boundaries();
    test_deduplicator_wrap_capacity_duplicate();
    test_noop_impact_is_not_presented_or_relayed();
    test_runtime_replay_does_not_overwrite_native_health();

    if (failures != 0) {
        std::cerr << failures << " impact damage test(s) failed\n";
        return 1;
    }

    std::cout << "impact damage tests passed\n";
    return 0;
}
