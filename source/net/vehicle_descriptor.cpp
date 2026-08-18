#include "net/vehicle_descriptor.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace kraken::net {
namespace {

[[nodiscard]] bool valid_name(const std::string& value,
                              std::size_t max_length) noexcept
{
    return !value.empty() && value.size() <= max_length;
}

[[nodiscard]] bool valid_scalar(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0f &&
           value <= kMaxVehicleDescriptorScalar;
}

[[nodiscard]] bool valid_signed_scalar(float value) noexcept
{
    return std::isfinite(value) &&
           std::fabs(value) <= kMaxVehicleDescriptorScalar;
}

[[nodiscard]] VehicleDescriptorCodecError validate_affixes(
    const std::vector<VehicleAffix>& affixes) noexcept
{
    if (affixes.size() > kMaxVehicleDescriptorAffixes)
        return VehicleDescriptorCodecError::TooManyAffixes;
    for (const VehicleAffix& affix : affixes) {
        if (affix.id < 0)
            return VehicleDescriptorCodecError::InvalidAffixId;
        if (!valid_name(affix.name, kMaxVehicleDescriptorAffixNameLength))
            return VehicleDescriptorCodecError::InvalidAffixName;
        if (affix.target_resource_id < 0 ||
            !valid_name(affix.target_resource_name,
                        kMaxVehicleDescriptorNameLength))
            return VehicleDescriptorCodecError::InvalidAffixTarget;
    }
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] bool valid_modifier_payload(
    const VehicleModifier& modifier) noexcept
{
    const std::size_t size = modifier.value_payload.size();
    if (size > kMaxVehicleModifierValuePayloadSize)
        return false;
    switch (modifier.value_type) {
    case VehicleModifierValueType::Undefined:
        return size == 0;
    case VehicleModifierValueType::Vector:
        return size == 12;
    case VehicleModifierValueType::Quaternion:
        return size == 16;
    case VehicleModifierValueType::Id:
    case VehicleModifierValueType::Float:
        return size == 4;
    case VehicleModifierValueType::String:
    case VehicleModifierValueType::StringList:
        return true;
    case VehicleModifierValueType::IdList:
        return (size % sizeof(std::int32_t)) == 0;
    case VehicleModifierValueType::Range:
        return size == 8;
    }
    return false;
}

[[nodiscard]] VehicleDescriptorCodecError validate_modifiers(
    const std::vector<VehicleModifier>& modifiers) noexcept
{
    if (modifiers.size() > kMaxVehicleDescriptorModifiers)
        return VehicleDescriptorCodecError::TooManyModifiers;
    for (const VehicleModifier& modifier : modifiers) {
        if (!valid_signed_scalar(modifier.timeout))
            return VehicleDescriptorCodecError::InvalidModifierTimeout;
        if (!is_valid_vehicle_modifier_operation(modifier.operation))
            return VehicleDescriptorCodecError::InvalidModifierOperation;
        if (modifier.magic_prototype_id < -1)
            return VehicleDescriptorCodecError::InvalidMagicPrototype;
        if (!valid_name(modifier.property_name,
                        kMaxVehicleDescriptorNameLength))
            return VehicleDescriptorCodecError::InvalidModifierProperty;
        if (modifier.sender_id < -1)
            return VehicleDescriptorCodecError::InvalidModifierSender;
        if (!is_valid_vehicle_modifier_value_type(modifier.value_type))
            return VehicleDescriptorCodecError::InvalidModifierValueType;
        if (!valid_modifier_payload(modifier))
            return VehicleDescriptorCodecError::InvalidModifierValue;
    }
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_blob(
    const std::vector<Byte>& blob) noexcept
{
    return blob.size() <= kMaxVehicleDescriptorBlobSize
               ? VehicleDescriptorCodecError::None
               : VehicleDescriptorCodecError::InvalidBlob;
}

[[nodiscard]] VehicleDescriptorCodecError validate_state(
    float health, float durability, float fuel, std::uint32_t ammo,
    std::uint32_t magazine, float reload) noexcept
{
    if (!valid_scalar(health))
        return VehicleDescriptorCodecError::InvalidHealth;
    if (!valid_scalar(durability))
        return VehicleDescriptorCodecError::InvalidDurability;
    if (!valid_scalar(fuel))
        return VehicleDescriptorCodecError::InvalidFuel;
    if (ammo > kMaxVehicleDescriptorCounter)
        return VehicleDescriptorCodecError::InvalidAmmo;
    if (magazine > kMaxVehicleDescriptorCounter)
        return VehicleDescriptorCodecError::InvalidMagazine;
    if (!valid_scalar(reload))
        return VehicleDescriptorCodecError::InvalidReload;
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_gun(
    const VehicleGunState& gun) noexcept
{
    if (!gun.present) {
        if (gun.barrel_index != 0 || gun.rotation != 0.0f ||
            gun.charge_state != VehicleGunChargeState::Ready ||
            gun.reload != 0.0f || gun.current_charge != 0 || gun.pool != 0 ||
            gun.firing)
            return VehicleDescriptorCodecError::InvalidGunState;
        return VehicleDescriptorCodecError::None;
    }
    if (!is_valid_vehicle_gun_charge_state(gun.charge_state) ||
        gun.barrel_index > kMaxVehicleDescriptorCounter ||
        !valid_signed_scalar(gun.rotation) || !valid_scalar(gun.reload) ||
        gun.current_charge > kMaxVehicleDescriptorCounter ||
        gun.pool > kMaxVehicleDescriptorCounter)
        return VehicleDescriptorCodecError::InvalidGunState;
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_cargo_placement(
    const VehicleCargoPlacement& placement) noexcept
{
    if (!is_valid_vehicle_cargo_repository(placement.repository))
        return VehicleDescriptorCodecError::InvalidCargoRepository;
    if (placement.x < 0 || placement.y < 0 ||
        placement.x > kMaxVehicleDescriptorCargoCoordinate ||
        placement.y > kMaxVehicleDescriptorCargoCoordinate)
        return VehicleDescriptorCodecError::InvalidCargoPlacement;
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_cargo_stacks(
    const std::vector<VehicleCargoStack>& stacks) noexcept
{
    if (stacks.size() > kMaxVehicleDescriptorCargoStacks)
        return VehicleDescriptorCodecError::TooManyCargoStacks;
    for (std::size_t index = 0; index < stacks.size(); ++index) {
        const VehicleCargoStack& stack = stacks[index];
        if (stack.resource_id < 0 ||
            !valid_name(stack.resource_name,
                        kMaxVehicleDescriptorNameLength))
            return VehicleDescriptorCodecError::InvalidCargoResource;
        if (stack.amount == 0 || stack.amount > kMaxVehicleDescriptorCounter)
            return VehicleDescriptorCodecError::InvalidCargoAmount;
        VehicleDescriptorCodecError error =
            validate_cargo_placement(stack.placement);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (stacks[prior].placement == stack.placement)
                return VehicleDescriptorCodecError::DuplicateCargoPlacement;
        }
    }
    return VehicleDescriptorCodecError::None;
}

template <typename Object>
[[nodiscard]] VehicleDescriptorCodecError validate_core(
    const Object& object) noexcept
{
    if (object.prototype_id < 0)
        return VehicleDescriptorCodecError::InvalidPrototype;
    if (!valid_name(object.prototype_name,
                    kMaxVehicleDescriptorNameLength))
        return VehicleDescriptorCodecError::InvalidName;
    if (object.skin > kMaxVehicleDescriptorSkin)
        return VehicleDescriptorCodecError::InvalidSkin;

    VehicleDescriptorCodecError error = validate_affixes(object.prefixes);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    error = validate_affixes(object.suffixes);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    error = validate_modifiers(object.modifiers);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    if (!object.runtime.empty())
        return VehicleDescriptorCodecError::RuntimeStateNotSupported;
    error = validate_blob(object.native_structure);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    error = validate_blob(object.runtime);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    error = validate_cargo_stacks(object.cargo_stacks);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    return validate_state(object.health, object.durability, object.fuel,
                          object.ammo, object.magazine, object.reload);
}

[[nodiscard]] VehicleDescriptorCodecError collect_node_ids(
    const VehicleDescriptorNode& node, std::size_t depth,
    std::vector<VehicleInstanceId>& ids)
{
    if (depth > kMaxVehicleDescriptorCargoDepth)
        return VehicleDescriptorCodecError::CargoDepthExceeded;
    if (node.instance_id == 0)
        return VehicleDescriptorCodecError::InvalidInstanceId;
    if (std::find(ids.begin(), ids.end(), node.instance_id) != ids.end())
        return VehicleDescriptorCodecError::DuplicateInstanceId;
    if (ids.size() >= kMaxVehicleDescriptorNodes)
        return VehicleDescriptorCodecError::TooManyNodes;
    ids.push_back(node.instance_id);
    if (node.cargo_objects.size() > kMaxVehicleDescriptorCargoObjects)
        return VehicleDescriptorCodecError::TooManyCargoObjects;
    for (const VehicleDescriptorNode& child : node.cargo_objects) {
        const VehicleDescriptorCodecError error =
            collect_node_ids(child, depth + 1, ids);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
    }
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_cargo_tree(
    const VehicleDescriptorNode& node, VehicleInstanceId expected_parent,
    std::size_t depth) noexcept
{
    if (depth > kMaxVehicleDescriptorCargoDepth)
        return VehicleDescriptorCodecError::CargoDepthExceeded;
    if (node.parent_instance_id != expected_parent)
        return VehicleDescriptorCodecError::InvalidCargoParent;
    if (!is_valid_vehicle_descriptor_node_kind(node.kind))
        return VehicleDescriptorCodecError::InvalidNodeKind;
    if (!valid_name(node.slot, kMaxVehicleDescriptorSlotLength))
        return VehicleDescriptorCodecError::InvalidSlot;
    VehicleDescriptorCodecError placement_error =
        validate_cargo_placement(node.cargo_placement);
    if (!vehicle_descriptor_codec_succeeded(placement_error))
        return placement_error;

    VehicleDescriptorCodecError error = validate_core(node);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    error = validate_gun(node.gun);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    if (node.cargo_objects.size() > kMaxVehicleDescriptorCargoObjects)
        return VehicleDescriptorCodecError::TooManyCargoObjects;

    for (std::size_t index = 0; index < node.cargo_objects.size(); ++index) {
        const VehicleDescriptorNode& child = node.cargo_objects[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (node.cargo_objects[prior].slot == child.slot)
                return VehicleDescriptorCodecError::DuplicateSlot;
        }
        error = validate_cargo_tree(child, node.instance_id, depth + 1);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
    }
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_attachment_tree(
    const VehicleDescriptorNode& node) noexcept
{
    if (!is_valid_vehicle_descriptor_node_kind(node.kind))
        return VehicleDescriptorCodecError::InvalidNodeKind;
    if (!valid_name(node.slot, kMaxVehicleDescriptorSlotLength))
        return VehicleDescriptorCodecError::InvalidSlot;
    VehicleDescriptorCodecError placement_error =
        validate_cargo_placement(node.cargo_placement);
    if (!vehicle_descriptor_codec_succeeded(placement_error))
        return placement_error;
    VehicleDescriptorCodecError error = validate_core(node);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    error = validate_gun(node.gun);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    if (node.cargo_objects.size() > kMaxVehicleDescriptorCargoObjects)
        return VehicleDescriptorCodecError::TooManyCargoObjects;
    for (std::size_t index = 0; index < node.cargo_objects.size(); ++index) {
        const VehicleDescriptorNode& child = node.cargo_objects[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (node.cargo_objects[prior].slot == child.slot)
                return VehicleDescriptorCodecError::DuplicateSlot;
        }
        error = validate_cargo_tree(child, node.instance_id, 1);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
    }
    return VehicleDescriptorCodecError::None;
}

[[nodiscard]] VehicleDescriptorCodecError validate_descriptor(
    const VehicleDescriptor& descriptor)
{
    VehicleDescriptorCodecError error = validate_core(descriptor);
    if (!vehicle_descriptor_codec_succeeded(error))
        return error;
    if (descriptor.attachments.size() > kMaxVehicleDescriptorNodes)
        return VehicleDescriptorCodecError::TooManyNodes;
    if (descriptor.cargo_objects.size() > kMaxVehicleDescriptorCargoObjects)
        return VehicleDescriptorCodecError::TooManyCargoObjects;

    std::vector<VehicleInstanceId> ids;
    ids.reserve(kMaxVehicleDescriptorNodes);
    for (const VehicleDescriptorNode& cargo : descriptor.cargo_objects) {
        error = collect_node_ids(cargo, 1, ids);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
    }
    for (const VehicleDescriptorNode& attachment : descriptor.attachments) {
        error = collect_node_ids(attachment, 0, ids);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
    }

    for (std::size_t index = 0; index < descriptor.cargo_objects.size();
         ++index) {
        const VehicleDescriptorNode& cargo = descriptor.cargo_objects[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (descriptor.cargo_objects[prior].slot == cargo.slot)
                return VehicleDescriptorCodecError::DuplicateSlot;
        }
        error = validate_cargo_tree(cargo, 0, 1);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
    }

    for (std::size_t index = 0; index < descriptor.attachments.size(); ++index) {
        const VehicleDescriptorNode& node = descriptor.attachments[index];
        error = validate_attachment_tree(node);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
        for (std::size_t prior = 0; prior < index; ++prior) {
            const VehicleDescriptorNode& previous = descriptor.attachments[prior];
            if (previous.parent_instance_id == node.parent_instance_id &&
                previous.slot == node.slot)
                return VehicleDescriptorCodecError::DuplicateSlot;
        }
    }

    // Attachment parent links form a graph independent from repository
    // containment.  Parent IDs must therefore resolve among top-level
    // attachment records, not merely anywhere in the object tree.
    for (const VehicleDescriptorNode& start : descriptor.attachments) {
        std::vector<VehicleInstanceId> visited;
        visited.reserve(descriptor.attachments.size() + 1);
        visited.push_back(start.instance_id);
        VehicleInstanceId current = start.parent_instance_id;
        while (current != 0) {
            if (std::find(visited.begin(), visited.end(), current) !=
                visited.end())
                return VehicleDescriptorCodecError::CycleDetected;
            const auto parent = std::find_if(
                descriptor.attachments.begin(), descriptor.attachments.end(),
                [current](const VehicleDescriptorNode& candidate) {
                    return candidate.instance_id == current;
                });
            if (parent == descriptor.attachments.end())
                return VehicleDescriptorCodecError::UnknownParent;
            visited.push_back(current);
            current = parent->parent_instance_id;
        }
    }
    return VehicleDescriptorCodecError::None;
}

class WireSizer final {
public:
    [[nodiscard]] bool add(std::size_t amount) noexcept
    {
        if (amount > std::numeric_limits<std::size_t>::max() - m_size)
            return false;
        m_size += amount;
        return m_size <= kMaxVehicleDescriptorWireSize;
    }

    [[nodiscard]] bool string(const std::string& value) noexcept
    {
        return add(sizeof(std::uint16_t)) && add(value.size());
    }

    [[nodiscard]] bool blob(const std::vector<Byte>& value,
                            std::size_t length_size) noexcept
    {
        return add(length_size) && add(value.size());
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }

private:
    std::size_t m_size = 0;
};

[[nodiscard]] bool size_affixes(WireSizer& size,
                                const std::vector<VehicleAffix>& affixes)
    noexcept
{
    if (!size.add(sizeof(std::uint16_t)))
        return false;
    for (const VehicleAffix& affix : affixes) {
        if (!size.add(sizeof(std::int32_t)) || !size.string(affix.name) ||
            !size.add(sizeof(std::int32_t)) ||
            !size.string(affix.target_resource_name))
            return false;
    }
    return true;
}

[[nodiscard]] bool size_modifiers(
    WireSizer& size, const std::vector<VehicleModifier>& modifiers) noexcept
{
    if (!size.add(sizeof(std::uint16_t)))
        return false;
    for (const VehicleModifier& modifier : modifiers) {
        if (!size.add(16) || !size.string(modifier.property_name) ||
            !size.blob(modifier.value_payload, sizeof(std::uint16_t)))
            return false;
    }
    return true;
}

template <typename Object>
[[nodiscard]] bool size_core(WireSizer& size, const Object& object) noexcept
{
    return size.add(8) && size.string(object.prototype_name) && size.add(24) &&
           size.blob(object.native_structure, sizeof(std::uint32_t)) &&
           size.blob(object.runtime, sizeof(std::uint32_t)) &&
           size_affixes(size, object.prefixes) &&
           size_affixes(size, object.suffixes) &&
           size_modifiers(size, object.modifiers);
}

[[nodiscard]] bool size_node(WireSizer&, const VehicleDescriptorNode&,
                             std::size_t) noexcept;

template <typename Object>
[[nodiscard]] bool size_cargo(WireSizer& size, const Object& object,
                              std::size_t depth) noexcept
{
    if (!size.add(sizeof(std::uint16_t)))
        return false;
    for (const VehicleCargoStack& stack : object.cargo_stacks) {
        if (!size.add(17) || !size.string(stack.resource_name))
            return false;
    }
    if (!size.add(sizeof(std::uint16_t)))
        return false;
    for (const VehicleDescriptorNode& child : object.cargo_objects) {
        if (!size_node(size, child, depth + 1))
            return false;
    }
    return true;
}

[[nodiscard]] bool size_node(WireSizer& size,
                             const VehicleDescriptorNode& node,
                             std::size_t depth) noexcept
{
    if (depth > kMaxVehicleDescriptorCargoDepth)
        return false;
    return size.add(21) && size.string(node.slot) && size_core(size, node) &&
           size.add(24) && size_cargo(size, node, depth);
}

[[nodiscard]] bool descriptor_wire_size(const VehicleDescriptor& descriptor,
                                        std::size_t& result) noexcept
{
    WireSizer size;
    if (!size.add(8) || !size_core(size, descriptor) ||
        !size_cargo(size, descriptor, 0) ||
        !size.add(sizeof(std::uint16_t)))
        return false;
    for (const VehicleDescriptorNode& node : descriptor.attachments) {
        if (!size_node(size, node, 0))
            return false;
    }
    result = size.size();
    return true;
}

class Writer final {
public:
    explicit Writer(std::vector<Byte>& output) noexcept : m_output(output) {}

    void u8(std::uint8_t value) { m_output.push_back(static_cast<Byte>(value)); }

    void u16(std::uint16_t value)
    {
        const std::size_t offset = m_output.size();
        m_output.resize(offset + 2);
        m_output[offset + 0] = static_cast<Byte>(value & 0xffu);
        m_output[offset + 1] = static_cast<Byte>((value >> 8) & 0xffu);
    }

    void u32(std::uint32_t value)
    {
        const std::size_t offset = m_output.size();
        m_output.resize(offset + 4);
        for (std::size_t index = 0; index < 4; ++index)
            m_output[offset + index] =
                static_cast<Byte>((value >> (index * 8)) & 0xffu);
    }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }

    void string(const std::string& value)
    {
        u16(static_cast<std::uint16_t>(value.size()));
        bytes(value.data(), value.size());
    }

    void blob32(const std::vector<Byte>& value)
    {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(value.data(), value.size());
    }

    void blob16(const std::vector<Byte>& value)
    {
        u16(static_cast<std::uint16_t>(value.size()));
        bytes(value.data(), value.size());
    }

private:
    void bytes(const void* data, std::size_t size)
    {
        if (size == 0)
            return;
        const Byte* first = reinterpret_cast<const Byte*>(data);
        m_output.insert(m_output.end(), first, first + size);
    }

    std::vector<Byte>& m_output;
};

void write_affixes(Writer& writer,
                   const std::vector<VehicleAffix>& affixes)
{
    writer.u16(static_cast<std::uint16_t>(affixes.size()));
    for (const VehicleAffix& affix : affixes) {
        writer.i32(affix.id);
        writer.string(affix.name);
        writer.i32(affix.target_resource_id);
        writer.string(affix.target_resource_name);
    }
}

void write_modifiers(Writer& writer,
                     const std::vector<VehicleModifier>& modifiers)
{
    writer.u16(static_cast<std::uint16_t>(modifiers.size()));
    for (const VehicleModifier& modifier : modifiers) {
        writer.f32(modifier.timeout);
        writer.u8(static_cast<std::uint8_t>(modifier.operation));
        writer.u8(static_cast<std::uint8_t>(modifier.value_type));
        writer.u16(0);
        writer.i32(modifier.magic_prototype_id);
        writer.i32(modifier.sender_id);
        writer.string(modifier.property_name);
        writer.blob16(modifier.value_payload);
    }
}

template <typename Object>
void write_core(Writer& writer, const Object& object)
{
    writer.i32(object.prototype_id);
    writer.u32(object.skin);
    writer.string(object.prototype_name);
    writer.f32(object.health);
    writer.f32(object.durability);
    writer.f32(object.fuel);
    writer.u32(object.ammo);
    writer.u32(object.magazine);
    writer.f32(object.reload);
    writer.blob32(object.native_structure);
    writer.blob32(object.runtime);
    write_affixes(writer, object.prefixes);
    write_affixes(writer, object.suffixes);
    write_modifiers(writer, object.modifiers);
}

void write_gun(Writer& writer, const VehicleGunState& gun)
{
    writer.u8(gun.present ? 1 : 0);
    writer.u8(gun.firing ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(gun.charge_state));
    writer.u8(0);
    writer.u32(gun.barrel_index);
    writer.f32(gun.rotation);
    writer.f32(gun.reload);
    writer.u32(gun.current_charge);
    writer.u32(gun.pool);
}

void write_placement(Writer& writer, const VehicleCargoPlacement& placement)
{
    writer.u8(static_cast<std::uint8_t>(placement.repository));
    writer.i32(placement.x);
    writer.i32(placement.y);
}

[[nodiscard]] std::vector<const VehicleDescriptorNode*> sorted_nodes(
    const std::vector<VehicleDescriptorNode>& nodes)
{
    std::vector<const VehicleDescriptorNode*> sorted;
    sorted.reserve(nodes.size());
    for (const VehicleDescriptorNode& node : nodes)
        sorted.push_back(&node);
    std::sort(sorted.begin(), sorted.end(),
              [](const VehicleDescriptorNode* left,
                 const VehicleDescriptorNode* right) {
                  return left->instance_id < right->instance_id;
              });
    return sorted;
}

[[nodiscard]] std::vector<const VehicleCargoStack*> sorted_stacks(
    const std::vector<VehicleCargoStack>& stacks)
{
    std::vector<const VehicleCargoStack*> sorted;
    sorted.reserve(stacks.size());
    for (const VehicleCargoStack& stack : stacks)
        sorted.push_back(&stack);
    std::sort(sorted.begin(), sorted.end(),
              [](const VehicleCargoStack* left,
                 const VehicleCargoStack* right) {
                  if (left->placement.repository !=
                      right->placement.repository)
                      return left->placement.repository <
                             right->placement.repository;
                  if (left->placement.x != right->placement.x)
                      return left->placement.x < right->placement.x;
                  if (left->placement.y != right->placement.y)
                      return left->placement.y < right->placement.y;
                  if (left->resource_id != right->resource_id)
                      return left->resource_id < right->resource_id;
                  return left->resource_name < right->resource_name;
              });
    return sorted;
}

void write_node(Writer&, const VehicleDescriptorNode&);

template <typename Object>
void write_cargo(Writer& writer, const Object& object)
{
    const std::vector<const VehicleCargoStack*> stacks =
        sorted_stacks(object.cargo_stacks);
    writer.u16(static_cast<std::uint16_t>(stacks.size()));
    for (const VehicleCargoStack* stack : stacks) {
        writer.i32(stack->resource_id);
        writer.u32(stack->amount);
        write_placement(writer, stack->placement);
        writer.string(stack->resource_name);
    }
    const std::vector<const VehicleDescriptorNode*> objects =
        sorted_nodes(object.cargo_objects);
    writer.u16(static_cast<std::uint16_t>(objects.size()));
    for (const VehicleDescriptorNode* child : objects)
        write_node(writer, *child);
}

void write_node(Writer& writer, const VehicleDescriptorNode& node)
{
    writer.u8(static_cast<std::uint8_t>(node.kind));
    writer.u8(0);
    writer.u16(0);
    writer.u32(node.instance_id);
    writer.u32(node.parent_instance_id);
    writer.string(node.slot);
    write_placement(writer, node.cargo_placement);
    write_core(writer, node);
    write_gun(writer, node.gun);
    write_cargo(writer, node);
}

class Reader final {
public:
    explicit Reader(ByteView input) noexcept : m_input(input) {}

    [[nodiscard]] VehicleDescriptorCodecError error() const noexcept
    {
        return m_error;
    }

    [[nodiscard]] bool done() const noexcept
    {
        return m_position == m_input.size();
    }

    void fail(VehicleDescriptorCodecError error) noexcept
    {
        if (m_error == VehicleDescriptorCodecError::None)
            m_error = error;
    }

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept
    {
        if (!ensure(1))
            return false;
        value = static_cast<std::uint8_t>(m_input[m_position++]);
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) noexcept
    {
        if (!ensure(2))
            return false;
        value = static_cast<std::uint16_t>(m_input[m_position + 0]) |
                static_cast<std::uint16_t>(m_input[m_position + 1]) << 8;
        m_position += 2;
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) noexcept
    {
        if (!ensure(4))
            return false;
        value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(m_input[m_position + index])
                     << (index * 8);
        m_position += 4;
        return true;
    }

    [[nodiscard]] bool i32(std::int32_t& value) noexcept
    {
        std::uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = static_cast<std::int32_t>(raw);
        return true;
    }

    [[nodiscard]] bool f32(float& value) noexcept
    {
        std::uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = std::bit_cast<float>(raw);
        return true;
    }

    [[nodiscard]] bool string(std::string& value, std::size_t max_length,
                              VehicleDescriptorCodecError invalid_error)
    {
        std::uint16_t length = 0;
        if (!u16(length))
            return false;
        if (length == 0 || length > max_length) {
            fail(invalid_error);
            return false;
        }
        if (!ensure(length))
            return false;
        value.assign(reinterpret_cast<const char*>(m_input.data() + m_position),
                     length);
        m_position += length;
        return true;
    }

    [[nodiscard]] bool blob32(std::vector<Byte>& value,
                              std::size_t max_length,
                              VehicleDescriptorCodecError invalid_error)
    {
        std::uint32_t length = 0;
        if (!u32(length))
            return false;
        return blob_data(value, length, max_length, invalid_error);
    }

    [[nodiscard]] bool blob16(std::vector<Byte>& value,
                              std::size_t max_length,
                              VehicleDescriptorCodecError invalid_error)
    {
        std::uint16_t length = 0;
        if (!u16(length))
            return false;
        return blob_data(value, length, max_length, invalid_error);
    }

private:
    [[nodiscard]] bool ensure(std::size_t amount) noexcept
    {
        if (m_position > m_input.size() ||
            amount > m_input.size() - m_position) {
            fail(VehicleDescriptorCodecError::InputSizeMismatch);
            return false;
        }
        return m_error == VehicleDescriptorCodecError::None;
    }

    [[nodiscard]] bool blob_data(std::vector<Byte>& value,
                                 std::size_t length, std::size_t max_length,
                                 VehicleDescriptorCodecError invalid_error)
    {
        if (length > max_length) {
            fail(invalid_error);
            return false;
        }
        if (!ensure(length))
            return false;
        value.assign(m_input.begin() + static_cast<std::ptrdiff_t>(m_position),
                     m_input.begin() +
                         static_cast<std::ptrdiff_t>(m_position + length));
        m_position += length;
        return true;
    }

    ByteView m_input;
    std::size_t m_position = 0;
    VehicleDescriptorCodecError m_error = VehicleDescriptorCodecError::None;
};

[[nodiscard]] bool read_affixes(Reader& reader,
                                std::vector<VehicleAffix>& affixes)
{
    std::uint16_t count = 0;
    if (!reader.u16(count))
        return false;
    if (count > kMaxVehicleDescriptorAffixes) {
        reader.fail(VehicleDescriptorCodecError::TooManyAffixes);
        return false;
    }
    affixes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        VehicleAffix affix{};
        if (!reader.i32(affix.id) ||
            !reader.string(affix.name,
                           kMaxVehicleDescriptorAffixNameLength,
                           VehicleDescriptorCodecError::InvalidAffixName) ||
            !reader.i32(affix.target_resource_id) ||
            !reader.string(affix.target_resource_name,
                           kMaxVehicleDescriptorNameLength,
                           VehicleDescriptorCodecError::InvalidAffixTarget))
            return false;
        affixes.push_back(std::move(affix));
    }
    return true;
}

[[nodiscard]] bool read_modifiers(Reader& reader,
                                  std::vector<VehicleModifier>& modifiers)
{
    std::uint16_t count = 0;
    if (!reader.u16(count))
        return false;
    if (count > kMaxVehicleDescriptorModifiers) {
        reader.fail(VehicleDescriptorCodecError::TooManyModifiers);
        return false;
    }
    modifiers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        VehicleModifier modifier{};
        std::uint8_t operation = 0;
        std::uint8_t value_type = 0;
        std::uint16_t reserved = 0;
        if (!reader.f32(modifier.timeout) || !reader.u8(operation) ||
            !reader.u8(value_type) || !reader.u16(reserved))
            return false;
        if (reserved != 0) {
            reader.fail(VehicleDescriptorCodecError::BadFlags);
            return false;
        }
        modifier.operation = static_cast<VehicleModifierOperation>(operation);
        modifier.value_type =
            static_cast<VehicleModifierValueType>(value_type);
        if (!reader.i32(modifier.magic_prototype_id) ||
            !reader.i32(modifier.sender_id) ||
            !reader.string(modifier.property_name,
                           kMaxVehicleDescriptorNameLength,
                           VehicleDescriptorCodecError::InvalidModifierProperty) ||
            !reader.blob16(modifier.value_payload,
                           kMaxVehicleModifierValuePayloadSize,
                           VehicleDescriptorCodecError::InvalidModifierValue))
            return false;
        modifiers.push_back(std::move(modifier));
    }
    return true;
}

template <typename Object>
[[nodiscard]] bool read_core(Reader& reader, Object& object)
{
    return reader.i32(object.prototype_id) && reader.u32(object.skin) &&
           reader.string(object.prototype_name,
                         kMaxVehicleDescriptorNameLength,
                         VehicleDescriptorCodecError::InvalidName) &&
           reader.f32(object.health) && reader.f32(object.durability) &&
           reader.f32(object.fuel) && reader.u32(object.ammo) &&
           reader.u32(object.magazine) && reader.f32(object.reload) &&
           reader.blob32(object.native_structure,
                         kMaxVehicleDescriptorBlobSize,
                         VehicleDescriptorCodecError::InvalidBlob) &&
           reader.blob32(object.runtime, kMaxVehicleDescriptorBlobSize,
                         VehicleDescriptorCodecError::InvalidBlob) &&
           read_affixes(reader, object.prefixes) &&
           read_affixes(reader, object.suffixes) &&
           read_modifiers(reader, object.modifiers);
}

[[nodiscard]] bool read_gun(Reader& reader, VehicleGunState& gun) noexcept
{
    std::uint8_t present = 0;
    std::uint8_t firing = 0;
    std::uint8_t charge_state = 0;
    std::uint8_t reserved = 0;
    if (!reader.u8(present) || !reader.u8(firing) ||
        !reader.u8(charge_state) || !reader.u8(reserved))
        return false;
    if (present > 1 || firing > 1 || reserved != 0) {
        reader.fail(VehicleDescriptorCodecError::InvalidGunState);
        return false;
    }
    gun.present = present != 0;
    gun.firing = firing != 0;
    gun.charge_state = static_cast<VehicleGunChargeState>(charge_state);
    return reader.u32(gun.barrel_index) && reader.f32(gun.rotation) &&
           reader.f32(gun.reload) && reader.u32(gun.current_charge) &&
           reader.u32(gun.pool);
}

[[nodiscard]] bool read_placement(Reader& reader,
                                  VehicleCargoPlacement& placement) noexcept
{
    std::uint8_t repository = 0;
    if (!reader.u8(repository) || !reader.i32(placement.x) ||
        !reader.i32(placement.y))
        return false;
    placement.repository = static_cast<VehicleCargoRepository>(repository);
    const VehicleDescriptorCodecError error =
        validate_cargo_placement(placement);
    if (!vehicle_descriptor_codec_succeeded(error)) {
        reader.fail(error);
        return false;
    }
    return true;
}

[[nodiscard]] bool read_node(Reader&, VehicleDescriptorNode&, std::size_t);

template <typename Object>
[[nodiscard]] bool read_cargo(Reader& reader, Object& object,
                              std::size_t depth)
{
    std::uint16_t stack_count = 0;
    if (!reader.u16(stack_count))
        return false;
    if (stack_count > kMaxVehicleDescriptorCargoStacks) {
        reader.fail(VehicleDescriptorCodecError::TooManyCargoStacks);
        return false;
    }
    object.cargo_stacks.reserve(stack_count);
    for (std::size_t index = 0; index < stack_count; ++index) {
        VehicleCargoStack stack{};
        if (!reader.i32(stack.resource_id) || !reader.u32(stack.amount) ||
            !read_placement(reader, stack.placement) ||
            !reader.string(stack.resource_name,
                           kMaxVehicleDescriptorNameLength,
                           VehicleDescriptorCodecError::InvalidCargoResource))
            return false;
        object.cargo_stacks.push_back(std::move(stack));
    }

    std::uint16_t object_count = 0;
    if (!reader.u16(object_count))
        return false;
    if (object_count > kMaxVehicleDescriptorCargoObjects) {
        reader.fail(VehicleDescriptorCodecError::TooManyCargoObjects);
        return false;
    }
    if (object_count != 0 && depth >= kMaxVehicleDescriptorCargoDepth) {
        reader.fail(VehicleDescriptorCodecError::CargoDepthExceeded);
        return false;
    }
    object.cargo_objects.reserve(object_count);
    for (std::size_t index = 0; index < object_count; ++index) {
        VehicleDescriptorNode child{};
        if (!read_node(reader, child, depth + 1))
            return false;
        object.cargo_objects.push_back(std::move(child));
    }
    return true;
}

[[nodiscard]] bool read_node(Reader& reader, VehicleDescriptorNode& node,
                             std::size_t depth)
{
    if (depth > kMaxVehicleDescriptorCargoDepth) {
        reader.fail(VehicleDescriptorCodecError::CargoDepthExceeded);
        return false;
    }
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
    if (!reader.u8(kind) || !reader.u8(flags) || !reader.u16(reserved))
        return false;
    if (flags != 0 || reserved != 0) {
        reader.fail(VehicleDescriptorCodecError::BadFlags);
        return false;
    }
    node.kind = static_cast<VehicleDescriptorNodeKind>(kind);
    return reader.u32(node.instance_id) &&
           reader.u32(node.parent_instance_id) &&
           reader.string(node.slot, kMaxVehicleDescriptorSlotLength,
                         VehicleDescriptorCodecError::InvalidSlot) &&
           read_placement(reader, node.cargo_placement) &&
           read_core(reader, node) && read_gun(reader, node.gun) &&
           read_cargo(reader, node, depth);
}

[[nodiscard]] bool same_float(float left, float right) noexcept
{
    return std::bit_cast<std::uint32_t>(left) ==
           std::bit_cast<std::uint32_t>(right);
}

[[nodiscard]] bool same_modifier(const VehicleModifier& left,
                                 const VehicleModifier& right) noexcept
{
    return same_float(left.timeout, right.timeout) &&
           left.operation == right.operation &&
           left.magic_prototype_id == right.magic_prototype_id &&
           left.property_name == right.property_name &&
           left.sender_id == right.sender_id &&
           left.value_type == right.value_type &&
           left.value_payload == right.value_payload;
}

[[nodiscard]] bool same_modifiers(
    const std::vector<VehicleModifier>& left,
    const std::vector<VehicleModifier>& right) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_modifier(left[index], right[index]))
            return false;
    }
    return true;
}

