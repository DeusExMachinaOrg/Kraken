#ifndef KRAKEN_NET_VEHICLE_DESCRIPTOR_HPP
#define KRAKEN_NET_VEHICLE_DESCRIPTOR_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kraken::net {

// Vehicle descriptors are data-only snapshots.  Engine object pointers and
// native ABI layouts deliberately do not cross this interface.
using VehicleInstanceId = std::uint32_t;
using VehiclePrototypeId = std::int32_t;

inline constexpr std::uint32_t kVehicleDescriptorWireMagic = 0x31445656u; // VVD
inline constexpr std::uint16_t kVehicleDescriptorLegacyWireVersion = 1;
inline constexpr std::uint16_t kVehicleDescriptorPreviousWireVersion = 2;
inline constexpr std::uint16_t kVehicleDescriptorWireVersion = 3;
inline constexpr std::uint16_t kVehicleDescriptorWireFlags = 0;

// Bounds are part of the protocol contract.  In particular, they are checked
// before allocating from untrusted wire counts or lengths.
inline constexpr std::size_t kMaxVehicleDescriptorNodes = 256;
inline constexpr std::size_t kMaxVehicleDescriptorAttachments =
    kMaxVehicleDescriptorNodes;
inline constexpr std::size_t kMaxVehicleDescriptorParts =
    kMaxVehicleDescriptorNodes;
inline constexpr std::size_t kMaxVehicleDescriptorAffixes = 64;
inline constexpr std::size_t kMaxVehicleDescriptorAffixesPerNode =
    kMaxVehicleDescriptorAffixes;
inline constexpr std::size_t kMaxVehicleDescriptorNameLength = 127;
inline constexpr std::size_t kMaxVehicleDescriptorSlotLength = 63;
inline constexpr std::size_t kMaxVehicleDescriptorAffixNameLength = 127;
inline constexpr std::size_t kMaxVehicleDescriptorModifiers = 128;
inline constexpr std::size_t kMaxVehicleModifierValuePayloadSize = 4096;
inline constexpr std::size_t kMaxVehicleDescriptorCargoStacks = 256;
inline constexpr std::size_t kMaxVehicleDescriptorCargoObjects = 256;
inline constexpr std::size_t kMaxVehicleDescriptorCargoDepth = 16;
inline constexpr std::int32_t kMaxVehicleDescriptorCargoCoordinate = 4095;
inline constexpr std::size_t kMaxVehicleDescriptorBlobSize = 64u * 1024u;
inline constexpr std::size_t kMaxVehicleDescriptorWireSize = 4u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxVehicleDescriptorSkin = 1'000'000u;
inline constexpr std::uint32_t kMaxVehicleDescriptorCounter = 1'000'000'000u;
inline constexpr float kMaxVehicleDescriptorScalar = 1'000'000'000.0f;

struct VehicleAffix {
    std::int32_t id = 0;
    std::string name;
    std::int32_t target_resource_id = -1;
    std::string target_resource_name;

    friend bool operator==(const VehicleAffix&, const VehicleAffix&) = default;
};

// Values match hta::ai::eModifierOperation without exposing the native ABI.
enum class VehicleModifierOperation : std::uint8_t {
    Set = 0,
    Back = 1,
    Add = 2,
    Subtract = 3,
    Multiply = 4,
    Divide = 5,
};

[[nodiscard]] constexpr bool is_valid_vehicle_modifier_operation(
    VehicleModifierOperation operation) noexcept
{
    return operation >= VehicleModifierOperation::Set &&
           operation <= VehicleModifierOperation::Divide;
}

// Values match hta::m3d::eAIParamType.  The payload is a bounded canonical
// byte representation supplied by the native adapter; fixed-width kinds are
// checked for their exact expected size by this codec.
enum class VehicleModifierValueType : std::uint8_t {
    Undefined = 0,
    Vector = 1,
    Quaternion = 2,
    Id = 3,
    Float = 4,
    String = 5,
    IdList = 6,
    StringList = 7,
    Range = 8,

    Vector3 = Vector,
    ObjectId = Id,
};

[[nodiscard]] constexpr bool is_valid_vehicle_modifier_value_type(
    VehicleModifierValueType type) noexcept
{
    return type >= VehicleModifierValueType::Undefined &&
           type <= VehicleModifierValueType::Range;
}

struct VehicleModifier {
    float timeout = 0.0f;
    VehicleModifierOperation operation = VehicleModifierOperation::Set;
    std::int32_t magic_prototype_id = -1;
    std::string property_name;
    std::int32_t sender_id = -1;
    VehicleModifierValueType value_type = VehicleModifierValueType::Undefined;
    std::vector<Byte> value_payload;

