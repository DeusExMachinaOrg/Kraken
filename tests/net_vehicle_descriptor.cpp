#include "net/vehicle_descriptor.hpp"
#include "net/vehicle_archive_validation.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace kraken::net;

namespace {

void put_u16(Byte* destination, std::uint16_t value)
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* destination, std::uint32_t value)
{
    destination[0] = static_cast<Byte>((value >> 0) & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
    destination[2] = static_cast<Byte>((value >> 16) & 0xffu);
    destination[3] = static_cast<Byte>((value >> 24) & 0xffu);
}

std::uint16_t get_u16(const Byte* source)
{
    return static_cast<std::uint16_t>(source[0]) |
           static_cast<std::uint16_t>(source[1]) << 8;
}

std::uint32_t get_u32(const Byte* source)
{
    return static_cast<std::uint32_t>(source[0]) |
           static_cast<std::uint32_t>(source[1]) << 8 |
           static_cast<std::uint32_t>(source[2]) << 16 |
           static_cast<std::uint32_t>(source[3]) << 24;
}

std::vector<Byte> payload(std::initializer_list<std::uint8_t> values)
{
    std::vector<Byte> result;
    result.reserve(values.size());
    for (std::uint8_t value : values)
        result.push_back(static_cast<Byte>(value));
    return result;
}

VehicleAffix affix(std::int32_t id, const char* name,
                   std::int32_t resource_id, const char* resource_name)
{
    return {id, name, resource_id, resource_name};
}

VehicleModifier modifier(VehicleModifierOperation operation,
                         VehicleModifierValueType value_type,
                         std::vector<Byte> value_payload,
                         const char* property_name)
{
    VehicleModifier result{};
    result.timeout = 2.5f;
    result.operation = operation;
    result.magic_prototype_id = 9001;
    result.property_name = property_name;
    result.sender_id = 77;
    result.value_type = value_type;
    result.value_payload = std::move(value_payload);
    return result;
}

VehicleDescriptorNode minimal_node(std::uint32_t id, std::uint32_t parent,
                                   const std::string& slot,
                                   const std::string& prototype = "part")
{
    VehicleDescriptorNode result{};
    result.instance_id = id;
    result.parent_instance_id = parent;
    result.slot = slot;
    result.prototype_id = static_cast<std::int32_t>(1000 + id);
    result.prototype_name = prototype;
    return result;
}

VehicleDescriptorNode rich_node(
    std::uint32_t id, std::uint32_t parent, const char* slot,
    const char* prototype,
    VehicleDescriptorNodeKind kind = VehicleDescriptorNodeKind::Attachment)
{
    VehicleDescriptorNode result = minimal_node(id, parent, slot, prototype);
    result.kind = kind;
    result.skin = id + 3;
    result.health = 10.0f + static_cast<float>(id);
    result.durability = 20.0f + static_cast<float>(id);
    result.fuel = 30.0f + static_cast<float>(id);
    result.ammo = id * 2;
    result.magazine = id;
    result.reload = 0.25f * static_cast<float>(id);
    result.prefixes = {affix(static_cast<std::int32_t>(id),
                             ("prefix" + std::to_string(id)).c_str(), 5,
                             "weapon")};
    result.suffixes = {affix(static_cast<std::int32_t>(id + 100),
                             ("suffix" + std::to_string(id)).c_str(), 5,
                             "weapon")};
    result.modifiers = {
        modifier(VehicleModifierOperation::Add,
                 VehicleModifierValueType::Float,
                 payload({0x00, 0x00, 0x20, 0x41}), "Durability")};
    result.native_structure =
        {Byte{static_cast<unsigned char>(id)}, Byte{0xA5}};
    return result;
}

VehicleDescriptor rich_descriptor()
{
    VehicleDescriptor result{};
    result.prototype_id = 42;
    result.prototype_name = "vehicle.prototype.rich";
    result.skin = 7;
    result.health = 91.5f;
    result.durability = 73.25f;
    result.fuel = 18.0f;
    result.ammo = 93;
    result.magazine = 11;
    result.reload = 1.75f;
    result.prefixes = {affix(11, "reinforced", 4, "cabin"),
                       affix(12, "accurate", 5, "weapon")};
    result.suffixes = {affix(21, "of-haste", 4, "cabin"),
                       affix(22, "of-capacity", 6, "basket")};
    result.modifiers = {
        modifier(VehicleModifierOperation::Multiply,
                 VehicleModifierValueType::Float,
                 payload({0x00, 0x00, 0xC0, 0x3F}), "MaxDurability"),
        modifier(VehicleModifierOperation::Set,
                 VehicleModifierValueType::String,
                 payload({'r', 'i', 'c', 'h'}), "DisplayMode")};
    result.native_structure = {Byte{0x00}, Byte{0x01}, Byte{0xFE}, Byte{0xFF}};
    result.cargo_stacks = {
        {9, "shell.heavy", 27,
         {VehicleCargoRepository::Main, 0, 0}},
        {9, "shell.heavy", 3,
         {VehicleCargoRepository::Ground, 4, 1}},
        {2, "fuel.cell", 4,
         {VehicleCargoRepository::Main, 1, 0}}};

    VehicleDescriptorNode root_cargo_a = rich_node(
        100, 0, "repository/1", "crate.armored",
        VehicleDescriptorNodeKind::Container);
    root_cargo_a.cargo_placement =
        {VehicleCargoRepository::Main, 2, 3};
    root_cargo_a.cargo_stacks = {
        {30, "parts.rare", 3,
         {VehicleCargoRepository::Main, 0, 0}}};
    root_cargo_a.cargo_objects.push_back(
        rich_node(101, 100, "repository/0", "artifact.core"));
    VehicleDescriptorNode root_cargo_b =
        rich_node(102, 0, "repository/0", "repair.kit");
    root_cargo_b.cargo_placement =
        {VehicleCargoRepository::Ground, 3, 2};
    result.cargo_objects = {std::move(root_cargo_a), std::move(root_cargo_b)};

    VehicleDescriptorNode container = rich_node(
        20, 10, "cargo", "cargo.large",
        VehicleDescriptorNodeKind::Container);
    container.cargo_stacks = {
        {44, "ammo.rail", 80,
         {VehicleCargoRepository::Main, 0, 0}},
        {12, "scrap", 9,
         {VehicleCargoRepository::Main, 1, 0}}};
    VehicleDescriptorNode chassis =
        rich_node(10, 0, "chassis", "chassis.main");
    chassis.cargo_objects.push_back(
        rich_node(110, 10, "repository/0", "module.spare"));
    VehicleDescriptorNode gun = rich_node(30, 20, "weapon", "gun.rail");
    gun.gun.present = true;
    gun.gun.barrel_index = 2;
    gun.gun.rotation = -0.375f;
    gun.gun.charge_state = VehicleGunChargeState::Charging;
    gun.gun.reload = 1.125f;
    gun.gun.current_charge = 3;
    gun.gun.pool = 47;
    gun.gun.firing = true;

    // Intentionally non-canonical source order.
    result.attachments =
        {std::move(container), std::move(chassis), std::move(gun)};
    return result;
}

VehicleDescriptor graph_descriptor()
{
    VehicleDescriptor result{};
    result.prototype_id = 1;
    result.prototype_name = "vehicle";
    result.attachments = {minimal_node(20, 10, "b"),
                          minimal_node(10, 0, "a"),
                          minimal_node(30, 20, "c")};
    return result;
}

std::size_t minimal_node_wire_size(const VehicleDescriptorNode& node)
{
    // Relation header + slot, core, gun state, and empty cargo counts.
    return 21 + 2 + node.slot.size() + 48 + node.prototype_name.size() + 24 + 4;
}

std::vector<std::size_t> graph_node_offsets(const VehicleDescriptor& graph)
{
    std::vector<const VehicleDescriptorNode*> sorted;
    for (const VehicleDescriptorNode& node : graph.attachments)
        sorted.push_back(&node);
    std::sort(sorted.begin(), sorted.end(),
              [](const VehicleDescriptorNode* left,
                 const VehicleDescriptorNode* right) {
                  return left->instance_id < right->instance_id;
              });

    // Header + minimal root core + empty root cargo + attachment count.
    std::size_t offset = 8 + 48 + graph.prototype_name.size() + 4 + 2;
    std::vector<std::size_t> result;
    for (const VehicleDescriptorNode* node : sorted) {
        result.push_back(offset);
        offset += minimal_node_wire_size(*node);
    }
    return result;
}

void test_v3_rich_round_trip_and_canonical_order()
{
    const VehicleDescriptor expected = rich_descriptor();
    std::vector<Byte> wire;
    assert(encode_vehicle_descriptor(expected, wire) ==
           VehicleDescriptorCodecError::None);
    assert(get_u32(wire.data()) == kVehicleDescriptorWireMagic);
    assert(get_u16(wire.data() + 4) == kVehicleDescriptorWireVersion);

    VehicleDescriptor decoded{};
    assert(decode_vehicle_descriptor(wire, decoded) ==
           VehicleDescriptorCodecError::None);
    assert(decoded == expected);
    assert(decoded.skin == 7 && decoded.durability == 73.25f &&
           decoded.fuel == 18.0f && decoded.runtime.empty());
    assert(decoded.attachments[0].instance_id == 10);
    assert(decoded.attachments[2].gun.present);
    assert(decoded.attachments[2].gun.current_charge == 3);
    assert(decoded.cargo_objects[0].instance_id == 100);
    assert(decoded.cargo_objects[0].cargo_objects[0].instance_id == 101);

    std::vector<Byte> reencoded;
    assert(encode_vehicle_descriptor(decoded, reencoded) ==
           VehicleDescriptorCodecError::None);
    assert(reencoded == wire);
    assert(vehicle_descriptor_digest(expected) != 0);
    assert(vehicle_descriptor_digest(expected) ==
           vehicle_descriptor_digest(decoded));

    VehicleDescriptor reordered = expected;
    std::reverse(reordered.attachments.begin(), reordered.attachments.end());
    std::reverse(reordered.cargo_stacks.begin(), reordered.cargo_stacks.end());
    std::reverse(reordered.cargo_objects.begin(), reordered.cargo_objects.end());
    std::reverse(reordered.attachments[1].cargo_stacks.begin(),
                 reordered.attachments[1].cargo_stacks.end());
    assert(vehicle_descriptor_semantic_equal(expected, reordered));
    std::vector<Byte> reordered_wire;
    assert(encode_vehicle_descriptor(reordered, reordered_wire) ==
           VehicleDescriptorCodecError::None);
    assert(reordered_wire == wire);

    std::vector<Byte> fixed(wire.size() + 1, Byte{0xCD});
    assert(encode_vehicle_descriptor(expected, MutableByteView{fixed}) ==
           VehicleDescriptorCodecError::None);
    assert(std::equal(wire.begin(), wire.end(), fixed.begin()));
    assert(fixed.back() == Byte{0xCD});
}

void test_legacy_and_malformed_wire_are_rejected()
{
    const VehicleDescriptor expected = rich_descriptor();
    std::vector<Byte> wire;
    assert(encode_vehicle_descriptor(expected, wire) ==
           VehicleDescriptorCodecError::None);
    VehicleDescriptor output = expected;

    std::vector<Byte> malformed = wire;
    put_u16(malformed.data() + 4, kVehicleDescriptorLegacyWireVersion);
    assert(decode_vehicle_descriptor(malformed, output) ==
           VehicleDescriptorCodecError::BadVersion);
    assert(output == expected);

    malformed = wire;
    put_u16(malformed.data() + 4, kVehicleDescriptorPreviousWireVersion);
    assert(decode_vehicle_descriptor(malformed, output) ==
           VehicleDescriptorCodecError::BadVersion);
    assert(output == expected);

    malformed = wire;
    malformed[0] = Byte{};
    assert(decode_vehicle_descriptor(malformed, output) ==
           VehicleDescriptorCodecError::BadMagic);
    malformed = wire;
    malformed[6] = Byte{1};
    assert(decode_vehicle_descriptor(malformed, output) ==
           VehicleDescriptorCodecError::BadFlags);
    malformed = wire;
    malformed.pop_back();
    assert(decode_vehicle_descriptor(malformed, output) ==
           VehicleDescriptorCodecError::InputSizeMismatch);
    malformed = wire;
    malformed.push_back(Byte{});
    assert(decode_vehicle_descriptor(malformed, output) ==
           VehicleDescriptorCodecError::InputSizeMismatch);
    assert(encode_vehicle_descriptor(expected, MutableByteView{}) ==
           VehicleDescriptorCodecError::OutputTooSmall);
}

void test_modifier_affix_gun_and_cargo_validation()
{
    std::vector<Byte> wire;
    VehicleDescriptor invalid = rich_descriptor();
    invalid.prefixes[0].target_resource_id = -1;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidAffixTarget);

    invalid = rich_descriptor();
    invalid.modifiers[0].operation =
        static_cast<VehicleModifierOperation>(99);
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidModifierOperation);
    invalid = rich_descriptor();
    invalid.modifiers[0].value_type =
        static_cast<VehicleModifierValueType>(99);
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidModifierValueType);
    invalid = rich_descriptor();
    invalid.modifiers[0].value_payload.pop_back();
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidModifierValue);
    invalid = rich_descriptor();
    invalid.modifiers[0].timeout = std::numeric_limits<float>::quiet_NaN();
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidModifierTimeout);

    invalid = rich_descriptor();
    invalid.attachments[0].gun.barrel_index = 1;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidGunState);
    invalid = rich_descriptor();
    invalid.attachments[2].gun.charge_state =
        static_cast<VehicleGunChargeState>(99);
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidGunState);

    invalid = rich_descriptor();
    invalid.cargo_stacks.push_back(invalid.cargo_stacks[0]);
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::DuplicateCargoResource);
    invalid = rich_descriptor();
    invalid.cargo_stacks[0].amount = 0;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidCargoAmount);
    invalid = rich_descriptor();
    invalid.cargo_stacks[0].placement.x =
        kMaxVehicleDescriptorCargoCoordinate + 1;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidCargoPlacement);
    invalid = rich_descriptor();
    invalid.cargo_objects[0].parent_instance_id = 44;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::InvalidCargoParent);

    invalid = rich_descriptor();
    invalid.runtime = {Byte{0x01}};
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::RuntimeStateNotSupported);
}