template <typename Left, typename Right>
[[nodiscard]] bool same_core(const Left& left, const Right& right) noexcept
{
    return left.prototype_id == right.prototype_id &&
           left.prototype_name == right.prototype_name &&
           left.skin == right.skin && left.prefixes == right.prefixes &&
           left.suffixes == right.suffixes &&
           same_modifiers(left.modifiers, right.modifiers) &&
           left.native_structure == right.native_structure &&
           left.runtime == right.runtime &&
           same_float(left.health, right.health) &&
           same_float(left.durability, right.durability) &&
           same_float(left.fuel, right.fuel) && left.ammo == right.ammo &&
           left.magazine == right.magazine &&
           same_float(left.reload, right.reload);
}

[[nodiscard]] bool same_gun(const VehicleGunState& left,
                            const VehicleGunState& right) noexcept
{
    return left.present == right.present &&
           left.barrel_index == right.barrel_index &&
           same_float(left.rotation, right.rotation) &&
           left.charge_state == right.charge_state &&
           same_float(left.reload, right.reload) &&
           left.current_charge == right.current_charge &&
           left.pool == right.pool && left.firing == right.firing;
}

[[nodiscard]] bool same_node(const VehicleDescriptorNode&,
                             const VehicleDescriptorNode&) noexcept;

