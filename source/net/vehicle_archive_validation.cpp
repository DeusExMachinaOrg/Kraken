#include "net/vehicle_archive_validation.hpp"

#include <algorithm>

namespace kraken::net {
namespace {

using PathRecord = VehicleArchiveNameBinding;

bool append_path(std::string& path, const std::string& slot)
{
    if (slot.empty())
        return false;
    if (!path.empty())
        path.push_back('/');
    path += slot;
    return path.size() <= kMaxVehicleDescriptorNameLength * 4u;
}

bool build_attachment_path(
    const VehicleDescriptorNode& node,
    const std::vector<VehicleDescriptorNode>& nodes, std::string& path)
{
    std::vector<const VehicleDescriptorNode*> chain;
    const VehicleDescriptorNode* current = &node;
    for (std::size_t count = 0; current != nullptr && count <= nodes.size();
         ++count) {
        if (std::find(chain.begin(), chain.end(), current) != chain.end())
            return false;
        chain.push_back(current);
        if (current->parent_instance_id == 0)
            break;
        current = nullptr;
        for (const VehicleDescriptorNode& candidate : nodes) {
            if (candidate.instance_id == chain.back()->parent_instance_id) {
                current = &candidate;
                break;
            }
        }
    }
    if (chain.empty() || chain.back()->parent_instance_id != 0)
        return false;
    std::reverse(chain.begin(), chain.end());
    for (const VehicleDescriptorNode* element : chain) {
        if (!append_path(path, element->slot))
            return false;
    }
    return true;
}

} // namespace

VehicleArchivePreflightResult preflight_vehicle_archive(
    const VehicleDescriptor& descriptor,
    const std::span<const VehicleArchiveNameBinding> destination_bindings)
    noexcept
{
    try {
        std::vector<Byte> validation_wire;
        const VehicleDescriptorCodecError codec_error =
            encode_vehicle_descriptor(descriptor, validation_wire);
        if (!vehicle_descriptor_codec_succeeded(codec_error))
            return {VehicleArchivePreflightError::DescriptorInvalid,
                    codec_error, 0};
        if (descriptor.native_structure.empty())
            return {VehicleArchivePreflightError::MissingNativeArchive,
                    VehicleDescriptorCodecError::None, 0};

        std::vector<PathRecord> descriptor_paths;
        descriptor_paths.reserve(descriptor.attachments.size());
        for (std::size_t index = 0; index < descriptor.attachments.size();
             ++index) {
            std::string path;
            if (!build_attachment_path(descriptor.attachments[index],
                                       descriptor.attachments, path))
                return {VehicleArchivePreflightError::DescriptorInvalid,
                        VehicleDescriptorCodecError::UnknownParent, index};
            if (std::any_of(descriptor_paths.begin(), descriptor_paths.end(),
                            [&path](const PathRecord& candidate) {
                                return candidate.path == path;
                            }))
                return {VehicleArchivePreflightError::DuplicateDestinationPath,
                        VehicleDescriptorCodecError::DuplicateSlot, index};
            descriptor_paths.push_back({path,
                                        descriptor.attachments[index].prototype_id,
                                        descriptor.attachments[index].prototype_name});
        }

        for (std::size_t index = 0; index < destination_bindings.size();
             ++index) {
            const VehicleArchiveNameBinding& destination =
                destination_bindings[index];
            if (destination.path.empty() ||
                std::any_of(destination_bindings.begin(),
                            destination_bindings.begin() + index,
                            [&destination](const PathRecord& prior) {
                                return prior.path == destination.path;
                            }))
                return {VehicleArchivePreflightError::DuplicateDestinationPath,
                        VehicleDescriptorCodecError::None, index};
        }

        for (std::size_t index = 0; index < descriptor_paths.size(); ++index) {
            const PathRecord& expected = descriptor_paths[index];
            const auto found = std::find_if(
                destination_bindings.begin(), destination_bindings.end(),
                [&expected](const PathRecord& candidate) {
                    return candidate.path == expected.path;
                });
            if (found == destination_bindings.end())
                return {VehicleArchivePreflightError::MissingDestinationPath,
                        VehicleDescriptorCodecError::None, index};
            if (found->prototype_id != expected.prototype_id ||
                found->prototype_name != expected.prototype_name)
                return {VehicleArchivePreflightError::NameCollision,
                        VehicleDescriptorCodecError::None, index};
        }
        return {};
    }
    catch (...) {
        return {VehicleArchivePreflightError::DescriptorInvalid,
                VehicleDescriptorCodecError::AllocationFailure, 0};
    }
}

} // namespace kraken::net
