#ifndef KRAKEN_NET_COMBAT_PRESENTATION_HPP
#define KRAKEN_NET_COMBAT_PRESENTATION_HPP

#include "net/entity_registry.hpp"
#include "net/net_types.hpp"
#include "net/vehicle_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace kraken::net {

// Combat presentation has its own versioned, fixed-width little-endian packet
// header. Resource names are the only cue identity that crosses this boundary;
// engine-local numeric handles are deliberately absent from the schema.
inline constexpr std::uint32_t kCombatPresentationWireMagic =
    0x31504343u; // CCP1
inline constexpr std::uint16_t kCombatPresentationWireVersion = 6;
inline constexpr std::uint16_t kCombatPresentationPreviousWireVersion = 5;
inline constexpr std::size_t kCombatPresentationHeaderSize = 24;
inline constexpr std::size_t kMaxCombatPresentationPayload = 64u * 1024u;
inline constexpr std::size_t kMaxCombatPresentationStringBytes = 64;
inline constexpr std::size_t kMaxResourceCueNameBytes = 96;
inline constexpr std::size_t kResourceCueWirePrefixSize = 2 + 8;
inline constexpr std::size_t kMaxResourceCueWireSize =
    kResourceCueWirePrefixSize + kMaxResourceCueNameBytes;

inline constexpr std::size_t kMaxPresentationJipChunks = 64;
inline constexpr std::size_t kMaxPresentationJipWeaponStatesPerChunk = 32;
inline constexpr std::size_t kMaxPresentationJipAimStatesPerChunk = 32;
inline constexpr std::size_t kMaxPresentationJipHornStatesPerChunk = 32;
inline constexpr std::size_t kMaxPresentationJipDeathStatesPerChunk = 8;
inline constexpr std::size_t kMaxPresentationJipWeaponStates =
    kMaxPresentationJipChunks * kMaxPresentationJipWeaponStatesPerChunk;
inline constexpr std::size_t kMaxPresentationJipAimStates =
    kMaxPresentationJipChunks * kMaxPresentationJipAimStatesPerChunk;
inline constexpr std::size_t kMaxPresentationJipHornStates =
    kMaxPresentationJipChunks * kMaxPresentationJipHornStatesPerChunk;
inline constexpr std::size_t kMaxPresentationJipDeathStates =
    kMaxPresentationJipChunks * kMaxPresentationJipDeathStatesPerChunk;
inline constexpr std::size_t kMaxDeathWreckBrokenParts = 32;
inline constexpr std::size_t kMaxDeathWreckArchiveBytes = 256u * 1024u;
inline constexpr std::size_t kMaxDeathWreckArchiveChunks = 256;
inline constexpr std::size_t kMaxDeathWreckArchiveChunkBytes = 1024;
inline constexpr std::uint32_t kMaxPresentationIdentifier = 0x00ffffffu;
inline constexpr std::uint32_t kMaxCombatAmmoCount = 1'000'000u;
inline constexpr float kMaxCombatCoordinate = 1'000'000.0f;
inline constexpr float kMaxCombatHealth = 1'000'000.0f;
inline constexpr float kMaxCombatAimSpeed = 100.0f;
inline constexpr float kMaxCombatEffectScale = 100.0f;

inline constexpr std::size_t kWeaponTriggerStateWireSize =
    kCombatPresentationHeaderSize + 40;
inline constexpr std::size_t kWeaponAimStateWireSize =
    kCombatPresentationHeaderSize + 64;
inline constexpr std::size_t kShotConfirmedWirePrefixSize =
    kCombatPresentationHeaderSize + 72;
inline constexpr std::size_t kShotConfirmedWireSize =
    kShotConfirmedWirePrefixSize + 4 * kResourceCueWirePrefixSize;
inline constexpr std::size_t kMaxShotConfirmedWireSize =
    kShotConfirmedWirePrefixSize + 4 * kMaxResourceCueWireSize;
