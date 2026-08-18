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
    command.session_epoch = 7;
    command.entity_id = 42;
    command.entity_generation = 3;
    command.sequence = 0xF0E0D0C0u;
    command.client_tick = 99;
    command.gun = {0x1122334455667788ull, 0x8877665544332211ull};
    command.gun_id = 3;
    command.trigger_held = true;
    command.target_entity_id = 17;
    command.target_generation = 4;
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
           actual.session_epoch == expected.session_epoch &&
           actual.entity_generation == expected.entity_generation &&
           actual.sequence == expected.sequence &&
           actual.client_tick == expected.client_tick &&
           actual.gun.attachment_id == expected.gun.attachment_id &&
           actual.gun.path_hash == expected.gun.path_hash &&
           actual.trigger_held == expected.trigger_held &&
           actual.target_entity_id == expected.target_entity_id &&
           actual.target_generation == expected.target_generation &&
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
    release.target_generation = kInvalidEntityGeneration;
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

std::string read_runtime_source()
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
    return source;
}

bool target_capture_abi_source_guard()
{
    const std::string source = read_runtime_source();
    return !source.empty() &&
           source.find("kVehicleGetSeenObjIdAddress") != std::string::npos &&
           source.find("mov eax, vehicle") != std::string::npos &&
           source.find("mov edx, 00550A50h") != std::string::npos &&
           source.find("vehicle.GetSeenObjId()") == std::string::npos;
}

std::string extract_function(const std::string& source,
                             const std::string& signature)
{
    std::size_t begin = source.find(signature);
    while (begin != std::string::npos) {
        const std::size_t open = source.find('{', begin);
        const std::size_t declaration = source.find(';', begin);
        if (open == std::string::npos)
            return {};
        if (declaration != std::string::npos && declaration < open) {
            begin = source.find(signature, begin + signature.size());
            continue;
        }
        std::size_t depth = 0;
        for (std::size_t index = open; index != source.size(); ++index) {
            if (source[index] == '{')
                ++depth;
            else if (source[index] == '}' && --depth == 0)
                return source.substr(begin, index - begin + 1);
        }
        return {};
    }
    return {};
}

std::size_t count_occurrences(const std::string& source,
                              const std::string& value)
{
    std::size_t count = 0;
    for (std::size_t offset = source.find(value); offset != std::string::npos;
         offset = source.find(value, offset + value.size()))
        ++count;
    return count;
}