    friend bool operator==(const VehicleModifier&,
                           const VehicleModifier&) = default;
};

enum class VehicleGunChargeState : std::uint8_t {
    Ready = 0,
    Charging = 1,
};

[[nodiscard]] constexpr bool is_valid_vehicle_gun_charge_state(
    VehicleGunChargeState state) noexcept
{
    return state == VehicleGunChargeState::Ready ||
           state == VehicleGunChargeState::Charging;
}

struct VehicleGunState {
    bool present = false;
    std::uint32_t barrel_index = 0;
    float rotation = 0.0f;
    VehicleGunChargeState charge_state = VehicleGunChargeState::Ready;
    float reload = 0.0f;
    std::uint32_t current_charge = 0;
    std::uint32_t pool = 0;
    bool firing = false;

    friend bool operator==(const VehicleGunState&,
                           const VehicleGunState&) = default;
};

enum class VehicleCargoRepository : std::uint8_t {
    Main = 0,
    Ground = 1,
};

[[nodiscard]] constexpr bool is_valid_vehicle_cargo_repository(
    VehicleCargoRepository repository) noexcept
{
    return repository == VehicleCargoRepository::Main ||
           repository == VehicleCargoRepository::Ground;
}

// Coordinates are the native save's preferred GeomRepository places, not a
// flattened resource identity.  The native loader may deterministically
// repack stale or overlapping positions; repository identity and exact cargo
// content remain authoritative.
struct VehicleCargoPlacement {
    VehicleCargoRepository repository = VehicleCargoRepository::Main;
    std::int32_t x = 0;
    std::int32_t y = 0;

    friend bool operator==(const VehicleCargoPlacement&,
                           const VehicleCargoPlacement&) = default;
};

struct VehicleCargoStack {
    std::int32_t resource_id = -1;
    std::string resource_name;
    std::uint32_t amount = 0;
    VehicleCargoPlacement placement{};

    friend bool operator==(const VehicleCargoStack&,
                           const VehicleCargoStack&) = default;
};

enum class VehicleDescriptorNodeKind : std::uint8_t {
    Attachment = 1,
    Container = 2,
};

using VehicleAttachmentKind = VehicleDescriptorNodeKind;

[[nodiscard]] constexpr bool is_valid_vehicle_descriptor_node_kind(
    VehicleDescriptorNodeKind kind) noexcept
{
    return kind == VehicleDescriptorNodeKind::Attachment ||
           kind == VehicleDescriptorNodeKind::Container;
}

// A node is linked to the vehicle root when parent_instance_id is zero.  All
// other parent IDs refer to another node in the same descriptor.  The flat
// representation keeps stable IDs explicit while still describing arbitrary
// recursive attachment/container graphs.
struct VehicleDescriptorNode {
    VehicleDescriptorNodeKind kind = VehicleDescriptorNodeKind::Attachment;
    VehicleInstanceId instance_id = 0;
    VehicleInstanceId parent_instance_id = 0;
    std::string slot;

    VehiclePrototypeId prototype_id = -1;
    std::string prototype_name;
    std::uint32_t skin = 0;

    std::vector<VehicleAffix> prefixes;
    std::vector<VehicleAffix> suffixes;
    std::vector<VehicleModifier> modifiers;

    // These blobs are opaque to the network layer and are copied byte-for-
    // byte.  They are not interpreted, aligned, or ABI-cast.
    std::vector<Byte> native_structure;
    std::vector<Byte> runtime;

    float health = 0.0f;
    float durability = 0.0f;
    float fuel = 0.0f;
    std::uint32_t ammo = 0;
    std::uint32_t magazine = 0;
    float reload = 0.0f;
    VehicleGunState gun;

    // Repository resource entries and object entries are distinct in HTA.
    // Object cargo recursively carries the same portable descriptor fields.
    std::vector<VehicleCargoStack> cargo_stacks;
    std::vector<VehicleDescriptorNode> cargo_objects;
    VehicleCargoPlacement cargo_placement{};

    friend bool operator==(const VehicleDescriptorNode&,
                           const VehicleDescriptorNode&) = default;
};

using VehicleAttachment = VehicleDescriptorNode;
using VehiclePartDescriptor = VehicleDescriptorNode;
using VehicleNode = VehicleDescriptorNode;

struct VehicleDescriptor {
    VehiclePrototypeId prototype_id = -1;
    std::string prototype_name;
    std::uint32_t skin = 0;