inline constexpr std::size_t kImpactPresentationWirePrefixSize =
    kCombatPresentationHeaderSize + 176;
inline constexpr std::size_t kImpactPresentationWireSize =
    kImpactPresentationWirePrefixSize + 2 * kResourceCueWirePrefixSize;
inline constexpr std::size_t kMaxImpactPresentationWireSize =
    kImpactPresentationWirePrefixSize + 2 * kMaxResourceCueWireSize;
inline constexpr std::size_t kDamageResultWirePrefixSize =
    kCombatPresentationHeaderSize + 48;
inline constexpr std::size_t kMaxDamageResultWireSize =
    kDamageResultWirePrefixSize + kMaxCombatPresentationStringBytes;
inline constexpr std::size_t kDeathWreckPresentationWirePrefixSize =
    kCombatPresentationHeaderSize + 60 + 2 * kResourceCueWirePrefixSize;
inline constexpr std::size_t kMaxDeathWreckPresentationWireSize =
    kCombatPresentationHeaderSize + 60 + 2 * kMaxResourceCueWireSize +
    kMaxDeathWreckBrokenParts * 16;
inline constexpr std::size_t kHornStateWirePrefixSize =
    kCombatPresentationHeaderSize + 12;
inline constexpr std::size_t kHornStateWireSize =
    kHornStateWirePrefixSize + kResourceCueWirePrefixSize;
inline constexpr std::size_t kMaxHornStateWireSize =
    kHornStateWirePrefixSize + kMaxResourceCueWireSize;
inline constexpr std::size_t kMaxPresentationJipChunkWireSize =
    kCombatPresentationHeaderSize + 12 +
    kMaxPresentationJipWeaponStatesPerChunk * (12 + 40) +
    kMaxPresentationJipAimStatesPerChunk * (12 + 64) +
    kMaxPresentationJipHornStatesPerChunk *
        (12 + 12 + kMaxResourceCueWireSize) +
    kMaxPresentationJipDeathStatesPerChunk *
        (12 + 60 + 2 * kMaxResourceCueWireSize +
         kMaxDeathWreckBrokenParts * 16);
inline constexpr std::size_t kMaxPresentationJipStateWireSize =
    kMaxPresentationJipChunkWireSize;
inline constexpr std::size_t kMaxCombatPresentationWireSize =
    kMaxCombatPresentationPayload;
inline constexpr std::size_t kCombatPresentationMaxPayload =
    kMaxCombatPresentationPayload;

using CombatEventId = std::uint64_t;
using CombatTransitionId = std::uint64_t;
using AimUpdateSequence = std::uint64_t;
using PresentationStateRevision = std::uint64_t;
using StablePathHash = std::uint64_t;
using MeshIdentity = std::uint64_t;

enum class CombatPresentationPacketKind : std::uint8_t {
    WeaponTriggerState = 1,
    ShotConfirmed = 2,
    ImpactPresentation = 3,
    DamageResult = 4,
    DeathWreckPresentation = 5,
    HornState = 6,
    PresentationJipState = 7,
    WeaponAimState = 8,
};

[[nodiscard]] constexpr bool is_valid_combat_presentation_kind(
    const CombatPresentationPacketKind kind) noexcept
{
    switch (kind) {
    case CombatPresentationPacketKind::WeaponTriggerState:
    case CombatPresentationPacketKind::ShotConfirmed:
    case CombatPresentationPacketKind::ImpactPresentation:
    case CombatPresentationPacketKind::DamageResult:
    case CombatPresentationPacketKind::DeathWreckPresentation:
    case CombatPresentationPacketKind::HornState:
    case CombatPresentationPacketKind::PresentationJipState:
    case CombatPresentationPacketKind::WeaponAimState:
        return true;
    }
    return false;
}

struct ResourceCue {
    std::string name{};
    std::uint64_t hash = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return name.empty() && hash == 0;
    }
};

inline constexpr std::uint64_t kResourceCueFnv1aOffset =
    14695981039346656037ull;
