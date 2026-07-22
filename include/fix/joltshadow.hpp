#pragma once

#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"

namespace kraken::fix::joltshadow {
    void Apply();

    // Moves the player's Jolt shadow body (not just the ODE mirror) to pos/rot and zeroes its
    // velocity. See the definition in joltshadow.cpp for why this exists - a plain ODE-side
    // teleport alone gets silently overwritten by ApplyJoltToVehicle under [jolt_harness]
    // apply=1. Returns false (no-op, not an error) if Jolt isn't active or no player shadow
    // body exists yet.
    bool TeleportPlayerShadow(const hta::CVector& pos, const hta::Quaternion& rot);
}
