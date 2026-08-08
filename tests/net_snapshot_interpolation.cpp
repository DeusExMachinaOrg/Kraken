#include "net/snapshot_interpolation.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

using kraken::net::SnapshotInterpolationBuffer;
using kraken::net::SnapshotInterpolationStatus;
using kraken::net::SnapshotTimestampMs;
using kraken::net::VehicleQuaternion;
using kraken::net::VehicleSnapshot;

bool close(float left, float right, float epsilon = 0.0001f)
{
    return std::fabs(left - right) <= epsilon;
}

bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

VehicleSnapshot make_snapshot(std::uint32_t entity_id, float position_x,
                              std::uint32_t sequence = 0)
{
    VehicleSnapshot snapshot{};
    snapshot.entity_id = entity_id;
    snapshot.sequence = sequence;
    snapshot.server_tick = sequence * 10;
    snapshot.position.x = position_x;
    snapshot.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    return snapshot;
}

bool test_endpoints()
{
    SnapshotInterpolationBuffer buffer;
    const VehicleSnapshot first = make_snapshot(7, 10.0f, 1);
    const VehicleSnapshot last = make_snapshot(7, 20.0f, 2);
    bool passed = buffer.push(100, first) == SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(200, last) == SnapshotInterpolationStatus::Ok;

    VehicleSnapshot output{};
    passed &= buffer.sample(100, output) == SnapshotInterpolationStatus::Ok;
    passed &= output.position.x == first.position.x;
    passed &= output.sequence == first.sequence;
    passed &= buffer.sample(200, output) == SnapshotInterpolationStatus::Ok;
    passed &= output.position.x == last.position.x;
    passed &= output.sequence == last.sequence;
    return check(passed, "endpoints");
}

bool test_two_sample_linear()
{
    SnapshotInterpolationBuffer buffer;
    VehicleSnapshot first = make_snapshot(9, 0.0f);
    VehicleSnapshot last = make_snapshot(9, 10.0f);
    first.linear_velocity.x = 2.0f;
    last.linear_velocity.x = 6.0f;
    first.angular_velocity.z = 4.0f;
    last.angular_velocity.z = 8.0f;
    bool passed = buffer.push(0, first) == SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(100, last) == SnapshotInterpolationStatus::Ok;

    VehicleSnapshot output{};
    passed &= buffer.sample(50, output) == SnapshotInterpolationStatus::Ok;
    passed &= close(output.position.x, 5.0f);
    passed &= close(output.linear_velocity.x, 4.0f);
    passed &= close(output.angular_velocity.z, 6.0f);
    return check(passed, "two sample linear interpolation");
}

bool test_four_sample_catmull_rom()
{
    SnapshotInterpolationBuffer buffer;
    bool passed = buffer.push(0, make_snapshot(11, 0.0f)) ==
                  SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(100, make_snapshot(11, 10.0f)) ==
              SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(200, make_snapshot(11, 20.0f)) ==
              SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(300, make_snapshot(11, 60.0f)) ==
              SnapshotInterpolationStatus::Ok;

    VehicleSnapshot output{};
    passed &= buffer.sample(150, output) == SnapshotInterpolationStatus::Ok;
    // Uniform Catmull-Rom through 0, 10, 20, 60 at t=.5 on [10, 20].
    passed &= close(output.position.x, 13.125f);
    return check(passed, "four sample Catmull-Rom position");
}

bool test_no_extrapolation_and_bounded_history()
{
    SnapshotInterpolationBuffer buffer;
    bool passed = buffer.push(10, make_snapshot(13, 10.0f)) ==
                  SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(20, make_snapshot(13, 20.0f)) ==
              SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(30, make_snapshot(13, 30.0f)) ==
              SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(40, make_snapshot(13, 40.0f)) ==
              SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(50, make_snapshot(13, 50.0f)) ==
              SnapshotInterpolationStatus::Ok;
    passed &= buffer.size() == 4;

    VehicleSnapshot output{};
    passed &= buffer.sample(0, output) == SnapshotInterpolationStatus::Ok;
    passed &= output.position.x == 20.0f;
    passed &= buffer.sample(100, output) == SnapshotInterpolationStatus::Ok;
    passed &= output.position.x == 50.0f;
    return check(passed, "bounded history and no extrapolation");
}

bool test_quaternion_normalization()
{
    SnapshotInterpolationBuffer buffer;
    VehicleSnapshot first = make_snapshot(17, 0.0f);
    VehicleSnapshot last = make_snapshot(17, 1.0f);
    first.rotation = {0.0f, 0.0f, 0.0f, 2.0f};
    last.rotation = {0.0f, 0.0f, 2.0f, 0.0f};
    bool passed = buffer.push(0, first) == SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(100, last) == SnapshotInterpolationStatus::Ok;

    VehicleSnapshot output{};
    passed &= buffer.sample(50, output) == SnapshotInterpolationStatus::Ok;
    const VehicleQuaternion& q = output.rotation;
    const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z +
                                  q.w * q.w);
    passed &= close(norm, 1.0f);
    passed &= close(q.z, 0.70710677f, 0.0002f);
    passed &= close(q.w, 0.70710677f, 0.0002f);
    return check(passed, "normalized quaternion nlerp");
}

bool test_entity_id_mismatch()
{
    SnapshotInterpolationBuffer buffer;
    bool passed = buffer.push(0, make_snapshot(21, 1.0f)) ==
                  SnapshotInterpolationStatus::Ok;
    passed &= buffer.push(1, make_snapshot(22, 2.0f)) ==
              SnapshotInterpolationStatus::EntityIdMismatch;
    passed &= buffer.size() == 1;
    passed &= buffer.entity_id() == 21;
    return check(passed, "entity ID mismatch rejection");
}

} // namespace

int main()
{
    bool passed = true;
    passed &= test_endpoints();
    passed &= test_two_sample_linear();
    passed &= test_four_sample_catmull_rom();
    passed &= test_no_extrapolation_and_bounded_history();
    passed &= test_quaternion_normalization();
    passed &= test_entity_id_mismatch();

    if (passed)
        std::cout << "snapshot interpolation tests passed\n";
    return passed ? 0 : 1;
}