void test_graph_validation_on_encode_and_decode()
{
    std::vector<Byte> wire;
    VehicleDescriptor invalid = graph_descriptor();
    invalid.attachments[2].instance_id = 20;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::DuplicateInstanceId);
    invalid = graph_descriptor();
    invalid.attachments[0].parent_instance_id = 9999;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::UnknownParent);
    invalid = graph_descriptor();
    invalid.attachments[1].parent_instance_id = 30;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::CycleDetected);
    invalid = rich_descriptor();
    invalid.cargo_objects[0].instance_id = 10;
    assert(encode_vehicle_descriptor(invalid, wire) ==
           VehicleDescriptorCodecError::DuplicateInstanceId);

    const VehicleDescriptor graph = graph_descriptor();
    const std::vector<std::size_t> offsets = graph_node_offsets(graph);
    VehicleDescriptor output{};
    assert(encode_vehicle_descriptor(graph, wire) ==
           VehicleDescriptorCodecError::None);
    put_u32(wire.data() + offsets[0] + 8, 9999);
    assert(decode_vehicle_descriptor(wire, output) ==
           VehicleDescriptorCodecError::UnknownParent);

    assert(encode_vehicle_descriptor(graph, wire) ==
           VehicleDescriptorCodecError::None);
    put_u32(wire.data() + offsets[1] + 4, 10);
    assert(decode_vehicle_descriptor(wire, output) ==
           VehicleDescriptorCodecError::DuplicateInstanceId);

    assert(encode_vehicle_descriptor(graph, wire) ==
           VehicleDescriptorCodecError::None);
    put_u32(wire.data() + offsets[0] + 8, 30);
    put_u32(wire.data() + offsets[1] + 8, 10);
    put_u32(wire.data() + offsets[2] + 8, 20);
    assert(decode_vehicle_descriptor(wire, output) ==
           VehicleDescriptorCodecError::CycleDetected);
}

