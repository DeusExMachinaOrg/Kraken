#include "net/entity_registry.hpp"
#include "net/weapon_command.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {

using kraken::net::Byte;

void set_wire_u32(std::array<Byte, kraken::net::kWeaponCommandWireSize>& wire,
                  std::size_t offset, std::uint32_t value) noexcept
{
    for (std::size_t byte = 0; byte != sizeof(value); ++byte)
        wire[offset + byte] = static_cast<Byte>(value >> (8 * byte));
}

void set_wire_f32(std::array<Byte, kraken::net::kWeaponCommandWireSize>& wire,
                  std::size_t offset, float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    set_wire_u32(wire, offset, bits);
}

bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

kraken::net::WeaponCommand valid_command()
{
    using namespace kraken::net;
    WeaponCommand command{};
    command.entity_id = 42;
    command.sequence = 0xF0E0D0C0u;
    command.client_tick = 99;
    command.gun_id = 3;
    command.trigger_held = true;
    command.target_entity_id = 17;
    command.shot_id = 0x10203040u;
    command.aim_point = {12.5f, -34.25f, 56.75f};
    command.has_aim_point = true;
    command.aim_speed = 0.37f;
    command.shells_in_current_charge = 17;
    command.shells_in_pool = 99;
    command.has_ammo_state = true;
    return command;
}

bool same_command(const kraken::net::WeaponCommand& expected,
                  const kraken::net::WeaponCommand& actual)
{
    return actual.entity_id == expected.entity_id &&
           actual.sequence == expected.sequence &&
           actual.client_tick == expected.client_tick &&
           actual.gun_id == expected.gun_id &&
           actual.trigger_held == expected.trigger_held &&
           actual.target_entity_id == expected.target_entity_id &&
           actual.shot_id == expected.shot_id &&
           actual.aim_point.x == expected.aim_point.x &&
           actual.aim_point.y == expected.aim_point.y &&
           actual.aim_point.z == expected.aim_point.z &&
           actual.has_aim_point == expected.has_aim_point &&
           actual.aim_speed == expected.aim_speed &&
           actual.shells_in_current_charge == expected.shells_in_current_charge &&
           actual.shells_in_pool == expected.shells_in_pool &&
           actual.has_ammo_state == expected.has_ammo_state;
}

kraken::net::NetId capture_current_native_target(
    const kraken::net::ObjId custom_target_obj_id,
    const kraken::net::ObjId seen_target_obj_id,
    kraken::net::NetId& retained_target)
{
    retained_target = kraken::net::kInvalidNetId;
    const auto lookup_current = [](const kraken::net::ObjId obj_id,
                                   kraken::net::NetId& entity_id) {
        if (obj_id == 900) {
            entity_id = 17;
            return true;
        }
        if (obj_id == 901) {
            entity_id = 18;
            return true;
        }
        return false;
    };

    if (custom_target_obj_id != kraken::net::kInvalidObjId &&
        custom_target_obj_id != 0) {
        kraken::net::NetId entity_id = kraken::net::kInvalidNetId;
        if (lookup_current(custom_target_obj_id, entity_id)) {
            retained_target = entity_id;
            return entity_id;
        }
    }
    if (seen_target_obj_id == kraken::net::kInvalidObjId ||
        seen_target_obj_id == 0)
        return kraken::net::kInvalidNetId;

    kraken::net::NetId entity_id = kraken::net::kInvalidNetId;
    if (!lookup_current(seen_target_obj_id, entity_id))
        return kraken::net::kInvalidNetId;
    retained_target = entity_id;
    return entity_id;
}