inline constexpr std::uint64_t kResourceCueFnv1aPrime = 1099511628211ull;

[[nodiscard]] std::uint64_t resource_cue_hash(std::string_view canonical_name) noexcept;
[[nodiscard]] std::string normalize_resource_name(std::string_view name);
[[nodiscard]] ResourceCue make_resource_cue(std::string_view name);
[[nodiscard]] bool try_make_resource_cue(std::string_view name,
                                         ResourceCue& output) noexcept;

struct NetEntityRef {
    NetId net_id = kInvalidNetId;
    EntityGeneration generation = kInvalidEntityGeneration;
};

using EntityIdentity = NetEntityRef;
using CombatEntityIdentity = NetEntityRef;

struct AttachmentIdentity {
    std::uint64_t attachment_id = 0;
    StablePathHash path_hash = 0;
};

using GunAttachmentIdentity = AttachmentIdentity;
using BrokenPartIdentity = AttachmentIdentity;
using WreckVariantId = std::uint64_t;

struct CombatPose {
    VehicleVector3 position{};
    VehicleQuaternion rotation{};
};

using MuzzlePose = CombatPose;
using HitPose = CombatPose;

struct ShotPresentationCues {
    ResourceCue muzzle_cue{};
    ResourceCue projectile_cue{};
    ResourceCue shot_cue{};
    ResourceCue reload_cue{};
};

// Compatibility name only: these are canonical cues, never local numeric IDs.
using ShotPresentationIds = ShotPresentationCues;

enum class AmmoReloadState : std::uint8_t {
    Ready = 0,
    Reloading = 1,
    CoolingDown = 2,
};

using ReloadState = AmmoReloadState;

[[nodiscard]] constexpr bool is_valid_ammo_reload_state(
    const AmmoReloadState state) noexcept
{
    return state == AmmoReloadState::Ready ||
           state == AmmoReloadState::Reloading ||
           state == AmmoReloadState::CoolingDown;
}

enum class ImpactTargetKind : std::uint8_t {
    DynamicEntity = 0,
    StableStatic = 1,
    Environment = 2,
};

enum class EnvironmentKind : std::uint8_t {
    Terrain = 0,
    Road = 1,
    Statics = 2,
    Water = 3,
    UnboundStatic = 4,
};

[[nodiscard]] constexpr bool is_valid_impact_target_kind(
    const ImpactTargetKind kind) noexcept
{
    return kind == ImpactTargetKind::DynamicEntity ||
           kind == ImpactTargetKind::StableStatic ||
           kind == ImpactTargetKind::Environment;
}

[[nodiscard]] constexpr bool is_valid_environment_kind(
    const EnvironmentKind kind) noexcept
{
    switch (kind) {
    case EnvironmentKind::Terrain:
    case EnvironmentKind::Road:
    case EnvironmentKind::Statics:
    case EnvironmentKind::Water:
    case EnvironmentKind::UnboundStatic:
        return true;
    }
    return false;
}

struct StableStaticTargetIdentity {
    std::uint64_t stable_id = 0;
    StablePathHash path_hash = 0;
};

struct ImpactTargetIdentity {
    ImpactTargetKind kind = ImpactTargetKind::DynamicEntity;
    NetEntityRef dynamic{};
    StableStaticTargetIdentity stable{};
    EnvironmentKind environment_kind = EnvironmentKind::UnboundStatic;
};

enum class SurfaceKind : std::uint8_t {
    Unknown = 0,
    Metal = 1,
    Concrete = 2,
    Dirt = 3,
    Wood = 4,
    Flesh = 5,
    Glass = 6,
    Water = 7,
    Stone = 8,
    Energy = 9,
    Other = 10,
};