void test_maximum_bounds_and_cargo_depth()
{
    VehicleDescriptor maximum{};
    maximum.prototype_id = 1;
    maximum.prototype_name = "vehicle";
    maximum.native_structure.resize(kMaxVehicleDescriptorBlobSize,
                                    Byte{0xAB});
    maximum.modifiers.reserve(kMaxVehicleDescriptorModifiers);
    for (std::size_t index = 0; index < kMaxVehicleDescriptorModifiers;
         ++index) {
        VehicleModifier entry{};
        entry.property_name = "property" + std::to_string(index);
        maximum.modifiers.push_back(std::move(entry));
    }
    maximum.cargo_stacks.reserve(kMaxVehicleDescriptorCargoStacks);
    for (std::size_t index = 0; index < kMaxVehicleDescriptorCargoStacks;
         ++index) {
        maximum.cargo_stacks.push_back(
            {static_cast<std::int32_t>(index),
             "resource" + std::to_string(index), 1,
             {VehicleCargoRepository::Main,
              static_cast<std::int32_t>(index), 0}});
    }
    for (std::size_t index = 0; index < kMaxVehicleDescriptorNodes; ++index) {
        maximum.attachments.push_back(minimal_node(
            static_cast<std::uint32_t>(index + 1), 0,
            "slot" + std::to_string(index)));
    }

    std::vector<Byte> wire;
    assert(encode_vehicle_descriptor(maximum, wire) ==
           VehicleDescriptorCodecError::None);
    VehicleDescriptor decoded{};
    assert(decode_vehicle_descriptor(wire, decoded) ==
           VehicleDescriptorCodecError::None);
    assert(decoded == maximum);

    maximum.attachments.push_back(minimal_node(10000, 0, "overflow"));
    assert(encode_vehicle_descriptor(maximum, wire) ==
           VehicleDescriptorCodecError::TooManyNodes);

    VehicleDescriptor modifier_overflow{};
    modifier_overflow.prototype_id = 1;
    modifier_overflow.prototype_name = "vehicle";
    modifier_overflow.modifiers.resize(kMaxVehicleDescriptorModifiers + 1);
    assert(encode_vehicle_descriptor(modifier_overflow, wire) ==
           VehicleDescriptorCodecError::TooManyModifiers);

    VehicleDescriptor stack_overflow{};
    stack_overflow.prototype_id = 1;
    stack_overflow.prototype_name = "vehicle";
    stack_overflow.cargo_stacks.resize(kMaxVehicleDescriptorCargoStacks + 1);
    assert(encode_vehicle_descriptor(stack_overflow, wire) ==
           VehicleDescriptorCodecError::TooManyCargoStacks);

    VehicleDescriptor depth{};
    depth.prototype_id = 1;
    depth.prototype_name = "vehicle";
    depth.cargo_objects.push_back(minimal_node(1, 0, "depth1"));
    VehicleDescriptorNode* cursor = &depth.cargo_objects.back();
    for (std::size_t level = 2; level <= kMaxVehicleDescriptorCargoDepth;
         ++level) {
        cursor->cargo_objects.push_back(minimal_node(
            static_cast<std::uint32_t>(level), cursor->instance_id,
            "depth" + std::to_string(level)));
        cursor = &cursor->cargo_objects.back();
    }
    assert(encode_vehicle_descriptor(depth, wire) ==
           VehicleDescriptorCodecError::None);
    cursor->cargo_objects.push_back(minimal_node(
        static_cast<std::uint32_t>(kMaxVehicleDescriptorCargoDepth + 1),
        cursor->instance_id, "too-deep"));
    assert(encode_vehicle_descriptor(depth, wire) ==
           VehicleDescriptorCodecError::CargoDepthExceeded);
}

