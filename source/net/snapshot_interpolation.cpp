#include "net/snapshot_interpolation.hpp"

#include <algorithm>
#include <cmath>

namespace kraken::net {
namespace {

[[nodiscard]] bool finite(const VehicleQuaternion& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] float length_squared(const VehicleQuaternion& value) noexcept
{
    return value.x * value.x + value.y * value.y + value.z * value.z +
           value.w * value.w;
}

[[nodiscard]] bool normalize(VehicleQuaternion& value) noexcept
{
    if (!finite(value))
        return false;

    const float norm_squared = length_squared(value);
    if (!std::isfinite(norm_squared) || norm_squared <= 0.0f)
        return false;

    const float inverse_norm = 1.0f / std::sqrt(norm_squared);
    value.x *= inverse_norm;
    value.y *= inverse_norm;
    value.z *= inverse_norm;
    value.w *= inverse_norm;
    return finite(value);
}

[[nodiscard]] VehicleVector3 lerp(const VehicleVector3& left,
                                   const VehicleVector3& right,
                                   float alpha) noexcept
{
    return {left.x + (right.x - left.x) * alpha,
            left.y + (right.y - left.y) * alpha,
            left.z + (right.z - left.z) * alpha};
}

[[nodiscard]] VehicleQuaternion nlerp(const VehicleQuaternion& left,
                                      const VehicleQuaternion& right,
                                      float alpha,
                                      bool& valid) noexcept
{
    VehicleQuaternion a = left;
    VehicleQuaternion b = right;
    valid = normalize(a) && normalize(b);
    if (!valid)
        return {};

    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }

    VehicleQuaternion result{
        a.x + (b.x - a.x) * alpha,
        a.y + (b.y - a.y) * alpha,
        a.z + (b.z - a.z) * alpha,
        a.w + (b.w - a.w) * alpha,
    };
    valid = normalize(result);
    return result;
}

[[nodiscard]] float catmull_rom(float p0, float p1, float p2, float p3,
                                float alpha) noexcept
{
    const float alpha_squared = alpha * alpha;
    const float alpha_cubed = alpha_squared * alpha;
    return 0.5f *
           (2.0f * p1 + (-p0 + p2) * alpha +
            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * alpha_squared +
            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * alpha_cubed);
}

[[nodiscard]] VehicleVector3 catmull_rom(const VehicleVector3& p0,
                                          const VehicleVector3& p1,
                                          const VehicleVector3& p2,
                                          const VehicleVector3& p3,
                                          float alpha) noexcept
{
    return {catmull_rom(p0.x, p1.x, p2.x, p3.x, alpha),
            catmull_rom(p0.y, p1.y, p2.y, p3.y, alpha),
            catmull_rom(p0.z, p1.z, p2.z, p3.z, alpha)};
}

[[nodiscard]] float interpolation_fraction(SnapshotTimestampMs target,
                                            SnapshotTimestampMs left,
                                            SnapshotTimestampMs right) noexcept
{
    // The comparisons made by the caller guarantee right > left and target is
    // inside the interval.  Subtraction is therefore overflow-free.
    const double numerator = static_cast<double>(target - left);
    const double denominator = static_cast<double>(right - left);
    return static_cast<float>(std::clamp(numerator / denominator, 0.0, 1.0));
}

[[nodiscard]] SnapshotInterpolationStatus endpoint_snapshot(
    const VehicleSnapshot& source, VehicleSnapshot& output) noexcept
{
    output = source;
    if (!normalize(output.rotation))
        return SnapshotInterpolationStatus::InvalidQuaternion;
    return SnapshotInterpolationStatus::Ok;
}

} // namespace

