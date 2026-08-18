#include "net/combat_presentation.hpp"
#include "net/wire_protocol.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace kraken::net;

namespace {

int failures = 0;

void check(const bool condition, const char* const message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

NetEntityRef entity(const NetId id, const EntityGeneration generation = 1)
{
    return {id, generation};
}

GunAttachmentIdentity gun(const std::uint64_t id = 7,
                          const StablePathHash path = 0x7007)
{
    return {id, path};
}

CombatPose pose()
{
    return {{10.0f, -20.0f, 30.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
}

ResourceCue cue(const char* const name)
{
    return make_resource_cue(name);
}

void put_u16(std::vector<Byte>& bytes, const std::size_t offset,
             const std::uint16_t value)
{
    bytes[offset + 0] = static_cast<Byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u64(std::vector<Byte>& bytes, const std::size_t offset,
             const std::uint64_t value)
{
    for (std::size_t byte = 0; byte != 8; ++byte)
        bytes[offset + byte] = static_cast<Byte>((value >> (byte * 8)) & 0xffu);
}

ImpactPresentation impact(const bool damaged)
{
    ImpactPresentation value{};
    value.session_epoch = 9;
    value.event_id = damaged ? 100 : 101;
    value.server_tick = 500;
    value.shot_id = 77;
    value.shooter = entity(11, 3);
    value.gun = gun(4, 0x4004);
    value.target.kind = ImpactTargetKind::DynamicEntity;
    value.target.dynamic = entity(12, 8);
    value.target_part = gun(3, 0x3003);
    value.surface = SurfaceKind::Metal;
    value.effect_cue = cue("effects/impact/metal");
    value.decal_cue = damaged ? cue("decals/impact/metal") : ResourceCue{};
    value.hit_position = {1.0f, 2.0f, 3.0f};
    value.effect_position = {4.0f, 5.0f, 6.0f};
    value.contact_normal = {0.0f, 1.0f, 0.0f};
    value.decal_tangent = damaged ? VehicleVector3{1.0f, 0.0f, 0.0f}
                                  : VehicleVector3{};
    value.has_decal_tangent = damaged;
    value.mesh_id = 0xfeed1234;
    value.material_id = 0x00abcdef;
    value.effect_rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    value.effect_scale = 1.25f;
    value.remove_if_free = true;
    value.did_damage = damaged;
    value.blocked_reason = damaged ? ImpactBlockedReason::None
                                   : ImpactBlockedReason::Occluded;
    return value;
}

DeathWreckPresentation death()
{
    DeathWreckPresentation value{};
    value.session_epoch = 9;
    value.transition_id = 88;
    value.server_tick = 600;
    value.entity = entity(12, 8);
    value.wreck_entity = entity(112, 1);
    value.reason = DeathWreckReason::Combat;
    value.death_cue = cue("effects/death/vehicle");
    value.wreck_cue = cue("wrecks/vehicle/default");
    value.wreck_archive_id = 0x123456789ull;
    value.wreck_archive_revision = 4;
    value.wreck_archive_digest = 0xabcdef0123456789ull;
    value.wreck_archive_size = 8192;
    value.wreck_archive_chunk_count = 8;
    value.wreck_archive_chunk_size = 1024;
    value.wreck_variant_id = 0x1011;
    value.broken_parts = {{1, 0x1001}, {2, 0x1002}};
    return value;
}

WeaponTriggerState trigger(const std::size_t index)
{
    WeaponTriggerState value{};
    value.session_epoch = 9;
    value.transition_id = 1000 + index;
    value.server_tick = 700 + static_cast<std::uint32_t>(index);
    value.shooter = entity(1000 + static_cast<NetId>(index), 1);
    value.gun = gun(1 + index, 0x10000 + index);
    value.trigger_held = true;
    value.shells_in_pool = 20;
    value.reload_fraction = 0.25f;
    return value;
}

WeaponAimState aim(const std::size_t index)
{
    WeaponAimState value{};
    value.session_epoch = 9;
    value.update_sequence = 2000 + index;
    value.server_tick = 800 + static_cast<std::uint32_t>(index);
    value.shooter = entity(1000 + static_cast<NetId>(index), 1);
    value.gun = gun(1 + index, 0x10000 + index);
    value.aim_point = {1.0f, 2.0f, 3.0f};
    value.aim_direction = {0.0f, 0.0f, 1.0f};
    value.aim_speed = 4.0f;
    return value;
}

void test_resource_cues_and_policies()
{
    const ResourceCue normalized = cue("Effects\\Metal/Hit");
    check(normalized.name == "Effects/Metal/Hit", "cue separators normalize");
    check(normalized.hash == resource_cue_hash(normalized.name),
          "cue hash is deterministic");
    check(validate_resource_cue(normalized) ==
              CombatPresentationCodecError::None,
          "canonical cue validates");
    check(normalize_resource_name("../escape").empty(),
          "parent traversal is rejected");
    check(normalize_resource_name("effects//double").empty(),
          "empty path component is rejected");
    check(normalize_resource_name("/absolute").empty(),
          "absolute path is rejected");
    const std::string nul_name = std::string("effects/") + '\0' + "bad";
    check(normalize_resource_name(nul_name).empty(),
          "NUL is rejected");
    const std::string control_name = std::string("effects/") + '\x01' + "bad";
    check(normalize_resource_name(control_name).empty(),
          "control bytes are rejected");
    ResourceCue collision = normalized;
    ++collision.hash;
    check(validate_resource_cue(collision) ==
              CombatPresentationCodecError::InvalidCue,
          "cue hash collision/mismatch is rejected");
    ResourceCue only_name{normalized.name, 0};
    check(validate_resource_cue(only_name) ==
              CombatPresentationCodecError::InvalidCue,
          "cue name without hash is rejected");
    ResourceCue only_hash{"", normalized.hash};
    check(validate_resource_cue(only_hash) ==
              CombatPresentationCodecError::InvalidCue,
          "cue hash without name is rejected");
    check(validate_resource_cue({}, true) ==
              CombatPresentationCodecError::None,
          "empty cue is representable when allowed");
    check(validate_resource_cue({}, false) ==
              CombatPresentationCodecError::InvalidCue,
          "empty cue is rejected when required");

    HornState inactive{};
    inactive.session_epoch = 9;
    inactive.transition_id = 1;
    inactive.vehicle = entity(1);
    check(validate_horn_state(inactive) == CombatPresentationCodecError::None,
          "inactive horn may fail closed without a cue");
    inactive.active = true;
    check(validate_horn_state(inactive) == CombatPresentationCodecError::InvalidCue,
          "active horn requires canonical cue");

    ImpactPresentation no_damage_effect_only = impact(false);
    check(validate_impact_presentation(no_damage_effect_only) ==
              CombatPresentationCodecError::None,
          "no-damage impact accepts effect-only cue");
    ImpactPresentation no_damage_decal_only = impact(false);
    no_damage_decal_only.effect_cue = {};
    no_damage_decal_only.decal_cue = cue("decals/impact/blocked");
    no_damage_decal_only.has_decal_tangent = true;
    no_damage_decal_only.decal_tangent = {1.0f, 0.0f, 0.0f};
    check(validate_impact_presentation(no_damage_decal_only) ==
              CombatPresentationCodecError::None,
          "no-damage impact accepts decal-only cue");
    ImpactPresentation damage_effect_only = impact(true);
    damage_effect_only.decal_cue = {};
    check(validate_impact_presentation(damage_effect_only) ==
              CombatPresentationCodecError::None,
          "damage impact accepts effect-only cue");
    ImpactPresentation damage_decal_only = impact(true);
    damage_decal_only.effect_cue = {};
    check(validate_impact_presentation(damage_decal_only) ==
              CombatPresentationCodecError::None,
          "damage impact accepts decal-only cue");
    ImpactPresentation no_cues = impact(false);
    no_cues.effect_cue = {};
    no_cues.decal_cue = {};
    check(validate_impact_presentation(no_cues) ==
              CombatPresentationCodecError::InvalidCue,
          "impact rejects both cues empty");
    ImpactPresentation nonfinite = impact(true);
    nonfinite.effect_scale = std::numeric_limits<float>::quiet_NaN();
    check(validate_impact_presentation(nonfinite) ==
              CombatPresentationCodecError::NonFiniteValue,
          "non-finite effect scale is rejected");
    ImpactPresentation invalid_enum = impact(true);
    invalid_enum.surface = static_cast<SurfaceKind>(0xff);
    check(validate_impact_presentation(invalid_enum) ==
              CombatPresentationCodecError::InvalidEnum,
          "unknown surface enum is rejected");

    ImpactPresentation terrain = impact(false);
    terrain.target = {ImpactTargetKind::Environment, {}, {},
                      EnvironmentKind::Terrain};
    terrain.target_part = {};
    check(validate_impact_presentation(terrain) ==
              CombatPresentationCodecError::None,
          "terrain environment uses world-space identity without fake object");
    ImpactPresentation road = terrain;
    road.event_id = 102;
    road.target.environment_kind = EnvironmentKind::Road;
    check(validate_impact_presentation(road) ==
              CombatPresentationCodecError::None,
          "road environment kind is bounded and valid");
    ImpactPresentation unbound = terrain;
    unbound.event_id = 103;
    unbound.target.environment_kind = EnvironmentKind::UnboundStatic;
    check(validate_impact_presentation(unbound) ==
              CombatPresentationCodecError::None,
          "unbound environment remains valid and fail-closed for static decal");
    ImpactPresentation fake_environment = terrain;
    fake_environment.event_id = 104;
    fake_environment.target.dynamic = entity(99, 1);
    check(validate_impact_presentation(fake_environment) ==
              CombatPresentationCodecError::InvalidTarget,
          "environment rejects fake dynamic identity");
    ImpactPresentation conflated_tangent = impact(false);
    conflated_tangent.event_id = 105;
    conflated_tangent.decal_cue = cue("decals/impact/blocked");
    conflated_tangent.decal_tangent = {1.0f, 0.0f, 0.0f};
    conflated_tangent.has_decal_tangent = false;
    check(validate_impact_presentation(conflated_tangent) ==
              CombatPresentationCodecError::InvalidTarget,
          "decal tangent cannot be inferred from an unflagged direction");
    ImpactPresentation incoming = impact(false);
    incoming.event_id = 106;
    incoming.has_incoming_direction = true;
    incoming.incoming_direction = {0.0f, 0.0f, 1.0f};
    check(validate_impact_presentation(incoming) ==
              CombatPresentationCodecError::None,
          "incoming direction is independently flagged");
}

void test_impact_and_versioned_wire()
{
    ImpactPresentation expected = impact(true);
    std::vector<Byte> bytes;
    check(encode_impact_presentation(expected, bytes) ==
              CombatPresentationCodecError::None,
          "impact encodes");
    check(bytes.size() == kImpactPresentationWirePrefixSize +
                             2 * kResourceCueWirePrefixSize +
                             expected.effect_cue.name.size() +
                             expected.decal_cue.name.size(),
          "impact has exact canonical size");
    ImpactPresentation decoded{};
    check(decode_impact_presentation(bytes, decoded) ==
              CombatPresentationCodecError::None,
          "impact decodes");
    check(decoded.target.dynamic.generation == 8 &&
              decoded.target_part.path_hash == 0x3003 &&
              decoded.surface == SurfaceKind::Metal &&
              decoded.decal_tangent.x == 1.0f &&
              decoded.effect_position.z == 6.0f && decoded.effect_scale == 1.25f &&
              decoded.effect_cue.name == expected.effect_cue.name,
          "impact typed fields round trip");

    std::vector<Byte> trailing = bytes;
    trailing.push_back(Byte{});
    check(decode_impact_presentation(trailing, decoded) ==
              CombatPresentationCodecError::InputSizeMismatch,
          "impact trailing bytes are rejected");
    check(decode_impact_presentation(
              ByteView{bytes}.first(bytes.size() - 1), decoded) ==
              CombatPresentationCodecError::InputSizeMismatch,
          "impact truncation is rejected");

    std::vector<Byte> bad_hash = bytes;
    const std::size_t effect_hash = kImpactPresentationWirePrefixSize +
                                    expected.effect_cue.name.size() + 2;
    put_u64(bad_hash, effect_hash, expected.effect_cue.hash + 1);
    check(decode_impact_presentation(bad_hash, decoded) ==
              CombatPresentationCodecError::InvalidCue,
          "wire cue hash collision is rejected");

    std::vector<Byte> old_version = bytes;
    put_u16(old_version, 4, kCombatPresentationPreviousWireVersion);
    check(decode_impact_presentation(old_version, decoded) ==
              CombatPresentationCodecError::BadVersion,
          "v4 combat presentation is incompatible");

    ImpactPresentation bad_flags = bytes.empty() ? ImpactPresentation{} : expected;
    std::vector<Byte> malformed = bytes;
    // kind + reserved + dynamic/static identities + target part = 76 bytes;
    // the next byte is SurfaceKind and the following byte is the bounded flags.
    malformed[kCombatPresentationHeaderSize + 77] = static_cast<Byte>(0x80);
    check(decode_impact_presentation(malformed, bad_flags) !=
              CombatPresentationCodecError::None,
          "unknown impact flags are rejected");
    (void)bad_flags;

    std::vector<Byte> oversized = bytes;
    oversized.resize(kMaxImpactPresentationWireSize + 1);
    check(decode_impact_presentation(oversized, decoded) ==
              CombatPresentationCodecError::PayloadTooLarge,
          "impact maximum payload bound is enforced");
}

void test_shot_damage_horn_death()
{
    ShotConfirmed shot_value{};
    shot_value.session_epoch = 9;
    shot_value.shot_id = 55;
    shot_value.server_tick = 900;
    shot_value.shooter = entity(3, 2);
    shot_value.gun = gun();
    shot_value.burst_id = 2;
    shot_value.burst_size = 2;
    shot_value.burst_index = 1;
    shot_value.muzzle_pose = pose();
    shot_value.presentation.muzzle_cue = cue("weapons/muzzle");
    shot_value.presentation.projectile_cue = cue("weapons/projectile");
    shot_value.presentation.shot_cue = cue("audio/shot");
    shot_value.presentation.reload_cue = cue("audio/reload");
    std::vector<Byte> bytes;
    check(encode_shot_confirmed(shot_value, bytes) ==
              CombatPresentationCodecError::None,
          "shot with four cues encodes");
    check(bytes.size() <= kMaxShotConfirmedWireSize, "shot stays bounded");
    ShotConfirmed decoded_shot{};
    check(decode_shot_confirmed(bytes, decoded_shot) ==
              CombatPresentationCodecError::None &&
              decoded_shot.presentation.reload_cue.name == "audio/reload",
          "shot cues round trip");

    DamageResult damage{};
    damage.session_epoch = 9;
    damage.event_id = 56;
    damage.server_tick = 901;
    damage.shot_id = shot_value.shot_id;
    damage.impact_event_id = 100;
    damage.shooter = shot_value.shooter;
    damage.target = entity(12, 8);
    damage.damage = 25.0f;
    damage.post_health = 0.0f;
    damage.damaged_part = "front_plate";
    damage.dead_transition = true;
    check(encode_damage_result(damage, bytes) ==
              CombatPresentationCodecError::None,
          "damage correlation encodes");
    DamageResult decoded_damage{};
    check(decode_damage_result(bytes, decoded_damage) ==
              CombatPresentationCodecError::None &&
              decoded_damage.target.generation == 8 &&
              decoded_damage.shot_id == 55 &&
              decoded_damage.impact_event_id == 100 &&
              decoded_damage.post_health == 0.0f &&
              decoded_damage.dead_transition,
          "damage preserves exact generation/correlation");
    DamageResult missing_correlation = damage;
    missing_correlation.impact_event_id = 0;
    check(validate_damage_result(missing_correlation) ==
              CombatPresentationCodecError::None,
          "damage without visual impact correlation is valid");
    std::vector<Byte> zero_reference_bytes;
    check(encode_damage_result(missing_correlation, zero_reference_bytes) ==
              CombatPresentationCodecError::None,
          "zero-reference authoritative damage encodes");
    DamageResult decoded_zero_reference{};
    check(decode_damage_result(zero_reference_bytes, decoded_zero_reference) ==
              CombatPresentationCodecError::None &&
              decoded_zero_reference.impact_event_id == 0,
          "zero-reference authoritative damage round trips");
    DamageResult missing_shot = missing_correlation;
    missing_shot.shot_id = 0;
    check(validate_damage_result(missing_shot) ==
              CombatPresentationCodecError::InvalidCorrelation,
          "damage without shot correlation is rejected");
    DamageResult stale_generation = damage;
    stale_generation.target.generation = 0;
    check(validate_damage_result(stale_generation) ==
              CombatPresentationCodecError::InvalidGeneration,
          "damage without target generation is rejected");

    HornState horn{};
    horn.session_epoch = 9;
    horn.transition_id = 4;
    horn.server_tick = 902;
    horn.vehicle = entity(3, 2);
    horn.active = true;
    horn.horn_cue = cue("audio/horn");
    check(encode_horn_state(horn, bytes) == CombatPresentationCodecError::None,
          "horn cue encodes");
    HornState decoded_horn{};
    check(decode_horn_state(bytes, decoded_horn) ==
              CombatPresentationCodecError::None &&
              decoded_horn.horn_cue.name == "audio/horn",
          "horn canonical cue round trips");

    DeathWreckPresentation expected_death = death();
    check(encode_death_wreck_presentation(expected_death, bytes) ==
              CombatPresentationCodecError::None,
          "death/wreck archive metadata encodes");
    DeathWreckPresentation decoded_death{};
    check(decode_death_wreck_presentation(bytes, decoded_death) ==
              CombatPresentationCodecError::None && decoded_death.terminal &&
              decoded_death.entity.generation == 8 &&
              decoded_death.wreck_entity.net_id != decoded_death.entity.net_id &&
              decoded_death.wreck_cue.name == "wrecks/vehicle/default" &&
              decoded_death.wreck_archive_id == expected_death.wreck_archive_id &&
              decoded_death.wreck_archive_digest ==
                  expected_death.wreck_archive_digest &&
              decoded_death.wreck_archive_chunk_count == 8,
          "death terminal identity and archive metadata round trip");
    std::vector<Byte> death_trailing = bytes;
    death_trailing.push_back(Byte{});
    check(decode_death_wreck_presentation(death_trailing, decoded_death) ==
              CombatPresentationCodecError::InputSizeMismatch,
          "death trailing bytes are rejected");
    DeathWreckPresentation nonterminal = expected_death;
    nonterminal.terminal = false;
    check(validate_death_wreck_presentation(nonterminal) ==
              CombatPresentationCodecError::InvalidTerminal,
          "nonterminal wreck is rejected");
    DeathWreckPresentation no_archive = expected_death;
    no_archive.wreck_archive_id = 0;
    check(validate_death_wreck_presentation(no_archive) ==
              CombatPresentationCodecError::InvalidArchive,
          "missing archive identity is rejected");
    DeathWreckPresentation reused_identity = expected_death;
    reused_identity.wreck_entity = reused_identity.entity;
    check(validate_death_wreck_presentation(reused_identity) ==
              CombatPresentationCodecError::InvalidEntity,
          "live and wreck identities cannot be reused");
}

void test_jip_64_by_8_and_new_terminal_data()
{
    constexpr std::uint16_t chunk_count = 16;
    std::vector<PresentationJipState> chunks;
    chunks.reserve(chunk_count);
    for (std::uint16_t chunk_index = 0; chunk_index != chunk_count;
         ++chunk_index) {
        PresentationJipState chunk{};
        chunk.session_epoch = 9;
        chunk.state_revision = 77;
        chunk.server_tick = 1000;
        chunk.chunk_index = chunk_index;
        chunk.chunk_count = chunk_count;
        for (std::size_t local = 0;
             local != kMaxPresentationJipWeaponStatesPerChunk; ++local) {
            const std::size_t global =
                chunk_index * kMaxPresentationJipWeaponStatesPerChunk + local;
            chunk.weapon_triggers.push_back(trigger(global));
            chunk.weapon_aims.push_back(aim(global));
        }
        if (chunk_index < 2) {
            HornState horn{};
            horn.session_epoch = 9;
            horn.transition_id = 500 + chunk_index;
            horn.server_tick = 1100 + chunk_index;
            horn.vehicle = entity(9000 + chunk_index, 1);
            horn.active = false;
            chunk.horn_states.push_back(horn);
            DeathWreckPresentation wreck = death();
            wreck.transition_id = 700 + chunk_index;
            wreck.entity = entity(9100 + chunk_index, 1);
            wreck.death_cue = {};
            wreck.wreck_cue = {};
            chunk.terminal_deaths.push_back(std::move(wreck));
        }
        std::vector<Byte> bytes;
        check(encode_presentation_jip_state(chunk, bytes) ==
                  CombatPresentationCodecError::None &&
                  bytes.size() <= kMaxPresentationJipChunkWireSize,
              "JIP chunk with cue-bearing terminal data is bounded");
        PresentationJipState decoded{};
        check(decode_presentation_jip_state(bytes, decoded) ==
                  CombatPresentationCodecError::None,
              "JIP chunk decodes");
        chunks.push_back(std::move(decoded));
    }

    PresentationJipReassembler reassembler;
    ReassembledPresentationJipState assembled{};
    for (std::size_t reverse = chunks.size(); reverse != 0; --reverse)
        check(reassembler.accept(chunks[reverse - 1]) ==
                  (reverse == 1 ? PresentationJipAssemblyResult::Complete
                                : PresentationJipAssemblyResult::Accepted),
              "JIP chunks reassemble in arbitrary order");
    check(reassembler.assemble(assembled), "JIP assembly completes");
    check(assembled.weapon_triggers.size() == 64u * 8u &&
              assembled.weapon_aims.size() == 64u * 8u &&
              assembled.horn_states.size() == 2 &&
              assembled.terminal_deaths.size() == 2,
          "JIP proves 64x8 aim/trigger plus horn/death state");
}

void test_wire_envelope_and_dedup()
{
    check(kWireVersion == 4, "generic wire envelope version bumped");
    WireHeader expected{kWireMagic, kWireVersion,
                       MessageType::CombatImpactPresentation,
                       Channel::Reliable, 0, 42, 7};
    std::array<Byte, kWireHeaderSize> bytes{};
    encode_header(expected, MutableByteView{bytes});
    WireHeader decoded{};
    check(decode_header(ByteView{bytes}, decoded) == WireDecodeError::None &&
              decoded.message_type == expected.message_type,
          "wire envelope round trips");
    bytes[4] = static_cast<Byte>(1);
    check(decode_header(ByteView{bytes}, decoded) == WireDecodeError::BadVersion,
          "old generic envelope version is rejected");
    check(requires_reliable_channel(MessageType::CombatHornState) &&
              requires_reliable_channel(MessageType::CombatDeathWreckPresentation) &&
              requires_reliable_channel(MessageType::CombatWreckArchiveChunk) &&
              uses_unreliable_sequenced_channel(MessageType::CombatWeaponAimState),
          "typed combat channel policy is explicit");

    HornTransitionDeduplicator horns;
    HornState horn{};
    horn.session_epoch = 9;
    horn.transition_id = 1;
    horn.vehicle = entity(5, 2);
    check(horns.accept(horn) && !horns.accept(horn),
          "horn transitions deduplicate by generation");
    horn.transition_id = 2;
    check(horns.accept(horn) && horns.contains(9, horn.vehicle, 2),
          "later horn transition is accepted");
    DeathWreckDeduplicator deaths;
    DeathWreckPresentation wreck = death();
    check(deaths.accept(wreck) && !deaths.accept(wreck),
          "terminal death deduplicates by exact generation");
}

} // namespace

int main()
{
    test_resource_cues_and_policies();
    test_impact_and_versioned_wire();
    test_shot_damage_horn_death();
    test_jip_64_by_8_and_new_terminal_data();
    test_wire_envelope_and_dedup();
    if (failures != 0) {
        std::cerr << failures << " combat presentation test(s) failed\n";
        return 1;
    }
    std::cout << "combat presentation tests passed\n";
    return 0;
}
