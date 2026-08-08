#ifndef KRAKEN_NET_SNAPSHOT_INTERPOLATION_HPP
#define KRAKEN_NET_SNAPSHOT_INTERPOLATION_HPP

#include "net/vehicle_snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kraken::net {

// Milliseconds from a caller-defined monotonic epoch.  Keeping the public API
// chrono-free makes it usable from the engine and from Lua-facing adapters.
using SnapshotTimestampMs = std::uint64_t;

inline constexpr std::size_t kSnapshotInterpolationCapacity = 4;

struct TimestampedVehicleSnapshot {
    SnapshotTimestampMs timestamp_ms = 0;
    VehicleSnapshot snapshot{};
};

enum class SnapshotInterpolationStatus : std::uint8_t {
    Ok,
    Empty,
    EntityIdMismatch,
    InvalidQuaternion,
};

[[nodiscard]] constexpr bool snapshot_interpolation_succeeded(
    SnapshotInterpolationStatus status) noexcept
{
    return status == SnapshotInterpolationStatus::Ok;
}

// A fixed-size, single-entity history.  The owner can keep one instance per
// network entity; this keeps the module allocation-free and deterministic.
// Samples are kept in timestamp order, and only the four newest timestamps
// are retained.  Sampling never extrapolates.
class SnapshotInterpolationBuffer final {
public:
    [[nodiscard]] SnapshotInterpolationStatus push(
        SnapshotTimestampMs timestamp_ms,
        const VehicleSnapshot& snapshot) noexcept;

    [[nodiscard]] SnapshotInterpolationStatus sample(
        SnapshotTimestampMs target_timestamp_ms,
        VehicleSnapshot& output) const noexcept;

    void clear() noexcept;

    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return count_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return count_ == 0;
    }

    [[nodiscard]] constexpr bool has_entity() const noexcept
    {
        return has_entity_;
    }

    [[nodiscard]] constexpr std::uint32_t entity_id() const noexcept
    {
        return entity_id_;
    }

private:
    std::array<TimestampedVehicleSnapshot,
               kSnapshotInterpolationCapacity>
        samples_{};
    std::size_t count_ = 0;
    bool has_entity_ = false;
    std::uint32_t entity_id_ = 0;
};

} // namespace kraken::net

#endif // KRAKEN_NET_SNAPSHOT_INTERPOLATION_HPP