SnapshotInterpolationStatus SnapshotInterpolationBuffer::push(
    SnapshotTimestampMs timestamp_ms, const VehicleSnapshot& snapshot) noexcept
{
    if (has_entity_ && snapshot.entity_id != entity_id_)
        return SnapshotInterpolationStatus::EntityIdMismatch;

    if (!has_entity_) {
        has_entity_ = true;
        entity_id_ = snapshot.entity_id;
    }

    std::size_t insertion_index = 0;
    while (insertion_index < count_ &&
           samples_[insertion_index].timestamp_ms < timestamp_ms)
        ++insertion_index;

    if (insertion_index < count_ &&
        samples_[insertion_index].timestamp_ms == timestamp_ms) {
        samples_[insertion_index] = {timestamp_ms, snapshot};
        return SnapshotInterpolationStatus::Ok;
    }

    if (count_ == samples_.size()) {
        // A full buffer cannot retain a sample older than its oldest entry.
        if (insertion_index == 0)
            return SnapshotInterpolationStatus::Ok;

        if (insertion_index == count_) {
            // The new sample is newest: discard the oldest entry.
            for (std::size_t index = 1; index < count_; ++index)
                samples_[index - 1] = samples_[index];
            samples_[count_ - 1] = {timestamp_ms, snapshot};
            return SnapshotInterpolationStatus::Ok;
        }
    }

    const std::size_t new_count =
        std::min(count_ + 1, static_cast<std::size_t>(samples_.size()));
    for (std::size_t index = new_count - 1; index > insertion_index; --index)
        samples_[index] = samples_[index - 1];
    samples_[insertion_index] = {timestamp_ms, snapshot};
    count_ = new_count;
    return SnapshotInterpolationStatus::Ok;
}

SnapshotInterpolationStatus SnapshotInterpolationBuffer::sample(
    SnapshotTimestampMs target_timestamp_ms,
    VehicleSnapshot& output) const noexcept
{
    if (count_ == 0)
        return SnapshotInterpolationStatus::Empty;

    if (target_timestamp_ms <= samples_[0].timestamp_ms)
        return endpoint_snapshot(samples_[0].snapshot, output);
    if (target_timestamp_ms >= samples_[count_ - 1].timestamp_ms)
        return endpoint_snapshot(samples_[count_ - 1].snapshot, output);

    std::size_t right_index = 1;
    while (right_index < count_ &&
           target_timestamp_ms > samples_[right_index].timestamp_ms)
        ++right_index;

    if (target_timestamp_ms == samples_[right_index].timestamp_ms)
        return endpoint_snapshot(samples_[right_index].snapshot, output);

    const std::size_t left_index = right_index - 1;
    const TimestampedVehicleSnapshot& left = samples_[left_index];
    const TimestampedVehicleSnapshot& right = samples_[right_index];
    const float alpha = interpolation_fraction(
        target_timestamp_ms, left.timestamp_ms, right.timestamp_ms);

    VehicleSnapshot result = left.snapshot;
    result.sequence = right.snapshot.sequence;
    result.server_tick = right.snapshot.server_tick;
    result.position = lerp(left.snapshot.position, right.snapshot.position,
                           alpha);

    // With four samples, the two middle samples form the only interval that
    // has a point on both sides.  Boundary intervals use linear interpolation
    // because extrapolating a Catmull-Rom tangent would need a fifth sample.
    if (count_ == kSnapshotInterpolationCapacity && left_index > 0 &&
        right_index + 1 < count_) {
        result.position = catmull_rom(samples_[left_index - 1].snapshot.position,
                                      left.snapshot.position,
                                      right.snapshot.position,
                                      samples_[right_index + 1].snapshot.position,
                                      alpha);
    }

    result.linear_velocity =
        lerp(left.snapshot.linear_velocity, right.snapshot.linear_velocity,
             alpha);
    result.angular_velocity =
        lerp(left.snapshot.angular_velocity, right.snapshot.angular_velocity,
             alpha);

    bool quaternion_valid = false;
    result.rotation = nlerp(left.snapshot.rotation, right.snapshot.rotation,
                            alpha, quaternion_valid);
    if (!quaternion_valid)
        return SnapshotInterpolationStatus::InvalidQuaternion;

    output = result;
    return SnapshotInterpolationStatus::Ok;
}

void SnapshotInterpolationBuffer::clear() noexcept
{
    count_ = 0;
    has_entity_ = false;
    entity_id_ = 0;
}

} // namespace kraken::net