bool target_release_free_aim_semantics()
{
    using namespace kraken::net;
    NetId retained_target = kInvalidNetId;
    WeaponCommand target = valid_command();
    target.target_entity_id = capture_current_native_target(900, 901,
                                                            retained_target);
    if (target.target_entity_id != 17 || retained_target != 17)
        return false;

    std::array<Byte, kWeaponCommandWireSize> wire{};
    WeaponCommand decoded{};
    if (encode_weapon_command(target, wire) != WeaponCommandCodecError::None ||
        decode_weapon_command(wire, decoded) != WeaponCommandCodecError::None ||
        decoded.target_entity_id != 17 || !decoded.trigger_held)
        return false;

    WeaponCommand seen_target = target;
    seen_target.sequence++;
    seen_target.shot_id++;
    seen_target.target_entity_id = capture_current_native_target(
        902, 901, retained_target);
    if (seen_target.target_entity_id != 18 || retained_target != 18 ||
        encode_weapon_command(seen_target, wire) != WeaponCommandCodecError::None ||
        decode_weapon_command(wire, decoded) != WeaponCommandCodecError::None ||
        decoded.target_entity_id != 18 || !decoded.trigger_held)
        return false;

    WeaponCommand release = seen_target;
    release.sequence++;
    release.shot_id++;
    release.trigger_held = false;
    retained_target = 17;
    release.target_entity_id = capture_current_native_target(
        kInvalidObjId, kInvalidObjId, retained_target);
    if (release.target_entity_id != kInvalidNetId ||
        retained_target != kInvalidNetId ||
        encode_weapon_command(release, wire) != WeaponCommandCodecError::None ||
        decode_weapon_command(wire, decoded) != WeaponCommandCodecError::None ||
        decoded.target_entity_id != kInvalidNetId || decoded.trigger_held)
        return false;

    WeaponCommand free_aim = release;
    free_aim.sequence++;
    free_aim.shot_id++;
    free_aim.trigger_held = true;
    retained_target = 17;
    free_aim.target_entity_id = capture_current_native_target(
        0, 0, retained_target);
    if (free_aim.target_entity_id != kInvalidNetId ||
        retained_target != kInvalidNetId ||
        encode_weapon_command(free_aim, wire) != WeaponCommandCodecError::None ||
        decode_weapon_command(wire, decoded) != WeaponCommandCodecError::None ||
        decoded.target_entity_id != kInvalidNetId || !decoded.trigger_held)
        return false;

    retained_target = 17;
    return capture_current_native_target(902, 902, retained_target) ==
               kInvalidNetId &&
           retained_target == kInvalidNetId;
}

bool current_bound_vehicle_pointer(const void* candidate,
                                   const void* current_object,
                                   const kraken::net::ObjId bound_obj_id) noexcept
{
    // The production guard may compare addresses, but must not inspect the
    // candidate object until ObjContainer returns this exact live object for
    // the stable bound ObjId.
    return candidate != nullptr && current_object != nullptr &&
           bound_obj_id != kraken::net::kInvalidObjId &&
           candidate == current_object;
}

bool target_capture_lifetime_semantics()
{
    using namespace kraken::net;
    int current_object = 0;
    int stale_object = 0;
    return current_bound_vehicle_pointer(&current_object, &current_object, 42) &&
           !current_bound_vehicle_pointer(&stale_object, &current_object, 42) &&
           !current_bound_vehicle_pointer(&current_object, &current_object,
                                          kInvalidObjId) &&
           !current_bound_vehicle_pointer(nullptr, &current_object, 42);
}

bool target_capture_abi_source_guard()
{
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
    return !source.empty() &&
           source.find("kVehicleGetSeenObjIdAddress") != std::string::npos &&
           source.find("mov eax, vehicle") != std::string::npos &&
           source.find("mov edx, 00550A50h") != std::string::npos &&
           source.find("vehicle.GetSeenObjId()") == std::string::npos;
}

} // namespace