    std::vector<VehicleAffix> prefixes;
    std::vector<VehicleAffix> suffixes;
    std::vector<VehicleModifier> modifiers;
    std::vector<Byte> native_structure;
    std::vector<Byte> runtime;

    float health = 0.0f;
    float durability = 0.0f;
    float fuel = 0.0f;
    std::uint32_t ammo = 0;
    std::uint32_t magazine = 0;
    float reload = 0.0f;

    std::vector<VehicleCargoStack> cargo_stacks;
    std::vector<VehicleDescriptorNode> cargo_objects;

    // Ordered here for source ergonomics, but canonical wire encoding orders
    // records by instance_id.  Affix vectors remain ordered by contract.
    std::vector<VehicleDescriptorNode> attachments;

    [[nodiscard]] bool semantic_equal(const VehicleDescriptor&) const noexcept;
    [[nodiscard]] std::uint64_t digest() const noexcept;
};

enum class VehicleDescriptorCodecError : std::uint8_t {
    None,
    OutputTooSmall,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    PayloadTooLarge,
    AllocationFailure,
    InvalidPrototype,
    InvalidName,
    InvalidSlot,
    InvalidSkin,
    InvalidNodeKind,
    InvalidInstanceId,
    DuplicateInstanceId,
    UnknownParent,
    CycleDetected,
    DuplicateSlot,
    TooManyNodes,
    TooManyAffixes,
    InvalidAffixId,
    InvalidAffixName,
    InvalidBlob,
    InvalidHealth,
    InvalidDurability,
    InvalidFuel,
    InvalidAmmo,
    InvalidMagazine,
    InvalidReload,
    TooManyModifiers,
    InvalidModifierTimeout,
    InvalidModifierOperation,
    InvalidMagicPrototype,
    InvalidModifierProperty,
    InvalidModifierSender,
    InvalidModifierValueType,
    InvalidModifierValue,
    InvalidAffixTarget,
    TooManyCargoStacks,
    TooManyCargoObjects,
    InvalidCargoResource,
    InvalidCargoAmount,
    InvalidCargoRepository,
    InvalidCargoPlacement,
    DuplicateCargoPlacement,
    RuntimeStateNotSupported,
    DuplicateCargoResource = DuplicateCargoPlacement,
    InvalidCargoParent,
    CargoDepthExceeded,
    InvalidGunState,

    // Vocabulary aliases kept in the error surface so callers can use the
    // graph terminology most natural to their integration layer.
    DuplicateId = DuplicateInstanceId,
    InvalidParent = UnknownParent,
    Cycle = CycleDetected,
    TooManyAttachments = TooManyNodes,
};

[[nodiscard]] constexpr bool vehicle_descriptor_codec_succeeded(
    VehicleDescriptorCodecError error) noexcept
{
    return error == VehicleDescriptorCodecError::None;
}

// Vector encoding writes one complete descriptor and leaves the destination
// untouched on failure.  The wire representation is canonical: node records
// are sorted by instance ID, while prefix/suffix affix order is preserved.
[[nodiscard]] VehicleDescriptorCodecError encode_vehicle_descriptor(
    const VehicleDescriptor&, std::vector<Byte>&) noexcept;

// This overload is useful for callers that already have a packet buffer.  It
// writes exactly the encoded descriptor prefix and leaves trailing capacity
// untouched.
[[nodiscard]] VehicleDescriptorCodecError encode_vehicle_descriptor(
    const VehicleDescriptor&, MutableByteView) noexcept;

[[nodiscard]] VehicleDescriptorCodecError decode_vehicle_descriptor(
    ByteView, VehicleDescriptor&) noexcept;

[[nodiscard]] bool vehicle_descriptor_semantic_equal(
    const VehicleDescriptor&, const VehicleDescriptor&) noexcept;

inline bool operator==(const VehicleDescriptor& left,
                       const VehicleDescriptor& right) noexcept
{
    return vehicle_descriptor_semantic_equal(left, right);
}

[[nodiscard]] std::uint64_t vehicle_descriptor_digest(
    const VehicleDescriptor&) noexcept;

// Equivalent spellings used by generic snapshot code.
[[nodiscard]] inline bool semantic_equal_vehicle_descriptor(
    const VehicleDescriptor& left, const VehicleDescriptor& right) noexcept
{
    return vehicle_descriptor_semantic_equal(left, right);
}

[[nodiscard]] inline std::uint64_t digest_vehicle_descriptor(
    const VehicleDescriptor& descriptor) noexcept
{
    return vehicle_descriptor_digest(descriptor);
}

} // namespace kraken::net

#endif // KRAKEN_NET_VEHICLE_DESCRIPTOR_HPP