template <typename Left, typename Right>
[[nodiscard]] bool same_cargo(const Left& left, const Right& right) noexcept
{
    if (left.cargo_stacks.size() != right.cargo_stacks.size() ||
        left.cargo_objects.size() != right.cargo_objects.size())
        return false;
    for (const VehicleCargoStack& stack : left.cargo_stacks) {
        const auto matching = std::find_if(
            right.cargo_stacks.begin(), right.cargo_stacks.end(),
            [&stack](const VehicleCargoStack& candidate) {
                return candidate.resource_id == stack.resource_id &&
                       candidate.placement == stack.placement;
            });
        if (matching == right.cargo_stacks.end() || !(*matching == stack))
            return false;
    }
    for (const VehicleDescriptorNode& object : left.cargo_objects) {
        const auto matching = std::find_if(
            right.cargo_objects.begin(), right.cargo_objects.end(),
            [&object](const VehicleDescriptorNode& candidate) {
                return candidate.instance_id == object.instance_id;
            });
        if (matching == right.cargo_objects.end() ||
            !same_node(object, *matching))
            return false;
    }
    return true;
}

[[nodiscard]] bool same_node(const VehicleDescriptorNode& left,
                             const VehicleDescriptorNode& right) noexcept
{
    return left.kind == right.kind && left.instance_id == right.instance_id &&
           left.parent_instance_id == right.parent_instance_id &&
           left.slot == right.slot &&
           left.cargo_placement == right.cargo_placement &&
           same_core(left, right) &&
           same_gun(left.gun, right.gun) && same_cargo(left, right);
}

