#ifndef KRAKEN_NET_VEHICLE_ARCHIVE_VALIDATION_HPP
#define KRAKEN_NET_VEHICLE_ARCHIVE_VALIDATION_HPP

#include "net/vehicle_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kraken::net {

// Engine-neutral description of a destination graph name.  It contains no
// pointer or ObjId; runtime adapters derive these records from the suspended
// native graph before LoadFromXML.
struct VehicleArchiveNameBinding {
    std::string path;
    VehiclePrototypeId prototype_id = -1;
    std::string prototype_name;
};

enum class VehicleArchivePreflightError : std::uint8_t {
    None,
    DescriptorInvalid,
    MissingNativeArchive,
    DuplicateDestinationPath,
    NameCollision,
    MissingDestinationPath,
};

struct VehicleArchivePreflightResult {
    VehicleArchivePreflightError error = VehicleArchivePreflightError::None;
    VehicleDescriptorCodecError codec_error =
        VehicleDescriptorCodecError::None;
    std::size_t record_index = 0;

    [[nodiscard]] bool ok() const noexcept
    { return error == VehicleArchivePreflightError::None; }
    explicit operator bool() const noexcept { return ok(); }
};

// Validates the typed descriptor and proves that its attachment paths resolve
// to the expected suspended destination graph.  A mismatch is rejected before
// native LoadFromXML, because that ABI is void-returning and may partially
// mutate the object.  This layer never remaps opaque native local IDs or
// external references; callers must reject archives whose references cannot
// be proven safe.  Empty destination bindings skip graph comparison for
// engine-neutral callers; the runtime adapter always supplies them.
[[nodiscard]] VehicleArchivePreflightResult preflight_vehicle_archive(
    const VehicleDescriptor& descriptor,
    std::span<const VehicleArchiveNameBinding> destination_bindings) noexcept;

} // namespace kraken::net

#endif // KRAKEN_NET_VEHICLE_ARCHIVE_VALIDATION_HPP