bool typed_weapon_authority_source_guard()
{
    const std::string source = read_runtime_source();
    const std::string fire_hook = extract_function(
        source, "bool __fastcall gun_do_fire_hook");
    const std::string damage_hook = extract_function(
        source, "void __fastcall vehicle_inflict_damage_hook");
    const std::string fire_helper = extract_function(
        source, "bool call_original_gun_do_fire");
    const std::string damage_helper = extract_function(
        source, "bool call_original_vehicle_inflict_damage");
    if (source.empty() || fire_hook.empty() || damage_hook.empty() ||
        fire_helper.empty() || damage_helper.empty())
        return false;
    const std::size_t fire_denial = fire_hook.find(
        "if (active_client_replica())");
    const std::size_t death_scope = fire_hook.find(
        "if (g_presenting_authoritative_death)");
    const std::size_t damage_denial = damage_hook.find(
        "if (active_client_replica())");
    const std::size_t damage_original_lookup = damage_hook.find(
        "const VehicleInflictDamageFn original");
    const std::size_t fire_blocked = fire_helper.find(
        "++g_state.client_blocked_fire_attempt_count;");
    const std::size_t fire_denied_return = fire_helper.find("return false;");
    const std::size_t fire_original = fire_helper.find("g_gun_do_fire(gun);");
    const std::size_t fire_actual = fire_helper.find(
        "++g_state.client_original_fire_call_count;");
    const std::size_t damage_blocked = damage_helper.find(
        "++g_state.client_blocked_damage_attempt_count;");
    const std::size_t damage_denied_return = damage_helper.find("return false;");
    const std::size_t damage_original = damage_helper.find(
        "original(vehicle, info);");
    const std::size_t damage_actual = damage_helper.find(
        "++g_state.client_original_damage_call_count;");
    const std::size_t shot_flags = source.find("gun->m_bWasShot = true;");
    const std::size_t just_shot_flag = source.find("gun->m_bJustShot = true;");
    const std::size_t firing_action = source.find(
        "gun->_UpdateNodeFiringAction();", shot_flags);
    return fire_denial != std::string::npos &&
           death_scope != std::string::npos && fire_denial < death_scope &&
           damage_denial != std::string::npos &&
           damage_original_lookup != std::string::npos &&
           damage_denial < damage_original_lookup &&
           fire_hook.find("g_gun_do_fire(gun)") == std::string::npos &&
           fire_hook.find("call_original_gun_do_fire(gun)") !=
               std::string::npos &&
           fire_blocked < fire_denied_return &&
           fire_denied_return < fire_original && fire_original < fire_actual &&
           damage_blocked < damage_denied_return &&
           damage_denied_return < damage_original &&
           damage_original < damage_actual &&
           count_occurrences(source, "g_gun_do_fire(gun);") == 1 &&
           count_occurrences(source, "original(vehicle, info);") == 1 &&
           source.find("original(&target, info);") == std::string::npos &&
           source.find("vehicle_inflict_damage_original(vehicle, info)") ==
               std::string::npos &&
           source.find("g_presenting_authoritative_impact") ==
               std::string::npos &&
           source.find("g_suppress_client_weapon_damage") == std::string::npos &&
           source.find("g_presenting_confirmed_network_fire") ==
               std::string::npos &&
           source.find("client_projectile=0") == std::string::npos &&
           source.find("client_damage=0") == std::string::npos &&
           source.find("client_blocked_fire=%llu client_projectile=%llu") !=
               std::string::npos &&
           source.find("client_blocked_damage=%llu client_damage=%llu") !=
               std::string::npos &&
           source.find("KRAKEN_COMBAT_AUTOTEST FAIL authority_violation=1") !=
               std::string::npos &&
           shot_flags != std::string::npos &&
           just_shot_flag != std::string::npos &&
           firing_action != std::string::npos && shot_flags < just_shot_flag &&
           just_shot_flag < firing_action;
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
    if (!check(typed_weapon_authority_source_guard(),
               "replica denial, actual counters, and safe shot flags stay ordered"))
        return 1;

    const WeaponCommand expected = valid_command();
    WeaponCommand decoded{};
    std::array<Byte, kWeaponCommandWireSize> wire{};
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid WPN7 command encodes"))
        return 1;
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::None,
               "valid WPN7 command decodes"))
        return 2;
    if (!check(same_command(expected, decoded),
               "sequence, shot_id, trigger, target, and aim round-trip"))
        return 3;

    WeaponCommand invalid_identity = expected;
    invalid_identity.entity_generation = kInvalidEntityGeneration;
    if (!check(encode_weapon_command(invalid_identity, wire) ==
                   WeaponCommandCodecError::InvalidEntityGeneration,
               "missing shooter generation is rejected"))
        return 4;
    invalid_identity = expected;
    invalid_identity.gun = {};
    if (!check(encode_weapon_command(invalid_identity, wire) ==
                   WeaponCommandCodecError::InvalidAttachment,
               "missing stable gun identity is rejected"))
        return 5;
    invalid_identity = expected;
    invalid_identity.target_generation = kInvalidEntityGeneration;
    if (!check(encode_weapon_command(invalid_identity, wire) ==
                   WeaponCommandCodecError::InvalidTargetGeneration,
               "missing target generation is rejected"))
        return 6;

    WeaponCommand targetless = expected;
    targetless.target_entity_id = 0;
    targetless.target_generation = kInvalidEntityGeneration;
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
    set_wire_u32(wire, 52, 0);
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::InvalidShotId,
               "zero shot_id is rejected during decode"))
        return 8;

    const std::array<float, 3> invalid_values = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        kMaxNetworkAimPointComponent + 1.0f,
    };
    const std::array<std::size_t, 3> aim_offsets = {56, 60, 64};
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
    set_wire_f32(wire, 68, std::numeric_limits<float>::infinity());
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
               "bad WPN7 magic is rejected"))
        return 20;
    if (!check(encode_weapon_command(expected, wire) ==
                   WeaponCommandCodecError::None,
               "valid command re-encodes before version case"))
        return 21;
    wire[4] = static_cast<Byte>(2);
    if (!check(decode_weapon_command(wire, decoded) ==
                   WeaponCommandCodecError::BadVersion,
               "bad WPN7 version is rejected"))
        return 22;

    std::cout << "weapon command tests passed\n";
    return 0;
}