[[nodiscard]] constexpr bool is_valid_surface_kind(
    const SurfaceKind kind) noexcept
{
    switch (kind) {
    case SurfaceKind::Unknown:
    case SurfaceKind::Metal:
    case SurfaceKind::Concrete:
    case SurfaceKind::Dirt:
    case SurfaceKind::Wood:
    case SurfaceKind::Flesh:
    case SurfaceKind::Glass:
    case SurfaceKind::Water:
    case SurfaceKind::Stone:
    case SurfaceKind::Energy:
    case SurfaceKind::Other:
        return true;
    }
    return false;
}

enum class ImpactBlockedReason : std::uint8_t {
    None = 0,
    NoTarget = 1,
    OutOfRange = 2,
    Occluded = 3,
    FriendlyFire = 4,
    Armor = 5,
    InvalidTarget = 6,
    Unknown = 7,
};

using BlockedReason = ImpactBlockedReason;

[[nodiscard]] constexpr bool is_valid_impact_blocked_reason(
    const ImpactBlockedReason reason) noexcept
{
    switch (reason) {
    case ImpactBlockedReason::None:
    case ImpactBlockedReason::NoTarget:
    case ImpactBlockedReason::OutOfRange:
    case ImpactBlockedReason::Occluded:
    case ImpactBlockedReason::FriendlyFire:
    case ImpactBlockedReason::Armor:
    case ImpactBlockedReason::InvalidTarget:
    case ImpactBlockedReason::Unknown:
        return true;
    }
    return false;
}

enum class DeathWreckReason : std::uint8_t {
    Combat = 0,
    Despawn = 1,
    Abandoned = 2,
    Scripted = 3,
    Unknown = 4,
};

[[nodiscard]] constexpr bool is_valid_death_wreck_reason(
    const DeathWreckReason reason) noexcept
{
    switch (reason) {
    case DeathWreckReason::Combat:
    case DeathWreckReason::Despawn:
    case DeathWreckReason::Abandoned:
    case DeathWreckReason::Scripted:
    case DeathWreckReason::Unknown:
        return true;
    }
    return false;
}

struct WeaponTriggerState {
    std::uint32_t session_epoch = 0;
    CombatTransitionId transition_id = 0;
    std::uint32_t server_tick = 0;
    NetEntityRef shooter{};
    GunAttachmentIdentity gun{};
    bool trigger_held = false;
    bool reloading = false;
    std::uint32_t shells_in_current_charge = 0;
    std::uint32_t shells_in_pool = 0;
    float reload_fraction = 0.0f;
};

struct WeaponAimState {
    std::uint32_t session_epoch = 0;
    AimUpdateSequence update_sequence = 0;
    std::uint32_t server_tick = 0;
    NetEntityRef shooter{};
    GunAttachmentIdentity gun{};
    bool has_target = false;
    NetEntityRef target{};
    VehicleVector3 aim_point{};
    VehicleVector3 aim_direction{0.0f, 0.0f, 1.0f};
    float aim_speed = 0.0f;
};

struct ShotConfirmed {
    std::uint32_t session_epoch = 0;
    CombatEventId shot_id = 0;
    std::uint32_t server_tick = 0;
    NetEntityRef shooter{};
    GunAttachmentIdentity gun{};
    std::uint32_t burst_id = 0;
    std::uint16_t burst_index = 0;
    std::uint16_t burst_size = 1;
    CombatPose muzzle_pose{};
    std::uint32_t shells_in_current_charge = 0;
    std::uint32_t shells_in_pool = 0;
    AmmoReloadState reload_state = AmmoReloadState::Ready;
    ShotPresentationCues presentation{};
};

struct ImpactPresentation {
    std::uint32_t session_epoch = 0;
    CombatEventId event_id = 0;
    std::uint32_t server_tick = 0;
    CombatEventId shot_id = 0;
    NetEntityRef shooter{};
    GunAttachmentIdentity gun{};
    ImpactTargetIdentity target{};
    AttachmentIdentity target_part{};
    SurfaceKind surface = SurfaceKind::Unknown;
    ResourceCue effect_cue{};
    ResourceCue decal_cue{};
    VehicleVector3 hit_position{};
    VehicleVector3 effect_position{};
    VehicleVector3 incoming_direction{};
    VehicleVector3 contact_normal{0.0f, 1.0f, 0.0f};
    VehicleVector3 decal_tangent{};
    bool has_incoming_direction = false;
    bool has_decal_tangent = false;
    MeshIdentity mesh_id = 0;
    std::uint32_t material_id = 0; // diagnostic only; never a cue identity
    VehicleQuaternion effect_rotation{};
    float effect_scale = 1.0f;
    bool remove_if_free = false;
    bool did_damage = false;
    ImpactBlockedReason blocked_reason = ImpactBlockedReason::Unknown;
};