int main()
{
    using namespace kraken::net;

    if (!check(target_release_free_aim_semantics(),
               "native target release clears stale target for free aim"))
        return 1;
    if (!check(target_capture_lifetime_semantics(),
               "target capture rejects stale vehicle pointers"))
        return 1;
    if (!check(target_capture_abi_source_guard(),
               "seen-target accessor uses the EAX ABI bridge"))
        return 1;

    const WeaponCommand expected = valid_command();
    WeaponCommand decoded{};
    std::array<Byte, kWeaponCommandWireSize> wire{};
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid WPN6 command encodes"))
        return 1;
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::None,
               "valid WPN6 command decodes"))
        return 2;
    if (!check(same_command(expected, decoded),
               "sequence, shot_id, trigger, target, and aim round-trip"))
        return 3;

    WeaponCommand targetless = expected;
    targetless.target_entity_id = 0;
    targetless.shot_id = 0x50607080u;
    targetless.aim_point = {-1000.0f, 0.5f, 250000.0f};
    if (!check(encode_weapon_command(targetless, wire) ==
                   WeaponCommandCodecError::None,
               "targetless finite aim encodes"))
        return 4;
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::None &&
                   same_command(targetless, decoded) &&
                   std::isfinite(decoded.aim_point.x) &&
                   std::isfinite(decoded.aim_point.y) &&
                   std::isfinite(decoded.aim_point.z),
               "targetless finite aim round-trips"))
        return 5;

    WeaponCommand invalid = expected;
    invalid.shot_id = 0;
    if (!check(encode_weapon_command(invalid, wire) ==
                   WeaponCommandCodecError::InvalidShotId,
               "zero shot_id is rejected during encode"))
        return 6;
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes after zero shot_id case"))
        return 7;
    set_wire_u32(wire, 28, 0);
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::InvalidShotId,
               "zero shot_id is rejected during decode"))
        return 8;

    const std::array<float, 3> invalid_values = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        kMaxNetworkAimPointComponent + 1.0f,
    };
    const std::array<std::size_t, 3> aim_offsets = {32, 36, 40};
    for (std::size_t component = 0; component != aim_offsets.size(); ++component) {
        for (const float value : invalid_values) {
            invalid = expected;
            if (component == 0)
                invalid.aim_point.x = value;
            else if (component == 1)
                invalid.aim_point.y = value;
            else
                invalid.aim_point.z = value;
            if (!check(encode_weapon_command(invalid, wire) ==
                           WeaponCommandCodecError::InvalidAimPoint,
                       "non-finite or out-of-bound aim is rejected during encode"))
                return 9;

            if (!check(encode_weapon_command(expected, wire) ==
                           WeaponCommandCodecError::None,
                       "valid command re-encodes before malformed aim decode"))
                return 10;
            set_wire_f32(wire, aim_offsets[component], value);
            if (!check(decode_weapon_command(wire, decoded) ==
                           WeaponCommandCodecError::InvalidAimPoint,
                       "non-finite or out-of-bound aim is rejected during decode"))
                return 11;
        }
    }

    invalid = expected;
    invalid.aim_speed = std::numeric_limits<float>::quiet_NaN();
    if (!check(encode_weapon_command(invalid, wire) ==
                   WeaponCommandCodecError::InvalidAimSpeed,
               "non-finite aim speed is rejected during encode"))
        return 12;
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes before malformed speed decode"))
        return 13;
    set_wire_f32(wire, 44, std::numeric_limits<float>::infinity());
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::InvalidAimSpeed,
               "non-finite aim speed is rejected during decode"))
        return 14;

    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes before flag cases"))
        return 12;
    wire[6] = static_cast<Byte>(2);
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::BadFlags,
               "illegal trigger flag is rejected"))
        return 13;
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes before aim flag case"))
        return 14;
    wire[7] = static_cast<Byte>(4);
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::BadFlags,
               "reserved weapon flag is rejected"))
        return 15;

    if (!check(encode_weapon_command(expected,
                                    std::span<Byte>(wire).subspan(0,
                                        kWeaponCommandWireSize - 1)) ==
                   WeaponCommandCodecError::OutputTooSmall,
               "short output is rejected"))
        return 16;
    std::array<Byte, kWeaponCommandWireSize - 1> short_wire{};
    if (!check(decode_weapon_command(short_wire, decoded) ==
                   WeaponCommandCodecError::InputSizeMismatch,
               "short input is rejected"))
        return 17;
    std::array<Byte, kWeaponCommandWireSize + 1> long_wire{};
    if (!check(decode_weapon_command(long_wire, decoded) ==
                   WeaponCommandCodecError::InputSizeMismatch,
               "long input is rejected"))
        return 18;

    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes before header cases"))
        return 19;
    wire[0] = Byte{};
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::BadMagic,
               "bad WPN6 magic is rejected"))
        return 20;
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes before version case"))
        return 21;
    wire[4] = static_cast<Byte>(2);
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::BadVersion,
               "bad WPN6 version is rejected"))
        return 22;

    std::cout << "weapon command tests passed\n";
    return 0;
}