void test_archive_preflight_rejects_name_collision()
{
    VehicleDescriptor descriptor = graph_descriptor();
    descriptor.native_structure = {Byte{0xAA}};
    std::vector<VehicleArchiveNameBinding> destination{
        {"a", 1010, "part"},
        {"a/b", 9999, "unrelated"},
        {"a/b/c", 1030, "part"}};
    const auto rejected = preflight_vehicle_archive(descriptor, destination);
    assert(rejected.error == VehicleArchivePreflightError::NameCollision);

    destination[1] = {"a/b", 1020, "part"};
    const auto accepted = preflight_vehicle_archive(descriptor, destination);
    assert(accepted.ok());

    destination.pop_back();
    const auto partial = preflight_vehicle_archive(descriptor, destination);
    assert(partial.error == VehicleArchivePreflightError::MissingDestinationPath);

    descriptor.native_structure.clear();
    assert(preflight_vehicle_archive(descriptor, destination).error ==
           VehicleArchivePreflightError::MissingNativeArchive);
}

} // namespace

int main()
{
    test_v3_rich_round_trip_and_canonical_order();
    test_legacy_and_malformed_wire_are_rejected();
    test_modifier_affix_gun_and_cargo_validation();
    test_graph_validation_on_encode_and_decode();
    test_maximum_bounds_and_cargo_depth();
    test_archive_preflight_rejects_name_collision();
    return 0;
}
