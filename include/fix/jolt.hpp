#pragma once

// Forward-declared rather than pulling in <Jolt/Physics/PhysicsSystem.h> here - this header
// is included by entry.cpp and other light callers that only need Apply(); anything that
// actually needs the Jolt API (e.g. fix::joltshadow) includes the real Jolt headers itself.
namespace JPH {
    class PhysicsSystem;
}

namespace kraken::fix::jolt {
    void Apply();

    // Exposed for Stage 1+ modules (e.g. fix::joltshadow) that need to add their own bodies/
    // constraints to the same PhysicsSystem instance rather than owning a separate one.
    JPH::PhysicsSystem* GetPhysicsSystem();

    // Steps the shared PhysicsSystem by inDeltaTime seconds, using the same TempAllocator/
    // JobSystemThreadPool fix::jolt::Apply() already set up. No-op if Jolt isn't initialized
    // (config.jolt disabled). Stage 0's static bodies never needed this (nothing moves), but
    // Stage 1's shadow vehicle is dynamic and needs the physics system actually stepped once
    // per frame - kept here rather than exposing g_tempAllocator/g_jobSystem directly so the
    // init-lifecycle globals stay private to this translation unit.
    void StepPhysics(float inDeltaTime);
}