struct DamageResult {
    std::uint32_t session_epoch = 0;
    CombatEventId event_id = 0;
    std::uint32_t server_tick = 0;
    CombatEventId shot_id = 0;
    CombatEventId impact_event_id = 0;
    NetEntityRef shooter{};
    NetEntityRef target{};
    float damage = 0.0f;
    float post_health = 0.0f;
    std::string damaged_part{};
    bool dead_transition = false;
};

struct DeathWreckPresentation {
    std::uint32_t session_epoch = 0;
    CombatTransitionId transition_id = 0;
    std::uint32_t server_tick = 0;
    // entity is the destroyed live vehicle. wreck_entity is a distinct
    // authoritative identity allocated for the inert replacement.
    NetEntityRef entity{};
    NetEntityRef wreck_entity{};
    DeathWreckReason reason = DeathWreckReason::Combat;
    ResourceCue death_cue{};
    ResourceCue wreck_cue{};
    std::uint64_t wreck_archive_id = 0;
    std::uint32_t wreck_archive_revision = 0;
    std::uint64_t wreck_archive_digest = 0;
    std::uint32_t wreck_archive_size = 0;
    std::uint16_t wreck_archive_chunk_count = 0;
    std::uint16_t wreck_archive_chunk_size = 0;
    WreckVariantId wreck_variant_id = 0;
    std::vector<BrokenPartIdentity> broken_parts;
    bool terminal = true;
};

struct HornState {
    std::uint32_t session_epoch = 0;
    CombatTransitionId transition_id = 0;
    std::uint32_t server_tick = 0;
    NetEntityRef vehicle{};
    bool active = false;
    ResourceCue horn_cue{};
};

struct PresentationJipState {
    std::uint32_t session_epoch = 0;
    PresentationStateRevision state_revision = 0;
    std::uint32_t server_tick = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 1;
    std::vector<WeaponTriggerState> weapon_triggers;
    std::vector<WeaponAimState> weapon_aims;
    std::vector<HornState> horn_states;
    std::vector<DeathWreckPresentation> terminal_deaths;
};

struct ReassembledPresentationJipState {
    std::uint32_t session_epoch = 0;
    PresentationStateRevision state_revision = 0;
    std::uint32_t server_tick = 0;
    std::vector<WeaponTriggerState> weapon_triggers;
    std::vector<WeaponAimState> weapon_aims;
    std::vector<HornState> horn_states;
    std::vector<DeathWreckPresentation> terminal_deaths;
};

enum class CombatPresentationCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadKind,
    BadFlags,
    InvalidEpoch,
    InvalidEventId,
    InvalidTransitionId,
    InvalidUpdateSequence,
    InvalidStateRevision,
    InvalidEntity,
    InvalidGeneration,
    InvalidAttachment,
    InvalidEnum,
    InvalidIdentifier,
    InvalidTarget,
    InvalidShotReference,
    InvalidBurst,
    NonFiniteValue,
    ValueOutOfBounds,
    InvalidQuaternion,
    InvalidAimDirection,
    InvalidString,
    StringTooLong,
    InvalidUtf8,
    InvalidPath,
    InvalidCue,
    CueTooLong,
    InvalidCount,
    InvalidChunk,
    DuplicateIdentity,
    PayloadTooLarge,
    InvalidWreckVariant,
    InvalidArchive,
    InvalidCorrelation,
    InvalidTerminal,
    AllocationFailure,
};