[[nodiscard]] std::uint64_t fnv1a(ByteView bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (Byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

VehicleDescriptorCodecError encode_vehicle_descriptor(
    const VehicleDescriptor& descriptor, std::vector<Byte>& output) noexcept
{
    try {
        const VehicleDescriptorCodecError validation =
            validate_descriptor(descriptor);
        if (!vehicle_descriptor_codec_succeeded(validation))
            return validation;

        std::size_t wire_size = 0;
        if (!descriptor_wire_size(descriptor, wire_size))
            return VehicleDescriptorCodecError::PayloadTooLarge;

        std::vector<Byte> encoded;
        encoded.reserve(wire_size);
        Writer writer(encoded);
        writer.u32(kVehicleDescriptorWireMagic);
        writer.u16(kVehicleDescriptorWireVersion);
        writer.u16(kVehicleDescriptorWireFlags);
        write_core(writer, descriptor);
        write_cargo(writer, descriptor);

        const std::vector<const VehicleDescriptorNode*> attachments =
            sorted_nodes(descriptor.attachments);
        writer.u16(static_cast<std::uint16_t>(attachments.size()));
        for (const VehicleDescriptorNode* node : attachments)
            write_node(writer, *node);

        if (encoded.size() != wire_size)
            return VehicleDescriptorCodecError::PayloadTooLarge;
        output.swap(encoded);
        return VehicleDescriptorCodecError::None;
    }
    catch (const std::bad_alloc&) {
        return VehicleDescriptorCodecError::AllocationFailure;
    }
}

VehicleDescriptorCodecError encode_vehicle_descriptor(
    const VehicleDescriptor& descriptor, MutableByteView output) noexcept
{
    try {
        std::vector<Byte> encoded;
        const VehicleDescriptorCodecError error =
            encode_vehicle_descriptor(descriptor, encoded);
        if (!vehicle_descriptor_codec_succeeded(error))
            return error;
        if (output.size() < encoded.size())
            return VehicleDescriptorCodecError::OutputTooSmall;
        if (!encoded.empty())
            std::memcpy(output.data(), encoded.data(), encoded.size());
        return VehicleDescriptorCodecError::None;
    }
    catch (const std::bad_alloc&) {
        return VehicleDescriptorCodecError::AllocationFailure;
    }
}

VehicleDescriptorCodecError decode_vehicle_descriptor(
    ByteView input, VehicleDescriptor& output) noexcept
{
    try {
        if (input.size() > kMaxVehicleDescriptorWireSize)
            return VehicleDescriptorCodecError::PayloadTooLarge;
        if (input.size() < 8)
            return VehicleDescriptorCodecError::InputSizeMismatch;

        Reader reader(input);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t flags = 0;
        if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(flags))
            return reader.error();
        if (magic != kVehicleDescriptorWireMagic)
            return VehicleDescriptorCodecError::BadMagic;
        // v1 and v2 lack the v3 repository identity/placement contract.
        // Decode is intentionally rejected rather than guessing whether a
        // resource belongs to main or ground storage.
        if (version != kVehicleDescriptorWireVersion)
            return VehicleDescriptorCodecError::BadVersion;
        if (flags != kVehicleDescriptorWireFlags)
            return VehicleDescriptorCodecError::BadFlags;

        VehicleDescriptor decoded{};
        if (!read_core(reader, decoded) || !read_cargo(reader, decoded, 0))
            return reader.error();

        std::uint16_t attachment_count = 0;
        if (!reader.u16(attachment_count))
            return reader.error();
        if (attachment_count > kMaxVehicleDescriptorNodes)
            return VehicleDescriptorCodecError::TooManyNodes;
        decoded.attachments.reserve(attachment_count);
        for (std::size_t index = 0; index < attachment_count; ++index) {
            VehicleDescriptorNode node{};
            if (!read_node(reader, node, 0))
                return reader.error();
            decoded.attachments.push_back(std::move(node));
        }
        if (!reader.done())
            return VehicleDescriptorCodecError::InputSizeMismatch;

        const VehicleDescriptorCodecError validation =
            validate_descriptor(decoded);
        if (!vehicle_descriptor_codec_succeeded(validation))
            return validation;
        output = std::move(decoded);
        return VehicleDescriptorCodecError::None;
    }
    catch (const std::bad_alloc&) {
        return VehicleDescriptorCodecError::AllocationFailure;
    }
}

bool vehicle_descriptor_semantic_equal(const VehicleDescriptor& left,
                                       const VehicleDescriptor& right) noexcept
{
    if (!same_core(left, right) || !same_cargo(left, right) ||
        left.attachments.size() != right.attachments.size())
        return false;
    for (const VehicleDescriptorNode& node : left.attachments) {
        const auto matching = std::find_if(
            right.attachments.begin(), right.attachments.end(),
            [&node](const VehicleDescriptorNode& candidate) {
                return candidate.instance_id == node.instance_id;
            });
        if (matching == right.attachments.end() || !same_node(node, *matching))
            return false;
    }
    return true;
}

std::uint64_t vehicle_descriptor_digest(
    const VehicleDescriptor& descriptor) noexcept
{
    try {
        std::vector<Byte> encoded;
        if (!vehicle_descriptor_codec_succeeded(
                encode_vehicle_descriptor(descriptor, encoded)))
            return 0;
        return fnv1a(encoded);
    }
    catch (const std::bad_alloc&) {
        return 0;
    }
}

bool VehicleDescriptor::semantic_equal(
    const VehicleDescriptor& other) const noexcept
{
    return vehicle_descriptor_semantic_equal(*this, other);
}

std::uint64_t VehicleDescriptor::digest() const noexcept
{
    return vehicle_descriptor_digest(*this);
}

} // namespace kraken::net