[[nodiscard]] constexpr bool combat_presentation_codec_succeeded(
    const CombatPresentationCodecError error) noexcept
{
    return error == CombatPresentationCodecError::None;
}

[[nodiscard]] CombatPresentationCodecError validate_resource_cue(
    const ResourceCue&, bool allow_empty = true) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_weapon_trigger_state(
    const WeaponTriggerState&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_weapon_aim_state(
    const WeaponAimState&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_shot_confirmed(
    const ShotConfirmed&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_impact_presentation(
    const ImpactPresentation&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_damage_result(
    const DamageResult&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_death_wreck_presentation(
    const DeathWreckPresentation&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_horn_state(
    const HornState&) noexcept;
[[nodiscard]] CombatPresentationCodecError validate_presentation_jip_state(
    const PresentationJipState&) noexcept;

#define KRAKEN_DECLARE_COMBAT_CODEC(Name, Type)                                  \
    [[nodiscard]] CombatPresentationCodecError encode_##Name(                    \
        const Type&, std::vector<Byte>&);                                        \
    [[nodiscard]] CombatPresentationCodecError encode_##Name(                    \
        const Type&, MutableByteView) noexcept;                                  \
    [[nodiscard]] CombatPresentationCodecError decode_##Name(                    \
        ByteView, Type&) noexcept

KRAKEN_DECLARE_COMBAT_CODEC(weapon_trigger_state, WeaponTriggerState);
KRAKEN_DECLARE_COMBAT_CODEC(weapon_aim_state, WeaponAimState);
KRAKEN_DECLARE_COMBAT_CODEC(shot_confirmed, ShotConfirmed);
KRAKEN_DECLARE_COMBAT_CODEC(impact_presentation, ImpactPresentation);
KRAKEN_DECLARE_COMBAT_CODEC(damage_result, DamageResult);
KRAKEN_DECLARE_COMBAT_CODEC(death_wreck_presentation, DeathWreckPresentation);
KRAKEN_DECLARE_COMBAT_CODEC(horn_state, HornState);
KRAKEN_DECLARE_COMBAT_CODEC(presentation_jip_state, PresentationJipState);

#undef KRAKEN_DECLARE_COMBAT_CODEC

using CombatPresentationState = PresentationJipState;
using JipPresentationState = PresentationJipState;
using CombatPresentationWireError = CombatPresentationCodecError;

[[nodiscard]] inline CombatPresentationCodecError encode_jip_presentation_state(
    const PresentationJipState& value, std::vector<Byte>& output)
{
    return encode_presentation_jip_state(value, output);
}

[[nodiscard]] inline CombatPresentationCodecError decode_jip_presentation_state(
    ByteView input, PresentationJipState& output) noexcept
{
    return decode_presentation_jip_state(input, output);
}

[[nodiscard]] constexpr bool combat_event_id_is_newer(
    const CombatEventId previous, const CombatEventId candidate) noexcept
{
    const CombatEventId delta = candidate - previous;
    return candidate != previous && delta < 0x8000000000000000ull;
}

class CombatEventDeduplicator final {
public:
    explicit CombatEventDeduplicator(std::size_t capacity = 4096);
    [[nodiscard]] bool accept(std::uint32_t session_epoch,
                              CombatEventId event_id);
    [[nodiscard]] bool contains(std::uint32_t session_epoch,
                                CombatEventId event_id) const noexcept;
    void clear() noexcept;

private:
    struct Key {
        std::uint32_t session_epoch = 0;
        CombatEventId event_id = 0;
    };
    std::deque<Key> m_recent;
    std::size_t m_capacity = 0;
    std::uint32_t m_epoch = 0;
    CombatEventId m_high_water = 0;
    bool m_have_high_water = false;
};

struct WeaponAimInterpolationInput {
    WeaponAimState previous{};
    WeaponAimState current{};
};

class WeaponAimStateTracker final {
public:
    explicit WeaponAimStateTracker(
        std::size_t capacity = kMaxPresentationJipAimStates);
    [[nodiscard]] bool accept(const WeaponAimState&);
    [[nodiscard]] bool interpolation_input(
        std::uint32_t session_epoch, const NetEntityRef& shooter,
        const GunAttachmentIdentity& gun,
        WeaponAimInterpolationInput& output) const noexcept;
    void clear() noexcept;

private:
    struct Entry {
        std::uint32_t session_epoch = 0;
        NetEntityRef shooter{};
        GunAttachmentIdentity gun{};
        WeaponAimState previous{};
        WeaponAimState current{};
        bool have_previous = false;
    };
    std::vector<Entry> m_latest;
    std::size_t m_capacity = 0;
    std::uint32_t m_epoch = 0;
    bool m_have_epoch = false;
};

class DeathWreckDeduplicator final {
public:
    explicit DeathWreckDeduplicator(
        std::size_t capacity = kMaxPresentationJipDeathStates);
    [[nodiscard]] bool accept(const DeathWreckPresentation&);
    [[nodiscard]] bool contains(std::uint32_t session_epoch,
                                const NetEntityRef&) const noexcept;
    void clear() noexcept;

private:
    struct Entry {
        std::uint32_t session_epoch = 0;
        NetEntityRef entity{};
    };
    std::vector<Entry> m_terminal;
    std::size_t m_capacity = 0;
    std::uint32_t m_epoch = 0;
    bool m_have_epoch = false;
};

class HornTransitionDeduplicator final {
public:
    explicit HornTransitionDeduplicator(
        std::size_t capacity = kMaxPresentationJipHornStates);
    [[nodiscard]] bool accept(const HornState&);
    [[nodiscard]] bool contains(std::uint32_t session_epoch,
                                const NetEntityRef&,
                                CombatTransitionId) const noexcept;
    void clear() noexcept;

private:
    struct Entry {
        std::uint32_t session_epoch = 0;
        NetEntityRef vehicle{};
        CombatTransitionId transition_id = 0;
    };
    std::vector<Entry> m_latest;
    std::size_t m_capacity = 0;
    std::uint32_t m_epoch = 0;
    bool m_have_epoch = false;
};

enum class PresentationJipAssemblyResult : std::uint8_t {
    Accepted,
    Complete,
    Duplicate,
    Stale,
    Inconsistent,
    DuplicateIdentity,
    InvalidChunk,
    AllocationFailure,
};

class PresentationJipReassembler final {
public:
    [[nodiscard]] PresentationJipAssemblyResult accept(
        const PresentationJipState& chunk);
    [[nodiscard]] bool assemble(
        ReassembledPresentationJipState& output) const;
    void clear() noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::size_t received_chunk_count() const noexcept;
    [[nodiscard]] std::uint32_t session_epoch() const noexcept;
    [[nodiscard]] PresentationStateRevision state_revision() const noexcept;

private:
    struct StoredChunk {
        bool present = false;
        PresentationJipState value;
        std::vector<Byte> canonical_wire;
    };
    [[nodiscard]] bool duplicates_existing_identity(
        const PresentationJipState& chunk) const noexcept;
    std::vector<StoredChunk> m_chunks;
    std::uint32_t m_epoch = 0;
    PresentationStateRevision m_revision = 0;
    std::uint32_t m_server_tick = 0;
    std::size_t m_received = 0;
};

using PresentationJipAssembler = PresentationJipReassembler;
using PresentationJipReassemblyResult = PresentationJipAssemblyResult;

static_assert(kMaxPresentationJipChunkWireSize <=
              kMaxCombatPresentationPayload);
static_assert(kMaxPresentationJipChunks <= 0xffffu);
static_assert(kMaxPresentationJipAimStatesPerChunk <= 0xffffu);
static_assert(kMaxDeathWreckBrokenParts <= 0xffffu);

} // namespace kraken::net

#endif // KRAKEN_NET_COMBAT_PRESENTATION_HPP
