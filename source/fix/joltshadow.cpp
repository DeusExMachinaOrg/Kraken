#define LOGGER "joltshadow"

// Must precede any transitive <windows.h> include (via config.hpp/routines.hpp/stdafx.hpp)
// in this translation unit - same NOMINMAX gotcha already documented in fix::jolt (jolt.cpp):
// windows.h's min/max macros mangle Jolt's own min()/max() calls into syntax errors otherwise.
#define NOMINMAX

#include "ext/logger.hpp"
#include "fix/joltshadow.hpp"
#include "fix/jolt.hpp"
#include "fix/kineticfriction.hpp"
#include "fix/wheelmodel_core.hpp" // docs §39: spring_wheel's engine-agnostic Pacejka contact-force model, being ported onto the Jolt vehicle
#include "config.hpp"
#include "routines.hpp"
#include "ode/ode.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h> // docs §59: SubShapeID -> sub-shape index decode
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h> // docs §58 (Этап 1, шаг 2): the wheel strut
#include <Jolt/Physics/IslandBuilder.h>        // docs §124: WheelContactConstraint::BuildIslands
#include <Jolt/Physics/LargeIslandSplitter.h>  // docs §124: WheelContactConstraint::BuildIslandSplits
#include <Jolt/Physics/Body/BodyManager.h>     // docs §124: WheelContactConstraint::BuildIslands

#include "hta/ai/BreakableObject.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/Geom.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/VehiclePart.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/m3d/CWorld.hpp"
#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/WheelTraceMgr.hpp"
#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

JPH_SUPPRESS_WARNINGS

// Stages 1-3 (docs/jolt-integration-techanalysis.md, per the staged plan in §5) live in this
// one module - the underlying per-vehicle mechanics (build a JPH::VehicleConstraint mirroring
// a vehicle's prototype geometry, feed it driver input, step it, optionally write the result
// back into the real ODE body) are identical regardless of whether the vehicle is the
// player's or an AI's, so they're factored into a single ShadowState + per-vehicle update
// path rather than duplicated:
//   Stage 1 - shadow-only (read-only, logs divergence) - [jolt_harness] shadow
//   Stage 2 - Jolt actually drives the player's vehicle - [jolt_harness] apply + player_only
//   Stage 3 - extend Jolt-driving to a handful of AI vehicles too - [jolt_harness] ai_count
//             (requires player_only=0 - see Apply() below for the exact gating)
//
// The chassis/wheel geometry fed into Jolt is a deliberate approximation, not a 1:1 port of
// ODE's actual collision (multi-part boxes + spherical wheels via Hinge2 joints) - a single
// box chassis sized from ComplexPhysicObjPrototypeInfo::m_massSize, and WheelSettingsWV per
// wheel using confirmed position/radius/width/driven/steering data but Jolt-default
// suspension axes (assumed chassis-local Y-up/Z-forward - NOT independently confirmed by
// disassembly, see the comment in BuildShadow below). This is intentional: the whole point of
// running a real Jolt simulation alongside ODE is to reveal how much a Jolt-native vehicle
// model diverges given identical inputs, not to be bit-exact.
//
// Suspension spring (frequency/damping) and tire friction (longitudinal/lateral scale) ARE
// tunable, via [jolt_harness] susp_frequency/susp_damping/friction_long/friction_lat - see
// TuningParams/g_activeTuning below - and can additionally be searched automatically by the
// in-process Nelder-Mead autotuner ([jolt_harness] autotune=1, see the AutoTune* section
// near the bottom of this file), per docs/jolt-integration-techanalysis.md §8.5/§16/§20.3.
namespace kraken::fix::joltshadow {
    // Must match kraken::fix::jolt::Layers::MOVING (jolt.cpp) - duplicated here rather than
    // exposing Jolt-typed constants through the intentionally lightweight jolt.hpp.
    static constexpr JPH::ObjectLayer kMovingLayer = 1;

    // Must match kraken::fix::jolt::Layers::WHEEL_QUERY (jolt.cpp, see its comment for why
    // this exists) - a query-only layer restricted to colliding with NON_MOVING (ground)
    // only, used for the wheel-ground raycast tester so it can't hit neighboring vehicles'
    // kinematic mirror bodies (docs §22.11/§23.5).
    static constexpr JPH::ObjectLayer kWheelQueryLayer = 2;

    // Must match kraken::fix::jolt::Layers::WHEEL_PROXY (jolt.cpp, docs §34) - a REAL body
    // layer (unlike WHEEL_QUERY above), also restricted to NON_MOVING only, for the auxiliary
    // wheel-proxy bodies built by BuildWheelProxy below.
    static constexpr JPH::ObjectLayer kWheelProxyLayer = 3;
    // docs §58 (Этап 1, шаг 2): the real wheel BODIES. Must match kraken::fix::jolt::Layers::WHEEL
    // (jolt.cpp) - same duplicate-rather-than-export convention as the three above. Unlike the
    // proxy layer this one reaches MOVING too, because a real wheel has to be able to hit another
    // vehicle; its own chassis and sibling wheels are excluded by the collision GROUP instead.
    static constexpr JPH::ObjectLayer kWheelLayer = 4;

    // docs §110 (Этап 1, шаг 7): counters for the three LEGACY paths step 7 is meant to take out
    // of the hot path - the wheel proxies, and the two per-wheel CollideShape queries.
    //
    // Instrumentation rather than an assertion on purpose. Every one of these sites is guarded by
    // `state.constraint == nullptr`, and mode 4 builds no VehicleConstraint, so reading the code
    // says they are already dead there. That is exactly the kind of claim this project has been
    // wrong about before, and "I read the guard" is not a measurement. These count what actually
    // executed; mode 2 is the POSITIVE CONTROL, because a counter that reads zero for the boring
    // reason that nothing ever increments it would otherwise pass silently.
    // docs §112 (шаг 8): how many shadows were allowed to sleep this frame. Without this the
    // sleep lever's effect is indistinguishable from noise in the frame time - "it got faster"
    // has to be traceable to "N of 75 shadows stopped simulating".
    // docs §114 (шаг 6, проверка 2): the largest world-space AABB shift any kinematic steer
    // assignment has produced this session, and the steer angle it happened at. Zero means the
    // assignment provably moved no collision geometry - see the call site for why that is expected
    // and why it is worth measuring anyway.
    static float g_steerAabbShiftMax = 0.0f;
    static float g_steerAabbShiftAt  = 0.0f;
    // Tracked SEPARATELY from the shift, and that separation is the whole point. The first version
    // only recorded the angle when the shift grew - so a shift that is always exactly zero left the
    // angle at zero as well, and "no geometry moved at full lock" became indistinguishable from
    // "the wheel never turned". A null result has to prove the test actually ran.
    static float g_steerCmdMaxSeen   = 0.0f;
    static uint64_t g_steerAssignments = 0;

    struct SleepStats {
        uint64_t sleptShadows = 0;
        uint64_t awakeShadows = 0;
        // Why a shadow was kept awake. "0% slept" is a dead end without these - it cannot
        // distinguish "the vehicles really are all busy" from "one of my thresholds is wrong",
        // and those call for opposite next moves.
        uint64_t rejThrottle = 0;
        uint64_t rejBrake    = 0;
        uint64_t rejSpeed    = 0;
        uint64_t rejHand     = 0;
        uint64_t rejChanged  = 0;
        uint64_t rejMargin   = 0;   // quiet, but not yet for long enough
    };
    static SleepStats g_sleepStats;

    struct LegacyPathCounters {
        uint64_t proxyBuilds       = 0;   // BuildWheelProxy bodies actually created
        uint64_t proxyPassedGuard  = 0;   // TryBuildWheelProxiesOnceSettled past its constraint check
        uint64_t evalCollideShape  = 0;   // LogWheelModelEval's CollideShape (WHEEL_QUERY layer)
        uint64_t stepCollideShape  = 0;   // StepWheelModel's CollideShape (mode 2's hot path)
    };
    static LegacyPathCounters g_legacyPaths;

    // docs §37 item 3: explicit chassis-vs-own-wheel-proxy collision exclusion, hardening
    // BuildWheelProxy's previously sole safeguard (mLimitsMin keeping the proxy geometrically
    // clear of the chassis shape - docs §34.1's known simplification, never proven watertight
    // in every possible extreme rotation). Subgroup 0 is always the chassis; subgroups
    // 1..kMaxWheelsPerVehicleGroupFilter are wheel slots (wheelIndex+1). Every vehicle shares
    // this SAME table instance but gets its own CollisionGroup::GroupID (see collisionGroupId
    // threaded alongside label through BuildShadow/BuildWheelProxy below) - GroupFilterTable::
    // CanCollide only consults the bit table once both bodies already share a GroupID (see
    // GroupFilterTable.h), so a DIFFERENT vehicle's chassis/proxies (different GroupID) still
    // always collide normally, unaffected by this table - only a vehicle's own chassis-vs-own-
    // proxy pairs are ever suppressed.
    //
    // docs §58 (Этап 1, шаг 2): the inner loop below is NEW and the table is now shared with the
    // real wheel bodies, so the name lost its "proxy". A wheel PROXY only ever braces against
    // terrain (Layers::WHEEL_PROXY reaches NON_MOVING only), so two proxies of one vehicle could
    // never meet and chassis-vs-proxy was the only pair worth suppressing. Real wheel bodies sit
    // on Layers::WHEEL and DO see each other, and at full droop or under a hard steer two wheels
    // of the same vehicle can overlap - so every wheel-vs-sibling-wheel pair has to be disabled
    // too, not just chassis-vs-wheel.
    // docs §58 (Этап 1, шаг 2): a wheel body's Jolt UserData. The contact callback runs on worker
    // threads inside PhysicsSystem::Update with the body mutexes held, so it must identify a wheel
    // with no lookup and no lock at all - a tagged handle read straight off the Body is the only
    // shape that satisfies that. High dword is the literal 'WHL\0' so an untagged body (or one
    // whose UserData some other subsystem owns) can never be mistaken for a wheel; the low dword
    // packs the vehicle slot and the wheel index.
    static constexpr uint64_t kWheelUserDataTag  = 0x57484C00ull << 32; // 'WHL\0'
    static constexpr uint64_t kWheelUserDataMask = 0xFFFFFFFFull << 32;
    static inline uint64_t MakeWheelUserData(uint32_t slot, uint32_t wheelIndex) {
        return kWheelUserDataTag | (uint64_t) ((slot & 0xFFFFu) << 16) | (uint64_t) (wheelIndex & 0xFFFFu);
    }
    static inline bool     IsWheelUserData(uint64_t ud)   { return (ud & kWheelUserDataMask) == kWheelUserDataTag; }
    static inline uint32_t WheelUserDataSlot(uint64_t ud) { return (uint32_t) ((ud >> 16) & 0xFFFFu); }
    static inline uint32_t WheelUserDataIndex(uint64_t ud){ return (uint32_t) (ud & 0xFFFFu); }

    // docs §66: crash fix, confirmed live via a full disassembly of the fault (no PDB for
    // kraken.dll itself, so this was done by hand: pefile+capstone against the deployed DLL,
    // cross-referenced with the crash log's own stack trace and kraken.map). A static prop body's
    // BreakableObject* tag (propTag/ResolveBreakableOwner, jolt.cpp) is resolved ONCE, at level
    // export, and never refreshed - unlike g_aiTargets (docs §Stage-3-Шаг-5's own crash fix,
    // same shape of bug), nothing here re-confirms the tagged object is still the SAME live one
    // before every use. Live crash during combat: `breakable->GetMass()` faulted reading
    // address 0x110 - the vtable slot for GetMass(), meaning `*breakable` (the vtable pointer
    // itself) was 0. The read of `*breakable` at offset 0 did NOT fault, so the pointer was
    // mapped/readable, just pointing at memory that's since been zeroed - a live prop's C++
    // object got invalidated (destroyed, pooled, or reused) while the static Jolt body's stale
    // tag kept pointing at it, most likely something combat-specific (an explosion or gunfire
    // interacting with level geometry) that this session's earlier, controlled ram-tests never
    // exercised.
    //
    // Not a full liveness fix - a proper one would need jolt.cpp's export to track a live set
    // and invalidate entries as objects are destroyed, refreshed each frame the way
    // GetCurrentlyLiveVehicles() does for vehicles. That's the right long-term shape but a
    // bigger change and props can number in the thousands where vehicles number in the tens, so
    // a per-frame full-container rescan from a Jolt WORKER thread (these Handle* functions run
    // from OnContactAdded/OnContactPersisted, not the main thread) isn't a safe drop-in either.
    // This is the minimal, thread-safe, evidence-matched guard: a polymorphic object's first
    // word is its vtable pointer, and reading it is exactly as safe as the crash already proved
    // it is for THIS failure mode (mapped-but-zeroed memory) - it converts that crash into a
    // skip. It will NOT catch every possible corruption (a non-null-but-wrong vtable, or memory
    // that's been fully unmapped rather than zeroed, would still fault) - it catches the one
    // that's actually been observed live.
    static inline bool LooksLikeLiveBreakableObject(const hta::ai::BreakableObject* obj) {
        return obj != nullptr && *reinterpret_cast<void* const*>(const_cast<hta::ai::BreakableObject*>(obj)) != nullptr;
    }

    // docs §59/§63 (Этап 1, шаг 3): the harvest buffer. The narrow phase runs on worker threads
    // and the step listener runs in a different job in the SAME step, so the contact data has to
    // cross that boundary without a lock and without allocating. A fixed per-wheel record array
    // with an atomic counter is the whole mechanism.
    //
    // Double-buffered by step PARITY, not by copying: JobStepListeners runs BEFORE
    // JobFindCollisions within a step, so during step N the listener must read what the narrow
    // phase of step N-1 wrote. Writer uses buf[step & 1], reader uses buf[(step ^ 1) & 1].
    //
    // `count` is deliberately allowed to run past kMaxHarvestRecs: the excess is the overflow
    // figure, and a counter that saturated would hide exactly the condition worth knowing about.
    static constexpr uint32_t kMaxHarvestRecs   = 16;  // per wheel per step; 1-4 expected on flat ground
    static constexpr uint32_t kMaxHarvestWheels = 16;
    static constexpr uint32_t kMaxVehicleSlots  = 129; // player (slot 0) + AI shadows

    struct WheelHarvestRec {
        JPH::Vec3 point;   // world-space contact point on the SURFACE side
        JPH::Vec3 normal;  // unit, pointing OUT of the surface toward the wheel
        float     depth;   // manifold penetration depth - diagnostics only; the band reprojects
        uint32_t  sub;     // 0 = TYRE (radius R), 1 = RIM (radius R - tau)
        // docs §124 step 3: the body on the SURFACE side of this contact (terrain, a prop,
        // another vehicle's chassis - whatever the wheel is touching). Resolved fresh by BodyID
        // each step in WheelContactConstraint::SetupVelocityConstraint rather than kept as a
        // Body* here, because it can be removed/replaced between harvest (this step's narrow
        // phase, a worker thread) and use (next step's Setup) - BodyID equality-checks the
        // occupant's generation on lookup (BodyManager::TryGetBody), a raw pointer would not.
        JPH::BodyID other;
    };
    struct WheelHarvest {
        std::atomic<uint32_t> count;
        WheelHarvestRec       rec[kMaxHarvestRecs];
    };
    static WheelHarvest g_wheelHarvest[2][kMaxVehicleSlots][kMaxHarvestWheels];

    // Advanced exactly once per physics step, on the MAIN thread, immediately before Update().
    // Never inside a listener: there is one listener per vehicle, so per-listener flipping would
    // advance a shared counter once per vehicle per step instead of once per step.
    static std::atomic<uint32_t> g_harvestStep{0};

    static constexpr uint32_t kMaxWheelsPerVehicleGroupFilter = 16; // generous upper bound - largest real vehicle seen so far (6-wheel truck) is well under this
    static JPH::GroupFilterTable* g_wheelGroupFilter = nullptr; // built once, shared/leaked forever across every rebuild - same convention as g_collisionTester below
    static JPH::GroupFilterTable* GetWheelGroupFilter() {
        if (g_wheelGroupFilter == nullptr) {
            g_wheelGroupFilter = new JPH::GroupFilterTable(kMaxWheelsPerVehicleGroupFilter + 1);
            for (uint32_t i = 1; i <= kMaxWheelsPerVehicleGroupFilter; ++i) {
                g_wheelGroupFilter->DisableCollision(0, i); // chassis (subgroup 0) vs each wheel slot
                for (uint32_t j = i + 1; j <= kMaxWheelsPerVehicleGroupFilter; ++j)
                    g_wheelGroupFilter->DisableCollision(i, j); // wheel vs sibling wheel
            }
        }
        return g_wheelGroupFilter;
    }

    // docs §107: the collision filter for VARIANT shadows - several shadows of the same vehicle
    // running in one pass with different parameters.
    //
    // They spawn at the same pose, so every pair among them has to be suppressed, chassis included.
    // The ordinary wheel filter cannot do it: it disables chassis-vs-wheel and wheel-vs-sibling,
    // but Jolt's GroupFilterTable asserts on DisableCollision(i, i), so two chassis bodies sharing
    // subgroup 0 would still collide. Hence a table where EVERY pair is disabled and each family
    // member gets its own subgroup block - then sharing one GroupID makes the whole family
    // mutually transparent while each still collides with the static world, which is the thing
    // being compared.
    static constexpr uint32_t kMaxVariantShadows    = 4;   // + the player shadow itself
    static constexpr uint32_t kVariantSubGroupStride = kMaxWheelsPerVehicleGroupFilter + 1;
    static constexpr uint32_t kVariantGroupId       = 0;   // the player shadow's own group - it joins the family
    static JPH::GroupFilterTable* g_variantGroupFilter = nullptr;
    static JPH::GroupFilterTable* GetVariantGroupFilter() {
        if (g_variantGroupFilter == nullptr) {
            const uint32_t n = (kMaxVariantShadows + 1) * kVariantSubGroupStride;
            g_variantGroupFilter = new JPH::GroupFilterTable(n);
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t j = i + 1; j < n; ++j)
                    g_variantGroupFilter->DisableCollision(i, j);
        }
        return g_variantGroupFilter;
    }

    // Not in extern/hta's ode.hpp yet - declared locally rather than editing that submodule.
    // Confirmed via disassembly (docs §22.3): counts the body's dxJointNode linked list at
    // [body+0x14]/[node+8], i.e. exactly ODE's real, live joint count - not cached anywhere.
    static const auto dBodyGetNumJoints = (int (__fastcall*)(dxBody*))(0x007C4B90);

    // docs §100: hta::ai::Wheel::STEERING_LIMIT IS RECOVERED, and it is pi/4 = 45 deg.
    // The PDB places it at rva 0x5931f4, and reading that address gives 0x3f490fdb =
    // 0.7853981852531433. It is not folded into immediates as the note below assumed - it has
    // real addressable storage, and the player-control path loads it straight from there:
    // CMiracle3d::Controls (game.cpp:305-309) calls ai::Vehicle::SetSteer with +STEERING_LIMIT,
    // -STEERING_LIMIT or 0. So the player's steering command is a three-way +-45 deg, never
    // anything between.
    static constexpr float kWheelSteeringLimitRadians = 0.7853982f;   // = pi/4, read from 0x5931f4

    // docs §105: the reference's MECHANICAL stop on the steering axis, which is NOT the command
    // limit above. §94.2 reports ODE's Hinge2 axis-1 LoStop/HiStop as +-pi for steered wheels and
    // 0 for unsteered ones - i.e. a steered wheel is mechanically free through half a turn each
    // way and is held at its angle by something else entirely (see below). A hair under pi so the
    // swing limit cannot degenerate at exactly half a turn.
    static constexpr float kOdeSteerStopRadians = 3.1415927f - 0.01f;

    // docs §105: how the reference actually holds the steer angle - and it is not a torque.
    // ai::Vehicle::_KeepSteer (RVA 0x1da940) never touches the joint: its only calls are
    // _AdjustWheel and _TurnWheelByAngle, and _TurnWheelByAngle ends at vtable slot +0x118 =
    // PhysicObj::SetRotation. The angle is ASSIGNED to the wheel body every frame. The joint's
    // only motor is on axis 2: _KeepGearBox writes dParamVel2 (0x102) and dParamFMax2 (0x103) at
    // 0x1e1259/0x1e126f, and nothing anywhere writes an axis-1 motor parameter.
    //
    // That is why §104's "give the kingpin a lever arm" instinct was the wrong lead: the reference
    // never enters a torque contest over the steer angle, so no scrub radius or caster trail would
    // have changed the outcome. A Position motor with a finite cap is a different mechanism, and
    // contact torque beats it whenever the load is high enough - which the §103 bundle guaranteed
    // by raising the load.
    //
    // The dynamic stand-in for "kinematic" is a motor strong enough never to be the binding
    // constraint. It is expressed as a multiple of the disturbance actually measured (8591 Nm at
    // 64% downforce), not tuned by eye, and the §68 `sat` field is the check: if sat stops pinning
    // at 1.00, the motor is no longer losing.
    static constexpr float kSteerKinematicCapNm = 200000.0f;   // >20x the measured 8591 Nm peak

    // SUPERSEDED but deliberately still here. Everything the comment below says was wrong: the
    // value exists and is 45 deg, not 35. It stays only because mode 2 and the VehicleConstraint
    // path use it, and mode 2 is Stage 1's rollback path on every step - silently changing what
    // the fallback does would remove the thing the rollback is for. Mode 4 uses the real constant
    // above. Migrating these call sites is its own change, with its own measurement.
    //
    //   (original note) ... has no backing definition anywhere in extern/hta ... so its value
    //   hasn't been recovered. Use a plausible generic max steer angle instead ...
    static constexpr float kApproxMaxSteerAngleRadians = 35.0f * (3.14159265f / 180.0f);

    // "Body is flying" gate (docs/jolt-integration-techanalysis.md Stage 2 section, ported
    // from the wheelmodel branch's own safety-rail precedent, source/fix/wheelmodel.cpp on
    // git branch spring_wheel) - if a Jolt chassis' own linear speed exceeds this, or any
    // component of its position/rotation/velocity is non-finite, ApplyJoltToVehicle skips
    // writing this frame entirely and leaves ODE in sole control - self-healing per frame,
    // no persistent "disabled" state, matching wheelmodel's own pattern exactly.
    //
    // Was a hardcoded 60.0 constexpr; docs/stage2-plan.md §2.5's cross-vehicle sweep found
    // ArcadeScout01 legitimately clearing it under full throttle, so it's now
    // config.jolt_wm4_max_speed_mps (kraken.ini [jolt_harness] wm4_max_speed_mps, default 100).
    static float MaxAppliedSpeedMps() {
        return kraken::Config::Instance().jolt_wm4_max_speed_mps.value;
    }

    static bool AllFinite(std::initializer_list<float> values) {
        for (float v : values)
            if (!(v == v) || v > 1e18f || v < -1e18f) // NaN != itself; catch Inf too
                return false;
        return true;
    }

    // Suspension/friction tuning (docs/jolt-integration-techanalysis.md §8.5/§16) - the four
    // parameters BuildShadow was previously leaving at Jolt's own hardcoded defaults (see the
    // namespace-level comment above). Held as a plain in-memory struct rather than read directly
    // from Config each build, because the in-process autotuner (further below) needs to swap
    // these between trials without a kraken.ini round-trip (Config is loaded once at startup,
    // there is no hot-reload in this codebase).
    struct TuningParams {
        // docs §31: NOT a direct Hz value or ratio despite the field name (kept from §27 to
        // avoid config/autotuner churn a third time) - BuildShadow now computes REAL ODE-
        // derived stiffness (N/m) and damping (N*s/m) per wheel from WheelPrototypeInfo's own
        // CFM/ERP, and these two fields are dimensionless residual MULTIPLIERS on top of that
        // (1.0 = trust ODE's real values as-is). See BuildShadow's wheel loop for the derivation.
        float suspensionFrequency; // multiplies the derived stiffness (JPH::SpringSettings::mStiffness, N/m)
        float suspensionDamping;   // multiplies the derived damping (JPH::SpringSettings::mDamping, N*s/m)
        float frictionLongScale;   // uniform multiplier on WheelSettingsWV's default longitudinal friction curve
        float frictionLatScale;    // uniform multiplier on WheelSettingsWV's default lateral friction curve
    };

    static TuningParams DefaultTuningParams() {
        const kraken::Config& config = kraken::Config::Instance();
        return TuningParams{
            config.jolt_susp_frequency.value,
            config.jolt_susp_damping.value,
            config.jolt_friction_long.value,
            config.jolt_friction_lat.value,
        };
    }

    // Live tuning state - starts at Jolt's own literal defaults (matches WheelSettings::
    // mSuspensionSpring{1.5f,0.5f} and WheelSettingsWV's friction curves at scale 1.0) until
    // Apply() overwrites it from kraken.ini; g_tuningGeneration is bumped on every change so
    // BuildShadow's callers know to rebuild (see UpdateOneVehiclePreStep).
    static TuningParams g_activeTuning     = {1.5f, 0.5f, 1.0f, 1.0f};
    static uint32_t     g_tuningGeneration = 0;

    static void SetTuningOverride(const TuningParams& params) {
        g_activeTuning = params;
        ++g_tuningGeneration;
    }

    // Per-vehicle Jolt state - one instance for the player, plus one per Stage 3 AI vehicle.
    // docs §57 (Этап 1, шаг 1): the per-wheel settings, extracted out of JPH::WheelSettingsWV into
    // a plain POD this file owns. Step 1's whole contract is ZERO behaviour change: nothing reads
    // this yet, it is filled from the SAME locals the WheelSettingsWV assignments use so the two
    // cannot drift, and the VehicleConstraint keeps driving everything exactly as before. Step 2
    // builds real wheel bodies off this array, and plan §5 item 19 deletes WheelSettingsWV once
    // nothing needs it.
    //
    // The field set is deliberately NOT the plan's list. The plan (§5 item 19) names
    // `float R, width, minLen, maxLen, kSusp, cSusp, mU, mass, inertia`, but the shipped Stage 1
    // build carried no `inertia` here - it lived in two separate per-wheel float vectors - and it
    // DID carry `tau`, the tyre band half-thickness, which the plan does not mention. Both facts
    // come from the recovered layout (docs/recovered/SPEC.md, steps 1-2 cluster: the 112-byte
    // stride is proven by the compiler's divide-by-112 idiom at two independent call sites). This
    // is the plan being superseded by what actually shipped, not a liberty.
    //
    // tau and the two sign fields are left at their defaults here on purpose: they are WRITTEN by
    // the step-2 body build and the step-5/6 spin and steer work respectively, not by extraction.
    // A field this struct cannot yet fill honestly is better left obviously unset than filled with
    // a plausible value that later code silently trusts.
    struct WheelSetup {
        JPH::Vec3 attachPos;                 // chassis-local suspension attachment (= WheelRuntimeInfo::m_initialPos)
        JPH::Vec3 suspDir   {0.0f, -1.0f, 0.0f}; // chassis-local suspension travel direction ("down")
        JPH::Vec3 steerAxis {0.0f,  1.0f, 0.0f}; // chassis-local steering axis ("up")
        JPH::Vec3 axleLocal {1.0f,  0.0f, 0.0f}; // chassis-local axle = wheelUp x wheelForward at zero steer

        float radius   = 0.3f;   // hta::ai::Wheel::GetRadius()
        float width    = 0.2f;   // hta::ai::Wheel::GetWidth()
        float minLen   = 0.05f;  // INVENTED, not data - see the comment at the assignment below
        float maxLen   = 0.25f;  // minLen + WheelPrototypeInfo::m_suspensionRange
        float kSusp    = 0.0f;   // N/m,   docs §31: derived from the real ODE CFM/ERP
        float cSusp    = 0.0f;   // N*s/m, same derivation
        float tau      = 0.0f;   // tyre band half-thickness (m) - written by the step-2 body build
        float unsprungMass = 0.0f; // the wheel's own mass, hta::ai::Wheel::GetMass()

        // Both derived rather than assumed, at steps 5 and 6. +-1. Zero here means "not yet
        // derived", which is a value neither step can produce, so it cannot be mistaken for one.
        float spinSign  = 0.0f;
        float steerSign = 0.0f;

        bool driven   = false;
        bool steering = false;

        // docs §133 (task #58, wheel-spin visual bug): the native reference render path
        // (ai::PhysicBody::TransferPhysicParamsToSceneGraphNode, disassembled via tools/lora
        // against game.pdb) never uses a global left/right mirror at all - it composes the
        // physics body's raw world rotation with THIS per-object, per-geom offset
        // (ai::PhysicBody::GetNodeRelativeRotation(), itself Inverse(bodyGeomRefRotation) *
        // geom->GetRotation()) before writing to the SgNode. Captured once per wheel, at build
        // time - it is a static attachment-time property (ODE geom placement relative to its
        // owning body), not something that changes per frame.
        JPH::Quat nodeRelativeRotation = JPH::Quat::sIdentity();

        // docs §136 (task #58, wheel-spin visual bug - real fix, not the rejected §65.8
        // per-vehicle-name list): the wheel BODY is spawned at chassisRot (comment at its
        // construction explains why - the constraint frame math wants wheel-local==chassis-local
        // at t=0), discarding the wheel's own NATIVE rotation - which is exactly the
        // artist-authored placement (including any mesh mirroring) that native ODE's geom starts
        // from and simply integrates a side-agnostic world-space spin on top of (confirmed this
        // session: dJointSetHinge2Axis1/2 and dParamVel2 are BOTH computed purely from chassis
        // rotation, zero per-wheel branching - ODE needs no correction because its STARTING POSE
        // already carries the mirror; nothing about its motion does).
        //
        // Captured once at body-build time as `Inverse(chassisRot) * wheel->GetRotation()` -
        // measured live: exactly identity (0 deg) on all 4 of Fighter01's wheels, exactly 180 deg
        // on Molokovoz01's right wheels only. Composing it back on the JOLT side (visualRot =
        // jointBodyRot * visualMirrorDelta) reconstructs precisely the native quaternion, because
        // both engines apply the identical world-space spin - see this field's use in
        // ApplyJoltToVehicleWheelModel for the derivation. Touches ONLY the cosmetic writeback;
        // the wheel body's own rotation (used everywhere else - constraint frames, axleLocal
        // projection, the tyre model) is completely unchanged.
        JPH::Quat visualMirrorDelta = JPH::Quat::sIdentity();
    };

    struct ShadowState {
        hta::ai::Vehicle*       vehicle    = nullptr; // vehicle this state was last successfully built for
        JPH::BodyID             bodyId;
        JPH::VehicleConstraint* constraint = nullptr;
        std::vector<hta::ai::Wheel*> wheelOrder; // parallel to constraint's internal wheel array, index-for-index
        std::vector<WheelSetup> wheelSetup;      // docs §57: parallel to wheelOrder, rebuilt in the same loop

        // docs §58 (Этап 1, шаг 2): the real wheel bodies and their struts. Parallel to
        // wheelOrder. Empty unless [jolt_harness] wheelmodel==4 - wheelBodyMode records that,
        // separately from wheelModelMode, because at step 2 BOTH are true: the bodies exist and
        // are constrained, while the chassis is still driven by the old StepWheelModel force
        // path. They only diverge at step 4, when the forces move onto these bodies.
        bool                            wheelBodyMode = false;
        std::vector<JPH::BodyID>        wheelBodies;
        std::vector<JPH::Constraint*>   wheelConstraints;
        // docs §124 step 1: the wheel's ground-contact constraint, parallel to wheelOrder/
        // wheelConstraints. Starts life as a pure no-op stub (empty Setup/Solve) - see
        // WheelContactConstraint. Only meaningful when jolt_wm4_contact_constraint != 0; empty
        // otherwise, same as the rest of this file's convention for gated features.
        std::vector<JPH::Constraint*>   wheelContactConstraints;
        // The as-built disc inertia, kept so the step-5 spin work and the reflected-drivetrain
        // term can reuse it per gear instead of re-deriving it from mass and radius each time.
        // Ixx is about the axle, Ir about the two radial axes.
        std::vector<float>              wmDiscInertiaX;
        std::vector<float>              wmDiscInertiaR;
        // Fix for the steer-drift bug reported live (kraken.ini wm4_steer_kinematic=2): the
        // steer-assignment code used to extract "twist" (pure spin, to preserve) via
        // wheelLocal.GetSwingTwist() off the WHEEL BODY'S CURRENT orientation every frame - but
        // that orientation is itself disturbed each frame by real physics (gyroscopic coupling
        // at high spin rate, lateral tyre force reacting through the SixDOF) between one
        // assignment and the next, and swing-twist decomposition has no way to tell "real spin"
        // apart from "disturbance that crept in since the last assignment" - so the disturbance
        // got preserved as if it were spin instead of corrected. Confirmed live: holding full
        // lock for ~10s while accelerating 10->20 m/s made measured decay from the commanded
        // angle down through zero and out the other side. Tracking spin as its OWN accumulated
        // scalar (integrated from angular velocity, never read back off the body) makes the
        // steer and spin components fully independent - disturbance can still perturb the body
        // between frames, but the NEXT assignment rebuilds strictly from steerCmd + this
        // accumulator, so nothing it doesn't explicitly integrate can leak in as "spin".
        std::vector<float>              wmSpinAngle;

        // docs §67 (Этап 1, шаг 5): what the main thread COMMANDED the spin motors this frame,
        // kept so the post-step pass can print it beside what the solver actually delivered.
        // Snapshotted rather than re-read: the log line's "| PRE-STEP" is a contract that brake
        // and handBrake are the values seen BEFORE PhysicsSystem::Update, and re-reading them
        // after would silently invalidate every trace parsed on that assumption.
        struct SpinCmd {
            float target = 0.0f;   // commanded angular velocity in constraint space, SIGNED
            float limit  = 0.0f;   // commanded mMaxTorqueLimit, a magnitude
            float cap    = 0.0f;   // the contact torque cap this wheel was given
            float brakeT = 0.0f;   // commanded SetMaxFriction(RotationX)
            // docs §68 (шаг 6). steerCap is deliberately the SAME scalar as cap: the one thing the
            // recovered log actually supports is a single pair (cap=3020 and cmdTq=3020 on wheels
            // 0/1 in one frame) - the stronger "3049 on wheel 2" claim was refuted, wheel 2 is
            // unsteered and printed FLT_MAX. So they start shared because that is the evidence,
            // and they get split only if a measurement demands it.
            float steerTarget = 0.0f;  // commanded angle in CONSTRAINT space (sign already applied)
            float steerCap    = 0.0f;
            bool  steerable   = false;
        };
        std::vector<SpinCmd>            wmSpinCmd;
        // The dt the commands were issued against. The post-step pass turns an accumulated lambda
        // into a torque by dividing by it, and reaching for a fresh frame time there would divide
        // this step's impulse by the next step's duration.
        float                           lastStepDt      = 1.0f / 60.0f;
        float                           wmDriveTorque   = 0.0f;  // engine torque at the wheels, before the split
        float                           wmPerWheel      = 0.0f;  // driveTorque / nDriven
        uint32_t                        wmDrivenCount   = 0;
        float                           wmSpinThrottle  = 0.0f;
        float                           wmSpinBrake     = 0.0f;  // PRE-STEP snapshot
        bool                            wmSpinHandBrake = false; // PRE-STEP snapshot
        // docs §137.2 (task #60, Molokovoz01 standstill creep, real-terrain follow-up): latches
        // once the standstill snap first catches the vehicle genuinely stopped - see the snap
        // block's own comment in UpdateOneVehiclePostStep for why a one-shot "speed already below
        // threshold" gate does not hold on real (sloped) terrain.
        bool                            restHeld = false;
        // docs §137.3 (same task, ordering fix): the wheel orientation each wheel body had at the
        // instant restHeld first latched - see UpdateOneVehiclePostStep's own comment for why
        // zeroing velocity alone cannot stop the VISIBLE creep (ApplyJoltToVehicleWheelModel reads
        // the body's rotation and writes it to the mesh BEFORE the velocity snap ever runs, so a
        // one-frame disturbance is already on screen by the time velocity gets corrected).
        std::vector<JPH::Quat>         wheelLockedRot;
        // docs §137.4 (task #60, temporary diagnostic): previous frame's WRITTEN visual rotation,
        // per wheel - see its own comment at the ApplyJoltToVehicleWheelModel call site.
        std::vector<JPH::Quat>         dbgPrevVisualRot;
        // docs §101: what the ported arcade assists actually applied this frame, for the log.
        float                           wmAssistDownForce = 0.0f;
        float                           wmAssistTorque    = 0.0f;   // APPLIED (0 when §109 gates it off)
        // docs §109: what the yaw assist WOULD have applied, computed even on the off arm. An A/B
        // that only prints the applied value cannot tell "the gate worked" from "the term was zero
        // anyway", and this term is zero whenever the driver is not steering - which is most of
        // every scenario run so far, and exactly why it went unmeasured for so long.
        float                           wmAssistTorqueRaw = 0.0f;
        float                           wmAssistHorizVel  = 0.0f;
        // docs §102: the speed governor's state, for the log.
        float                           wmSpeedLimit   = 0.0f;
        float                           wmSurfaceSpeed = 0.0f;
        bool                            wmGoverned     = false;
        // docs §103: soil rolling drag, summed magnitude over the wheels that got it.
        float                           wmSoilDragN      = 0.0f;
        float                           wmSoilResistance = 0.0f;
        // docs §122: body damping. Read BACK off the bodies rather than echoed from the config, so
        // the off arm reports the as-built values it declined to change instead of printing the
        // values it would have written - the §109 discipline, for the same reason.
        float                           wmDampLinear     = 0.0f;
        float                           wmDampAngular    = 0.0f;
        uint32_t                        wmDampBodies     = 0;
        // docs §59: the parallel harvest listener for this vehicle. Registered on first build and
        // then re-pointed rather than re-registered - see BuildWheelBodies for why the OLD one is
        // deliberately left registered on a rebuild instead of being removed.
        class VehicleStepListener* stepListener = nullptr;
        std::vector<uint32_t>   wheelSourceIndex; // parallel to wheelOrder: the vehicle->m_wheels[] index each entry was captured from at build time - lets ShadowWheelsStillPresent detect a wheel that's since been detached/replaced (see its comment)
        uint64_t                frameCounter    = 0;
        uint32_t                builtGeneration = 0; // g_tuningGeneration at the time this state's shadow was (re)built

        // Docs §22.3 ramming diagnostic (see ApplyJoltToVehicle). dBodyGetNumJoints() counts
        // EVERY joint on the body, including its own permanent per-wheel Hinge2 suspension
        // joints - not just transient ODE contact joints - so a raw >0 check is useless (it's
        // never 0 for a normal vehicle). -1 means "not yet captured"; BuildShadow resets it so
        // the first post-rebuild ApplyJoltToVehicle call re-captures the structural baseline.
        int  chassisBaselineJointCount = -1;
        bool chassisHadExtraJointLastFrame = false; // rising-edge tracker vs. the baseline above

        // Same idea, per wheel (parallel to wheelOrder) - wheels touch the ground EVERY frame,
        // so this is expected to fire immediately if wheel-ground contact goes through the same
        // ai::NearCallback/dJointCreateContact pipeline as chassis ramming (docs §7's still-open
        // question: does CollideWheelAndLandscape/CollideWheelAndAsphalt use a separate path?).
        std::vector<int>  wheelBaselineJointCount;
        std::vector<bool> wheelHadExtraJointLastFrame;

        // docs §38.9: wheel-proxy construction is deferred until the chassis has actually
        // settled (see TryBuildWheelProxiesOnceSettled) rather than happening synchronously in
        // BuildShadow - false whenever a (re)build just happened, so the deferred check knows to
        // run again for the new chassis. collisionGroupId is stashed here so the deferred check
        // (which runs well after BuildShadow returns) still has it on hand. consecutiveSlowFrames
        // counts how many ticks in a row the chassis has read below the settled-speed threshold -
        // live-measured that a single-frame check is noisy enough (a big vehicle rocking on
        // impact can dip under the threshold for one tick, then spike back up) to occasionally
        // call it "settled" mid-tumble, so several ticks in a row are required instead.
        bool     wheelProxiesBuilt      = false;
        uint32_t collisionGroupId       = 0;
        // docs §107: the harvest buffer index, DECOUPLED from collisionGroupId. They are the same
        // number for every shadow today, and the split exists for variant shadows - several
        // shadows of the SAME vehicle, run in one pass with different parameters so one game
        // launch answers what used to take one launch per arm.
        //
        // Variant shadows spawn at the SAME pose, so they would interpenetrate. Giving them a
        // SHARED collisionGroupId makes the group filter disable every pair among them (each still
        // collides with the static world, which is what is being compared). But the harvest buffer
        // is per-vehicle state and must stay private, so it cannot be keyed on that shared id.
        // Hence two fields. Anything that identifies "which vehicle's buffer" uses harvestSlot;
        // anything about "who may collide with whom" uses collisionGroupId.
        uint32_t harvestSlot           = 0;

        // docs §107: per-shadow parameter overrides. When `set` is false every accessor below
        // falls through to the global config, so a shadow that is not a variant behaves exactly as
        // before - which is what makes the whole feature inert until a wm4_variant_N key exists.
        struct Variant {
            bool     set          = false;
            uint32_t spin         = 1;
            uint32_t steer        = 0;
            uint32_t steerMode    = 2;
            uint32_t assists      = 1;
            uint32_t assistYaw    = 1;   // docs §109: sub-gate, yaw torque only
            uint32_t engineBrake  = 0;   // docs §120: off by default, the magnitude is not recovered
            float    engineBrakeScale = 1.0f;
            uint32_t governor     = 1;
            uint32_t soildrag     = 1;
            uint32_t bodyDamping  = 0;   // docs §122: off by default, same as the global lever
            float    dampingLinear  = 0.1f;
            float    dampingAngular = 0.3f;
            float    steerHz      = 20.0f;
            float    steerDamping = 1.0f;
            uint32_t familyIndex  = 0;   // 0 = the player shadow, 1..N = variants
            // docs §107.5: diagnostic lever. 1 means this variant does NOT join the shared
            // collision family and takes its own GroupID like an AI shadow instead. Isolated
            // variants then physically collide with each other, which is useless for measuring -
            // its only job is to split the remaining hypothesis space: if the PLAYER's divergence
            // comes back with the family dissolved, the cause is its group change; if not, the
            // cause is something shared between shadows of one vehicle.
            uint32_t isolate      = 0;
        };
        Variant  var;
        // True once any wm4_variant_N line exists - including for the PLAYER shadow, whose var is
        // never `set` but which still has to join the family's collision group so the variants do
        // not collide with it. Filled by the update loop, which is the only place that knows.
        bool     variantFamily = false;
        bool VariantFamilyActive() const { return variantFamily; }

        // Read through these, never through the config directly, anywhere the mode-4 paths care.
        uint32_t VarSpin()      const { return var.set ? var.spin      : kraken::Config::Instance().jolt_wm4_spin.value; }
        uint32_t VarSteer()     const { return var.set ? var.steer     : kraken::Config::Instance().jolt_wm4_steer.value; }
        uint32_t VarSteerMode() const { return var.set ? var.steerMode : kraken::Config::Instance().jolt_wm4_steer_kinematic.value; }
        uint32_t VarAssists()   const { return var.set ? var.assists   : kraken::Config::Instance().jolt_wm4_assists.value; }
        uint32_t VarAssistYaw() const { return var.set ? var.assistYaw : kraken::Config::Instance().jolt_wm4_assist_yaw.value; }
        uint32_t VarEngineBrake() const { return var.set ? var.engineBrake : kraken::Config::Instance().jolt_wm4_engine_brake.value; }
        float VarEngineBrakeScale() const { return var.set ? var.engineBrakeScale : kraken::Config::Instance().jolt_wm4_engine_brake_scale.value; }
        uint32_t VarGovernor()  const { return var.set ? var.governor  : kraken::Config::Instance().jolt_wm4_governor.value; }
        uint32_t VarSoilDrag()  const { return var.set ? var.soildrag  : kraken::Config::Instance().jolt_wm4_soildrag.value; }
        uint32_t VarBodyDamping()   const { return var.set ? var.bodyDamping    : kraken::Config::Instance().jolt_body_damping.value; }
        float    VarDampingLinear() const { return var.set ? var.dampingLinear  : kraken::Config::Instance().jolt_damping_linear.value; }
        float    VarDampingAngular()const { return var.set ? var.dampingAngular : kraken::Config::Instance().jolt_damping_angular.value; }
        float    VarSteerHz()   const { return var.set ? var.steerHz   : kraken::Config::Instance().jolt_wm4_steer_hz.value; }
        float    VarSteerDamp() const { return var.set ? var.steerDamping : kraken::Config::Instance().jolt_wm4_steer_damping.value; }
        uint32_t consecutiveSlowFrames  = 0;
        // docs §112 (шаг 8): the wake condition's memory. Reference-side inputs as of last frame,
        // plus how long this shadow has been quiet.
        float    lastWakeThrottle  = 0.0f;
        float    lastWakeBrake     = 0.0f;
        bool     lastWakeHandBrake = false;
        uint32_t quietFrames       = 0;

        // docs §39: per-wheel state for the wheelmodel apply path (parallel to wheelOrder).
        // suspLen/suspVel are the explicit suspension-travel DOF (spring_wheel got travel from
        // ODE's Hinge2; the Jolt port has no wheel body so integrates it here). omega is the §5
        // spin DOF. Only populated/used when [jolt_harness] wheelmodel==2 (apply). wheelModelMode
        // records that this state was built WITHOUT a live VehicleConstraint (see BuildShadow).
        bool               wheelModelMode = false;
        std::vector<float> wmSuspLen;   // current suspension extension along mSuspensionDirection (m)
        std::vector<float> wmSuspVel;   // its rate (m/s)
        std::vector<float> wmOmega;     // wheel spin about the axle (rad/s)
        std::vector<float> wmRestLen;   // per-wheel spring zero-force length (= raycast-init length, so comp=0 at spawn -> no launch)
        std::vector<int32_t> wmBottomedFrames; // docs §44: consecutive frames comp has been pinned at compMax (0 = not bottomed)
        std::vector<float>   wmBottomedImpulse; // docs §49: cumulative hardStopForce*dt delivered this bottoming-out streak (0 = not bottomed)
        // docs §41: vehicle-level (not per-wheel) gearbox state - own_spin's drive torque is now
        // generated by a real, self-contained multi-gear model calibrated from this vehicle's OWN
        // real drivetrain data (hta::ai::Vehicle::GEAR_RATIOS, m_diffRatio, GetMaxTorque(), shift
        // RPM limits, m_maxEngineRpm) instead of a flat constant + linear falloff - see
        // StepWheelModelGearbox for the derivation.
        int32_t            wmGear      = 0;    // index into GEAR_RATIOS[5], matches real m_currentGear's range
        float              wmEngineRpm = 0.0f; // this model's OWN estimate, same units as the real GetEngineRpm()
    };

    // docs §39: gather wheelmodel_core params from config into the plain-float WMParams the
    // engine-agnostic core expects. Read fresh each step so live kraken.ini edits retune without
    // a rebuild (the whole point of exposing them).
    static kraken::fix::wheelmodel::WMParams WheelModelParamsFromConfig() {
        const kraken::Config& c = kraken::Config::Instance();
        kraken::fix::wheelmodel::WMParams p;
        p.k_t         = c.jolt_wm_tyre_stiffness.value;
        p.zeta_t      = c.jolt_wm_tyre_damping.value;
        p.lambda      = c.jolt_wm_hard_core_lambda.value;
        p.mu          = c.jolt_wm_grip.value;
        p.B           = c.jolt_wm_pac_B.value;
        p.C           = c.jolt_wm_pac_C.value;
        p.E           = c.jolt_wm_pac_E.value;
        p.eps         = c.jolt_wm_slip_floor.value;
        p.stick_speed = c.jolt_wm_stick_speed.value;
        p.inertia     = c.jolt_wm_wheel_inertia.value;
        p.rollingResist = c.jolt_wm_rolling_resist.value;
        return p;
    }

    static ShadowState              g_playerShadow;
    static std::vector<hta::ai::Vehicle*> g_aiTargets;  // which vehicle each g_aiShadows[i] tracks (fixed at selection time)
    static std::vector<ShadowState> g_aiShadows;
    static bool                     g_aiShadowsInitialized = false;

    // "Other vehicles" mirroring (docs §22.6/§22.11) - every vehicle that ISN'T the player's own
    // shadow target or a fully Jolt-shadowed AI target (g_aiTargets) gets a lightweight kinematic
    // box body in Jolt, position/rotation-synced from its real ODE state every frame, so a
    // Jolt-driven vehicle collides with it instead of ghosting through (the dynamic-geom half of
    // the ghost-passthrough bug that fix::jolt's static-obstacle export deliberately excludes).
    // Re-scanned fresh every frame rather than cached, specifically so a vehicle destroyed since
    // last frame is dropped by simply not appearing in this frame's scan - never by dereferencing
    // a stale Vehicle* (same use-after-free caution as the wheel-teardown fix, docs §22.2).
    struct VehicleMirrorEntry {
        hta::ai::Vehicle* vehicle = nullptr; // used only for pointer-identity comparison, never dereferenced across frames without a fresh scan confirming it's still live
        JPH::BodyID       bodyId;
    };
    static std::vector<VehicleMirrorEntry> g_vehicleMirrors;

    static JPH::VehicleCollisionTester* g_collisionTester = nullptr; // built once, shared/leaked forever across every rebuild
    static uint32_t                     g_shadowGeneration = 0;      // how many shadow vehicles have been built this process, for logging only

    static hta::ai::Vehicle* GetPlayerVehicle() {
        hta::ai::DynamicScene* scene = hta::ai::DynamicScene::Instance();
        if (scene == nullptr)
            return nullptr;
        return scene->GetVehicleControlledByPlayer();
    }

    // docs §23.10: assembles the chassis's REAL multi-part collision geometry (Chassis/Cabin/
    // Basket/etc. boxes+spheres+capsules, via ComplexPhysicObj::m_vehicleParts ->
    // PhysicBody::GetNumGeoms/GetGeom -> ai::Geom::GetGeomId()'s raw dxGeom*) into a single
    // Jolt compound shape, instead of the single bounding box used since Stage 1. Box/sphere/
    // capsule extraction mirrors fix::jolt's WalkSpaceForStaticExport (same ODE accessor
    // calls, same capsule Z(ODE)->Y(Jolt) axis remap) - duplicated rather than shared across
    // translation units given this session's time budget, not because the logic differs.
    // Trimesh parts are deliberately NOT supported (skipped + logged if seen): unlike static
    // terrain, vehicle collision parts are performance-critical and, in every prototype seen
    // so far, simple shapes - and a trimesh part would need its world-space vertices (that's
    // what dGeomTriMeshGetTriangle returns) converted into chassis-local space, which adds
    // real complexity for a case that may never occur.
    // Returns null (caller falls back to the existing single-box shape) if no usable part
    // geoms are found at all - never leaves the vehicle without SOME collision shape.
    static JPH::RefConst<JPH::Shape> BuildChassisCompoundShape(hta::ai::Vehicle* vehicle,
            const hta::CVector& chassisPos, const hta::Quaternion& chassisRot) {
        JPH::Ref<JPH::StaticCompoundShapeSettings> compound = new JPH::StaticCompoundShapeSettings();
        int32_t partCount = 0, geomCount = 0, boxCount = 0, sphereCount = 0, capsuleCount = 0, skippedCount = 0;

        const hta::Quaternion invChassisRot = chassisRot.Conjugate();

        for (const auto& [partName, part] : vehicle->m_vehicleParts) {
            if (part == nullptr)
                continue;
            ++partCount;

            const uint32_t numGeoms = part->GetNumGeoms();
            for (uint32_t i = 0; i < numGeoms; ++i) {
                hta::ai::Geom* partGeom = part->GetGeom(i); // PhysicBody::GetGeom returns non-const Geom* despite being a const method
                if (partGeom == nullptr)
                    continue;

                // docs §23.10: PhysicBody::m_pGeoms stores ai::GeomTransform* (ODE's
                // dGeomTransform, geom class 6, wrapping the real box/sphere/capsule as an
                // "inner" geom so it can sit at an offset from the body) - GetGeom() returns
                // the base Geom* type, but the underlying object IS actually a GeomTransform
                // (confirmed by m_pGeoms's declared element type); static_cast is safe here,
                // no RTTI needed. dGeomGetClass on the OUTER transform is always class 6, never
                // the real shape - unwrap to the inner geom first. Must stay non-const: only
                // the non-const GeomTransform::GetGeom() overload has a NATIVE trampoline
                // registered (extern/hta/source/hta/ai/GeomTransform.cpp) - the const overload
                // is declaration-only and would fail to link.
                hta::ai::GeomTransform* transform = static_cast<hta::ai::GeomTransform*>(partGeom);
                hta::ai::Geom* innerGeom = transform->GetGeom();
                if (innerGeom == nullptr)
                    continue;
                dxGeom* geom = innerGeom->GetGeomId();
                if (geom == nullptr)
                    continue;
                ++geomCount;

                const int32_t geomClass = dGeomGetClass(geom);
                JPH::Ref<JPH::Shape> shape;
                // Extra LOCAL-frame rotation the shape needs before the geom's own world
                // rotation is applied (docs §22.13) - only capsule needs this.
                JPH::Quat localShapeRemap = JPH::Quat::sIdentity();

                if (geomClass == dBoxClass) {
                    float lengths[3]; // full side lengths, not half-extents
                    dGeomBoxGetLengths(geom, lengths);
                    JPH::ShapeSettings::ShapeResult result =
                        JPH::BoxShapeSettings(JPH::Vec3(lengths[0] * 0.5f, lengths[1] * 0.5f, lengths[2] * 0.5f)).Create();
                    if (result.HasError()) { ++skippedCount; continue; }
                    shape = result.Get();
                    ++boxCount;
                } else if (geomClass == dSphereClass) {
                    const float radius = (float) dGeomSphereGetRadius(geom);
                    JPH::ShapeSettings::ShapeResult result = JPH::SphereShapeSettings(radius).Create();
                    if (result.HasError()) { ++skippedCount; continue; }
                    shape = result.Get();
                    ++sphereCount;
                } else if (geomClass == dCCylinderClass) {
                    float radius = 0.0f, length = 0.0f;
                    dGeomCCylinderGetParams(geom, &radius, &length);
                    if (radius <= 0.0f || length <= 0.0f) { ++skippedCount; continue; }
                    JPH::ShapeSettings::ShapeResult result = JPH::CapsuleShapeSettings(length * 0.5f, radius).Create();
                    if (result.HasError()) { ++skippedCount; continue; }
                    shape = result.Get();
                    // ODE capsule long axis is local Z, Jolt's is local Y - same remap as
                    // WalkSpaceForStaticExport (docs §22.13).
                    localShapeRemap = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0f));
                    ++capsuleCount;
                } else {
                    ++skippedCount; // trimesh/other - not supported for vehicle parts, see comment above
                    continue;
                }

                // docs §23.10: confirmed live (diagnostic log, since removed) that the OUTER
                // transform's own world position is a constant per-vehicle offset (matches
                // PhysicObj::GetPosition() + rot*GetMassCenter(), i.e. it tracks the RAW ODE
                // body origin, not PhysicObj's own adjusted GetPosition()) - identical across
                // every part of the same vehicle, NOT a per-part placement. The INNER geom's
                // position/rotation (ODE's dGeomTransform contract: a geom attached to a
                // transform has its coordinates set relative to that transform) is where the
                // real per-part offset lives. Combine both to get the part's true world
                // transform, then re-express relative to the chassis body (PhysicObj::
                // GetPosition/GetRotation) our compound shape is actually rooted at.
                const hta::CVector    outerWorldPos  = partGeom->GetPosition();
                const hta::Quaternion outerWorldRot  = partGeom->GetRotation();
                const hta::CVector    innerLocalPos  = innerGeom->GetPosition();
                const hta::Quaternion innerLocalRot  = innerGeom->GetRotation();

                const hta::CVector    partWorldPos = outerWorldPos + outerWorldRot * innerLocalPos;
                const hta::Quaternion partWorldRot = outerWorldRot * innerLocalRot;

                const hta::CVector    localPos = invChassisRot * (partWorldPos - chassisPos);
                const hta::Quaternion localRotRaw = invChassisRot * partWorldRot;
                const JPH::Quat       localRot = JPH::Quat(localRotRaw.x, localRotRaw.y, localRotRaw.z, localRotRaw.w).Normalized() * localShapeRemap;

                compound->AddShape(JPH::Vec3(localPos.x, localPos.y, localPos.z), localRot, shape.GetPtr());
            }
        }

        if (compound->mSubShapes.empty()) {
            LOG_WARNING("Shadow: no usable vehicle-part geoms for compound chassis shape (parts=%d geoms=%d) - falling back to bounding box",
                partCount, geomCount);
            return nullptr;
        }

        // docs §32: tried adding a small per-wheel "safety sphere" to this compound shape here
        // (positioned at mSuspensionMaxLength along the suspension direction, meant to only
        // engage once a wheel's own raycast already failed) to give the chassis body some real
        // collision presence where Jolt's raycast-only wheels have none - real ODE wheels are
        // physical bodies that can still brace against a slope even when heavily tilted; Jolt's
        // wheels contribute nothing once their raycast misses. REVERTED after live testing showed
        // it made things worse, not better: across 2 separate settle attempts on the same save,
        // the vehicle's resting trajectory changed substantially (once sliding ~26m further down
        // the slope than without the spheres, once settling with ALL 4 wheel raycasts reporting
        // no contact simultaneously - held up by the new spheres directly touching ground instead
        // of by wheel physics at all). Most likely cause: the added spheres, being part of the
        // PRIMARY collision shape (not a last-resort-only mechanism), also engage during the
        // initial chaotic post-spawn settling fall, changing the vehicle's bounce/roll trajectory
        // unpredictably - and for wheels already in normal contact, the sphere sits close enough
        // to the wheel's own contact point to compete with the suspension's own force rather than
        // staying inert. The underlying diagnosis (docs §32.1-32.4: Jolt's shadow chassis tips to
        // ~45° vs real ODE's ~19° on this save, because Jolt's wheels have no physical presence of
        // their own to arrest rotation once their raycast fails) still stands - just without a
        // fix here yet. A different approach (e.g. a true auxiliary body per wheel instead of a
        // shape folded into the same rigid body, or accepting this as a known Jolt-vehicle-model
        // limitation on steep terrain) would need its own dedicated investigation.

        JPH::ShapeSettings::ShapeResult result = compound->Create();
        if (result.HasError()) {
            LOG_WARNING("Shadow: compound chassis shape creation failed (%s) - falling back to bounding box", result.GetError().c_str());
            return nullptr;
        }

        LOG_INFO("Shadow: compound chassis shape built - %d part(s), %d geom(s) (%d box, %d sphere, %d capsule, %d skipped)",
            partCount, geomCount, boxCount, sphereCount, capsuleCount, skippedCount);
        return result.Get();
    }

    // docs §34: gives one wheel some genuine physical presence for the one case Jolt's raycast-
    // only WheeledVehicleController wheels can never handle on their own - a chassis tipped far
    // enough that the wheel's own raycast (bounded to mSuspensionMaxLength + radius, fixed
    // direction from a fixed attachment point) no longer reaches the ground at all. Confirmed
    // live (docs §32.1-32.3): on a save that rests a vehicle on a steep slope, Jolt's shadow
    // chassis settles at ~45° pitch where real ODE settles at ~19° on the same save - because
    // once 2 of 4 wheels' raycasts stop finding contact, NOTHING (wheels contribute zero force
    // when their raycast fails; real ODE wheels are physical bodies that still brace against
    // the slope) arrests the tip. Root goal (per explicit user direction: the target is Jolt
    // fully replacing ODE, so "fall back to ODE when this happens" is not an option) - make
    // Jolt's own vehicle model self-sufficient here instead.
    //
    // A prior attempt (docs §32.4) fused a same-sized sphere directly into the chassis's own
    // compound shape, positioned at exactly the wheel's maximum raycast reach - reverted after
    // live testing showed real regressions (vehicle sliding ~26m further down a slope once;
    // settling with all 4 wheels' raycasts reporting zero contact another time, held up
    // entirely by the new spheres instead of by any wheel physics). Root cause of that failure:
    // a shape fused into the SAME rigid body (a) participates in the chaotic post-spawn
    // settling fall unpredictably, and (b) sitting at exactly the wheel's own boundary reach
    // means it's ALREADY touching the ground whenever the wheel's own raycast is ALSO finding
    // ground nearby - directly competing with the suspension spring under perfectly normal
    // conditions, not just as a last resort.
    //
    // This version is a genuinely SEPARATE dynamic body, connected to the chassis via a
    // JPH::SliderConstraint (translation-only along the suspension axis, no rotation - exactly
    // what's wanted here), with its allowed range set to [wheelReach, wheelReach+margin] - NOT
    // [0, wheelReach+margin]. That's the key structural difference from the reverted attempt:
    // this proxy is PHYSICALLY INCAPABLE of ever being closer to the chassis than exactly where
    // the wheel's own raycast reach already ends, so for the entire normal operating range
    // (ground anywhere within the wheel's own reach) it simply cannot also touch anything and
    // cannot compete with the suspension spring at all. It only has room to extend further once
    // the chassis has tipped enough that even the wheel's own full reach has already failed -
    // exactly the failure case this is meant to catch, and only that case.
    //
    // No motor, no driven friction, no steering - a plain sphere purely for last-resort
    // structural bracing, not a second wheel model. Object layer restricted to NON_MOVING only
    // (kWheelProxyLayer mirrors kWheelQueryLayer's own restriction, same reason: this must
    // brace against terrain, never push against another vehicle's body or kinematic mirror).
    // Leaked forever on rebuild, matching this file's established convention for the main
    // shadow body/constraint (see BuildShadow's own comment for why - tearing down Jolt
    // bodies/constraints on a vehicle swap caused a real, repeatable worker-thread crash
    // earlier this session).
    //
    // Self-collision against the chassis is excluded explicitly via GetWheelGroupFilter
    // (docs §38.1/§37 item 3), not just geometric separation. Gated behind [jolt_harness]
    // wheel_proxy (default on) so it can be disabled without a rebuild if it ever needs to be
    // ruled out live. Takes the base JPH::WheelSettings (not the WV-derived type) since it's
    // called from TryBuildWheelProxiesOnceSettled with whatever wheel->GetSettings() returns,
    // and every field used below (mPosition/mSuspensionDirection/mSuspensionMaxLength/mRadius)
    // already lives on the base class.
    static void BuildWheelProxy(JPH::PhysicsSystem* physics, JPH::Body* chassisBody,
            const JPH::WheelSettings* ws, const char* label, uint32_t wheelIndex, uint32_t collisionGroupId) {
        if (kraken::Config::Instance().jolt_wheel_proxy.value == 0)
            return;

        ++g_legacyPaths.proxyBuilds;   // docs §110
        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();

        const float reachLimit = ws->mSuspensionMaxLength + ws->mRadius; // exactly as far as the wheel's own raycast already searches
        // docs §34 live-tested: 1.0m was too conservative - the actual measured shortfall on
        // the Molokovoz slope test was 2.2-3.2m (docs §32.1's LogNoContactRaycastDiagnostic),
        // so a 1.0m margin left the proxy still short of the ground and the vehicle settled at
        // the same ~45° pitch as with no proxy at all. Widened to something that can plausibly
        // reach across a real tip on this scale of vehicle - not a rigorously derived number,
        // just generously beyond the worst shortfall actually measured.
        constexpr float kExtraReachMargin = 3.5f; // meters beyond the wheel's own reach - the ONLY zone this proxy can ever occupy
        const float proxyRadius = std::max(ws->mRadius, 0.05f);

        const JPH::RMat44 chassisTransform = chassisBody->GetWorldTransform();
        const JPH::RVec3  worldAttachPoint = chassisTransform * ws->mPosition;
        const JPH::Vec3   worldSuspensionDir = chassisTransform.Multiply3x3(ws->mSuspensionDirection).Normalized();
        const JPH::RVec3  worldProxyStart = worldAttachPoint + worldSuspensionDir * reachLimit;

        JPH::RefConst<JPH::Shape> proxyShape = new JPH::SphereShape(proxyRadius);
        JPH::BodyCreationSettings proxySettings(proxyShape.GetPtr(), worldProxyStart,
            JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, kWheelProxyLayer);
        proxySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        proxySettings.mMassPropertiesOverride.mMass = 10.0f; // light - a structural probe, not meant to add meaningfully to the vehicle's own dynamics

        JPH::Body* proxyBody = bodyInterface.CreateBody(proxySettings);
        if (proxyBody == nullptr) {
            LOG_WARNING("Shadow (%s): wheel proxy body creation failed for wheel=%u (out of bodies?)", label, wheelIndex);
            return;
        }
        bodyInterface.AddBody(proxyBody->GetID(), JPH::EActivation::Activate);

        // docs §37 item 3: explicit exclusion, hardening the geometric-only safeguard below -
        // see GetWheelGroupFilter's comment. Falls back to geometric-only separation
        // (silently, but logged) if wheelIndex somehow exceeds the table's generous slot count.
        if (wheelIndex + 1 <= kMaxWheelsPerVehicleGroupFilter) {
            proxyBody->SetCollisionGroup(JPH::CollisionGroup(GetWheelGroupFilter(), collisionGroupId, wheelIndex + 1));
        } else {
            LOG_WARNING("Shadow (%s): wheel=%u exceeds kMaxWheelsPerVehicleGroupFilter=%u - self-collision exclusion not applied for this wheel, relying on geometric separation only",
                label, wheelIndex, kMaxWheelsPerVehicleGroupFilter);
        }

        // mPoint1 == mPoint2 (both the wheel's real attachment point) explicitly defines the
        // slider's "0" position to be there, regardless of where the proxy body actually starts
        // (worldProxyStart, reachLimit away) - the constraint computes the ACTUAL initial slider
        // value from the bodies' real positions relative to this shared reference, which comes
        // out to exactly reachLimit given where the proxy was just placed. mLimitsMin=0 then
        // means "can't get closer than reachLimit from the attachment", not "can't get closer
        // than the attachment itself" - the whole point of this design.
        JPH::SliderConstraintSettings sliderSettings;
        sliderSettings.mSpace         = JPH::EConstraintSpace::WorldSpace;
        sliderSettings.mAutoDetectPoint = false;
        sliderSettings.mPoint1       = worldAttachPoint;
        sliderSettings.mPoint2       = worldAttachPoint;
        sliderSettings.mSliderAxis1  = worldSuspensionDir;
        sliderSettings.mSliderAxis2  = worldSuspensionDir;
        // Any world axis not parallel to the slider axis works as the perpendicular reference -
        // chassis-local forward is always perpendicular to chassis-local "down".
        const JPH::Vec3 worldNormalAxis = chassisTransform.Multiply3x3(JPH::Vec3(0.0f, 0.0f, 1.0f)).Normalized();
        sliderSettings.mNormalAxis1  = worldNormalAxis;
        sliderSettings.mNormalAxis2  = worldNormalAxis;
        sliderSettings.mLimitsMin    = 0.0f;
        sliderSettings.mLimitsMax    = kExtraReachMargin;

        JPH::Constraint* constraint = sliderSettings.Create(*chassisBody, *proxyBody);
        physics->AddConstraint(constraint);

        LOG_INFO("Shadow (%s): wheel proxy built for wheel=%u - engages only beyond %.3fm from attachment (wheel's own raycast already covers up to there), extra reach margin %.1fm",
            label, wheelIndex, (double) reachLimit, (double) kExtraReachMargin);
    }

    // docs §38.9: BuildWheelProxy used to be called synchronously inside BuildShadow, snapshotting
    // worldAttachPoint - and therefore the proxy's entire geometric relationship to the chassis -
    // at whatever transform the chassis happened to have at that exact instant. Fine for a
    // vehicle already resting on a save's terrain (every prior Scout/Molokovoz test), wrong for
    // one that still has to fall from a mid-air spawn - confirmed live via docs §38.8's hit-body
    // logging: a chassis that free-falls and tumbles several meters before landing (e.g. a
    // same-session vehicle swap via ai::Player::ChangeVehicleByNew - spawning in the air and
    // needing time to land is itself correct, existing behavior, not something to change) built
    // its wheel-proxy geometry against a stale, airborne snapshot, and ended up permanently
    // resting on its OWN wheel-proxy spheres instead of real terrain - wheels forever reporting
    // no contact, throttle forever ignored. Deferring proxy construction until the chassis has
    // actually stopped moving (checked every tick here, cheap) fixes this at the source without
    // needing to tear down or rebuild an already-built proxy - this file's hard rule (see
    // BuildShadow's comment below) is to never tear down a live Jolt body/constraint; delaying
    // FIRST construction sidesteps that rule entirely instead of bending it.
    static void TryBuildWheelProxiesOnceSettled(JPH::PhysicsSystem* physics, ShadowState& state, const char* label) {
        if (state.wheelProxiesBuilt || state.constraint == nullptr)
            return;

        ++g_legacyPaths.proxyPassedGuard;   // docs §110: past the constraint guard, i.e. legacy alive
        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();

        // A brand new dynamic body reads a real velocity of exactly 0 before its very first
        // physics step - gravity only gets integrated once StepPhysicsProfiled actually runs -
        // so checking speed on frame 0 always looks "already settled" whether or not the body is
        // about to fall. kMinFramesBeforeCheck gives it a real chance to start moving first
        // (live-confirmed this was needed: without it, proxies built at frame=0/speed=0.000 every
        // time, same bug as the old synchronous build just moved one tick later).
        //
        // docs §38.9: a single-frame speed check is noisy - live-measured a big vehicle (Ural01,
        // falling from its mid-air swap spawn point, a real impact spike to 12.3m/s before
        // decaying) rocking on impact and dipping under the threshold for one tick before
        // spiking back up, which was enough to call it "settled" mid-tumble and reproduce the
        // exact frozen-shadow bug this whole mechanism exists to avoid. Requiring several
        // consecutive under-threshold ticks (not just one) fixed that. Run-to-run variance in
        // how long the SAME fall actually takes to settle was also larger than expected (453
        // frames one run, still >900 and 1.4m/s on a later run) - kSettleTimeoutFrames is
        // generous specifically so the real (consecutive-ticks) condition is what fires in
        // practice; the timeout is a last-resort backstop, not the common path.
        constexpr uint64_t kMinFramesBeforeCheck    = 15;   // ~0.25s at this level's observed ~90-100Hz tick rate
        constexpr float    kSettledSpeedMps         = 0.5f;
        constexpr uint32_t kRequiredConsecutiveSlow = 30;   // ~0.3-0.5s of sustained low speed, not just one lucky tick
        constexpr uint64_t kSettleTimeoutFrames     = 3600; // ~40-60s - backstop, should rarely if ever actually fire
        if (state.frameCounter < kMinFramesBeforeCheck)
            return;

        const float speed = bodyInterface.GetLinearVelocity(state.bodyId).Length();
        state.consecutiveSlowFrames = (speed < kSettledSpeedMps) ? (state.consecutiveSlowFrames + 1) : 0;
        if (state.frameCounter % 60 == 0)
            LOG_INFO("Shadow (%s): waiting to settle before building wheel proxies - frame=%llu speed=%.3fm/s consecutiveSlow=%u",
                label, (unsigned long long) state.frameCounter, (double) speed, state.consecutiveSlowFrames);
        const bool settled = state.consecutiveSlowFrames >= kRequiredConsecutiveSlow;
        if (!settled && state.frameCounter < kSettleTimeoutFrames)
            return;

        JPH::Body* chassisBody = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.bodyId);
        if (chassisBody == nullptr)
            return;

        uint32_t wheelIndex = 0;
        for (JPH::Wheel* wheel : state.constraint->GetWheels())
            BuildWheelProxy(physics, chassisBody, wheel->GetSettings(), label, wheelIndex++, state.collisionGroupId);

        state.wheelProxiesBuilt = true;
        LOG_INFO("Shadow (%s): wheel proxies built post-settle (speed=%.3fm/s, frame=%llu, settled=%d, viaTimeout=%d)",
            label, (double) speed, (unsigned long long) state.frameCounter, settled ? 1 : 0,
            (!settled && state.frameCounter >= kSettleTimeoutFrames) ? 1 : 0);
    }

    // Builds a fresh shadow chassis+wheel body/constraint mirroring `vehicle`'s prototype
    // geometry into `state`, called once for the first vehicle a given ShadowState tracks and
    // again every time it changes (level reload, vehicle switch - see UpdateOneVehiclePreStep below).
    // Returns false (and logs why) if the vehicle's data isn't usable yet.
    //
    // Deliberately NEVER tears down a ShadowState's previous JPH::VehicleConstraint/Body - an
    // earlier version called PhysicsSystem::RemoveConstraint/RemoveStepListener and
    // BodyInterface::DestroyBody on the old ones before building new ones, which produced a
    // real, repeatable crash during testing: an access violation inside
    // JPH::VehicleConstraint::OnStep (VehicleConstraint.cpp:236, reading through what the
    // debug heap's 0xDDDDDDDD "freed" fill pattern strongly suggests was a dangling pointer)
    // on a JobSystemThreadPool worker thread, immediately after the second BuildShadow() call
    // for the same state - which deadlocked the whole game (the main thread blocks inside the
    // synchronous PhysicsSystem::Update() call waiting for that worker's job to finish, which
    // never happens once the worker thread has taken a hard fault). The exact Jolt-internal
    // lifetime rule being violated wasn't conclusively pinned down (constraint/body pool reuse
    // across RemoveConstraint+DestroyBody was the leading theory - see docs/jolt-integration-
    // techanalysis.md Stage 1 section for the full writeup). Fix: never destroy anything - the
    // old constraint/body are simply abandoned (state.vehicle/bodyId/constraint get
    // overwritten, so our own code stops touching them), left registered and simulated
    // forever, matching this codebase's established leak-forever convention (see fix::jolt's
    // g_physicsSystem et al.). Vehicle swaps are rare (once per level/vehicle change, not a
    // hot loop) and each abandoned shadow costs one small dynamic body (no separate wheel
    // bodies - JPH's wheels are raycast-based), so accumulating a handful of them across a
    // long play session is a trivial, accepted cost for avoiding the use-after-free entirely
    // rather than guessing at the real fix.
    // docs §40: forward declaration - defined below (near StepWheelModel), but BuildShadow (right
    // here) needs to call it during initial construction, before its own definition appears.
    static void InitWheelModelSuspension(JPH::PhysicsSystem* physics, ShadowState& state,
                                         const hta::CVector& pos, const hta::Quaternion& rot,
                                         const char* label);

    // ------------------------------------------------------------------------------------------
    // docs §54.4 (Этап 1, шаг -1F): deferred destruction of abandoned Jolt bodies/constraints.
    //
    // This file's standing rule is leak-forever on rebuild (see BuildShadow's comment): an earlier
    // version tore the old constraint/body down inline and produced a repeatable access violation
    // inside JPH::VehicleConstraint::OnStep on a worker thread, so nothing has been destroyed
    // since. That was an acceptable trade while a rebuild cost exactly ONE small body. It stops
    // being acceptable once every wheel is its own body on its own constraint: a rebuild then
    // abandons 1 + N bodies and N constraints, and rebuilds happen on every vehicle swap, every
    // tuning change and every torn-off wheel.
    //
    // The safe window is the one DrainPendingPushbacks (docs §23.11) already established and
    // documents: back on the main thread, immediately AFTER PhysicsSystem::Update() has fully
    // returned, when no worker job can still be touching a constraint. Enqueue from anywhere,
    // destroy only there.
    //
    // Ordering matters and is the most likely cause of the original crash: a VehicleConstraint is
    // ALSO a PhysicsStepListener, so it must stop being called (RemoveStepListener) before it
    // stops being a constraint (RemoveConstraint), and only then may the bodies it references be
    // removed and destroyed. In wheelmodel mode the constraint is never registered at all (see
    // BuildShadow's `if (!state.wheelModelMode)` branch), which is why `registered` is tracked
    // rather than assumed.
    //
    // Gated OFF by default ([jolt_harness] deferred_destroy). Turning it on is an experiment
    // against an unexplained historical crash, not a settled fix - the leak-forever path stays
    // the default until this is proven live under vehicle swaps.
    struct PendingJoltDestroy {
        std::vector<JPH::BodyID>                bodies;
        std::vector<JPH::Ref<JPH::Constraint>>  constraints;
        bool                                    constraintsRegistered = false;
    };
    static std::mutex                      g_pendingDestroyMutex;
    static std::vector<PendingJoltDestroy> g_pendingDestroys;

    static void EnqueueJoltDestroy(PendingJoltDestroy&& item) {
        if (kraken::Config::Instance().jolt_deferred_destroy.value == 0)
            return; // leak-forever (historical default): abandon, never destroy
        if (item.bodies.empty() && item.constraints.empty())
            return;
        // docs §54.5: HARD restriction, established by live bisection, not by theory. Destroying a
        // rebuilt shadow is safe in wheelmodel mode and reliably fatal on the VehicleConstraint
        // path (4/4 runs, 0xE06D7363, inside the drain). The difference between the two is that
        // the VehicleConstraint path also builds wheel proxies - a dynamic sphere plus a
        // SliderConstraint per wheel, neither of which is stored anywhere, so nothing can enqueue
        // them alongside the chassis they reference. Sweeping live constraints for references to
        // the doomed bodies (implemented in the drain) did NOT fix it, so at least one more
        // untracked reference to a destroyed object exists and has not been identified.
        //
        // Rather than keep guessing at an unexplained crash on a code path that Этап 1 deletes
        // outright (wheel proxies and the whole non-simulated VehicleConstraint container go away
        // when wheels become real bodies), the queue simply refuses to accept work it cannot prove
        // it owns. When Этап 1's topology lands, wheel bodies and their SixDOF constraints ARE
        // tracked per ShadowState, so they can be enqueued explicitly and this restriction lifted
        // deliberately, with a live test, instead of by assumption.
        if (item.constraintsRegistered) {
            static bool s_warnedOnce = false;
            if (!s_warnedOnce) {
                s_warnedOnce = true;
                LOG_WARNING("docs §54.5: deferred_destroy=1 ignored for the VehicleConstraint path - "
                            "untracked wheel-proxy bodies/constraints still reference the chassis and "
                            "destroying it crashes (reproduced 4/4). Falling back to leak-forever here. "
                            "Only the wheelmodel path (jolt_wheelmodel=2) actually destroys.");
            }
            return;
        }
        std::lock_guard<std::mutex> lock(g_pendingDestroyMutex);
        g_pendingDestroys.push_back(std::move(item));
    }

    // Called once per frame from UpdateShadow, right after StepPhysicsProfiled returns - the same
    // "physics step has fully finished, single-threaded again" point DrainPendingPushbacks uses.
    static void DrainPendingJoltDestroys() {
        std::vector<PendingJoltDestroy> items;
        {
            std::lock_guard<std::mutex> lock(g_pendingDestroyMutex);
            items.swap(g_pendingDestroys);
        }
        if (items.empty())
            return;

        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return;
        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();

        size_t destroyedBodies = 0, destroyedConstraints = 0, sweptConstraints = 0;
        for (PendingJoltDestroy& item : items) {
            // 1. stop it being called back, 2. stop it being solved, 3. only then free bodies.
            for (JPH::Ref<JPH::Constraint>& c : item.constraints) {
                if (c == nullptr)
                    continue;
                if (item.constraintsRegistered) {
                    JPH::VehicleConstraint* vc = dynamic_cast<JPH::VehicleConstraint*>(c.GetPtr());
                    if (vc != nullptr)
                        physics->RemoveStepListener(vc);
                    physics->RemoveConstraint(c.GetPtr());
                }
                ++destroyedConstraints;
            }
            item.constraints.clear(); // Ref<> release: frees whatever no longer has an owner

            // docs §54.5 - THE crash that this whole mechanism tripped over, found live and
            // deterministically reproducible: destroying a body that some OTHER, still-registered
            // constraint references leaves that constraint holding a freed Body*, and Jolt dies
            // the next time it touches it. In this file the untracked references are the
            // wheel-proxy sliders (BuildWheelProxy adds a SliderConstraint per wheel between the
            // chassis body and a proxy sphere, and stores NEITHER anywhere), so a rebuild in the
            // VehicleConstraint path would free the chassis out from under 4-6 live sliders.
            // Live evidence: with wheelmodel=2 (no proxies are ever built) the drain succeeded;
            // with wheelmodel=0 (proxies exist) the process died with 0xE06D7363 inside this
            // function every single run. This is also the most plausible explanation for the
            // original, never-root-caused Stage 1 teardown crash (see BuildShadow's comment).
            //
            // Rather than require every future call site to remember to enqueue every related
            // constraint, sweep for them: Jolt can enumerate all live constraints, and both
            // constraint shapes used here expose the bodies they reference. O(constraints) and
            // only on a rebuild, which is rare by construction.
            if (!item.bodies.empty()) {
                const JPH::Constraints all = physics->GetConstraints();
                for (const JPH::Ref<JPH::Constraint>& c : all) {
                    if (c == nullptr)
                        continue;
                    bool referencesDoomedBody = false;
                    if (const JPH::TwoBodyConstraint* tb = dynamic_cast<const JPH::TwoBodyConstraint*>(c.GetPtr())) {
                        const JPH::Body* b1 = tb->GetBody1();
                        const JPH::Body* b2 = tb->GetBody2();
                        for (const JPH::BodyID& id : item.bodies)
                            if ((b1 != nullptr && b1->GetID() == id) || (b2 != nullptr && b2->GetID() == id))
                                referencesDoomedBody = true;
                    } else if (const JPH::VehicleConstraint* vc = dynamic_cast<const JPH::VehicleConstraint*>(c.GetPtr())) {
                        const JPH::Body* vb = vc->GetVehicleBody();
                        for (const JPH::BodyID& id : item.bodies)
                            if (vb != nullptr && vb->GetID() == id)
                                referencesDoomedBody = true;
                    }
                    if (!referencesDoomedBody)
                        continue;
                    if (JPH::VehicleConstraint* vcNonConst = dynamic_cast<JPH::VehicleConstraint*>(c.GetPtr()))
                        physics->RemoveStepListener(vcNonConst);
                    physics->RemoveConstraint(c.GetPtr());
                    ++sweptConstraints;
                }
            }

            for (const JPH::BodyID& id : item.bodies) {
                if (id.IsInvalid())
                    continue;
                if (bodyInterface.IsAdded(id))
                    bodyInterface.RemoveBody(id);
                bodyInterface.DestroyBody(id);
                ++destroyedBodies;
            }
        }

        LOG_INFO("docs §54.4: deferred destroy drained - %zu body(ies), %zu constraint(s) freed, "
                 "%zu dangling constraint(s) swept (see §54.5 - these would have been a use-after-free)",
            destroyedBodies, destroyedConstraints, sweptConstraints);
    }

    // ------------------------------------------------------------------------------------------
    // docs §54 (Этап 1, шаг -1A/-1D): one-shot per-vehicle audit of the two facts the whole
    // wheel-as-a-body topology rests on, neither of which has ever been verified against the real
    // game data:
    //
    //   (A) The chassis-local axis convention. BuildShadow below hard-codes suspension = -Y,
    //       steering axis = +Y, wheel forward = +Z (see its own comment: "NOT independently
    //       confirmed by disassembly ... would need dJointGetHinge2Axis1/Axis2 read back in
    //       body-local space"). Everything about a SixDOF strut frame - which axis translates,
    //       which rotates for steering, which is the spin axis - is derived from that guess, so a
    //       wrong guess would not produce an obvious error, it would produce a subtly wrong
    //       vehicle. ODE itself holds the ground truth: Hinge2 axis1 is the chassis-side axis
    //       (the one softened by dParamSuspensionERP/CFM, i.e. the suspension travel + steering
    //       axis) and axis2 is the wheel-side axle. Both come back in WORLD space, so they are
    //       rotated into the chassis body frame here before being compared to the convention.
    //   (D) The real spread of suspension stiffness/damping across prototypes. If kSusp is on the
    //       order of 1e6 N/m while an unsprung wheel body weighs ~11kg, the mass ratio against a
    //       300-3000kg chassis will make a SixDOF strut spongy at Jolt's default 10 velocity
    //       iterations, and the constraint will need SetNumVelocityStepsOverride. That has to be
    //       known from real data before the topology is written, not discovered afterwards.
    //
    // Deliberately a pure logger: it reads ODE, computes angles, writes lines. It changes no
    // state and is called exactly once per vehicle from BuildShadow, so it costs nothing on the
    // hot path and can stay in the tree as documentation of what the real data actually was.
    using DJointGetHinge2VecAuditFn = void(__fastcall*)(void* joint, float* result);
    static const auto AuditDJointGetHinge2Axis1 = (DJointGetHinge2VecAuditFn) (0x007d0040);
    static const auto AuditDJointGetHinge2Axis2 = (DJointGetHinge2VecAuditFn) (0x007d00f0); // RVA 0x3d00f0, sibling of Axis1 - same __fastcall(joint, float[3]) shape
    using DJointGetBodyAuditFn = void* (__fastcall*)(void* joint, int index);
    static const auto AuditDJointGetBody = (DJointGetBodyAuditFn) (0x007c5150);
    using DBodyGetQuaternionAuditFn = const float* (__fastcall*)(void* body);
    static const auto AuditDBodyGetQuaternion = (DBodyGetQuaternionAuditFn) (0x007c4740); // RVA 0x3c4740, returns ODE's {w,x,y,z}

    // Angle between two unit-ish vectors, in degrees, folded to [0,90] - the convention check
    // cares whether an axis LIES ALONG the expected direction, not which of the two senses it
    // points in (ODE's axis1 sign depends on how AttachToPhysicObj happened to build the joint).
    static float AuditAxisAngleDeg(const JPH::Vec3& a, const JPH::Vec3& b) {
        const float la = a.Length(), lb = b.Length();
        if (la < 1.0e-6f || lb < 1.0e-6f)
            return -1.0f;
        const float c = std::clamp(std::fabs(a.Dot(b) / (la * lb)), 0.0f, 1.0f);
        return JPH::RadiansToDegrees(std::acos(c));
    }

    static void LogHinge2AxisAudit(hta::ai::Vehicle* vehicle, const char* label,
                                   const JPH::Vec3& assumedSuspDir, const JPH::Vec3& assumedAxle) {
        const uint32_t numWheels = vehicle->GetNumWheels();
        void* chassisBody = nullptr;
        for (uint32_t i = 0; i < numWheels && chassisBody == nullptr; ++i) {
            const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[i];
            if (info.m_bWheelPresent && info.m_wheel != nullptr && info.m_wheel->m_jointID != nullptr)
                chassisBody = AuditDJointGetBody(info.m_wheel->m_jointID, 0); // body1 = chassis
        }
        if (chassisBody == nullptr) {
            LOG_WARNING("docs §54 axis-audit (%s): no wheel has a live Hinge2 joint yet - convention UNVERIFIED for this vehicle", label);
            return;
        }

        const float* q = AuditDBodyGetQuaternion(chassisBody);
        if (q == nullptr)
            return;
        // ODE stores {w,x,y,z}; JPH::Quat takes (x,y,z,w). Conjugate rotates world -> body-local.
        const JPH::Quat chassisRot(q[1], q[2], q[3], q[0]);
        const JPH::Quat worldToLocal = chassisRot.Conjugated();

        float worstSuspDeg = 0.0f, worstAxleDeg = 0.0f, worstPerpDeg = 0.0f;
        for (uint32_t i = 0; i < numWheels; ++i) {
            const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[i];
            hta::ai::Wheel* wheel = info.m_wheel;
            if (!info.m_bWheelPresent || wheel == nullptr || wheel->m_jointID == nullptr)
                continue;

            float a1[4] = {}, a2[4] = {};
            AuditDJointGetHinge2Axis1(wheel->m_jointID, a1); // chassis side: suspension travel + steering
            AuditDJointGetHinge2Axis2(wheel->m_jointID, a2); // wheel side: the axle

            const JPH::Vec3 axis1Local = worldToLocal * JPH::Vec3(a1[0], a1[1], a1[2]);
            const JPH::Vec3 axis2Local = worldToLocal * JPH::Vec3(a2[0], a2[1], a2[2]);

            const float suspDeg = AuditAxisAngleDeg(axis1Local, assumedSuspDir);
            const float axleDeg = AuditAxisAngleDeg(axis2Local, assumedAxle);
            // Do the two axes stay perpendicular? A SixDOF strut frame needs axis1 (translate +
            // steer) and axis2 (spin) orthogonal; if a prototype violates that, one constraint
            // cannot represent it and the topology needs a separate hub body + hinge.
            const float perpDeg = 90.0f - AuditAxisAngleDeg(axis1Local, axis2Local);

            const bool steerable = wheel->m_steering != hta::ai::Wheel::STEERING_NO;
            if (suspDeg > worstSuspDeg) worstSuspDeg = suspDeg;
            // A STEERED wheel's axle is supposed to deviate from the chassis X axis - by exactly
            // the current steer angle, since it rotates about axis1. Live-confirmed on the first
            // run of this audit: every steer=0 wheel read 0.03-0.10 deg while steered ones read
            // whatever the AI happened to be commanding (up to 45 deg, e.g. axis2_local =
            // (-0.707, 0.001, -0.707)). Comparing a steered axle against the unsteered convention
            // therefore measures the steering, not the convention, and an earlier version of this
            // check reported a spurious "VIOLATED" on 55 of 66 vehicles because of it. The
            // convention gate is the UNSTEERED wheels; for steered ones the meaningful structural
            // invariant is orthogonality to axis1 (checked separately via perpDeg below), which
            // holds regardless of steer angle.
            if (!steerable && axleDeg > worstAxleDeg) worstAxleDeg = axleDeg;
            if (perpDeg > worstPerpDeg) worstPerpDeg = perpDeg;

            const hta::ai::WheelPrototypeInfo* proto = wheel->GetPrototypeInfo();
            const float cfm = proto != nullptr ? proto->m_suspensionCFM : 0.0f;
            const float erp = proto != nullptr ? proto->m_suspensionERP : 0.0f;
            const float range = proto != nullptr ? proto->m_suspensionRange : 0.0f;
            const float referenceH = 1.0f / std::max(kraken::Config::Instance().jolt_susp_reference_hz.value, 1.0f);
            const float kSusp = cfm > 1.0e-8f ? erp / (referenceH * cfm) : 0.0f;
            const float cSusp = cfm > 1.0e-8f ? (1.0f - erp) / cfm : 0.0f;

            LOG_INFO("docs §54 axis-audit (%s) w=%u: axis1_local=(%.3f,%.3f,%.3f) vs assumedSusp -> %.2f deg | "
                     "axis2_local=(%.3f,%.3f,%.3f) vs assumedAxle -> %.2f deg | axis1^axis2 off-perp %.2f deg || "
                     "kSusp=%.0f N/m cSusp=%.0f Ns/m range=%.3f R=%.3f mWheel=%.1f driven=%d steer=%d",
                label, i,
                (double) axis1Local.GetX(), (double) axis1Local.GetY(), (double) axis1Local.GetZ(), (double) suspDeg,
                (double) axis2Local.GetX(), (double) axis2Local.GetY(), (double) axis2Local.GetZ(), (double) axleDeg,
                (double) perpDeg,
                (double) kSusp, (double) cSusp, (double) range,
                (double) wheel->GetRadius(), (double) wheel->GetMass(),
                wheel->m_driven ? 1 : 0, wheel->m_steering != hta::ai::Wheel::STEERING_NO ? 1 : 0);
        }

        // The single line to grep for. Threshold 1 deg per the plan: anything above it means the
        // hard-coded convention is wrong and the SixDOF frame must be built from the real axes.
        // Three independent gates, all of which must hold for a one-SixDOF-per-wheel strut:
        //   susp - the softened Hinge2 axis really is chassis-local Y (the translate+steer axis);
        //   axle - UNSTEERED wheels really do spin about chassis-local X;
        //   perp - axle stays orthogonal to the strut axis (true at any steer angle), which is
        //          what makes a single constraint able to represent the whole strut.
        LOG_INFO("docs §54 axis-audit SUMMARY (%s): worst susp %.2f deg, worst axle (unsteered only) %.2f deg, worst off-perp %.2f deg -> convention %s",
            label, (double) worstSuspDeg, (double) worstAxleDeg, (double) worstPerpDeg,
            (worstSuspDeg <= 1.0f && worstAxleDeg <= 1.0f && worstPerpDeg <= 1.0f)
                ? "CONFIRMED" : "**VIOLATED - build the strut frame from real axes**");
    }

    // docs §59/§63 (Этап 1, шаг 3): the read half of the harvest. One instance per vehicle,
    // registered on the PhysicsSystem, so Jolt runs them in parallel across vehicles.
    //
    // What it may touch is not a style preference, it is Jolt's access grant inside
    // JobStepListeners: positions are READ-ONLY (the broadphase is updating concurrently),
    // velocities are read/write, bodies may be activated but not deactivated. So reading a cached
    // Body* is legal and any BodyInterface call is not - resolving a BodyID through the interface
    // in here deadlocks against locks this job already holds. Hence the cached pointers, taken at
    // build time, and the rule that each listener touches only its own chassis and its own wheels:
    // that is exactly the non-overlapping subset Jolt requires for the parallel run to be sound.
    //
    // At step 3 it applies NO force. It reprojects each harvested contact into a penetration,
    // decides which records survive, and records what it saw. Step 4 turns that into force.
    // docs §124 step 2: forward-declared so VehicleStepListener can hold routing pointers to it
    // (OnStep pushes each step's contact geometry into whichever wheel's constraint is live).
    // Defined below VehicleStepListener, since it reuses VehicleStepListener::WheelDiag's shape;
    // OnStep is therefore declared in-class but DEFINED out-of-line, after WheelContactConstraint,
    // so its body can call into a complete type instead of just this forward declaration.
    class WheelContactConstraint;

    class VehicleStepListener final : public JPH::PhysicsStepListener {
    public:
        // Filled at build time. Deliberately a POD snapshot rather than a ShadowState* - the AI
        // shadows live in a std::vector that clears and refills on re-init, so a pointer into it
        // can outlive what it described.
        struct WheelTick {
            JPH::Body* body   = nullptr;
            float      radius = 0.3f;
            float      tau    = 0.01f;
            // docs §63 / plan §3.5: the two inputs whose MEANING changes in mode 4. `mass` is the
            // wheel BODY's mass, not the per-corner sprung mass mode 2 passed, and it must be
            // bit-identical to what BodyCreationSettings got - it feeds c_t = 2*zeta*sqrt(k_t*m)
            // and capLat inside the core. `inertia` is the real disc inertia, not the flat
            // wheelmodel.wheel_inertia = 30.0 mode 2 used; the ~10x drop rescales capPar.
            float      mass    = 10.0f;
            float      inertia = 1.0f;
            JPH::Vec3  axleLocal{1.0f, 0.0f, 0.0f};
            bool       driven  = false;
        };
        JPH::Body* chassis = nullptr;
        uint32_t   slot    = 0;
        uint32_t   wheelCount = 0;
        WheelTick  wheels[kMaxHarvestWheels];
        // docs §124 step 2: routing only, not a data owner - tells OnStep WHERE to push this
        // step's contact cache (nullptr when wm4_contact_constraint is off, or index is past
        // kMaxHarvestWheels). The cache itself lives ON the constraint, never here: a leaked/
        // abandoned constraint from a prior vehicle rebuild must keep describing its OWN wheel
        // forever, not silently start reading whatever the next vehicle's build overwrites this
        // slot with. Populated in BuildWheelBodies alongside wheels[] itself.
        WheelContactConstraint* contactConstraints[kMaxHarvestWheels] = {};
        // Snapshotted on the main thread at build time: the listener must not reach into the
        // config singleton from a worker, and a value that changed mid-step would make the two
        // halves of one force disagree.
        kraken::fix::wheelmodel::WMParams params;

        // Diagnostics for the §59/§63 line, written here and read by the main thread after
        // Update() has fully returned. Plain values, not atomics: only this listener writes them,
        // and the only reader runs when no worker is live.
        uint32_t lastWheelsWithContact = 0;
        uint32_t lastContactPoints     = 0;
        uint32_t lastOverflow          = 0;
        uint32_t lastSurvived          = 0;
        float    lastMaxNormalF        = 0.0f;

        // docs §66: per-wheel band diagnostics. These exist to DISCRIMINATE, not to decorate.
        // distMax in units of R separates "the wheel is riding on the rim sphere" (~1-tau/R) from
        // "the tyre sphere is being hit" (~1.00R); the tyre/rim record split separates "the tyre
        // sub-shape generates no manifolds at all" from "it does and they are being rejected".
        // Without them, an inert band and a mis-placed wheel look identical from outside.
        struct WheelDiag {
            float    fnGround = 0.0f;   // normal force from the GROUND slot alone
            float    fnTotal  = 0.0f;   // normal force summed over every applied slot
            // Broken out because "fnTotal is exactly twice fnGround" is what exposed the side-slot
            // weight bug, and reading that off a difference of two aggregates is a trick nobody
            // should have to repeat. With these, a slot being double-counted is visible directly.
            float    fnObst   = 0.0f;
            float    fnSide   = 0.0f;
            float    pen      = 0.0f;   // the tau-clamped penetration actually fed to the core
            float    penRaw   = 0.0f;   // before the clamp - penRaw > tau means the band saturated
            float    distMax  = 0.0f;   // max |centre - contactPoint|, in units of R
            uint32_t recsTyre = 0;
            uint32_t recsRim  = 0;
            uint32_t survived = 0;
            // The split distMax could not make. A max over BOTH sub-shapes cannot tell
            // "resting on the rim with the tyre contact merely speculative" (rim ~= (R-tau)/R,
            // tyre >= 1.00R) from "floating clear of the ground with both speculative" (both
            // >= 1.00R). Those two want opposite fixes, so the summary max is not enough.
            // Nearest approach, not farthest: the closest point is the one that decides whether
            // anything is touching, and a manifold's other points are on the same body anyway.
            float distTyre = 0.0f;   // min tyre-record distance, in units of R (0 = none seen)
            float distRim  = 0.0f;   // min rim-record  distance, in units of R (0 = none seen)
            // penRaw over ALL tyre records INCLUDING the rejected ones, signed. dg.penRaw above
            // is a max over survivors only, so it reads 0.00000 whenever nothing survives - which
            // is precisely the case under investigation, and precisely when the number is needed.
            // Signed and in metres it says HOW FAR short the band falls: -0.005 is a wheel hanging
            // 5 mm clear, -0.00001 is a seam/epsilon problem. Different faults, same survived=0.
            float penRawAny    = 0.0f;
            bool  penRawAnySet = false;
            // Whether this wheel was SKIPPED because its body is asleep. Without it the struct
            // keeps whatever the last awake step wrote, and a frozen snapshot is indistinguishable
            // from a live one - which is exactly how a parked vehicle read as "the band engages,
            // survived=1" while the §59/63 summary for the same step read wheelsWithContact=0.
            // A diagnostic that silently reports stale data is worse than one that reports nothing.
            bool asleep = false;
            // docs §125 debug taps (standstill-creep investigation) - straight passthrough of
            // GeneralizedContactForce's own dbg_* fields for the GROUND slot, whichever wheel
            // last had one. Temporary instrumentation, not a permanent diagnostic contract.
            float dbg_v_par = 0.0f, dbg_v_lat = 0.0f, dbg_stick = 0.0f;
            float dbg_capPar = 0.0f, dbg_damperPar = 0.0f, dbg_D = 0.0f;
            // docs §124 step 3: the NEW path's own normal force (WheelContactConstraint's solved
            // impulse/dt), read back into the SAME wheel this step for a like-for-like comparison
            // against fnGround/fnObst/fnSide above - the step-3 verification gate. Stay 0 when the
            // flag is off or a slot has no contact, same convention as the fields above.
            float fnGroundNew = 0.0f, fnObstNew = 0.0f, fnSideNew = 0.0f;
            int   dbgGroundReason = -9;   // docs §124.7 debug (temporary) - see WheelContactConstraint::DbgGroundReason
            uint32_t dbgWarmStartCalls = 0, dbgSolveCalls = 0;   // docs §124.7 debug (temporary)
            uint32_t dbgBuildIslandsCalls = 0;   // docs §124.11 debug (temporary)
            float posY = 0.0f, chassisPosY = 0.0f;   // docs §124.12 debug (temporary)
            float posX = 0.0f, posZ = 0.0f;          // docs §124.12 debug (temporary)
            bool chassisActive = false;              // docs §124.12 debug (temporary)
            float wheelAngVel = 0.0f, wheelLinVel = 0.0f;   // docs §124.12 debug (temporary)
            bool userDataTagOk = false; uint64_t userDataRaw = 0;   // docs §124.12 debug (temporary)
            const void* dbgThis = nullptr;   // docs §124.12 debug (temporary)
            float dbgPen = 0.0f, dbgVn = 0.0f;   // docs §124.7 debug (temporary)
            // docs §131 debug (temporary, §56 chassis-rests-on-terrain finding): chassis world
            // "up" vector's Y component - 1.0 = perfectly upright, 0.0 = on its side, -1.0 =
            // upside down. §130 confirmed the chassis (not any wheel) is in stable contact with
            // a static body, and §124.12 already showed chassisPosY sitting abnormally close to
            // wheel height - this checks whether that is a pure vertical sinking (chassis still
            // upright, just too low) or the vehicle actually landed tipped/rolled.
            float chassisUpY = 1.0f;
        };
        WheelDiag diag[kMaxHarvestWheels];

        // docs §124 step 2: defined out-of-line, after WheelContactConstraint - the body now
        // pushes each wheel's freshly-computed contact geometry into that wheel's constraint
        // (CacheContact/ClearContact), which requires WheelContactConstraint to be a complete
        // type at the call site. See the forward declaration above for why the split is needed.
        void OnStep(const JPH::PhysicsStepListenerContext& inContext) override;
    };

    // docs §124 step 1: settings shim for WheelContactConstraint below. Empty on purpose - this
    // project never round-trips physics state through Jolt's own object-stream serialization
    // (the shadow is rebuilt from the game's own state every time, not saved/loaded), so none of
    // ConstraintSettings' serialization machinery is exercised. It exists only because
    // JPH::Constraint's constructor requires a ConstraintSettings reference.
    class WheelContactConstraintSettings final : public JPH::ConstraintSettings {
    };

    // docs §124 step 2: wheel ground-contact as a real Jolt Constraint. Registered alongside the
    // strut in BuildWheelBodies. Setup/Solve are STILL no-ops behaviourally (steps 3-4 give them
    // real work) - what step 2 adds is the cache: VehicleStepListener::OnStep, every step, pushes
    // the SAME cts/gm/slots/centre/axleWorld/omega it has always computed for the old AddForce
    // path into whichever wheel's constraint is live (CacheContact below), so a later step's
    // Setup/Solve has real per-wheel contact geometry on hand instead of needing to recompute or
    // re-detect anything. The old AddForce path (VehicleStepListener::applyTo) remains the sole
    // source of contact force until step 4.
    //
    // Why OnStep produces and this constraint merely consumes, rather than the reverse the plan
    // doc originally sketched: read literally, Jolt's PhysicsSystem.cpp job graph makes "compute
    // in SetupVelocityConstraint, read from OnStep" produce a EXTRA full step of staleness, not
    // zero - step.mStepListeners has NO dependency on step.mSetupVelocityConstraints, but
    // step.mDetermineActiveConstraints (and through it step.mSetupVelocityConstraints) DOES
    // depend on step.mStepListeners finishing first (see the CreateJob calls for both, same
    // file). So within one step, OnStep always completes before this constraint's
    // SetupVelocityConstraint even begins - there is no way for OnStep to read a same-step
    // product of Setup. Producing here and consuming in Setup (this file's actual shape) keeps
    // the cache exactly as fresh as the old AddForce path already is (still one step behind the
    // contact listener's own narrow-phase, per docs §124.1's framing note - untouched), while
    // still landing the geometry in the constraint in time for Setup/Solve (steps 3-4) to use it
    // later in the SAME step, which is the property that actually matters for those steps.
    //
    // Plain JPH::Constraint, not JPH::TwoBodyConstraint: which body the wheel is touching
    // changes frame to frame (different props, terrain vs. a bridge deck, nothing at all while
    // airborne...) and TwoBodyConstraint binds both bodies permanently at construction. Steps 3-4
    // resolve body2 fresh by BodyID each step instead of storing it - the same shape Jolt's own
    // JPH::VehicleConstraint uses for its per-wheel contacts (docs §124.1), even though detection
    // itself stays on this codebase's existing ContactListener/harvest-buffer pipeline rather
    // than gaining a second PhysicsStepListener role.
    class WheelContactConstraint final : public JPH::Constraint {
    public:
        JPH_OVERRIDE_NEW_DELETE

        WheelContactConstraint(JPH::Body& inWheelBody, const WheelContactConstraintSettings& inSettings)
            : JPH::Constraint(inSettings), mWheelBody(&inWheelBody) { }

        // docs §124 step 2: static per-wheel identity, set once right after construction from
        // BuildWheelBodies' second pass - same source (state.wheelSetup[w] / the body itself),
        // same cadence as VehicleStepListener::wheels[w]. Deliberately duplicated here rather
        // than read through the listener by index at solve time: that would let a leaked/
        // abandoned constraint from a prior vehicle rebuild silently start reading whatever the
        // NEXT vehicle's build overwrites that slot with, instead of continuing to (harmlessly)
        // describe its own, now-orphaned wheel. Owning its own copy keeps a leaked constraint's
        // worst case "frozen, self-consistent, eventually inert" - the same failure shape this
        // file already accepts for the leaked strut/body themselves (docs §124.1).
        void SetStatic(JPH::PhysicsSystem* inPhysics, JPH::Body* inChassis, uint32_t inSlot,
                uint32_t inWheelIndex, float inRadius, float inTau, float inMass, float inInertia,
                JPH::Vec3Arg inAxleLocal, const kraken::fix::wheelmodel::WMParams& inParams) {
            mPhysics    = inPhysics;
            mChassis    = inChassis;
            mSlot       = inSlot;
            mWheelIndex = inWheelIndex;
            mRadius     = inRadius;
            mTau        = inTau;
            mMass       = inMass;
            mInertia    = inInertia;
            mAxleLocal  = inAxleLocal;
            mParams     = inParams;
        }

        // docs §124: which slot each of the 3 parallel per-contact solves below belongs to -
        // ground/obstacle get the RADIAL weight (wr), side gets the LATERAL one (wl) (docs
        // §124's OnStep comment on why that split cannot be collapsed into one shared call).
        enum ESlotKind { kGround = 0, kObstacle = 1, kSide = 2, kNumSlotKinds = 3 };

        // docs §124 step 2: pushed by VehicleStepListener::OnStep once per step, right after it
        // computes this SAME data for the old AddForce path (see the class comment above for why
        // production has to stay in OnStep). nRec/cts/gm/others mirror OnStep's own locals
        // exactly; slots/centre/axleWorld/omega likewise.
        void CacheContact(const kraken::fix::wheelmodel::WMContact* inCts,
                const kraken::fix::wheelmodel::WMGeom* inGm, const JPH::BodyID* inOthers,
                uint32_t inNumRecs, const kraken::fix::wheelmodel::WMSlots& inSlots,
                const kraken::fix::wheelmodel::vec3& inCentre,
                const kraken::fix::wheelmodel::vec3& inAxleWorld, float inOmega) {
            mNumRecs = std::min(inNumRecs, kMaxHarvestRecs);
            for (uint32_t i = 0; i < mNumRecs; ++i) {
                mCts[i]    = inCts[i];
                mGm[i]     = inGm[i];
                mOtherIds[i] = inOthers[i];
            }
            mSlots      = inSlots;
            mCentre     = inCentre;
            mAxleWorld  = inAxleWorld;
            mOmega      = inOmega;
            mHasContact = true;
        }
        // Pushed instead of CacheContact whenever OnStep's own loop would have `continue`d for
        // this wheel (asleep, or nRec==0) - clearing FIRST is what keeps a skipped step from
        // reading last step's geometry as if it were still current (same principle as
        // WheelDiag's own asleep-clears-first rule, docs §66).
        void ClearContact() {
            mNumRecs    = 0;
            mSlots      = kraken::fix::wheelmodel::WMSlots();
            mHasContact = false;
        }

        // docs §124 step 3: the new path's own normal force for slot `inSlot`, in Newtons - a
        // read-only diagnostic tap so OnStep can log it alongside the old path's fnGround/fnObst/
        // fnSide (docs §66) for the step-3 verification gate ("matched within ~1%"). Impulse/dt,
        // not the spring's target force: this is what the solver actually applied, warm-start
        // carry-over and clamping included, which is the honest quantity to compare against a
        // force that was itself just applied via AddForce.
        float GetSlotNormalForce(ESlotKind inSlot, float inDt) const {
            if (inSlot < 0 || inSlot >= kNumSlotKinds || !mSlotActive[inSlot] || inDt <= 0.0f)
                return 0.0f;
            return mNormalPart[inSlot].GetTotalLambda() / inDt;
        }

        virtual JPH::EConstraintSubType GetSubType() const override { return JPH::EConstraintSubType::User1; }

        virtual bool IsActive() const override {
            return JPH::Constraint::IsActive() && mWheelBody->IsActive();
        }

        virtual void NotifyShapeChanged(const JPH::BodyID&, JPH::Vec3Arg) override { }
        // A real reset, not a no-op: stale cached contact or accumulated impulse from before a
        // Jolt-level ResetWarmStart (shape change, teleport) must not bias next step's solve or
        // be read as still current.
        virtual void ResetWarmStart() override {
            mHasContact = false;
            for (int s = 0; s < kNumSlotKinds; ++s) {
                mNormalPart[s].Deactivate();
                mSlotActive[s] = false;
            }
        }

        // docs §124 step 3: real normal axis, one JPH::AxisConstraintPart per slot (ground/
        // obstacle/side - docs §124's OnStep comment on why a lone shared call would double-count
        // side contacts). Not an approximation: the harvest-site clamp (cts[].depth =
        // min(penRaw, tau), still applied in OnStep before CacheContact) already makes
        // wm::GeneralizedContactForce's live F_n = k_t*pen - c_t*v_n in production (delta_hard is
        // identically zero, docs §124.1) - exactly the spring CalculateConstraintPropertiesWithStiffnessAndDamping
        // computes. Friction stays on the old AddForce path (VehicleStepListener::applyTo) until
        // step 4.
        //
        // C = -pen, not +pen: AxisConstraintPart's axis points body1->body2 (here:
        // other->wheel, matching the harvest normal's own "out of the surface toward the wheel"
        // convention, docs §59/§63), and its C is the standard "separation along axis" a spring
        // constraint expects everywhere else in Jolt (SixDOFConstraint's own translation motor,
        // DistanceConstraint, ...) - positive when pulling apart, negative when overlapping. A
        // wheel penetrating BY pen is overlapping by pen, i.e. C=-pen; feeding +pen would aim the
        // spring's restoring force the wrong way (pull the wheel INTO the ground, harder the more
        // it penetrates - unstable in the wrong direction, not merely wrong-magnitude, so a sign
        // slip here does not hide quietly). Derived twice independently (constraint-equation
        // geometry AND the sphere/manifold relation penRaw = R - (c-p)·n) before trusting it -
        // see the docs §124.7 write-up for both derivations.
        virtual void SetupVelocityConstraint(float inDeltaTime) override {
            for (int s = 0; s < kNumSlotKinds; ++s)
                mSlotActive[s] = false;
            // docs §124.7 debug (temporary, step-3 bring-up): records WHY the ground slot did or
            // didn't activate this call - removed once the F_n comparison gate is clean.
            mDbgGroundReason = 1;   // 1 = no mHasContact / no mPhysics
            if (!mHasContact || mPhysics == nullptr)
                return;
            const int idxBySlot[kNumSlotKinds] = { mSlots.ground, mSlots.obstacle, mSlots.side };
            const float cSpring = 2.0f * mParams.zeta_t * sqrtf(std::max(mParams.k_t * mMass, 0.0f));
            const float damping = (mTau > 0.0f) ? cSpring : 0.0f;
            for (int s = 0; s < kNumSlotKinds; ++s) {
                const int idx = idxBySlot[s];
                if (s == kGround) mDbgGroundReason = 2;   // 2 = idx out of range
                if (idx < 0 || (uint32_t) idx >= mNumRecs)
                    continue;
                const JPH::Vec3 n(mCts[idx].n.x, mCts[idx].n.y, mCts[idx].n.z);
                if (s == kGround) mDbgGroundReason = 3;   // 3 = degenerate normal
                if (n.LengthSq() < 1.0e-8f)
                    continue;
                // Resolved fresh by BodyID, not cached across steps - see WheelHarvestRec::other
                // and the class comment above for why. A vanished body (removed since harvest)
                // is treated as no contact this step, same as an empty slot.
                if (s == kGround) mDbgGroundReason = 4;   // 4 = other body not resolved
                JPH::Body* other = mPhysics->GetBodyLockInterfaceNoLock().TryGetBody(mOtherIds[idx]);
                if (other == nullptr)
                    continue;
                const JPH::RVec3 p(mCts[idx].p.x, mCts[idx].p.y, mCts[idx].p.z);
                // r1: the actual harvest point relative to the OTHER body - correct as-is, no
                // radius concept applies to whatever the wheel is touching.
                const JPH::Vec3 r1 = JPH::Vec3(p - other->GetCenterOfMassPosition());
                // r2: NOT the harvest point relative to the wheel's centre (measured live,
                // discovered wrong - see below) - reconstructed as exactly -R*n instead.
                //
                // AxisConstraintPart's own jv includes the wheel's FULL point velocity, spin
                // included (r2 x axis, dotted with angular velocity) - unlike the old model's
                // v_p, which explicitly subtracts the spin term back out (docs §63's vpAt
                // lambda). For a mathematically exact radial r2 (r2 parallel to n) that is
                // harmless: (axle x r2) is perpendicular to r2 by the cross product's own
                // definition, hence perpendicular to n too, so spin cannot project onto the
                // normal axis regardless of spin rate - the algebra this constraint's whole
                // "just use AxisConstraintPart's own jv" approach leans on.
                //
                // Measured live (docs §124.7 debug build, Bug01 idle settle): using the RAW
                // harvest point instead (p - wheelCentre, which is only ~radial - a mesh contact
                // point is the closest point on a TRIANGLE, not exactly R from centre along n)
                // broke that guarantee. pen stayed pinned at tau (0.01024, unmoving - no real
                // separation) while the constraint's own read of vn climbed to +0.6 up to
                // +6.5 m/s over 20s and kept climbing with wheel spin - a wheel actually
                // separating at 6.5 m/s would have LOST contact in milliseconds, not held pen
                // constant, so that reading was spin leaking through a non-exact r2, not real
                // velocity. Every SolveVelocityConstraint call (thousands measured) then saw a
                // huge false "opening" signal, drove lambda negative, and the [0,+inf) unilateral
                // clamp floored it at exactly 0 every single time - a real bug, not a rare edge
                // case, since any driven/rolling wheel has non-trivial spin. Forcing r2 = -R*n
                // removes the possibility by construction instead of trusting mesh precision.
                const JPH::Vec3 r2 = -mRadius * n;
                const float pen = mGm[idx].pen;   // already tau-clamped at the OnStep harvest seam
                mNormalPart[s].CalculateConstraintPropertiesWithStiffnessAndDamping(
                    inDeltaTime, *other, r1, *mWheelBody, r2, n, /*inBias=*/0.0f, /*inC=*/-pen,
                    mParams.k_t, damping);
                mOtherBody[s]  = other;
                mSlotNormal[s] = n;
                mSlotActive[s] = mNormalPart[s].IsActive();
                if (s == kGround) {
                    mDbgGroundReason = mSlotActive[s] ? 0 : 5;   // 0=ok, 5=Calculate deactivated it
                    // docs §124.7 debug (temporary): velocity at the SAME reconstructed point r2
                    // feeds the solver's own jv, so this must use it too - reading the raw
                    // harvest point here (as the first cut of this diagnostic did) is what showed
                    // the spin-leakage bug in the first place (see the r2 comment above).
                    mDbgPen = pen;
                    mDbgVn  = mWheelBody->GetPointVelocity(JPH::RVec3(mWheelBody->GetCenterOfMassPosition() + r2)).Dot(n);
                }
            }
        }
        int DbgGroundReason() const { return mDbgGroundReason; }
        uint32_t DbgWarmStartCalls() const { return mDbgWarmStartCalls; }
        uint32_t DbgSolveCalls() const { return mDbgSolveCalls; }
        float DbgPen() const { return mDbgPen; }
        float DbgVn() const { return mDbgVn; }
        // docs §124.11 debug (temporary, Ural01 cold-pin hang bring-up): BuildIslands runs on a
        // DIFFERENT path than SetupVelocityConstraint - reason stuck at -9 could mean either
        // "BuildIslands never ran" (constraint never reached by the physics system this step) or
        // "BuildIslands ran but the solver still skipped Setup" (island built, something after
        // rejects it). This counter distinguishes the two without guessing.
        uint32_t DbgBuildIslandsCalls() const { return mDbgBuildIslandsCalls; }
        // docs §124.12 debug (temporary): mConstraintIndex on the JPH::Constraint base is
        // private (only ConstraintManager is a friend) - use this object's OWN address instead
        // to cross-check identity against the vendored-Jolt scan trace (kraken §124.12: scanned
        // User1 constraint ptr=...) directly, not by index guesswork.
        const void* DbgThis() const { return this; }

        virtual void WarmStartVelocityConstraint(float inWarmStartImpulseRatio) override {
            ++mDbgWarmStartCalls;   // docs §124.7 debug (temporary)
            for (int s = 0; s < kNumSlotKinds; ++s) {
                if (mSlotActive[s] && mOtherBody[s] != nullptr)
                    mNormalPart[s].WarmStart(*mOtherBody[s], *mWheelBody, mSlotNormal[s], inWarmStartImpulseRatio);
            }
        }

        virtual bool SolveVelocityConstraint(float inDeltaTime) override {
            ++mDbgSolveCalls;   // docs §124.7 debug (temporary)
            bool any = false;
            for (int s = 0; s < kNumSlotKinds; ++s) {
                if (!mSlotActive[s] || mOtherBody[s] == nullptr)
                    continue;
                // Unilateral contact: the band can only PUSH (min=0), never pull - the same
                // fmaxf(0, ...) the old path's F_n applies, just enforced as an impulse bound
                // instead of a post-hoc clamp.
                if (mNormalPart[s].SolveVelocityConstraint(
                        *mOtherBody[s], *mWheelBody, mSlotNormal[s], 0.0f, FLT_MAX))
                    any = true;
                // docs §124 step 4: friction, evaluated AFTER the normal axis for THIS SAME
                // iteration - reacts to whatever this iteration's normal impulse just did to the
                // velocity, not a stale pre-step guess (the whole point of moving it off AddForce).
                //
                // docs §138 (task #60): gating this to fire ONCE per step (first iteration only)
                // was TRIED and REFUTED - live-measured on Molokovoz01 standstill, it did not
                // shrink the creep, it turned a brief self-correcting spike into an UNBOUNDED
                // runaway (omega climbed past 18 rad/s over the scenario instead of the ~0.01
                // baseline). So the repeated per-iteration reapplication is not over-application -
                // it is the mechanism actually holding the wheel still, converging jointly with
                // mNormalPart across the ~20 iterations the same way a real PGS solve is supposed
                // to. Left as ORIGINAL (every iteration). The real cause of the creep, and its
                // fix, ended up living in a completely different place - see docs §137/§137.3 in
                // UpdateOneVehiclePostStep (a visual writeback-ordering bug, not a solver one).
                ApplyFrictionForSlot(s, inDeltaTime);
            }
            return any;
        }
        virtual bool SolvePositionConstraint(float /*inDeltaTime*/, float /*inBaumgarte*/) override { return false; }

        // docs §124 step 4: the SAME wm::GeneralizedContactForce as the old path (engine-agnostic
        // core untouched, per the plan), called per solver iteration against LIVE velocity instead
        // of once per step against a pre-step snapshot. Only the tangential share is applied here -
        // the normal share is redundant with (and would double-count) mNormalPart's own impulse,
        // computed above in the SAME call.
        //
        // Applied to the WHEEL body only, matching the old AddForce path's own scope exactly
        // ("The WHEEL body, not the chassis", docs §59/63) - a deliberate, documented asymmetry
        // from the normal axis (which DOES react on a dynamic body1 through AxisConstraintPart):
        // fidelity to what the friction model has always done, not a new limitation step 4
        // introduces. A dynamic body1 (a loose prop, say) still gets no frictional reaction force
        // here, same as it always has.
        void ApplyFrictionForSlot(int inSlot, float inDt) {
            if (!mWheelBody->IsDynamic())
                return;
            const int idxBySlot[kNumSlotKinds] = { mSlots.ground, mSlots.obstacle, mSlots.side };
            const int idx = idxBySlot[inSlot];
            if (idx < 0 || (uint32_t) idx >= mNumRecs)
                return;
            namespace wm = kraken::fix::wheelmodel;
            const wm::vec3& n = mCts[idx].n;
            const JPH::Vec3 nJ(n.x, n.y, n.z);
            // Same exactly-radial reconstruction as the normal axis (docs §124.7's r2 fix) - not
            // just for consistency, but for the same reason: GeneralizedContactForce internally
            // re-derives r = p - c from p/c to subtract the spin term back in a controlled way
            // (docs §63's v_c = v_p + Cross(a*omega, r)), and that derivation wants the same
            // exact-radial r this axis already established is required for a clean result.
            const JPH::Vec3 r2 = -mRadius * nJ;
            const JPH::RVec3 p = mWheelBody->GetCenterOfMassPosition() + r2;
            const JPH::Vec3 axleWorldJ(mAxleWorld.x, mAxleWorld.y, mAxleWorld.z);
            const JPH::Vec3 chassisAngVel = (mChassis != nullptr) ? mChassis->GetAngularVelocity() : JPH::Vec3::sZero();
            // Live omega, re-read every iteration - NOT the mOmega CacheContact captured before
            // this step even began (docs §124.6's producer/consumer note: that value is already
            // one OnStep-to-Setup hop old by the time Setup runs, let alone by later Solve calls).
            const float omega = (mWheelBody->GetAngularVelocity() - chassisAngVel).Dot(axleWorldJ);
            const JPH::Vec3 vFull = mWheelBody->GetPointVelocity(p);
            const JPH::Vec3 vp = vFull - (axleWorldJ * omega).Cross(r2);   // docs §63 vpAt, live

            const wm::vec3 pW{ (float) p.GetX(), (float) p.GetY(), (float) p.GetZ() };
            const wm::vec3 vpW{ vp.GetX(), vp.GetY(), vp.GetZ() };
            const wm::vec3 aW{ axleWorldJ.GetX(), axleWorldJ.GetY(), axleWorldJ.GetZ() };
            const float weight = (inSlot == kSide) ? mGm[idx].wl : mGm[idx].wr;   // docs §124's OnStep comment on why
            wm::WMParams P = mParams;
            P.inertia = mInertia;

            const wm::WMForce f = wm::GeneralizedContactForce(pW, n, mGm[idx].pen, weight,
                mCentre, aW, vpW, omega, mRadius, mTau, mMass, inDt, P);
            const JPH::Vec3 F(f.F.x, f.F.y, f.F.z);
            if (!std::isfinite(F.GetX()) || !std::isfinite(F.GetY()) || !std::isfinite(F.GetZ()))
                return;
            const JPH::Vec3 Ftang = F - nJ * F.Dot(nJ);
            const JPH::Vec3 impulse = Ftang * inDt;
            JPH::MotionProperties* mp = mWheelBody->GetMotionPropertiesUnchecked();
            mp->AddLinearVelocityStep(impulse * mp->GetInverseMass());
            mp->AddAngularVelocityStep(
                mp->MultiplyWorldSpaceInverseInertiaByVector(mWheelBody->GetRotation(), r2.Cross(impulse)));
        }

        // Mirrors JPH::TwoBodyConstraint::BuildIslands (TwoBodyConstraint.cpp) for the
        // single-body case: only the wheel exists as a stable body reference at step 1, so it is
        // the only one linked. A dynamic wheel body still needs activating/linking into the
        // island so this constraint is genuinely part of the solve, not a no-op by omission -
        // that distinction is exactly what step 1's perf gate (docs §124.2) is measuring.
        virtual void BuildIslands(uint32_t inConstraintIndex, JPH::IslandBuilder& ioBuilder,
                JPH::BodyManager& inBodyManager) override {
            ++mDbgBuildIslandsCalls;   // docs §124.11 debug (temporary): does this even run?
            if (mWheelBody->IsDynamic()) {
                if (!mWheelBody->IsActive()) {
                    JPH::BodyID id = mWheelBody->GetID();
                    inBodyManager.ActivateBodies(&id, 1);
                }
                ioBuilder.LinkConstraint(inConstraintIndex, mWheelBody->GetIndexInActiveBodiesInternal());
            }
        }

        virtual JPH::uint BuildIslandSplits(JPH::LargeIslandSplitter& ioSplitter) const override {
            return ioSplitter.AssignToNonParallelSplit(mWheelBody);
        }

#ifdef JPH_DEBUG_RENDERER
        virtual void DrawConstraint(JPH::DebugRenderer*) const override { }
#endif

        virtual JPH::Ref<JPH::ConstraintSettings> GetConstraintSettings() const override {
            return new WheelContactConstraintSettings();
        }

    private:
        JPH::Body* mWheelBody;

        // docs §124 step 2 statics - see SetStatic.
        JPH::PhysicsSystem* mPhysics    = nullptr;
        JPH::Body*           mChassis    = nullptr;
        uint32_t             mSlot       = 0;
        uint32_t             mWheelIndex = 0;
        float                mRadius     = 0.3f;
        float                mTau        = 0.01f;
        float                mMass       = 10.0f;
        float                mInertia    = 1.0f;
        JPH::Vec3             mAxleLocal{ 1.0f, 0.0f, 0.0f };
        kraken::fix::wheelmodel::WMParams mParams;

        // docs §124 step 2 cache - see CacheContact/ClearContact. Written by OnStep, read by
        // SetupVelocityConstraint, later in the SAME step.
        bool                                mHasContact = false;
        uint32_t                            mNumRecs    = 0;
        kraken::fix::wheelmodel::WMContact  mCts[kMaxHarvestRecs];
        kraken::fix::wheelmodel::WMGeom     mGm[kMaxHarvestRecs];
        JPH::BodyID                         mOtherIds[kMaxHarvestRecs];   // docs §124 step 3
        kraken::fix::wheelmodel::WMSlots    mSlots;
        kraken::fix::wheelmodel::vec3       mCentre;
        kraken::fix::wheelmodel::vec3       mAxleWorld;
        float                               mOmega = 0.0f;

        // docs §124 step 3: per-slot (ground/obstacle/side) solver state, valid for the duration
        // of one step once SetupVelocityConstraint has run (WarmStart/Solve are called multiple
        // times per step - these must survive across those calls, unlike the locals above).
        JPH::AxisConstraintPart mNormalPart[kNumSlotKinds];
        JPH::Body*              mOtherBody[kNumSlotKinds] = {};
        bool                    mSlotActive[kNumSlotKinds] = {};
        JPH::Vec3               mSlotNormal[kNumSlotKinds];
        // docs §124.7 debug (temporary): -9 means SetupVelocityConstraint has never run at all
        // for this constraint - distinguishing "never called" from "called but bailed" is the
        // whole point of this field. See DbgGroundReason().
        int                     mDbgGroundReason = -9;
        uint32_t                mDbgWarmStartCalls = 0;
        uint32_t                mDbgSolveCalls      = 0;
        float                   mDbgPen = 0.0f, mDbgVn = 0.0f;
        uint32_t                mDbgBuildIslandsCalls = 0;   // docs §124.11 debug (temporary)
    };

    // docs §124 step 2: VehicleStepListener::OnStep, defined out-of-line (declared above,
    // immediately after WheelContactConstraint's WheelDiag-shaped diagnostics) now that
    // WheelContactConstraint is a complete type. Identical to the step-1 body except for the
    // CacheContact/ClearContact calls marked below - the old AddForce path is untouched, still
    // the sole source of contact force.
    void VehicleStepListener::OnStep(const JPH::PhysicsStepListenerContext& inContext) {
        namespace wm = kraken::fix::wheelmodel;
        const float dt = std::max(inContext.mDeltaTime, 1.0e-6f);
        // Read the buffer the PREVIOUS step's narrow phase filled - see StepPhysicsProfiled
        // for why the parity is advanced on the main thread and why this is the other one.
        const uint32_t readParity = (g_harvestStep.load(std::memory_order_relaxed) ^ 1u) & 1u;
        const JPH::Vec3 chassisAngVel = (chassis != nullptr) ? chassis->GetAngularVelocity() : JPH::Vec3::sZero();

        uint32_t withContact = 0, points = 0, overflow = 0, survived = 0;
        float    maxNormalF = 0.0f;
        for (uint32_t w = 0; w < wheelCount && w < kMaxHarvestWheels; ++w) {
            const WheelTick& wt = wheels[w];
            WheelContactConstraint* cc = contactConstraints[w];   // docs §124 step 2, may be nullptr
            if (wt.body == nullptr)
                continue;
            // AddForce on a sleeping body is a no-op, so an ungated loop either wakes every
            // vehicle every step or silently stops driving one that fell asleep. Neither
            // failure announces itself.
            if (!wt.body->IsActive()) {
                // Clear FIRST, then mark. Leaving the previous step's numbers in place is what
                // made a sleeping vehicle look like a working band (see WheelDiag::asleep).
                if (w < kMaxHarvestWheels) {
                    diag[w] = WheelDiag();
                    diag[w].asleep = true;
                }
                if (cc != nullptr)
                    cc->ClearContact();
                continue;
            }
            WheelHarvest& buf = g_wheelHarvest[readParity][slot][w];
            const uint32_t n    = buf.count.load(std::memory_order_relaxed);
            const uint32_t used = std::min(n, kMaxHarvestRecs);
            if (n > kMaxHarvestRecs)
                overflow += n - kMaxHarvestRecs;
            if (n > 0)
                ++withContact;
            points += n;

            const JPH::RVec3 centre = wt.body->GetCenterOfMassPosition();
            // The RELATIVE spin about the axle. Relative, not absolute: a chassis yawing under
            // the wheel would otherwise read as wheel rotation and invent slip out of the
            // vehicle's own turn. The same omega must be used both here and in the v_p
            // subtraction below, or the two halves describe different wheels.
            const JPH::Vec3 axleWorld = (wt.body->GetRotation() * wt.axleLocal).Normalized();
            const float omega = (wt.body->GetAngularVelocity() - chassisAngVel).Dot(axleWorld);

            WheelDiag& dg = diag[w];
            dg = WheelDiag();
            dg.pen = 0.0f;
            // docs §124.12 debug (temporary, Ural01 hang): set UNCONDITIONALLY, before the
            // nRec==0 early continue below - the user's live observation was wheels visibly
            // under the ground, and every other new-path diagnostic field gets skipped by that
            // continue, so this is the one number in this whole investigation guaranteed to
            // still be captured on every "no contact detected" step.
            dg.posY = (float) centre.GetY();
            dg.posX = (float) centre.GetX();
            dg.posZ = (float) centre.GetZ();
            dg.chassisPosY = (chassis != nullptr) ? (float) chassis->GetCenterOfMassPosition().GetY() : 0.0f;
            dg.chassisActive = (chassis != nullptr) && chassis->IsActive();
            dg.chassisUpY = (chassis != nullptr)
                ? (chassis->GetRotation() * JPH::Vec3(0.0f, 1.0f, 0.0f)).GetY() : 1.0f;
            dg.wheelAngVel = wt.body->GetAngularVelocity().Length();
            dg.wheelLinVel = wt.body->GetLinearVelocity().Length();
            // docs §124.12 debug (temporary, Ural01 hang): HarvestWheelContact bails out
            // silently (before it ever sets mIsSensor on the tyre or zeroes rim friction) if
            // IsWheelUserData() reads false on this body at CONTACT time - checking that same
            // read from here, not guessing whether construction-time SetUserData "should" have
            // stuck.
            dg.userDataTagOk = IsWheelUserData(wt.body->GetUserData());
            dg.userDataRaw   = wt.body->GetUserData();

            wm::WMContact cts[kMaxHarvestRecs];
            wm::WMGeom    gm[kMaxHarvestRecs];
            JPH::BodyID   others[kMaxHarvestRecs];   // docs §124 step 3, parallel to cts/gm
            uint32_t      nRec = 0;   // NOT `n` - that is the raw harvested count above
            for (uint32_t r = 0; r < used && nRec < kMaxHarvestRecs; ++r) {
                const WheelHarvestRec& rec = buf.rec[r];
                // Only the TYRE sub-shape feeds the band. A RIM record is harvested and
                // counted, but must never produce band force: the solver already resolves the
                // rim contact, so using it here would apply the normal load twice.
                // Counted BEFORE the survival test, and split by sub-shape, because "no
                // tyre records arrived" and "tyre records arrived and were all rejected" are
                // different faults with the same outward symptom.
                const float dist  = JPH::Vec3(centre - JPH::RVec3(rec.point)).Length();
                const float distR = dist / std::max(wt.radius, 1e-6f);
                dg.distMax = std::max(dg.distMax, distR);
                if (rec.sub == 0) {
                    ++dg.recsTyre;
                    dg.distTyre = (dg.distTyre == 0.0f) ? distR : std::min(dg.distTyre, distR);
                } else {
                    ++dg.recsRim;
                    dg.distRim  = (dg.distRim  == 0.0f) ? distR : std::min(dg.distRim,  distR);
                }
                if (rec.sub != 0)
                    continue;
                // Reproject rather than trusting the manifold's own penetration depth: for a
                // sphere against a locally planar triangle this is exact, and it makes
                // lift-off self-handling - penRaw goes negative and the record simply drops,
                // with no OnContactRemoved bookkeeping to get wrong.
                const float penRaw = wt.radius - JPH::Vec3(centre - JPH::RVec3(rec.point)).Dot(rec.normal);
                // Captured BEFORE the test that discards it, and kept as the LEAST negative
                // (i.e. the closest this wheel came to engaging) rather than an average, which
                // a single far-away manifold point would drag away from the answer.
                if (!dg.penRawAnySet || penRaw > dg.penRawAny) {
                    dg.penRawAny    = penRaw;
                    dg.penRawAnySet = true;
                }
                if (penRaw <= 0.0f)
                    continue;
                ++survived;
                ++dg.survived;
                dg.penRaw = std::max(dg.penRaw, penRaw);
                dg.pen    = std::max(dg.pen, std::min(penRaw, wt.tau));
                cts[nRec].p = wm::vec3{ rec.point.GetX(), rec.point.GetY(), rec.point.GetZ() };
                cts[nRec].n = wm::vec3{ rec.normal.GetX(), rec.normal.GetY(), rec.normal.GetZ() };
                // The tau clamp lives HERE, at the seam, not inside the core - which is what
                // makes the core's own delta_hard = max(0, pen - tau) identically zero and
                // hard_core_lambda dead, exactly as plan §5 predicted.
                cts[nRec].depth = std::min(penRaw, wt.tau);
                others[nRec]    = rec.other;
                ++nRec;
            }
            if (nRec == 0) {
                if (cc != nullptr)
                    cc->ClearContact();
                continue;
            }

            const wm::vec3 c{ (float) centre.GetX(), (float) centre.GetY(), (float) centre.GetZ() };
            const wm::vec3 a{ axleWorld.GetX(), axleWorld.GetY(), axleWorld.GetZ() };
            const wm::vec3 up{ 0.0f, 1.0f, 0.0f };
            for (uint32_t h = 0; h < nRec; ++h)
                gm[h] = wm::ComputeGeom(cts[h], c, up, a, wt.radius, wt.radius);
            const wm::WMSlots slots = wm::Classify(gm, (int) nRec);

            // docs §124 step 2: pushed here, right after Classify and before any force is
            // computed - this is the exact preprocessing/force boundary the plan draws between
            // step 2 and step 3 (the first real reader of what gets cached here).
            if (cc != nullptr)
                cc->CacheContact(cts, gm, others, nRec, slots, c, a, omega);

            // docs §63 / plan §3.5: v_p is the wheel body's point velocity MINUS the spin term
            // the core is about to add back (v_c = v_p + Cross(a*omega, r)). Feeding
            // GetPointVelocity raw counts the spin twice - SelfTest[5] exists precisely to
            // catch that, and measures the error at 3069 N on a freely rolling wheel.
            auto vpAt = [&](const wm::vec3& pt) {
                const JPH::RVec3 pw(pt.x, pt.y, pt.z);
                const JPH::Vec3  v = wt.body->GetPointVelocity(pw);
                const JPH::Vec3  rr(pt.x - c.x, pt.y - c.y, pt.z - c.z);
                const JPH::Vec3  spin = (axleWorld * omega).Cross(rr);
                const JPH::Vec3  vp = v - spin;
                return wm::vec3{ vp.GetX(), vp.GetY(), vp.GetZ() };
            };

            wm::WMParams P = params;
            P.inertia = wt.inertia;   // the real disc inertia, not mode 2's flat constant

            // The WEIGHT differs by slot: radial (wr) for ground and obstacle, LATERAL (wl) for
            // side. Factoring the three calls into one lambda quietly collapsed that, and the
            // single shared `wr` is not a small error - Classify picks the side slot from ALL
            // records, not just non-ground ones, so a lone ground contact with any wl > 0 lands
            // in BOTH slots and, evaluated with the same weight, yields the SAME force twice.
            //
            // It measured exactly that: fnTotal = 1638 N against a 1638 N vehicle looked like a
            // perfect result, but fnGround was 819 N - a ratio of exactly 2.00, in every pass.
            // Statics hides this completely (a doubled spring just settles at half the
            // penetration and still sums to the weight), so only the ratio gave it away.
            // The mode-2 reference does it correctly; this is a porting error, not a design
            // difference, which is why the three calls are spelled out here as they are there.
            auto forceFor = [&](int idx, float w) -> wm::WMForce {
                if (idx < 0)
                    return wm::WMForce();
                return wm::GeneralizedContactForce(cts[idx].p, cts[idx].n, gm[idx].pen, w,
                    c, a, vpAt(cts[idx].p), omega, wt.radius, wt.tau, wt.mass, dt, P);
            };
            const wm::WMForce fG = forceFor(slots.ground,   slots.ground   >= 0 ? gm[slots.ground].wr   : 0.0f);
            const wm::WMForce fO = forceFor(slots.obstacle, slots.obstacle >= 0 ? gm[slots.obstacle].wr : 0.0f);
            const wm::WMForce fS = forceFor(slots.side,     slots.side     >= 0 ? gm[slots.side].wl     : 0.0f);
            dg.dbg_v_par = fG.dbg_v_par; dg.dbg_v_lat = fG.dbg_v_lat; dg.dbg_stick = fG.dbg_stick;
            dg.dbg_capPar = fG.dbg_capPar; dg.dbg_damperPar = fG.dbg_damperPar; dg.dbg_D = fG.dbg_D;

            // Deliberately NO maxForce cap here. Mode 2 clamps at jolt_wm_max_g * m * g with
            // m the per-corner SPRUNG mass (~33-42 kg), giving ~2000-2500 N. With m now the
            // UNSPRUNG mass (~9-11 kg) the same expression collapses to ~530-650 N, which is
            // below the band's own peak D = mu*k_t*tau = 2400 N - it would clamp ordinary
            // driving rather than outliers. Plan step 7 scheduled this removal as its own
            // measured step; in new code "not porting it" and "removing it" are one act, so
            // step 7 loses that independent variable and the plan needs to say so.
            auto applyTo = [&](const wm::WMForce& f, int idx) {
                if (idx < 0)
                    return;
                const JPH::Vec3 F(f.F.x, f.F.y, f.F.z);
                if (!std::isfinite(F.GetX()) || !std::isfinite(F.GetY()) || !std::isfinite(F.GetZ()))
                    return;
                // The NORMAL component, not |F|. The bound this is compared against, k_t*tau,
                // is a ceiling on the band's normal force only - friction adds a tangential
                // component on top, so |F| = sqrt(Fn^2 + Ft^2) routinely exceeds it with
                // nothing wrong. Tracking |F| here made the very first step-4 run report
                // "EXCEEDS GEOMETRIC BOUND" at ratio 1.28 on a vehicle that was standing
                // perfectly still: an instrument comparing two different quantities, not a
                // physics problem. Comparing like with like is what makes the flag mean
                // something.
                const JPH::Vec3 nrm(cts[idx].n.x, cts[idx].n.y, cts[idx].n.z);
                const float fn = F.Dot(nrm);
                maxNormalF = std::max(maxNormalF, fn);
                dg.fnTotal += fn;
                // Assigned by which SLOT is being applied, not by index: the same index can
                // legitimately occupy two slots, and attributing by index would hide exactly
                // the overlap these fields exist to show.
                if (&f == &fG)      dg.fnGround = fn;
                else if (&f == &fO) dg.fnObst   = fn;
                else                dg.fnSide   = fn;
                // docs §124 step 4: AddForce retired for any wheel on the new constraint path
                // (cc != nullptr) - normal comes from WheelContactConstraint's own
                // AxisConstraintPart, friction from its ApplyFrictionForSlot (both solved into
                // the SAME body's velocity, per solver iteration, later in this same step).
                // fn/fnGround/fnTotal/dg.* above stay computed from the full old-model force
                // regardless of cc - they are the diagnostic the step-3/4 gates compare against,
                // not what got applied. Old behaviour (flag off) is untouched: full F, still via
                // AddForce, exactly as every step before this one.
                if (cc == nullptr) {
                    // The WHEEL body, not the chassis. The chassis feels the load through the
                    // SixDOF, and the r x F torque about the wheel's own COM arises on its own
                    // and two-sidedly - which is the whole point of giving the wheel a body.
                    wt.body->AddForce(F, JPH::RVec3(cts[idx].p.x, cts[idx].p.y, cts[idx].p.z));
                }
            };
            applyTo(fG, slots.ground);
            applyTo(fO, slots.obstacle);
            applyTo(fS, slots.side);

            // docs §124 step 3: read-only diagnostic tap for the step-3 verification gate. This
            // reads WHATEVER the constraint's Setup/Solve last computed - one step behind this
            // step's own fnGround/fnObst/fnSide above, for the same reason CacheContact's
            // producer/consumer are inverted from the plan doc (see the class comment on
            // WheelContactConstraint). Fine for a multi-second settle comparison, the gate this
            // exists for; not a same-tick identity.
            if (cc != nullptr) {
                dg.fnGroundNew = cc->GetSlotNormalForce(WheelContactConstraint::kGround,   dt);
                dg.fnObstNew   = cc->GetSlotNormalForce(WheelContactConstraint::kObstacle, dt);
                dg.fnSideNew   = cc->GetSlotNormalForce(WheelContactConstraint::kSide,     dt);
                dg.dbgGroundReason = cc->DbgGroundReason();
                dg.dbgWarmStartCalls = cc->DbgWarmStartCalls();
                dg.dbgSolveCalls     = cc->DbgSolveCalls();
                dg.dbgBuildIslandsCalls = cc->DbgBuildIslandsCalls();
                dg.dbgThis = cc->DbgThis();
                dg.dbgPen = cc->DbgPen();
                dg.dbgVn  = cc->DbgVn();
            } else {
                // docs §124.11 debug (temporary, Ural01 hang): WheelDiag's own struct-literal
                // default for dbgGroundReason is ALSO -9 (see its declaration) - meaning "no cc
                // object this rebuild" and "cc exists but Jolt never solved it" were previously
                // indistinguishable in the log. -99 can only mean the former.
                dg.dbgGroundReason = -99;
            }
        }
        lastMaxNormalF = maxNormalF;
        lastWheelsWithContact = withContact;
        lastContactPoints     = points;
        lastOverflow          = overflow;
        lastSurvived          = survived;
    }

    // docs §58 (Этап 1, шаг 2): one real dynamic body per wheel, hung off the chassis by one
    // JPH::SixDOFConstraint. Called only under [jolt_harness] wheelmodel==4, and only after the
    // chassis body is live and InitWheelModelSuspension has filled wmRestLen - that is what
    // places each wheel, so the order is not incidental.
    //
    // What step 2 deliberately does NOT do: apply any force. The old StepWheelModel path keeps
    // driving the chassis exactly as in mode 2 (plan §7 step 2: «силы ядра не применяются»), and
    // the wheel bodies ride along as sensors. Anyone reading this expecting the vehicle to sink
    // once the bodies appear is wrong - if it sinks, that is a real defect, not the step working
    // as designed.
    static void BuildWheelBodies(JPH::PhysicsSystem* physics, JPH::Body* chassisBody,
            ShadowState& state, const char* label, uint32_t collisionGroupId) {
        using EAx = JPH::SixDOFConstraintSettings::EAxis;
        JPH::BodyInterface& bi = physics->GetBodyInterface();
        const kraken::Config& cfg = kraken::Config::Instance();

        const JPH::RMat44 chassisXform = chassisBody->GetWorldTransform();
        const JPH::Quat   chassisRot   = chassisBody->GetRotation();
        // The PRE-subtraction mass: this is `max(vehicle->GetMass(), 100.0f)`, and GetMass()
        // already includes the wheels (docs §95.4). Both the per-wheel mass floor and tau are
        // deliberately derived from it BEFORE the §96 subtraction below, because a threshold
        // guarding a wheel/vehicle ratio wants the whole-vehicle mass as its basis - and taking
        // it after would make the floor depend on the very sum it helps compute.
        const float chassisMass  = 1.0f / std::max(chassisBody->GetMotionProperties()->GetInverseMass(), 1.0e-8f);
        const float massFloorDiv = 40.0f;      // plan §2.1's guard against a pathological mass ratio
        const float massFloor    = chassisMass / massFloorDiv;
        const float gAbs         = std::max(std::fabs(cfg.gravity.value), 0.1f);
        const float kTyre        = std::max(cfg.jolt_wm_tyre_stiffness.value, 1.0f);
        const size_t nWheels     = state.wheelSetup.size();
        const float cornerMass   = chassisMass / std::max<float>((float) nWheels, 1.0f);

        // docs §66.9, applied PRE-EMPTIVELY rather than after it bites again. This file never
        // tears down an abandoned body (see BuildShadow's comment: doing so produced a repeatable
        // worker-thread crash), so a rebuild leaves the previous wheel bodies alive in the world
        // forever. That is tolerable for a chassis, but a leaked WHEEL still carries its 'WHL\0'
        // UserData with the same slot and index - so the step-3 contact harvest would pick up the
        // ghost alongside the real wheel and silently double its contacts. Untagging on
        // abandonment is what makes the leak harmless: the body stays, but nothing can mistake it
        // for a wheel again. Cheap, and the alternative is a contamination bug with no symptom.
        for (JPH::BodyID old : state.wheelBodies) {
            if (!old.IsInvalid())
                bi.SetUserData(old, 0);
        }
        state.wheelBodies.clear();
        state.wheelConstraints.clear();
        state.wheelContactConstraints.clear();
        state.wmDiscInertiaX.clear();
        state.wmDiscInertiaR.clear();
        state.wmSpinAngle.clear();
        state.wheelLockedRot.clear();
        state.dbgPrevVisualRot.clear();
        state.restHeld = false;   // docs §137.3 - a rebuilt vehicle has fresh bodies, nothing to hold yet

        float totalWheelMass = 0.0f;

        for (size_t i = 0; i < nWheels; ++i) {
            WheelSetup& ws = state.wheelSetup[i];

            // docs §136: real fix for the wheel-spin visual bug (task #58), replacing the
            // rejected §65.8 per-vehicle-name list. Confirmed live (both vehicles, angle-only
            // logged then, full quaternion captured now): Fighter01 reads identity on all 4
            // wheels, Molokovoz01 reads exactly 180 deg about Y on its 2 right wheels only -
            // this genuinely is a per-instance geometric fact, not something derivable from
            // attachPos or any vehicle name. See the field's own comment for the derivation
            // (visualRot = jointBodyRot * visualMirrorDelta reconstructs the native quaternion
            // because both engines apply the identical world-space spin on top of a different
            // starting pose).
            if (i < state.wheelOrder.size() && state.wheelOrder[i] != nullptr) {
                const hta::ai::Wheel* hw = state.wheelOrder[i];
                const hta::Quaternion nativeRot = hw->GetRotation();
                const JPH::Quat nativeRotJ(nativeRot.x, nativeRot.y, nativeRot.z, nativeRot.w);
                ws.visualMirrorDelta = (chassisRot.Inversed() * nativeRotJ).Normalized();
            }

            const JPH::RVec3 worldAttach  = chassisXform * ws.attachPos;
            const JPH::Vec3  worldSuspDir = (chassisRot * ws.suspDir).Normalized();
            // wmRestLen is the raycast-seeded spring zero-force length, so the wheel spawns where
            // the ground actually is rather than at mid-travel. Fall back to the middle of travel
            // only if the seed is missing - a wheel placed at full droop would start below the
            // one-sided heightfield surface (docs §40).
            const float restLen = (i < state.wmRestLen.size() && state.wmRestLen[i] > 0.0f)
                ? state.wmRestLen[i] : 0.5f * (ws.minLen + ws.maxLen);
            const JPH::RVec3 wheelCentreWorld = worldAttach + worldSuspDir * restLen;

            // The tyre band's half-thickness. Sized so the band can carry roughly three times a
            // corner's static weight before it saturates: k_t * tau is the force at full
            // penetration, so tau = 3*m_corner*g / k_t. Clamped from below so it never degenerates
            // and from above so the RIM sphere stays a meaningful fraction of the tyre.
            ws.tau = std::clamp(3.0f * cornerMass * gAbs / kTyre, 0.01f, 0.15f * ws.radius);

            // TYRE (sub-shape 0, radius R) and RIM (sub-shape 1, radius R-tau), concentric. The
            // band between them is where the tyre force lives; the rim is what stops the wheel
            // sinking through the world if the band ever saturates. Both are solid geometry here;
            // which of them acts as a sensor is a PER-PAIR decision taken in the contact callback
            // at step 3, never a property of the body.
            JPH::StaticCompoundShapeSettings compound;
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), new JPH::SphereShape(ws.radius));
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                              new JPH::SphereShape(std::max(ws.radius - ws.tau, 0.01f)));
            JPH::ShapeSettings::ShapeResult compoundResult = compound.Create();
            if (compoundResult.HasError()) {
                LOG_WARNING("Shadow (%s): wheel %zu compound shape failed: %s",
                    label, i, compoundResult.GetError().c_str());
                return;
            }

            const float bodyMass = std::max(ws.unsprungMass, massFloor);
            totalWheelMass += bodyMass;   // the FLOORED mass - this is the sum §96 subtracts

            // Spawned with the CHASSIS rotation, not identity, so wheel-local equals chassis-local
            // at build time and the constraint frame below can be expressed in either.
            JPH::BodyCreationSettings bcs(compoundResult.Get(), wheelCentreWorld, chassisRot,
                                          JPH::EMotionType::Dynamic, kWheelLayer);
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            bcs.mMassPropertiesOverride.mMass = bodyMass;
            // A DISC about its own axle, not a sphere: (1/2 mR^2) about local X (= the axle),
            // (1/4 mR^2) about the two radial axes.
            const float halfMR2 = 0.5f * bodyMass * ws.radius * ws.radius;
            bcs.mMassPropertiesOverride.mInertia =
                JPH::Mat44::sScale(JPH::Vec3(halfMR2, 0.5f * halfMR2, 0.5f * halfMR2));
            // Jolt's default cap is 0.25*pi*60 ~= 47 rad/s, which silently clips wheel spin at
            // roughly 85 km/h with R=0.5 in Release with no assert.
            bcs.mMaxAngularVelocity = 300.0f;
            // Zeroed here on the reasoning that Jolt's 0.05 defaults would be "parasitic braking on
            // top of the tyre model". docs §122 found that reasoning backwards: the reference damps
            // every ODE body HARDER than that - 0.1 linear and 0.3 angular, set once for the whole
            // world in ai::DynamicScene::InitOnce and copied into each body by dBodyCreate - so a
            // wheel at zero damping is further from ODE than Jolt's default was, not closer. Left
            // at zero as the as-built state; jolt_harness/body_damping is the lever that restores
            // the reference's values, and its OFF arm is exactly this line.
            bcs.mLinearDamping  = 0.0f;
            bcs.mAngularDamping = 0.0f;
            bcs.mFriction       = 0.0f;
            bcs.mRestitution    = 0.0f;
            // NOT a sensor body. Step 2 set this flag as scaffolding; step 3 replaced it with the
            // real mechanism, which is a PER-PAIR decision in the contact callback
            // (HarvestWheelContact): the tyre sphere is made a sensor for each contact so the
            // solver draws no force from it, while the rim stays a real contact with friction and
            // restitution zeroed. A body-wide sensor flag cannot express that split - it would
            // take the rim's non-penetration guarantee away with it.
            bcs.mIsSensor = false;

            JPH::Body* wheelBody = bi.CreateBody(bcs);
            if (wheelBody == nullptr) {
                LOG_WARNING("Shadow (%s): wheel %zu body creation failed (out of bodies?)", label, i);
                return;
            }

            // A tagged handle the contact callback can read with no lookup and no lock: high
            // dword is the literal 'WHL\0', low dword packs the HARVEST SLOT and the wheel index.
            // The harvest slot, not the collision group - the callback uses this to find which
            // buffer to append to, and variant shadows deliberately share a collision group while
            // keeping separate buffers (see ShadowState::harvestSlot).
            wheelBody->SetUserData(MakeWheelUserData(state.harvestSlot, (uint32_t) i));
            // Without this Jolt merges manifolds from different SubShapeIDs whose normals are
            // near-equal, and the TYRE/RIM distinction disappears with no symptom at all - the
            // whole banded-contact scheme dies quietly.
            wheelBody->SetUseManifoldReduction(false);
            if (state.VariantFamilyActive() && i + 1 <= kMaxWheelsPerVehicleGroupFilter) {
                // docs §107: same shared group, this family member's own subgroup block.
                wheelBody->SetCollisionGroup(JPH::CollisionGroup(GetVariantGroupFilter(), kVariantGroupId,
                    (JPH::CollisionGroup::SubGroupID) (state.var.familyIndex * kVariantSubGroupStride + i + 1)));
                if (i == 0)
                    LOG_INFO("docs §107.3: collision group (%s) wheels: filter=VARIANT group=%u subGroup=%u..%u",
                        label, kVariantGroupId,
                        state.var.familyIndex * kVariantSubGroupStride + 1,
                        state.var.familyIndex * kVariantSubGroupStride + (uint32_t) nWheels);
            } else if (i + 1 <= kMaxWheelsPerVehicleGroupFilter) {
                if (i == 0)
                    LOG_INFO("docs §107.3: collision group (%s) wheels: filter=wheel group=%u subGroup=1..%zu",
                        label, collisionGroupId, nWheels);
                wheelBody->SetCollisionGroup(JPH::CollisionGroup(
                    GetWheelGroupFilter(), collisionGroupId, (JPH::CollisionGroup::SubGroupID) (i + 1)));
            } else {
                LOG_WARNING("Shadow (%s): wheel %zu exceeds kMaxWheelsPerVehicleGroupFilter=%u - "
                            "self-collision exclusion not applied for this wheel",
                    label, i, kMaxWheelsPerVehicleGroupFilter);
            }
            bi.AddBody(wheelBody->GetID(), JPH::EActivation::Activate);
            // docs §46d (task #59): match the chassis's just-assigned velocity (itself copied
            // from ODE, see BuildShadow) so the strut isn't immediately fighting a full-speed
            // relative-velocity mismatch on the very first solver step after a moving vehicle's
            // shadow is (re)built.
            wheelBody->SetLinearVelocity(chassisBody->GetLinearVelocity());
            wheelBody->SetAngularVelocity(chassisBody->GetAngularVelocity());

            // --- the strut ---
            JPH::SixDOFConstraintSettings six;
            six.mSpace = JPH::EConstraintSpace::WorldSpace;
            // docs §75: the anchor sits at the WHEEL CENTRE by default, NOT at the chassis mount.
            // Plan §2.2 says the mount; the shipped build disagreed and the binary is newer. The
            // wheel centre is what makes the translation limits and the motor target below
            // coherent as "relative to spawn" - anchoring at the mount would need absolute
            // [minLen, maxLen] limits instead. wm4_joint_at_mount is the lever back to the plan.
            const JPH::RVec3 anchor = (cfg.jolt_wm4_joint_at_mount.value != 0) ? worldAttach : wheelCentreWorld;
            six.mPosition1 = six.mPosition2 = anchor;

            // Frame: X = the axle, Y = chassis forward orthonormalised against it, so Jolt's
            // Z = X x Y comes out as the strut/kingpin axis. Gram-Schmidt rather than trusting
            // the two to be perpendicular: they are per-prototype data, and Jolt asserts on a
            // degenerate frame.
            const JPH::Vec3 axleWorld = (chassisRot * ws.axleLocal).Normalized();
            const JPH::Vec3 fwdWorld  = (chassisRot * JPH::Vec3(0.0f, 0.0f, 1.0f)).Normalized();
            JPH::Vec3 yAxis = fwdWorld - axleWorld * fwdWorld.Dot(axleWorld);
            yAxis = (yAxis.Length() > 1.0e-3f) ? yAxis.Normalized() : axleWorld.GetNormalizedPerpendicular();
            six.mAxisX1 = six.mAxisX2 = axleWorld;
            six.mAxisY1 = six.mAxisY2 = yAxis;
            // Pyramid, not Cone: Cone symmetrises and couples the two swing limits, and steering
            // needs an independent asymmetric limit from camber.
            six.mSwingType = JPH::ESwingType::Pyramid;

            // Translation: the strut slides along Z only.
            six.MakeFixedAxis(EAx::TranslationX);
            six.MakeFixedAxis(EAx::TranslationY);
            const float range = std::max(ws.maxLen - ws.minLen, 0.05f);
            const float frac  = std::clamp(cfg.jolt_wm4_compress_fraction.value, 0.05f, 0.95f);
            const float compressHeadroom = frac * range;          // wheel travels UP, toward -Z
            const float droopHeadroom    = range - compressHeadroom;
            six.SetLimitedAxis(EAx::TranslationZ, -compressHeadroom, droopHeadroom);

            // A vehicle whose static sag exceeds its compression headroom rests on the bump stop
            // from the moment it is grounded, which reads as "the suspension is rock hard" rather
            // than as a config error. Cheap to check, so check rather than assume.
            const float cornerWeight  = chassisMass * 9.81f / std::max<float>((float) nWheels, 1.0f);
            const float sagPredicted  = ws.kSusp > 0.0f ? cornerWeight / ws.kSusp : 0.0f;
            if (sagPredicted > compressHeadroom) {
                LOG_WARNING("docs §60 (%s): wheel %zu predicted static sag %.3fm EXCEEDS compression "
                            "headroom %.3fm (range %.3f, fraction %.2f) - it will rest on the bump stop once grounded",
                    label, i, (double) sagPredicted, (double) compressHeadroom, (double) range, (double) frac);
            }

            // The spring is a translation motor in Position mode targeting the spawn offset, so
            // sag emerges physically instead of being placed. Jolt deactivates a Position motor
            // whose stiffness is zero, so a prototype with no usable CFM would silently lose its
            // spring - guarded rather than trusted.
            if (ws.kSusp > 0.0f) {
                six.mMotorSettings[EAx::TranslationZ].mSpringSettings.mMode = JPH::ESpringMode::StiffnessAndDamping;
                six.mMotorSettings[EAx::TranslationZ].mSpringSettings.mStiffness = ws.kSusp;
                six.mMotorSettings[EAx::TranslationZ].mSpringSettings.mDamping   = ws.cSusp;
                // Motor force limits default to +-FLT_MAX. Bound them: a suspension that can pull
                // with unbounded force is how a solver blow-up gets laundered into "stiff spring".
                six.mMotorSettings[EAx::TranslationZ].SetForceLimit(4.0f * ws.kSusp * range);
            }

            // Rotation: RotationY (camber) is locked unconditionally. RotationX (spin, the twist)
            // opens at step 5 behind wm4_spin; RotationZ (steer, the swing) at step 6. The
            // assignment is not interchangeable: swing-twist decomposition is R = R_swing *
            // R_twist, so steering must be the swing and spin the twist, and the reverse is
            // kinematically wrong.
            six.MakeFixedAxis(EAx::RotationY);

            // docs §67: the sign that carries a game-side angular velocity into constraint space.
            // DERIVED, never a literal - every quantity crossing this boundary is multiplied by
            // it, and a hard-coded +1 would be invisible right up until a prototype whose axle
            // runs the other way drives backwards. The comparison is between the constraint's own
            // X axis and the axle the CORE uses, which is the one for which t = a x n points
            // forward (joltshadow.cpp's `up x forward` derivation, and the bug it records).
            //
            // Honest note on what this currently proves: mAxisX is built from the same
            // ws.axleLocal that the core reads, so today the answer is +1 BY CONSTRUCTION, not by
            // measurement. That is a reason to keep computing it - it turns a future change to
            // either side into a visible sign flip instead of a silent one - not a reason to
            // claim the +1 was discovered.
            const JPH::Vec3 axleCoreWorld = (chassisRot * ws.axleLocal).Normalized();
            ws.spinSign = (six.mAxisX1.Dot(axleCoreWorld) >= 0.0f) ? +1.0f : -1.0f;

            // docs §68: the sign that carries m_steerRadians into constraint space. The frame is
            // X = axle, Y = forward, so the implied Z = X x Y - and with the live-confirmed chassis
            // convention (up = +Y, forward = +Z) that is X_c x Z_c = -Y_c, i.e. the constraint's Z
            // points DOWN and is ANTI-PARALLEL to the kingpin. So the honest expectation here is
            // -1, not +1, and this is flagged in the recovered spec as the single most likely
            // silent inversion in the whole cluster: a hard-coded +1 gives a vehicle that steers
            // the wrong way at every speed and still passes any test that only checks |angle|.
            // The sibling bug is on record in this file - using Jolt's "right" = forward x up gave
            // t = -Z and drove the vehicle backwards in the first bring-up.
            const JPH::Vec3 constraintZ  = six.mAxisX1.Cross(six.mAxisY1);
            const JPH::Vec3 kingpinWorld = (chassisRot * ws.steerAxis).Normalized();
            ws.steerSign = (constraintZ.Dot(kingpinWorld) >= 0.0f) ? +1.0f : -1.0f;

            const bool spinDof = state.VarSpin() != 0;
            if (spinDof) {
                // Free twist, for EVERY wheel - not only driven ones. A non-driven wheel still has
                // to roll, and locking it would drag the vehicle on four skids.
                six.MakeFreeAxis(EAx::RotationX);
                // Torque limits start at zero and are commanded per frame. Rotational motors act
                // in the constraint space of BODY 2 (the wheel), so twist = X is the natural axis.
                six.mMotorSettings[EAx::RotationX].SetTorqueLimit(0.0f);
            } else {
                six.MakeFixedAxis(EAx::RotationX);
            }

            // Steering. MakeFixedAxis for a non-steerable wheel costs zero solver rows, so the
            // unsteered case is cheaper than the steered one rather than merely equivalent.
            const bool steerDof = state.VarSteer() != 0 && ws.steering;
            if (steerDof) {
                // docs §105: the MECHANICAL STOP and the COMMAND LIMIT are different things, and
                // step 6 conflated them. Wheel::STEERING_LIMIT (pi/4) is what the player-control
                // path passes to SetSteer - a COMMAND bound. The reference's mechanical stops are
                // ODE's Hinge2 axis-1 LoStop/HiStop, which §94.2 reports as +-pi for steered
                // wheels. Clamping the stop to pi/4 as well meant the only thing that ever reached
                // it was DISTURBANCE, never a command - and §104 measured the stop then supplying
                // 8591 Nm to hold a wheel there while commanded read 0.000.
                six.SetLimitedAxis(EAx::RotationZ, -kOdeSteerStopRadians, +kOdeSteerStopRadians);
                // docs §106: the spring frequency is a CONFIG value, not a literal, because it is
                // now the binding constraint on the steer angle (§105.4) and because it is the one
                // steering constant the recovery pass listed as never established - the plan says
                // "~20 Гц / ζ=1" with a tilde and no artefact carries it. A named lever is what
                // lets it be swept instead of guessed.
                six.mMotorSettings[EAx::RotationZ].mSpringSettings.mMode      = JPH::ESpringMode::FrequencyAndDamping;
                six.mMotorSettings[EAx::RotationZ].mSpringSettings.mFrequency = state.VarSteerHz();
                six.mMotorSettings[EAx::RotationZ].mSpringSettings.mDamping   = state.VarSteerDamp();
                six.mMotorSettings[EAx::RotationZ].SetTorqueLimit(0.0f);   // commanded per frame
            } else {
                six.MakeFixedAxis(EAx::RotationZ);
            }

            JPH::Constraint* c = six.Create(*chassisBody, *wheelBody);  // body 1 = chassis, body 2 = wheel
            physics->AddConstraint(c);
            {
                JPH::SixDOFConstraint* sc = static_cast<JPH::SixDOFConstraint*>(c);
                if (ws.kSusp > 0.0f) {
                    sc->SetMotorState(EAx::TranslationZ, JPH::EMotorState::Position);
                    sc->SetTargetPositionCS(JPH::Vec3::sZero());  // return to the spawn offset
                }
                // docs §124.12: TRIED, DID NOT FIX (kept - extra solver budget is never harmful,
                // and this project documents refuted levers rather than deleting the evidence).
                // Hypothesis was: a cold switch_vehicle.txt pin hands this strut a large initial
                // position error at the same moment the new WheelContactConstraint adds more
                // competing constraint work to the island, and the default iteration budget isn't
                // enough to converge. Live re-test (docs §124.12) with this override in place
                // reproduced the SAME hang, byte-for-byte the same symptom (wheel settles into a
                // stable, static, zero-contact position) - so the failure is not an iteration-
                // count/convergence-speed problem. Real cause traced further in §124.12: the
                // wheel body reaches a genuine zero-velocity equilibrium with the harvest system
                // reporting recs=0 on every wheel throughout, which points at contact NEVER being
                // found at all (not "found too slowly"), not at the solver failing to converge.
                if (cfg.jolt_wm4_contact_constraint.value != 0) {
                    sc->SetNumVelocityStepsOverride(20);
                    sc->SetNumPositionStepsOverride(10);
                }
                // Velocity mode only for DRIVEN wheels here; a free-roller gets no motor until
                // something (the handbrake) needs one, and the per-frame code switches it then.
                // The axis is free either way, so an undriven wheel spins on the tyre force alone.
                if (spinDof && ws.driven)
                    sc->SetMotorState(EAx::RotationX, JPH::EMotorState::Velocity);
                if (steerDof)
                    sc->SetMotorState(EAx::RotationZ, JPH::EMotorState::Position);
            }

            state.wheelBodies.push_back(wheelBody->GetID());
            state.wheelConstraints.push_back(c);

            // docs §124 step 1: the new contact constraint, gated and additive - the old
            // AddForce path (VehicleStepListener::applyTo) stays authoritative regardless of
            // this flag. Kept fully empty (not padded with null) when off, same invariant as
            // every other var-gated vector in this file: either fully parallel to wheelOrder or
            // not populated at all, never partial.
            if (cfg.jolt_wm4_contact_constraint.value != 0) {
                WheelContactConstraintSettings ccSettings;
                JPH::Constraint* cc = new WheelContactConstraint(*wheelBody, ccSettings);
                physics->AddConstraint(cc);
                state.wheelContactConstraints.push_back(cc);
            }

            state.wmDiscInertiaX.push_back(halfMR2);
            state.wmDiscInertiaR.push_back(0.5f * halfMR2);
            // Freshly built body, no accumulated spin yet - matches its actual as-built pose.
            state.wmSpinAngle.push_back(0.0f);

            LOG_INFO("docs §75: constraint frame (%s) w=%zu axle.fwd=%+.4f (%.2f deg off perpendicular) | "
                     "anchorOffsetFromWheelCentre=%.4f m (restLen)",
                label, i, (double) axleWorld.Dot(fwdWorld),
                (double) (std::asin(std::min(std::fabs(axleWorld.Dot(fwdWorld)), 1.0f)) * 57.29578f),
                (double) JPH::Vec3(wheelCentreWorld - worldAttach).Length());
        }

        // docs §124.11 debug (temporary, Ural01 hang bring-up): confirms construction/AddConstraint
        // actually ran for every wheel on THIS rebuild, not just that the flag was on at file scope.
        LOG_INFO("docs §124.11: rebuild (%s) wheels=%zu contactConstraintsBuilt=%zu flag=%d",
            label, state.wheelBodies.size(), state.wheelContactConstraints.size(),
            cfg.jolt_wm4_contact_constraint.value);

        // docs §95.4/§96: take the wheel bodies' mass back OUT of the chassis, so the shadow's
        // total matches ODE's instead of exceeding it by the whole unsprung mass. Mass and
        // inertia are scaled by the SAME factor: the chassis shape has not changed, only how much
        // matter it represents, so its tensor is linear in mass. Done as a post-pass rather than
        // at CreateBody so the compound-derived tensor keeps its SHAPE - Jolt computed that
        // correctly, it was only the magnitude that was wrong.
        float chassisMassFinal = chassisMass;
        if (cfg.jolt_wm4_chassis_mass_excl_wheels.value != 0 && totalWheelMass > 0.0f) {
            if (totalWheelMass < chassisMass * 0.9f) {
                const float scale = (chassisMass - totalWheelMass) / chassisMass;
                JPH::MotionProperties* mp = chassisBody->GetMotionProperties();
                mp->SetInverseMass(mp->GetInverseMass() / scale);
                mp->SetInverseInertia(mp->GetInverseInertiaDiagonal() / scale, mp->GetInertiaRotation());
                chassisMassFinal = chassisMass - totalWheelMass;
            } else {
                // Refuse rather than drive the chassis toward zero mass: a vehicle whose wheels
                // claim >=90% of its total is either bad data or the mass floor biting hard, and
                // silently producing a near-massless chassis is how a solver blow-up gets
                // laundered into "the fix made it unstable".
                LOG_WARNING("docs §95.4 (%s): wheel mass %.1f is >=90%% of vehicle mass %.1f - NOT "
                            "subtracting; the shadow stays heavier than ODE for this vehicle",
                    label, (double) totalWheelMass, (double) chassisMass);
            }
        }

        // docs §59: register the parallel harvest listener now that this vehicle owns bodies. On a
        // REBUILD the old listener is deliberately left registered and simply re-pointed rather
        // than removed: RemoveStepListener during a rebuild is the same teardown-ordering hazard
        // that BuildShadow's comment documents as a real, repeatable worker-thread crash, and this
        // file's standing answer to that hazard is to leak rather than to unregister.
        if (state.stepListener == nullptr) {
            state.stepListener = new VehicleStepListener();
            physics->AddStepListener(state.stepListener);
        }
        state.stepListener->chassis    = chassisBody;
        state.stepListener->slot       = state.harvestSlot;
        state.stepListener->params     = WheelModelParamsFromConfig();
        state.stepListener->wheelCount = (uint32_t) std::min<size_t>(state.wheelBodies.size(), kMaxHarvestWheels);
        for (uint32_t w = 0; w < state.stepListener->wheelCount; ++w) {
            JPH::Body* wb = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.wheelBodies[w]);
            state.stepListener->wheels[w].body      = wb;
            state.stepListener->wheels[w].radius    = state.wheelSetup[w].radius;
            state.stepListener->wheels[w].tau       = state.wheelSetup[w].tau;
            state.stepListener->wheels[w].axleLocal = state.wheelSetup[w].axleLocal;
            state.stepListener->wheels[w].driven    = state.wheelSetup[w].driven;
            state.stepListener->wheels[w].inertia   = (w < state.wmDiscInertiaX.size()) ? state.wmDiscInertiaX[w] : 1.0f;
            // docs §63 / plan §3.5: read the mass back OFF THE BODY rather than recomputing it.
            // The core's c_t = 2*zeta*sqrt(k_t*m) and capLat must use bit-identically the value
            // the body was created with; recomputing max(unsprungMass, chassisMass/40) here would
            // be the same arithmetic today and a silent divergence the first time either input
            // moves - and nothing would report it.
            state.stepListener->wheels[w].mass = (wb != nullptr)
                ? 1.0f / std::max(wb->GetMotionProperties()->GetInverseMass(), 1.0e-8f)
                : 10.0f;
            // docs §124 step 2: route this wheel's contact cache to its own constraint (nullptr
            // when the flag is off, or the vector is short for some other reason) - OnStep's own
            // cc!=nullptr guard already treats a nullptr entry as "old path only", so leaving
            // this default-nullptr is a safe, silent no-op rather than a state to special-case.
            WheelContactConstraint* cc = (w < state.wheelContactConstraints.size())
                ? static_cast<WheelContactConstraint*>(state.wheelContactConstraints[w])
                : nullptr;
            state.stepListener->contactConstraints[w] = cc;
            if (cc != nullptr) {
                cc->SetStatic(physics, chassisBody, state.harvestSlot, w,
                    state.wheelSetup[w].radius, state.wheelSetup[w].tau,
                    state.stepListener->wheels[w].mass, state.stepListener->wheels[w].inertia,
                    state.wheelSetup[w].axleLocal, state.stepListener->params);
            }
        }

        // The engagement check is inside the line: chassisMassFinal + wheelMass must equal
        // chassisMass, or the subtraction did not happen.
        LOG_INFO("docs §58 (%s): mode 4 - built %zu wheel bodies + SixDOF constraints (chassisMass=%.1f, massFloor=%.2f)"
                 " | docs §95.4 wheelMass=%.1f chassisMassFinal=%.1f total=%.1f",
            label, state.wheelBodies.size(), (double) chassisMass, (double) massFloor,
            (double) totalWheelMass, (double) chassisMassFinal, (double) (chassisMassFinal + totalWheelMass));
    }

    static bool BuildShadow(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label, uint32_t collisionGroupId) {
        const uint32_t numWheels = vehicle->GetNumWheels();
        if (numWheels == 0) {
            LOG_WARNING("Shadow build skipped (%s): vehicle has no wheels", label);
            return false;
        }

        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        JPH::BodyInterface&  bodyInterface = physics->GetBodyInterface();

        hta::CVector    pos = vehicle->GetPosition();
        hta::Quaternion rot = vehicle->GetRotation(); // literal dBodyGetQuaternion passthrough (confirmed)

        // Chassis: prefer the real multi-part collision geometry (docs §23.10 -
        // BuildChassisCompoundShape, Chassis/Cabin/Basket/etc. boxes+spheres+capsules via
        // ComplexPhysicObj::m_vehicleParts); fall back to the original single mass-box
        // approximation (same dimensions ODE's own RefreshMass/dMassSetBoxTotal uses, VA
        // 0x2BCAC0) if no usable part geoms were found for any reason - never leaves the
        // vehicle without a shape.
        const hta::ai::VehiclePrototypeInfo* protoInfo = vehicle->GetPrototypeInfo();
        hta::CVector massSize = protoInfo != nullptr ? protoInfo->m_massSize : hta::CVector(2.0f, 1.0f, 4.0f);
        JPH::Vec3 halfExtents( // still computed unconditionally: used as the fallback shape AND for the build-log dimensions below, even when the compound shape wins
            std::max(massSize.x * 0.5f, 0.1f),
            std::max(massSize.y * 0.5f, 0.1f),
            std::max(massSize.z * 0.5f, 0.1f));

        JPH::RefConst<JPH::Shape> chassisBaseShape = BuildChassisCompoundShape(vehicle, pos, rot);
        if (chassisBaseShape == nullptr)
            chassisBaseShape = new JPH::BoxShape(halfExtents);

        // PhysicObj::m_massCenter (local-space) is exactly the offset PhysicObj::GetPosition()
        // itself subtracts from the raw ODE body origin (confirmed via disassembly of
        // PhysicObj::GetPosition, VA 0x5FC410) - i.e. rawOdeOrigin = GetPosition() +
        // rot*massCenter. The GOAL is putting Jolt's world COM at exactly that same point.
        hta::CVector localCom = vehicle->GetMassCenter();
        JPH::Vec3 desiredComOffset(localCom.x, localCom.y, localCom.z);

        // docs §46h (task #59, real root cause of "Ural01 chassis rides too high" - found after
        // §46f/§46g both patched symptoms and neither closed the gap): OffsetCenterOfMassShape::
        // GetCenterOfMass() (extern/joltphysics/.../OffsetCenterOfMassShape.h) returns
        // "mInnerShape->GetCenterOfMass() + mOffset" - ADDITIVE to the wrapped shape's OWN
        // natural geometric centroid, not an absolute replacement. This code was passing
        // desiredComOffset (vehicle->GetMassCenter(), meant as the ABSOLUTE final local COM)
        // straight in as mOffset, so the actual result was
        // chassisBaseShape's-own-centroid + desiredComOffset - silently wrong by exactly
        // however far the compound shape's own natural centroid sits from local (0,0,0).
        // Confirmed live on Ural01: intended offset Y=-2.300, but the live shape's actual
        // GetCenterOfMass() read Y=-1.492 (X and Z off too, -0.000/-0.504) - and Ural's long,
        // asymmetric compound chassis (cab/cargo spread far from the vehicle's GetPosition()
        // pivot) gives it an unusually large natural centroid, which is exactly why this vehicle
        // showed the bug so much more visibly than smaller/more symmetric ones. Fix: subtract
        // the shape's own natural centroid first, so mOffset ends up being the DELTA needed to
        // land the final COM at the real target, not the target itself.
        const JPH::Vec3 shapeNaturalCom = chassisBaseShape->GetCenterOfMass();
        JPH::Vec3 comOffset = desiredComOffset - shapeNaturalCom;
        LOG_INFO("docs §46h: chassis COM offset fix (%s) shapeNaturalCom=(%.3f,%.3f,%.3f) "
            "desired=(%.3f,%.3f,%.3f) mOffset(corrected)=(%.3f,%.3f,%.3f)",
            label, (double) shapeNaturalCom.GetX(), (double) shapeNaturalCom.GetY(), (double) shapeNaturalCom.GetZ(),
            (double) desiredComOffset.GetX(), (double) desiredComOffset.GetY(), (double) desiredComOffset.GetZ(),
            (double) comOffset.GetX(), (double) comOffset.GetY(), (double) comOffset.GetZ());

        JPH::ShapeSettings::ShapeResult chassisResult =
            JPH::OffsetCenterOfMassShapeSettings(comOffset, chassisBaseShape.GetPtr()).Create();
        if (chassisResult.HasError()) {
            LOG_ERROR("Shadow chassis shape creation failed (%s): %s", label, chassisResult.GetError().c_str());
            return false;
        }

        // docs §132 (task #56, Ural01 cold-pin hang - root cause and fix): live diagnostics
        // (§130 chassis-contact log, §124.12's existing wheelBelowChassis field, and the new
        // chassisUpY field - all confirmed via a clean 90s repro) established that the vehicle
        // never actually gets stuck "in the wheel-contact pipeline" at all - the CHASSIS itself
        // (perfectly upright, chassisUpY=1.000, not a rollover) sits in stable, continuous
        // contact with a static, untagged body (almost certainly raw terrain) for the entire
        // stuck duration, while every wheel dangles just above the real ground and genuinely
        // never touches it - recs=0 is 100% correct in this scenario, not a bug. Immediately
        // above, `pos = vehicle->GetPosition()` (ODE's own state) is used completely as-is for
        // the chassis spawn pose, with NO ground-clearance check at all - unlike wheels, which
        // already get exactly this kind of correction via InitWheelModelSuspension's raycast
        // (docs §127). If the ODE-side position places the chassis's own geometry below the
        // REAL Jolt-exported terrain height at that exact spot (a stale saved position from
        // different terrain/slope, or the same kind of export-mesh discrepancy §124.13 already
        // flagged as a live possibility), the chassis embeds directly into terrain and Jolt's
        // ordinary rigid-body contact response holds it there forever - no different in kind
        // from any other body resting in the ground, just never visibly resolved because
        // nothing here ever re-checks or corrects chassis clearance after spawn.
        //
        // Fix: use the ALREADY-BUILT chassis shape's own exact world-space bounds (not an
        // approximation from massSize/2 - this shape may be the real multi-part compound, docs
        // §23.10, whose true lowest point is not simply half the bounding box height) to find
        // its lowest point at the intended pose, raycast straight down from above it on the
        // SAME safe, terrain-only query layer §127's wheel raycast already established
        // (kWheelQueryLayer - restricted to NON_MOVING, cannot hit another vehicle's kinematic
        // mirror), and nudge pos.y UP by exactly the overlap (plus a small margin) only when the
        // chassis is genuinely embedded below the detected ground. Deliberately one-directional
        // (never moves the chassis DOWN) and a no-op whenever the raycast finds no ground at all
        // (a legitimately airborne spawn, or the exact terrain-gap case this cannot and should
        // not try to paper over) - this only touches the one degenerate case that was actually
        // observed, matching this project's own established minimal-fix discipline.
        {
            const JPH::Quat chassisRotQ(rot.x, rot.y, rot.z, rot.w);
            const JPH::Mat44 chassisXform = JPH::Mat44::sRotationTranslation(
                chassisRotQ, JPH::Vec3(pos.x, pos.y, pos.z));
            const JPH::AABox worldBounds = chassisResult.Get()->GetWorldSpaceBounds(
                chassisXform, JPH::Vec3::sReplicate(1.0f));
            const float lowestWorldY = worldBounds.mMin.GetY();

            JPH::PhysicsSystem* clearancePhysics = physics; // already resolved above, same pointer
            if (clearancePhysics != nullptr) {
                const JPH::BroadPhaseLayerFilter& bpF = clearancePhysics->GetDefaultBroadPhaseLayerFilter(kWheelQueryLayer);
                const JPH::DefaultObjectLayerFilter olF = clearancePhysics->GetDefaultLayerFilter(kWheelQueryLayer);
                const float rayStartMargin = 5.0f;  // clear of the chassis's own highest point
                const float rayLen = (worldBounds.mMax.GetY() - lowestWorldY) + rayStartMargin + 20.0f;
                const JPH::RVec3 rayStart(pos.x, worldBounds.mMax.GetY() + rayStartMargin, pos.z);
                JPH::RRayCast ray{ rayStart, JPH::Vec3(0.0f, -rayLen, 0.0f) };
                JPH::RayCastResult hit;
                if (clearancePhysics->GetNarrowPhaseQuery().CastRay(ray, hit, bpF, olF)) {
                    const float groundY = (float) rayStart.GetY() - rayLen * hit.mFraction;
                    const float overlap = groundY - lowestWorldY;
                    if (overlap > 0.0f) {
                        const float correction = overlap + 0.02f; // small margin, same spirit as §127's tau-scale slack
                        LOG_WARNING("docs §132: chassis spawn clearance fix (%s) - chassis embedded "
                            "%.3fm below detected ground at (%.1f,%.1f,%.1f), nudging pos.y up by %.3fm",
                            label, (double) overlap, (double) pos.x, (double) pos.y, (double) pos.z, (double) correction);
                        pos.y += correction;
                    }
                }
            }
        }

        JPH::BodyCreationSettings bodySettings(
            chassisResult.Get(),
            JPH::RVec3(pos.x, pos.y, pos.z),
            JPH::Quat(rot.x, rot.y, rot.z, rot.w),
            JPH::EMotionType::Dynamic, kMovingLayer);
        const float chassisMass = std::max(vehicle->GetMass(), 100.0f);
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = chassisMass;

        // docs §122.17: same collision shape either way (chassisResult, above - the real compound
        // geometry) - this only swaps which MASS PROPERTIES get baked into the body. Default path
        // (CalculateInertia, above) derives the tensor from that compound shape's own geometry,
        // same as the game's actual chassis silhouette. This branch instead reproduces exactly
        // what ai::ComplexPhysicObj::RefreshMass (RVA 0x2bcac0) does for the reference - a single
        // uniform box sized from m_massSize, mass-scaled, no dMassAdd - via Jolt's own equivalent
        // helper rather than hand-built diagonal math, so it matches Jolt's own axis convention.
        const bool useOdeBoxInertia = kraken::Config::Instance().jolt_chassis_inertia_ode_box.value != 0;
        if (useOdeBoxInertia) {
            JPH::MassProperties odeBoxMass;
            odeBoxMass.SetMassAndInertiaOfSolidBox(JPH::Vec3(massSize.x, massSize.y, massSize.z), 1.0f);
            odeBoxMass.ScaleToMass(chassisMass);
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            bodySettings.mMassPropertiesOverride = odeBoxMass;
        }

        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (body == nullptr) {
            LOG_ERROR("Shadow chassis body creation failed (%s, out of bodies?)", label);
            return false;
        }

        // docs §54.4 (шаг -1F): hand whatever this ShadowState previously owned to the deferred
        // destroy queue BEFORE overwriting the handles - this is the last point at which the old
        // body/constraint are still reachable. Enqueueing (rather than destroying here) preserves
        // the file's hard rule that nothing is torn down inside the frame; the queue is drained
        // after PhysicsSystem::Update returns. No-op unless [jolt_harness] deferred_destroy=1, in
        // which case this is exactly the historical leak-forever behaviour.
        // Placed after the new body is known to exist so a failed rebuild doesn't destroy a
        // still-working shadow.
        if (!state.bodyId.IsInvalid() || state.constraint != nullptr) {
            PendingJoltDestroy old;
            if (!state.bodyId.IsInvalid())
                old.bodies.push_back(state.bodyId);
            if (state.constraint != nullptr) {
                old.constraints.push_back(state.constraint);
                // wheelmodel mode never calls AddConstraint/AddStepListener (see below), so the
                // old constraint is only "registered" when the previous build was the
                // VehicleConstraint path.
                old.constraintsRegistered = !state.wheelModelMode;
            }
            EnqueueJoltDestroy(std::move(old));
        }

        state.bodyId = body->GetID();
        // docs §23.11: lets VehiclePushbackContactListener map a contact straight back to the
        // hta::ai::Vehicle* that owns this DYNAMIC body, without a per-frame lookup - same
        // technique used for the kinematic mirrors below (MirrorOtherVehicles).
        body->SetUserData(reinterpret_cast<uint64_t>(vehicle));
        // docs §37 item 3: chassis is always subgroup 0 within this vehicle's own GroupID - see
        // GetWheelGroupFilter's comment above.
        // docs §107: with variants active every family member - the player shadow included - moves
        // onto the all-pairs-disabled table under one shared GroupID, each with its own subgroup
        // block. The player shadow HAS to join: otherwise the variants would collide with it, and
        // the baseline arm would be the one being disturbed.
        if (state.VariantFamilyActive()) {
            body->SetCollisionGroup(JPH::CollisionGroup(GetVariantGroupFilter(), kVariantGroupId,
                (JPH::CollisionGroup::SubGroupID) (state.var.familyIndex * kVariantSubGroupStride)));
        } else {
            body->SetCollisionGroup(JPH::CollisionGroup(GetWheelGroupFilter(), collisionGroupId, 0));
        }
        // docs §107.3: which FILTER TABLE each body ended up on, not just which group. The
        // hypothesis under test is that two different tables end up sharing one GroupID - Jolt
        // consults the first body's filter, so a variant's subgroup would index past the smaller
        // table. That is invisible in any log that prints only group and subgroup, which is why
        // the pointer is here.
        LOG_INFO("docs §107.3: collision group (%s) chassis: filter=%s group=%u subGroup=%u familyIdx=%u",
            label, state.VariantFamilyActive() ? "VARIANT" : "wheel",
            state.VariantFamilyActive() ? kVariantGroupId : collisionGroupId,
            state.VariantFamilyActive() ? state.var.familyIndex * kVariantSubGroupStride : 0u,
            state.var.familyIndex);

        // docs §27: read once here (constant for the whole body, not per-wheel) - used by the
        // per-wheel suspension-frequency derivation below. Valid immediately after CreateBody:
        // EOverrideMassProperties::CalculateInertia computes these as part of shape/mass
        // processing, no AddBody/simulation step required first.
        const JPH::MotionProperties* motionProps = body->GetMotionProperties();
        const float          chassisInvMass    = motionProps->GetInverseMass();
        const JPH::Mat44      chassisInvInertia = motionProps->GetLocalSpaceInverseInertia();

        // docs §122.16: does the SHADOW's chassis inertia tensor even resemble the REFERENCE's?
        // Never checked before now. ai::ComplexPhysicObj::RefreshMass (RVA 0x2bcac0, 224 bytes,
        // read via lora) computes ODE's real chassis dMass with ONE call to dMassSetBoxTotal (or,
        // only when a part-count field reads 1, dMassSetSphereTotal) - there is no dMassAdd
        // anywhere in the function, so the reference NEVER sums a multi-part body. Its box uses
        // the exact same `m_massSize` this file already reads into `massSize`/`halfExtents` two
        // screens up - the SAME numbers this file's own FALLBACK box (chassisBaseShape when
        // BuildChassisCompoundShape fails) would use. But the code above does not take that
        // fallback when compound geometry exists - §23.10's Chassis/Cabin/Basket boxes+spheres+
        // capsules is the PREFERRED path, and Molokovoz01 has one (4 boxes, per the build log).
        // So the shadow's inertia comes from Jolt's CalculateInertia over that multi-part shape,
        // the reference's from a single uniform box over the bounding massSize - two different
        // computations of the same physical quantity, compared here for the first time.
        {
            const JPH::Vec3 invDiag = chassisInvInertia.GetDiagonal3();
            const float jIxx = invDiag.GetX() > 1.0e-9f ? 1.0f / invDiag.GetX() : -1.0f;
            const float jIyy = invDiag.GetY() > 1.0e-9f ? 1.0f / invDiag.GetY() : -1.0f;
            const float jIzz = invDiag.GetZ() > 1.0e-9f ? 1.0f / invDiag.GetZ() : -1.0f;
            const float m = bodySettings.mMassPropertiesOverride.mMass;
            const float oIxx = m / 12.0f * (massSize.y * massSize.y + massSize.z * massSize.z);
            const float oIyy = m / 12.0f * (massSize.x * massSize.x + massSize.z * massSize.z);
            const float oIzz = m / 12.0f * (massSize.x * massSize.x + massSize.y * massSize.y);
            LOG_INFO("docs §122.16: chassis inertia (%s) Jolt(%s) Ixx=%.0f Iyy=%.0f Izz=%.0f | "
                     "ODE(box, same massSize=%.2fx%.2fx%.2f m=%.1f) Ixx=%.0f Iyy=%.0f Izz=%.0f | "
                     "ratio pitch(Ixx) %.2fx",
                label, useOdeBoxInertia ? "ode_box" : "compound",
                (double) jIxx, (double) jIyy, (double) jIzz,
                (double) massSize.x, (double) massSize.y, (double) massSize.z, (double) m,
                (double) oIxx, (double) oIyy, (double) oIzz,
                (double) (oIxx > 1.0e-6f ? jIxx / oIxx : -1.0f));
        }

        JPH::VehicleConstraintSettings vehicleSettings;
        std::vector<hta::ai::Wheel*>& wheelOrder = state.wheelOrder; // parallel to vehicleSettings.mWheels; also reused by ApplyJoltToVehicle
        std::vector<uint32_t>& wheelSourceIndex = state.wheelSourceIndex; // parallel to wheelOrder - see its field comment
        wheelOrder.clear();
        wheelOrder.reserve(numWheels);
        state.wheelSetup.clear();               // docs §57: parallel to wheelOrder, refilled below
        state.wheelSetup.reserve(numWheels);
        wheelSourceIndex.clear();
        wheelSourceIndex.reserve(numWheels);
        state.wheelBaselineJointCount.clear();      // re-captured lazily in ApplyJoltToVehicle (docs §22.3)
        state.wheelHadExtraJointLastFrame.clear();
        state.wheelProxiesBuilt     = false; // docs §38.9: (re)built lazily once this new chassis settles
        state.collisionGroupId      = collisionGroupId;
        // docs §107: identical to the collision group for every shadow that exists today. Set
        // explicitly rather than left implicit so that the day a caller wants them to differ
        // (variant shadows sharing a group), the only change is at the caller.
        state.harvestSlot           = collisionGroupId;
        state.consecutiveSlowFrames = 0;

        // docs §31: suspension stiffness/damping are now read directly from real ODE data per
        // wheel (see the per-wheel derivation below) - superseding §29's front/rear axle weight-
        // share pre-pass that used to live here. That pre-pass only ever existed to feed §27's
        // target-rest-fraction formula, which is no longer the primary path; kGravity is kept
        // for the rare defensive fallback inside the wheel loop.
        constexpr float kGravity = 9.81f;

        // docs §54 (Этап 1, шаг -1A/-1D): verify - once per vehicle, against live ODE - the
        // chassis-local axis convention the loop below is about to hard-code, and dump the real
        // per-prototype suspension constants. See LogHinge2AxisAudit's own comment for why this
        // has to happen before the wheel-as-a-body topology is written rather than after.
        // The two assumed vectors passed here are exactly the ones the loop assigns to
        // ws->mSuspensionDirection and (implicitly, as mWheelUp x mWheelForward) the axle.
        LogHinge2AxisAudit(vehicle, label, JPH::Vec3(0.0f, -1.0f, 0.0f), JPH::Vec3(1.0f, 0.0f, 0.0f));

        for (uint32_t i = 0; i < numWheels; ++i) {
            const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[i];
            hta::ai::Wheel* wheel = info.m_wheel;
            if (!info.m_bWheelPresent || wheel == nullptr)
                continue;

            JPH::WheelSettingsWV* ws = new JPH::WheelSettingsWV();
            ws->mPosition = JPH::Vec3(info.m_initialPos.x, info.m_initialPos.y, info.m_initialPos.z);
            ws->mRadius   = wheel->GetRadius();
            ws->mWidth    = wheel->GetWidth();

            // Assumes chassis-local Y-up/Z-forward axes, the overwhelmingly standard vehicle-
            // authoring convention - NOT independently confirmed by disassembly for this
            // engine (would need dJointGetHinge2Axis1/Axis2 read back in body-local space).
            // If the shadow chassis immediately tips over or the suspension visibly pushes
            // sideways in the smoke test, this assumption is wrong and needs real evidence
            // rather than this convention-based guess.
            ws->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
            ws->mSteeringAxis        = JPH::Vec3(0.0f, 1.0f, 0.0f);
            ws->mWheelUp             = JPH::Vec3(0.0f, 1.0f, 0.0f);
            ws->mWheelForward        = JPH::Vec3(0.0f, 0.0f, 1.0f);

            // WheelPrototypeInfo::m_suspensionRange (confirmed field, offset 0x8c) is real
            // per-wheel suspension-travel data (vehicleparts.xml).
            const hta::ai::WheelPrototypeInfo* wheelProto = wheel->GetPrototypeInfo();
            const float suspensionRange = wheelProto != nullptr ? std::max(wheelProto->m_suspensionRange, 0.05f) : 0.2f;
            // The 0.05 is INVENTED and it is worth saying so at the assignment rather than in a
            // doc nobody opens. `m_suspensionRange` is the one real number here; the split of it
            // into a min and a max exists only because Jolt's VehicleConstraint demands a pair.
            // The reference has nothing to derive that split from: docs §94.2 disassembled all
            // six dJointSetHinge2Param calls in ai::Wheel::AttachToPhysicObj and found
            // dParamLoStop/dParamHiStop set on AXIS 1 - the steering angle (0/0 when the wheel
            // does not steer, +-pi when it does) - and nothing at all bounding suspension travel.
            // ODE's suspension is a pure CFM/ERP soft constraint with no stop whatsoever. So this
            // constant is a convention to be settled by measurement, never an argument to be won
            // from the reference, and any code that treats it as real data is mistaken.
            ws->mSuspensionMinLength = 0.05f;
            // docs §46g/§46i (task #59): measured live against a genuinely independent jolt=0
            // baseline (not the circular apply=1 writeback echo - see §46i's own comment) on
            // Ural01/save 00000014:
            //   scale=1.00 (ceiling 1.05m) -> real gap 0.98m
            //   scale=0.60 (ceiling 0.65m) -> real gap 0.65m
            //   scale=0.15 (ceiling 0.20m) -> real gap 0.20m
            //   scale=0.05 (ceiling 0.10m) -> real gap 0.11m
            // Roughly linear, no hard floor this time (unlike the earlier probe against the
            // contaminated echo value, which falsely plateaued ~0.7m) - closing the gap further
            // needs suspension travel small enough to feel like a rigid axle, not a suspension,
            // so the right number is a judgment call rather than a fact - [jolt_harness]
            // wm4_susp_max_scale (default 0.15, see config.hpp's own comment), not hardcoded, so
            // it can be tuned without a rebuild.
            const float suspMaxLengthScale = std::clamp(kraken::Config::Instance().jolt_wm4_susp_max_scale.value, 0.02f, 1.0f);
            ws->mSuspensionMaxLength = 0.05f + suspMaxLengthScale * suspensionRange;
            // docs §46f/§46g/§46h (task #59, "Ural01 chassis rides too high"): three fixes were
            // tried and abandoned at this exact line before the real cause was found elsewhere -
            // a wheel-travel-fraction heuristic nudging the chassis (§46f), then a direct
            // height-anchor position corrector (§46g), then shrinking this very ceiling (also
            // under §46g) - none fully closed the gap because none of them were the actual bug.
            // The real cause: BuildShadow's chassis COM offset (search "docs §46h" there) was
            // being computed as an absolute target instead of a delta from the compound shape's
            // own natural centroid, a real misuse of Jolt's OffsetCenterOfMassShapeSettings API
            // (its GetCenterOfMass() is documented as ADDITIVE to the inner shape's own centroid,
            // not a replacement). Fixed there; this line is back to its original, unscaled form -
            // confirmed live afterward: Shadow divergence pos=0.000m, Jolt COM landed exactly on
            // ODE's real COM.

            // docs §31: suspension stiffness/damping now read directly from REAL ODE data
            // (WheelPrototypeInfo::m_suspensionCFM/m_suspensionERP, vehicleparts.xml),
            // superseding §27's target-rest-fraction formula and §29's weight-share pre-pass
            // entirely - those existed only because this session hadn't yet worked out how to
            // turn ODE's CFM/ERP into a Jolt spring. History: (§23.5) discovered susp_frequency
            // alone isn't scale-invariant across suspensionRange; (§27) discovered Jolt's real
            // effective_mass depends on the whole body's inertia tensor, not mass/numWheels;
            // (§29) discovered weight isn't split evenly across wheels either, needing a real
            // front/rear axle static-equilibrium split. All three fixes were successive, ever-
            // more-careful approximations of "what stiffness makes this wheel behave like real
            // ODE" - approximations that turned out unnecessary once §30 found ODE's own
            // authored CFM/ERP data and confirmed (by reading it straight off a live Hinge2
            // joint, bit-for-bit match) that WheelPrototypeInfo's copy is exactly what's live.
            //
            // ODE's Hinge2 suspension DOF is a softened 1D linear constraint; ODE's standard
            // soft-constraint relationship (see ODE's own manual for the CFM/ERP force law)
            // gives real physical stiffness/damping directly:
            //   kp = ERP / (h * CFM)   -- N/m, the spring's real stiffness
            //   kd = (1 - ERP) / CFM   -- N*s/m, the spring's real damping
            // where h is ODE's integration timestep. h is NOT fixed in this game - disassembly
            // of ai::DynamicScene::StepScene confirms it passes the real frame elapsedTime
            // straight into dWorldQuickStep - so ODE's own suspension stiffness silently varies
            // with framerate every frame. Jolt's spring has no such dependency once set, so an
            // exact match is only possible at one reference framerate;
            // [jolt_harness] susp_reference_hz picks that point (default 60).
            //
            // Jolt's ESpringMode::StiffnessAndDamping takes raw N/m stiffness and N*s/m damping
            // directly (F = -k*x - c*v, the exact same force law ODE's kp/kd already are) - no
            // effective_mass conversion needed here at all, unlike the old FrequencyAndDamping
            // approach: Jolt's solver folds the body's real mass/inertia into the constraint
            // solve internally regardless of which spring mode is used.
            const float cfm = wheelProto != nullptr ? wheelProto->m_suspensionCFM : 0.0f;
            const float erp = wheelProto != nullptr ? wheelProto->m_suspensionERP : 0.0f;

            float stiffness, damping;
            if (cfm > 1.0e-8f) {
                const float referenceH = 1.0f / std::max(kraken::Config::Instance().jolt_susp_reference_hz.value, 1.0f);
                stiffness = erp / (referenceH * cfm);
                damping   = (1.0f - erp) / cfm;
            } else {
                // Defensive fallback for a wheel prototype with no usable CFM - not observed in
                // this game's actual data (every sampled prototype in vehicleparts.xml has one),
                // but stay safe rather than dividing by zero. Reuses §27's effective_mass-based
                // derivation with a plain equal weight split (no axle clustering - simplest thing
                // that's still physically sane for a case that shouldn't occur in practice).
                const JPH::Vec3 forcePoint = ws->mPosition
                    + 0.5f * (ws->mSuspensionMinLength + ws->mSuspensionMaxLength) * ws->mSuspensionDirection;
                const JPH::Vec3 up(0.0f, 1.0f, 0.0f);
                const JPH::Vec3 forcePointXNegUp = forcePoint.Cross(-up);
                const float rotationalTerm = forcePointXNegUp.Dot(chassisInvInertia.Multiply3x3(forcePointXNegUp));
                const float effectiveMass = 1.0f / std::max(chassisInvMass + rotationalTerm, 1.0e-8f);

                const float weightShare = (bodySettings.mMassPropertiesOverride.mMass * kGravity) / (float) numWheels;
                const float restFraction = std::clamp(kraken::Config::Instance().jolt_susp_rest_fraction.value, 0.02f, 0.4f);
                const float xRestTarget = restFraction * suspensionRange;
                stiffness = weightShare / std::max(xRestTarget, 1.0e-4f);
                damping   = 2.0f * std::sqrt(stiffness * effectiveMass); // critically-damped baseline
            }

            ws->mSuspensionSpring.mMode      = JPH::ESpringMode::StiffnessAndDamping;
            ws->mSuspensionSpring.mStiffness = stiffness * g_activeTuning.suspensionFrequency; // residual fine-tune multiplier, see TuningParams
            ws->mSuspensionSpring.mDamping   = damping * g_activeTuning.suspensionDamping;     // residual fine-tune multiplier, see TuningParams
            // WheelSettingsWV's constructor already seeds mLongitudinalFriction/mLateralFriction
            // with a plausible-shaped 3-point slip curve (see WheeledVehicleController.cpp) -
            // scale its Y values uniformly rather than replacing the curve shape outright, so
            // the tuning knob is a single "more/less grip" scalar per axis.
            for (JPH::LinearCurve::Point& point : ws->mLongitudinalFriction.mPoints)
                point.mY *= g_activeTuning.frictionLongScale;
            for (JPH::LinearCurve::Point& point : ws->mLateralFriction.mPoints)
                point.mY *= g_activeTuning.frictionLatScale;

            ws->mMaxSteerAngle = (wheel->m_steering != hta::ai::Wheel::STEERING_NO) ? kApproxMaxSteerAngleRadians : 0.0f;
            if (!wheel->m_driven)
                ws->mMaxBrakeTorque = 0.0f; // approximate: brakes on driven wheels only

            // docs §57 (Этап 1, шаг 1): snapshot the same values into the POD array, from the SAME
            // locals the WheelSettings above were just assigned from - not re-derived, so the two
            // cannot drift. Kept adjacent to those assignments on purpose: when step 2 starts
            // building real bodies off this array and plan §5 item 19 eventually deletes
            // WheelSettingsWV, the diff is a deletion rather than a re-derivation, and anything
            // that was only ever true of the WheelSettings path fails loudly instead of silently.
            //
            // Note `ws->mSuspensionSpring.mStiffness` rather than the local `stiffness`: the
            // tuning multipliers are part of what the constraint actually runs with, so the POD
            // has to carry the post-multiplier value or step 2's bodies would be built against a
            // different spring than the one being replaced.
            WheelSetup setup;
            setup.attachPos     = ws->mPosition;
            setup.suspDir       = ws->mSuspensionDirection;
            setup.steerAxis     = ws->mSteeringAxis;
            setup.axleLocal     = ws->mWheelUp.Cross(ws->mWheelForward);
            setup.radius        = ws->mRadius;
            setup.width         = ws->mWidth;
            setup.minLen        = ws->mSuspensionMinLength;
            setup.maxLen        = ws->mSuspensionMaxLength;
            setup.kSusp         = ws->mSuspensionSpring.mStiffness;
            setup.cSusp         = ws->mSuspensionSpring.mDamping;
            setup.unsprungMass  = wheel->GetMass();
            setup.driven        = wheel->m_driven;
            setup.steering      = wheel->m_steering != hta::ai::Wheel::STEERING_NO;
            // docs §133: see the WheelSetup field's own comment - read-only, safe (an existing
            // native const method, already declared/used the same NATIVE() way as every other
            // hta accessor in this file), called once per wheel per rebuild, not per frame.
            {
                const hta::ai::SphericBody* sb = static_cast<const hta::ai::Wheel*>(wheel)->_SphericBody();
                if (sb != nullptr) {
                    const hta::Quaternion rel = sb->GetNodeRelativeRotation();
                    setup.nodeRelativeRotation = JPH::Quat(rel.x, rel.y, rel.z, rel.w).Normalized();
                }
            }
            state.wheelSetup.push_back(setup);

            vehicleSettings.mWheels.push_back(ws);
            wheelOrder.push_back(wheel);
            wheelSourceIndex.push_back(i);
        }

        if (vehicleSettings.mWheels.empty()) {
            LOG_WARNING("Shadow build skipped (%s): no present wheels found on vehicle", label);
            bodyInterface.RemoveBody(state.bodyId);
            bodyInterface.DestroyBody(state.bodyId);
            state.bodyId = JPH::BodyID();
            return false;
        }

        JPH::WheeledVehicleControllerSettings* controllerSettings = new JPH::WheeledVehicleControllerSettings();
        controllerSettings->mEngine.mMaxTorque = std::max(vehicle->GetMaxTorque(), 100.0f);

        // Pair wheels into axles by consecutive index (0,1), (2,3), ... - the standard
        // left/right authoring order for vehicle prototypes (not independently confirmed
        // per-vehicle, but consistent with every wheel-ordering convention seen in this
        // engine so far). Axles with at least one driven wheel get a differential, engine
        // torque split evenly across however many driven axles exist.
        int drivenAxles = 0;
        for (size_t i = 0; i + 1 < wheelOrder.size(); i += 2) {
            if (wheelOrder[i]->m_driven || wheelOrder[i + 1]->m_driven)
                ++drivenAxles;
        }
        if (drivenAxles > 0) {
            const float torqueRatio = 1.0f / (float) drivenAxles;
            for (size_t i = 0; i + 1 < wheelOrder.size(); i += 2) {
                if (!wheelOrder[i]->m_driven && !wheelOrder[i + 1]->m_driven)
                    continue;
                JPH::VehicleDifferentialSettings diff;
                diff.mLeftWheel  = (int) i;
                diff.mRightWheel = (int) i + 1;
                diff.mEngineTorqueRatio = torqueRatio;
                controllerSettings->mDifferentials.push_back(diff);
            }
        } else {
            LOG_WARNING("Shadow (%s): no driven wheels detected, shadow vehicle will coast only", label);
        }

        vehicleSettings.mController = controllerSettings;

        state.constraint = new JPH::VehicleConstraint(*body, vehicleSettings);
        if (g_collisionTester == nullptr) {
            // docs §38.10: tried as a cleaner alternative to the wheel-proxy workaround for
            // wheels missing ground on steep terrain (§32) - a real cylinder shape-cast (the
            // wheel's own width/radius) instead of an infinitely-thin ray naturally catches a
            // much wider range of slope/edge cases a bare ray can miss by even a hair, with no
            // extra body/constraint of its own. Gated behind [jolt_harness] collision_cylinder
            // (default on) so it's a one-line revert to the old ray tester if it doesn't pan out.
            g_collisionTester = kraken::Config::Instance().jolt_collision_cylinder.value != 0
                ? static_cast<JPH::VehicleCollisionTester*>(new JPH::VehicleCollisionTesterCastCylinder(kWheelQueryLayer))
                : static_cast<JPH::VehicleCollisionTester*>(new JPH::VehicleCollisionTesterRay(kWheelQueryLayer));
        }
        state.constraint->SetVehicleCollisionTester(g_collisionTester);

        // docs §23.8: reuse fix::kineticfriction's slip-based tire model (already the ONLY
        // friction model every ODE-driven vehicle's wheels use, via CollideWheelAndAsphalt/
        // CollideWheelAndLandscape) instead of Jolt's own simplified linear slip curve, so a
        // Jolt-shadowed vehicle's tire feel isn't a quietly different, unrelated model from
        // every other vehicle in the game. docs §23.12 closed the per-soil-type friction gap
        // this comment used to list here; skid-trace visual effects remain open (§23.9/§23.12).
        // Captures wheelMu/frictionScale BY VALUE (not a pointer into wheelOrder/g_activeTuning
        // - this lambda outlives the BuildShadow call that created it) and the owning Vehicle*
        // for oil-surface handling (Vehicle::m_onOilMode). Safe against the vehicle pointer
        // going stale: this callback is replaced wholesale, along with the rest of
        // state.constraint, on every rebuild (vehicle swap/tuning change/wheel loss) - the
        // old callback is abandoned together with the old constraint, never called again,
        // same leak-forever-on-rebuild pattern already used for the rest of ShadowState.
        {
            std::vector<float> wheelMu;
            wheelMu.reserve(wheelOrder.size());
            for (hta::ai::Wheel* w : wheelOrder) {
                const hta::ai::WheelPrototypeInfo* proto = w->GetPrototypeInfo();
                wheelMu.push_back(proto != nullptr ? proto->m_mU : 1.0f);
            }
            const float frictionLongScale = g_activeTuning.frictionLongScale;
            const float frictionLatScale  = g_activeTuning.frictionLatScale;

            // docs §23.12: same tile-size formula as CollideWheelAndLandscape (kineticfriction.cpp)
            // - computed once here (level geometry is constant for the run), not per tire-callback
            // invocation, since this runs on a hot path (every wheel, every physics sub-step).
            const float tileSize = (float) hta::ai::CServer::Instance()->GetLevelSize()
                / (float) hta::ai::CServer::Instance()->GetWorld()->GetLandscape().GetTileSize();

            JPH::WheeledVehicleController* controller =
                static_cast<JPH::WheeledVehicleController*>(state.constraint->GetController());
            controller->SetTireMaxImpulseCallback(
                [wheelMu, vehicle, frictionLongScale, frictionLatScale, tileSize, constraint = state.constraint](JPH::uint inWheelIndex,
                        float& outLongitudinalImpulse, float& outLateralImpulse, float inSuspensionImpulse,
                        float /*inLongitudinalFriction*/, float /*inLateralFriction*/,
                        float inLongitudinalSlip, float /*inLateralSlip*/, float /*inDeltaTime*/) {
                    static const kraken::fix::kineticfriction::TireParams kTireParams;
                    float muLong = kraken::fix::kineticfriction::mu_from_kappa(inLongitudinalSlip, kTireParams);
                    float muLat  = muLong * kTireParams.lateral_factor;

                    // docs §23.12/§24 Milestone 1: per-soil-type friction multiplier - ODE's own
                    // `friction` param to CollideWheelSurface. CollideWheelAndAsphalt (paved
                    // roads) always passes friction=1.0; CollideWheelAndLandscape looks up
                    // whatever soil tile sits under the contact. Told apart here by comparing
                    // the wheel's JPH::Wheel::GetContactBodyID() against the roads static body
                    // (fix::jolt::GetRoadsBodyRawId(), the same body ExportRoadsToJolt creates) -
                    // both static bodies already exist as SEPARATE Jolt bodies (docs §22.9), so
                    // this is just asking Jolt which one the raycast actually hit, no new
                    // information needed. §23.12's original version couldn't tell them apart and
                    // always used the terrain-tile lookup even on roads - this closes that gap.
                    //
                    // Skid-trace visuals are deliberately NOT triggered from here (docs §23.12
                    // dead-end write-up) - this callback runs once per SolveVelocityConstraint
                    // velocity-iteration (PhysicsSettings::mNumVelocitySteps, default 10), not
                    // once per physics step, so anything with a real per-call cost (WheelTraceMgr
                    // mutation) fires up to 10x too often here. That's handled once per frame
                    // instead, in ApplyJoltToVehicle's per-wheel post-step loop.
                    const JPH::Wheel* wheel = constraint->GetWheel(inWheelIndex);
                    if (wheel != nullptr && wheel->HasContact()) {
                        const JPH::BodyID roadsBodyId(kraken::fix::jolt::GetRoadsBodyRawId());
                        if (wheel->GetContactBodyID() != roadsBodyId) {
                            const JPH::RVec3 contactPos = wheel->GetContactPosition();
                            const int32_t soilX = (int32_t) (contactPos.GetX() / tileSize + 0.5f);
                            const int32_t soilZ = (int32_t) (contactPos.GetZ() / tileSize + 0.5f);
                            const hta::ai::DynamicScene::SoilProps& props =
                                hta::ai::DynamicScene::Instance()->GetSoilProps((uint32_t) soilX, (uint32_t) soilZ);
                            muLong *= props.m_friction;
                            muLat  *= props.m_friction;
                        }
                        // else: on the road mesh - friction stays at the tire model's own mu
                        // (equivalent to CollideWheelAndAsphalt's friction=1.0 multiplier).
                    }

                    const float perWheelMu = inWheelIndex < wheelMu.size() ? wheelMu[inWheelIndex] : 1.0f;
                    muLong *= perWheelMu * frictionLongScale;
                    muLat  *= perWheelMu * frictionLatScale;

                    if (vehicle != nullptr && vehicle->m_onOilMode) {
                        muLong *= kTireParams.oil_factor;
                        muLat  *= kTireParams.oil_factor;
                    }

                    outLongitudinalImpulse = muLong * inSuspensionImpulse;
                    outLateralImpulse      = muLat  * inSuspensionImpulse;
                });
        }

        // docs §39: in wheelmodel APPLY mode the chassis is driven by the ported wheelmodel_core
        // forces (StepWheelModel), NOT by Jolt's VehicleConstraint - so DON'T add the constraint
        // to the simulation (no AddConstraint/AddStepListener). The constraint object still exists
        // and is read for its static per-wheel settings (mPosition/mSuspensionDirection/mRadius/
        // mWheelForward/Up), but it never runs OnStep, so it applies no suspension/friction of its
        // own and can't fight the wheelmodel forces. This is NOT a teardown of a live constraint
        // (the file's hard rule, from a real worker-thread crash) - it's simply never activating
        // it; safe. Everything before this point (chassis body/shape/mass, wheel settings) is
        // shared with the normal path.
        // docs §58 (Этап 1, шаг 2): mode 4 is mode 2 PLUS real wheel bodies. Both flags are true
        // for it, and they only diverge at step 4 when the forces move off the chassis and onto
        // those bodies - until then the chassis force path is byte-for-byte the mode-2 one, which
        // is what keeps step 2's effect on the vehicle observable rather than tangled.
        const uint32_t wheelModelSetting = kraken::Config::Instance().jolt_wheelmodel.value;
        state.wheelModelMode = (wheelModelSetting == 2 || wheelModelSetting == 4);
        state.wheelBodyMode  = (wheelModelSetting == 4);
        if (!state.wheelModelMode) {
            physics->AddConstraint(state.constraint);
            physics->AddStepListener(state.constraint);
        } else {
            const size_t nw = state.wheelOrder.size();
            state.wmSuspLen.assign(nw, 0.0f);
            state.wmSuspVel.assign(nw, 0.0f);
            state.wmOmega.assign(nw, 0.0f);
            state.wmRestLen.assign(nw, 0.0f);
            state.wmBottomedFrames.assign(nw, 0);
            state.wmBottomedImpulse.assign(nw, 0.0f);
            // docs §40: start each wheel exactly ON the ground (raycast-init). Full-droop init
            // (=maxLen) put the wheels ~2.7m below the COM, BELOW the one-sided heightfield surface
            // where CollideShape can't see them - the vehicle then fell through with zero wheel
            // support. See InitWheelModelSuspension (shared with the post-teleport re-init path).
            InitWheelModelSuspension(physics, state, pos, rot, label);
            LOG_INFO("Shadow (%s): wheelmodel APPLY mode - VehicleConstraint built but NOT simulated; chassis driven by wheelmodel_core (%zu wheels)",
                label, nw);
        }

        bodyInterface.AddBody(state.bodyId, JPH::EActivation::Activate);

        // docs §46d (task #59): user-reported live and confirmed via direct A/B (pure ODE vs
        // Jolt on the same save) - on a save load with the vehicle already moving, ODE coasts
        // forward from that residual velocity but the Jolt shadow just sits there. Root cause:
        // BuildShadow/BodyCreationSettings never reads vehicle->GetLinearVelocity()/
        // GetAngularVelocity() anywhere - a freshly created JPH body defaults to zero velocity
        // regardless of what the ODE vehicle it shadows was doing the instant before. Every
        // rebuild (save load, car(N) switch, tuning change) hits this, not just save load - it's
        // just most visible right after a load, when the vehicle is most likely to have real
        // carried-over speed. Finite-checked defensively: this is the first place in the file
        // that reads GetAngularVelocity() at all, and an untested native accessor is not a
        // reason to skip validating its output before feeding it to Jolt (a NaN/inf velocity
        // would otherwise poison the whole body).
        {
            const hta::CVector odeLinVel = vehicle->GetLinearVelocity();
            const hta::CVector odeAngVel = vehicle->GetAngularVelocity();
            const JPH::Vec3 linVel(odeLinVel.x, odeLinVel.y, odeLinVel.z);
            const JPH::Vec3 angVel(odeAngVel.x, odeAngVel.y, odeAngVel.z);
            if (linVel.IsNaN() || angVel.IsNaN()) {
                LOG_WARNING("Shadow (%s): docs §46d - ODE velocity read as NaN/inf (lin=(%.2f,%.2f,%.2f) "
                    "ang=(%.2f,%.2f,%.2f)), leaving shadow at zero velocity instead",
                    label, (double) odeLinVel.x, (double) odeLinVel.y, (double) odeLinVel.z,
                    (double) odeAngVel.x, (double) odeAngVel.y, (double) odeAngVel.z);
            } else {
                bodyInterface.SetLinearVelocity(state.bodyId, linVel);
                bodyInterface.SetAngularVelocity(state.bodyId, angVel);
            }
        }

        // docs §58 (Этап 1, шаг 2): after the chassis is in the world (the SixDOF needs both
        // bodies live) and after InitWheelModelSuspension has filled wmRestLen, which is what
        // places each wheel body. Both preconditions are why this sits here and not earlier.
        if (state.wheelBodyMode) {
            JPH::Body* chassisForWheels = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.bodyId);
            if (chassisForWheels != nullptr)
                BuildWheelBodies(physics, chassisForWheels, state, label, collisionGroupId);
        }

        ++g_shadowGeneration;
        state.builtGeneration = g_tuningGeneration;
        state.chassisBaselineJointCount = -1;      // re-capture structural joint count on next apply (docs §22.3)
        state.chassisHadExtraJointLastFrame = false;
        LOG_INFO("Shadow vehicle #%u built (%s): %u wheels (%d driven axle(s)), mass=%.1f, chassis=%.2fx%.2fx%.2f, susp_mult=%.2fx/damp=%.2f, friction=%.2f/%.2f",
            g_shadowGeneration, label, (uint32_t) vehicleSettings.mWheels.size(), drivenAxles, (double) bodySettings.mMassPropertiesOverride.mMass,
            (double) (halfExtents.GetX() * 2.0f), (double) (halfExtents.GetY() * 2.0f), (double) (halfExtents.GetZ() * 2.0f),
            (double) g_activeTuning.suspensionFrequency, (double) g_activeTuning.suspensionDamping,
            (double) g_activeTuning.frictionLongScale, (double) g_activeTuning.frictionLatScale);

        return true;
    }

    // Deliberately OUTSIDE UpdateShadowInputs and free of its `constraint != nullptr` guard: mode 4
    // has no VehicleConstraint and returns from the pre-step before UpdateShadowInputs is ever
    // reached, so anything that lives in there is dead code for the mode that needs it most.
    // That is not hypothetical - it is exactly the bug this function was extracted to fix.
    // docs §112 (Этап 1, шаг 8): the plan's "переделка на пробуждение по изменению ввода".
    //
    // Unconditional ActivateBody every frame means NOTHING a shadow owns can ever sleep: at 74 AI
    // shadows that is 75 chassis and ~300 wheel bodies permanently in the active set, paying
    // broadphase, narrow-phase and solver every step whether or not the vehicle they mirror has
    // moved in the last ten minutes.
    //
    // It cannot simply be deleted, and that is the whole difficulty. §97 put it there because a
    // wheel body receives force ONLY from VehicleStepListener::OnStep, and Jolt skips step
    // listeners for sleeping bodies - asleep -> listener skipped -> no force -> asleep forever.
    // A latch, not slow recovery. So sleeping is only safe with a wake condition that fires
    // BEFORE the vehicle needs to move, evaluated from the REFERENCE (which is always simulated),
    // never from the shadow's own velocity - a sleeping shadow reads zero velocity by definition
    // and would conclude it may keep sleeping.
    //
    // Behind jolt_wm4_sleep, default 0 (the pre-§112 behaviour), until measured.
    static bool ShadowMaySleep(ShadowState& state, hta::ai::Vehicle* vehicle) {
        if (kraken::Config::Instance().jolt_wm4_sleep.value == 0 || vehicle == nullptr)
            return false;
        // Every term below is the REFERENCE's, not the shadow's.
        const float throttle = std::fabs(vehicle->m_throttle);
        const float brake    = std::fabs(vehicle->m_brake);
        const float odeSpeed = std::fabs(vehicle->m_averageWheelAVel);
        const bool  hand     = vehicle->m_bHandBrake;
        if (throttle >= 1e-3f) ++g_sleepStats.rejThrottle;
        if (brake    >= 1e-3f) ++g_sleepStats.rejBrake;
        if (odeSpeed >= 1e-2f) ++g_sleepStats.rejSpeed;
        if (hand)              ++g_sleepStats.rejHand;
        const bool  quiet    = (throttle < 1e-3f) && (brake < 1e-3f) && (odeSpeed < 1e-2f) && !hand;
        // Wake on CHANGE as well as on magnitude: a step from one held throttle to another is not
        // caught by "is it non-zero", and the whole point is to be awake before the force lands.
        const bool  changed  = (std::fabs(throttle - state.lastWakeThrottle) > 1e-3f)
                            || (std::fabs(brake    - state.lastWakeBrake)    > 1e-3f)
                            || (hand != state.lastWakeHandBrake);
        state.lastWakeThrottle  = throttle;
        state.lastWakeBrake     = brake;
        state.lastWakeHandBrake = hand;
        if (!quiet || changed) {
            if (quiet && changed) ++g_sleepStats.rejChanged;
            state.quietFrames = 0;
            return false;
        }
        // A margin of quiet frames before letting go, for the same reason §38.9 needed consecutive
        // slow ticks rather than one: a single quiet frame mid-manoeuvre is common and letting the
        // bodies go there would re-arm the latch at the worst moment.
        constexpr uint32_t kQuietFramesBeforeSleep = 120;
        if (++state.quietFrames < kQuietFramesBeforeSleep) {
            ++g_sleepStats.rejMargin;   // quiet, just not for long enough YET
            return false;
        }
        return true;
    }

    static void KeepShadowBodiesAwake(ShadowState& state, hta::ai::Vehicle* vehicle = nullptr) {
        if (ShadowMaySleep(state, vehicle)) {
            ++g_sleepStats.sleptShadows;
            return;   // let Jolt put them down; the wake condition above brings them back
        }
        ++g_sleepStats.awakeShadows;
        JPH::BodyInterface& bi = kraken::fix::jolt::GetPhysicsSystem()->GetBodyInterface();
        bi.ActivateBody(state.bodyId);
        // docs §97: the mode-4 wheel bodies need the same treatment, for the same reason and one
        // step worse. They receive force ONLY from VehicleStepListener::OnStep, and a step
        // listener is skipped for sleeping bodies - so a wheel that sleeps can never be woken by
        // the thing that is supposed to drive it. That is not slow recovery, it is a latch:
        // asleep -> listener skipped -> no force -> stays asleep, for the rest of the session.
        //
        // Measured, not assumed. A 5 s full-throttle run logged 112 §66.2 lines in the drive
        // window and every one read [ASLEEP]. The band itself was already carrying load in the
        // awake frames before the latch closed (penRaw +0.00127..+0.00237 m, 128-285 N), so the
        // "band does not engage" symptom was never about the band.
        for (JPH::BodyID wid : state.wheelBodies)
            bi.ActivateBody(wid);
    }

    static void UpdateShadowInputs(hta::ai::Vehicle* vehicle, ShadowState& state) {
        if (state.constraint == nullptr)
            return;

        // docs §38.7: a resting chassis falls asleep like any other Jolt body (correct,
        // expected behavior) - but nothing else here ever re-activates it, so once asleep it
        // silently ignores every future SetDriverInput call below forever, even under fresh
        // full-throttle input (the body's own step-listener/constraint processing is skipped
        // entirely while asleep - a Jolt-wide optimization, not vehicle-specific). Never
        // surfaced before this session's vehicle-switch testing because every prior test always
        // started driving within a couple seconds of the shadow settling; this is the first
        // time a real idle gap (queued the drive scenario ~40s after a mid-session vehicle
        // swap) was long enough for it to actually fall asleep first. ActivateBody is a cheap
        // no-op if already active, so it's simplest to just call it unconditionally here rather
        // than track sleep state ourselves.
        KeepShadowBodiesAwake(state);

        JPH::WheeledVehicleController* controller =
            static_cast<JPH::WheeledVehicleController*>(state.constraint->GetController());

        // m_realThrottle (not the raw m_throttle) is what ODE's own _KeepGearBox actually
        // turns into wheel torque this frame (docs/jolt-integration-techanalysis.md Stage 1
        // section) - its formula (m_throttle - sign(engineRpm)*m_brake*10) can exceed
        // [-1, 1], so clamp defensively; WheeledVehicleController::SetDriverInput documents
        // -1..1 for forward. Same fields drive AI vehicles too - _KeepThrottle consumes them
        // identically regardless of who (human or AI) last wrote them.
        const float forward    = std::clamp(vehicle->m_realThrottle, -1.0f, 1.0f);
        const float right      = std::clamp(vehicle->m_steerRadians / kApproxMaxSteerAngleRadians, -1.0f, 1.0f);
        const float brake      = std::clamp(vehicle->m_brake, 0.0f, 1.0f);
        const float handBrake  = vehicle->m_bHandBrake ? 1.0f : 0.0f;

        controller->SetDriverInput(forward, right, brake, handBrake);
    }

    // Instantaneous Jolt-vs-ODE divergence for one vehicle, this frame. Factored out of
    // LogDivergence (below) so the autotuner's per-frame RMSE accumulator (further below) can
    // reuse the exact same numbers the human-readable log already reports, instead of a second,
    // possibly-drifting implementation of the same comparison.
    struct DivergenceSample {
        float     posDrift      = 0.0f; // meters
        float     velDrift      = 0.0f; // m/s
        float     angleDriftDeg = 0.0f; // degrees
        JPH::Vec3 joltCom       = JPH::Vec3::sZero();
        JPH::Vec3 odeCom        = JPH::Vec3::sZero();
    };

    static DivergenceSample ComputeDivergence(hta::ai::Vehicle* vehicle, ShadowState& state) {
        JPH::PhysicsSystem*  physics       = kraken::fix::jolt::GetPhysicsSystem();
        JPH::BodyInterface&  bodyInterface = physics->GetBodyInterface();

        JPH::RVec3 joltCom = bodyInterface.GetCenterOfMassPosition(state.bodyId);
        JPH::Quat  joltRot = bodyInterface.GetRotation(state.bodyId);
        JPH::Vec3  joltVel = bodyInterface.GetLinearVelocity(state.bodyId);

        hta::CVector    odeCom = vehicle->GetMassCenterPosition();
        hta::Quaternion odeRot = vehicle->GetRotation();
        hta::CVector    odeVel = vehicle->GetLinearVelocity();
        // docs §46c (task #59): temporary - BuildShadow spawns/places the Jolt chassis at
        // vehicle->GetPosition(), but this divergence log compares against
        // vehicle->GetMassCenterPosition() instead. Checking live whether those two ODE-side
        // readings actually agree (they should, if "position" means the body's own COM by ODE's
        // usual convention) or whether they're two genuinely different reference points on this
        // vehicle - which would mean the ~1m gap the user is seeing is a mismatched-reference
        // comparison, not a real Jolt/ODE simulation difference.
        {
            const hta::CVector odePivot = vehicle->GetPosition();
            LOG_INFO("docs §46c: ode GetPosition=(%.2f,%.2f,%.2f) vs GetMassCenterPosition=(%.2f,%.2f,%.2f) "
                "diff.y=%.3f",
                (double) odePivot.x, (double) odePivot.y, (double) odePivot.z,
                (double) odeCom.x, (double) odeCom.y, (double) odeCom.z,
                (double) (odeCom.y - odePivot.y));
        }

        DivergenceSample result;
        result.joltCom = JPH::Vec3(joltCom.GetX(), joltCom.GetY(), joltCom.GetZ());
        result.odeCom  = JPH::Vec3(odeCom.x, odeCom.y, odeCom.z);
        JPH::Vec3 odeVelF(odeVel.x, odeVel.y, odeVel.z);

        result.posDrift = (result.joltCom - result.odeCom).Length();
        result.velDrift = (joltVel - odeVelF).Length();

        JPH::Quat odeRotJ(odeRot.x, odeRot.y, odeRot.z, odeRot.w);
        float dot = std::fabs(joltRot.Dot(odeRotJ));
        dot = std::min(dot, 1.0f);
        result.angleDriftDeg = 2.0f * std::acos(dot) * (180.0f / 3.14159265f);
        return result;
    }

    static void LogDivergence(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        if (state.constraint == nullptr)
            return;

        DivergenceSample d = ComputeDivergence(vehicle, state);
        LOG_INFO("Shadow divergence (%s): pos=%.3fm vel=%.3fm/s angle=%.1fdeg (jolt com=[%.1f %.1f %.1f] ode com=[%.1f %.1f %.1f])",
            label, (double) d.posDrift, (double) d.velDrift, (double) d.angleDriftDeg,
            (double) d.joltCom.GetX(), (double) d.joltCom.GetY(), (double) d.joltCom.GetZ(),
            (double) d.odeCom.GetX(), (double) d.odeCom.GetY(), (double) d.odeCom.GetZ());
    }

    // docs §23.5: direct ground-truth for the suspension/traction bug report - reads Jolt's
    // OWN per-wheel state (JPH::Wheel::HasContact/GetSuspensionLength/GetAngularVelocity)
    // instead of the stale ODE-side m_numWheelsTouchingGround counter (which testharness's
    // telemetry already showed doesn't reflect Jolt's wheel state at all, since the ODE
    // wheels are DisablePhysics()'d every frame under apply=1 and never re-collide). Same
    // log cadence as LogDivergence.
    // docs §32: answers "why doesn't this wheel touch the ground" with a real measurement
    // instead of a guess. Replicates Jolt's own raycast exactly (confirmed from
    // VehicleCollisionTesterRay::Collide's source: origin = chassis_transform * mPosition,
    // direction = chassis_transform.Multiply3x3(mSuspensionDirection), length =
    // mSuspensionMaxLength + mRadius) but re-casts the SAME ray much further, to distinguish
    // "the ground is right there, just past the wheel's travel range" from "this ray doesn't
    // hit the ground at all, at any distance" (e.g. pointed away from the slope entirely).
    static void LogNoContactRaycastDiagnostic(JPH::PhysicsSystem* physics, const JPH::BodyID& chassisBodyId,
            const JPH::Wheel* wheel, const char* label, size_t wheelIndex) {
        const JPH::WheelSettings* settings = wheel->GetSettings();
        if (settings == nullptr || physics == nullptr)
            return;

        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();
        const JPH::RVec3  bodyPos = bodyInterface.GetPosition(chassisBodyId);
        const JPH::Quat   bodyRot = bodyInterface.GetRotation(chassisBodyId);
        const JPH::RMat44 bodyTransform = JPH::RMat44::sRotationTranslation(bodyRot, bodyPos);

        const JPH::RVec3 wsOrigin    = bodyTransform * settings->mPosition;
        const JPH::Vec3  wsDirection = bodyTransform.Multiply3x3(settings->mSuspensionDirection);
        const float      normalRayLength = settings->mSuspensionMaxLength + settings->mRadius;

        constexpr float kExtendedLength = 20.0f; // meters - generous, just to find the ground at all
        JPH::RRayCast extendedRay{ wsOrigin, wsDirection * kExtendedLength };
        JPH::RayCastResult hit;
        const bool foundGround = physics->GetNarrowPhaseQuery().CastRay(extendedRay, hit);

        if (foundGround) {
            const float hitDistance = kExtendedLength * hit.mFraction;
            // docs §38.8: this cast has no layer/broadphase filter (unlike the wheel's own
            // NON_MOVING-restricted VehicleCollisionTesterRay), so "ground found" here could
            // actually be hitting something the real wheel raycast deliberately ignores -
            // another vehicle's leaked shadow chassis (kMovingLayer, never torn down on
            // rebuild - see BuildShadow's own comment), a wheel-proxy sphere, etc. Log exactly
            // what was hit so a plausible-looking distance doesn't get mistaken for real ground.
            const JPH::ObjectLayer hitLayer  = bodyInterface.GetObjectLayer(hit.mBodyID);
            const JPH::EMotionType hitMotion = bodyInterface.GetMotionType(hit.mBodyID);
            const bool             isSelf    = hit.mBodyID == chassisBodyId;
            LOG_INFO("docs §32: no-contact raycast (%s) wheel=%zu bodyPos=(%.2f,%.2f,%.2f) localMPos=(%.3f,%.3f,%.3f) "
                "origin=(%.2f,%.2f,%.2f) dir=(%.3f,%.3f,%.3f) normalRayLen=%.3f groundFoundAt=%.3f shortfallBeyondTravel=%.3f "
                "hitBody=%u hitLayer=%u hitMotion=%d hitIsSelfChassis=%d",
                label, wheelIndex, (double) bodyPos.GetX(), (double) bodyPos.GetY(), (double) bodyPos.GetZ(),
                (double) settings->mPosition.GetX(), (double) settings->mPosition.GetY(), (double) settings->mPosition.GetZ(),
                (double) wsOrigin.GetX(), (double) wsOrigin.GetY(), (double) wsOrigin.GetZ(),
                (double) wsDirection.GetX(), (double) wsDirection.GetY(), (double) wsDirection.GetZ(),
                (double) normalRayLength, (double) hitDistance, (double) (hitDistance - normalRayLength),
                hit.mBodyID.GetIndexAndSequenceNumber(), (unsigned) hitLayer, (int) hitMotion, isSelf ? 1 : 0);
        } else {
            LOG_INFO("docs §32: no-contact raycast (%s) wheel=%zu origin=(%.2f,%.2f,%.2f) dir=(%.3f,%.3f,%.3f) "
                "normalRayLen=%.3f - NO GROUND FOUND within %.1fm along this ray at all",
                label, wheelIndex, (double) wsOrigin.GetX(), (double) wsOrigin.GetY(), (double) wsOrigin.GetZ(),
                (double) wsDirection.GetX(), (double) wsDirection.GetY(), (double) wsDirection.GetZ(),
                (double) normalRayLength, (double) kExtendedLength);
        }
    }

    // docs §39: log-only evaluation of the ported wheelmodel_core on the LIVE Jolt world. For
    // each wheel it generates the SAME kind of contact manifold spring_wheel's OnWheelContacts
    // gets from ODE - here via a Jolt CollideShape of the wheel's sphere (HTA wheels are real
    // ODE sphere geoms, per wheelmodel_core's own note) against the static world - then runs
    // wheelmodel_core Classify + GeneralizedContactForce and LOGS the force it WOULD apply.
    // Applies nothing (read-only): this validates manifold generation + force sanity in the Jolt
    // context before the apply path (docs §39.2) drives the chassis with it. Uses WMParams
    // defaults for now; config wiring comes with the apply path.
    static void LogWheelModelEval(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        namespace wm = kraken::fix::wheelmodel;
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr || state.constraint == nullptr || vehicle == nullptr)
            return;

        JPH::BodyInterface& bi = physics->GetBodyInterface();
        const JPH::Quat  chassisRot = bi.GetRotation(state.bodyId);
        const JPH::RVec3 comW       = bi.GetCenterOfMassPosition(state.bodyId);
        const JPH::Vec3  vLin       = bi.GetLinearVelocity(state.bodyId);
        const JPH::Vec3  vAng       = bi.GetAngularVelocity(state.bodyId);

        const wm::vec3 U{0.0f, 1.0f, 0.0f}; // world up (+Y), same convention as spring_wheel
        const wm::WMParams P;               // defaults for now
        const float dt = 1.0f / std::max(kraken::Config::Instance().jolt_susp_reference_hz.value, 1.0f);
        const uint32_t numWheels = std::max<uint32_t>(vehicle->GetNumWheels(), 1);
        const float m = vehicle->GetMass() / (float) numWheels;

        const JPH::BroadPhaseLayerFilter& bpFilter  = physics->GetDefaultBroadPhaseLayerFilter(kWheelQueryLayer);
        const JPH::DefaultObjectLayerFilter objFilter = physics->GetDefaultLayerFilter(kWheelQueryLayer);

        const JPH::Wheels& wheels = state.constraint->GetWheels();
        for (size_t i = 0; i < wheels.size(); ++i) {
            const JPH::Wheel* wheel = wheels[i];
            const JPH::WheelSettings* settings = wheel ? wheel->GetSettings() : nullptr;
            if (settings == nullptr)
                continue;

            const float R   = settings->mRadius;
            const float tau = std::min(0.15f, R * 0.9f); // tyre thickness placeholder (config later)

            // Wheel world centre: use the constraint's current wheel transform (includes live
            // suspension compression) - convenient and accurate for this read-only pass. The
            // apply path will instead own the wheel position (no VehicleConstraint there).
            const JPH::RMat44 wheelXform = state.constraint->GetWheelWorldTransform((JPH::uint) i, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
            const JPH::RVec3  wheelC     = wheelXform.GetTranslation();
            const wm::vec3    c{ (float) wheelC.GetX(), (float) wheelC.GetY(), (float) wheelC.GetZ() };

            // Axle in world = chassisRot * (forward x up), per GetWheelLocalBasis.
            const JPH::Vec3 localAxle = settings->mWheelForward.Cross(settings->mWheelUp).Normalized();
            const JPH::Vec3 wAxle     = (chassisRot * localAxle).Normalized();
            const wm::vec3  a{ wAxle.GetX(), wAxle.GetY(), wAxle.GetZ() };

            // Spin about the axle from the constraint's own wheel state (log-only).
            const float omega = wheel->GetAngularVelocity();

            // --- Jolt manifold: sphere of the wheel vs the static world (matches ODE geom) ---
            JPH::SphereShape sphere(R);
            sphere.SetEmbedded();
            JPH::CollideShapeSettings csSettings;
            csSettings.mMaxSeparationDistance = tau; // collect near-contacts too, like a soft tyre band
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            ++g_legacyPaths.evalCollideShape;   // docs §110
            physics->GetNarrowPhaseQuery().CollideShape(
                &sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(wheelC),
                csSettings, JPH::RVec3::sZero(), collector, bpFilter, objFilter);

            const int nHits = (int) collector.mHits.size();
            if (nHits == 0) {
                if ((state.frameCounter % 120) == 0)
                    LOG_INFO("docs §39.1: wheelmodel eval (%s) wheel=%zu - no manifold contacts", label, i);
                continue;
            }

            constexpr int kMaxC = 16;
            wm::WMContact cts[kMaxC];
            wm::WMGeom    gm[kMaxC];
            const int n = std::min(nHits, kMaxC);
            for (int h = 0; h < n; ++h) {
                const JPH::CollideShapeResult& r = collector.mHits[(size_t) h];
                const JPH::Vec3 nrm = (-r.mPenetrationAxis).Normalized(); // out of the surface toward the wheel
                cts[h].p = { (float) r.mContactPointOn2.GetX(), (float) r.mContactPointOn2.GetY(), (float) r.mContactPointOn2.GetZ() };
                cts[h].n = { nrm.GetX(), nrm.GetY(), nrm.GetZ() };
                cts[h].depth = r.mPenetrationDepth;
                gm[h] = wm::ComputeGeom(cts[h], c, U, a, R, settings->mRadius);
            }
            wm::WMSlots slots = wm::Classify(gm, n);

            auto vpAt = [&](const wm::vec3& p) {
                const JPH::Vec3 rr(p.x - (float) comW.GetX(), p.y - (float) comW.GetY(), p.z - (float) comW.GetZ());
                const JPH::Vec3 v = vLin + vAng.Cross(rr);
                return wm::vec3{ v.GetX(), v.GetY(), v.GetZ() };
            };
            auto evalSlot = [&](int idx, bool side) -> wm::WMForce {
                if (idx < 0) return {};
                const wm::WMGeom& g = gm[idx];
                const float w = side ? g.wl : g.wr;
                return wm::GeneralizedContactForce(cts[idx].p, cts[idx].n, g.pen, w, c, a,
                    vpAt(cts[idx].p), omega, R, tau, m, dt, P);
            };
            const wm::WMForce fG = evalSlot(slots.ground, false);

            if ((state.frameCounter % 30) == 0) {
                const float penG = slots.ground >= 0 ? gm[slots.ground].pen : 0.0f;
                LOG_INFO("docs §39.1: wheelmodel eval (%s) wheel=%zu hits=%d groundSlot=%d pen=%.3f "
                    "F_ground=(%.0f,%.0f,%.0f) |F|=%.0f fpar=%.0f (m=%.1f dt=%.4f)",
                    label, i, nHits, slots.ground, (double) penG,
                    (double) fG.F.x, (double) fG.F.y, (double) fG.F.z,
                    (double) wm::Len(fG.F), (double) fG.fpar_w, (double) m, (double) dt);
            }
        }
    }

    // docs §40: (re)initialise the wheelmodel suspension DOF so each wheel starts exactly ON the
    // ground for the given chassis pose - a raycast down from each attachment sets both the
    // current length AND the spring zero-force (rest) length, so comp=0 (no launch) and the wheel
    // has support from frame 1 instead of the chassis dropping onto its belly through a one-sided
    // heightfield. Also zeroes the per-wheel rates (suspVel/omega). Called from BuildShadow and
    // after any teleport (TeleportPlayerShadow): a teleport moves the body but would otherwise
    // leave wmSuspLen at the OLD pose's ground height, so the wheels graze/miss the new ground and
    // the chassis sinks (seen live: a spawn-reset run sank the shadow 49m while ODE stayed put).
    static void InitWheelModelSuspension(JPH::PhysicsSystem* physics, ShadowState& state,
                                         const hta::CVector& pos, const hta::Quaternion& rot,
                                         const char* label) {
        if (physics == nullptr || state.constraint == nullptr)
            return;
        state.wmGear = 0; // docs §41: start in 1st gear, matching a vehicle about to move off from rest
        state.wmEngineRpm = 0.0f;
        const size_t nw = std::min(state.wmSuspLen.size(), (size_t) state.constraint->GetWheels().size());
        const JPH::RMat44 bxform = JPH::RMat44::sRotationTranslation(
            JPH::Quat(rot.x, rot.y, rot.z, rot.w), JPH::RVec3(pos.x, pos.y, pos.z));
        const JPH::BroadPhaseLayerFilter& bpF = physics->GetDefaultBroadPhaseLayerFilter(kWheelQueryLayer);
        const JPH::DefaultObjectLayerFilter olF = physics->GetDefaultLayerFilter(kWheelQueryLayer);
        // docs stage2-plan.md §Step 2: found live via ram_test - this function re-seats the
        // SCALAR suspension bookkeeping (wmSuspLen/wmRestLen, mode 2's whole model and mode 4's
        // rest-length target) at the new pose, but wheelBodyMode (mode 4) ALSO has real Jolt
        // wheel bodies jointed to the chassis by a SixDOFConstraint - and this function never
        // moved them. TeleportPlayerShadow moves the chassis body directly
        // (bodyInterface.SetPositionAndRotation) and calls this right after; left as it was, the
        // wheel bodies stayed exactly where they were (potentially thousands of metres away, at
        // the OLD chassis pose) while now jointed to a chassis that just jumped somewhere else -
        // the constraint solver saw an enormous violation and fought it every step, which is
        // exactly the ~450-460 m/s "smooth near-ballistic launch, independent of ram_test_offset"
        // observed live (offset changes where the CHASSIS lands; it does nothing about the wheels
        // being left behind, which is why 15m and 35m failed identically). Reusing suspLen/
        // attachW/suspDirW already computed above for the raycast - same position formula
        // BuildShadow itself uses at construction time (wheelCentreWorld = attach + dir*len).
        JPH::BodyInterface* bi = state.wheelBodyMode ? &physics->GetBodyInterface() : nullptr;
        // docs §46f (task #59): user-confirmed live via screenshot + §46b telemetry on Ural01 -
        // a plain save load (no car(N)) can ALSO put the chassis implausibly high above ground,
        // not just embedded below it. §132 already fixes "embedded below" for the chassis's own
        // bounding box, but nothing corrects the opposite case, and §132's box-level check can't
        // catch it anyway - the WHEEL ATTACH points sit below the chassis's own lowest bound, so
        // the chassis box itself can be perfectly clear of the ground while every wheel still
        // needs 90%+ of its travel to reach it (measured live: 92-95% on all 6 of Ural01's
        // wheels simultaneously, attachW.y a full ~1m above real ground - a stale saved position
        // for this specific vehicle/terrain, same root cause class as §132, opposite direction).
        for (size_t i = 0; i < nw; ++i) {
            const JPH::WheelSettings* s = state.constraint->GetWheel((JPH::uint) i)->GetSettings();
            const float maxLen = s ? s->mSuspensionMaxLength : 0.5f;
            const float R = s ? s->mRadius : 0.3f;
            float suspLen = maxLen; // airborne fallback
            JPH::RVec3 attachW  = bxform.GetTranslation();
            JPH::Vec3  suspDirW = JPH::Vec3::sAxisY();
            if (s != nullptr) {
                attachW  = bxform * s->mPosition;
                suspDirW = bxform.Multiply3x3(s->mSuspensionDirection).Normalized();
                // docs §127: user-reported live, "chassis jacked up above wheels" right after a
                // vehicle switch (car(N)/ChangeVehicleByNew). The old +1.0f margin only reaches
                // ~1-2m below the attach point - a NEW vehicle's wheels are commonly further than
                // that from the OLD vehicle's ground height (different chassis/wheel geometry),
                // so this raycast routinely missed and fell through to the "airborne fallback"
                // (suspLen=maxLen, full droop) below - which then gets baked into BOTH the
                // wheel's spawn position AND the strut's own "return to spawn offset" target
                // (BuildWheelBodies, SetTargetPositionCS(sZero())) - once the wheel finds REAL
                // ground and the strut still thinks maxLen (full extension) is neutral, it keeps
                // pulling the chassis up toward that wrong target. +15.0f covers the realistic
                // range of a vehicle-switch height mismatch without the cost of a much longer
                // cast (one ray per wheel, once per rebuild - not a hot-path cost either way).
                //
                // docs §46 (task #59): §127's fix above covers a wheel that spawns TOO FAR
                // ABOVE the ground. Live repro (switching a short/low vehicle - e.g. Scout01 -
                // into a tall/long-travel one - Ural01 - repeatedly at the same spot) found the
                // opposite case: pos.y is inherited close to the OLD vehicle's low resting height,
                // and the NEW vehicle's own attachPos offset (its own, unrelated-to-any-other-
                // vehicle local geometry) then places attachW slightly BELOW the real ground
                // surface - a case §132 already named for the chassis shape ("chassis embeds
                // below terrain") but never covered for individual wheel attach points, which can
                // sit lower than the chassis's own lowest bound. Casting straight down FROM
                // attachW in that state never finds the ground: the terrain collision mesh is
                // one-sided (docs §40 - CollideShape/raycasts only see it from above), so a ray
                // that starts already on/below the surface and points further down cannot hit it,
                // no matter how long the ray is - confirmed live: a 500m version of this exact ray
                // still missed. Same fix shape as §132: start the cast safely ABOVE the attach
                // point instead of AT it, so the ray always approaches the one-sided surface from
                // the correct side regardless of which side of it attachW happens to land on.
                const float upMargin = 5.0f;
                const float rayLen = upMargin + maxLen + R + 15.0f;
                const JPH::RVec3 rayStart = attachW - suspDirW * upMargin; // upMargin above attachW
                JPH::RRayCast ray{ rayStart, suspDirW * rayLen };
                JPH::RayCastResult hitR;
                if (physics->GetNarrowPhaseQuery().CastRay(ray, hitR, bpF, olF)) {
                    const float dFromStart  = rayLen * hitR.mFraction; // rayStart -> ground distance
                    const float dFromAttach = dFromStart - upMargin;   // attachment -> ground distance
                    const float rawBottomDist = dFromAttach - R;       // uncapped wheel-bottom -> ground distance
                    suspLen = std::clamp(rawBottomDist, s->mSuspensionMinLength, maxLen); // wheel bottom at ground
                } else {
                    // docs §46 (task #59): permanent, cheap warning (not a temporary diagnostic) -
                    // if this still fires live, the wheel is genuinely landing in the "airborne
                    // fallback" (suspLen=maxLen, full droop) that reads as "stretched suspension",
                    // and it's neither the §127 case (ray too short) nor the §46 case (ray started
                    // on the wrong side of a one-sided terrain surface) - both are now covered, so a
                    // live hit here means a third, still-unidentified cause and is worth a report.
                    LOG_WARNING("docs §46: wheel %zu InitWheelModelSuspension raycast missed even "
                        "with the upMargin=%.1f/%.1fm range fix (%s) - falling back to suspLen=maxLen=%.3f "
                        "(this wheel will look fully extended)",
                        i, (double) upMargin, (double) rayLen, label, (double) maxLen);
                }
            }
            // docs §46b (task #59): permanent, cheap - one line per wheel per build, not per
            // frame. Reports the ACTUAL computed rest length so a live "looks stretched" report
            // can be checked against real numbers instead of guessed at: a suspLen near maxLen
            // here is a legitimate raycast result (real ground is genuinely that far away),
            // distinct from the §46 fallback warning above (raycast found nothing at all).
            LOG_INFO("docs §46b: wheel %zu (%s) suspLen=%.3f maxLen=%.3f (%.0f%% of travel) "
                "attachW.y=%.3f",
                i, label, (double) suspLen, (double) maxLen,
                (double) (maxLen > 0.0f ? 100.0f * suspLen / maxLen : 0.0f), (double) attachW.GetY());
            state.wmSuspLen[i] = suspLen;
            state.wmRestLen[i] = suspLen; // spring zero-force point = where the wheel starts on the ground
            if (i < state.wmSuspVel.size()) state.wmSuspVel[i] = 0.0f;
            if (i < state.wmOmega.size())   state.wmOmega[i]   = 0.0f;

            if (bi != nullptr && i < state.wheelBodies.size() && !state.wheelBodies[i].IsInvalid()) {
                const JPH::RVec3 wheelCentreW = attachW + suspDirW * suspLen;
                bi->SetPositionAndRotation(state.wheelBodies[i], wheelCentreW,
                    JPH::Quat(rot.x, rot.y, rot.z, rot.w), JPH::EActivation::Activate);
                bi->SetLinearVelocity(state.wheelBodies[i], JPH::Vec3::sZero());
                bi->SetAngularVelocity(state.wheelBodies[i], JPH::Vec3::sZero());
            }
        }

        // docs §46g (task #59): §46f (a wheel-travel-fraction heuristic, retired) and a
        // continuous position-correction corrector (also retired - user feedback: wheels are
        // real physics bodies, fix the constraint's own limit instead of fighting the symptom
        // every frame) were both tried and abandoned here. The actual fix lives at the source:
        // WheelSettingsWV::mSuspensionMaxLength, where the wheel settings are built (search
        // "docs §46g: real suspension travel"). See that comment for the reasoning.
    }

    // docs §41: real, self-contained multi-gear drivetrain - replaces docs §40's flat
    // drive_torque constant + linear torque_falloff_omega curve. That single curve couldn't
    // fit both a short launch window and a long sustained drive (measured: tuned for 5s gave
    // ratio~1.0 there but undershot 0.79 at 8s) because they're different regimes of the SAME
    // real multi-gear system, not a tuning miss - so build the real system instead of a better
    // single curve.
    //
    // All the real numbers here were found by disassembling the actual game functions (not
    // guessed): ai::Vehicle::_KeepGearBox (RVA 0x1e0e40) and ai::Vehicle::_CalcRpms (RVA
    // 0x1db3b0). _CalcRpms computes, for an ODE-simulated vehicle, an average wheel angular
    // velocity (m_averageWheelAVel) then:
    //   engineRpm = GEAR_RATIOS[currentGear] * diffRatio * wheelAVel * 108.0 * (1/(2*pi))
    // (both 108.0 and 1/(2*pi) are read directly from the binary's static data, RVA 0x592b00
    // and 0x5e5960). GEAR_RATIOS is a real static const float[5] on ai::Vehicle -
    // {4.0, 2.5, 1.5, 1.0, 0.7} - a single game-wide 5-speed table shared by every vehicle
    // prototype (already declared in extern/hta/source/hta/ai/Vehicle.hpp). _KeepGearBox's own
    // shift rule is a simple threshold, no extra hysteresis beyond the low/high gap:
    //   if (engineRpm < m_lowGearShiftLimit)  gear -= 1;
    //   if (engineRpm > m_highGearShiftLimit) gear += 1;
    //   gear = clamp(gear, 0, 4);
    // and it drives each wheel's real ODE Hinge2 motor with a target-velocity + max-force pair
    // derived from gear/diff/GetMaxTorque() (a velocity-servo, not a raw constant torque) -
    // confirming the origin spring_wheel branch's "chase a target speed" philosophy was the
    // right shape, just via real per-vehicle data instead of a live P-controller.
    //
    // This function does NOT read the real ODE vehicle's live m_currentGear/m_engineRpm/
    // m_averageWheelAVel - only its FIXED, real per-vehicle-type data (diffRatio, shift RPM
    // limits, max torque, redline) plus the game-wide GEAR_RATIOS table. Gear/RPM state is our
    // OWN (ShadowState::wmGear/wmEngineRpm), driven by OUR OWN wheel omega - so the model stays
    // self-contained (works even if ODE's real vehicle sim is ever retired), matching the
    // Jolt migration's actual goal, unlike literally chasing ODE's live wheel speed which would
    // only work as long as ODE keeps simulating this vehicle in parallel.
    //
    // Returns the max torque (N.m, unscaled by throttle) available at each driven wheel this
    // step; the caller multiplies by throttle position, same convention as the old code.
    //
    // hta::ai::Vehicle::GEAR_RATIOS[5] is declared in extern/hta/source/hta/ai/Vehicle.hpp
    // (confirming it's a REAL static const array in the game binary, RVA 0x591b2c) but has no
    // out-of-line definition anywhere in this codebase - only member FUNCTIONS get NATIVE()
    // linkage here, not static data members, so referencing it directly is a link error. Kept as
    // a local, explicitly-labelled copy of the exact values read off the binary instead of
    // inventing a new data-binding mechanism for extern/hta just for this one constant.
    static constexpr float kGearRatios[5] = { 4.0f, 2.5f, 1.5f, 1.0f, 0.7f }; // == real GEAR_RATIOS
    // docs §67 (Этап 1, шаг 5): what mode 4 needs out of the gearbox beyond the single torque
    // mode 2 wanted. Kept as an OUT-param with a default of nullptr so mode 2's call is
    // byte-for-byte the call it always was - mode 2 is the rollback path on every step of Stage 1,
    // and a step that quietly changes it has removed the thing it is supposed to fall back to.
    struct GearboxOut {
        float    driveTorque = 0.0f;  // engine torque delivered at the wheels, before the split
        float    perWheel    = 0.0f;  // driveTorque / nDriven - mode 2 does NOT divide
        float    omegaTarget = 0.0f;  // the wheel speed at which the engine would hit its redline
        uint32_t nDriven     = 0;
        // docs §102: the speed governor's inputs and its verdict, kept for the log so a run that
        // is slower than expected can be told from one that is being governed.
        float    speedLimit   = 0.0f;
        float    surfaceSpeed = 0.0f;
        bool     governed     = false;
    };

    static float StepWheelModelGearbox(hta::ai::Vehicle* vehicle, ShadowState& state, float dt,
                                       const float* omegaOverride = nullptr, GearboxOut* out = nullptr) {
        float sumOmega = 0.0f;
        int   nDriven  = 0;
        // In mode 4 the spin lives on the bodies and wmOmega is never allocated, so the source of
        // the average is the caller's array; mode 2 keeps reading its own scalar integrator.
        const size_t nw = (omegaOverride != nullptr)
            ? state.wheelOrder.size()
            : std::min(state.wmOmega.size(), state.wheelOrder.size());
        for (size_t i = 0; i < nw; ++i) {
            const hta::ai::Wheel* hw = state.wheelOrder[i];
            if (hw != nullptr && hw->m_driven) {
                sumOmega += std::fabs((omegaOverride != nullptr) ? omegaOverride[i] : state.wmOmega[i]);
                ++nDriven;
            }
        }
        // docs §41.1: average across DRIVEN wheels only - a real differential only feels driven
        // wheels; non-driven wheels can free-roll at a different rate on rough terrain without
        // affecting engine RPM in a real vehicle either.
        const float avgOmega = (nDriven > 0) ? (sumOmega / (float) nDriven) : 0.0f;

        const float diffRatio = vehicle->m_diffRatio;
        constexpr float kRpmScale = 108.0f / (2.0f * 3.14159265f); // RVA 0x592b00 / 0x5e5960

        state.wmGear = std::clamp(state.wmGear, 0, 4);
        const float gearRatioNow = kGearRatios[state.wmGear];
        state.wmEngineRpm = gearRatioNow * diffRatio * avgOmega * kRpmScale;

        // docs §41.1: real threshold shift rule (see _KeepGearBox above) - own gear counter,
        // not the real vehicle's m_currentGear.
        if (state.wmEngineRpm < vehicle->m_lowGearShiftLimit)  state.wmGear -= 1;
        if (state.wmEngineRpm > vehicle->m_highGearShiftLimit) state.wmGear += 1;
        state.wmGear = std::clamp(state.wmGear, 0, 4);

        const float gearRatio = kGearRatios[state.wmGear];
        const float rpmAtGear = gearRatio * diffRatio * avgOmega * kRpmScale;

        // docs §41.1: real drivetrain relationship - wheel torque = engine torque * gear ratio *
        // diff ratio (power roughly conserved through the gearbox). GetMaxTorque() is the
        // vehicle's own real (possibly forced-override) max engine torque - a real accessor
        // already NATIVE-bound in extern/hta, not a guessed constant.
        float wheelTorque = vehicle->GetMaxTorque() * gearRatio * diffRatio;

        // docs §41.1: rev limiter. _KeepGearBox has no higher gear once already at gear 4 (top,
        // last GEAR_RATIOS entry) - without an explicit cap here the model would still
        // accelerate forever past redline once stuck in top gear (the same unbounded-
        // acceleration problem docs §40 found and fixed for a single gear, just delayed by 4
        // more gear ratios). Taper smoothly over the last 10% below the vehicle's OWN real
        // m_maxEngineRpm so top speed is bounded by this vehicle's real redline in its real top
        // gear, not an arbitrary tuned constant.
        if (state.wmGear >= 4) {
            const float redline = std::max(vehicle->m_maxEngineRpm, 1.0f);
            const float taper = std::clamp((redline - rpmAtGear) / (0.1f * redline), 0.0f, 1.0f);
            wheelTorque *= taper;
        }

        // docs §102 (§95.3 ported): the SPEED GOVERNOR. Mode 4 only - it hangs off `out`, so mode
        // 2's call is still the call it always was.
        //
        // _KeepGearBox (RVA 0x1e0e40, vehicle.cpp:6532-6547) does this before handing torque to
        // the joint, and every piece of it is read from the disassembly:
        //   limit = m_bIsControlledByPlayer(+0x2dc) ? GetMaxSpeed()
        //         : m_attackStatus(+0x338) == 1     ? GetMaxSpeed()
        //                                          : m_cruisingSpeed(+0x214)
        //   if (m_turboThrottleTime(+0x1cc) > 0) limit *= m_turboThrottleValue(+0x1d0)
        //   if (speed - limit > 0.1 && m_brake <= proto->m_selfBrakingCoeff + 1e-4) drive = 0
        //
        // Two details that are easy to get wrong and are NOT guesses. First, `speed` is the WHEEL
        // SURFACE speed |m_averageWheelAVel| * R (lines 6519-6521 read the first wheel's
        // Sphere::GetRadius and multiply), not the chassis' linear velocity - so a vehicle whose
        // wheels are spinning is governed even if it is going nowhere. Second, the cut only
        // applies when the driver is NOT braking: m_brake at or below the prototype's idle
        // self-braking floor. Constants 0.1 @0x5e599c and 1e-4 @0x5e5b40.
        //
        // This matters beyond fidelity: §101 measured that porting the §94 downforce WITHOUT this
        // makes divergence worse, because the assists add grip and nothing takes the speed back.
        if (out != nullptr) {
            const hta::ai::VehiclePrototypeInfo* proto = vehicle->GetPrototypeInfo();
            float speedLimit = (vehicle->m_bIsControlledByPlayer || (int) vehicle->m_attackStatus == 1)
                ? vehicle->GetMaxSpeed()
                : vehicle->m_cruisingSpeed;
            if (vehicle->m_turboThrottleTime > 0.0f)
                speedLimit *= vehicle->m_turboThrottleValue;
            // The same first wheel the reference measures against.
            const float govR = state.wheelSetup.empty() ? 0.0f : state.wheelSetup[0].radius;
            const float surfSpeed = avgOmega * govR;   // avgOmega is already |omega|
            const float brakeFloor = (proto != nullptr) ? proto->m_selfBrakingCoeff + 1.0e-4f : 1.0e-4f;
            out->speedLimit  = speedLimit;
            out->surfaceSpeed = surfSpeed;
            out->governed = (surfSpeed - speedLimit > 0.1f) && (vehicle->m_brake <= brakeFloor);
            if (out->governed && state.VarGovernor() != 0)
                wheelTorque = 0.0f;
        }

        if (out != nullptr) {
            out->driveTorque = wheelTorque;
            out->nDriven     = (uint32_t) std::max(nDriven, 0);
            // The split across driven wheels. Mode 2 hands the FULL torque to every wheel, so this
            // is a real change in delivered torque by a factor of nDriven - it is not in the plan
            // at all and is visible only because the recovered log has separate driveTorque and
            // perWheel fields. The log is newer than the plan.
            out->perWheel    = wheelTorque / (float) std::max(nDriven, 1);
            // The exact inverse of the RPM relation above: the wheel speed at which the engine
            // would sit on its own redline in the gear it is now in. The gear-4 taper deliberately
            // does NOT appear here - it shapes torque, not the servo's target, and folding it in
            // would quietly lower top speed in top gear only.
            const float denom = gearRatio * diffRatio * kRpmScale;
            out->omegaTarget  = (std::fabs(denom) > 1.0e-6f)
                ? std::max(vehicle->m_maxEngineRpm, 0.0f) / denom
                : 0.0f;
        }

        return wheelTorque;
    }

    // docs §101 (§94 ported): the two arcade assists ai::Vehicle::_ApplyStabilizingForces
    // (RVA 0x1d5e60, vehicle.cpp:6717-6769) applies to every ODE vehicle every frame. The shadow
    // has never reproduced either, and §98/§99 both ended up pointing here: without the downforce
    // the shadow leaves the ground above ~25 m/s, which dominates every post-throttle metric and
    // leaves an unloaded steered wheel with nothing to hold it off its stop.
    //
    // Every constant below was read out of the binary, not taken from the notes:
    //   -0.1962 @0x5927d8 (= -0.02*9.81), 5.0 @0x5e59ac, 0.5 @0x5e5968,
    //   INITIAL_UP_DIRECTION @0x60095c = (0,1,0)
    //
    // The shadow evaluates the RULE on its OWN state - its own velocity and its own contact count -
    // rather than copying the reference's force. Copying would inject reference state into the
    // thing being compared and make the divergence metric measure nothing.
    static void ApplyArcadeAssists(hta::ai::Vehicle* vehicle, ShadowState& state) {
        if (vehicle == nullptr || !state.wheelBodyMode)
            return;
        if (state.VarAssists() == 0)
            return;
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return;
        JPH::Body* chassis = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.bodyId);
        if (chassis == nullptr || !chassis->IsActive())
            return;
        const hta::ai::VehiclePrototypeInfo* proto = vehicle->GetPrototypeInfo();
        if (proto == nullptr)
            return;

        const JPH::Vec3 v    = chassis->GetLinearVelocity();
        const float     mass = std::max(vehicle->GetMass(), 1.0f);
        // OUR contact count, not the game's m_numWheelsTouchingGround: the game RESETS that field
        // to 0 at the end of _ApplyStabilizingForces (mov [esi+0x4b8], ebx), so what it reads at
        // our pre-step depends on where we sit relative to that reset. The band already knows how
        // many of our wheels are on the ground, and that is the shadow's own answer to the same
        // question.
        const uint32_t wheelsDown = (state.stepListener != nullptr)
            ? state.stepListener->lastWheelsWithContact : 0;
        if (wheelsDown == 0)
            return;

        // --- 1. Downforce. Gated on horizontal speed only (the disassembly squares [esp+0x18] and
        // [esp+0x20] and skips [esp+0x1c]), so a vehicle falling fast gets none of it.
        const float horizVel = std::sqrt(v.GetX() * v.GetX() + v.GetZ() * v.GetZ());
        float downForce = 0.0f;
        if (horizVel > 5.0f) {
            downForce = -0.1962f * mass * proto->m_pressingForce * horizVel;
            chassis->AddForce(JPH::Vec3(0.0f, downForce, 0.0f));
        }

        // --- 2. Yaw assist. The game turns the vehicle with a torque, straight past the tyres.
        // curAngle comes from the FIRST wheel with a live object, not from a steered wheel and not
        // averaged - the loop at 0x1d6100 walks the WheelInfo array and takes the first whose
        // Wheel* is non-null. Faithful, even though it looks arbitrary.
        const hta::ai::Wheel* firstWheel = nullptr;
        for (hta::ai::Wheel* w : state.wheelOrder) {
            if (w != nullptr) { firstWheel = w; break; }
        }
        float torqueMag = 0.0f;
        if (firstWheel != nullptr) {
            const float speed = v.Length();               // FULL speed here, not horizontal
            const JPH::Vec3 fwd = chassis->GetRotation() * JPH::Vec3(0.0f, 0.0f, 1.0f);
            // comiss 0, dot ; jbe -> +1. So dot >= 0 gives +1, reversing gives -1: the assist
            // turns the vehicle the same way whether it is going forwards or backwards.
            const float sgn = (fwd.Dot(v) >= 0.0f) ? 1.0f : -1.0f;
            const float throttleTerm = std::fabs(0.5f * vehicle->m_throttle) + 0.5f;
            torqueMag = firstWheel->m_curAngle * mass * speed * vehicle->m_driftCoeff
                      * vehicle->_GetCabinControlCoeff() * sgn * throttleTerm;
            // AddRelTorque takes a BODY-relative vector; the base is -INITIAL_UP_DIRECTION, so
            // body-local -Y. Rotating it into world is what AddRelTorque does internally.
            //
            // docs §109: the sub-gate sits HERE, after the term is computed, so the off arm still
            // reports the magnitude it declined to apply.
            if (state.VarAssistYaw() != 0) {
                const JPH::Vec3 tLocal(0.0f, -torqueMag, 0.0f);
                chassis->AddTorque(chassis->GetRotation() * tLocal);
            }
        }

        state.wmAssistDownForce = downForce;
        state.wmAssistTorqueRaw = torqueMag;
        state.wmAssistTorque    = (state.VarAssistYaw() != 0) ? torqueMag : 0.0f;
        state.wmAssistHorizVel  = horizVel;
    }

    // docs §103 (§95.3 ported): soil rolling drag, the last un-ported every-frame force.
    // ai::CollideWheelAndLandscape (RVA 0x491db0, source lines 193-314) ends with, at line 311:
    //
    //   F = -SoilProps::m_resistance * wheel->GetMass() * wheelVelocity * (1.0 / wheelRadius)
    //
    // applied to the WHEEL, not the chassis. Three things here were settled by reading the code
    // rather than by choosing the convenient answer:
    //   * the mass is the WHEEL's own - `mov edx,[esi]; call [edx+0x110]` dispatches on esi, and
    //     esi is the wheel (the same object whose [+0x120]->[+0x114]->[vtbl+0x18] chain is its
    //     Sphere::GetRadius). §95.3 cited [vtbl+0x110] and then called it the wheel's mass, which
    //     are different claims; this is the one that holds.
    //   * the scalar is m_resistance (SoilProps +0x20, read into xmm3 at 0x892639), NOT
    //     m_friction (+0x1c, which feeds a different path at 0x8925e8).
    //   * it is a plain linear damper on velocity, outside the contact loop and not subject to the
    //     friction circle - which is exactly why folding rolling resistance into Pacejka (§42) is
    //     NOT the same mechanism.
    //
    // Scale, since §95.3 dismissed this one against the wrong quantity: at 28 m/s on Molokovoz01
    // it is ~580 N across four wheels against ~935 N of average tractive force. Not 1.4% of
    // anything.
    static void ApplySoilRollingDrag(ShadowState& state, const char* label) {
        if (!state.wheelBodyMode)
            return;
        if (state.VarSoilDrag() == 0)
            return;
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        hta::ai::DynamicScene* scene = hta::ai::DynamicScene::Instance();
        if (physics == nullptr || scene == nullptr)
            return;
        // Same derivation the other two soil lookups in this file use - world units per tile, not
        // the tile count. Copying the expression rather than the concept: getting this inverted
        // would sample a soil tile far from the wheel and the drag would look randomly wrong.
        const float tileSize = (float) hta::ai::CServer::Instance()->GetLevelSize()
            / (float) hta::ai::CServer::Instance()->GetWorld()->GetLandscape().GetTileSize();
        if (!(tileSize > 0.0f))
            return;

        float applied = 0.0f, resistSeen = 0.0f;
        const size_t nw = std::min(state.wheelBodies.size(), state.wheelSetup.size());
        for (size_t i = 0; i < nw && i < kMaxHarvestWheels; ++i) {
            // Only for a wheel actually touching the landscape: the reference applies this from
            // INSIDE the wheel-vs-landscape collide handler, so an airborne wheel never sees it.
            if (state.stepListener == nullptr || state.stepListener->diag[i].recsTyre == 0)
                continue;
            JPH::Body* wb = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.wheelBodies[i]);
            if (wb == nullptr || !wb->IsActive())
                continue;

            const JPH::RVec3 p = wb->GetCenterOfMassPosition();
            const int32_t soilX = (int32_t) ((float) p.GetX() / tileSize + 0.5f);
            const int32_t soilZ = (int32_t) ((float) p.GetZ() / tileSize + 0.5f);
            const hta::ai::DynamicScene::SoilProps& props =
                scene->GetSoilProps((uint32_t) soilX, (uint32_t) soilZ);
            if (!(props.m_resistance > 0.0f))
                continue;

            const float R = std::max(state.wheelSetup[i].radius, 1.0e-3f);
            const float m = 1.0f / std::max(wb->GetMotionProperties()->GetInverseMass(), 1.0e-8f);
            const JPH::Vec3 F = -wb->GetLinearVelocity() * (props.m_resistance * m / R);
            wb->AddForce(F);
            applied += F.Length();
            resistSeen = props.m_resistance;
        }
        state.wmSoilDragN     = applied;
        state.wmSoilResistance = resistSeen;
        (void) label;
    }

    // docs §122: ODE's GLOBAL body damping, which the port has never matched.
    //
    // ai::DynamicScene::InitOnce switches it on for the whole world - dWorldSetDampingFlag(1),
    // dWorldSetDampingParameters(linear 0.1, angular 0.3) - and dBodyCreate (RVA 0x3c5ac0) copies
    // those into EVERY body at construction, chassis and wheels alike, with no per-body override
    // anywhere in the game. dxStepBody (RVA 0x4fc8f0) then does, once per body per step:
    //
    //     lvel *= (1 - linear_scale * h)        avel *= (1 - angular_scale * h)
    //
    // with NO velocity threshold - dxDamping in this build is a bare {float,float}, the threshold
    // fields belong to a later ODE revision - and with the stepsize multiplied in explicitly, so
    // the two numbers are rates in 1/s rather than a per-tick haircut.
    //
    // Jolt writes the same law: MotionProperties.inl:143 is `v *= max(0, 1 - c*dt)`. So this is an
    // assignment of two constants, not a reimplementation, and the only difference is a clamp that
    // cannot engage at c*dt in the 0.003-0.01 range.
    //
    // As-built state, which is what the OFF arm preserves: wheel bodies are explicitly 0/0 (see
    // BuildWheelBodies) and the chassis is on Jolt's 0.05/0.05 default, never having been set. So
    // "the port has no damping" is not quite right - it has half the linear on the chassis and none
    // on the wheels.
    //
    // Written every frame rather than once at build, so a variant or config value takes effect
    // without a rebuild and a rebuilt body cannot silently come back on the wrong value. Two setter
    // calls per body is nothing beside the step.
    //
    // The wheels get it only in mode 4, because only there do they exist. That is the faithful
    // mapping and not an omission: mode 2 has no wheel body for the angular term to act on, and
    // folding it into the chassis instead would invent a force the reference does not apply.
    static void ApplyBodyDamping(ShadowState& state, const char* label) {
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return;
        const JPH::BodyLockInterfaceNoLock& bodies = physics->GetBodyLockInterfaceNoLock();
        const bool on = state.VarBodyDamping() != 0;

        float seenLin = 0.0f, seenAng = 0.0f;
        uint32_t touched = 0;
        const auto visit = [&](JPH::BodyID id) {
            JPH::Body* b = bodies.TryGetBody(id);
            if (b == nullptr || !b->IsDynamic())
                return;
            JPH::MotionProperties* mp = b->GetMotionProperties();
            if (mp == nullptr)
                return;
            if (on) {
                mp->SetLinearDamping(std::max(state.VarDampingLinear(), 0.0f));
                mp->SetAngularDamping(std::max(state.VarDampingAngular(), 0.0f));
            }
            // Read back either way - on the off arm this is the whole point of the line.
            seenLin = mp->GetLinearDamping();
            seenAng = mp->GetAngularDamping();
            ++touched;
        };

        if (state.wheelBodyMode) {
            for (JPH::BodyID id : state.wheelBodies)
                visit(id);
        }
        // Chassis LAST so the reported pair is the chassis's, which is the one that exists in every
        // mode and so the one a log line can be compared across modes.
        visit(state.bodyId);

        state.wmDampLinear  = seenLin;
        state.wmDampAngular = seenAng;
        state.wmDampBodies  = touched;
        (void) label;
    }

    // docs §67 (Этап 1, шаг 5): the per-frame spin command. MAIN THREAD, before
    // PhysicsSystem::Update, from live game fields - nothing here may run inside the step listener
    // or a contact callback.
    //
    // The drive is a TORQUE-LIMITED VELOCITY MOTOR, not an explicit torque. That is not a
    // preference: ai::Vehicle::_KeepGearBox (RVA 0x1e0e40) drives the ODE Hinge2 through
    // dParamVel2 / dParamFMax2, which is exactly a velocity servo with a force cap, so the motor
    // is the port and the explicit-torque integrator in StepWheelModel is mode 2's own thing.
    // Porting THAT would drag kMaxOmega, react_scale and the manual spin reaction back in with it.
    static void UpdateWheelSpinCommands(hta::ai::Vehicle* vehicle, ShadowState& state, float dt) {
        using EAx = JPH::SixDOFConstraint::EAxis;
        if (vehicle == nullptr || dt <= 1.0e-6f || !state.wheelBodyMode)
            return;
        // The two DOFs are independent levers - either can be rolled back without the other, so
        // this runs whenever EITHER is on, and each block checks its own gate.
        const bool spinDofOn  = state.VarSpin()  != 0;
        const bool steerDofOn = state.VarSteer() != 0;
        if (!spinDofOn && !steerDofOn)
            return;

        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return;
        const size_t nw = std::min(state.wheelConstraints.size(), state.wheelSetup.size());
        if (nw == 0)
            return;
        state.wmSpinCmd.assign(nw, ShadowState::SpinCmd());
        state.lastStepDt = dt;

        // PRE-STEP snapshot. The "| PRE-STEP" in the log line is a contract - see SpinCmd.
        //
        // m_realThrottle is the drive command, but it is NOT bounded to [-1,1] and clamping it is
        // wrong. docs §67.2 measured three regimes over one run:
        //   throttle=0.985 brake=0.000 realThrottle=0.985   free driving  (rpm ~1600)
        //   throttle=0.000 brake=0.006 realThrottle=1.000   scripted drive (rpm 3200-4900)
        //   throttle=0.000 brake=0.006 realThrottle=+-10.00 at rest       (rpm ~0)
        // In the third the sign follows -sign(engineRpm) and flips frame to frame as rpm dithers
        // about zero. Clamped into [-1,1] that reads as full throttle forward and full throttle
        // reverse on alternating frames: mode 2's scalar integrator averages it into nothing, a
        // velocity servo executes it, and it wrecked the brake tail (angle 106-150 deg).
        //
        // So the split is on MAGNITUDE, not on which field: inside [-1,1] the value is a throttle,
        // outside it the brake term is dominating and there is no drive intent - confirmed by
        // m_throttle reading exactly 0 in all 32 such samples. The !(<=) form also rejects NaN.
        //
        // Reading m_throttle instead was tried and is wrong: it is 0 throughout the scripted drive
        // window (the harness writes m_realThrottle), so the shadow simply stopped moving. Mode 2's
        // comment about m_throttle reading 0 at this point in the frame is correct; the 0.985
        // samples above come from the free-driving phase before the vehicle is pinned.
        float driveIntent = vehicle->m_realThrottle;
        if (!(std::fabs(driveIntent) <= 1.0f))
            driveIntent = std::clamp(vehicle->m_throttle, -1.0f, 1.0f);
        const float throttle = driveIntent;
        const float brake     = std::clamp(vehicle->m_brake, 0.0f, 1.0f);
        const bool  handBrake = vehicle->m_bHandBrake;
        state.wmSpinThrottle  = throttle;
        state.wmSpinBrake     = brake;
        state.wmSpinHandBrake = handBrake;

        const JPH::Vec3 chassisAngVel =
            physics->GetBodyInterface().GetAngularVelocity(state.bodyId);

        // omega is RELATIVE spin, wheel minus chassis about the axle read OFF THE BODY. Absolute
        // angular velocity would feed chassis yaw and pitch into the gearbox as engine RPM.
        float omegaNow[kMaxHarvestWheels] = {};
        for (size_t i = 0; i < nw && i < kMaxHarvestWheels; ++i) {
            JPH::Body* wb = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.wheelBodies[i]);
            if (wb == nullptr)
                continue;
            const JPH::Vec3 axleWorld = (wb->GetRotation() * state.wheelSetup[i].axleLocal).Normalized();
            const float rawOmega = (wb->GetAngularVelocity() - chassisAngVel).Dot(axleWorld);
            omegaNow[i] = state.wheelSetup[i].spinSign * rawOmega;

            // Steer-drift fix: integrate spin as its OWN state, never read back off the body -
            // see wmSpinAngle's own comment. Raw (no spinSign) because this feeds
            // Quat::sRotation(axleLocal, ...) directly, the same physics-space convention
            // axleLocal itself is in.
            if (i < state.wmSpinAngle.size()) {
                state.wmSpinAngle[i] += rawOmega * dt;
                // Keep the accumulator numerically well-conditioned over a long play session -
                // Quat::sRotation only cares about the angle mod 2*pi, so wrapping changes
                // nothing about the resulting rotation.
                if (state.wmSpinAngle[i] > 3.14159265f)
                    state.wmSpinAngle[i] -= 6.28318531f;
                else if (state.wmSpinAngle[i] < -3.14159265f)
                    state.wmSpinAngle[i] += 6.28318531f;
            }
        }

        GearboxOut gb;
        StepWheelModelGearbox(vehicle, state, dt, omegaNow, &gb);
        state.wmDriveTorque  = gb.driveTorque;
        state.wmPerWheel     = gb.perWheel;
        state.wmDrivenCount  = gb.nDriven;
        state.wmSpeedLimit   = gb.speedLimit;
        state.wmSurfaceSpeed = gb.surfaceSpeed;
        state.wmGoverned     = gb.governed;

        const float mu    = state.stepListener != nullptr ? state.stepListener->params.mu : 1.0f;
        const float gAbs  = std::max(std::fabs(kraken::Config::Instance().gravity.value), 0.1f);
        const float mVeh  = std::max(vehicle->GetMass(), 1.0f);

        for (size_t i = 0; i < nw && i < kMaxHarvestWheels; ++i) {
            JPH::SixDOFConstraint* sc = static_cast<JPH::SixDOFConstraint*>(state.wheelConstraints[i]);
            JPH::Body* wb = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.wheelBodies[i]);
            if (sc == nullptr || wb == nullptr)
                continue;
            // Commands to a sleeping body do nothing. Gated rather than blanket-woken: an ungated
            // loop either wakes every vehicle every frame or silently stops driving a sleeping one.
            if (!wb->IsActive())
                continue;

            const WheelSetup& ws = state.wheelSetup[i];
            const float R = std::max(ws.radius, 1.0e-3f);

            // The contact torque cap. DECIDED HERE, not recovered: the recovered build's cap was
            // reverse-engineered from logs and the proposed floor m_vehicle*g*R was REFUTED during
            // verification (a 100 kg vehicle logged cap=432 Nm where that formula demands 816), so
            // there is no formula to port and inventing one silently would be the worse option.
            //
            // The choice: a wheel can never transmit more torque than its contact patch supports,
            // mu*F_n*R, so that is the ceiling; the floor is one corner's static weight so an
            // airborne wheel still gets a finite, sane handbrake instead of an infinite one.
            // F_n comes from the PREVIOUS step's band (docs §66 fnGround) - one step stale, which
            // for a cap is acceptable and is stated rather than hidden.
            const float fnPrev = (state.stepListener != nullptr && i < kMaxHarvestWheels)
                ? state.stepListener->diag[i].fnGround : 0.0f;
            const float cap = R * std::max(mu * fnPrev, mVeh * gAbs / (float) std::max<size_t>(nw, 1));

            float target = 0.0f, limit = 0.0f;
            // Every write below touches RotationX, which is MakeFixedAxis when wm4_spin=0 - and
            // commanding a motor on a fixed axis is not a no-op in Jolt, it asserts. The rollback
            // lever has to actually roll back.
            if (!spinDofOn) {
                // nothing: RotationX is locked, the wheel turns with the chassis
            } else if (handBrake) {
                // The handbrake overrides the drive ENTIRELY - throttle and gearbox torque are
                // ignored, which the recovered log confirms directly (throttle=1.00 and
                // perWheel=150Nm while cmdTq read 815-3049Nm in the same frame). Applied to every
                // wheel, driven or not: what a free-roller's motor does under handbrake is one of
                // the stated unknowns, and letting half the wheels spin freely is not a defensible
                // reading of "handbrake".
                sc->SetMotorState(EAx::RotationX, JPH::EMotorState::Velocity);
                target = 0.0f;
                limit  = cap;
            } else if (ws.driven) {
                sc->SetMotorState(EAx::RotationX, JPH::EMotorState::Velocity);
                // The target carries the throttle's SIGN (reverse asks for a negative wheel
                // speed); the limit is a magnitude, so the direction lives entirely in the target.
                target = gb.omegaTarget * ((throttle >= 0.0f) ? 1.0f : -1.0f);
                limit  = std::fabs(throttle) * gb.perWheel;
                // docs §120 (§118 ported): ENGINE BRAKING. The reference's force cap has no
                // throttle factor at all - _KeepGearBox builds dParamFMax2 from the gear ratio,
                // the diff, the wheel radius and the WHEEL SURFACE SPEED (§118.1), so off throttle
                // the servo still holds the wheel to the engine-RPM-derived target and brakes it.
                // The line above, with its |throttle| factor, releases the wheel entirely instead:
                // measured, the shadow keeps 18% of its wheel speed over an 8 s coast where the
                // reference reaches zero (§117).
                //
                // §120 MEASURED AND REFUTED - THIS IS NOT THE ENGINE-BRAKING PORT. Do not enable it
                // expecting one. Lifting the torque limit makes the wheel FASTER, not slower:
                //
                //     coast, wheel speed retained    reference 0%   off 0%   scale 0.3 54%   1.0 34%
                //
                // The reason is the TARGET, not the cap. `gb.omegaTarget` is the wheel speed at
                // which the engine would hit its REDLINE - a static ceiling from m_maxEngineRpm -
                // whereas the reference's dParamVel2 comes from [esi+0x23c], the CURRENT engine RPM,
                // which decays once the throttle is released. Give a velocity servo a redline target
                // and any torque at all, and it holds the wheel up there instead of braking it.
                //
                // A real port therefore has to change what the gearbox TARGETS - a decaying
                // RPM-derived speed - and only then does the cap matter. Left in place, defaulted
                // off, as the measurement that says where the work actually is.
                if (state.VarEngineBrake() != 0)
                    limit = std::max(limit, state.VarEngineBrakeScale() * gb.perWheel);
            } else {
                // Free-rolling: no motor at all, the tyre force alone spins it.
                sc->SetMotorState(EAx::RotationX, JPH::EMotorState::Off);
            }

            if (spinDofOn) {
                sc->GetMotorSettings(EAx::RotationX).SetTorqueLimit(limit);
                sc->SetTargetAngularVelocityCS(JPH::Vec3(ws.spinSign * target, 0.0f, 0.0f));
            }

            // The service brake as a FRICTION constraint, per plan:239 - it drives the relative
            // angular velocity to zero under a torque bound, which is what pads do. Flagged
            // explicitly: this is the one part of step 5 with NO recovered evidence. The
            // alternative (a second velocity-motor-to-zero, the route the handbrake demonstrably
            // used) behaves differently on a slope. The plan is followed, the choice is logged in
            // the §67 line's brake field, and it must not be reported as "what the build did".
            // maxBrakeTorque has no recovered value either, so the same contact cap is reused
            // rather than a new invented constant.
            if (spinDofOn)
                sc->SetMaxFriction(EAx::RotationX, brake * cap);

            // docs §68 (Этап 1, шаг 6): the steering command. A Position motor about the
            // constraint's RotationZ, targeting the live game-logic field m_steerRadians - not an
            // ODE value, and not rate-smoothed here because the prototype's own SteeringSpeed
            // already limits how fast that field moves.
            float steerCS = 0.0f;
            const bool steerable = steerDofOn && ws.steering;
            const uint32_t steerMode = state.VarSteerMode();
            // The steering motor gets its OWN limit, not the contact torque cap. Measured, not
            // assumed: sharing them put the cap at 370-730 Nm while docs §68.2 showed the
            // mechanical stop supplying 212-533 Nm to hold the wheel there - the same order, so
            // the motor lost about a third of the time and the wheels splayed to opposite stops
            // (measured=-0.611 and +0.611 against commanded 0.03). The straight-line baseline fell
            // from ratio 1.01 to 0.85 with steering merely enabled.
            //
            // The disturbance is real physics, not a bug: the steering axis passes through the
            // wheel centre, so zero scrub radius and zero trail, and any wander of the contact
            // patch off the vertical produces R*F about the kingpin - 0.635 m x 800 N is 508 Nm,
            // which is exactly the observed range. A real steering rack is far stiffer than that;
            // a driver's wheel is not knocked to full lock by a bump.
            //
            // The shared-cap reading came from ONE coincidence in the recovered log (cap=3020 and
            // cmdTq=3020 on wheels 0/1 in a single frame) and verification refuted the stronger
            // claim built on it. So it is weak evidence, and the plan's own step-6 instruction is
            // to tune this limit until the wheel "neither jams nor bulldozes". Finite on purpose:
            // a kerb must still eventually win, which FLT_MAX would forbid.
            constexpr float kSteerCapWeightMultiple = 4.0f;   // ~8x the measured 533 Nm disturbance
            const float steerCap = (steerMode >= 1) ? kSteerKinematicCapNm
                                                    : (kSteerCapWeightMultiple * mVeh * gAbs * R);
            if (steerable) {
                // docs §100: the target is the WHEEL's own m_curAngle, not the vehicle-wide
                // m_steerRadians the plan named. ai::Vehicle::_KeepSteer (RVA 0x1da940,
                // vehicle.cpp:5225-5309) computes each wheel's target as
                //   (int)wheel->m_steering * vehicle->m_steerRadians
                // and then rate-limits m_curAngle (Wheel+0x15c) toward it at m_steeringSpeed,
                // calling _TurnWheelByAngle with the DELTA. So m_curAngle is the reference's own
                // per-wheel steer angle with three things already folded in that the raw field
                // does not have: the per-wheel direction (m_steering is a signed multiplier,
                // not a flag - STEERING_INVERSE), the rate limit, and the actual achieved angle
                // rather than the instantaneous command.
                //
                // Mirroring it is also strictly better for a SHADOW: matching the reference's
                // wheel angle is the whole job, and it removes both the sign question and the
                // smoothing question from our side of the comparison.
                const hta::ai::Wheel* hw = (i < state.wheelOrder.size()) ? state.wheelOrder[i] : nullptr;
                // Clamped to the mechanical stop before it is commanded: asking a Position motor
                // for an angle the limits forbid makes it fight the limit constraint every frame,
                // which reads as a jittering wheel rather than as an out-of-range command.
                const float steerCmd = std::clamp(hw != nullptr ? hw->m_curAngle : 0.0f,
                                                  -kWheelSteeringLimitRadians, kWheelSteeringLimitRadians);
                steerCS = ws.steerSign * steerCmd;
                if (steerMode >= 2) {
                    // docs §106: the LITERAL port. ai::Vehicle::_TurnWheelByAngle ends at
                    // PhysicObj::SetRotation - the reference ASSIGNS the wheel's orientation and
                    // never enters a torque contest. That is qualitatively different from a very
                    // stiff motor, and the difference is the whole point: a stiff motor buys a
                    // rigid wheel by reacting the disturbance into the chassis, and §106's sweep
                    // measured exactly what that costs (pos-div 12.4 -> 35.1 m as the spring went
                    // 20 -> 240 Hz, monotone). An assignment reacts nothing.
                    //
                    // The motor must be OFF here or the two mechanisms fight each other.
                    sc->SetMotorState(EAx::RotationZ, JPH::EMotorState::Off);

                    // Keep the SPIN, replace only the steer. In chassis space the wheel's rotation
                    // decomposes as swing*twist with twist about local X - and local X IS the axle
                    // (axleLocal = wheelUp x wheelForward), so the twist is exactly the spin that
                    // step 5 owns and must not be disturbed. Rebuilding from the commanded steer
                    // and the CURRENT twist is what makes this a steer assignment rather than a
                    // full pose override.
                    //
                    // "Current twist" used to mean re-decomposing wb->GetRotation() via
                    // GetSwingTwist() every frame - which sounds inert (spin is only ever read
                    // back, never written here) but is not: swing-twist decomposition cannot
                    // distinguish "the wheel actually spun" from "something disturbed it since
                    // the last assignment" (gyroscopic coupling at high spin rate, the tyre
                    // model's own lateral AddForce on this exact body - wheelmodel_core.hpp).
                    // Whatever disturbance crept into the body's orientation between one
                    // assignment and the next got re-extracted as "twist" and preserved instead
                    // of corrected, so it never washed out - it accumulated. Confirmed live
                    // (kraken.log, real play session): full lock held ~10s while accelerating
                    // 10->20 m/s, measured decayed steadily from commanded and crossed zero to
                    // the opposite sign. wmSpinAngle (see its own comment) is integrated
                    // independently of the body's orientation, so it cannot inherit that
                    // disturbance - only real angular velocity about the axle feeds it.
                    // docs §65/§65.4: a global +X-vs-native's-X sign was tried here (both signs,
                    // live-tested) and REFUTED - the user's precise per-corner report (right-front
                    // and left-rear correct, the other two backwards - a diagonal, not a front/rear
                    // split) rules out any single global or front/rear-keyed correction. The real
                    // cause is a LEFT/RIGHT one (docs §65.4) and is fixed once, uniformly for all
                    // four wheels, at the writeback (ApplyJoltToVehicleWheelModel) - not here. This
                    // twist stays exactly what §106/§68 already built it as: the plain, unflipped
                    // accumulated spin.
                    const JPH::Quat chassisRot = physics->GetBodyInterface().GetRotation(state.bodyId);
                    const JPH::Quat twist = (i < state.wmSpinAngle.size())
                        ? JPH::Quat::sRotation(ws.axleLocal.Normalized(), state.wmSpinAngle[i])
                        : JPH::Quat::sIdentity();
                    // docs §68's own comment (~line 1864, its derivation) already predicted this
                    // exact failure mode for a DIFFERENT reason: "a hard-coded +1 [instead of
                    // ws.steerSign] gives a vehicle that steers the wrong way at every speed and
                    // still passes any test that only checks |angle|" - the motor path (else
                    // branch below, `steerCS = ws.steerSign * steerCmd`) already applies that
                    // correction; this assignment path never did, silently assuming steerAxis
                    // ({0,1,0}, chassis "up") and Jolt's own rotation convention agree with the
                    // reference's sign for m_steerRadians, which they do not. Confirmed live
                    // (user report + verification): wheel angle, chassis rotation and lateral
                    // velocity were ALL internally self-consistent with the unsigned steerCmd
                    // (every automated cross-check this project ran only compares those against
                    // EACH OTHER, all downstream of the same steerCmd - so a whole-system mirror
                    // was invisible to all of them), just mirrored from what the player actually
                    // pressed. ws.steerSign (measured -1, docs/recovered/SPEC.md) is the same
                    // per-wheel-geometry-derived correction the motor path already trusts, not a
                    // new constant - reusing it here instead of a bare literal keeps this correct
                    // for any future prototype whose kingpin geometry measures the other way.
                    const JPH::Quat steerRot = JPH::Quat::sRotation(ws.steerAxis.Normalized(), ws.steerSign * steerCmd);
                    // docs §114 (шаг 6, проверка 2): "упор руля в бордюр не продавливает препятствие
                    // бесконечной силой". An ASSIGNMENT is exactly the mechanism that could shove
                    // geometry through a wall, so the check is more pointed here than for a motor.
                    //
                    // The answer is structural: the wheel is a StaticCompoundShape of two
                    // CONCENTRIC spheres, both at Vec3::sZero() (see BuildWheelBodies), and a
                    // rotationally symmetric shape rotated about its own centre occupies exactly
                    // the space it already occupied. So this call moves no collision geometry at
                    // all and cannot push anything anywhere.
                    //
                    // That is an argument, and this project measures arguments. The world-space
                    // bounds are captured either side of the call: if the claim holds they are
                    // bit-identical, and if a future wheel shape stops being symmetric (the plan's
                    // open sphere-vs-cylinder question) this starts reporting non-zero the day it
                    // happens, instead of the check silently going stale.
                    const JPH::AABox aabbBefore = wb->GetWorldSpaceBounds();
                    physics->GetBodyInterface().SetRotation(
                        state.wheelBodies[i], (chassisRot * steerRot * twist).Normalized(),
                        JPH::EActivation::DontActivate);
                    const JPH::AABox aabbAfter = wb->GetWorldSpaceBounds();
                    const float aabbShift = std::max(
                        (aabbAfter.mMin - aabbBefore.mMin).Abs().ReduceMax(),
                        (aabbAfter.mMax - aabbBefore.mMax).Abs().ReduceMax());
                    ++g_steerAssignments;
                    g_steerCmdMaxSeen = std::max(g_steerCmdMaxSeen, std::fabs(steerCmd));
                    if (aabbShift > g_steerAabbShiftMax) {
                        g_steerAabbShiftMax = aabbShift;
                        g_steerAabbShiftAt  = std::fabs(steerCmd);
                    }
                } else {
                    sc->GetMotorSettings(EAx::RotationZ).SetTorqueLimit(steerCap);
                    sc->SetTargetOrientationCS(JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), steerCS));
                }
            }

            ShadowState::SpinCmd& cmd = state.wmSpinCmd[i];
            cmd.target      = target;
            cmd.limit       = limit;
            cmd.cap         = cap;
            cmd.brakeT      = brake * cap;
            cmd.steerTarget = steerCS;
            cmd.steerCap    = steerable ? steerCap : FLT_MAX;  // FLT_MAX = motor never configured
            cmd.steerable   = steerable;
        }
    }

    // docs §39.2: the APPLY path. Drives the Jolt chassis body with the ported wheelmodel_core
    // forces + an explicit suspension-travel DOF, instead of a JPH::VehicleConstraint (which was
    // NOT added to the simulation for this state - see BuildShadow). Called once per physics step
    // (pre-step) for the player's wheelmodel shadow. Structure mirrors spring_wheel's ODE
    // OnWheelContacts, with Jolt CollideShape providing the manifold and an added suspension DOF
    // (spring_wheel got travel from ODE's Hinge2; here there's no wheel body so it's explicit):
    //   chassis --[soft suspension spring k_susp]-- wheel --[stiff tyre spring k_t]-- ground
    // The chassis feels the SOFT suspension force (normal) + the wheelmodel FRICTION (perp); the
    // stiff tyre force drives the wheel DOF. So ride height/travel is governed by the soft spring
    // (nice travel) while the tyre/Pacejka model still owns grip, obstacles and climb.
    static void StepWheelModel(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label, float dt) {
        namespace wm = kraken::fix::wheelmodel;
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr || state.constraint == nullptr || vehicle == nullptr || dt <= 1e-6f)
            return;

        const kraken::Config& cfg = kraken::Config::Instance();
        const wm::WMParams P = WheelModelParamsFromConfig();
        JPH::BodyInterface& bi = physics->GetBodyInterface();

        const JPH::RMat44 chassisXform = bi.GetWorldTransform(state.bodyId);
        const JPH::Quat   chassisRot   = bi.GetRotation(state.bodyId);
        const JPH::RVec3  comW         = bi.GetCenterOfMassPosition(state.bodyId);
        const JPH::Vec3   vLin         = bi.GetLinearVelocity(state.bodyId);
        const JPH::Vec3   vAng         = bi.GetAngularVelocity(state.bodyId);

        const wm::vec3 UP{0.0f, 1.0f, 0.0f};
        const uint32_t numWheels = std::max<uint32_t>(vehicle->GetNumWheels(), 1);
        const float m       = vehicle->GetMass() / (float) numWheels; // per-corner sprung mass
        const float gAbs    = std::max(std::fabs(kraken::Config::Instance().gravity.value), 0.1f);
        const float maxForce = cfg.jolt_wm_max_g.value * m * gAbs;
        const bool  ownSpin = cfg.jolt_wm_own_spin.value != 0;
        const float reactScale = cfg.jolt_wm_react_scale.value;

        // Drive intent: m_realThrottle, the SAME field the working VehicleConstraint path feeds
        // to Jolt (UpdateShadowInputs) - it's what the game's _KeepThrottle actually populates
        // each frame (raw m_throttle reads 0 at this pre-step point, so the wheels never got
        // drive torque - the vehicle only coasted). It folds braking in as a large negative
        // (throttle - sign(rpm)*brake*10), but that's harmless here: at rest the handbrake locks
        // omega=0 outright, and the separate brake-decay below dominates during service braking.
        const float throttle = std::clamp(vehicle->m_realThrottle, -1.0f, 1.0f);
        const float brake    = std::clamp(vehicle->m_brake, 0.0f, 1.0f);
        const bool  handBrake = vehicle->m_bHandBrake;
        const float steer    = std::clamp(vehicle->m_steerRadians, -kApproxMaxSteerAngleRadians, kApproxMaxSteerAngleRadians);

        // docs §41: real multi-gear drivetrain torque (replaces the old flat constant + linear
        // falloff) - see StepWheelModelGearbox's own comment for the derivation. maxWheelTorque is
        // this frame's available torque at the wheel (already gear/diff/redline-scaled); throttle
        // position scales how much of it is actually produced, same convention the old code used.
        const float maxWheelTorque = StepWheelModelGearbox(vehicle, state, dt);

        const JPH::BroadPhaseLayerFilter& bpFilter   = physics->GetDefaultBroadPhaseLayerFilter(kWheelQueryLayer);
        const JPH::DefaultObjectLayerFilter objFilter = physics->GetDefaultLayerFilter(kWheelQueryLayer);

        // docs §43: per-soil-type friction for wheelmodel's ground contact - same source/formula
        // the OTHER (VehicleConstraint) Jolt path already uses (SetTireMaxImpulseCallback above),
        // computed once per call rather than per wheel (level geometry is constant for the run,
        // same discipline as that path's own tileSize).
        const float tileSize = (float) hta::ai::CServer::Instance()->GetLevelSize()
            / (float) hta::ai::CServer::Instance()->GetWorld()->GetLandscape().GetTileSize();
        const JPH::BodyID roadsBodyId(kraken::fix::jolt::GetRoadsBodyRawId());

        const JPH::Wheels& wheels = state.constraint->GetWheels();
        const size_t nw = std::min(wheels.size(), state.wmSuspLen.size());

        // docs §51: frame-by-frame torque-tracing diagnostic - §50's stated next step before a
        // fourth guessed cap. §50 proved hardStopForce's torque vector (leverArm x upW) has zero
        // world-Y component and reasoned it therefore "cannot cause yaw" - but that only follows
        // if the inverse inertia tensor is diagonal in WORLD space, which is only true while the
        // chassis is perfectly level. Mid-impact the chassis is often rolled/pitched, and the
        // rotated (generally dense) tensor can leak a horizontal torque into yaw angular
        // acceleration through its off-diagonal terms. Rather than assume that away again, measure
        // it directly: accumulate each force source's torque about the chassis COM separately, then
        // run each through the chassis's REAL world-space inverse inertia
        // (MultiplyWorldSpaceInverseInertiaByVector, the same API already proven to compile/work
        // from reverted attempt 1) to see its actual yaw contribution.
        JPH::Vec3 torqueFriction = JPH::Vec3::sZero(); // leverArm x fFriction, ground slot
        JPH::Vec3 torqueVertical = JPH::Vec3::sZero(); // leverArm x (upW*(suspForce+hardStop+chassisDamp))
        JPH::Vec3 torqueHardStop = JPH::Vec3::sZero(); // leverArm x (upW*hardStopForce) - isolated
        JPH::Vec3 torqueObstSide = JPH::Vec3::sZero(); // leverArm x fApply, obstacle+side slots
        bool anyBottomedThisFrame = false;

        for (size_t i = 0; i < nw; ++i) {
            const JPH::Wheel* jwheel = wheels[i];
            const JPH::WheelSettings* s = jwheel ? jwheel->GetSettings() : nullptr;
            if (s == nullptr) continue;
            hta::ai::Wheel* hw = (i < state.wheelOrder.size()) ? state.wheelOrder[i] : nullptr;
            const bool driven   = hw && hw->m_driven;
            const bool steerable = hw && (hw->m_steering != hta::ai::Wheel::STEERING_NO);

            // docs §42.9: real per-wheel-type friction multiplier (WheelPrototypeInfo::m_mU,
            // vehicleparts.xml) - the SAME field the other (VehicleConstraint-based) Jolt path
            // already reads (see SetTireMaxImpulseCallback above) to scale ODE's real
            // mu_from_kappa curve, but wheelmodel's own Pacejka grip (P.mu, "grip" in
            // kraken.ini) was a single flat value shared by every wheel of every vehicle -
            // never varying per wheel type like the real game does. Scale a per-wheel COPY of
            // WMParams rather than touching the shared P (grip stays the base/reference value;
            // m_mU=1.0 for most wheels, so this is a no-op for the common case, matching how
            // the other path already behaves).
            const hta::ai::WheelPrototypeInfo* wheelProto = hw ? hw->GetPrototypeInfo() : nullptr;
            const float wheelMuReal = wheelProto ? wheelProto->m_mU : 1.0f;
            wm::WMParams Pw = P;
            Pw.mu = P.mu * wheelMuReal;

            // docs §43: real per-wheel unsprung mass (hta::ai::Wheel::GetMass(), NATIVE-bound,
            // backed by WheelPrototypeInfo's real per-wheel-type Mass field in vehicleparts.xml -
            // confirmed to vary 1-50kg across wheel types, not one flat guess) replaces the old
            // jolt_wm_unsprung_mass constant (20.0 for every wheel of every vehicle). Fallback
            // literal only for the defensive hw==nullptr case (shouldn't occur - this loop already
            // requires a wheelOrder entry for every wheel it processes).
            const float mUnsprung = std::max(hw ? hw->GetMass() : 20.0f, 0.5f);

            const float R   = s->mRadius;
            const float tau = std::min(cfg.jolt_wm_tyre_thickness.value, R * 0.9f);

            // --- wheel frame (world) ---
            const JPH::RVec3 attach   = chassisXform * s->mPosition;
            const JPH::Vec3  suspDir  = chassisXform.Multiply3x3(s->mSuspensionDirection).Normalized(); // down
            const JPH::RVec3 wheelC   = attach + suspDir * state.wmSuspLen[i];
            const wm::vec3   c{ (float) wheelC.GetX(), (float) wheelC.GetY(), (float) wheelC.GetZ() };

            // axle â for wheelmodel_core's convention: the rolling tangent is t=â×n, which must
            // point in the wheel's FORWARD direction. up x forward gives +X (for +Z fwd/+Y up),
            // and (+X) x (+Y up) = +Z = forward - matching wheelmodel_core's own SelfTest case 3.
            // (Jolt's GetWheelLocalBasis "right" = forward x up = -X gives t=-Z, which drove the
            // vehicle backwards in the first bring-up.)
            JPH::Vec3 localAxle = s->mWheelUp.Cross(s->mWheelForward).Normalized();
            if (steerable && std::fabs(steer) > 1e-4f)
                localAxle = JPH::Quat::sRotation(s->mSteeringAxis, steer) * localAxle;
            const JPH::Vec3 wAxleV = (chassisRot * localAxle).Normalized();
            const wm::vec3  a{ wAxleV.GetX(), wAxleV.GetY(), wAxleV.GetZ() };

            // --- spin DOF (§5). own_spin: throttle drive torque - friction reaction, torque-limited ---
            float& omega = state.wmOmega[i];

            // --- manifold: wheel sphere vs static world ---
            JPH::SphereShape sphere(R);
            sphere.SetEmbedded();
            JPH::CollideShapeSettings csSettings;
            csSettings.mMaxSeparationDistance = tau;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            ++g_legacyPaths.stepCollideShape;   // docs §110
            physics->GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f),
                JPH::RMat44::sTranslation(wheelC), csSettings, JPH::RVec3::sZero(), collector, bpFilter, objFilter);

            constexpr int kMaxC = 16;
            wm::WMContact cts[kMaxC];
            wm::WMGeom    gm[kMaxC];
            JPH::BodyID   hitBody[kMaxC];
            const int n = std::min((int) collector.mHits.size(), kMaxC);
            for (int h = 0; h < n; ++h) {
                const JPH::CollideShapeResult& r = collector.mHits[(size_t) h];
                const JPH::Vec3 nrm = (-r.mPenetrationAxis).Normalized();
                cts[h].p = { (float) r.mContactPointOn2.GetX(), (float) r.mContactPointOn2.GetY(), (float) r.mContactPointOn2.GetZ() };
                cts[h].n = { nrm.GetX(), nrm.GetY(), nrm.GetZ() };
                // docs §44: cap raw penetration at the wheel's own radius - found via the
                // bottomed-out diagnostic that when the suspension DOF is pinned at compMax and
                // the bounded ground reaction (maxForce, ~6g) isn't enough to arrest the chassis's
                // fall, the chassis (and the wheel rigidly following it) keeps sinking every frame,
                // so this raycast/CollideShape penetration - freshly re-measured each frame at the
                // wheel's current, ever-deeper world position - grows without bound (measured:
                // preClampForce climbed from ~3.5kN to ~138kN over 29 consecutive frames while comp
                // stayed pinned at compMax the whole time). k_c (the tyre's hard-core spring,
                // lambda*k_t = 2.4M N/m for this vehicle) then turns that unbounded depth into an
                // unbounded normal force, which the friction-circle clamp scales friction to match -
                // so friction grows unbounded too, right along with it. A sphere penetrating deeper
                // than its own radius into a one-sided surface is already a degenerate reading (the
                // contact point/normal from CollideShape stop being physically meaningful well
                // before that), so clamping here bounds the force at its actual source instead of
                // just re-capping the symptom downstream.
                cts[h].depth = std::min(r.mPenetrationDepth, R);
                hitBody[h] = r.mBodyID2;
                gm[h] = wm::ComputeGeom(cts[h], c, UP, a, R, R);
            }
            wm::WMSlots slots = wm::Classify(gm, n);

            // docs §43: soil-friction scaling only for the GROUND slot (the tyre's actual rolling
            // surface) - obstacle/side slots (wall faces, steps) keep the wheel-type-only Pw, same
            // reasoning the base grip/mU tuning already covers those. Same "not the roads body"
            // rule as the other Jolt path (SetTireMaxImpulseCallback above): roads stay at the tire
            // model's own mu (friction=1.0 equivalent), anything else gets the real terrain-tile
            // lookup, regardless of what's actually under the contact (matches that path's existing
            // precedent, which doesn't distinguish terrain from rock/obstacle either).
            wm::WMParams PwGround = Pw;
            float soilFrictionReal = 1.0f;
            if (slots.ground >= 0 && hitBody[slots.ground] != roadsBodyId) {
                const int32_t soilX = (int32_t) (cts[slots.ground].p.x / tileSize + 0.5f);
                const int32_t soilZ = (int32_t) (cts[slots.ground].p.z / tileSize + 0.5f);
                const hta::ai::DynamicScene::SoilProps& props =
                    hta::ai::DynamicScene::Instance()->GetSoilProps((uint32_t) soilX, (uint32_t) soilZ);
                soilFrictionReal = props.m_friction;
                PwGround.mu *= soilFrictionReal;
            }

            auto vpAt = [&](const wm::vec3& p) {
                const JPH::Vec3 rr(p.x - (float) comW.GetX(), p.y - (float) comW.GetY(), p.z - (float) comW.GetZ());
                const JPH::Vec3 v = vLin + vAng.Cross(rr);
                return wm::vec3{ v.GetX(), v.GetY(), v.GetZ() };
            };
            auto finite3 = [](const wm::vec3& v) { return v.x==v.x && v.y==v.y && v.z==v.z && wm::Len(v) < 1e18f; };

            // spin: integrate BEFORE the force eval so friction sees this frame's omega.
            // Drive torque spins up; brake decays toward 0 (never reverses); clamp to a sane
            // range so a transient can't run it away (the first bring-up hit omega=817 from a
            // sign bug here). Friction reaction is added AFTER the force eval below.
            constexpr float kMaxOmega = 250.0f; // rad/s; ~200 m/s at R=0.8, generously above any real wheel
            // handbrake (or a fully-locked service brake) pins the wheel: omega=0 so the tyre
            // patch is fully sliding vs the ground = maximum Pacejka resisting friction, which is
            // what actually holds a parked car on a slope. Without this the wheel rolls free and
            // the vehicle coasts downhill even "parked" (the first bring-up slid 8m at rest).
            if (handBrake) {
                omega = 0.0f;
            } else if (ownSpin) {
                const float I = std::max(P.inertia, 1e-3f);
                // docs §41: real gear/diff/redline-derived torque (maxWheelTorque, computed once
                // per vehicle per step above) instead of docs §40's flat constant + linear
                // falloff - see StepWheelModelGearbox. kMaxOmega above stays as a backstop only;
                // the gearbox's own rev-limiter is what actually governs top speed now.
                const float tauDrive = driven ? throttle * maxWheelTorque : 0.0f;
                omega += tauDrive / I * dt;
                if (brake > 0.0f)
                    omega -= omega * std::min(1.0f, brake * 4.0f * dt); // brake as decay toward 0
                omega = std::clamp(omega, -kMaxOmega, kMaxOmega);
            }

            wm::WMForce fG = (slots.ground   >= 0) ? wm::GeneralizedContactForce(cts[slots.ground].p,   cts[slots.ground].n,   gm[slots.ground].pen,   gm[slots.ground].wr, c, a, vpAt(cts[slots.ground].p),   omega, R, tau, m, dt, PwGround) : wm::WMForce();
            wm::WMForce fO = (slots.obstacle >= 0) ? wm::GeneralizedContactForce(cts[slots.obstacle].p, cts[slots.obstacle].n, gm[slots.obstacle].pen, gm[slots.obstacle].wr, c, a, vpAt(cts[slots.obstacle].p), omega, R, tau, m, dt, Pw) : wm::WMForce();
            wm::WMForce fS = (slots.side     >= 0) ? wm::GeneralizedContactForce(cts[slots.side].p,     cts[slots.side].n,     gm[slots.side].pen,     gm[slots.side].wl, c, a, vpAt(cts[slots.side].p),     omega, R, tau, m, dt, Pw) : wm::WMForce();

            // --- suspension travel DOF: the ground tyre force (stiff) drives the wheel; a soft
            // spring transmits to the chassis, so ride height is governed by k_susp (travel). ---
            // docs §42.5: kSusp/cSusp now read PER-WHEEL from s->mSuspensionSpring - the REAL
            // ODE CFM/ERP-derived stiffness/damping (docs §31's derivation, computed earlier in
            // BuildShadow for the VehicleConstraint that's built-but-not-simulated in wheelmodel
            // mode - see the big comment there) - instead of one flat jolt_wm_susp_stiffness/
            // damping constant shared by all 4 wheels. Found while chasing a real (if less
            // dramatic than first thought - see docs §42.5) difference from ODE: Jolt's own
            // absolute chassis pitch swung -24..+53deg over rough terrain vs real ODE's 6-40deg
            // over the same window - noticeably more violent, never previously explained. An
            // arbitrary one-size-fits-all spring can't reproduce how a real vehicle's front/rear
            // (often differently-tuned) suspensions jointly damp pitch impulses from bumps; the
            // per-wheel real data can, since it already varies wheel-to-wheel same as real ODE.
            const float kSusp = s->mSuspensionSpring.mStiffness;
            const float cSusp = s->mSuspensionSpring.mDamping;
            const wm::vec3 downW{ suspDir.GetX(), suspDir.GetY(), suspDir.GetZ() };
            const float normalLoad = std::max(0.0f, -wm::Dot(fG.F, downW)); // tyre force component pushing the wheel UP
            // Compression measured from the per-wheel REST length (raycast-init, wheel on ground),
            // NOT from full droop - so comp=0 at spawn (no launch) and settles to the tiny static
            // value weight/kSusp under load. comp<0 = drooped (wheel reaching below rest, airborne).
            const float restLen = state.wmRestLen[i];
            const float maxLen  = s->mSuspensionMaxLength;
            // docs §43: real per-wheel compression-travel budget (see the doc for the two failed
            // attempts before this formula - restLen-mSuspensionMinLength pinned at a 0.01 floor;
            // (1-restFraction)*range let the linear spring blow up on a bump).
            const float suspensionRangeReal = std::max(maxLen - s->mSuspensionMinLength, 0.01f);
            const float restFraction = std::clamp(cfg.jolt_susp_rest_fraction.value, 0.02f, 0.4f);
            const float compMax = std::clamp(5.0f * restFraction * suspensionRangeReal, 0.02f, suspensionRangeReal); // most-compressed

            float comp    = restLen - state.wmSuspLen[i];
            float compVel = state.wmSuspVel[i];

            // docs §45: a progressive (non-linear) spring stiffening near full compression was
            // tried here and REVERTED - kProgressiveStart=0.6/kProgressiveMult=4x gave no
            // measurable pitch improvement (absolute-tilt ratio stayed ~1.4x ODE, same as the
            // plain linear spring) while making the forward-travel ratio noticeably worse and
            // noisier (5 confirmed-vehicle repeats: 0.16-1.17, avg 0.75, vs the linear spring's
            // 0.72-1.13, avg 0.925) - a clear regression with no compensating benefit, so reverted
            // rather than kept on the theory it might help. A real fix for the remaining pitch gap
            // would need a properly re-tuned progressive curve (different start point/multiplier,
            // possibly per-vehicle) verified to actually help before adopting - left open rather
            // than shipped on a hypothesis that didn't pan out when tested.
            // chassis support: only the compressed spring pushes up (a drooped strut just hangs)
            const float suspForce = std::max(0.0f, kSusp * std::max(comp, 0.0f) + cSusp * compVel);
            // Wheel (unsprung) DOF, semi-implicit Euler with IMPLICIT damping. Explicit damping
            // (the old `accel = (normalLoad - (kSusp*comp + cSusp*compVel))/mUnsprung; compVel +=
            // accel*dt`) is only conditionally stable - it needs cSusp < 2*mUnsprung/dt (= 4000
            // N.s/m at mUnsprung=20, dt=0.01); past that the damper term flips sign and grows
            // ~|1-cSusp/mUnsprung*dt|x each step. Live at cSusp=5000 this blew up as suspF
            // oscillating 0<->366kN with comp swinging -1.0<->+0.35 and flung the body 175m (docs
            // §40). Implicit damping is unconditionally stable for any cSusp, so the damper can be
            // tuned freely: v_new = (v + dt*(normalLoad - kSusp*comp)/m) / (1 + dt*cSusp/m).
            const float springAccel = (normalLoad - kSusp * comp) / mUnsprung;
            compVel = (compVel + springAccel * dt) / (1.0f + (cSusp / mUnsprung) * dt);
            comp   += compVel * dt; // symplectic: integrate position with the just-updated velocity
            const float compMin = restLen - maxLen; // most-drooped (negative)
            if (comp < compMin) { comp = compMin; if (compVel < 0.0f) compVel = 0.0f; }
            const bool bottomedOut = comp >= compMax;
            // docs §44: a real mechanical bump-stop delivers a reaction PROPORTIONAL to how much
            // velocity it needs to remove (harder hit -> bigger, brief stopping force) - simply
            // zeroing compVel here (the old behaviour) discards that velocity with nowhere to go,
            // leaving suspForce (computed above, before this clamp) at a FIXED kSusp*compMax
            // regardless of impact speed. Any leftover momentum then keeps sinking the chassis
            // into rigid ground frame after frame (wheelC is rigidly attached to the chassis once
            // wmSuspLen stops changing), which re-measures as ever-growing raw penetration on the
            // TIRE's own separate spring - confirmed live via the bottomed-out diagnostic
            // (preClampFriction climbing from ~3.5kN to 680kN+ over a dozen-odd frames while comp/
            // suspForce sat constant). That tire spring's hard-core term was never meant to
            // substitute for the suspension's own hard stop. Fixed at the actual source: convert
            // the excess velocity into an explicit one-frame stopping force (F=m*dv/dt, the
            // standard inelastic-impulse formula) added to this frame's ground force, so the
            // chassis actually decelerates proportionally to the hit instead of the tire spring
            // having to compensate for an undersized, constant suspension reaction.
            // docs §44: track how many CONSECUTIVE frames this wheel has been pinned at compMax -
            // testing the hypothesis that a hard-bump launch isn't a single-frame spike (maxForce
            // already clamps those) but a SUSTAINED near-maxForce push across many frames while the
            // impact keeps demanding more compression than compMax allows, which the per-frame
            // clamp can't see (it only bounds each frame's force, not the cumulative impulse of many
            // frames at that bound in a row). wmBottomedFrames is always sized to match wmSuspLen
            // (same nw at BuildShadow time), and i is already bounded by nw above - guard anyway
            // rather than assume. Computed BEFORE the impulse decision below so that decision can
            // tell a single-frame graze from a genuinely sustained bottom-out.
            const bool haveBottomedCounter = i < state.wmBottomedFrames.size();
            if (haveBottomedCounter)
                state.wmBottomedFrames[i] = bottomedOut ? (state.wmBottomedFrames[i] + 1) : 0;
            const int32_t bottomedFrames = haveBottomedCounter ? state.wmBottomedFrames[i] : 0;
            if (bottomedFrames > 0) anyBottomedThisFrame = true; // docs §51: gates the post-loop torque-trace log

            float hardStopForce = 0.0f;
            if (bottomedOut) {
                comp = compMax;
                if (compVel > 0.0f) {
                    // Escalate to a full impulse response only from the SECOND consecutive
                    // bottomed frame onward - a single-frame graze (the common case on ordinary
                    // bumps, confirmed live to make up the bulk of bottomed-out events) recovers
                    // fine with the old gentle zero-velocity behaviour; it's specifically the
                    // sustained, many-frame case (the runaway-sinking mechanism this section
                    // fixed, which by definition keeps recurring for several frames) that needs a
                    // real stopping force. First live test of the unconditional version prevented
                    // every launch but measurably slowed ordinary forward travel (ratio settled
                    // ~0.4-0.5 over 5 repeats vs the established 1.12-1.20 baseline) - applying it
                    // on literally every single-frame bottom-out was fighting normal bumps, not
                    // just the pathological case.
                    if (bottomedFrames >= 2) {
                        // docs §48: found testing a second, much lighter vehicle (Bug01, ~132kg
                        // vs Molokovoz's 167kg but with a proportionally different real kSusp/
                        // cSusp/mUnsprung combination) that this impulse formula, left completely
                        // uncapped, can reach genuinely pathological values - live-caught at
                        // 87,498N (~45x that vehicle's own maxForce of 1942N) on a single wheel,
                        // spinning the chassis through roughly 90+ degrees of yaw in about 3
                        // seconds (fwd.x swept from -0.02 to 1.00) from one asymmetric one-wheel
                        // impulse - not a rollover (fwd.y, the pitch component, stayed under 0.42
                        // throughout) but still a clearly excessive, real-vehicle-implausible
                        // kick. maxForce itself can't be reused as the cap here (that was already
                        // shown in §44.1 to be too small to arrest a genuine hard impact - the
                        // whole reason this impulse term exists uncapped in the first place), so
                        // bound it at a generous multiple instead: enough headroom for any
                        // realistic impact (10x maxForce is already a ~60g-equivalent ceiling,
                        // vs maxForce's own 6g) while catching the actual runaway case.
                        //
                        // docs §50: tried rebasing this on chassis effective-mass-at-lever-arm
                        // (translation+rotation combined) instead of mUnsprung, reasoning Bug01's
                        // yaw-spin problem was about how little this LIGHT chassis resists torque
                        // at its wheels - REVERTED after live-testing showed no improvement (still
                        // ~163deg). Root cause on reflection: hardStopForce is applied PURELY along
                        // upW (vertical) - torqueAxis = leverArm x upDir always lies in the
                        // horizontal plane (a vertical force's moment arm can only ever produce a
                        // horizontal torque axis, i.e. pitch/roll), so this force literally cannot
                        // cause YAW at all, regardless of how its magnitude is capped. Bug01's
                        // observed spin (fwd.x sweeping through 90+ degrees = rotation about the
                        // VERTICAL axis) was never something this term could have caused - the real
                        // culprit is the horizontal friction force below, still capped by the flat,
                        // mass-only maxForce untouched since §44.1. See that fix instead.
                        constexpr float kHardStopMaxForceMult = 10.0f;
                        const float magnitudeCapped = std::min(mUnsprung * compVel / dt, maxForce * kHardStopMaxForceMult);

                        // docs §49: the magnitude cap alone wasn't enough - found on Belaz01 (a
                        // much heavier vehicle) that normalLoad can stay persistently high frame
                        // after frame (the tire's own contact force not resolving, e.g. the wheel
                        // still jammed into whatever it hit), so compVel gets re-accelerated to a
                        // large value EVERY frame even though hardStopForce zeroes it each time -
                        // 17 consecutive frames of a magnitude-capped-but-still-large force still
                        // added up to enough total impulse to launch the vehicle airborne (confirmed
                        // via all-wheels-off-ground + wild orientation swings - genuine tumbling,
                        // per §42.8, not a grounded rollover, but still an undesired launch). Bound
                        // the TOTAL impulse one bottoming-out streak can deliver, not just each
                        // frame's peak: budget it against a generous (15 m/s) impact velocity
                        // change - once that's spent, further frames within the SAME streak get
                        // zero extra force (suspForce/friction/the tire's own contact force still
                        // apply normally; only this supplemental impulse term is exhausted) rather
                        // than continuing to add energy to an event that should already have been
                        // arrested several frames ago.
                        constexpr float kMaxImpactVelocity = 5.0f; // m/s - see docs §49: first tried 15 (live-tested too generous - a heavy
                            // vehicle's whole initial burst, BEFORE the budget ran out, was still enough to launch it airborne)
                        const float impulseBudget = mUnsprung * kMaxImpactVelocity;
                        const bool haveImpulseTracker = i < state.wmBottomedImpulse.size();
                        const float impulseSoFar = haveImpulseTracker ? state.wmBottomedImpulse[i] : 0.0f;
                        const float remainingBudget = std::max(0.0f, impulseBudget - impulseSoFar);
                        const float wantedImpulse = magnitudeCapped * dt;
                        hardStopForce = (wantedImpulse <= remainingBudget) ? magnitudeCapped : remainingBudget / dt;
                        if (haveImpulseTracker)
                            state.wmBottomedImpulse[i] = impulseSoFar + hardStopForce * dt;
                    }
                    compVel = 0.0f;
                }
            } else if (i < state.wmBottomedImpulse.size()) {
                state.wmBottomedImpulse[i] = 0.0f; // streak ended - reset the budget for the next one
            }
            state.wmSuspVel[i] = compVel;
            state.wmSuspLen[i] = restLen - comp;

            // spin reaction from ground friction (couples traction<->spin) for next step
            if (ownSpin && !handBrake && reactScale > 0.0f && slots.ground >= 0) {
                const wm::vec3 nG = cts[slots.ground].n;
                const wm::vec3 Ft = fG.F - nG * wm::Dot(fG.F, nG); // friction (tangential) part
                const wm::vec3 rC = cts[slots.ground].p - c;
                const float tSpin = wm::Dot(wm::Cross(rC, Ft), a) * reactScale;
                const float I = std::max(P.inertia, 1e-3f);
                omega += tSpin / I * dt; // τ_react
                omega = std::clamp(omega, -kMaxOmega, kMaxOmega);
            }

            // --- apply to chassis ---
            const JPH::Vec3 upW = -suspDir;
            // ground: soft suspension normal (up) + wheelmodel friction (perp to suspension axis)
            float groundForcePreClampMag = 0.0f; // docs §44: pre-clamp magnitude, for the bottomed-out diagnostic below
            if (slots.ground >= 0 && finite3(fG.F)) {
                // docs §44: suspForce (the real per-wheel kSusp/cSusp spring reaction) and the
                // tyre's friction/tangential force are now capped SEPARATELY, not bundled into one
                // vector before a single maxForce clamp. Found via the bottomed-out diagnostic that
                // when friction grew large (from the now-also-fixed unbounded-penetration issue
                // above), it could dominate the COMBINED vector's direction - since maxForce then
                // scales the whole blended vector down to one fixed magnitude, a friction-dominated
                // blend could leave almost no effective UPWARD support even though suspForce itself
                // (a finite, real, physically-derived value) was perfectly reasonable on its own -
                // exactly backwards from what a hard bottom-out needs (maximum vertical support).
                // suspForce is already bounded by real data (kSusp*compMax at worst) so it doesn't
                // need an extra artificial ceiling; only the friction/tangential part - which can
                // still be large from a hard sideways slip even after the penetration cap - keeps
                // the maxForce safety clamp.
                const wm::vec3 Fperp = fG.F - downW * wm::Dot(fG.F, downW); // remove along-suspension part
                JPH::Vec3 fFriction(Fperp.x, Fperp.y, Fperp.z);
                const float ffl = fFriction.Length();
                groundForcePreClampMag = ffl;
                // docs §50: three attempts to fix Bug01's yaw-spin problem by tightening this cap
                // were all tried live and reverted - (1) rebasing hardStopForce on chassis
                // effective mass at the wheel's lever arm: WRONG TARGET, proven analytically that
                // a purely-vertical force's torque axis is confined to the horizontal plane and
                // can never cause yaw regardless of magnitude; (2) the same effective-mass idea
                // applied to friction's own direction instead (which CAN cause yaw): a real,
                // sound mechanism, but a direct diagnostic showed chassisEffMassAtContact was
                // ALWAYS larger than the flat per-corner mass for this vehicle, so it never
                // actually tightened anything - a confirmed no-op; (3) sharing hardStopForce's
                // cumulative-impulse budget (§49) with friction too, reasoning friction sustained
                // over many bottomed frames could accumulate yaw impulse the same way §49 found
                // for pitch: caused a real, live-confirmed REGRESSION on Belaz01 (angle back up to
                // 130-167deg, from the established 47.8-108.8deg §49 baseline) - splitting the
                // budget between two force components starved hardStopForce of what it needed for
                // the mechanism §49 was actually fixing. Reverted to the plain §44.1 cap. Bug01's
                // yaw-spin root cause remains genuinely unidentified after three reasoned attempts -
                // needs a frame-by-frame torque-tracing investigation (separating friction's,
                // hardStopForce's, and the spin-reaction tSpin's individual contributions to
                // angular velocity) before a fourth attempt, not another guessed cap.
                if (ffl > maxForce && ffl > 1e-3f) fFriction = fFriction * (maxForce / ffl);
                // docs §46: chassis-heave damping - the missing HALF of the suspension damper.
                // The real damper opposes the RELATIVE velocity between chassis and wheel along the
                // strut; suspForce's own cSusp*compVel term only captures the WHEEL side (compVel,
                // integrated from tire load). The CHASSIS's own velocity at this corner reaches
                // compVel only indirectly, lagged 1-2 frames through the tire-spring feedback loop -
                // which under-damps the chassis pitch mode specifically vs real ODE's Hinge2 damper
                // that acts on the true relative velocity directly (docs §42.5-45 left this gap at
                // ~1.4x ODE's pitch range). Add the chassis-side term: resist the chassis's own
                // downward velocity at the contact. Front dives + rear rises -> this resists both ->
                // damps pitch. Clamped to maxForce so a hard-landing spike can't dominate (the §44
                // hardStopForce impulse already handles those); the main suspForce/hardStop stay
                // uncapped per §44's reasoning.
                const float vChassisDown = wm::Dot(vpAt(cts[slots.ground].p), downW); // + = compressing
                const float chassisDamp = std::clamp(cSusp * vChassisDown, -maxForce, maxForce);
                // docs §51: hardStopForce applied along WORLD-up rather than the wheel's local
                // (chassis-tilted) upW. Found via the torque-tracing diagnostic below that
                // hardStopForce leaks real yaw torque once the chassis is significantly rolled/
                // pitched - live-measured up to tiltDeg=47-50 during Bug01's yaw-spin event, at
                // which point hardStopForce's own torqueY reached -15890 (dwarfing friction's
                // -3177 at that same instant). §50's "a vertical force's torque is horizontal-only,
                // so it can't cause yaw" proof implicitly assumed upW equals world Y exactly - only
                // true while the chassis is level. suspForce+chassisDamp are a REAL strut-aligned
                // spring/damper and must stay along upW even when tilted (that's how a real
                // suspension behaves), but hardStopForce is a synthetic numerical patch (docs §44's
                // inelastic-impulse stand-in for "arrest the chassis's WORLD-FRAME downward
                // velocity"), not a physical spring - nothing requires it to be strut-aligned.
                // Pinning it to world-up closes the yaw leak at its source (leverArm x worldUp has
                // zero Y-component by construction) instead of re-capping the symptom, which is what
                // all three of §50's reverted attempts tried instead. Verified: hardStop's own
                // torqueY is now a structural 0 in every frame. 1 of 3 Bug01 repeats improved a lot
                // (160-179deg -> 80.4deg); the other 2 stayed bad (172.7/170.7deg) because friction
                // and obstacle/side forces leak yaw through the SAME tilted-frame mechanism and
                // aren't touched by this fix - so Bug01's yaw-spin is NOT fully solved, only
                // partially. First live pass looked like a Molokovoz01 regression (two runs showed
                // max-angle 152-165deg vs a remembered 8.6-14.9deg baseline) - investigated by
                // rebuilding the untouched pre-§51 commit and testing it cold: THAT baseline itself
                // produced max-angle 151.3/161.4/166.7deg across 3 repeats, i.e. this scripted
                // scenario's "angle" metric (yaw-conflated, see docs §47's own caveat) has much
                // wider natural run-to-run variance than the old remembered figures reflected. With
                // that corrected baseline, this fix's numbers (157.1/165.7deg, ratio 1.23/0.92, both
                // healthy) are statistically indistinguishable from unmodified code - no regression.
                // Lesson: don't trust 1-2 samples of this angle metric against a remembered range;
                // re-baseline cold before attributing a noisy metric's swing to a code change.
                const float suspAndDampMag = std::max(0.0f, suspForce + chassisDamp);
                const JPH::Vec3 kWorldUp(0.0f, 1.0f, 0.0f);
                const JPH::Vec3 fApply = upW * suspAndDampMag + kWorldUp * hardStopForce + fFriction;
                const JPH::RVec3 at(cts[slots.ground].p.x, cts[slots.ground].p.y, cts[slots.ground].p.z);
                bi.AddForce(state.bodyId, fApply, at);

                // docs §51: split this wheel's torque contribution by source for the post-loop
                // inverse-inertia diagnostic below.
                if (vehicle->m_bIsControlledByPlayer) {
                    const wm::vec3& gp = cts[slots.ground].p;
                    const JPH::Vec3 lever(gp.x - (float) comW.GetX(), gp.y - (float) comW.GetY(), gp.z - (float) comW.GetZ());
                    torqueFriction += lever.Cross(fFriction);
                    torqueVertical += lever.Cross(upW * suspAndDampMag + kWorldUp * hardStopForce);
                    torqueHardStop += lever.Cross(kWorldUp * hardStopForce); // structurally 0 by construction - confirms the fix
                }
            }
            // docs §44: unthrottled (not the %30 sampled diagnostic below, which would likely MISS
            // a transient few-frame bump event entirely) - only fires while a wheel is actually
            // pinned at compMax, testing whether a launch is a SUSTAINED many-frame near-maxForce
            // push rather than a single clamped spike (which maxForce already bounds fine on its
            // own, one frame at a time).
            if (bottomedFrames > 0 && vehicle->m_bIsControlledByPlayer) {
                LOG_INFO("docs §44: wm bottomed-out (%s) w=%zu frames=%d preClampFriction=%.0f maxForce=%.0f suspForce=%.0f hardStop=%.0f comp=%.3f compMax=%.3f pen=%.3f",
                    label, i, bottomedFrames, (double) groundForcePreClampMag, (double) maxForce, (double) suspForce, (double) hardStopForce,
                    (double) comp, (double) compMax, (double)(slots.ground >= 0 ? gm[slots.ground].pen : 0.0f));
            }
            // obstacle & side: apply the full wheelmodel force (climb/wall bracing) directly
            auto applyDirect = [&](int idx, const wm::WMForce& fr) {
                if (idx < 0 || !finite3(fr.F)) return;
                JPH::Vec3 fApply(fr.F.x, fr.F.y, fr.F.z);
                const float fl = fApply.Length();
                if (fl > maxForce && fl > 1e-3f) fApply = fApply * (maxForce / fl);
                const JPH::RVec3 at(cts[idx].p.x, cts[idx].p.y, cts[idx].p.z);
                bi.AddForce(state.bodyId, fApply, at);
                // docs §51: obstacle/side forces weren't previously suspected for Bug01's yaw-spin
                // (the event was found via the ground-only §44 bottomed-out log), but measure them
                // too rather than assume - a wall/curb-clip kick is exactly the kind of asymmetric,
                // single-wheel impulse that could independently cause yaw.
                if (vehicle->m_bIsControlledByPlayer) {
                    const JPH::Vec3 lever(cts[idx].p.x - (float) comW.GetX(), cts[idx].p.y - (float) comW.GetY(), cts[idx].p.z - (float) comW.GetZ());
                    torqueObstSide += lever.Cross(fApply);
                }
            };
            applyDirect(slots.obstacle, fO);
            applyDirect(slots.side, fS);

            if ((state.frameCounter % 30) == 0 && vehicle->m_bIsControlledByPlayer) {
                // diagnostic: how far is real ground straight below the wheel centre? tells us
                // whether the wheel sphere is above the terrain (chassis resting on its own shape)
                // or buried. Also log wheelC.Y and chassis COM Y.
                JPH::RRayCast downRay{ wheelC, JPH::Vec3(0, -20.0f, 0) };
                JPH::RayCastResult hit;
                const bool gHit = physics->GetNarrowPhaseQuery().CastRay(downRay, hit);
                const float groundBelow = gHit ? 20.0f * hit.mFraction : -1.0f;
                const JPH::Vec3 fwdW = chassisRot * JPH::Vec3(0, 0, 1); // vehicle forward in world
                LOG_INFO("docs §43: wm apply (%s) w=%zu drv=%d n=%d gSlot=%d pen=%.3f comp=%.3f suspF=%.0f omega=%.1f thr=%.2f fwd=(%.2f,%.2f,%.2f) Fg=(%.0f,%.0f,%.0f) fpar=%.0f gear=%d rpm=%.0f maxTq=%.0f muReal=%.3f compMax=%.3f mUnsprung=%.1f soilMu=%.3f",
                    label, i, driven?1:0, n, slots.ground, (double)(slots.ground>=0?gm[slots.ground].pen:0.0f), (double) comp, (double) suspForce,
                    (double) omega, (double) throttle, (double) fwdW.GetX(), (double) fwdW.GetY(), (double) fwdW.GetZ(),
                    (double) fG.F.x, (double) fG.F.y, (double) fG.F.z, (double) fG.fpar_w,
                    state.wmGear, (double) state.wmEngineRpm, (double) maxWheelTorque, (double) wheelMuReal,
                    (double) compMax, (double) mUnsprung, (double) soilFrictionReal);
            }
        }

        // docs §51: log the actual per-source yaw angular-ACCELERATION each force would produce
        // through the chassis's real (possibly rotated, possibly non-diagonal) inverse inertia
        // tensor - not just each torque vector's raw direction. Unthrottled (fires every frame of
        // a bottomed-out event, same reasoning as the existing §44 log: these events are rare and
        // brief enough that a %30 sample could miss them entirely).
        if (anyBottomedThisFrame && vehicle->m_bIsControlledByPlayer) {
            JPH::Body* chassisBody = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.bodyId);
            if (chassisBody != nullptr) {
                const JPH::MotionProperties* mp = chassisBody->GetMotionProperties();
                const JPH::Vec3 alphaFriction = mp->MultiplyWorldSpaceInverseInertiaByVector(chassisRot, torqueFriction);
                const JPH::Vec3 alphaVertical = mp->MultiplyWorldSpaceInverseInertiaByVector(chassisRot, torqueVertical);
                const JPH::Vec3 alphaHardStop = mp->MultiplyWorldSpaceInverseInertiaByVector(chassisRot, torqueHardStop);
                const JPH::Vec3 alphaObstSide = mp->MultiplyWorldSpaceInverseInertiaByVector(chassisRot, torqueObstSide);
                // docs §51: direct confirmation of the "upW isn't world-Y once tilted" hypothesis -
                // §50's proof that a vertical force's torque axis is horizontal-only implicitly
                // assumed upW (the wheel's LOCAL suspension-up, chassisXform-rotated) equals world
                // Y exactly, which is only true while the chassis is perfectly level. Log the actual
                // tilt so a correlation between tilt magnitude and hardStop/vert leaking into yaw can
                // be checked directly instead of inferred.
                const JPH::Vec3 chassisUp = chassisRot * JPH::Vec3(0.0f, 1.0f, 0.0f);
                const float tiltDeg = JPH::RadiansToDegrees(std::acos(std::clamp(chassisUp.GetY(), -1.0f, 1.0f)));
                LOG_INFO("docs §51 torque-trace (%s): alphaY fric=%.3f vert=%.3f hardStop=%.3f obstSide=%.3f | torqueY fric=%.0f vert=%.0f hardStop=%.0f obstSide=%.0f | vAngY=%.3f tiltDeg=%.1f",
                    label, (double) alphaFriction.GetY(), (double) alphaVertical.GetY(), (double) alphaHardStop.GetY(), (double) alphaObstSide.GetY(),
                    (double) torqueFriction.GetY(), (double) torqueVertical.GetY(), (double) torqueHardStop.GetY(), (double) torqueObstSide.GetY(),
                    (double) vAng.GetY(), (double) tiltDeg);
            }
        }
    }

    static void LogWheelState(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        if (state.constraint == nullptr)
            return;

        // docs §39.1: read-only wheelmodel_core evaluation on the live Jolt world (see its
        // comment). Only in log-only mode (jolt_wheelmodel==1); in apply mode (==2) StepWheelModel
        // does its own §39.2 logging and there's no live VehicleConstraint pose to read here.
        if (kraken::Config::Instance().jolt_wheelmodel.value == 1)
            LogWheelModelEval(vehicle, state, label);

        // docs §23.6: the exact same fields UpdateShadowInputs feeds into SetDriverInput -
        // logged once here so a frozen-wheel report can be checked against real input state
        // (e.g. handbrake stuck on) instead of guessed at.
        LOG_INFO("docs §23.6: driver input (%s) realThrottle=%.2f brake=%.2f handBrake=%d steerRad=%.3f",
            label, (double) vehicle->m_realThrottle, (double) vehicle->m_brake,
            vehicle->m_bHandBrake ? 1 : 0, (double) vehicle->m_steerRadians);

        // docs §32: does Jolt's OWN shadow chassis settle to the same pitch as real ODE, or
        // does it tip further (e.g. because the compound chassis shape is missing collision
        // geometry - like an undercarriage/frame - that would arrest a real vehicle's tip
        // earlier)? Same forward-vector-Y-component measure LogRealOdeWheelState already logs
        // for the real ODE side (docs §30), computed here for Jolt's shadow body instead, same
        // frame/cadence, directly comparable line by line.
        {
            JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
            if (physics != nullptr) {
                JPH::BodyInterface& bi = physics->GetBodyInterface();
                const JPH::RVec3 joltPos = bi.GetPosition(state.bodyId);
                const JPH::Quat  joltRot = bi.GetRotation(state.bodyId);
                const JPH::Vec3  joltForward = joltRot * JPH::Vec3(0.0f, 0.0f, 1.0f);
                LOG_INFO("docs §32: Jolt SHADOW chassis pitch (%s) forward=(%.3f, %.3f, %.3f)",
                    label, (double) joltForward.GetX(), (double) joltForward.GetY(), (double) joltForward.GetZ());

                // docs §46i (task #59 continued): Jolt's OWN raw origin, read directly off the
                // body - NOT via vehicle->GetPosition()/GetMassCenterPosition(), which under
                // apply=1 are themselves written FROM this same value every frame
                // (ApplyJoltToVehicleWheelModel's SetPositionSelf) - comparing Jolt against ODE
                // through that path is circular by construction once the values agree. This is
                // the one number that must be checked against a genuinely independent jolt=0
                // baseline instead.
                LOG_INFO("docs §46i: Jolt raw origin (%s) = (%.3f,%.3f,%.3f)",
                    label, (double) joltPos.GetX(), (double) joltPos.GetY(), (double) joltPos.GetZ());

                // docs §32.4: does the compound CHASSIS shape itself ever come close to the
                // ground, or is it floating well clear while only the wheels' raycasts probe
                // downward? Sample all 8 corners of the shape's world-space bounding box, find
                // the lowest, and extended-raycast straight down from it to see the real
                // clearance to the terrain - same technique as LogNoContactRaycastDiagnostic.
                JPH::RefConst<JPH::Shape> shape = bi.GetShape(state.bodyId);
                if (shape != nullptr) {
                    const JPH::AABox localBounds = shape->GetLocalBounds();
                    const JPH::RMat44 bodyTransform = JPH::RMat44::sRotationTranslation(joltRot, joltPos);
                    float lowestWorldY = 0.0f;
                    JPH::RVec3 lowestWorldPos = joltPos;
                    bool haveLowest = false;
                    for (int corner = 0; corner < 8; ++corner) {
                        const JPH::Vec3 localCorner(
                            (corner & 1) ? localBounds.mMax.GetX() : localBounds.mMin.GetX(),
                            (corner & 2) ? localBounds.mMax.GetY() : localBounds.mMin.GetY(),
                            (corner & 4) ? localBounds.mMax.GetZ() : localBounds.mMin.GetZ());
                        const JPH::RVec3 worldCorner = bodyTransform * localCorner;
                        if (!haveLowest || worldCorner.GetY() < lowestWorldY) {
                            lowestWorldY = worldCorner.GetY();
                            lowestWorldPos = worldCorner;
                            haveLowest = true;
                        }
                    }

                    if (haveLowest) {
                        JPH::RRayCast groundRay{ lowestWorldPos, JPH::Vec3(0.0f, -20.0f, 0.0f) };
                        JPH::RayCastResult hit;
                        const bool foundGround = physics->GetNarrowPhaseQuery().CastRay(groundRay, hit);
                        const float clearance = foundGround ? 20.0f * hit.mFraction : -1.0f;
                        LOG_INFO("docs §32.4: Jolt SHADOW chassis lowest corner (%s) = (%.2f,%.2f,%.2f) clearanceToGround=%.3f%s",
                            label, (double) lowestWorldPos.GetX(), (double) lowestWorldPos.GetY(), (double) lowestWorldPos.GetZ(),
                            (double) clearance, foundGround ? "" : " (no ground found within 20m straight down)");
                    }
                }
            }
        }

        const JPH::Wheels& wheels = state.constraint->GetWheels();
        for (size_t i = 0; i < wheels.size(); ++i) {
            const JPH::Wheel* wheel = wheels[i];
            if (wheel == nullptr)
                continue;

            const JPH::WheelSettings* settings = wheel->GetSettings();
            const float minLen = settings ? settings->mSuspensionMinLength : 0.0f;
            const float maxLen = settings ? settings->mSuspensionMaxLength : 0.0f;
            const float len    = wheel->GetSuspensionLength();
            const float range  = maxLen - minLen;
            const float compressionPct = range > 1.0e-6f ? (maxLen - len) / range * 100.0f : -1.0f;

            LOG_INFO("docs §23.5: wheel state (%s) wheel=%zu contact=%d suspLen=%.3f (range %.3f-%.3f, %.0f%% compressed) angVel=%.2f",
                label, i, wheel->HasContact() ? 1 : 0, (double) len, (double) minLen, (double) maxLen,
                (double) compressionPct, (double) wheel->GetAngularVelocity());

            // docs §38.11: validating WHY cylinder-cast underperformed vs ray. Three competing
            // hypotheses, distinguished here in one shot (this runs on whatever tester is active,
            // but the two decisive signals - what surface the wheel sits on, and the wheel's
            // world axle orientation - are tester-independent):
            //  (1) trimesh: is the contact body a Mesh (road) or HeightField (terrain)? shape
            //      sub-type answers directly. Cylinder shape-casts are known to be flakier vs
            //      arbitrary Mesh than vs a HeightField.
            //  (2) wrong wheel axes: cylinder uses mWheelUp/mWheelForward (set by an unverified
            //      Y-up/Z-forward convention); ray ignores them. If the axle isn't ~horizontal on
            //      a level chassis, or the contact normal is wild, the wheel basis is off.
            //  (3) over-tip feedback loop: contact fine at rest, degrades as pitch grows.
            if (settings != nullptr) {
                JPH::PhysicsSystem* diagPhysics = kraken::fix::jolt::GetPhysicsSystem();
                if (diagPhysics != nullptr) {
                    const JPH::Quat chassisRot = diagPhysics->GetBodyInterface().GetRotation(state.bodyId);
                    // axle (wheel rotation axis) in chassis-local space, per GetWheelLocalBasis:
                    // right = forward x up, normalized. Transform to world.
                    const JPH::Vec3 localAxle = settings->mWheelForward.Cross(settings->mWheelUp).Normalized();
                    const JPH::Vec3 worldAxle = (chassisRot * localAxle).Normalized();
                    if (wheel->HasContact()) {
                        const JPH::Vec3 n = wheel->GetContactNormal();
                        const char* surf = "other";
                        JPH::Body* cb = diagPhysics->GetBodyLockInterfaceNoLock().TryGetBody(wheel->GetContactBodyID());
                        if (cb != nullptr && cb->GetShape() != nullptr) {
                            const JPH::EShapeSubType st = cb->GetShape()->GetSubType();
                            surf = (st == JPH::EShapeSubType::HeightField) ? "HeightField"
                                 : (st == JPH::EShapeSubType::Mesh)        ? "Mesh"
                                 : "other";
                        }
                        LOG_INFO("docs §38.11: wheel=%zu ON surface=%s contactNormal=(%.3f,%.3f,%.3f) worldAxle=(%.3f,%.3f,%.3f)",
                            i, surf, (double) n.GetX(), (double) n.GetY(), (double) n.GetZ(),
                            (double) worldAxle.GetX(), (double) worldAxle.GetY(), (double) worldAxle.GetZ());
                    } else {
                        LOG_INFO("docs §38.11: wheel=%zu NO contact, worldAxle=(%.3f,%.3f,%.3f)",
                            i, (double) worldAxle.GetX(), (double) worldAxle.GetY(), (double) worldAxle.GetZ());
                    }
                }
            }

            if (!wheel->HasContact())
                LogNoContactRaycastDiagnostic(kraken::fix::jolt::GetPhysicsSystem(), state.bodyId, wheel, label, i);
        }
    }

    // docs §28: the REAL ODE-side counterpart to LogWheelState above - that function (and
    // every other "wheel state" log line in this file) reads Jolt's OWN JPH::Wheel shadow
    // state, which exists and updates regardless of [jolt_harness] apply, so it was never a
    // valid way to answer "does Jolt's tuning match real ODE". This reads the REAL Hinge2
    // joint ODE actually uses for wheel suspension (confirmed live by disassembly, not
    // assumed: ai::Wheel::AttachToPhysicObj, VA 0x5eec30, calls dJointCreateHinge2 then
    // dJointAttach and stores the result at Wheel+0x144 = m_jointID).
    //
    // Hinge2's own contract: aside from the one axis softened by dParamSuspensionERP/CFM
    // (confirmed live: dJointSetHinge2Param, VA 0x7cbc50, writes param 9 to joint+0x114 and
    // param 10 to joint+0x118 - stock ODE's dParamSuspensionERP/CFM numbering), the joint
    // RIGIDLY enforces anchor-point coincidence between the two bodies. That means
    // dJointGetHinge2Anchor() (body1/chassis side) and dJointGetHinge2Anchor2() (body2/wheel
    // side) are EXACTLY equal at zero compression by construction - no separate "unloaded
    // reference" needs calibrating. Any real-time deviation between them, projected onto
    // dJointGetHinge2Axis1() (the compliant direction), IS the actual current suspension
    // travel, directly comparable to WheelPrototypeInfo::m_suspensionRange the same way
    // Jolt's own suspLen/range comparison above works.
    //
    // All three ODE accessors confirmed live via disassembly to share one calling convention -
    // JPH_FASTCALL-equivalent __fastcall(dJointID joint /*ecx*/, float* result /*edx*/, writes
    // 3 floats) - matching this file's existing raw-VA-binding style (see
    // fix::kineticfriction's CollideWheelDefault for precedent).
    using DJointGetHinge2VecFn = void(__fastcall*)(void* joint, float* result);
    static const auto RealDJointGetHinge2Anchor  = (DJointGetHinge2VecFn) (0x007d0000);
    static const auto RealDJointGetHinge2Anchor2 = (DJointGetHinge2VecFn) (0x007d0020);
    static const auto RealDJointGetHinge2Axis1   = (DJointGetHinge2VecFn) (0x007d0040);

    // docs §30: dBodyGetPosition (RVA 0x3c4720 = VA 0x7c4720, confirmed by disasm: `lea eax,
    // [ecx+0x98]; ret` - __fastcall(dBody* /*ecx*/) returning a pointer to the body's world
    // position float[3] at body+0x98). In ODE the body origin IS the center of mass (confirmed
    // by disassembling ai::PhysicObj::_AdjustMassCenter, RVA 0x1fb130, which shifts every geom
    // by -m_massCenter so the body frame origin coincides with the COM), so this reads the REAL
    // ODE fore-aft COM directly - the ground truth to check GetMassCenter() against.
    //
    // NOTE: vehicle->GetBody() (PhysicObj::m_body) is NOT the chassis physics body for a
    // ComplexPhysicObj vehicle (it read (0,0,0) live) - the real chassis body is what the wheel
    // Hinge2 joints attach to as body1, so we fetch it via dJointGetBody(joint, 0) instead
    // (RVA 0x3c5150, confirmed by disasm: __fastcall(dxJoint* /*ecx*/, int index /*edx*/),
    // returns node[index].body).
    using DBodyGetPositionFn = const float* (__fastcall*)(void* body);
    static const auto RealDBodyGetPosition = (DBodyGetPositionFn) (0x007c4720);
    using DJointGetBodyFn = void* (__fastcall*)(void* joint, int index);
    static const auto RealDJointGetBody = (DJointGetBodyFn) (0x007c5150);

    static void LogRealOdeWheelState(hta::ai::Vehicle* vehicle, const char* label) {
        const uint32_t numWheels = vehicle->GetNumWheels();

        const hta::Quaternion rot = vehicle->GetRotation();
        const hta::CVector forwardV = rot * hta::CVector(0.0f, 0.0f, 1.0f);

        // docs §30 investigation: get the REAL ODE chassis body from the FIRST valid wheel's
        // Hinge2 joint (body1/chassis side), read its world COM, and log it once. Per-wheel we
        // then log the true fore-aft lever arm from that actual COM - independent of
        // GetMassCenter()/m_initialPos - to test whether the load imbalance comes from a COM
        // that differs from what GetMassCenter() reports.
        void* chassisBody = nullptr;
        for (uint32_t i = 0; i < numWheels; ++i) {
            const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[i];
            if (info.m_bWheelPresent && info.m_wheel != nullptr && info.m_wheel->m_jointID != nullptr) {
                chassisBody = RealDJointGetBody(info.m_wheel->m_jointID, 0);
                break;
            }
        }
        const float* comPtr = chassisBody != nullptr ? RealDBodyGetPosition(chassisBody) : nullptr;
        if (comPtr != nullptr) {
            LOG_INFO("docs §30: REAL ODE chassis COM (%s) body=%p = (%.3f, %.3f, %.3f) forward=(%.3f, %.3f, %.3f)",
                label, chassisBody, (double) comPtr[0], (double) comPtr[1], (double) comPtr[2],
                (double) forwardV.x, (double) forwardV.y, (double) forwardV.z);
        }

        // docs §33: does the REAL ODE chassis body have geoms that vehicle->m_vehicleParts
        // doesn't account for? BuildChassisCompoundShape (§23.10) only ever walks
        // vehicle->m_vehicleParts - this walks the body's ACTUAL geom list directly (same
        // dBodyGetFirstGeom/dGeomGetBodyNext pair PhysicObj::_AdjustMassCenter itself uses,
        // confirmed via disassembly, docs §30) as an independent ground truth, bypassing
        // m_vehicleParts entirely. If the counts disagree, m_vehicleParts is missing real
        // collision geometry that could close the 0.375m gap found in docs §32.4 with real
        // data instead of a synthetic safety shape.
        if (chassisBody != nullptr) {
            int32_t realGeomCount = 0, realTransformCount = 0;
            dxGeom* geom = dBodyGetFirstGeom(reinterpret_cast<dxBody*>(chassisBody));
            while (geom != nullptr) {
                ++realGeomCount;
                // docs §56.6: is THIS geom (the one actually sitting in ai::gGlobalSpace, body-
                // attached or not) enabled from ai::NearCallback's point of view - its very first
                // gate on both sides of a pair, before dCollide even runs (docs §56.2). Checked
                // on the outer geom specifically (not the transform's inner one), since the outer
                // is what's actually linked into the body/space - a disabled inner geom
                // wouldn't be what NearCallback tests anyway.
                const bool enabled = dGeomIsEnabled(geom);
                const int32_t geomClass = dGeomGetClass(geom);
                if (geomClass == dGeomTransformClass) {
                    ++realTransformCount;
                    dxGeom* inner = dGeomTransformGetGeom(geom);
                    const int32_t innerClass = inner != nullptr ? dGeomGetClass(inner) : -1;
                    const float* innerPos = inner != nullptr ? dGeomGetPosition(inner) : nullptr;
                    if (innerPos != nullptr)
                        LOG_INFO("docs §33/§56.6: real ODE chassis geom (%s) #%d = transform, inner class=%d pos=(%.3f,%.3f,%.3f) enabled=%d",
                            label, realGeomCount, innerClass, (double) innerPos[0], (double) innerPos[1], (double) innerPos[2], (int) enabled);
                    else
                        LOG_INFO("docs §33/§56.6: real ODE chassis geom (%s) #%d = transform, inner class=%d enabled=%d",
                            label, realGeomCount, innerClass, (int) enabled);
                } else {
                    const float* pos = dGeomGetPosition(geom);
                    const double radius = geomClass == dSphereClass ? dGeomSphereGetRadius(geom) : -1.0;
                    LOG_INFO("docs §33/§56.6: real ODE chassis geom (%s) #%d = class %d (not a transform) pos=(%.3f,%.3f,%.3f) radius=%.3f enabled=%d",
                        label, realGeomCount, geomClass, (double) pos[0], (double) pos[1], (double) pos[2], radius, (int) enabled);
                }
                geom = dGeomGetBodyNext(geom);
            }
            LOG_INFO("docs §33: real ODE chassis (%s) has %d total geom(s) directly attached (%d transform(s)) - "
                "compare against BuildChassisCompoundShape's own part/geom count in the build log",
                label, realGeomCount, realTransformCount);
        }

        for (uint32_t i = 0; i < numWheels; ++i) {
            const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[i];
            hta::ai::Wheel* wheel = info.m_wheel;
            if (!info.m_bWheelPresent || wheel == nullptr || wheel->m_jointID == nullptr)
                continue;

            float anchor1[4] = {}, anchor2[4] = {}, axis1[4] = {};
            RealDJointGetHinge2Anchor(wheel->m_jointID, anchor1);
            RealDJointGetHinge2Anchor2(wheel->m_jointID, anchor2);
            RealDJointGetHinge2Axis1(wheel->m_jointID, axis1);

            const float dx = anchor2[0] - anchor1[0];
            const float dy = anchor2[1] - anchor1[1];
            const float dz = anchor2[2] - anchor1[2];
            const float displacement = dx * axis1[0] + dy * axis1[1] + dz * axis1[2];

            const hta::ai::WheelPrototypeInfo* wheelProto = wheel->GetPrototypeInfo();
            const float range = wheelProto != nullptr ? std::max(wheelProto->m_suspensionRange, 0.05f) : -1.0f;
            const float pct = range > 0.0f ? (displacement / range) * 100.0f : -1.0f;

            // docs §30: signed fore-aft distance from the REAL ODE COM to this wheel's chassis-
            // side anchor, along the vehicle's forward axis (+ = ahead of COM). Comparing the
            // front group's average to the rear group's gives the true beam lever arms, hence
            // the real static load split, with no dependence on GetMassCenter().
            float foreAft = 0.0f;
            if (comPtr != nullptr) {
                foreAft = (anchor1[0] - comPtr[0]) * forwardV.x
                        + (anchor1[1] - comPtr[1]) * forwardV.y
                        + (anchor1[2] - comPtr[2]) * forwardV.z;
            }

            // docs §30/§31: WheelPrototypeInfo's CFM/ERP (vehicleparts.xml, loaded via
            // LoadFromXML RVA 0x1ee840) - confirmed live to be exactly what's on the Hinge2
            // joint (once, by reading raw floats off the joint at the confirmed
            // dParamSuspensionERP/CFM offsets, +0x114/+0x118, and matching bit-for-bit; that
            // one-shot verification code has since been removed - BuildShadow's real fix now
            // trusts this value directly). Logged here for the same reason foreAft is: a
            // standing, reusable diagnostic for comparing any future vehicle against ODE.
            const float cfm = wheelProto != nullptr ? wheelProto->m_suspensionCFM : -1.0f;
            const float erp = wheelProto != nullptr ? wheelProto->m_suspensionERP : -1.0f;

            LOG_INFO("docs §28: REAL ODE wheel state (%s) wheel=%u displacement=%.4f range=%.3f (%.0f%% compressed, signed) foreAft=%.3f localZ=%.3f cfm=%.6f erp=%.4f",
                label, i, (double) displacement, (double) range, (double) pct,
                (double) foreAft, (double) info.m_initialPos.z, (double) cfm, (double) erp);
        }
    }

    // RMSE accumulator for the in-process autotuner (AutoTune* section, further below) - only
    // ever active for the player's own ShadowState (g_playerShadow), during a scripted autotune
    // trial window. Kept separate from the always-on periodic LogDivergence above: that one is
    // for a human tailing kraken.log, this one is the scalar optimization target from docs §8.4.
    struct DivergenceAccum {
        double   sumSqPos   = 0.0;
        double   sumSqVel   = 0.0;
        double   sumSqAngle = 0.0;
        uint64_t samples    = 0;
    };
    static DivergenceAccum g_autotuneAccum;
    static bool            g_autotuneAccumulating = false;

    static void AccumulateForAutotune(hta::ai::Vehicle* vehicle, ShadowState& state) {
        if (!g_autotuneAccumulating || &state != &g_playerShadow || state.constraint == nullptr)
            return;

        DivergenceSample d = ComputeDivergence(vehicle, state);
        if (!AllFinite({d.posDrift, d.velDrift, d.angleDriftDeg}))
            return; // e.g. the very first frame after a mid-trial rebuild - skip rather than poison the trial's score

        g_autotuneAccum.sumSqPos   += (double) d.posDrift * (double) d.posDrift;
        g_autotuneAccum.sumSqVel   += (double) d.velDrift * (double) d.velDrift;
        g_autotuneAccum.sumSqAngle += (double) d.angleDriftDeg * (double) d.angleDriftDeg;
        ++g_autotuneAccum.samples;
    }

    // Stage 2/3 (docs/jolt-integration-techanalysis.md): writes this frame's Jolt simulation
    // result INTO the real ODE vehicle, so it actually drives gameplay instead of just being
    // logged. Gated by the caller (UpdateShadow, via UpdateOneVehiclePostStep's allowApply
    // parameter) on [jolt_harness] apply, and (for the player) player_only, and (for AI)
    // ai_count - see Apply() for the exact ini semantics.
    //
    // Two things confirmed by research before writing this (see the Stage 2 doc section for
    // full evidence): (1) PhysicObj::GetPosition/GetRotation always read fresh from the live
    // ODE body with no caching, and both the scene-graph render sync and the follow-camera
    // read those same accessors - so SetPositionSelf/SetRotationSelf alone redirect physics
    // AND visuals/camera together, no extra sync call needed; (2) this ODE fork never got
    // ODE's dBodySetKinematic feature (confirmed absent from the whole PDB) - PhysicObj::
    // DisablePhysics() is the closest equivalent (dBodyDisable + dBodyDetachAllContactJoints,
    // leaves geometry/mass/externally-writable transform intact), and it's the same
    // primitive fix::testharness.cpp already uses safely to teleport the vehicle.
    //
    // The wheels are SEPARATE ODE bodies jointed to the chassis via Hinge2, not part of the
    // chassis body itself - disabling only the chassis while wheels stay enabled would let
    // the still-active Hinge2 joints either drag the chassis back awake (ODE's normal
    // behavior for a joint between an active and a disabled body) or fight a teleported
    // chassis with large per-frame correction impulses. So every wheel gets DisablePhysics()
    // too, every frame - once the whole chassis+wheels island is disabled, the joints have
    // nothing active to solve and the island stays dormant, matching stock ODE semantics.
    // Wheel position/rotation is repositioned to JPH::VehicleConstraint's own computed wheel
    // world transform (GetWheelWorldTransform - already accounts for steer angle and current
    // suspension compression), not just rigidly following the chassis at its neutral pose.
    //
    // Stage 3 addition: confirmed via research (docs/jolt-integration-techanalysis.md Stage 3
    // section) that AI decision logic (_CalcSteeringForce/_KeepThrottle) reads the exact same
    // no-cache GetPosition/GetLinearVelocity accessors, and runs strictly AFTER this sync
    // point within the same ai::CServer::Update call (StepScene at source line 1065,
    // ObjContainer::Update at line 1087) - so AI sees fully consistent, already-synced state,
    // same as the player/camera/renderer. No AI-specific handling needed in this function.
    static void ApplyJoltToVehicle(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        if (state.constraint == nullptr)
            return;

        JPH::PhysicsSystem* physics       = kraken::fix::jolt::GetPhysicsSystem();
        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();

        JPH::RVec3 joltPos = bodyInterface.GetPosition(state.bodyId);
        JPH::Quat  joltRot = bodyInterface.GetRotation(state.bodyId);
        JPH::Vec3  joltVel = bodyInterface.GetLinearVelocity(state.bodyId);
        JPH::Vec3  joltAngVel = bodyInterface.GetAngularVelocity(state.bodyId);

        // "Body is flying" gate, ported from the wheelmodel branch's safety-rail precedent
        // (source/fix/wheelmodel.cpp on git branch spring_wheel: max_speed + NaN/Inf guard,
        // self-healing per frame - no permanent disable). If this trips, skip writing this
        // frame entirely and leave ODE fully in control (no DisablePhysics call either) -
        // next frame re-evaluates fresh, exactly like wheelmodel's own pattern.
        const bool finite = AllFinite({
            joltPos.GetX(), joltPos.GetY(), joltPos.GetZ(),
            joltRot.GetX(), joltRot.GetY(), joltRot.GetZ(), joltRot.GetW(),
            joltVel.GetX(), joltVel.GetY(), joltVel.GetZ(),
            joltAngVel.GetX(), joltAngVel.GetY(), joltAngVel.GetZ(),
        });
        if (!finite) {
            LOG_ERROR("Shadow apply (%s): non-finite Jolt state, skipping this frame (leaving ODE in control)", label);
            return;
        }
        const float maxAppliedSpeedMps = MaxAppliedSpeedMps();
        if (joltVel.Length() > maxAppliedSpeedMps) {
            LOG_WARNING("Shadow apply (%s): Jolt speed %.1f m/s exceeds %.1f m/s cap, skipping this frame (leaving ODE in control)",
                label, (double) joltVel.Length(), (double) maxAppliedSpeedMps);
            return;
        }

        // Docs §22.3 ramming diagnostic: confirms empirically (not just via disassembly) whether
        // ai::NearCallback actually attached a live ODE contact joint to this chassis THIS frame,
        // before DisablePhysics()'s dBodyDetachAllContactJoints() below clears it again - i.e.
        // whether the "ODE creates contact joints against a disabled body" risk found by static
        // analysis really fires during gameplay ramming. dBodyGetNumJoints() counts EVERY joint
        // (including the chassis's own permanent per-wheel Hinge2 suspension joints), so compare
        // against a captured structural baseline rather than a raw >0 check - only an EXCESS over
        // that baseline means something extra (a contact joint) got attached. Rising-edge only
        // (not logged every frame of a sustained contact) to avoid log spam.
        if (kraken::Config::Instance().jolt_hotpath_diag.value != 0
            && vehicle->m_body != nullptr && vehicle->m_body->_id != nullptr) {
            const int numJoints = dBodyGetNumJoints(vehicle->m_body->_id);
            if (state.chassisBaselineJointCount < 0) {
                state.chassisBaselineJointCount = numJoints;
            } else {
                const bool hasExtra = numJoints > state.chassisBaselineJointCount;
                if (hasExtra && !state.chassisHadExtraJointLastFrame) {
                    LOG_WARNING("Shadow apply (%s): chassis had %d ODE joint(s) attached "
                                "(baseline %d structural) before this frame's DisablePhysics "
                                "(docs §22.3 ramming risk observed live)",
                                label, numJoints, state.chassisBaselineJointCount);
                }
                state.chassisHadExtraJointLastFrame = hasExtra;
            }
        }

        vehicle->DisablePhysics();
        // docs §67.4: confirmed live (verify_cellreg.py, §67.3's own diagnostic) - m_physicState's
        // bit1 read 0 for EVERY Jolt-shadowed vehicle (player and all 7 AI), every sample, for a
        // full ~24s drive. PhysicObj::SetCorrectEnabledCellsCounter (called from SetPositionSelf,
        // next line - disassembled by hand, offsets confirmed against the recovered PhysicObj.hpp
        // field names) only re-walks the landscape's collision-cell grid and re-registers this
        // object when bit0==0 (true here - DisablePhysics clears it) AND bit1==1 (never true here,
        // and DisablePhysics never touches it either) - otherwise it's a no-op. So a Jolt-driven
        // vehicle's cell registration freezes wherever it was before physics was disabled and
        // never follows the Jolt-driven position - and the weapon hit-trace (ShellTraceLineCallback)
        // queries THIS cell grid, not raw ODE space, so a vehicle stuck in its old cells goes
        // invisible to it (live report: machine-gun hits stopped registering on enemies, "no
        // reaction at all" - works under [jolt] enabled=0). Setting bit1 here, every frame, is the
        // minimal fix - SetCorrectEnabledCellsCounter's own native logic does the real cell walk
        // once its gate is open; nothing about that function is touched or duplicated.
        vehicle->m_physicState |= 2;
        vehicle->SetPositionSelf(hta::CVector(joltPos.GetX(), joltPos.GetY(), joltPos.GetZ()));
        vehicle->SetRotationSelf(hta::Quaternion(joltRot.GetX(), joltRot.GetY(), joltRot.GetZ(), joltRot.GetW()));
        vehicle->SetLinearVelocity(hta::CVector(joltVel.GetX(), joltVel.GetY(), joltVel.GetZ()));
        vehicle->SetAngularVelocity(hta::CVector(joltAngVel.GetX(), joltAngVel.GetY(), joltAngVel.GetZ()));

        // Lazily size the per-wheel diagnostic trackers to match wheelOrder (docs §22.3) - grown
        // here rather than in BuildShadow's own wheel-population loop to keep that loop untouched.
        if (state.wheelBaselineJointCount.size() < state.wheelOrder.size()) {
            state.wheelBaselineJointCount.resize(state.wheelOrder.size(), -1);
            state.wheelHadExtraJointLastFrame.resize(state.wheelOrder.size(), false);
        }

        // docs §23.12: same tile-size formula as BuildShadow's tire-callback copy (and
        // ultimately CollideWheelAndLandscape) - computed once per frame here (level geometry
        // is constant for the run), used below for the skid-trace soil lookup.
        const float wheelTileSize = (float) hta::ai::CServer::Instance()->GetLevelSize()
            / (float) hta::ai::CServer::Instance()->GetWorld()->GetLandscape().GetTileSize();

        const JPH::Wheels& wheels = state.constraint->GetWheels();
        for (size_t i = 0; i < state.wheelOrder.size() && i < wheels.size(); ++i) {
            hta::ai::Wheel* wheel = state.wheelOrder[i];
            if (wheel == nullptr)
                continue;

            JPH::RMat44 wheelTransform = state.constraint->GetWheelWorldTransform(
                (JPH::uint) i, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
            JPH::Vec3 wheelPos = wheelTransform.GetTranslation();
            JPH::Quat wheelRot = wheelTransform.GetQuaternion();

            if (!AllFinite({wheelPos.GetX(), wheelPos.GetY(), wheelPos.GetZ(),
                            wheelRot.GetX(), wheelRot.GetY(), wheelRot.GetZ(), wheelRot.GetW()}))
                continue; // leave this one wheel wherever it last was rather than write garbage

            // Docs §22.3 ramming diagnostic, wheel side (see the chassis version above for the
            // full rationale) - wheels touch the ground EVERY frame, so this answers docs §7's
            // still-open question (does wheel-ground contact go through ai::NearCallback at all,
            // or a separate CollideWheelAndLandscape/CollideWheelAndAsphalt path that bypasses it)
            // almost immediately, rather than waiting for a rare ramming event.
            if (kraken::Config::Instance().jolt_hotpath_diag.value != 0
                && wheel->m_body != nullptr && wheel->m_body->_id != nullptr) {
                const int numJoints = dBodyGetNumJoints(wheel->m_body->_id);
                if (state.wheelBaselineJointCount[i] < 0) {
                    state.wheelBaselineJointCount[i] = numJoints;
                } else {
                    const bool hasExtra = numJoints > state.wheelBaselineJointCount[i];
                    if (hasExtra && !state.wheelHadExtraJointLastFrame[i]) {
                        LOG_WARNING("Shadow apply (%s): wheel %zu had %d ODE joint(s) attached "
                                    "(baseline %d structural) before this frame's DisablePhysics "
                                    "(docs §22.3/§7 wheel-ground contact-joint check)",
                                    label, i, numJoints, state.wheelBaselineJointCount[i]);
                    }
                    state.wheelHadExtraJointLastFrame[i] = hasExtra;
                }
            }

            wheel->DisablePhysics();
            wheel->SetPositionSelf(hta::CVector(wheelPos.GetX(), wheelPos.GetY(), wheelPos.GetZ()));
            wheel->SetRotationSelf(hta::Quaternion(wheelRot.GetX(), wheelRot.GetY(), wheelRot.GetZ(), wheelRot.GetW()));

            // docs §23.12: skid-trace visuals (WheelTraceMgr::StartSkidding/AddTrace/
            // EndSkidding), same kappa-threshold/oil-mode rule as CollideWheelSurface -
            // triggered HERE, once per real frame per wheel (matching the ODE path's actual
            // cadence), not from SetTireMaxImpulseCallback. That callback runs up to
            // PhysicsSettings::mNumVelocitySteps (10, default) times per physics STEP, not
            // once - calling a real, state-mutating side effect (WheelTraceMgr) that often
            // caused an apparent hang in live testing the first time this was tried (kraken.log
            // froze solid mid-frame, process still "Responding" per Windows but CPU essentially
            // idle - almost certainly WheelTraceMgr's internal trace-mesh growth choking on a
            // 10x call rate it was never designed for). JPH::WheelWV::mLongitudinalSlip already
            // holds this step's final settled slip value by the time PhysicsSystem::Update()
            // has returned, so reading it once here gives the same skid/no-skid answer the
            // tire callback would have, at the intended cadence.
            const JPH::Wheel* jphWheel = wheels[i];
            const JPH::WheelWV* wheelWV = static_cast<const JPH::WheelWV*>(jphWheel);
            hta::m3d::WheelTraceMgr& traceMgr = hta::ai::CServer::Instance()->GetWorld()->GetWheelTracesMgr();
            const bool inSkid = jphWheel->HasContact()
                && (vehicle->m_onOilMode || std::fabs(wheelWV->mLongitudinalSlip) > 0.75f);
            if (inSkid) {
                const JPH::RVec3 tracePos = jphWheel->GetContactPosition() + jphWheel->GetContactNormal() * 0.01f;
                const hta::CVector pos(tracePos.GetX(), tracePos.GetY(), tracePos.GetZ());
                const int32_t soilX = (int32_t) (tracePos.GetX() / wheelTileSize + 0.5f);
                const int32_t soilZ = (int32_t) (tracePos.GetZ() / wheelTileSize + 0.5f);
                const hta::ai::DynamicScene::SoilProps& props =
                    hta::ai::DynamicScene::Instance()->GetSoilProps((uint32_t) soilX, (uint32_t) soilZ);
                if (traceMgr.IsSkiddingStarted(wheel)) {
                    traceMgr.AddTrace(pos, wheel->GetRotation(), wheel->GetWidth(), wheel, props.m_idx, false);
                } else {
                    traceMgr.StartSkidding(wheel, props.m_idx);
                    traceMgr.AddTrace(pos, wheel->GetRotation(), wheel->GetWidth(), wheel, props.m_idx, true);
                }
            } else if (traceMgr.IsSkiddingStarted(wheel)) {
                traceMgr.EndSkidding(wheel, true);
            }
        }
    }

    // Stage 2 for wheelmodel=4 (docs/stage2-plan.md correction, written after a live acceptance
    // run exposed this gap). ApplyJoltToVehicle above assumes JPH::VehicleConstraint's own wheel
    // array (state.constraint->GetWheels()/GetWheelWorldTransform) - but BuildShadow's
    // wheelModelMode branch (:2503-2506) never calls physics->AddConstraint for wheelmodel 2 or
    // 4, so that array is never populated there. UpdateOneVehiclePostStep's own
    // `allowApply && !state.wheelModelMode` guard already excluded both those modes from calling
    // ApplyJoltToVehicle at all - meaning [jolt_harness] apply=1 was a complete no-op under
    // wheelmodel=4, the one mode all of sec94-123's validation work (assists/governor/soildrag/
    // damping/contact-model/chassis-inertia) actually targets. Found live: an unscripted-driving
    // Stage 2 acceptance run (apply=1, wheelmodel=4) showed unbounded, growing divergence -
    // 250-300m position, up to 180deg orientation, over ~70s - because ODE was never being
    // overwritten at all and the two sims were simply free-running independently, exactly like
    // shadow-only mode. No NaN/speed-cap gate trip, because ApplyJoltToVehicle's own gate never
    // ran either.
    //
    // The chassis half is copied verbatim from ApplyJoltToVehicle - BuildShadow's own comment at
    // :2494 confirms the chassis body/mass is constructed identically regardless of mode, so
    // state.bodyId means the same thing here. Only the wheel half differs: wheels come from
    // state.wheelBodies (real per-wheel Jolt bodies, populated only in wheelBodyMode/mode 4), not
    // a VehicleConstraint's wheel array. Skid-trace visuals (WheelTraceMgr) are NOT ported here -
    // they read JPH::WheelWV::mLongitudinalSlip, a field that belongs to the native vehicle
    // wheel type and has no equivalent on a plain wheelmodel wheel body; left as a named gap
    // rather than guessed at. Mode 2 (wheelModelMode but not wheelBodyMode - forces applied
    // straight to the chassis, no wheel bodies to sync) still has no writeback at all, unchanged.
    static void ApplyJoltToVehicleWheelModel(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        if (state.bodyId.IsInvalid())
            return;

        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return;
        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();

        JPH::RVec3 joltPos    = bodyInterface.GetPosition(state.bodyId);
        JPH::Quat  joltRot    = bodyInterface.GetRotation(state.bodyId);
        JPH::Vec3  joltVel    = bodyInterface.GetLinearVelocity(state.bodyId);
        JPH::Vec3  joltAngVel = bodyInterface.GetAngularVelocity(state.bodyId);

        // Same "body is flying" policy as ApplyJoltToVehicle (:4569-4588) - duplicated rather
        // than shared because the two functions read wheels in incompatible ways below and a
        // shared helper would just take these four values as parameters anyway.
        const bool finite = AllFinite({
            joltPos.GetX(), joltPos.GetY(), joltPos.GetZ(),
            joltRot.GetX(), joltRot.GetY(), joltRot.GetZ(), joltRot.GetW(),
            joltVel.GetX(), joltVel.GetY(), joltVel.GetZ(),
            joltAngVel.GetX(), joltAngVel.GetY(), joltAngVel.GetZ(),
        });
        if (!finite) {
            LOG_ERROR("Shadow apply wheelmodel (%s): non-finite Jolt state, skipping this frame (leaving ODE in control)", label);
            return;
        }
        const float maxAppliedSpeedMps = MaxAppliedSpeedMps();
        if (joltVel.Length() > maxAppliedSpeedMps) {
            LOG_WARNING("Shadow apply wheelmodel (%s): Jolt speed %.1f m/s exceeds %.1f m/s cap, skipping this frame (leaving ODE in control)",
                label, (double) joltVel.Length(), (double) maxAppliedSpeedMps);
            return;
        }

        vehicle->DisablePhysics();
        // docs §67.4: confirmed live (verify_cellreg.py, §67.3's own diagnostic) - m_physicState's
        // bit1 read 0 for EVERY Jolt-shadowed vehicle (player and all 7 AI), every sample, for a
        // full ~24s drive. PhysicObj::SetCorrectEnabledCellsCounter (called from SetPositionSelf,
        // next line - disassembled by hand, offsets confirmed against the recovered PhysicObj.hpp
        // field names) only re-walks the landscape's collision-cell grid and re-registers this
        // object when bit0==0 (true here - DisablePhysics clears it) AND bit1==1 (never true here,
        // and DisablePhysics never touches it either) - otherwise it's a no-op. So a Jolt-driven
        // vehicle's cell registration freezes wherever it was before physics was disabled and
        // never follows the Jolt-driven position - and the weapon hit-trace (ShellTraceLineCallback)
        // queries THIS cell grid, not raw ODE space, so a vehicle stuck in its old cells goes
        // invisible to it (live report: machine-gun hits stopped registering on enemies, "no
        // reaction at all" - works under [jolt] enabled=0). Setting bit1 here, every frame, is the
        // minimal fix - SetCorrectEnabledCellsCounter's own native logic does the real cell walk
        // once its gate is open; nothing about that function is touched or duplicated.
        vehicle->m_physicState |= 2;
        vehicle->SetPositionSelf(hta::CVector(joltPos.GetX(), joltPos.GetY(), joltPos.GetZ()));
        vehicle->SetRotationSelf(hta::Quaternion(joltRot.GetX(), joltRot.GetY(), joltRot.GetZ(), joltRot.GetW()));
        vehicle->SetLinearVelocity(hta::CVector(joltVel.GetX(), joltVel.GetY(), joltVel.GetZ()));
        vehicle->SetAngularVelocity(hta::CVector(joltAngVel.GetX(), joltAngVel.GetY(), joltAngVel.GetZ()));

        const size_t nw = std::min(state.wheelBodies.size(), state.wheelOrder.size());
        for (size_t i = 0; i < nw; ++i) {
            hta::ai::Wheel* wheel = state.wheelOrder[i];
            if (wheel == nullptr || state.wheelBodies[i].IsInvalid())
                continue;
            JPH::Body* wb = physics->GetBodyLockInterfaceNoLock().TryGetBody(state.wheelBodies[i]);
            if (wb == nullptr)
                continue;

            const JPH::RVec3 wheelPos = wb->GetPosition();
            const JPH::Quat  wheelRot = wb->GetRotation();
            if (!AllFinite({(float) wheelPos.GetX(), (float) wheelPos.GetY(), (float) wheelPos.GetZ(),
                            wheelRot.GetX(), wheelRot.GetY(), wheelRot.GetZ(), wheelRot.GetW()}))
                continue; // leave this one wheel wherever it last was rather than write garbage

            // docs §65.4-§65.8, §127-§135 (task #58, wheel-spin visual bug - superseded, kept in
            // jolt-integration-techanalysis.md §65.8/§136 for the record): five things were tried
            // here in turn - a global/front-rear sign, an attachPos.x>0 swing-twist mirror
            // (worked for one vehicle, backwards on Ural01/Fighter01), and a per-vehicle-name
            // exception list (rejected: "ODE works without one, so Jolt has a real answer too" -
            // correct call; find it below).
            //
            // docs §136: the real fix. BuildWheelBodies spawns every wheel body at chassisRot
            // (see its own comment, :2321-2ish) for constraint-frame convenience, discarding the
            // wheel's own NATIVE rotation - exactly the artist-authored placement (including any
            // mesh mirroring) that native ODE's geom starts from and simply integrates a
            // side-agnostic world-space spin on top of (confirmed this session by full
            // disassembly: dJointSetHinge2Axis1/2 in ai::Wheel::AttachToPhysicObj and dParamVel2
            // in ai::Vehicle::_KeepGearBox are BOTH computed purely from chassis rotation and a
            // single global constant, zero per-wheel branching - ODE needs no runtime correction
            // anywhere because its STARTING POSE already carries whatever mirror the mesh needs;
            // nothing about its motion does). ws.visualMirrorDelta (WheelSetup's own comment has
            // the full derivation) is exactly `Inverse(chassisRot) * wheel->GetRotation()`,
            // captured once at body-build time - measured live: identity on all 4 of Fighter01's
            // wheels, exactly 180 deg about Y on Molokovoz01's 2 right wheels only. Composing it
            // back here reconstructs the native quaternion because both engines apply the
            // identical world-space spin on top of a (possibly) different starting pose - no
            // vehicle name, wheel count, or axle layout enters the formula, so it self-adapts to
            // any prototype without curation. Touches ONLY this cosmetic writeback; wheelRot
            // itself (used everywhere else - constraint frames, axleLocal projection, the tyre
            // model) is completely unchanged.
            const JPH::Quat visualRot = (wheelRot * (i < state.wheelSetup.size()
                ? state.wheelSetup[i].visualMirrorDelta : JPH::Quat::sIdentity())).Normalized();

            // docs §137.4 (task #60, temporary): ground-truth check for the §137.3 ordering fix -
            // this is the EXACT quaternion that just got written to the mesh (wheel->SetRotationSelf
            // below), so a delta-angle computed here, frame to frame, is what the player actually
            // sees rotate - not a proxy like §67's omega. Gated on restHeld so it stays silent
            // during normal driving and only speaks while the vehicle is supposed to be parked.
            if (state.restHeld) {
                if (state.dbgPrevVisualRot.size() != state.wheelBodies.size())
                    state.dbgPrevVisualRot.assign(state.wheelBodies.size(), visualRot);
                if (i < state.dbgPrevVisualRot.size()) {
                    const float dot = std::clamp(std::fabs(visualRot.Dot(state.dbgPrevVisualRot[i])), 0.0f, 1.0f);
                    const float deltaDeg = 2.0f * std::acos(dot) * (180.0f / 3.14159265f);
                    LOG_INFO("docs §137.4: visual wheel delta (%s) w=%zu deltaDeg=%.4f", label, i, (double) deltaDeg);
                    state.dbgPrevVisualRot[i] = visualRot;
                }
            } else if (i < state.dbgPrevVisualRot.size()) {
                state.dbgPrevVisualRot[i] = visualRot;   // keep it current so re-entering restHeld starts from 0
            }

            wheel->DisablePhysics();
            wheel->SetPositionSelf(hta::CVector((float) wheelPos.GetX(), (float) wheelPos.GetY(), (float) wheelPos.GetZ()));
            wheel->SetRotationSelf(hta::Quaternion(visualRot.GetX(), visualRot.GetY(), visualRot.GetZ(), visualRot.GetW()));
        }
    }

    // No Profiled wrapper: unlike ApplyJoltToVehicleProfiled below, this would need to reference
    // g_joltProfile/JoltProfileState, both declared further down this file - called directly from
    // UpdateOneVehiclePostStep instead, at the (small) cost of this one path missing perfmon's
    // per-call timing breakdown until someone cares enough to reshuffle the declarations.

    // ------------------------------------------------------------------------------------------
    // Perf split instrumentation (docs/jolt-integration-techanalysis.md §17 follow-up). §17
    // measured -57% FPS at ai_count=16 (~2.25ms/AI vehicle) and flagged, but explicitly did NOT
    // confirm with a profiler, a hypothesis: that the cost is dominated by ApplyJoltToVehicle's
    // single-threaded ODE write-back above, not by JPH::PhysicsSystem::Update() itself (which
    // Jolt's own job system parallelizes across islands). This section measures - it does not
    // change - the two existing call sites (see StepPhysicsProfiled/ApplyJoltToVehicleProfiled
    // below - StepPhysicsProfiled is called once per frame from UpdateShadow itself since the
    // §18.1 single-step fix, ApplyJoltToVehicleProfiled from UpdateOneVehiclePostStep, one per
    // live vehicle): no control flow, arguments, or return values are altered anywhere, only
    // std::chrono::steady_clock timestamps wrapped
    // around calls that already happen. Same measurement style (steady_clock, accumulate between
    // logs, periodic kraken::logger summary rather than per-frame spam) as fix::testharness's own
    // PerfTick (testharness.cpp).
    //
    // Gated on the ALREADY-EXISTING [testharness] perfmon/perfmon_interval config rather than a
    // new ini flag - this is conceptually the same feature PerfTick already is ("periodic
    // wall-clock perf line to kraken.log"), just splitting a different pair of calls. When
    // perfmon=0 the only added cost versus the pre-instrumentation code is one extra uint32
    // config read per call - no timing, no accumulation, no logging.
    struct JoltProfileState {
        bool                                   hasLast           = false;
        std::chrono::steady_clock::time_point  lastLogTime;
        double                                 physicsUpdateMs   = 0.0; // sum of StepPhysics() wall time this interval - at most one call per frame (see UpdateShadow's Pass 2 - docs/jolt-integration-techanalysis.md §18.1/§19 on why it used to be one call per vehicle)
        double                                 applyVehicleMs    = 0.0; // sum of ApplyJoltToVehicle() wall time this interval - ALL calls
        uint64_t                               applyVehicleCalls = 0;  // count of ApplyJoltToVehicle() calls this interval (only the ones that actually ran, i.e. allowApply==true)
        uint64_t                               frames            = 0;  // UpdateShadow() invocations this interval - one per real game frame, see JoltProfileFrameEnd's call site
        // docs §52 (Этап 0): the full per-frame cost breakdown needed to reconcile §17/§18.3/§26.2
        // and to see where time actually goes. odeStep is the baseline Jolt must beat; joltTotal is
        // the whole per-frame Jolt-side overhead; wheelModel and mirror are its two biggest
        // main-thread (non-parallelized) components. CRUCIAL context: in wheelmodel APPLY mode
        // (wheelmodel=2, the current config) ApplyJoltToVehicle NEVER runs (UpdateOneVehiclePostStep
        // gates it on !wheelModelMode), so applyVehicleMs is 0 and the dominant cost is wheelModelMs
        // (per-wheel CollideShape on the main thread) - which the original profiler didn't measure.
        double                                 odeStepMs         = 0.0; // scene->StepScene() wall time - the ODE cost to beat
        double                                 joltTotalMs       = 0.0; // whole UpdateShadow() wall time - total per-frame Jolt-side cost
        double                                 wheelModelMs      = 0.0; // sum of StepWheelModel() across all vehicles this interval
        double                                 mirrorMs          = 0.0; // MirrorOtherVehicles() wall time this interval
    };
    static JoltProfileState g_joltProfile;

    // Thin wrapper around kraken::fix::jolt::StepPhysics - identical call/args/return (none) to
    // the call it replaces; only measures wall time when profiling is on.
    static void StepPhysicsProfiled(float elapsedTime) {
        // docs §59/§63 (Этап 1, шаг 3): advance the harvest parity exactly here - once per step,
        // on the main thread, immediately BEFORE the step runs. Not inside a listener: there is
        // one listener per vehicle, so per-listener flipping would advance a shared counter once
        // per vehicle per step. The write buffer for the step about to run becomes
        // g_harvestStep&1, and the listener inside that step reads the other one, which is what
        // the previous step's narrow phase filled - JobStepListeners runs before
        // JobFindCollisions, so a listener can never read its own step's contacts.
        const uint32_t writeParity = g_harvestStep.fetch_add(1, std::memory_order_relaxed) + 1;
        for (uint32_t s = 0; s < kMaxVehicleSlots; ++s)
            for (uint32_t w = 0; w < kMaxHarvestWheels; ++w)
                g_wheelHarvest[writeParity & 1u][s][w].count.store(0, std::memory_order_relaxed);

        if (kraken::Config::Instance().testharness_perfmon.value == 0) {
            kraken::fix::jolt::StepPhysics(elapsedTime);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        kraken::fix::jolt::StepPhysics(elapsedTime);
        const auto t1 = std::chrono::steady_clock::now();
        g_joltProfile.physicsUpdateMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    // Thin wrapper around ApplyJoltToVehicle, same pattern as StepPhysicsProfiled above.
    static void ApplyJoltToVehicleProfiled(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        if (kraken::Config::Instance().testharness_perfmon.value == 0) {
            ApplyJoltToVehicle(vehicle, state, label);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        ApplyJoltToVehicle(vehicle, state, label);
        const auto t1 = std::chrono::steady_clock::now();
        g_joltProfile.applyVehicleMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ++g_joltProfile.applyVehicleCalls;
    }

    // Called once per UpdateShadow() invocation (i.e. once per real game frame - see the call
    // site at the end of UpdateShadow, below), after every vehicle this frame has already been
    // run through StepPhysicsProfiled/ApplyJoltToVehicleProfiled above. Counts the frame, then -
    // no more often than once every perfmon_interval seconds - logs one summary line and resets
    // the interval's accumulators, exactly like PerfTick's own windowed-average pattern.
    static void JoltProfileFrameEnd() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.testharness_perfmon.value == 0)
            return;

        ++g_joltProfile.frames;

        const auto now = std::chrono::steady_clock::now();
        if (!g_joltProfile.hasLast) {
            g_joltProfile.lastLogTime = now;
            g_joltProfile.hasLast     = true;
            return;
        }

        const double elapsedSec  = std::chrono::duration<double>(now - g_joltProfile.lastLogTime).count();
        const double intervalSec = (double) config.testharness_perfmon_interval.value;
        if (elapsedSec < intervalSec)
            return;

        const double physicsAvgMs  = g_joltProfile.physicsUpdateMs / (double) g_joltProfile.frames;
        const double applyAvgMs    = g_joltProfile.applyVehicleMs  / (double) g_joltProfile.frames;
        const double callsPerFrame = (double) g_joltProfile.applyVehicleCalls / (double) g_joltProfile.frames;
        const double odeStepAvgMs  = g_joltProfile.odeStepMs    / (double) g_joltProfile.frames;
        const double joltTotalAvgMs= g_joltProfile.joltTotalMs  / (double) g_joltProfile.frames;
        const double wheelModelAvg = g_joltProfile.wheelModelMs / (double) g_joltProfile.frames;
        const double mirrorAvgMs   = g_joltProfile.mirrorMs     / (double) g_joltProfile.frames;

        // docs §52: ode_step vs jolt_total are the headline A/B numbers; the rest is the Jolt-side
        // breakdown. jolt_total includes physics_update + wheelmodel + mirror + applyvehicle + glue.
        LOG_INFO("[jolt_profile] ode_step_avg_ms=%.2f jolt_total_avg_ms=%.2f | physics_update_avg_ms=%.2f wheelmodel_avg_ms=%.2f mirror_avg_ms=%.2f applyvehicle_avg_ms=%.2f applyvehicle_calls_per_frame=%.2f frames=%llu",
            odeStepAvgMs, joltTotalAvgMs, physicsAvgMs, wheelModelAvg, mirrorAvgMs, applyAvgMs, callsPerFrame, (unsigned long long) g_joltProfile.frames);

        g_joltProfile.physicsUpdateMs   = 0.0;
        g_joltProfile.applyVehicleMs    = 0.0;
        g_joltProfile.applyVehicleCalls = 0;
        g_joltProfile.frames            = 0;
        g_joltProfile.odeStepMs         = 0.0;
        g_joltProfile.joltTotalMs       = 0.0;
        g_joltProfile.wheelModelMs      = 0.0;
        g_joltProfile.mirrorMs          = 0.0;
        g_joltProfile.lastLogTime       = now;
    }
    // ------------------------------------------------------------------------------------------

    // Detects a wheel that ShadowState captured at build time (state.wheelOrder/
    // wheelSourceIndex, filled in BuildShadow's loop over vehicle->m_wheels) having since gone
    // away - e.g. shot off in combat - which ApplyJoltToVehicle would otherwise keep calling
    // wheel->DisablePhysics()/SetPositionSelf()/SetRotationSelf() through every frame via a
    // possibly-dangling hta::ai::Wheel* (use-after-free risk), or, if the object outlives
    // detachment, silently teleport a should-be-flying wheel fragment back onto the shadow's
    // computed Jolt position instead of letting it fly off as debris.
    //
    // Checks each captured (wheelOrder[k], wheelSourceIndex[k]) pair against the vehicle's
    // CURRENT m_wheels: the source index must still be in range (the wheel count itself could
    // have changed), the slot at that index must still hold the SAME Wheel* (not silently
    // reused for a different wheel), and that slot must still report m_bWheelPresent. Any one
    // pair failing means the shadow was built from stale wheel data and must be rebuilt from
    // scratch (see the rebuild condition in UpdateOneVehiclePreStep below) - BuildShadow will
    // re-scan vehicle->m_wheels fresh and simply compact out whatever's missing now, exactly as
    // it already does on first build.
    //
    // An empty wheelOrder (shadow not yet built, or a genuinely wheelless vehicle) is NOT a
    // failure here - it just means there's nothing to have lost, so this returns true (no
    // rebuild needed FOR THIS REASON) rather than forcing a spurious rebuild loop.
    static bool ShadowWheelsStillPresent(hta::ai::Vehicle* vehicle, const ShadowState& state) {
        const uint32_t numWheelsNow = vehicle->GetNumWheels();
        for (size_t k = 0; k < state.wheelOrder.size(); ++k) {
            const uint32_t sourceIndex = state.wheelSourceIndex[k];
            if (sourceIndex >= numWheelsNow)
                return false; // wheel count shrank past where this one used to live

            const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[sourceIndex];
            if (info.m_wheel != state.wheelOrder[k] || !info.m_bWheelPresent)
                return false; // detached/destroyed, or slot reused for a different wheel
        }
        return true;
    }

    // Shared per-vehicle per-frame tick, used identically for the player and for every Stage 3
    // AI vehicle - split into a pre-step half (rebuild-on-swap + feed inputs) and a post-step
    // half (apply/autotune/periodic log) so UpdateShadow (below) can sandwich exactly ONE shared
    // StepPhysicsProfiled call between them for every vehicle at once, instead of stepping the
    // one shared JPH::PhysicsSystem once per vehicle (docs/jolt-integration-techanalysis.md
    // §18.1/§18.4 - at ai_count=16 that used to re-simulate the entire shared world 17x/frame).
    //
    // Returns false if this ShadowState isn't usable this frame (BuildShadow failed, e.g. right
    // after a level load before the vehicle's data is fully ready) - same semantics as the old
    // combined function's early return: the caller simply skips this vehicle for this frame and
    // retries on the next one, no state is left half-initialized.
    static bool UpdateOneVehiclePreStep(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label, uint32_t collisionGroupId, float dt) {
        // Rebuilds on a vehicle swap (level reload, vehicle switch), a tuning-parameter change
        // (g_tuningGeneration bumped by SetTuningOverride - the autotuner uses this to apply a
        // new candidate suspension/friction set between trials without needing to recreate
        // ShadowState itself), or one of the wheels captured at the shadow's last build having
        // since been detached/destroyed/replaced (ShadowWheelsStillPresent - see its own
        // comment; guards against ApplyJoltToVehicle chasing a stale hta::ai::Wheel* after
        // combat damage tears a wheel off). See BuildShadow's comment for why the previous
        // constraint/body is abandoned rather than torn down either way.
        const bool vehicleSwapped = vehicle != state.vehicle;
        const bool tuningChanged  = state.builtGeneration != g_tuningGeneration;
        const bool wheelsChanged  = !vehicleSwapped && !ShadowWheelsStillPresent(vehicle, state); // only meaningful once already built for this same vehicle
        if (vehicleSwapped || tuningChanged || wheelsChanged) {
            if (wheelsChanged)
                LOG_WARNING("Shadow (%s): a wheel present at build time is now gone/replaced, rebuilding", label);

            if (!BuildShadow(vehicle, state, label, collisionGroupId))
                return false; // vehicle data can be not-yet-fully-initialized right after a level
                              // load - just keep retrying on later frames
            state.vehicle = vehicle;
            state.frameCounter = 0;
        }

        // docs §39.2: wheelmodel APPLY path drives the chassis itself (no VehicleConstraint,
        // no proxy, no constraint driver input) - the forces must be applied BEFORE this frame's
        // single StepPhysicsProfiled (pass 2), so it happens here in the pre-step.
        // docs §63 (Этап 1, шаг 4): in mode 4 the old chassis force path is switched off ENTIRELY
        // (plan line 350: «Старый StepWheelModel в режиме 4 отключается целиком»). Until step 3 it
        // still ran, which is what kept the vehicle standing while the wheel bodies were only
        // observed; now the forces are applied to those bodies inside VehicleStepListener::OnStep
        // and running both would support the chassis twice. The wheel bodies also stopped being
        // sensors at step 3, so the rim is a real contact - a second, parallel support path would
        // not merely double the load, it would fight the SixDOF.
        // docs §122: outside the mode-4 block on purpose. Damping is a BODY PROPERTY, not a force,
        // and the reference applies it to every body in the world regardless of which model drives
        // it - so gating it on wheelBodyMode would make the two modes differ by more than the wheel
        // representation, which is the one thing mode 2 exists to isolate.
        ApplyBodyDamping(state, label);

        if (state.wheelBodyMode) {
            // Wake FIRST, command second: UpdateWheelSpinCommands gates every write on IsActive,
            // so the other order would skip the whole vehicle on the frame it woke.
            KeepShadowBodiesAwake(state, vehicle);
            UpdateWheelSpinCommands(vehicle, state, dt);
            // AFTER the motor commands, but still pre-step: forces added here are consumed by the
            // Update that follows, which is the same relationship the reference has (ODE applies
            // these after StepScene and the next step consumes them). Never from a contact
            // callback - AddForce there is silently discarded.
            ApplyArcadeAssists(vehicle, state);
            // Main thread on purpose: GetSoilProps' thread safety is unverified, which is exactly
            // why step 4 declined to do the soil lookup from inside the step listener. Here there
            // is no such problem - mode 2's StepWheelModel already calls it from this same thread.
            ApplySoilRollingDrag(state, label);
            return true;
        }

        if (state.wheelModelMode) {
            // docs §52: StepWheelModel is the dominant main-thread cost in wheelmodel mode
            // (per-wheel CollideShape, serial, one call per vehicle/frame) - profiled separately
            // since it's the single biggest suspect for the AI-scaling slowdown, and it's NOT
            // covered by StepPhysicsProfiled/ApplyJoltToVehicleProfiled.
            if (kraken::Config::Instance().testharness_perfmon.value != 0) {
                const auto w0 = std::chrono::steady_clock::now();
                StepWheelModel(vehicle, state, label, dt);
                g_joltProfile.wheelModelMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
            } else {
                StepWheelModel(vehicle, state, label, dt);
            }
            return true;
        }

        TryBuildWheelProxiesOnceSettled(kraken::fix::jolt::GetPhysicsSystem(), state, label);
        UpdateShadowInputs(vehicle, state);
        return true;
    }

    // Second half of the per-vehicle tick (see UpdateOneVehiclePreStep above) - runs AFTER
    // UpdateShadow's single shared StepPhysicsProfiled call for this frame. Only called for
    // vehicles whose pre-step returned true.
    static void UpdateOneVehiclePostStep(hta::ai::Vehicle* vehicle, ShadowState& state, bool allowApply, const char* label) {
        // docs §125.2: a force-based stick damper, however strong, can only ever APPROACH v=0
        // under a persistent bias (gravity on a slope) - the instant v hits exactly zero, the
        // damper force ALSO hits zero (it's proportional to v), so the bias re-accelerates it a
        // tiny bit the very next frame. No coefficient closes that gap; it is a structural
        // property of a velocity-proportional force, not a tuning miss (see the abandoned
        // gain-bisection in GeneralizedContactForce, docs §125/stage2-plan.md §2.9). Confirmed
        // live: even the 2x stick damper left a residual around 0.004-0.007 m/s that never fully
        // died out - small, but the player correctly read continued motion as "still sliding"
        // regardless of magnitude. Closing that gap needs something whose value does NOT collapse
        // to zero together with v - here, a direct velocity floor.
        //
        // docs §137 (task #60, Molokovoz01 standstill wheel-spin creep): gate widened from
        // "handbrake engaged" to "handbrake engaged OR no real drive intent" - a parked vehicle
        // with the handbrake OFF (just released the wheel, no throttle) hit the exact same
        // §125.2 structural gap and had nothing here to catch it, which is what the user saw as
        // small forward jerks in the wheel mesh while the chassis itself never moved.
        //
        // docs §137.2 (same task, real-terrain follow-up): a one-shot snap - fire once, the
        // instant speed first reads below kStandstillSnapSpeed - measured clean on the flat
        // menu-placeholder ground but did NOT fix the user's real report; re-tested on an actual
        // save (real terrain, not flat) it showed the exact same bursty spin, now with ALL FOUR
        // wheels agreeing in SIGN at each burst (not the alternating per-side pattern a pure
        // numerical artifact would give) - i.e. genuine short bursts of real rolling, the
        // textbook signature of §125.2's structural bias: a velocity-proportional damper can only
        // ever APPROACH v=0 under a persistent slope bias, so real terrain keeps re-feeding it
        // just enough speed to climb back OUT of the "already below threshold" gate before the
        // next check, and a one-shot snap that requires catching it already-still can lose that
        // race indefinitely. Fixed by LATCHING: once the entry gate below has caught the vehicle
        // genuinely stopped a single time, `restHeld` keeps the snap applying every frame
        // regardless of small re-excursions above the entry threshold - matching what a real
        // handbrake/parked vehicle actually does (holds firmly against a slope, not just "helps
        // until it happens to coast below some speed").
        //
        // docs §137.3 (same task, STILL not fixed after §137.2 - the real bug): the whole block
        // used to live AFTER the ApplyJoltToVehicleWheelModel call below, which reads each wheel
        // body's CURRENT rotation and writes it straight to the visible mesh
        // (wheel->SetRotationSelf, joltshadow.cpp's own §136 comment on that function). Zeroing
        // velocity there fixed the NEXT physics step's starting point but did nothing about THIS
        // step's rotation - by the time the snap ran, whatever the disturbance already did to the
        // body's orientation this step was already on screen. That is why §137.2 measured
        // perfectly clean (it was reading state AFTER its own correction) while the user kept
        // seeing the exact same jerk live. Moved the whole block to run BEFORE the writeback, and
        // added an explicit rotation freeze: the instant restHeld first latches, each wheel's
        // CURRENT orientation is captured into wheelLockedRot; every frame afterward, while held,
        // that captured orientation is force-written back onto the body - not derived from
        // wmSpinAngle or any per-axis reconstruction, just "this wheel is not going anywhere,
        // hold the pose it already had" - so the writeback below reads the ALREADY-corrected pose
        // instead of whatever the physics step's disturbance left it at. Velocity is still zeroed
        // too (keeps the body from fighting the rotation override every step).
        //
        // Snaps the WHOLE rigid group (chassis + all wheel bodies), not just the chassis: the
        // wheels are separate bodies, each still carrying their own small residual velocity,
        // rigidly coupled to the chassis by the SixDOF's fixed in-plane axes - zeroing the
        // chassis alone creates a one-frame constraint violation (chassis=0 vs wheels != 0) that
        // the solver promptly "corrects" by dragging the chassis back off zero to match its own
        // wheels (confirmed live: chassis-only snap held for a few seconds, then resumed creeping
        // at the original rate). Horizontal (X/Z) only for the chassis linear snap - Y is left
        // alone so suspension settling and legitimate falling are untouched.
        //
        // NOTE on testing this specific gate: a SCRIPTED testharness handbrake=True does not
        // reliably hold vehicle->m_bHandBrake true frame-to-frame here (read false almost
        // immediately in isolated testing, despite testharness's own telemetry showing it
        // correctly written true throughout) - the same class of native-input-polling race this
        // codebase already fights for throttle/brake (KeepThrottleHook) and steering
        // (testharness.cpp's direct m_curAngle write), just not yet given an equivalent bypass
        // for handbrake specifically. This does NOT affect real play: live logs from an actual
        // held handbrake button (docs/stage2-plan.md §2.9's original live capture) show
        // handBrake=1 continuously and consistently, because native polling then agrees with the
        // held key instead of fighting a scripted value it disagrees with. Re-verify via a live
        // handbrake hold, not the harness, if this needs re-checking later.
        constexpr float kStandstillNoThrottle = 0.02f;   // docs §137 - matches m_realThrottle's own units
        if (state.wheelBodyMode
                && (vehicle->m_bHandBrake || std::fabs(state.wmSpinThrottle) < kStandstillNoThrottle)) {
            JPH::PhysicsSystem* physicsRest = kraken::fix::jolt::GetPhysicsSystem();
            JPH::BodyInterface& biRest = physicsRest->GetBodyInterface();
            if (biRest.IsAdded(state.bodyId)) {
                constexpr float kStandstillSnapSpeed = 0.05f; // m/s; entry gate only - see §137.2
                const JPH::Vec3 vRest = biRest.GetLinearVelocity(state.bodyId);
                const JPH::Vec3 vHorizRest(vRest.GetX(), 0.0f, vRest.GetZ());
                const bool justLatched =
                    !state.restHeld && vHorizRest.LengthSq() < kStandstillSnapSpeed * kStandstillSnapSpeed;
                if (justLatched) {
                    state.restHeld = true;   // docs §137.2 - latches, see block comment above
                    // docs §137.3: capture NOW, before anything below moves it further.
                    state.wheelLockedRot.assign(state.wheelBodies.size(), JPH::Quat::sIdentity());
                    for (size_t i = 0; i < state.wheelBodies.size(); ++i) {
                        if (!state.wheelBodies[i].IsInvalid() && biRest.IsAdded(state.wheelBodies[i]))
                            state.wheelLockedRot[i] = biRest.GetRotation(state.wheelBodies[i]);
                    }
                }
                if (state.restHeld) {
                    biRest.SetLinearVelocity(state.bodyId, JPH::Vec3(0.0f, vRest.GetY(), 0.0f));
                    for (size_t i = 0; i < state.wheelBodies.size(); ++i) {
                        const JPH::BodyID& wheelIdRest = state.wheelBodies[i];
                        if (wheelIdRest.IsInvalid() || !biRest.IsAdded(wheelIdRest))
                            continue;
                        const JPH::Vec3 vw = biRest.GetLinearVelocity(wheelIdRest);
                        biRest.SetLinearVelocity(wheelIdRest, JPH::Vec3(0.0f, vw.GetY(), 0.0f));
                        biRest.SetAngularVelocity(wheelIdRest, JPH::Vec3::sZero());
                        // docs §137.5 (task #60, review follow-up): a STEERABLE wheel's rotation
                        // is already reassigned every PRE-step from live steering input
                        // (chassisRot*steerRot*twist, the steerMode>=2 path above) - full-freezing
                        // it here too would fight that assignment and visually stick the steer
                        // angle while parked (turn the wheel with the handbrake on, front wheels
                        // would not move). Velocity is still zeroed above either way (keeps the
                        // body from accumulating momentum against its own assignment); only the
                        // rotation override below is skipped, and only for steering wheels - the
                        // ones actually reported broken are the rear (non-steering) axle.
                        if (i < state.wheelSetup.size() && state.wheelSetup[i].steering)
                            continue;
                        // docs §137.3: the actual visible fix - force the body back to the pose
                        // it had when the latch first engaged, EVERY frame, before the writeback
                        // below ever reads it. See this block's own comment above for why a
                        // velocity-only correction (the §137/§137.2 shape) could not work.
                        if (i < state.wheelLockedRot.size())
                            biRest.SetRotation(wheelIdRest, state.wheelLockedRot[i], JPH::EActivation::DontActivate);
                    }
                }
            }
        } else {
            state.restHeld = false;   // docs §137.2 - real drive intent returned, release the latch
        }

        // docs §39.2, corrected by the Stage 2 writeback added above: wheelmodel mode has no
        // JPH::VehicleConstraint, so ApplyJoltToVehicle (which reads one) can't run for it - but
        // wheelBodyMode (wheelmodel=4) DOES have real per-wheel bodies, and now has its own
        // writeback (ApplyJoltToVehicleWheelModel) that reads those instead. Mode 2
        // (wheelModelMode but not wheelBodyMode) still has no writeback - forces land straight on
        // the chassis with no wheel bodies to sync, unchanged from before.
        //
        // docs §137.3: runs AFTER the standstill snap above now (used to run before it) - see
        // that block's own comment for why the ordering itself was the bug.
        if (allowApply) {
            if (state.wheelBodyMode)
                ApplyJoltToVehicleWheelModel(vehicle, state, label);
            else if (!state.wheelModelMode)
                ApplyJoltToVehicleProfiled(vehicle, state, label);
        }
        AccumulateForAutotune(vehicle, state);

        ++state.frameCounter;
        // docs §122.10: was a hardcoded 60 (~once/second at 60fps). Config-driven so a short
        // investigation run can sample the §66/§101-§103/§122 block fast enough to resolve
        // suspension-frequency events instead of aliasing them - see jolt_wm4_diag_interval.
        const uint64_t kLogIntervalFrames =
            std::max<uint64_t>(kraken::Config::Instance().jolt_wm4_diag_interval.value, 1);

        if (state.frameCounter % kLogIntervalFrames == 0) {
            LogDivergence(vehicle, state, label);
            LogWheelState(vehicle, state, label);
            LogRealOdeWheelState(vehicle, label);
            // docs §59/§63 (Этап 1, шаг 3): the harvest health line. Read here rather than in the
            // listener because here no worker thread is live - PhysicsSystem::Update has fully
            // returned by the time this pass runs, which is what makes plain (non-atomic) reads
            // of the listener's counters correct.
            //
            // maxNormalF and the bound both read 0.0 at step 3 BY DESIGN: no force is produced
            // yet. That is the step's whole point - prove the harvest carries real contacts before
            // anything acts on them - so a zero here is a pass, not a missing feature. The fields
            // are present now so the line's shape does not change when step 4 fills them.
            if (state.wheelBodyMode && state.stepListener != nullptr) {
                // bound = k_t * tau is the band's GEOMETRIC ceiling: the tyre sphere can only
                // penetrate tau before the rim takes over, so no band force above it is physically
                // reachable. A ratio over 1.0 therefore means the model is producing force the
                // geometry cannot justify, which is why it is called out in the line rather than
                // left for someone to divide by hand.
                const float tau0  = state.wheelSetup.empty() ? 0.0f : state.wheelSetup[0].tau;
                const float bound = std::max(kraken::Config::Instance().jolt_wm_tyre_stiffness.value, 1.0f) * tau0;
                const float ratio = bound > 0.0f ? state.stepListener->lastMaxNormalF / bound : 0.0f;
                const char* over  = (ratio > 1.0f) ? "  *** EXCEEDS GEOMETRIC BOUND ***" : "";
                LOG_INFO("docs §59/63: harvest (%s) wheelsWithContact=%u contactPoints=%u overflow=%u (of %u)"
                         " | survived=%u maxNormalF=%.0fN bound(k_t*tau)=%.0fN ratio=%.2f%s",
                    label, state.stepListener->lastWheelsWithContact, state.stepListener->lastContactPoints,
                    state.stepListener->lastOverflow, state.stepListener->wheelCount,
                    state.stepListener->lastSurvived, (double) state.stepListener->lastMaxNormalF,
                    (double) bound, (double) ratio, over);
                // docs §66: the per-wheel breakdown. Emitted alongside the summary rather than
                // instead of it - the summary says whether the band works, this says why not.
                for (uint32_t w = 0; w < state.stepListener->wheelCount; ++w) {
                    const auto& d = state.stepListener->diag[w];
                    const float tw = (w < state.wheelSetup.size()) ? state.wheelSetup[w].tau : 0.0f;
                    LOG_INFO("docs §66: tyre band (%s) w=%u fnGround=%.0fN fnTotal=%.0fN pen=%.5f penRaw=%.5f "
                             "tau=%.5f pen/tau=%.2f | recs tyre=%u rim=%u survived=%u distMax=%.2fR",
                        label, w, (double) d.fnGround, (double) d.fnTotal, (double) d.pen, (double) d.penRaw,
                        (double) tw, (double) (tw > 0.0f ? d.pen / tw : 0.0f),
                        d.recsTyre, d.recsRim, d.survived, (double) d.distMax);
                    // docs §124.12 debug (temporary, Ural01 hang): direct world-space position -
                    // the user's live visual observation was wheels visibly under the ground, so
                    // this checks it as a number instead of an eyeball impression.
                    LOG_INFO("docs §124.12: position (%s) w=%u wheelXYZ=(%.2f,%.3f,%.2f) chassisY=%.3f "
                             "wheelBelowChassis=%.3f chassisActive=%d chassisUpY=%.3f wheelAngVel=%.3f wheelLinVel=%.3f "
                             "userDataTagOk=%d userDataRaw=%llx",
                        label, w, (double) d.posX, (double) d.posY, (double) d.posZ, (double) d.chassisPosY,
                        (double) (d.chassisPosY - d.posY), (int) d.chassisActive, (double) d.chassisUpY,
                        (double) d.wheelAngVel, (double) d.wheelLinVel,
                        (int) d.userDataTagOk, (unsigned long long) d.userDataRaw);
                    // A SECOND line rather than more fields on the first one. The §66 format above
                    // was recovered character-for-character from the lost build, and it is one of
                    // the few places where the restored code can still be checked against that
                    // build's own logs; widening it would spend that check to save a line.
                    LOG_INFO("docs §66.2: band split (%s) w=%u distTyre=%.3fR distRim=%.3fR "
                             "penRawAny=%+.5f (m, incl. rejected) tau=%.5f | fn ground=%.0f obst=%.0f side=%.0f%s",
                        label, w, (double) d.distTyre, (double) d.distRim,
                        (double) d.penRawAny, (double) tw,
                        (double) d.fnGround, (double) d.fnObst, (double) d.fnSide,
                        d.asleep ? "  [ASLEEP - body not simulated, nothing measured this step]"
                          : (!d.penRawAnySet) ? "  [no tyre records]"
                          : (d.penRawAny > 0.0f) ? ""
                          : (d.distRim > 0.0f && d.distRim < 0.999f) ? "  [rim loaded, tyre clear]"
                          : "  [both clear of ground]");
                    // docs §125: standstill-creep investigation. v_par/v_lat are the GROUND slot's
                    // slip velocities (post spin, pre force); stick is the near-zero blend weight
                    // (1 at rest, 0 above wm_stick_speed); capPar is the old (pre-fix) longitudinal
                    // clamp, kept here to show how small it still is; damperPar is the new
                    // longitudinal stick force BEFORE the stick-blend; D is the friction-circle
                    // ceiling both draw from.
                    LOG_INFO("docs §125: standstill (%s) w=%u v_par=%+.4f v_lat=%+.4f stick=%.3f "
                             "capPar=%.1fN damperPar=%+.1fN D=%.1fN",
                        label, w, (double) d.dbg_v_par, (double) d.dbg_v_lat, (double) d.dbg_stick,
                        (double) d.dbg_capPar, (double) d.dbg_damperPar, (double) d.dbg_D);
                    // docs §124 step 3: new-path (WheelContactConstraint/AxisConstraintPart)
                    // normal force next to the old path's, for the step-3 verification gate -
                    // "matched within ~1%" on a static settle. Reads 0/0/0 whenever the flag is
                    // off (fnGroundNew etc. never written), which is self-explanatory rather than
                    // a state worth guarding against, same precedent as maxNormalF at step 1.
                    LOG_INFO("docs §124.7: new-path normal force (%s) w=%u ground=%.0fN(old %.0fN) "
                             "obst=%.0fN(old %.0fN) side=%.0fN(old %.0fN) reason=%d warmStartCalls=%u solveCalls=%u "
                             "pen=%.5f vn=%+.4f buildIslandsCalls=%u ptr=%p",
                        label, w, (double) d.fnGroundNew, (double) d.fnGround,
                        (double) d.fnObstNew, (double) d.fnObst,
                        (double) d.fnSideNew, (double) d.fnSide, d.dbgGroundReason,
                        d.dbgWarmStartCalls, d.dbgSolveCalls, (double) d.dbgPen, (double) d.dbgVn,
                        d.dbgBuildIslandsCalls, d.dbgThis);
                }
                // docs §67 (Этап 1, шаг 5): the spin DOF, per wheel and then per vehicle. These
                // two lines ARE the step's acceptance instrumentation - the plan's checks
                // ("GetTotalLambdaMotorRotation()[0]/dt tracks the commanded torque", "omega at
                // 100 km/h does not hit the ceiling") are the motorTq/cmdTq pair and the omega
                // field, so the gates are read off the log rather than judged by eye.
                if (state.VarSpin() != 0 && !state.wmSpinCmd.empty()) {
                    const float dtLog = std::max(state.lastStepDt, 1.0e-6f);
                    const JPH::Vec3 chassisAngVel =
                        kraken::fix::jolt::GetPhysicsSystem()->GetBodyInterface().GetAngularVelocity(state.bodyId);
                    for (size_t w = 0; w < state.wmSpinCmd.size() && w < state.wheelSetup.size(); ++w) {
                        JPH::Body* wb = kraken::fix::jolt::GetPhysicsSystem()
                            ->GetBodyLockInterfaceNoLock().TryGetBody(state.wheelBodies[w]);
                        const WheelSetup& ws = state.wheelSetup[w];
                        float omega = 0.0f;
                        if (wb != nullptr) {
                            const JPH::Vec3 axleWorld = (wb->GetRotation() * ws.axleLocal).Normalized();
                            omega = ws.spinSign * (wb->GetAngularVelocity() - chassisAngVel).Dot(axleWorld);
                        }
                        // SIGNED, and deliberately not |motorTq|: when its magnitude equals cmdTq
                        // the motor is saturated, and comparing the two is the whole point.
                        const JPH::SixDOFConstraint* sc =
                            static_cast<const JPH::SixDOFConstraint*>(state.wheelConstraints[w]);
                        const float motorTq = (sc != nullptr)
                            ? sc->GetTotalLambdaMotorRotation().GetX() / dtLog : 0.0f;
                        const ShadowState::SpinCmd& cmdLog = state.wmSpinCmd[w];
                        const float spinAngleLog = (w < state.wmSpinAngle.size()) ? state.wmSpinAngle[w] : 0.0f;
                        LOG_INFO("docs §67: spin (%s) w=%zu driven=%d omega=%.2f rad/s surfaceSpeed=%.2f m/s "
                                 "spinSign=%+.0f motorTq=%.0fNm cmdTq=%.0fNm",
                            label, w, ws.driven ? 1 : 0, (double) omega, (double) (omega * ws.radius),
                            (double) ws.spinSign, (double) motorTq, (double) state.wmSpinCmd[w].limit);
                        // docs §65.2: does the READ-BACK omega (used to accumulate wmSpinAngle, the
                        // manual twist's own input) agree in sign with the COMMANDED target (the
                        // value that ALSO drives non-steerable wheels, where the visual result is
                        // confirmed correct) for a STEERABLE wheel specifically? If they disagree,
                        // the twist's input is corrupted for kinematically-overridden bodies and no
                        // choice of sign on the twist itself can fix it - the accumulator needs a
                        // different source. steerable=1 marks the wheels actually running the
                        // manual chassisRot*steerRot*twist path this frame (docs §65.3).
                        LOG_INFO("docs §65.2: spin-src (%s) w=%zu steerable=%d target=%+.3f omega_readback=%+.3f "
                                 "wmSpinAngle=%+.4f agree=%d",
                            label, w, cmdLog.steerable ? 1 : 0, (double) cmdLog.target, (double) omega,
                            (double) spinAngleLog,
                            (cmdLog.target == 0.0f || omega == 0.0f) ? -1
                                : (((cmdLog.target > 0.0f) == (omega > 0.0f)) ? 1 : 0));

                        // docs §122.15: does the wheel body hit its droop limit (or its motor's
                        // force limit) right where §122.12 measured it losing contact, or does it
                        // stay well inside both budgets and simply lag the ground? BuildWheelBodies
                        // gave the SixDOF TranslationZ axis a HARD stop at
                        // [-compressHeadroom, +droopHeadroom] and a motor force cap of
                        // 4*kSusp*range - both invented conventions (docs §94.2: the reference's
                        // own ODE suspension is a pure CFM/ERP soft constraint with NO stop
                        // whatsoever, so this pair of limits exists only on the port's side).
                        // Recomputed fresh each frame from live positions rather than cached at
                        // build time, and read back the same way §109's yaw term is - even the off
                        // arm reports what it actually measured, not a value it declined to act on.
                        if (wb != nullptr) {
                            const JPH::RMat44 chassisXformNow =
                                kraken::fix::jolt::GetPhysicsSystem()->GetBodyInterface().GetWorldTransform(state.bodyId);
                            const JPH::RVec3 worldAttachNow  = chassisXformNow * ws.attachPos;
                            const JPH::Vec3  worldSuspDirNow = (chassisXformNow.GetRotation() * ws.suspDir).Normalized();
                            const float suspNow   = (float) JPH::Vec3(wb->GetPosition() - worldAttachNow).Dot(worldSuspDirNow);
                            const float restLenW  = (w < state.wmRestLen.size()) ? state.wmRestLen[w] : 0.0f;
                            const float rangeW    = std::max(ws.maxLen - ws.minLen, 0.05f);
                            const float fracW     = std::clamp(kraken::Config::Instance().jolt_wm4_compress_fraction.value, 0.05f, 0.95f);
                            const float compressHeadroomW = fracW * rangeW;
                            const float droopHeadroomW    = rangeW - compressHeadroomW;
                            const float zNow       = suspNow - restLenW;   // + = drooped, - = compressed
                            const float forceLimitW = 4.0f * ws.kSusp * rangeW;
                            const JPH::Vec3 motorForce = (sc != nullptr)
                                ? sc->GetTotalLambdaMotorTranslation() / dtLog : JPH::Vec3::sZero();
                            LOG_INFO("docs §122.15: droop (%s) w=%zu z=%+.3fm [droop<=%+.3f compress>=%-.3f] "
                                     "atDroopLimit=%d | motorFz=%.0fN limit=%.0fN atForceLimit=%d",
                                label, w, (double) zNow, (double) droopHeadroomW, (double) -compressHeadroomW,
                                (zNow > droopHeadroomW - 0.005f) ? 1 : 0,
                                (double) motorForce.GetZ(), (double) forceLimitW,
                                (std::fabs(motorForce.GetZ()) > 0.95f * forceLimitW) ? 1 : 0);
                        }
                    }
                    // gear is POST-shift while rpm is PRE-shift - that ordering is what the
                    // gearbox already does and what the recovered log shows; printing a matched
                    // pair here would silently disagree with every trace parsed on the old one.
                    LOG_INFO("docs §67: drivetrain (%s) gear=%d rpm=%.0f driveTorque=%.0fNm perWheel=%.0fNm "
                             "driven=%u throttle=%.2f spin=%d | PRE-STEP brake=%.2f handBrake=%d",
                        label, state.wmGear, (double) state.wmEngineRpm, (double) state.wmDriveTorque,
                        (double) state.wmPerWheel, state.wmDrivenCount, (double) state.wmSpinThrottle,
                        (int) state.VarSpin(),
                        (double) state.wmSpinBrake, state.wmSpinHandBrake ? 1 : 0);
                    // docs §67.2: the RAW driver fields, because m_realThrottle turned out not to
                    // be a throttle. It reaches +-10.00 live while m_brake reads 0.01, so it is
                    // neither bounded to [-1,1] nor equal to any documented combination of the
                    // two - and clamping it, which is what mode 2 does, converts "the brake term
                    // is dominating" into "full throttle in whichever direction the sign happens
                    // to be this frame". A scalar integrator averages that away; a velocity servo
                    // executes it. Logged rather than guessed at: the decomposition decides what
                    // the motor should actually be commanded with.
                    LOG_INFO("docs §67.2: drive intent (%s) throttle=%.3f brake=%.3f realThrottle=%.3f "
                             "engineRpm=%.0f avgWheelAVel=%.2f handBrake=%d autoBrake=%d",
                        label, (double) vehicle->m_throttle, (double) vehicle->m_brake,
                        (double) vehicle->m_realThrottle, (double) vehicle->m_engineRpm,
                        (double) vehicle->m_averageWheelAVel,
                        vehicle->m_bHandBrake ? 1 : 0, vehicle->m_bAutoBrake ? 1 : 0);
                    // docs §67: bullet-hit investigation - PhysicObj::SetCorrectEnabledCellsCounter
                    // (called from both DisablePhysics and SetPositionSelf, disassembled by hand -
                    // no PDB match needed, both are named/offset-confirmed in the recovered
                    // PhysicObj.hpp) only re-registers this object into the landscape's collision-
                    // cell grid (m3d::Landscape::GetCollisionCellItem, walking its bounding box)
                    // when m_physicState has bit0 CLEAR and bit1 SET - otherwise it returns
                    // immediately, doing nothing. DisablePhysics clears bit0 every frame (that part
                    // is satisfied) but never touches bit1 - if bit1 is normally maintained by the
                    // native per-step ODE update this DisablePhysics'd vehicle no longer runs, its
                    // cell registration could go stale as the Jolt-driven position moves it away
                    // from the cells it was last registered in - and if the weapon hit-trace
                    // (ShellTraceLineCallback, queries this same cell grid, not raw ODE space)
                    // walks cells to find candidates, a vehicle stuck registered in its OLD cells
                    // would be invisible to a trace aimed at its CURRENT position. Logged, not
                    // assumed - the live value of bit1 is unknown until this line has run.
                    LOG_INFO("docs §67.3: cell reg (%s) m_physicState=0x%x (bit0=%d bit1=%d) "
                             "m_enabledCellsCount=%d m_bIsUpdatingByODE=%d m_bBodyEnabledLastFrame=%d",
                        label, (unsigned) vehicle->m_physicState,
                        vehicle->m_physicState & 1, (vehicle->m_physicState >> 1) & 1,
                        vehicle->m_enabledCellsCount, vehicle->m_bIsUpdatingByODE ? 1 : 0,
                        vehicle->m_bBodyEnabledLastFrame ? 1 : 0);
                    // docs §101: the ported §94 assists. downForce is printed both absolutely and
                    // as a fraction of the vehicle's own weight, because "60% of its weight at
                    // 15 m/s" is the claim that made this layer worth porting and it should be
                    // readable off the log rather than recomputed by hand.
                    const float wN = std::max(vehicle->GetMass(), 1.0f)
                                   * std::max(std::fabs(kraken::Config::Instance().gravity.value), 0.1f);
                    // docs §109: curAngle is the yaw assist's ONLY driver-facing input, and it is
                    // the reference's own field - so it is non-zero whenever the player steers,
                    // whatever wm4_steer is set to. Printed because "the assist did nothing" and
                    // "the driver never turned the wheel" look identical in every earlier log.
                    const hta::ai::Wheel* logFirstWheel = nullptr;
                    for (hta::ai::Wheel* w : state.wheelOrder) {
                        if (w != nullptr) { logFirstWheel = w; break; }
                    }
                    LOG_INFO("docs §101: assists (%s) on=%d yaw=%d horizVel=%.1f m/s downForce=%.0fN (%.0f%% of weight "
                             "%.0fN) yawTorque=%.0fNm raw=%.0fNm curAngle=%.4f rad | pressingForce=%.2f "
                             "driftCoeff=%.3f cabin=%.2f",
                        label, (int) state.VarAssists(), (int) state.VarAssistYaw(),
                        (double) state.wmAssistHorizVel, (double) state.wmAssistDownForce,
                        (double) (100.0f * std::fabs(state.wmAssistDownForce) / wN), (double) wN,
                        (double) state.wmAssistTorque, (double) state.wmAssistTorqueRaw,
                        (double) (logFirstWheel != nullptr ? logFirstWheel->m_curAngle : 0.0f),
                        (double) (vehicle->GetPrototypeInfo() ? vehicle->GetPrototypeInfo()->m_pressingForce : 0.0f),
                        (double) vehicle->m_driftCoeff, (double) vehicle->_GetCabinControlCoeff());
                    // docs §102: the speed governor. surfaceSpeed is the WHEEL SURFACE speed the
                    // reference compares against, not the chassis velocity - both are printed so
                    // the difference is visible rather than assumed, and so the units of
                    // GetMaxSpeed() can be read off a real run instead of guessed at.
                    LOG_INFO("docs §102: governor (%s) on=%d src=%s limit=%.1f surfaceSpeed=%.1f "
                             "chassisSpeed=%.1f cut=%d | brake=%.3f selfBrake=%.3f turboT=%.2f",
                        label, (int) state.VarGovernor(),
                        vehicle->m_bIsControlledByPlayer ? "player"
                            : ((int) vehicle->m_attackStatus == 1 ? "attack" : "cruising"),
                        (double) state.wmSpeedLimit, (double) state.wmSurfaceSpeed,
                        (double) kraken::fix::jolt::GetPhysicsSystem()->GetBodyInterface()
                                     .GetLinearVelocity(state.bodyId).Length(),
                        state.wmGoverned ? 1 : 0, (double) vehicle->m_brake,
                        (double) (vehicle->GetPrototypeInfo() ? vehicle->GetPrototypeInfo()->m_selfBrakingCoeff : 0.0f),
                        (double) vehicle->m_turboThrottleTime);
                    // docs §103: soil drag. Printed against the vehicle's weight for scale, since
                    // the whole reason this was nearly skipped is that §95.3 compared it against
                    // the wrong quantity and concluded 1.4%.
                    LOG_INFO("docs §103: soil drag (%s) on=%d totalN=%.0f (%.0f%% of weight) resistance=%.3f",
                        label, (int) state.VarSoilDrag(),
                        (double) state.wmSoilDragN, (double) (100.0f * state.wmSoilDragN / wN),
                        (double) state.wmSoilResistance);
                    // docs §122: the ported ODE body damping. linear/angular are read BACK off the
                    // chassis, so the off arm prints the as-built 0.05/0.00 rather than the values
                    // it would have written - "the lever is off" and "the lever wrote zeros" are
                    // different states and have to look different in the log. Reference values are
                    // printed alongside so a wrong ini value is visible without opening it.
                    LOG_INFO("docs §122: body damping (%s) on=%d bodies=%u linear=%.3f angular=%.3f "
                             "(ODE world: 0.100/0.300)",
                        label, (int) state.VarBodyDamping(), state.wmDampBodies,
                        (double) state.wmDampLinear, (double) state.wmDampAngular);
                }
                // docs §68 (Этап 1, шаг 6): steering, per wheel. Emitted for UNSTEERED wheels too,
                // and that is the point: their cap prints as FLT_MAX, which is how the log proves
                // their RotationZ motor was never configured. Suppressing the line for
                // steerable=0 would remove that check.
                if (state.VarSteer() != 0 && !state.wmSpinCmd.empty()) {
                    const float dtLog = std::max(state.lastStepDt, 1.0e-6f);
                    for (size_t w = 0; w < state.wmSpinCmd.size() && w < state.wheelSetup.size(); ++w) {
                        const WheelSetup& ws = state.wheelSetup[w];
                        const ShadowState::SpinCmd& cmd = state.wmSpinCmd[w];
                        JPH::SixDOFConstraint* sc =
                            static_cast<JPH::SixDOFConstraint*>(state.wheelConstraints[w]);
                        // The measured angle comes off the CONSTRAINT, then has steerSign removed
                        // again so it is comparable with the commanded value in game space. Miss
                        // that second multiply and err reads 2*angle instead of ~0 on a straight
                        // road - which looks like a tracking failure and is an instrument bug.
                        float measured = 0.0f;
                        if (sc != nullptr) {
                            JPH::Quat swing, twist;
                            sc->GetRotationInConstraintSpace().GetSwingTwist(swing, twist);
                            measured = ws.steerSign * 2.0f * std::atan2(swing.GetZ(), swing.GetW());
                        }
                        const float commanded = ws.steerSign * cmd.steerTarget;   // back to game space
                        const float steerTq = (sc != nullptr)
                            ? sc->GetTotalLambdaMotorRotation().GetZ() / dtLog : 0.0f;
                        // sat = 1.00 means the motor is on its cap and the wheel goes where the
                        // tyre pushes it, not where it was told. The lost build ran at sat=1.00 in
                        // 18 of 64 steerable frames with up to 36 deg of following error, so this
                        // is a number to watch, not a formality.
                        const float sat = (cmd.steerCap > 0.0f && cmd.steerCap < FLT_MAX)
                            ? std::fabs(steerTq) / cmd.steerCap : 0.0f;
                        LOG_INFO("docs §68: steer (%s) w=%zu steerable=%d commanded=%.3f rad measured=%.3f rad "
                                 "err=%.2f deg steerSign=%+.0f steerTq=%.0fNm cap=%.0fNm sat=%.2f",
                            label, w, cmd.steerable ? 1 : 0, (double) commanded, (double) measured,
                            (double) ((measured - commanded) * 57.29578f), (double) ws.steerSign,
                            (double) steerTq, (double) cmd.steerCap, (double) sat);
                        // docs §68.2: WHY the wheel is where it is. sat=1.00 alone cannot separate
                        // "an external torque is pinning the wheel against the stop and the motor
                        // cannot out-pull it" from "nothing is pushing and the motor simply is not
                        // commanding it back". limRot is GetTotalLambdaRotation()/dt: x is 0 while
                        // RotationX is free, y is the camber reaction, and z is the LIMIT reaction
                        // - the torque the mechanical stop itself is supplying. A large z means
                        // something really is driving the wheel into the stop, and the motor cap
                        // has to beat THAT number; a z near zero means the cap is not the problem
                        // and raising it would be tuning against the wrong quantity.
                        if (sc != nullptr) {
                            const JPH::Vec3 limRot = sc->GetTotalLambdaRotation() / dtLog;
                            const JPH::Vec3 wAng = (w < state.wheelBodies.size())
                                ? kraken::fix::jolt::GetPhysicsSystem()->GetBodyInterface()
                                      .GetAngularVelocity(state.wheelBodies[w])
                                : JPH::Vec3::sZero();
                            LOG_INFO("docs §68.2: steer budget (%s) w=%zu limRot=%.0f/%.0f/%.0f Nm "
                                     "atStop=%d |wheelAngVel|=%.1f rad/s",
                                label, w, (double) limRot.GetX(), (double) limRot.GetY(),
                                (double) limRot.GetZ(),
                                (std::fabs(measured) > kWheelSteeringLimitRadians - 0.005f) ? 1 : 0,
                                (double) wAng.Length());
                        }
                    }
                }
            }
        }
    }

    // docs §107: variant shadows - one ShadowState per wm4_variant_N line, all tracking the PLAYER
    // vehicle, all built and stepped in the same pass. Kept in their own vector rather than reusing
    // g_aiShadows because the AI ones track DIFFERENT vehicles and their selection logic
    // (InitAiShadowsIfNeeded) has nothing to do with this.
    static std::vector<ShadowState> g_variantShadows;
    static std::vector<std::string> g_variantLabels;
    static bool                     g_variantsInitialized = false;
    static hta::ai::Vehicle*        g_variantInitVehicle  = nullptr;

    // Parses one "k=v,k=v" line. Unknown keys are IGNORED WITH A WARNING rather than silently: a
    // typo'd lever would otherwise read as "this arm inherited the default", which is exactly the
    // kind of quiet nothing that makes an A/B report a false null.
    static void ParseVariantSpec(const std::string& spec, ShadowState::Variant& out, std::string& label) {
        out.set = true;
        size_t pos = 0;
        while (pos <= spec.size()) {
            const size_t comma = spec.find(',', pos);
            const std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                std::string k = item.substr(0, eq), v = item.substr(eq + 1);
                while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(k.begin());
                while (!k.empty() && (k.back()  == ' ' || k.back()  == '\t')) k.pop_back();
                const auto asU = [&v]() { return (uint32_t) std::strtoul(v.c_str(), nullptr, 10); };
                const auto asF = [&v]() { return (float) std::atof(v.c_str()); };
                if      (k == "label")         label            = v;
                else if (k == "spin")          out.spin         = asU();
                else if (k == "steer")         out.steer        = asU();
                else if (k == "steer_mode")    out.steerMode    = asU();
                else if (k == "assists")       out.assists      = asU();
                else if (k == "assist_yaw")    out.assistYaw    = asU();
                else if (k == "engine_brake")  out.engineBrake  = asU();
                else if (k == "engine_brake_scale") out.engineBrakeScale = asF();
                else if (k == "governor")      out.governor     = asU();
                else if (k == "soildrag")      out.soildrag     = asU();
                else if (k == "body_damping")  out.bodyDamping  = asU();
                else if (k == "damping_linear")  out.dampingLinear  = asF();
                else if (k == "damping_angular") out.dampingAngular = asF();
                else if (k == "steer_hz")      out.steerHz      = asF();
                else if (k == "steer_damping") out.steerDamping = asF();
                else if (k == "isolate")       out.isolate      = asU();
                else LOG_WARNING("docs §107: variant spec has unknown key '%s' - IGNORED. A typo here "
                                 "reads as 'this arm inherited the default' and would report a false null.",
                                 k.c_str());
            }
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
    }

    static void InitVariantShadowsIfNeeded(hta::ai::Vehicle* playerVehicle) {
        const auto& specs = kraken::Config::Instance().jolt_wm4_variants.value;
        if (g_variantsInitialized && playerVehicle == g_variantInitVehicle)
            return;
        g_variantsInitialized = true;
        g_variantInitVehicle  = playerVehicle;
        // docs §107.8: before dropping the old states, UNTAG their wheel bodies. Those bodies are
        // abandoned rather than destroyed (the same leak-forever policy as everywhere else here),
        // and their UserData still says 'WHL' with THIS harvest slot - so they keep writing
        // contacts into the buffer the NEW variant is about to read. BuildWheelBodies already does
        // exactly this on its own rebuild path; clearing the vector skipped it, and that is the
        // whole bug: §66.2 caught a contact 319 wheel-radii away with penRaw = -18.9 m.
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics != nullptr) {
            JPH::BodyInterface& bi = physics->GetBodyInterface();
            for (ShadowState& old : g_variantShadows)
                for (JPH::BodyID id : old.wheelBodies)
                    if (!id.IsInvalid())
                        bi.SetUserData(id, 0);
        }
        g_variantShadows.clear();
        g_variantLabels.clear();
        if (playerVehicle == nullptr || specs.empty())
            return;

        const size_t n = std::min<size_t>(specs.size(), kMaxVariantShadows);
        if (specs.size() > n)
            LOG_WARNING("docs §107: %zu variant specs given but only %zu are built - the rest are "
                        "DROPPED, not silently merged.", specs.size(), n);
        for (size_t i = 0; i < n; ++i) {
            ShadowState st;
            std::string label = "var" + std::to_string(i + 1);
            ParseVariantSpec(specs[i], st.var, label);
            st.var.familyIndex = (uint32_t) (i + 1);   // 0 belongs to the player shadow
            g_variantShadows.push_back(std::move(st));
            g_variantLabels.push_back(label);
            LOG_INFO("docs §107: variant %zu '%s' <- %s", i + 1, label.c_str(), specs[i].c_str());
        }
    }

    // Which player-vehicle "generation" (see UpdateOneVehiclePreStep/BuildShadow) g_aiShadows was
    // last selected for - re-scanning whenever this changes, see InitAiShadowsIfNeeded.
    static hta::ai::Vehicle* g_aiInitPlayerVehicle = nullptr;

    // Stage 3 (docs/jolt-integration-techanalysis.md Stage 3 section): selects up to `aiCount`
    // AI vehicles to also shadow/drive, via hta::ai::CServer::Instance()->m_pObjects (the
    // engine's global ObjContainer) and its updatingBegin()/updatingEnd() iteration +
    // Obj::cast<Vehicle>() filter - the same enumeration primitives already used by
    // fix::tactics.cpp/fix::complexschwarz.cpp, not a newly-invented mechanism.
    //
    // Deliberately simple for this increment, NOT the XML Class="JoltVehicle" per-prototype
    // opt-in the original design doc sketched (§2.6/§5): research into that precedent (the
    // "KVehicle" git branch) found it was never actually finished - the submodule pin doesn't
    // even contain the CLASS/END_CLASS macro it needs, the hook that would install it
    // (kraken::meta::Init) is never called from EntryPoint, and the one prototype-factory
    // callback that exists constructs the WRONG C++ type (base VehiclePrototypeInfo, not the
    // derived one), so the whole subclass is unreachable dead code as it stands on that
    // branch. Porting/fixing all of that blind was judged higher-risk than this session's
    // budget justified for a first Stage 3 increment - see the Stage 3 doc section for the
    // full writeup.
    //
    // Re-scans whenever the player's own vehicle changes (level reload, vehicle switch) -
    // the same signal UpdateOneVehiclePreStep already uses for the player path. This matters in
    // practice: the very first opportunity to scan lands on testharness's autoload sequence's
    // placeholder pre-autoload map (a near-empty default level with no AI traffic) before the
    // real save loads - scanning once and latching "0 AI vehicles found" forever would
    // silently disable Stage 3 for the entire session. Still doesn't re-scan mid-level (a
    // vehicle destroyed/spawned after selection isn't picked up until the next vehicle swap).
    static void InitAiShadowsIfNeeded(hta::ai::Vehicle* playerVehicle, uint32_t aiCount) {
        if (aiCount == 0)
            return;
        if (g_aiShadowsInitialized && playerVehicle == g_aiInitPlayerVehicle)
            return;

        hta::ai::CServer* server = hta::ai::CServer::Instance();
        if (server == nullptr || server->m_pObjects == nullptr)
            return; // level not loaded yet - retry next frame

        g_aiShadowsInitialized  = true;
        g_aiInitPlayerVehicle   = playerVehicle;
        g_aiTargets.clear();
        // docs §107.8: the SAME hazard the variant path had, and this one predates it. The comment
        // below was right that the bodies are abandoned rather than destroyed - but abandoned
        // wheel bodies keep their 'WHL' UserData with THIS harvest slot, so they go on writing
        // contacts into a buffer that the rebuilt shadow then reads. Found via the variants, where
        // §66.2 caught a contact 319 wheel-radii away with penRaw = -18.9 m; here it is by
        // inspection, not by measurement - the mechanism is identical but this path has not been
        // observed failing, because it needs mode 4 plus an AI-shadow rebuild.
        {
            JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
            if (physics != nullptr) {
                JPH::BodyInterface& bi = physics->GetBodyInterface();
                for (ShadowState& old : g_aiShadows)
                    for (JPH::BodyID id : old.wheelBodies)
                        if (!id.IsInvalid())
                            bi.SetUserData(id, 0);
            }
        }
        g_aiShadows.clear(); // drops OUR tracking only - any already-built Jolt bodies are abandoned, not destroyed (same leak-forever policy as BuildShadow)

        hta::ai::ObjContainer* objects = server->m_pObjects;
        uint32_t found = 0;
        for (hta::ai::ObjContainer::iterator it = objects->updatingBegin(); it != objects->updatingEnd() && found < aiCount; ++it) {
            hta::ai::Vehicle* vehicle = (*it)->cast<hta::ai::Vehicle>();
            if (vehicle == nullptr || vehicle == playerVehicle)
                continue;
            g_aiTargets.push_back(vehicle);
            g_aiShadows.push_back(ShadowState{});
            ++found;
        }

        LOG_INFO("Stage 3: selected %u AI vehicle(s) for Jolt shadow (requested %u)", found, aiCount);
    }

    // Stage 3 crash fix (docs/stage3-plan.md §2 Шаг 5): g_aiTargets is "fixed at selection time"
    // (see its own comment) and dereferenced every frame via UpdateOneVehiclePreStep/BuildShadow -
    // unlike g_vehicleMirrors, which re-scans fresh every frame specifically so a vehicle
    // destroyed since last frame is dropped instead of dereferenced (see VehicleMirrorEntry's own
    // comment: "never dereferences a stale Vehicle* without a fresh scan confirming it's still
    // live"). That protection was never applied here. Confirmed live via a crash: an AI vehicle
    // destroyed mid-session (combat - the immediately-preceding log is a schwarz/economy
    // recalculation, and two AI shadows in a row logged "a wheel present at build time is now
    // gone/replaced" right before the fault) left a dangling g_aiTargets entry; BuildShadow then
    // read through it and hit an access violation at address 0. One fresh scan per frame, same
    // enumeration MirrorOtherVehicles already uses - O(liveObjects) + O(aiCount) std::find checks,
    // cheap next to the per-frame cost MirrorOtherVehicles already pays scanning the same list.
    static std::vector<hta::ai::Vehicle*> GetCurrentlyLiveVehicles() {
        std::vector<hta::ai::Vehicle*> live;
        hta::ai::CServer* server = hta::ai::CServer::Instance();
        if (server == nullptr || server->m_pObjects == nullptr)
            return live;
        hta::ai::ObjContainer* objects = server->m_pObjects;
        for (hta::ai::ObjContainer::iterator it = objects->updatingBegin(); it != objects->updatingEnd(); ++it) {
            hta::ai::Vehicle* vehicle = (*it)->cast<hta::ai::Vehicle>();
            if (vehicle != nullptr)
                live.push_back(vehicle);
        }
        return live;
    }

    // "Other vehicles" mirroring (docs §22.6/§22.11) - see g_vehicleMirrors' own comment above
    // for the why. Called once per real frame, only when the shared PhysicsSystem is actually
    // about to step (same "anyLive" gate UpdateShadow already uses for StepPhysicsProfiled) -
    // no point syncing kinematic mirrors into a world that isn't being simulated this frame.
    //
    // Chassis-only approximation, same box-from-prototype-mass-size shortcut BuildShadow already
    // uses for the Jolt-shadowed vehicle itself (docs' own comment there) - not the real
    // multi-part ODE collision. Good enough to stop a Jolt-driven vehicle from passing straight
    // through another vehicle; not a faithful reproduction of per-part collision.
    static void MirrorOtherVehicles(hta::ai::Vehicle* playerVehicle, float elapsedTime) {
        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return;
        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();

        hta::ai::CServer* server = hta::ai::CServer::Instance();
        if (server == nullptr || server->m_pObjects == nullptr)
            return;

        // Fresh scan every frame - see g_vehicleMirrors' comment on why this must never reuse a
        // pointer across frames without reconfirming it's still enumerated.
        std::vector<hta::ai::Vehicle*> currentOthers;
        hta::ai::ObjContainer* objects = server->m_pObjects;
        for (hta::ai::ObjContainer::iterator it = objects->updatingBegin(); it != objects->updatingEnd(); ++it) {
            hta::ai::Vehicle* vehicle = (*it)->cast<hta::ai::Vehicle>();
            if (vehicle == nullptr || vehicle == playerVehicle)
                continue;
            if (std::find(g_aiTargets.begin(), g_aiTargets.end(), vehicle) != g_aiTargets.end())
                continue; // already fully Jolt-shadowed (Stage 3) - don't also give it a mirror body
            currentOthers.push_back(vehicle);
        }

        // Drop mirrors for vehicles no longer present (destroyed, or promoted to a full AI
        // shadow target above) - compares pointer VALUES only, never dereferences the removed one.
        for (size_t i = 0; i < g_vehicleMirrors.size(); ) {
            bool stillPresent = std::find(currentOthers.begin(), currentOthers.end(), g_vehicleMirrors[i].vehicle) != currentOthers.end();
            if (stillPresent) {
                ++i;
                continue;
            }
            bodyInterface.RemoveBody(g_vehicleMirrors[i].bodyId);
            bodyInterface.DestroyBody(g_vehicleMirrors[i].bodyId);
            g_vehicleMirrors[i] = g_vehicleMirrors.back();
            g_vehicleMirrors.pop_back();
        }

        for (hta::ai::Vehicle* vehicle : currentOthers) {
            auto existing = std::find_if(g_vehicleMirrors.begin(), g_vehicleMirrors.end(),
                [vehicle](const VehicleMirrorEntry& e) { return e.vehicle == vehicle; });

            const hta::CVector    pos = vehicle->GetPosition();
            const hta::Quaternion rot = vehicle->GetRotation();
            const JPH::RVec3 joltPos(pos.x, pos.y, pos.z);
            const JPH::Quat  joltRot(rot.x, rot.y, rot.z, rot.w);

            if (existing != g_vehicleMirrors.end()) {
                bodyInterface.MoveKinematic(existing->bodyId, joltPos, joltRot, elapsedTime);
                continue;
            }

            // docs §23.13: real compound chassis shape (§23.10) instead of a single bounding
            // box - the same shape the player/Stage-3-shadowed vehicles use, extended to every
            // background AI vehicle now that the extraction technique exists. Only affects the
            // COLLISION PROFILE other vehicles present (a ram now feels the real hull shape,
            // not its bounding box); the pushback mechanism (§23.11) is unaffected either way -
            // it keys off Body::GetMotionType(), not shape. No OffsetCenterOfMassShapeSettings
            // wrapper here unlike BuildShadow's dynamic bodies - a Kinematic body's own mass/
            // inertia never enters the solve (MoveKinematic just sets its transform directly
            // every frame), so there's no rotational-dynamics reason to care where its center
            // of mass sits; the compound shape's local origin already sits exactly at
            // vehicle->GetPosition(), which is exactly where this body is placed below.
            const hta::ai::VehiclePrototypeInfo* protoInfo = vehicle->GetPrototypeInfo();
            const hta::CVector massSize = protoInfo != nullptr ? protoInfo->m_massSize : hta::CVector(2.0f, 1.0f, 4.0f);
            const JPH::Vec3 halfExtents(
                std::max(massSize.x * 0.5f, 0.1f),
                std::max(massSize.y * 0.5f, 0.1f),
                std::max(massSize.z * 0.5f, 0.1f));

            JPH::RefConst<JPH::Shape> mirrorShape = BuildChassisCompoundShape(vehicle, pos, rot);
            if (mirrorShape == nullptr)
                mirrorShape = new JPH::BoxShape(halfExtents);

            JPH::BodyCreationSettings bodySettings(
                mirrorShape.GetPtr(), joltPos, joltRot,
                JPH::EMotionType::Kinematic, kMovingLayer);

            JPH::Body* body = bodyInterface.CreateBody(bodySettings);
            if (body == nullptr) {
                LOG_ERROR("Jolt: other-vehicle mirror body creation failed (out of bodies?)");
                continue;
            }
            // docs §23.11: lets VehiclePushbackContactListener map a contact straight back to
            // the mirrored hta::ai::Vehicle* without a per-frame g_vehicleMirrors scan.
            body->SetUserData(reinterpret_cast<uint64_t>(vehicle));
            g_vehicleMirrors.push_back(VehicleMirrorEntry{vehicle, body->GetID()});
            bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);
        }
    }

    // ------------------------------------------------------------------------------------------
    // In-process suspension/friction autotuner (docs/jolt-integration-techanalysis.md §8.5/§16/§20.2/§20.3).
    //
    // Gated by [jolt_harness] autotune=1 - requires testharness=1 too (reuses its trigger.txt/
    // scenario.csv/output_<token>.done file protocol wholesale rather than inventing a second
    // scripted-replay mechanism) and apply=0 (Jolt must never actually drive the real vehicle
    // during a tuning run - if it did, the ODE "ground truth" the score is measured against
    // would already be corrupted by Jolt's own previous-trial output).
    //
    // Runs a standard Nelder-Mead simplex search over the 4 TuningParams fields (n=4, n+1=5
    // vertices) rather than the coordinate descent §8.5 originally called for: §16.5/§20.2 found
    // coordinate descent converges to a much better point on the fixed physics-step code (score
    // 29.53 -> 5.08) but still exhibits a sharp per-axis "cliff" (a neighboring probe on the same
    // axis, same direction, jumping the score straight back up from 5.08 to 37.53) - a method that
    // can move diagonally through parameter space, rather than one axis at a time, is expected to
    // ride that cliff more smoothly. Reflection/expansion/contraction/shrink coefficients are the
    // textbook defaults (alpha=1, gamma=2, rho=0.5, sigma=0.5) - no reason yet to believe this
    // particular score landscape needs anything hand-tuned. Stops when the trial budget
    // (autotune_max_trials) is exhausted or the simplex has collapsed (AutoTuneSimplexConverged,
    // see its own comment for the exact threshold and why) - logged explicitly either way, per
    // §8.5's requirement to never silently truncate the search.
    //
    // Drives testharness's OWN reset-to-spawn/scripted-input machinery one trial at a time by
    // writing a fresh trigger token and waiting for the matching .done file to appear - i.e. this
    // module IS the "external tool" testharness.cpp's own header comment says can drive it,
    // just implemented in-process instead of as a separate script.
    //
    // Nelder-Mead itself is NOT expressible as an ordinary blocking loop here: each vertex
    // evaluation is asynchronous (a real scripted maneuver that plays out over several seconds of
    // game time, advanced one frame at a time by AutoTuneTick - see AutoTunePhase below), so the
    // algorithm is its OWN small state machine (AutoTuneStage) layered on top of the existing
    // per-trial state machine (AutoTunePhase), rather than a synchronous function that "just runs"
    // the simplex to completion. AutoTuneStage tracks WHAT KIND of point is currently being
    // evaluated (which of the 5 bootstrap vertices / the reflected point / the expanded point /
    // the contracted point / which of the 4 shrink vertices), so that AutoTuneRecordResult -
    // called once a trial's score is known, an indeterminate number of frames after it was
    // submitted - can resume exactly where the algorithm left off and branch according to the
    // textbook decision tree. Whichever leaf of that tree needs a fresh point to evaluate computes
    // it right there and stores it into g_autotune.candidate; AutoTuneStartNextTrial's only job is
    // to submit whatever candidate is already queued (plus the trial-budget check).
    enum class AutoTunePhase { Idle, AwaitingReset, Running };

    // Which kind of point the CURRENT (or about-to-be-submitted) trial is evaluating - i.e. where
    // in the Nelder-Mead decision tree AutoTuneRecordResult should resume once its score arrives.
    enum class AutoTuneStage {
        Bootstrap, // filling the initial simplex's 5 vertices in order (index 0 = unperturbed baseline, see AutoTuneInitialize)
        Reflect,   // evaluating x_r; its score decides Expand vs. plain accept vs. Contract
        Expand,    // evaluating x_e (only reached when x_r already beat the best vertex)
        Contract,  // evaluating x_c (outside or inside the simplex, per the standard rule - see AutoTuneRecordResult)
        Shrink,    // re-evaluating vertices 1..4 in order after shrinking the whole simplex toward the best vertex
    };

    struct AutoTuneState {
        bool             enabled     = false;
        bool             initialized = false;
        bool             failed      = false;
        std::filesystem::path baseDir;
        hta::CVector     spawnPos;
        hta::Quaternion  spawnRot;

        TuningParams     current;             // best vertex found so far over the WHOLE run (see AutoTuneRecordResult) - what AutoTuneFinish reports/leaves live
        TuningParams     candidate;           // params under evaluation THIS trial
        double           bestScore   = -1.0;  // -1 = nothing evaluated yet; otherwise mirrors `current`'s score, kept alongside it for the trial-result log line
        uint32_t         trialIndex  = 0;
        uint32_t         maxTrials   = 24;

        // The simplex itself: n+1=5 vertices in the 4D TuningParams space, with their scores.
        // Sorted ascending by score (index 0 = best, 4 = worst) at the start of every main-loop
        // iteration (AutoTuneBeginIteration) and stays sorted in between, since within an
        // iteration we only ever overwrite the worst vertex (index 4) or shrink every non-best
        // vertex uniformly toward the best one - neither can change the relative order of 0..3.
        TuningParams     simplex[5];
        double           simplexScore[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
        int              fillIndex   = 0;     // Bootstrap: next vertex (0..4) awaiting a score. Shrink: next vertex (1..4) awaiting a re-evaluated score. Unused otherwise.

        AutoTuneStage    stage       = AutoTuneStage::Bootstrap;
        TuningParams     centroid;             // mean of simplex[0..3] (all but the worst) - computed once per iteration by AutoTuneBeginIteration, read by whichever of Expand/Contract ends up following Reflect's result
        TuningParams     reflected;            // x_r itself - needed by Expand's formula (x_e = centroid + gamma*(x_r-centroid)) and by the plain-accept assignment
        double           reflectedScore = 0.0; // score(x_r) - needed by the Contract-vs-shrink decision (compared against score(x_c))

        AutoTunePhase    phase       = AutoTunePhase::Idle;
        std::string      currentToken;

        // Which vehicle's transform g_autotune.spawnPos/spawnRot (and, if ownsScenarioFile, the
        // "spawn" line already written into scenario.csv) were captured from - see
        // AutoTuneCheckForVehicleChange for why this needs tracking.
        hta::ai::Vehicle* initVehicle      = nullptr;
        bool               ownsScenarioFile = false; // did WE write scenario.csv (vs. a user/tool-provided one)?
    };
    static AutoTuneState g_autotune;

    static float* AxisValue(TuningParams& p, int idx) {
        switch (idx) {
            case 0: return &p.suspensionFrequency;
            case 1: return &p.suspensionDamping;
            case 2: return &p.frictionLongScale;
            default: return &p.frictionLatScale;
        }
    }
    // Read-only counterpart of the above, under a deliberately DIFFERENT name (not an overload) -
    // the simplex math below constantly reads ONE vertex (centroid/best/worst/other) while
    // writing a DIFFERENT one, and giving the read path its own name/return-by-value makes it
    // impossible to accidentally do pointer arithmetic on a `float*` where a `float` was meant.
    static float AxisGet(const TuningParams& p, int idx) {
        switch (idx) {
            case 0: return p.suspensionFrequency;
            case 1: return p.suspensionDamping;
            case 2: return p.frictionLongScale;
            default: return p.frictionLatScale;
        }
    }
    static const char* AxisName(int idx) {
        static const char* names[4] = {"susp_frequency", "susp_damping", "friction_long", "friction_lat"};
        return names[idx >= 0 && idx < 4 ? idx : 0];
    }
    // Must match the [jolt_harness] susp_frequency/susp_damping/friction_long/friction_lat
    // ConfigValue ranges in config.cpp - kept in sync by hand, there are only 4 of them.
    static const float kAxisMin[4] = {0.5f, 0.2f, 0.2f, 0.2f};
    static const float kAxisMax[4] = {2.0f, 3.0f, 3.0f, 3.0f};

    // Clamps every one of a candidate's 4 fields into kAxisMin/kAxisMax - called at EVERY one of
    // the 5 places a new candidate point is constructed (bootstrap vertex, reflect, expand,
    // contract, shrink vertex) so there's exactly one place to get this right instead of five.
    static void ClampTuningParams(TuningParams& p) {
        for (int a = 0; a < 4; ++a) {
            float* v = AxisValue(p, a);
            *v = std::clamp(*v, kAxisMin[a], kAxisMax[a]);
        }
    }

    // Initial simplex edge length per axis (bootstrap vertex a+1 = baseline with axis `a` pushed
    // out by this much) - reuses the exact step sizes the old coordinate descent probed with, so
    // the initial simplex covers the same neighborhood the previous search started from.
    static const float kInitialSimplexStep[4] = {0.1f, 0.2f, 0.15f, 0.15f};

    // Standard Nelder-Mead coefficients - see the AutoTuneStage/AutoTunePhase comment above for
    // why the textbook defaults are used as-is rather than something bespoke for this problem.
    static constexpr double kNelderMeadAlpha = 1.0; // reflection
    static constexpr double kNelderMeadGamma = 2.0; // expansion
    static constexpr double kNelderMeadRho   = 0.5; // contraction
    static constexpr double kNelderMeadSigma = 0.5; // shrink

    // Convergence threshold for AutoTuneBeginIteration's stopping check: the max, over every pair
    // of simplex vertices and every one of the 4 axes, of |coord_i - coord_j| / (kAxisMax-kAxisMin)
    // for that axis - i.e. Chebyshev/max-norm distance in axis-range-normalized space, not a
    // combined Euclidean norm across axes. Chosen because "стало ли расстояние между вершинами по
    // всем 4 осям малым" reads naturally as a per-axis maximum, and because it's the more
    // conservative of the two (a Chebyshev-converged simplex is also converged in the Euclidean
    // sense, not necessarily vice versa) - so a run stopped by this check has genuinely collapsed
    // on every axis, not just on average.
    //
    // 1e-3 means every axis has collapsed to within 0.1% of its FULL tunable range at once (e.g.
    // susp_frequency's ~7.7Hz range -> a ~0.008Hz spread) - roughly 40-70x tighter than the
    // corresponding kInitialSimplexStep started the search at, depending on the axis (each axis'
    // initial bootstrap step is itself only a few percent of that axis' range). That's snug enough
    // that stopping here means the search has genuinely bottomed out rather than merely slowed
    // down, while still being loose enough that ordinary float roundoff in the simplex arithmetic
    // can't trip it by accident. If a real run turns out to stop here well before the trial budget
    // with a visibly non-optimal result, loosen this; if it never fires before the budget runs out
    // even on an easy landscape, tighten it - there was no prior empirical data for THIS specific
    // search (Nelder-Mead is new to this codebase), so this is a considered starting guess, not a
    // measured value.
    static constexpr double kSimplexConvergenceTol = 1e-3;

    // Scoring weights (docs §8.4): position drift is the metric a human actually feels ("the car
    // ended up somewhere else"), so it dominates; velocity/angle drift are smaller correction
    // terms rather than ignored outright. Deliberately simple, logged once at search start so
    // this approximation is visible rather than a silent assumption.
    static constexpr double kAutotuneVelWeight   = 0.2;
    static constexpr double kAutotuneAngleWeight = 0.02; // angle is in degrees - numerically much larger than meters or m/s

    static double ComputeAutotuneScore(const DivergenceAccum& accum) {
        if (accum.samples == 0)
            return 1e9; // no samples captured this trial (e.g. vehicle lost) - treat as maximally bad, not as a free win
        const double posRmse   = std::sqrt(accum.sumSqPos   / (double) accum.samples);
        const double velRmse   = std::sqrt(accum.sumSqVel   / (double) accum.samples);
        const double angleRmse = std::sqrt(accum.sumSqAngle / (double) accum.samples);
        return posRmse + kAutotuneVelWeight * velRmse + kAutotuneAngleWeight * angleRmse;
    }

    // Writes testharness's scenario.csv IF one isn't already present, so a user-provided
    // maneuver (testharness's own established convention - "external tool provides scenario.csv")
    // is always respected. The default maneuver touches all three regimes the tunable parameters
    // affect: straight acceleration (longitudinal friction, suspension squat), acceleration while
    // turning (lateral friction, suspension roll), then hard braking to a stop (longitudinal
    // friction + suspension dive) - see docs §16 for the exact numbers and rationale.
    static bool EnsureDefaultScenario(const std::filesystem::path& scenarioPath, const hta::CVector& spawnPos, const hta::Quaternion& spawnRot) {
        std::error_code ec;
        if (std::filesystem::exists(scenarioPath, ec))
            return true;

        std::ofstream out(scenarioPath, std::ios::trunc);
        if (!out.is_open())
            return false;

        out << "spawn," << spawnPos.x << ',' << spawnPos.y << ',' << spawnPos.z << ','
            << spawnRot.x << ',' << spawnRot.y << ',' << spawnRot.z << ',' << spawnRot.w << "\n";
        out << "0.0,0.0,0.0,0.0,0\n";   // stand still
        out << "0.2,1.0,0.0,0.0,0\n";   // accelerate straight
        out << "3.0,1.0,0.0,0.0,0\n";
        out << "3.2,1.0,0.5,0.0,0\n";   // accelerate + turn
        out << "5.0,1.0,0.5,0.0,0\n";
        out << "5.2,0.0,0.0,1.0,0\n";   // brake straight
        out << "7.0,0.0,0.0,1.0,0\n";
        out << "7.2,0.0,0.0,1.0,1\n";   // brake + handbrake to a stop
        out << "9.0,0.0,0.0,1.0,1\n";
        return true;
    }

    static void AutoTuneWriteTrigger(const std::string& token) {
        std::ofstream trigger(g_autotune.baseDir / "trigger.txt", std::ios::trunc);
        trigger << token;
    }

    static bool AutoTuneDoneFileExists(const std::string& token) {
        std::error_code ec;
        return std::filesystem::exists(g_autotune.baseDir / ("output_" + token + ".done"), ec);
    }

    static void AutoTuneFinish(const char* reason) {
        // The last trial evaluated before hitting the budget/convergence stop is NOT guaranteed to
        // be the best one found - unlike old coordinate descent (where the last ACCEPTED step was
        // usually recent, since it only ever moved when a probe improved things), Nelder-Mead's
        // last-evaluated point is very often a rejected reflect/expand/contract. Re-apply the best
        // vertex explicitly here rather than assuming the last SetTuningOverride call already left
        // it live.
        SetTuningOverride(g_autotune.current);

        LOG_INFO("Autotune: %s after %u trial(s). Best score=%.4f - susp_frequency=%.2f susp_damping=%.2f friction_long=%.2f friction_lat=%.2f",
            reason, g_autotune.trialIndex, g_autotune.bestScore,
            (double) g_autotune.current.suspensionFrequency, (double) g_autotune.current.suspensionDamping,
            (double) g_autotune.current.frictionLongScale, (double) g_autotune.current.frictionLatScale);
        LOG_INFO("Autotune: to keep these, set in kraken.ini [jolt_harness]: susp_frequency=%.2f susp_damping=%.2f friction_long=%.2f friction_lat=%.2f",
            (double) g_autotune.current.suspensionFrequency, (double) g_autotune.current.suspensionDamping,
            (double) g_autotune.current.frictionLongScale, (double) g_autotune.current.frictionLatScale);
        g_autotune.enabled     = false;
        g_autotuneAccumulating = false;
        // g_activeTuning/g_tuningGeneration now hold the best-found candidate (forced above) - a
        // human can keep driving with it live for the rest of the session without touching kraken.ini.
    }

    // See kSimplexConvergenceTol's comment for what this measures and why that threshold. Defined
    // before AutoTuneBeginIteration (which calls it) since there's no header forward-declaration
    // for these file-static helpers.
    static bool AutoTuneSimplexConverged() {
        double maxRelDist = 0.0;
        for (int i = 0; i < 5; ++i) {
            for (int j = i + 1; j < 5; ++j) {
                for (int a = 0; a < 4; ++a) {
                    const double range = (double) (kAxisMax[a] - kAxisMin[a]);
                    const double d = std::fabs((double) AxisGet(g_autotune.simplex[i], a) - (double) AxisGet(g_autotune.simplex[j], a)) / range;
                    maxRelDist = std::max(maxRelDist, d);
                }
            }
        }
        return maxRelDist < kSimplexConvergenceTol;
    }

    // Sorts the simplex ascending by score, checks for convergence (finishing the search early if
    // so), and - if not converged - computes this iteration's centroid + reflected point and
    // queues the reflected point as the next candidate. Called whenever a new main-loop iteration
    // is about to start: right after the initial 5-vertex bootstrap fills in, after every accepted
    // reflect/expand/contract, and after a completed 4-vertex shrink pass.
    static void AutoTuneBeginIteration() {
        // Insertion sort over 5 elements - the simplex is already "almost sorted" coming in (only
        // the worst vertex was replaced, or all 4 non-best vertices moved together), so this is
        // more than fast enough without pulling in a library sort for 5 elements.
        for (int i = 1; i < 5; ++i) {
            int j = i;
            while (j > 0 && g_autotune.simplexScore[j] < g_autotune.simplexScore[j - 1]) {
                const double scoreTmp = g_autotune.simplexScore[j];
                g_autotune.simplexScore[j]     = g_autotune.simplexScore[j - 1];
                g_autotune.simplexScore[j - 1] = scoreTmp;
                const TuningParams paramsTmp = g_autotune.simplex[j];
                g_autotune.simplex[j]     = g_autotune.simplex[j - 1];
                g_autotune.simplex[j - 1] = paramsTmp;
                --j;
            }
        }

        if (AutoTuneSimplexConverged()) {
            AutoTuneFinish("simplex converged (every vertex agrees to within tolerance on every axis), stopping early");
            return;
        }

        // Centroid of every vertex EXCEPT the worst (index 4, after the sort above).
        TuningParams centroid = g_autotune.simplex[0];
        for (int a = 0; a < 4; ++a) {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) sum += AxisGet(g_autotune.simplex[i], a);
            *AxisValue(centroid, a) = sum / 4.0f;
        }
        g_autotune.centroid = centroid;

        TuningParams reflected = centroid;
        for (int a = 0; a < 4; ++a) {
            const float c = AxisGet(centroid, a);
            const float w = AxisGet(g_autotune.simplex[4], a); // worst vertex
            *AxisValue(reflected, a) = c + (float) kNelderMeadAlpha * (c - w);
        }
        ClampTuningParams(reflected);
        g_autotune.reflected = reflected;

        g_autotune.stage     = AutoTuneStage::Reflect;
        g_autotune.candidate = reflected;
    }

    // Submits whatever AutoTuneRecordResult (or AutoTuneInitialize, for the very first bootstrap
    // vertex) already decided should be evaluated next. Unlike the old coordinate descent version,
    // this function no longer computes the candidate itself - it only enforces the trial budget
    // and does the trigger-file bookkeeping; ALL of "what to evaluate next" lives in
    // AutoTuneRecordResult/AutoTuneBeginIteration now, since that decision depends on the score
    // that just came back, not on anything available at submit time.
    static void AutoTuneStartNextTrial() {
        if (!g_autotune.enabled)
            return; // AutoTuneFinish may already have fired earlier this same tick (e.g. simplex converged)
        if (g_autotune.trialIndex >= g_autotune.maxTrials) {
            AutoTuneFinish("trial budget exhausted, stopping");
            return;
        }

        ++g_autotune.trialIndex;
        g_autotune.currentToken = "autotune_" + std::to_string(g_autotune.trialIndex);
        g_autotune.phase        = AutoTunePhase::AwaitingReset;
        AutoTuneWriteTrigger(g_autotune.currentToken);

        char stageDesc[48];
        switch (g_autotune.stage) {
            case AutoTuneStage::Bootstrap:
                if (g_autotune.fillIndex == 0)
                    std::snprintf(stageDesc, sizeof(stageDesc), "bootstrap 1/5 (baseline)");
                else
                    std::snprintf(stageDesc, sizeof(stageDesc), "bootstrap %d/5 (%s)", g_autotune.fillIndex + 1, AxisName(g_autotune.fillIndex - 1));
                break;
            case AutoTuneStage::Reflect:
                std::snprintf(stageDesc, sizeof(stageDesc), "reflect");
                break;
            case AutoTuneStage::Expand:
                std::snprintf(stageDesc, sizeof(stageDesc), "expand");
                break;
            case AutoTuneStage::Contract:
                std::snprintf(stageDesc, sizeof(stageDesc), "contract");
                break;
            case AutoTuneStage::Shrink:
                std::snprintf(stageDesc, sizeof(stageDesc), "shrink %d/4", g_autotune.fillIndex);
                break;
        }

        LOG_INFO("Autotune trial %u/%u starting (%s): susp_mult=%.2fx/damp=%.2f friction=%.2f/%.2f",
            g_autotune.trialIndex, g_autotune.maxTrials, stageDesc,
            (double) g_autotune.candidate.suspensionFrequency, (double) g_autotune.candidate.suspensionDamping,
            (double) g_autotune.candidate.frictionLongScale, (double) g_autotune.candidate.frictionLatScale);
    }

    static void AutoTuneRecordResult(double score) {
        LOG_INFO("Autotune trial %u result: score=%.4f (best so far=%.4f)",
            g_autotune.trialIndex, score, g_autotune.bestScore < 0.0 ? score : std::min(score, g_autotune.bestScore));

        if (g_autotune.bestScore < 0.0 || score < g_autotune.bestScore) {
            // Best single point evaluated so far over the WHOLE run. This always ends up equal to
            // simplex[0] once the simplex is next sorted - Nelder-Mead's own accept/reject rule
            // can never discard a point that beats the incumbent best (a point that beats the best
            // vertex also automatically beats "min(reflected, worst)"/"second-worst", i.e. every
            // threshold this code ever rejects a candidate against - so it's always kept). Tracked
            // incrementally here anyway so AutoTuneFinish always has a sane value to report even if
            // the run stops mid-bootstrap or mid-iteration, before the next sort would happen.
            g_autotune.bestScore = score;
            g_autotune.current   = g_autotune.candidate;
        }

        switch (g_autotune.stage) {
            case AutoTuneStage::Bootstrap: {
                g_autotune.simplexScore[g_autotune.fillIndex] = score;
                ++g_autotune.fillIndex;
                if (g_autotune.fillIndex < 5) {
                    g_autotune.candidate = g_autotune.simplex[g_autotune.fillIndex]; // next bootstrap vertex - already clamped when built in AutoTuneInitialize
                    return;
                }
                AutoTuneBeginIteration(); // all 5 vertices known - sort, check convergence, reflect
                return;
            }

            case AutoTuneStage::Reflect: {
                g_autotune.reflectedScore = score;

                if (score < g_autotune.simplexScore[0]) {
                    // Better than the current best vertex - worth trying to push further in the
                    // same direction.
                    TuningParams expanded = g_autotune.centroid;
                    for (int a = 0; a < 4; ++a) {
                        const float c = AxisGet(g_autotune.centroid, a);
                        const float r = AxisGet(g_autotune.reflected, a);
                        *AxisValue(expanded, a) = c + (float) kNelderMeadGamma * (r - c);
                    }
                    ClampTuningParams(expanded);
                    g_autotune.candidate = expanded;
                    g_autotune.stage     = AutoTuneStage::Expand;
                    return;
                }

                if (score < g_autotune.simplexScore[3]) {
                    // Not the best, but still better than the second-worst - plain reflection is
                    // accepted outright, no extra trial needed this iteration.
                    g_autotune.simplex[4]      = g_autotune.reflected;
                    g_autotune.simplexScore[4] = score;
                    AutoTuneBeginIteration();
                    return;
                }

                // Reflect didn't even beat the second-worst - contract toward the centroid instead.
                // "Outside" (blend toward x_r) if x_r at least beat the worst vertex - reflecting
                // was a step in a useful direction, just not far enough; "inside" (blend toward the
                // worst vertex itself) if even that failed - reflecting made things worse, so pull
                // back toward the simplex's interior rather than past the centroid.
                const bool outside = score < g_autotune.simplexScore[4];
                TuningParams contracted = g_autotune.centroid;
                for (int a = 0; a < 4; ++a) {
                    const float c     = AxisGet(g_autotune.centroid, a);
                    const float other = outside ? AxisGet(g_autotune.reflected, a) : AxisGet(g_autotune.simplex[4], a);
                    *AxisValue(contracted, a) = c + (float) kNelderMeadRho * (other - c);
                }
                ClampTuningParams(contracted);
                g_autotune.candidate = contracted;
                g_autotune.stage     = AutoTuneStage::Contract;
                return;
            }

            case AutoTuneStage::Expand: {
                // Keep whichever of {reflected, expanded} scored better - expansion is only worth
                // committing to if it actually beat the plain reflection, not merely the vertex
                // being replaced.
                if (score < g_autotune.reflectedScore) {
                    g_autotune.simplex[4]      = g_autotune.candidate; // the expanded point
                    g_autotune.simplexScore[4] = score;
                } else {
                    g_autotune.simplex[4]      = g_autotune.reflected;
                    g_autotune.simplexScore[4] = g_autotune.reflectedScore;
                }
                AutoTuneBeginIteration();
                return;
            }

            case AutoTuneStage::Contract: {
                if (score < std::min(g_autotune.reflectedScore, g_autotune.simplexScore[4])) {
                    g_autotune.simplex[4]      = g_autotune.candidate; // the contracted point
                    g_autotune.simplexScore[4] = score;
                    AutoTuneBeginIteration();
                    return;
                }

                // Contraction didn't help either - shrink every vertex but the best toward it.
                for (int i = 1; i < 5; ++i) {
                    TuningParams shrunk = g_autotune.simplex[i];
                    for (int a = 0; a < 4; ++a) {
                        const float best = AxisGet(g_autotune.simplex[0], a);
                        const float old  = AxisGet(shrunk, a);
                        *AxisValue(shrunk, a) = best + (float) kNelderMeadSigma * (old - best);
                    }
                    ClampTuningParams(shrunk);
                    g_autotune.simplex[i] = shrunk;
                }
                g_autotune.fillIndex = 1;
                g_autotune.stage     = AutoTuneStage::Shrink;
                g_autotune.candidate = g_autotune.simplex[1];
                return;
            }

            case AutoTuneStage::Shrink: {
                g_autotune.simplexScore[g_autotune.fillIndex] = score;
                ++g_autotune.fillIndex;
                if (g_autotune.fillIndex <= 4) {
                    g_autotune.candidate = g_autotune.simplex[g_autotune.fillIndex];
                    return;
                }
                AutoTuneBeginIteration(); // all 4 shrunk vertices re-evaluated - sort, check convergence, reflect
                return;
            }
        }
    }

    static void AutoTuneInitialize() {
        const kraken::Config& config = kraken::Config::Instance();

        if (config.testharness.value == 0) {
            LOG_ERROR("Autotune requires [testharness] enabled=1 (it drives testharness's own scripted-replay file protocol) - disabling autotune");
            g_autotune.failed = true;
            return;
        }
        if (config.jolt_apply.value != 0) {
            LOG_ERROR("Autotune requires [jolt_harness] apply=0 - Jolt must not drive the real vehicle during tuning, or the ODE 'ground truth' the score is measured against would already be corrupted by Jolt's own output - disabling autotune");
            g_autotune.failed = true;
            return;
        }

        hta::ai::Vehicle* vehicle = GetPlayerVehicle();
        if (vehicle == nullptr)
            return; // no save loaded yet - retry next frame, same as BuildShadow's own not-ready handling

        g_autotune.baseDir = std::filesystem::path("./data/kraken_testharness");
        std::error_code ec;
        std::filesystem::create_directories(g_autotune.baseDir, ec);

        g_autotune.initVehicle = vehicle;
        g_autotune.spawnPos    = vehicle->GetPosition();
        g_autotune.spawnRot    = vehicle->GetRotation();

        const std::filesystem::path scenarioPath = g_autotune.baseDir / "scenario.csv";
        g_autotune.ownsScenarioFile = !std::filesystem::exists(scenarioPath, ec);
        if (!EnsureDefaultScenario(scenarioPath, g_autotune.spawnPos, g_autotune.spawnRot)) {
            LOG_ERROR("Autotune: failed to write default scenario.csv - disabling autotune");
            g_autotune.failed = true;
            return;
        }

        // Build the initial simplex: vertex 0 is the unperturbed baseline, vertices 1-4 are the
        // baseline with exactly one axis pushed out by kInitialSimplexStep (the same neighborhood
        // the old coordinate descent started its own search from) - a standard way to seed
        // Nelder-Mead when there's no better prior than "the current default is roughly sane".
        const TuningParams baseline = DefaultTuningParams();
        g_autotune.simplex[0] = baseline;
        for (int a = 0; a < 4; ++a) {
            TuningParams vertex = baseline;
            *AxisValue(vertex, a) += kInitialSimplexStep[a];
            ClampTuningParams(vertex);
            g_autotune.simplex[a + 1] = vertex;
        }
        for (int i = 0; i < 5; ++i) g_autotune.simplexScore[i] = 0.0; // placeholder - overwritten one at a time as bootstrap trials report in

        g_autotune.current     = baseline; // provisional - overwritten as soon as the first trial's score is known (see AutoTuneRecordResult)
        g_autotune.fillIndex   = 0;
        g_autotune.stage       = AutoTuneStage::Bootstrap;
        g_autotune.candidate   = g_autotune.simplex[0];
        g_autotune.maxTrials   = config.jolt_autotune_max_trials.value;
        g_autotune.trialIndex  = 0;
        g_autotune.bestScore   = -1.0;
        g_autotune.initialized = true;

        LOG_INFO("Autotune: starting Nelder-Mead simplex search, up to %u trial(s), baseline susp_mult=%.2fx/damp=%.2f friction=%.2f/%.2f, "
                 "score = pos_rmse + %.2f*vel_rmse + %.2f*angle_rmse_deg (approximate weights, see docs section 8.4/8.5)",
            g_autotune.maxTrials,
            (double) baseline.suspensionFrequency, (double) baseline.suspensionDamping,
            (double) baseline.frictionLongScale, (double) baseline.frictionLatScale,
            kAutotuneVelWeight, kAutotuneAngleWeight);
    }

    // Guards against the same race Stage 3's AI-vehicle selection already hit once (docs §14.2):
    // the FIRST opportunity to read the player vehicle can land on testharness's placeholder
    // pre-autoload map (autoload_save fires ~300 frames into boot, well after a vehicle already
    // exists on the default map), so an autotune search that started immediately would capture a
    // spawn transform and write scenario.csv for a vehicle that's about to be swapped out from
    // under it - every subsequent trial would then reset the REAL vehicle to a nonsensical,
    // unrelated spawn point, producing wildly inflated (meaningless) scores. Detected exactly the
    // same way Stage 3 detects it: the player vehicle POINTER changing underneath us, checked
    // every tick regardless of search phase.
    static void AutoTuneCheckForVehicleChange() {
        if (!g_autotune.initialized)
            return;

        hta::ai::Vehicle* current = GetPlayerVehicle();
        if (current == nullptr || current == g_autotune.initVehicle)
            return;

        // The search bookkeeping (trialIndex/bestScore/the whole simplex) is discarded and
        // restarted from scratch either way - whatever trial was in flight was measuring the
        // OLD vehicle and its score is no longer comparable to trials against the new one (and a
        // partially-filled simplex from the old vehicle has no valid meaning against the new
        // one's spawn point). AutoTuneInitialize rebuilds everything - simplex, fillIndex, stage,
        // trialIndex, bestScore - unconditionally on the next tick, same as it always has. Only
        // the scenario.csv FILE itself is conditionally touched: if we wrote it ourselves, its
        // "spawn" line was captured from the (possibly stale/placeholder) old vehicle and must be
        // regenerated; a user/tool-provided file's spawn line is just world coordinates and stays
        // valid regardless of which vehicle object it gets applied to, so it's left alone.
        if (g_autotune.ownsScenarioFile) {
            LOG_WARNING("Autotune: player vehicle changed mid-run (likely testharness autoload swapping in the real save) - "
                        "discarding the %u trial(s) run so far and restarting the search against the new vehicle's spawn point",
                g_autotune.trialIndex);
            std::error_code ec;
            std::filesystem::remove(g_autotune.baseDir / "scenario.csv", ec); // we wrote it, safe to regenerate from the new spawn
        } else {
            LOG_WARNING("Autotune: player vehicle changed mid-run - scenario.csv is user-provided so it's left untouched, "
                        "but restarting the search bookkeeping against the new vehicle (discarding %u trial(s) run so far)",
                g_autotune.trialIndex);
        }
        g_autotuneAccumulating = false;
        g_autotune.initialized = false;
        g_autotune.phase       = AutoTunePhase::Idle;
    }

    static void AutoTuneTick() {
        if (!g_autotune.enabled || g_autotune.failed)
            return;

        AutoTuneCheckForVehicleChange();

        if (!g_autotune.initialized) {
            AutoTuneInitialize();
            if (g_autotune.failed) { g_autotune.enabled = false; return; }
            if (!g_autotune.initialized) return;
            AutoTuneStartNextTrial();
            return;
        }

        switch (g_autotune.phase) {
            case AutoTunePhase::AwaitingReset:
                // Exactly one frame has passed since we wrote the trigger file. testharness's
                // own CollideScene hook runs earlier in the frame than joltshadow's StepScene
                // hook (see StepSceneHook's comment below) and has therefore already picked up
                // the new token and reset the vehicle to the spawn transform THIS frame - so it's
                // now safe to rebuild the shadow (BuildShadow reads vehicle->GetPosition() fresh)
                // and start accumulating divergence from here on.
                SetTuningOverride(g_autotune.candidate);
                g_autotuneAccum         = DivergenceAccum{};
                g_autotuneAccumulating  = true;
                g_autotune.phase        = AutoTunePhase::Running;
                break;

            case AutoTunePhase::Running:
                if (AutoTuneDoneFileExists(g_autotune.currentToken)) {
                    g_autotuneAccumulating = false;
                    double score = ComputeAutotuneScore(g_autotuneAccum);
                    AutoTuneRecordResult(score);
                    g_autotune.phase = AutoTunePhase::Idle;
                    AutoTuneStartNextTrial();
                }
                break;

            case AutoTunePhase::Idle:
                AutoTuneStartNextTrial();
                break;
        }
    }
    // ------------------------------------------------------------------------------------------

    // Hard upper bound on the number of AI shadows ever live at once - matches [jolt_harness]
    // ai_count's own ConfigValue range (source/config.cpp: `{"jolt_harness","ai_count", 0, true,
    // 0, 128}`), which clamps the ini value to 0..128 before InitAiShadowsIfNeeded ever selects
    // vehicles, so g_aiShadows/g_aiTargets never actually grow past this. Sized as a fixed stack
    // array (not a per-frame heap allocation - this whole refactor exists to keep this hot path
    // allocation-free) with a defensive runtime clamp below in case that config limit is ever
    // loosened without this array being resized to match. Raised from 16->128 per user request
    // ("убери ограничение ai_count") - nothing else in this file assumes <=16 (GroupIDs are plain
    // sequential uint32_t, g_aiShadows/g_aiTargets are already std::vector) so this is just two
    // small stack arrays growing (bool[128] + char[128][16], still trivial per-frame cost).
    static constexpr size_t kMaxAiShadowsPerFrame = 128;

    // Forward-declared: defined alongside VehiclePushbackContactListener (docs §23.11), well
    // after UpdateShadow in this file, but needs calling from UpdateShadow's Pass 2 below,
    // right after the one place PhysicsSystem::Update() actually runs for this frame.
    static void DrainPendingPushbacks();
    // docs §57: same forward-declaration reason as DrainPendingPushbacks above.
    static void DrainPendingBreaks();
    // docs §61: same forward-declaration reason as DrainPendingPushbacks above.
    static void DrainPendingEnables();

    // Three-pass per-frame update (docs/jolt-integration-techanalysis.md §18.1/§18.4): the
    // single JPH::PhysicsSystem is shared by every shadow vehicle, so it must be stepped exactly
    // ONCE per real frame regardless of how many shadows (0..17, player + up to 16 AI) are active
    // - not once per vehicle, which used to re-simulate the entire shared world up to 17x/frame
    // at ai_count=16.
    //   Pass 1 (pre-step): rebuild-on-swap + feed driver input for every candidate vehicle,
    //     recording which ones are actually live this frame (BuildShadow succeeded, now or
    //     previously).
    //   Pass 2: step physics exactly once, but only if at least one vehicle is live this frame -
    //     if none are (e.g. right after a level load, before any BuildShadow has succeeded yet),
    //     skip the step entirely, matching the old per-vehicle code's behavior for that edge case
    //     (it simply never called StepPhysics if BuildShadow kept failing) rather than stepping a
    //     not-yet-ready world.
    //   Pass 3 (post-step): apply/autotune-accumulate/periodic-log for every vehicle that was
    //     live this frame.
    static void UpdateShadow(float elapsedTime) {
        AutoTuneTick();

        const kraken::Config& config = kraken::Config::Instance();

        // docs §52: whole-UpdateShadow wall time = total per-frame Jolt-side cost. Captured here at
        // the top and accumulated just before JoltProfileFrameEnd (also at the bottom) so this
        // frame's jolt_total lands in the same interval bucket as its ode_step.
        const bool prof = config.testharness_perfmon.value != 0;
        const std::chrono::steady_clock::time_point joltT0 =
            prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        hta::ai::Vehicle* playerVehicle = GetPlayerVehicle();

        // Stage 3 requires player_only=0 - player_only=1 (the default) means "only the
        // player, full stop", so `ai` is deliberately inert unless that's turned off too.
        // Applying (vs. shadow-only logging) additionally still requires apply=1, same as the
        // player path. `ai` is a plain on/off switch (docs/stage3-plan.md: "все боты на Jolt
        // либо все на ODE", no partial counts) - on means every AI vehicle up to the engine's
        // own hard cap (kMaxAiShadowsPerFrame), off means none; InitAiShadowsIfNeeded's own
        // first-N scan then decides which ones actually get a slot if a level ever exceeds that
        // cap (never observed - busiest measured save had 74 of 128 slots).
        const uint32_t aiCount = (config.jolt_player_only.value == 0 && config.jolt_ai.value != 0)
            ? static_cast<uint32_t>(kMaxAiShadowsPerFrame) : 0;
        if (aiCount > 0)
            InitAiShadowsIfNeeded(playerVehicle, aiCount);

        assert(g_aiShadows.size() <= kMaxAiShadowsPerFrame);
        const size_t aiShadowCount = aiCount > 0 ? std::min(g_aiShadows.size(), kMaxAiShadowsPerFrame) : 0;

        bool playerLive = false;
        bool aiLive[kMaxAiShadowsPerFrame] = {};
        char aiLabels[kMaxAiShadowsPerFrame][16]; // filled in pass 1, reused as-is (same index) in pass 3

        // docs §107: variant shadows of the PLAYER vehicle. Selected before pass 1 so the family
        // flag is set on every member BEFORE any body is built - the collision group is chosen at
        // build time and cannot be changed retroactively without rebuilding.
        InitVariantShadowsIfNeeded(playerVehicle);
        const size_t variantCount = g_variantShadows.size();
        bool variantLive[kMaxVariantShadows] = {};
        // The player joins the family only if some variant actually needs it - an all-isolated
        // set leaves the player exactly as it was, which is what makes the §107.5 comparison mean
        // something.
        bool familyNeeded = false;
        for (size_t i = 0; i < variantCount; ++i)
            familyNeeded = familyNeeded || (g_variantShadows[i].var.isolate == 0);
        g_playerShadow.variantFamily = familyNeeded;

        // --- Pass 1 (pre-step) ---
        if (playerVehicle != nullptr)
            playerLive = UpdateOneVehiclePreStep(playerVehicle, g_playerShadow, "player", 0, elapsedTime);

        for (size_t i = 0; i < variantCount; ++i) {
            g_variantShadows[i].variantFamily = (g_variantShadows[i].var.isolate == 0);
            // The collision group is SHARED (kVariantGroupId, applied inside the build) while the
            // harvest slot must not be - so variants take slots from the top of the table, where
            // the AI shadows (1..aiCount, counting up) cannot reach them.
            const uint32_t slot = kMaxVehicleSlots - 1 - (uint32_t) i;
            variantLive[i] = UpdateOneVehiclePreStep(playerVehicle, g_variantShadows[i],
                                                     g_variantLabels[i].c_str(), slot, elapsedTime);
        }

        // See GetCurrentlyLiveVehicles' own comment: g_aiTargets can hold a pointer to a vehicle
        // destroyed since the last rescan (InitAiShadowsIfNeeded only rescans on a player vehicle
        // change, not on AI death) - confirmed to crash BuildShadow when dereferenced. Checked
        // once per frame here, not inside InitAiShadowsIfNeeded, because the whole point is
        // catching a death that happens BETWEEN rescans.
        const std::vector<hta::ai::Vehicle*> aiTargetsLiveNow =
            aiShadowCount > 0 ? GetCurrentlyLiveVehicles() : std::vector<hta::ai::Vehicle*>();

        for (size_t i = 0; i < aiShadowCount; ++i) {
            std::snprintf(aiLabels[i], sizeof(aiLabels[i]), "ai%zu", i);
            if (std::find(aiTargetsLiveNow.begin(), aiTargetsLiveNow.end(), g_aiTargets[i]) == aiTargetsLiveNow.end()) {
                aiLive[i] = false; // destroyed since selection - do not touch g_aiTargets[i] this frame
                continue;
            }
            // docs §37 item 3: player is always GroupID 0 (above); AI shadow slot i gets i+1 -
            // a stable, distinct GroupID per vehicle so GetWheelGroupFilter's shared table
            // only ever suppresses a vehicle's own chassis-vs-own-proxy pairs, never cross-
            // vehicle ones (see its comment).
            aiLive[i] = UpdateOneVehiclePreStep(g_aiTargets[i], g_aiShadows[i], aiLabels[i], static_cast<uint32_t>(i) + 1, elapsedTime);
        }

        // --- Pass 2: step the shared PhysicsSystem exactly once ---
        bool anyLive = playerLive;
        for (size_t i = 0; i < aiShadowCount; ++i)
            anyLive = anyLive || aiLive[i];
        for (size_t i = 0; i < variantCount; ++i)
            anyLive = anyLive || variantLive[i];
        if (anyLive) {
            if (prof) {
                const auto m0 = std::chrono::steady_clock::now();
                MirrorOtherVehicles(playerVehicle, elapsedTime);
                g_joltProfile.mirrorMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();
            } else {
                MirrorOtherVehicles(playerVehicle, elapsedTime);
            }
            StepPhysicsProfiled(elapsedTime);
            // docs §23.11: safe to drain here - PhysicsSystem::Update() (inside
            // StepPhysicsProfiled) has fully returned, so every worker thread that may have
            // queued a contact via VehiclePushbackContactListener during this step has
            // finished; back to single-threaded, safe to call into ODE (PhysicObj::AddImpulse
            // etc.) again.
            DrainPendingPushbacks();
            // docs §57: same safe window as DrainPendingPushbacks immediately above.
            DrainPendingBreaks();
            // docs §61: same safe window as DrainPendingPushbacks immediately above.
            DrainPendingEnables();
            // docs §54.4 (шаг -1F): same safe window - the step has fully returned, no worker job
            // can still hold a constraint, and we are single-threaded again.
            DrainPendingJoltDestroys();

            // docs §110 (шаг 7): are the legacy paths actually cold? Emitted HERE, once per frame
            // for the whole process, and NOT from the mode-4 diagnostic block where it started -
            // that block never runs in mode 2, which would have silenced the positive control and
            // left "mode 4 reads zero" resting on nothing. The counters are cumulative since
            // process start, which is what makes "still zero after a full drive" mean something.
            static uint64_t s_legacyLogFrame = 0;
            if (++s_legacyLogFrame % 60 == 0) {
                LOG_INFO("docs §110: legacy paths (cumulative) proxyBuilds=%llu proxyPastGuard=%llu "
                         "evalCollideShape=%llu stepCollideShape=%llu | mode=%d",
                    (unsigned long long) g_legacyPaths.proxyBuilds,
                    (unsigned long long) g_legacyPaths.proxyPassedGuard,
                    (unsigned long long) g_legacyPaths.evalCollideShape,
                    (unsigned long long) g_legacyPaths.stepCollideShape,
                    (int) kraken::Config::Instance().jolt_wheelmodel.value);
                // docs §112 (шаг 8): the sleep lever's effect, as a count rather than as a frame
                // time. "It got faster" has to be traceable to "N of M shadows stopped simulating",
                // otherwise the lever and the run-to-run noise are the same observation. Reported
                // per frame (the counters are reset here), not cumulative.
                LOG_INFO("docs §114: kinematic steer AABB shift max=%.9f m (at |steer|=%.4f rad) | "
                         "assignments=%llu maxSteerSeen=%.4f rad - a zero shift only means "
                         "something if maxSteerSeen is large",
                    (double) g_steerAabbShiftMax, (double) g_steerAabbShiftAt,
                    (unsigned long long) g_steerAssignments, (double) g_steerCmdMaxSeen);
                LOG_INFO("docs §112: shadow sleep on=%d slept=%llu awake=%llu of %llu | kept awake by: "
                         "throttle=%llu brake=%llu speed=%llu hand=%llu changed=%llu margin=%llu",
                    (int) kraken::Config::Instance().jolt_wm4_sleep.value,
                    (unsigned long long) g_sleepStats.sleptShadows,
                    (unsigned long long) g_sleepStats.awakeShadows,
                    (unsigned long long) (g_sleepStats.sleptShadows + g_sleepStats.awakeShadows),
                    (unsigned long long) g_sleepStats.rejThrottle,
                    (unsigned long long) g_sleepStats.rejBrake,
                    (unsigned long long) g_sleepStats.rejSpeed,
                    (unsigned long long) g_sleepStats.rejHand,
                    (unsigned long long) g_sleepStats.rejChanged,
                    (unsigned long long) g_sleepStats.rejMargin);
                g_sleepStats = SleepStats();
            }
        }

        // --- Pass 3 (post-step) ---
        if (playerLive) {
            const bool playerAllowApply = config.jolt_apply.value != 0
                && (config.jolt_player_only.value == 0 || playerVehicle->bIsControlledByPlayer());
            UpdateOneVehiclePostStep(playerVehicle, g_playerShadow, playerAllowApply, "player");
        }

        if (aiShadowCount > 0) {
            const bool aiAllowApply = config.jolt_apply.value != 0;
            for (size_t i = 0; i < aiShadowCount; ++i) {
                if (!aiLive[i])
                    continue;
                UpdateOneVehiclePostStep(g_aiTargets[i], g_aiShadows[i], aiAllowApply, aiLabels[i]);
            }
        }

        // docs §107: variants NEVER apply back into ODE, whatever jolt_apply says. Two shadows of
        // one vehicle both writing into it would be meaningless, and the whole point is that they
        // are passive comparisons - so the flag is hard-coded false here rather than read.
        for (size_t i = 0; i < variantCount; ++i) {
            if (!variantLive[i])
                continue;
            UpdateOneVehiclePostStep(playerVehicle, g_variantShadows[i], false, g_variantLabels[i].c_str());
        }

        // docs §52: accumulate this frame's total Jolt-side wall time BEFORE JoltProfileFrameEnd
        // (which may flush+reset the interval this very frame) so it lands in the same bucket as
        // this frame's ode_step/physics_update/etc.
        if (prof)
            g_joltProfile.joltTotalMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - joltT0).count();

        // Every vehicle this frame (player + any AI shadows above) has already run through
        // StepPhysicsProfiled/ApplyJoltToVehicleProfiled - this is the one place per real game
        // frame (UpdateShadow runs exactly once per StepSceneHook) to count the frame and
        // periodically flush the [jolt_profile] summary line (see JoltProfileFrameEnd's comment).
        JoltProfileFrameEnd();
    }

    // ai::DynamicScene::StepScene's call site inside ai::CServer::Update (VA 0x5F4260) -
    // confirmed via disassembly to wrap the ODE physics step (dWorldQuickStep, VA 0x7C53B0)
    // exactly once per frame, never sub-stepped (docs/jolt-integration-techanalysis.md Stage
    // 1 section). Same wrap-the-call-site ChangeCall pattern as fix::jolt's Stage 0 hooks -
    // the original StepScene runs completely untouched first, so by the time UpdateShadow()
    // reads any vehicle's state it's this frame's final, post-step value. Confirmed via
    // grep that nothing else in Kraken patches this exact call site (fix::testharness patches
    // a different one, VA 0x5F438D, for ai::DynamicScene::CollideScene).
    static void __fastcall StepSceneHook(hta::ai::DynamicScene* scene, void*, float elapsedTime) {
        // docs §52 (Этап 0): time the real ODE world step separately - this is the baseline cost
        // Jolt must ultimately beat once ODE is removed. Accumulated into the same [jolt_profile]
        // interval as the Jolt-side numbers (UpdateShadow flushes it). When perfmon=0 this is a
        // single uint config read + the untouched original call, no timing.
        if (kraken::Config::Instance().testharness_perfmon.value != 0) {
            const auto o0 = std::chrono::steady_clock::now();
            scene->StepScene(elapsedTime);
            g_joltProfile.odeStepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - o0).count();
        } else {
            scene->StepScene(elapsedTime);
        }

        // docs §46e (task #59): same ODE chassis coordinate line fix::odediag logs when Jolt is
        // OFF (see source/fix/odediag.cpp - that module owns this call site and can't run at
        // the same time this hook does, since only one of them is ever installed here). Logged
        // here too, gated on [ode_diag]enabled, so the SAME line format is available for a
        // jolt=1 launch - the two can be diffed directly across two separate launches instead of
        // comparing "Shadow divergence"'s ode com (a different reference point, see its own
        // comment) against a jolt=0 run's numbers.
        if (kraken::Config::Instance().ode_diag.value != 0) {
            hta::ai::Vehicle* playerVehicle = GetPlayerVehicle();
            if (playerVehicle != nullptr) {
                const hta::CVector odePivot = playerVehicle->GetPosition();
                const hta::CVector odeCom   = playerVehicle->GetMassCenterPosition();
                LOG_INFO("docs §46e: ODE chassis (player, jolt=1) GetPosition=(%.2f,%.2f,%.2f) "
                    "GetMassCenterPosition=(%.2f,%.2f,%.2f)",
                    (double) odePivot.x, (double) odePivot.y, (double) odePivot.z,
                    (double) odeCom.x, (double) odeCom.y, (double) odeCom.z);
            }
        }

        UpdateShadow(elapsedTime);
    }

    // Exposed for fix::testharness's ram_test debug mode (docs §22.4/§22.6). Under apply=1,
    // ApplyJoltToVehicle overwrites the player's ODE position/rotation/velocity EVERY frame
    // with whatever Jolt's own VehicleConstraint body independently tracks - so a plain
    // ODE-side PhysicObj::SetPositionSelf() teleport (the mechanism testharness already uses
    // for scripted-scenario spawn points) only lasts until the next StepScene, then silently
    // snaps back to wherever Jolt thinks the player is. Found live: testharness tried to spawn
    // the player 15m behind another vehicle to test a ram, and the player kept reverting to
    // its original autoload position one frame later. The fix is to move Jolt's OWN body too,
    // not just the ODE mirror - then the next ApplyJoltToVehicle call reads back the position
    // this function just set instead of fighting it. Returns false (no-op) if Jolt isn't
    // active or the player's shadow hasn't been built yet, both of which are normal states
    // testharness itself can't distinguish from "should have worked".
    bool TeleportPlayerShadow(const hta::CVector& pos, const hta::Quaternion& rot) {
        if (g_playerShadow.bodyId.IsInvalid())
            return false;

        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        if (physics == nullptr)
            return false;

        JPH::BodyInterface& bodyInterface = physics->GetBodyInterface();
        bodyInterface.SetPositionAndRotation(g_playerShadow.bodyId,
            JPH::RVec3(pos.x, pos.y, pos.z), JPH::Quat(rot.x, rot.y, rot.z, rot.w),
            JPH::EActivation::Activate);
        bodyInterface.SetLinearVelocity(g_playerShadow.bodyId, JPH::Vec3::sZero());
        bodyInterface.SetAngularVelocity(g_playerShadow.bodyId, JPH::Vec3::sZero());
        // docs §40: in wheelmodel mode the suspension DOF caches per-wheel ground heights
        // (wmSuspLen/wmRestLen) from the last build pose. Moving the body without re-seating them
        // leaves the wheels grazing/missing the new ground so the chassis sinks - re-raycast them
        // onto the ground at the destination pose (also zeroes the per-wheel rates, matching the
        // body velocity we just zeroed).
        if (g_playerShadow.wheelModelMode)
            InitWheelModelSuspension(physics, g_playerShadow, pos, rot, "player");
        return true;
    }

    // docs §23.4: the §22.3/§22.4 diagnostic (dBodyGetNumJoints excess on the player's own
    // chassis/wheel bodies) monitors ai::NearCallback's generic dJointCreateContact path -
    // but vehicle-vs-vehicle ramming does NOT go through that path at all. It's dispatched to
    // its own dedicated handler, ai::CollideVehiclePartAndVehiclePart (VA 0x00890430), which
    // calls ai::CalcDamageToVehicles (VA 0x0088F700) to compute ram damage directly from
    // dContact data - the exact same "specialized per-class-pair handler bypasses the generic
    // joint path" pattern already confirmed for wheel-ground contact (CollideWheelDefault).
    // So the old diagnostic was watching the wrong signal for vehicle-vs-vehicle specifically;
    // this one hooks the real one. Preserves 100% of stock behavior (calls through via a
    // 5-byte trampoline at a confirmed instruction boundary - `sub esp,0x5c; push ebx; push
    // ebp`, verified via tools/lora disasm against the deployed game.pdb) and only adds a log
    // line when the player's own vehicle is either party.
    using CalcDamageToVehiclesFn = void(__fastcall*)(hta::ai::Vehicle*, hta::ai::Vehicle*, void*, float*, void*, void*);
    static uint8_t s_calcDamageTrampoline[16];
    static CalcDamageToVehiclesFn Real_CalcDamageToVehicles = nullptr;

    // docs §126: the "O" hotkey recovery ("выбраться из сложного места" - user-reported live,
    // wheels visibly sunk under the terrain, save-load-triggered Ural01 hang under
    // wm4_contact_constraint=1).
    //
    // FIRST ATTEMPT (reverted after a live crash) hooked the PUBLIC Vehicle::GetOutOfDifficultPlace
    // (VA 0x005CBB00) - wrong on two counts, both found only after the crash by actually
    // disassembling it (tools/lora-style capstone check against the deployed hta.exe, the same
    // discipline CalcDamageToVehiclesHook's own comment already documents and this first attempt
    // skipped): (1) that function is a single 7-byte `mov byte ptr [ecx+0x4cc],1 / ret` - just
    // sets Vehicle::m_bMustGetOutOfDifficultPlace (Vehicle.hpp:226) - a 5-byte trampoline copy
    // sliced that one instruction in half, corrupting the trampoline buffer; every call through
    // it executed garbage bytes, observed live as an 0xC0000005 write AV inside kraken.dll.
    // (2) even with a correct trampoline it would have been a no-op fix: the flag-setter does
    // not reposition anything itself - the real work happens later, off Vehicle::Update()'s own
    // poll of that flag.
    //
    // Hooking the ACTUAL work function instead: Vehicle::_GetOutOfDifficlultPlaceInternal
    // (VA 0x005E4740, native's own typo, see Vehicle.cpp:251 - "TODO: must be __usercall" is the
    // native declaration's own uncertainty flag, NOT confirmed by disassembly here: the real
    // prologue is a plain `sub esp,0xb8 / push esi / push 0 / mov esi,ecx / call ...` - `this`
    // arrives in ECX and nothing else is read before it's saved off, i.e. plain __thiscall(this),
    // same ECX-first ABI as __fastcall with an unused EDX slot). Disassembly-verified safe
    // trampoline boundary is 6 bytes here (sub esp,0xb8 is a single 6-byte instruction, next
    // instruction starts cleanly at +6) - NOT 5, unlike CalcDamageToVehiclesHook's function.
    using GetOutOfDifficultPlaceInternalFn = void(__fastcall*)(hta::ai::Vehicle*, void*);
    static uint8_t s_getOutOfDifficultPlaceTrampoline[16];
    static GetOutOfDifficultPlaceInternalFn Real_GetOutOfDifficultPlaceInternal = nullptr;

    // docs §23.11: is `v` a vehicle whose ODE body is CURRENTLY disabled+overwritten every
    // frame by ApplyJoltToVehicle (i.e. a real dynamic body only inside Jolt, with no ODE-side
    // physics of its own this frame)? Mirrors UpdateShadow's playerAllowApply/aiAllowApply
    // gating exactly (same three config reads) rather than threading an extra bool through
    // from there - this is called from a totally different call path (an ODE collision
    // callback, invoked from inside scene->StepScene(), i.e. BEFORE UpdateShadow even runs
    // this frame - see StepSceneHook) so there is no already-computed value to reuse.
    static bool IsVehicleJoltAuthoritative(hta::ai::Vehicle* v) {
        if (v == nullptr)
            return false;
        const kraken::Config& config = kraken::Config::Instance();
        if (config.jolt_apply.value == 0)
            return false;
        if (v == GetPlayerVehicle())
            return config.jolt_player_only.value == 0 || v->bIsControlledByPlayer();
        return std::find(g_aiTargets.begin(), g_aiTargets.end(), v) != g_aiTargets.end();
    }

    // docs §23.11: closes the one direction the ghost-passthrough fix (§22.6/§22.11) never
    // covered - a Jolt-DYNAMIC vehicle (player or Stage-3 AI shadow) hitting a plain-ODE/
    // kinematically-mirrored one. Jolt's own solver already makes the KINEMATIC mirror push
    // the Jolt-dynamic vehicle correctly (that's the "other vehicles push the player" behavior
    // already working) - but a kinematic body can never itself receive a push, by definition,
    // and on the ODE side `DisablePhysics()` (what ApplyJoltToVehicle calls every frame under
    // apply=1) removes the Jolt-driven vehicle's body from ODE's own constraint solving
    // entirely, so any dContact joint CollideVehiclePartAndVehiclePart creates against it
    // produces no force on either party either. Nothing today makes the OTHER vehicle move.
    //
    // Deliberately approximate, same spirit as §23.9's accepted gaps: no real contact-solver
    // access, so the push direction is taken as the straight line between the two vehicles'
    // own GetPosition() centers, and applied through the mirrored vehicle's center of mass
    // (AddImpulse, not AddImpulseAtPos) rather than at the true contact height - a fair
    // approximation for a vehicle-body-sized hit, not a torque-inducing off-center one.
    // Self-limiting rather than cooldown-rate-limited: each applied impulse reduces the
    // closing speed between the two vehicles, so a sustained, already-equalized contact
    // should stop re-triggering on its own once the closing speed drops back under the
    // threshold - re-triggers only as long as (or whenever) the Jolt-driven side is still
    // actively closing the gap, which is the physically-correct case to keep pushing.
    //
    // Gates on IsVehicleJoltAuthoritative itself (not just at each call site) because there
    // are now two independent callers - see below - and a body being DYNAMIC in Jolt does NOT
    // by itself mean it's driving the real vehicle right now: under apply=0 (shadow-only) the
    // shadow chassis is still a real Dynamic body colliding with kinematic mirrors inside
    // Jolt's own PhysicsSystem::Update, entirely independent of the `apply` flag - pushing the
    // ODE-side mirrored vehicle from a collision that only exists in a read-only diagnostic
    // shadow would be a real bug (a visible gameplay effect from a simulation nobody asked to
    // be authoritative), not an approximation.
    static void ApplyRamPushback(hta::ai::Vehicle* joltVehicle, hta::ai::Vehicle* mirroredVehicle, float closingSpeed) {
        if (!IsVehicleJoltAuthoritative(joltVehicle))
            return;
        // CalcDamageToVehiclesHook's v1/v2 come straight from the native CalcDamageToVehicles
        // call - confirmed live (crash repro, docs: reproducible AV in ai::PhysicObj::GetRotation
        // with this==nullptr, called via GetPosition() below) that the real game calls it with
        // one of the two vehicle pointers null for some real collision case. joltVehicle is
        // already guaranteed non-null by IsVehicleJoltAuthoritative above (it returns false for
        // nullptr); mirroredVehicle has no such guarantee - DrainPendingPushbacks' caller already
        // null-checks it (HandleContact), but CalcDamageToVehiclesHook's caller didn't.
        if (mirroredVehicle == nullptr)
            return;

        const kraken::Config& config = kraken::Config::Instance();
        if (closingSpeed < config.jolt_pushback_min_dspeed.value)
            return; // resting/pile-up contact noise, not a real hit

        const hta::CVector delta = mirroredVehicle->GetPosition() - joltVehicle->GetPosition();
        const float dist = delta.Length();
        if (dist < 0.01f)
            return; // degenerate (near-exactly-overlapping centers) - avoid an unstable direction

        const hta::CVector pushDir = delta * (1.0f / dist);
        const float joltMass = joltVehicle->GetMass();
        const float mirroredMass = mirroredVehicle->GetMass();
        if (mirroredMass <= 0.0f)
            return;
        const float reducedMass = (joltMass * mirroredMass) / std::max(joltMass + mirroredMass, 1.0f);
        const float impulseMag = closingSpeed * reducedMass * config.jolt_pushback_scale.value;

        mirroredVehicle->EnablePhysics(); // make sure a resting/auto-disabled body actually wakes to receive this
        mirroredVehicle->AddImpulse(pushDir * impulseMag);

        LOG_INFO("docs §23.11: ram pushback applied - jolt-driven=%p mirrored=%p closingSpeed=%.2f impulse=%.1f",
            (void*) joltVehicle, (void*) mirroredVehicle, (double) closingSpeed, (double) impulseMag);
    }

    // docs §23.11: first detector tried - reuses the already-installed CalcDamageToVehiclesHook
    // (docs §23.4), which already fires on every real vehicle-vs-vehicle ODE collision and
    // already correlates (v1, v2) with a damage-relevant dSpeed. Kept as a cheap, best-effort
    // extra layer, VehiclePushbackContactListener below (registered directly on Jolt's own
    // PhysicsSystem) is the detector actually relied on as primary.
    //
    // Earlier live testing (apply=1, Stage 3 shape: the OTHER party also Jolt-shadowed, i.e.
    // BOTH sides' ODE bodies disabled via DisablePhysics()) found this hook never fired with the
    // player as a party, and speculated the dispatch path skips a disabled body outright. Stage
    // 2's own open-item verification (docs/stage2-plan.md "ApplyRamPushback"; player_only=1,
    // ai_count=0, so the other party is a plain kinematic mirror whose OWN ODE body stays live/
    // AI-driven - only the player's side is disabled) found the opposite: it fires readily (57
    // player-party calls in a 60s busy-save run, dSpeed up to 10.08, matching
    // VehiclePushbackContactListener's own closingSpeed for the same contacts). So the dispatch
    // path doesn't skip a disabled body outright - it needs the OTHER geom's owning body to still
    // be ODE-live for CollideVehiclePartAndVehiclePart to run at all; two Jolt-disabled bodies
    // colliding (Stage 3 player-vs-AI-shadow) never reaches it, one Jolt-disabled body against a
    // still-ODE-live one (Stage 2 player-vs-mirror) does. Practical effect: for the Stage 2 shape,
    // both detectors are live for the same contact and can each independently call
    // ApplyRamPushback in the same frame (this hook synchronously inside ODE's scene->StepScene(),
    // the listener queued and drained later that frame from StepPhysicsProfiled) - observed
    // harmless in that same 60s run (0 non-finite, sane impulse magnitudes throughout) since each
    // applied impulse reduces the closing speed the other detector would need to re-trigger, but
    // not verified deduplicated - a genuinely simultaneous double-impulse on the same contact in
    // the same frame remains possible and unmeasured.
    static void __fastcall CalcDamageToVehiclesHook(hta::ai::Vehicle* v1, hta::ai::Vehicle* v2,
            void* contacts, float* dSpeed, void* damageInfo, void* contactPos) {
        Real_CalcDamageToVehicles(v1, v2, contacts, dSpeed, damageInfo, contactPos);

        hta::ai::DynamicScene* scene = hta::ai::DynamicScene::Instance();
        hta::ai::Vehicle* player = scene ? scene->GetVehicleControlledByPlayer() : nullptr;
        if (player != nullptr && (v1 == player || v2 == player)) {
            LOG_WARNING("docs §23.4: live ram-damage call involves the PLAYER vehicle (v1=%p v2=%p player=%p) dSpeed=%.2f",
                (void*) v1, (void*) v2, (void*) player, dSpeed ? (double) *dSpeed : -1.0);
        }

        if (dSpeed == nullptr)
            return;
        const bool v1JoltDriven = IsVehicleJoltAuthoritative(v1);
        const bool v2JoltDriven = IsVehicleJoltAuthoritative(v2);
        if (v1JoltDriven == v2JoltDriven)
            return; // both or neither Jolt-driven - already handled natively either way, see docs §23.11
        ApplyRamPushback(v1JoltDriven ? v1 : v2, v1JoltDriven ? v2 : v1, *dSpeed);
    }

    // docs §126: call through first (unconditionally - preserves the ODE-side effects the
    // stock recovery already has, e.g. m_bWasStuck/m_prevPosToCheckStuck bookkeeping this file
    // knows nothing about and has no business touching), THEN re-seat the Jolt body at wherever
    // the native call just decided was safe. Player-only and Jolt-authoritative-only, same
    // scope TeleportPlayerShadow itself is written for (it unconditionally manipulates
    // g_playerShadow - calling it for an AI vehicle would silently move the PLAYER's shadow
    // instead of the one the game actually meant).
    static void __fastcall GetOutOfDifficultPlaceInternalHook(hta::ai::Vehicle* v, void* /*edx, unused*/) {
        Real_GetOutOfDifficultPlaceInternal(v, nullptr);
        if (v != nullptr && v == GetPlayerVehicle() && IsVehicleJoltAuthoritative(v)) {
            const bool moved = kraken::fix::joltshadow::TeleportPlayerShadow(v->GetPosition(), v->GetRotation());
            LOG_INFO("docs §126: O-key recovery (_GetOutOfDifficlultPlaceInternal) re-seated Jolt "
                     "shadow body=%d at pos=(%.2f,%.2f,%.2f)", (int) moved,
                (double) v->GetPosition().x, (double) v->GetPosition().y, (double) v->GetPosition().z);
        }
    }

    // docs §23.11: the real detector. Registered directly on Jolt's own PhysicsSystem
    // (SetContactListener, see Apply() below), so it observes exactly the collision that
    // already makes "other vehicles push the player" work - a Jolt-DYNAMIC vehicle chassis
    // (player shadow or Stage-3 AI shadow, both tagged with their owning hta::ai::Vehicle* via
    // Body::SetUserData at creation, see BuildShadow) touching a KINEMATIC mirror body (same
    // tagging, see MirrorOtherVehicles). Motion type alone distinguishes the two sides - no
    // need to re-derive "is this Jolt-driven" from config/g_aiTargets the way
    // IsVehicleJoltAuthoritative does, since only Jolt-dynamic vehicle chassis are ever Dynamic
    // and only mirrors are ever Kinematic in this physics world.
    //
    // OnContactAdded/OnContactPersisted can run concurrently on multiple JobSystem worker
    // threads for different body pairs within the same PhysicsSystem::Update() call (per
    // Jolt's own ContactListener contract) - so this must NOT call back into ODE (PhysicObj
    // methods are not known to be thread-safe, and the main thread isn't touching ODE
    // concurrently with Update() anyway, so there's no need to risk it). Instead it only
    // queues a lightweight event under a mutex; DrainPendingPushbacks (called once, back on
    // the main thread, right after StepPhysicsProfiled returns in UpdateShadow) does the
    // actual ODE-side work single-threaded.
    struct PendingPushback {
        hta::ai::Vehicle* joltVehicle     = nullptr;
        hta::ai::Vehicle* mirroredVehicle = nullptr;
        float             closingSpeed    = 0.0f;
    };
    static std::mutex                   g_pendingPushbackMutex;
    static std::vector<PendingPushback> g_pendingPushbacks;

    // docs §57 (goal: "деревья падали при таране как с ODE"): the native ram-damage/break
    // dispatch (ai::NearCallback -> ... -> ai::CollideVehicleAndBreakableObject,
    // docs §56) was confirmed live (a hook, zero firings across two harness ram tests that both
    // ended in the vehicle wedged against a tree) to never fire for the Jolt-driven vehicle - the
    // exact same symptom docs §23.11 already found and worked around for ram PUSHBACK via this
    // same ContactListener instead of trusting the native dispatch. Same fix shape here: queue on
    // Jolt's worker thread (no ODE/game calls there, see VehiclePushbackContactListener's own
    // comment on why), apply on the main thread once PhysicsSystem::Update() has returned.
    struct PendingBreak {
        hta::ai::BreakableObject* breakable    = nullptr;
        float                     impactEnergy = 0.0f;
    };
    static std::mutex                g_pendingBreakMutex;
    static std::vector<PendingBreak> g_pendingBreaks;

    // docs §61 (goal: "сделай чтобы в jolt деревья падали как в ode"): a SEPARATE queue from
    // PendingBreak above - docs §60.3 found (live, via a jolt-independent SetState hook) that a
    // non-destroyable prop (every tree, docs §59.2's XML table) never goes through
    // CreateBrokenObj/REMOVED at all. The native CollideVehicleAndBreakableObject
    // (docs §60.5's disasm_typed trace) calls SetState(ENABLED) on it instead, on ANY real
    // contact - no impactEnergy/criticalHitEnergy gate whatsoever, that comparison only ever
    // changes behavior for destroyable objects. ENABLED is what creates the real ODE joint
    // (SetJointAnchor).
    //
    // docs §64: extended to carry an impulse too - §63 found live that SetJointAnchor alone
    // leaves the tree perfectly rigid (a real 2.3s player ram: rotation byte-identical from
    // first sample to last). A tree is a Jolt STATIC body, never pushed by Jolt's own solver by
    // definition - under pure ODE the vehicle and tree share one simulation and continuous
    // contact resolution pushes the tree automatically every step; under Jolt nothing was
    // injecting the vehicle's actual momentum into the tree's now-real ODE body. Same problem
    // ApplyRamPushback already solved for vehicle-vs-vehicle - same fix shape here
    // (AddImpulseAtPos), just every step instead of once (jolt_tree_push_scale's own comment).
    struct PendingTreeContact {
        hta::ai::BreakableObject* breakable = nullptr;
        hta::CVector               impulse;
        hta::CVector               worldPos;
    };
    static std::mutex                      g_pendingEnableMutex;
    static std::vector<PendingTreeContact> g_pendingEnables;

    class VehiclePushbackContactListener final : public JPH::ContactListener {
    public:
        void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override {
            HarvestWheelContact(inBody1, inBody2, inManifold, ioSettings);
            HandleContact(inBody1, inBody2, inManifold);
            HandleBreakableContact(inBody1, inBody2, inManifold);
            HandleTreeEnableContact(inBody1, inBody2, inManifold);
        }
        void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override {
            HarvestWheelContact(inBody1, inBody2, inManifold, ioSettings);
            HandleContact(inBody1, inBody2, inManifold);
            // docs §63: HandleBreakableContact (destroyable/REMOVED path) deliberately stays
            // Added-only, unchanged - see its own comment. Only the tree/ENABLE path needs the
            // persisted-edge cadence too.
            HandleTreeEnableContact(inBody1, inBody2, inManifold);
        }

    private:
        // docs §59/§63 (Этап 1, шаг 3): the write half of the harvest. Runs on Jolt worker
        // threads, concurrently, with body mutexes held - so it does exactly three things: read
        // the tagged UserData, decode which sub-shape was hit, and append a record. No game call,
        // no BodyInterface, no allocation, no lock.
        //
        // This is also where the TYRE/RIM decision lives, PER PAIR rather than on the body. The
        // tyre sphere is made a sensor for this contact so the solver produces no force from it -
        // the band force is the model's job - while the rim stays a real contact with friction
        // and restitution zeroed, so it can only ever stop the wheel sinking through the world.
        static void HarvestWheelContact(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) {
            const bool w1 = IsWheelUserData(inBody1.GetUserData());
            const bool w2 = IsWheelUserData(inBody2.GetUserData());
            if (w1 == w2) {
                // docs §129 (temporary, read-only, not yet live-confirmed): §56/§124 Ural01
                // cold-pin hang - new hypothesis. This branch's own comment assumed "both are
                // wheels" was impossible "by group filter" - but JPH::GroupFilterTable only
                // suppresses collision between bodies sharing the SAME CollisionGroup::GroupID
                // (:171-176), and this project's vehicle-rebuild teardown is deliberately "leak
                // forever" (a previous vehicle instance's wheel bodies are never removed from
                // the PhysicsSystem, see the rebuild/teardown comment). A NEW vehicle's wheel
                // landing on an OLD leaked wheel from a DIFFERENT instance (different GroupID)
                // would read w1==w2==true right here, get silently discarded BEFORE the tyre
                // sensor flag a few lines below is ever set, and Jolt's own un-flagged solver
                // would then treat it as a perfectly normal solid contact - which would explain
                // both the confirmed genuine static equilibrium (something really is holding the
                // vehicle up) and the confirmed permanently-empty harvest buffer (this function
                // returns before ever writing to it) at the same time, without contradicting any
                // of the nine previously ruled-out hypotheses. Logging (throttled, atomic
                // counter - this runs on Jolt worker threads, same as the already-proven-safe
                // §124.12 ConstraintManager trace) whether this branch is actually reached with
                // BOTH sides wheel-tagged during a live stuck episode, and if so which two wheel
                // slots/indices collided - the single decisive live check before touching any
                // behavior here.
                if (w1 && w2) {
                    static std::atomic<uint32_t> sBothWheelDiscardCount{0};
                    const uint32_t n = sBothWheelDiscardCount.fetch_add(1, std::memory_order_relaxed);
                    if ((n & 0x3Fu) == 0) {
                        const uint64_t ud1 = inBody1.GetUserData();
                        const uint64_t ud2 = inBody2.GetUserData();
                        LOG_INFO("docs §129: wheel-vs-wheel contact discarded (n=%u) "
                                 "slot1=%u idx1=%u slot2=%u idx2=%u",
                            n, WheelUserDataSlot(ud1), WheelUserDataIndex(ud1),
                            WheelUserDataSlot(ud2), WheelUserDataIndex(ud2));
                    }
                }
                // docs §130 (temporary, read-only): §129 came back with ZERO hits during a clean
                // live repro of the stuck case (docs §129's own hypothesis is REFUTED - see
                // jolt-integration-techanalysis.md). SetupVelocityConstraint (below in this file)
                // correctly zeroes every slot when !mHasContact, and OnStep is confirmed to call
                // ClearContact() every step the harvest is empty - so if harvest really is empty
                // every step, WheelContactConstraint's own normal-axis spring cannot be what's
                // holding the vehicle up. Next-most-likely mechanism: the CHASSIS itself (not a
                // wheel at all) is resting directly on terrain/an obstacle - neither side wheel-
                // tagged, so HarvestWheelContact's caller never even considers it "a wheel
                // problem", yet it would produce exactly the same observable symptom (genuine
                // static equilibrium, zero WHEEL harvest records) if the wheels never reach the
                // ground because the chassis geometry touches first (e.g. a bad landing
                // orientation on vehicle switch). Under this diagnostic's own test config
                // ([jolt_harness] player_only=1 ai=0) there is exactly one dynamic body group in
                // the whole world (the player vehicle's own chassis+wheels) - so "neither side is
                // wheel-tagged AND at least one side is Dynamic" can only be that same vehicle's
                // chassis compound touching something. Reading GetMotionType()/GetUserData() only
                // (raw values already read safely elsewhere in this exact file, e.g. HandleContact
                // a few lines below) - NOT calling GetPlayerVehicle() or any other game/ODE
                // accessor from this worker thread, which this file's own §23.11 comment
                // documents as unsafe.
                if (!w1 && !w2 && (inBody1.GetMotionType() == JPH::EMotionType::Dynamic ||
                                    inBody2.GetMotionType() == JPH::EMotionType::Dynamic)) {
                    static std::atomic<uint32_t> sChassisContactCount{0};
                    const uint32_t n = sChassisContactCount.fetch_add(1, std::memory_order_relaxed);
                    if ((n & 0xFFu) == 0) {
                        LOG_INFO("docs §130: non-wheel dynamic contact (n=%u) "
                                 "body1=(motion=%d ud=%llx) body2=(motion=%d ud=%llx) pen=%.4f",
                            n, (int) inBody1.GetMotionType(), (unsigned long long) inBody1.GetUserData(),
                            (int) inBody2.GetMotionType(), (unsigned long long) inBody2.GetUserData(),
                            (double) inManifold.mPenetrationDepth);
                    }
                }
                return;   // neither is a wheel, or both are (see docs §129/§130 above)
            }

            const JPH::Body&  wheel = w1 ? inBody1 : inBody2;
            const uint64_t    ud    = wheel.GetUserData();
            const uint32_t    slot  = WheelUserDataSlot(ud);
            const uint32_t    idxW  = WheelUserDataIndex(ud);
            if (slot >= kMaxVehicleSlots || idxW >= kMaxHarvestWheels)
                return;

            // A SubShapeID is a bit-packed PATH through the shape hierarchy, never a raw index -
            // comparing it against 0 or 1 would be wrong in a way that happens to look right for
            // simple shapes. Decode it properly.
            uint32_t sub = 0;
            const JPH::Shape* shape = wheel.GetShape();
            if (shape != nullptr && shape->GetSubType() == JPH::EShapeSubType::StaticCompound) {
                const JPH::CompoundShape* compound = static_cast<const JPH::CompoundShape*>(shape);
                JPH::SubShapeID remainder;
                sub = compound->GetSubShapeIndexFromID(w1 ? inManifold.mSubShapeID1 : inManifold.mSubShapeID2, remainder);
            }

            if (sub == 0) {
                ioSettings.mIsSensor = true;             // TYRE: the band owns this force
            } else {
                ioSettings.mCombinedFriction    = 0.0f;  // RIM: non-penetration only, never grip
                ioSettings.mCombinedRestitution = 0.0f;
            }
            // Deliberately NOT touching mInvMassScale*/mInvInertiaScale* - ContactListener.h
            // requires those stay constant over a pair's lifetime.

            // The normal points from body 1 out toward body 2. We want it pointing OUT of the
            // surface TOWARD the wheel, regardless of which side Jolt called body 1.
            const JPH::Vec3 normal = w1 ? -inManifold.mWorldSpaceNormal : inManifold.mWorldSpaceNormal;
            const JPH::RVec3 base  = inManifold.mBaseOffset;

            WheelHarvest& buf = g_wheelHarvest[g_harvestStep.load(std::memory_order_relaxed) & 1u][slot][idxW];
            const auto& pts = w1 ? inManifold.mRelativeContactPointsOn2 : inManifold.mRelativeContactPointsOn1;
            for (const JPH::Vec3& rel : pts) {
                const uint32_t at = buf.count.fetch_add(1, std::memory_order_relaxed);
                if (at >= kMaxHarvestRecs)
                    continue;   // counted as overflow, not stored - the count is the diagnostic
                buf.rec[at].point  = JPH::Vec3(base + rel);
                buf.rec[at].normal = normal;
                buf.rec[at].depth  = inManifold.mPenetrationDepth;
                buf.rec[at].sub    = sub;
                // docs §124 step 3: the non-wheel side of this pair - whatever the wheel is
                // actually touching. w1/w2 already established exactly one side is the wheel.
                buf.rec[at].other  = (w1 ? inBody2 : inBody1).GetID();
            }
        }

        static void HandleContact(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold) {
            const bool body1Dynamic = inBody1.GetMotionType() == JPH::EMotionType::Dynamic;
            const bool body2Dynamic = inBody2.GetMotionType() == JPH::EMotionType::Dynamic;
            if (body1Dynamic == body2Dynamic)
                return; // both or neither dynamic - native Jolt already resolves dynamic-vs-dynamic correctly; static/kinematic-vs-kinematic never needs a push
            const JPH::Body& dynamicBody   = body1Dynamic ? inBody1 : inBody2;
            const JPH::Body& kinematicBody = body1Dynamic ? inBody2 : inBody1;
            if (kinematicBody.GetMotionType() != JPH::EMotionType::Kinematic)
                return; // the non-dynamic side must specifically be a vehicle mirror, not a static body

            // docs §58: a WHEEL body's UserData is a tagged handle ('WHL\0' in the high dword),
            // NOT an hta::ai::Vehicle*. Reinterpreting it would hand the pushback path a garbage
            // pointer and then dereference it. This guard has to sit BEFORE the casts, and it has
            // to cover both sides: a wheel can meet a kinematic mirror as easily as a chassis can.
            // Wheels are excluded from pushback entirely by design - a wheel's interaction with
            // another vehicle is the tyre model's business, not the ram-pushback path's.
            if (IsWheelUserData(dynamicBody.GetUserData()) || IsWheelUserData(kinematicBody.GetUserData()))
                return;

            hta::ai::Vehicle* joltVehicle     = reinterpret_cast<hta::ai::Vehicle*>(dynamicBody.GetUserData());
            hta::ai::Vehicle* mirroredVehicle = reinterpret_cast<hta::ai::Vehicle*>(kinematicBody.GetUserData());
            if (joltVehicle == nullptr || mirroredVehicle == nullptr)
                return;

            // mWorldSpaceNormal points from body1 toward resolving body2 out of body1 - flip
            // it so it consistently points FROM the dynamic body TOWARD the mirror regardless
            // of which one Jolt happened to assign as "body1" for this pair.
            JPH::Vec3 normal = inManifold.mWorldSpaceNormal;
            if (!body1Dynamic)
                normal = -normal;

            const float closingSpeed = dynamicBody.GetLinearVelocity().Dot(normal);
            if (closingSpeed <= 0.0f)
                return; // separating or tangential graze, not a push

            std::lock_guard<std::mutex> lock(g_pendingPushbackMutex);
            g_pendingPushbacks.push_back({joltVehicle, mirroredVehicle, closingSpeed});
        }

        // docs §57: the Jolt-side counterpart to HandleContact above, for a Jolt-DYNAMIC vehicle
        // chassis ramming a docs §55/§57 "resting prop" body (EMotionType::Static, UserData
        // tagged with its real hta::ai::BreakableObject* by jolt.cpp's WalkSpaceForStaticExport -
        // see CollectBreakableObjectOwners there). OnContactAdded only (rising edge) - a break
        // should fire once, at the moment of impact, not every step the vehicle keeps resting
        // against what's left of the prop. docs §63: handles ONLY the destroyable (energy-gated,
        // REMOVED) path now - the non-destroyable path moved to HandleTreeEnableContact below,
        // which needs a different (every-step) cadence and has no "don't re-process" concern
        // since SetState(ENABLED) is idempotent. Deliberately NOT touching this function's own
        // cadence or behavior for the destroyable case - it's proven working, docs §57/§58.
        static void HandleBreakableContact(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold) {
            const bool body1Dynamic = inBody1.GetMotionType() == JPH::EMotionType::Dynamic;
            const bool body2Dynamic = inBody2.GetMotionType() == JPH::EMotionType::Dynamic;
            if (body1Dynamic == body2Dynamic)
                return;
            const JPH::Body& dynamicBody = body1Dynamic ? inBody1 : inBody2;
            const JPH::Body& propBody    = body1Dynamic ? inBody2 : inBody1;
            if (propBody.GetMotionType() != JPH::EMotionType::Static)
                return; // the non-dynamic side must specifically be a docs §55 static prop, not a kinematic mirror (that's HandleContact's job)

            // Wheels ramming a prop are excluded (matches HandleContact's own wheel exclusion,
            // and the native path's own bias toward chassis hits, docs §56.2's
            // CollideVehicleAndBreakableObject reading - a wheel CAN reach it there via
            // Wheel::GetVehicle, but keeping this to chassis-only for the first working version
            // is the conservative choice: a wheel grazing a sapling shouldn't fell it).
            if (IsWheelUserData(dynamicBody.GetUserData()))
                return;
            const uint64_t propTag = propBody.GetUserData();
            if (propTag == 0)
                return; // an untagged static body - plain terrain/road/rock/other obstacle, not a resolved breakable prop (docs §57's ResolveBreakableOwner didn't find an owner for it)
            hta::ai::Vehicle* vehicle = reinterpret_cast<hta::ai::Vehicle*>(dynamicBody.GetUserData());
            if (vehicle == nullptr)
                return;
            hta::ai::BreakableObject* breakable = reinterpret_cast<hta::ai::BreakableObject*>(propTag);
            if (!LooksLikeLiveBreakableObject(breakable))
                return; // docs §66: stale export-time tag into memory since invalidated - see the helper's own comment
            if (!breakable->IsDestroyable())
                return; // docs §63: handled by HandleTreeEnableContact instead

            JPH::Vec3 normal = inManifold.mWorldSpaceNormal;
            if (!body1Dynamic)
                normal = -normal;

            // docs §58 (user: "калибруй"): point velocity (linear + angular contribution AT
            // the contact point), not just the chassis COM's linear velocity - matches native's
            // dBodyGetPointVel exactly (docs §56.2's CollideVehicleAndBreakableObject, confirmed
            // live via tools/lora disasm_typed). mBaseOffset + mRelativeContactPointsOn1/2 is the
            // same world-space-contact-point pattern HarvestWheelContact above already uses.
            const auto& contactPts = body1Dynamic ? inManifold.mRelativeContactPointsOn1
                                                    : inManifold.mRelativeContactPointsOn2;
            if (contactPts.empty())
                return;
            const JPH::RVec3 contactPoint = inManifold.mBaseOffset + contactPts[0];
            const float closingSpeed = dynamicBody.GetPointVelocity(contactPoint).Dot(normal);
            if (closingSpeed <= 0.0f)
                return; // separating or tangential graze, not a ram

            const JPH::MotionProperties* motionProps = dynamicBody.GetMotionProperties();
            if (motionProps == nullptr)
                return;
            const float invMass = motionProps->GetInverseMass();
            if (invMass <= 0.0f)
                return; // infinite mass (shouldn't happen for a Dynamic body) - no finite energy to compute
            const float mass = 1.0f / invMass;
            // docs §58: mass * closingSpeed^2, NO 0.5 factor - confirmed via tools/lora
            // disasm_typed to be the exact native break-gate in
            // ai::CollideVehicleAndBreakableObject (VA 0x492a00, source line 125): it compares
            // vehicle->GetMass() [vtable+0x110, ai::PhysicObj::GetMass, confirmed against PE
            // vtable bytes via vft_method] * closingSpeed^2 directly against
            // BreakableObject::GetCriticalHitEnergy() ([edi+0x148]) - no 1/2 anywhere in that
            // comparison. (There IS a reduced-mass * 0.5-if-destroyable formula nearby in the
            // same function, but it feeds ai::Obj::InflictDamage's DamageInfo - HP damage - a
            // few lines later, not this break decision; docs §57's code comment conflated the
            // two.) jolt_break_energy_scale now sits on top of this verified base formula
            // instead of an arbitrary guess.
            const float impactEnergy = mass * closingSpeed * closingSpeed
                * kraken::Config::Instance().jolt_break_energy_scale.value;

            std::lock_guard<std::mutex> lock(g_pendingBreakMutex);
            g_pendingBreaks.push_back({breakable, impactEnergy});
        }

        // docs §63 (goal: "деревья падали как в ODE"): split out of HandleBreakableContact above
        // specifically so this can run on EVERY step contact persists (wired into
        // OnContactPersisted too, below - not just OnContactAdded), not just the rising edge.
        // §62 found the joint force-threshold "break" system (physic.cpp) is dead code - zero
        // callers on any of its 5 setter functions anywhere in the binary - so nothing ever
        // arms a joint to snap. The only way left to confirm a tree is actually toppling (versus
        // just getting anchored and sitting there) is watching breakablediag.cpp's §63 pos/rot
        // log change across several samples of sustained contact - which needs this to fire every
        // step, not once. Calling SetState(ENABLED) repeatedly is free: its own same-state
        // early-exit already got exercised safely 18 times in a row under pure ODE (docs §60.2).
        static void HandleTreeEnableContact(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold) {
            const bool body1Dynamic = inBody1.GetMotionType() == JPH::EMotionType::Dynamic;
            const bool body2Dynamic = inBody2.GetMotionType() == JPH::EMotionType::Dynamic;
            if (body1Dynamic == body2Dynamic)
                return;
            const JPH::Body& dynamicBody = body1Dynamic ? inBody1 : inBody2;
            const JPH::Body& propBody    = body1Dynamic ? inBody2 : inBody1;
            if (propBody.GetMotionType() != JPH::EMotionType::Static)
                return;
            if (IsWheelUserData(dynamicBody.GetUserData()))
                return; // same conservative chassis-only bias as HandleBreakableContact
            const uint64_t propTag = propBody.GetUserData();
            if (propTag == 0)
                return;
            hta::ai::Vehicle* vehicle = reinterpret_cast<hta::ai::Vehicle*>(dynamicBody.GetUserData());
            if (vehicle == nullptr)
                return;
            hta::ai::BreakableObject* breakable = reinterpret_cast<hta::ai::BreakableObject*>(propTag);
            if (!LooksLikeLiveBreakableObject(breakable))
                return; // docs §66: stale export-time tag into memory since invalidated - see the helper's own comment
            if (breakable->IsDestroyable())
                return; // handled by HandleBreakableContact instead

            // docs §64: same normal/contactPoint/closingSpeed pattern as HandleBreakableContact's
            // own §58 comment (point velocity at the actual contact point, not COM) - the impulse
            // needs the same real contact geometry the (now-removed-here) energy calc used to.
            JPH::Vec3 normal = inManifold.mWorldSpaceNormal;
            if (!body1Dynamic)
                normal = -normal;
            const auto& contactPts = body1Dynamic ? inManifold.mRelativeContactPointsOn1
                                                    : inManifold.mRelativeContactPointsOn2;
            if (contactPts.empty())
                return;
            const JPH::RVec3 contactPoint = inManifold.mBaseOffset + contactPts[0];
            const float closingSpeed = dynamicBody.GetPointVelocity(contactPoint).Dot(normal);
            if (closingSpeed <= 0.0f)
                return; // separating or tangential graze, not a push

            const JPH::MotionProperties* motionProps = dynamicBody.GetMotionProperties();
            if (motionProps == nullptr)
                return;
            const float invMass = motionProps->GetInverseMass();
            if (invMass <= 0.0f)
                return;
            const float vehicleMass = 1.0f / invMass;
            // docs §64: reduced mass, same shape as ApplyRamPushback's own formula above - bounds
            // the result near the SMALLER of the two masses (a heavy truck can't inject more
            // momentum per step than the light tree itself could plausibly absorb), std::max(...,
            // 1.0f) guards a degenerate near-zero GetMass() on a prop prototype.
            const float treeMass = std::max(breakable->GetMass(), 1.0f);
            const float reducedMass = (vehicleMass * treeMass) / std::max(vehicleMass + treeMass, 1.0f);
            const float impulseMag = closingSpeed * reducedMass
                * kraken::Config::Instance().jolt_tree_push_scale.value;

            const hta::CVector impulse{normal.GetX() * impulseMag, normal.GetY() * impulseMag,
                                        normal.GetZ() * impulseMag};
            const hta::CVector worldPos{(float) contactPoint.GetX(), (float) contactPoint.GetY(),
                                         (float) contactPoint.GetZ()};

            std::lock_guard<std::mutex> lock(g_pendingEnableMutex);
            g_pendingEnables.push_back({breakable, impulse, worldPos});
        }
    };
    static VehiclePushbackContactListener g_pushbackContactListener;

    // A chassis is now a compound shape (docs §23.10) - a single real hit can generate one
    // manifold per sub-shape against the same mirror, which would multiply one hit's impulse
    // by however many parts happened to touch this step if applied independently. Keep only
    // the strongest closing speed seen per (joltVehicle, mirroredVehicle) pair this frame.
    static void DrainPendingPushbacks() {
        std::vector<PendingPushback> events;
        {
            std::lock_guard<std::mutex> lock(g_pendingPushbackMutex);
            events.swap(g_pendingPushbacks);
        }
        if (events.empty())
            return;

        std::map<std::pair<hta::ai::Vehicle*, hta::ai::Vehicle*>, float> strongest;
        for (const PendingPushback& e : events) {
            float& best = strongest[{e.joltVehicle, e.mirroredVehicle}];
            best = std::max(best, e.closingSpeed);
        }
        for (const auto& [pair, closingSpeed] : strongest)
            ApplyRamPushback(pair.first, pair.second, closingSpeed);
    }

    // docs §57: same "drain on the main thread, right after StepPhysicsProfiled returns"
    // placement/reasoning as DrainPendingPushbacks above - SetState() below reaches into
    // CreateBrokenObj/effect-node/blast-wave game code, none of it safe to call from a Jolt
    // worker thread. Dedupes by BreakableObject* first (same "a compound-shape hit can generate
    // several manifolds in one step" reasoning as DrainPendingPushbacks - though props are single
    // simple shapes today, docs §55, cheap insurance if that ever changes) and keeps only the
    // strongest impactEnergy seen this frame, rather than possibly calling SetState() twice on
    // the same object from two manifolds of the same hit.
    static void DrainPendingBreaks() {
        std::vector<PendingBreak> events;
        {
            std::lock_guard<std::mutex> lock(g_pendingBreakMutex);
            events.swap(g_pendingBreaks);
        }
        if (events.empty())
            return;

        std::map<hta::ai::BreakableObject*, float> strongest;
        for (const PendingBreak& e : events) {
            // docs §66: same stale-tag guard as the Handle* queueing functions (its own comment)
            // - the window between queueing (worker thread, this step) and draining (main thread,
            // moments later) is far narrower than the level-load-to-now window that actually
            // crashed live, but the check is cheap and the failure mode is identical if hit.
            if (!LooksLikeLiveBreakableObject(e.breakable))
                continue;
            float& best = strongest[e.breakable];
            best = std::max(best, e.impactEnergy);
        }
        for (const auto& [breakable, impactEnergy] : strongest) {
            // Re-checked here, not just at export time (jolt.cpp's CollectBreakableObjectOwners
            // already filters on IsDestroyable() once, at level load) - state can legitimately
            // change between then and now (destroyed by something else entirely, e.g. an
            // explosion via the STILL-WORKING native path, docs §56 only found the RAM-specific
            // dispatch broken) and re-calling SetState(REMOVED) on an already-REMOVED object is
            // pure waste at best.
            if (!breakable->IsDestroyable() || breakable->GetState() == hta::ai::BreakableObject::REMOVED)
                continue;
            const float threshold = breakable->GetCriticalHitEnergy();
            if (threshold < 0.0f || impactEnergy < threshold)
                continue; // negative threshold (docs §55 saw -1 on one prototype) means "never breaks this way" in the XML data, same convention respected here
            breakable->SetState(hta::ai::BreakableObject::REMOVED);
            LOG_INFO("docs §57: broke prop %p via Jolt-side ram detection (impactEnergy=%.1f >= "
                     "criticalHitEnergy=%.1f) - native dispatch path confirmed non-firing here, "
                     "docs §56.6", (void*) breakable, (double) impactEnergy, (double) threshold);
        }
    }

    // docs §61: the non-destroyable counterpart to DrainPendingBreaks above - same "drain on the
    // main thread, right after StepPhysicsProfiled returns" placement/reasoning (SetState() below
    // reaches into SetJointAnchor/ODE joint creation, not safe from a Jolt worker thread either).
    // No impactEnergy to compare here at all - see g_pendingEnables' own comment for why.
    static void DrainPendingEnables() {
        std::vector<PendingTreeContact> events;
        {
            std::lock_guard<std::mutex> lock(g_pendingEnableMutex);
            events.swap(g_pendingEnables);
        }
        if (events.empty())
            return;

        // No dedup-to-a-set here (unlike DrainPendingBreaks/DrainPendingPushbacks) - docs §64:
        // each event carries its OWN per-contact impulse now, and every one of them represents a
        // real step of sustained contact worth applying, not just a duplicate notification of the
        // same fact. SetState/IsDestroyable are still safe to re-check per event.
        for (const PendingTreeContact& e : events) {
            hta::ai::BreakableObject* breakable = e.breakable;
            // docs §66: same stale-tag guard as the Handle* queueing functions (its own comment).
            if (!LooksLikeLiveBreakableObject(breakable))
                continue;
            // Re-checked here, same IsDestroyable() reasoning as DrainPendingBreaks (state can
            // legitimately change between queueing and draining). No GetState()==DISABLED guard,
            // deliberately, matching HandleTreeEnableContact's own comment - repeats are free
            // (SetState's own same-state early-exit) and needed for breakablediag.cpp's §63
            // pos/rot log to sample more than once per tree.
            if (breakable->IsDestroyable())
                continue;
            breakable->SetState(hta::ai::BreakableObject::ENABLED);
            // docs §64: this is the actual push - §63 found SetState/SetJointAnchor alone leaves
            // the tree perfectly rigid (physic.cpp's force-threshold check is dead code, §62 -
            // it never "snaps", the tree has to be pushed like any other ODE dynamic body).
            // EnablePhysics() first in case the tree's ODE body starts auto-disabled/resting -
            // same reasoning as ApplyRamPushback's own call - a disabled body ignores impulses.
            breakable->EnablePhysics();
            breakable->AddImpulseAtPos(e.impulse, e.worldPos);
            LOG_INFO("docs §61: enabled non-destroyable prop %p via Jolt-side contact detection "
                     "(native dispatch path confirmed non-firing here, docs §56.6/§60.1) - real ODE "
                     "joint now anchors it via SetJointAnchor; docs §64 impulse=(%.2f,%.2f,%.2f) applied",
                     (void*) breakable, (double) e.impulse.x, (double) e.impulse.y, (double) e.impulse.z);
        }
    }

    static void InstallRamDamageDiagnostic() {
        void* const orig = reinterpret_cast<void*>(0x0088F700);
        DWORD oldProtect;
        VirtualProtect(s_calcDamageTrampoline, sizeof(s_calcDamageTrampoline), PAGE_EXECUTE_READWRITE, &oldProtect);
        std::memcpy(s_calcDamageTrampoline, orig, 5);
        s_calcDamageTrampoline[5] = 0xE9;
        *reinterpret_cast<int32_t*>(s_calcDamageTrampoline + 6) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(orig) + 5
            - (reinterpret_cast<uintptr_t>(s_calcDamageTrampoline) + 10));
        VirtualProtect(s_calcDamageTrampoline, sizeof(s_calcDamageTrampoline), oldProtect, &oldProtect);
        Real_CalcDamageToVehicles = reinterpret_cast<CalcDamageToVehiclesFn>(
            reinterpret_cast<uintptr_t>(s_calcDamageTrampoline));

        routines::Redirect(5, orig, reinterpret_cast<void*>(&CalcDamageToVehiclesHook));
        LOG_INFO("docs §23.4: ram-damage diagnostic installed (ai::CalcDamageToVehicles @ 0x0088F700)");
    }

    static void InstallGetOutOfDifficultPlaceHook() {
        // docs §126: 0x005E4740 = Vehicle::_GetOutOfDifficlultPlaceInternal, NOT the public
        // GetOutOfDifficultPlace (0x005CBB00) - see the long comment above
        // GetOutOfDifficultPlaceInternalFn for why the first attempt at this VA crashed live.
        // Trampoline copy size is 6, disassembly-verified (capstone against the deployed
        // hta.exe): first instruction at this VA is `sub esp, 0xb8`, a single clean 6-byte
        // instruction - copying only 5 would slice it in half exactly like the first attempt did
        // to GetOutOfDifficultPlace's 7-byte body.
        void* const orig = reinterpret_cast<void*>(0x005E4740);
        constexpr size_t kCopyLen = 6;
        DWORD oldProtect;
        VirtualProtect(s_getOutOfDifficultPlaceTrampoline, sizeof(s_getOutOfDifficultPlaceTrampoline),
            PAGE_EXECUTE_READWRITE, &oldProtect);
        std::memcpy(s_getOutOfDifficultPlaceTrampoline, orig, kCopyLen);
        s_getOutOfDifficultPlaceTrampoline[kCopyLen] = 0xE9;
        *reinterpret_cast<int32_t*>(s_getOutOfDifficultPlaceTrampoline + kCopyLen + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(orig) + kCopyLen
            - (reinterpret_cast<uintptr_t>(s_getOutOfDifficultPlaceTrampoline) + kCopyLen + 5));
        VirtualProtect(s_getOutOfDifficultPlaceTrampoline, sizeof(s_getOutOfDifficultPlaceTrampoline),
            oldProtect, &oldProtect);
        Real_GetOutOfDifficultPlaceInternal = reinterpret_cast<GetOutOfDifficultPlaceInternalFn>(
            reinterpret_cast<uintptr_t>(s_getOutOfDifficultPlaceTrampoline));

        // Redirect() itself always writes a 5-byte JMP regardless of `size` - the extra byte here
        // (6 vs the JMP's own 5) gets filled with 0xCC padding by Redirect's own memset, which is
        // exactly what should happen to the now-orphaned 6th byte of `sub esp,0xb8` at the LIVE
        // function (the trampoline above already has its own untouched copy to run from).
        routines::Redirect(kCopyLen, orig, reinterpret_cast<void*>(&GetOutOfDifficultPlaceInternalHook));
        LOG_INFO("docs §126: O-key recovery Jolt fix installed "
                 "(ai::Vehicle::_GetOutOfDifficlultPlaceInternal @ 0x005E4740)");
    }

    // docs §56.5: user confirmed live, on a real hard ram (not a gentle bump), that trees still
    // don't break after §55's ghosting fix. §56.2/§56.3 fully traced the native dispatch chain
    // (NearCallback -> MustCheckForCollision -> dCollide -> ColliderKrnl::CollideObjs ->
    // CollideVehicleAndBreakableObject -> hit-energy/damage) and found no explicit apply=1 gate
    // anywhere in it, and the project's own already-recorded §23.11 data says the native damage
    // path fires fine whenever the OTHER side of a contact is still ODE-live - which a breakable
    // prop always is (only vehicles ever get DisablePhysics()). So on paper this should already
    // work post-§55. It empirically doesn't, so this hooks the dispatcher itself instead of
    // continuing to reason from static disassembly alone.
    //
    // CollideObjs (VA 0x0088CB50), not CollideVehicleAndBreakableObject/
    // CollideVehiclePartAndVehiclePart directly - deliberately the lower-risk target of the
    // three: its signature is the most confidently determined (PDB regrel offsets +4/+8 for its
    // two stack params match a plain 2-register-then-2-stack __fastcall exactly - obj1/obj2 in
    // ecx/edx, dContact* and numContacts* on the stack, docs §56.2's own disasm of it), and its
    // call site inside NearCallback is a direct `call`, not an indirect table dispatch, so
    // there's no ambiguity about what's actually being redirected. Same 5-byte trampoline-at-a-
    // confirmed-instruction-boundary technique as InstallRamDamageDiagnostic above - the first
    // instruction at the hook site (`mov eax, dword ptr [0xa33e94]`, the A1-moffs32 encoding, EAX
    // destination only) is exactly 5 bytes (confirmed via disasm: 0x88cb50-0x88cb55), a clean
    // boundary for a 5-byte E9 rel32 patch.
    using CollideObjsFn = int(__fastcall*)(void*, void*, void*, uint32_t*);
    static uint8_t s_collideObjsTrampoline[16];
    static CollideObjsFn Real_CollideObjs = nullptr;

    static int __fastcall CollideObjsHook(void* obj1, void* obj2, void* contact, uint32_t* numContacts) {
        const int result = Real_CollideObjs(obj1, obj2, contact, numContacts);

        // Gated on the same opt-in flag as the other hot-path-per-contact diagnostics (§22.3's
        // joint-count check) rather than always-on - this fires for every dispatched near-
        // collision pair in the game, not just player-vs-tree, and would spam kraken.log during
        // normal play otherwise.
        if (kraken::Config::Instance().jolt_hotpath_diag.value != 0) {
            LOG_INFO("docs §56.5: CollideObjs obj1=%p obj2=%p numContacts=%u result=%d",
                obj1, obj2, numContacts != nullptr ? *numContacts : 0u, result);
        }
        return result;
    }

    static void InstallCollideObjsDiagnostic() {
        void* const orig = reinterpret_cast<void*>(0x0088CB50);
        DWORD oldProtect;
        VirtualProtect(s_collideObjsTrampoline, sizeof(s_collideObjsTrampoline), PAGE_EXECUTE_READWRITE, &oldProtect);
        std::memcpy(s_collideObjsTrampoline, orig, 5);
        s_collideObjsTrampoline[5] = 0xE9;
        *reinterpret_cast<int32_t*>(s_collideObjsTrampoline + 6) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(orig) + 5
            - (reinterpret_cast<uintptr_t>(s_collideObjsTrampoline) + 10));
        VirtualProtect(s_collideObjsTrampoline, sizeof(s_collideObjsTrampoline), oldProtect, &oldProtect);
        Real_CollideObjs = reinterpret_cast<CollideObjsFn>(
            reinterpret_cast<uintptr_t>(s_collideObjsTrampoline));

        routines::Redirect(5, orig, reinterpret_cast<void*>(&CollideObjsHook));
        LOG_INFO("docs §56.5: CollideObjs dispatch diagnostic installed (ai::ColliderKrnl::CollideObjs @ 0x0088CB50)");
    }

    // docs §39: sanity-check the ported wheelmodel_core math in this build - a subset of
    // spring_wheel's own wheelmodel::SelfTest, using ONLY the engine-agnostic core (no ODE, no
    // Jolt). Confirms the pure force routine compiles and behaves before it's wired into the
    // Jolt vehicle. Cases mirror wheel_model.md §6: flat ground -> vertical normal force only;
    // degenerate side (n parallel to axle) -> normal push only; wall/step -> vertical rolling
    // tangent so a spinning wheel produces climb; classify -> ground vs obstacle land in
    // distinct slots.
    static void WheelModelSelfTest() {
        using namespace kraken::fix::wheelmodel;
        WMParams P;
        const float dt = 1.0f / 120.0f, R = 0.5f, tau = 0.1f, m = 500.0f;
        auto az = [](float v, float tol) { return std::fabs(v) < tol; };
        bool ok = true;

        { // Case 1 - flat ground: vertical F only, F_n = k_t*pen
            const vec3 c{0,0,0}, a{1,0,0}, n{0,1,0}, p{0,-R,0};
            WMForce f = GeneralizedContactForce(p, n, 0.02f, 1.0f, c, a, vec3{}, 0.0f, R, tau, m, dt, P);
            const bool pass = f.F.y > 0.0f && az(f.F.x,1.0f) && az(f.F.z,1.0f) && az(f.fpar_w,1e-3f)
                && std::fabs(f.F.y - P.k_t*0.02f) < 1.0f;
            LOG_INFO("docs §39 SelfTest[1] flat: F=(%.1f,%.1f,%.1f) -> %s", f.F.x, f.F.y, f.F.z, pass?"PASS":"FAIL");
            ok = ok && pass;
        }
        { // Case 3 - wall/step: rolling tangent vertical, spinning wheel climbs
            const vec3 c{0,0,0}, a{1,0,0}, n{0,0,1}, p{0,0,0.5f};
            WMForce f = GeneralizedContactForce(p, n, 0.03f, 1.0f, c, a, vec3{}, 40.0f, R, tau, m, dt, P);
            const bool pass = std::fabs(f.fpar_w) > 1e-3f && std::fabs(f.F.y) > 1.0f;
            LOG_INFO("docs §39 SelfTest[3] wall-climb: F=(%.1f,%.1f,%.1f) fpar=%.2f -> %s", f.F.x, f.F.y, f.F.z, f.fpar_w, pass?"PASS":"FAIL");
            ok = ok && pass;
        }
        { // Case 4 - classify ground vs wall into distinct slots
            WMContact cts[2];
            cts[0].p = vec3{0,-R,0}; cts[0].n = vec3{0,1,0}; cts[0].depth = 0.02f;
            cts[1].p = vec3{0,0,R};  cts[1].n = vec3{0,0,1}; cts[1].depth = 0.03f;
            const vec3 c{0,0,0}, u{0,1,0}, a{1,0,0};
            WMGeom gm[2];
            for (int i = 0; i < 2; ++i) gm[i] = ComputeGeom(cts[i], c, u, a, R, 0.15f);
            WMSlots s = Classify(gm, 2);
            const bool pass = s.ground == 0 && s.obstacle == 1;
            LOG_INFO("docs §39 SelfTest[4] classify: ground=%d obstacle=%d -> %s", s.ground, s.obstacle, pass?"PASS":"FAIL");
            ok = ok && pass;
        }
        { // docs §63 / plan §3.5 - Case 5, the STEP 4 GATE. Built before step 4 wires forces, not
          // after, because plan §3.5 calls this «самый опасный класс изменения во всём этапе»:
          // four inputs of this core change MEANING when the force moves from the chassis to the
          // wheel body, the file itself is not edited, and so the compiler says nothing.
          //
          // The one this case pins is `v_p`. The core reconstructs the contact-point velocity as
          // v_c = v_p + Cross(a*omega, r) (wheelmodel_core.hpp:174), so mode 4 must hand it the
          // wheel body's point velocity MINUS that same spin term - otherwise the spin is counted
          // twice and every rolling wheel reports slip it does not have.
          //
          // Constructed so the two readings cannot both pass: a wheel rolling WITHOUT slip has a
          // contact point that is stationary in world space. Fed correctly, the core reconstructs
          // v_c = 0 and produces no longitudinal force. Fed the raw point velocity, it
          // reconstructs v_c = Cross(a*omega, r) and invents a large one. The assertion is that
          // the first is near zero AND the second is not - a test that only checked the first
          // would pass just as happily on a core that ignored v_p altogether.
            const vec3 c{0,0,0}, a{1,0,0}, n{0,1,0}, p{0,-R,0};
            const float omega = 20.0f;                      // rad/s about the axle
            const vec3  r{p.x - c.x, p.y - c.y, p.z - c.z};
            const vec3  spinAtContact = Cross(a * omega, r);
            // Rolling without slip: the contact point is stationary, so v_p is the negated spin.
            const vec3  v_p_correct{-spinAtContact.x, -spinAtContact.y, -spinAtContact.z};

            WMForce fGood = GeneralizedContactForce(p, n, 0.02f, 1.0f, c, a, v_p_correct, omega, R, tau, m, dt, P);
            WMForce fRaw  = GeneralizedContactForce(p, n, 0.02f, 1.0f, c, a, vec3{}, omega, R, tau, m, dt, P);

            const bool rollsFree   = std::fabs(fGood.fpar_w) < 1.0f;
            const bool rawInvents  = std::fabs(fRaw.fpar_w) > 10.0f * std::fabs(fGood.fpar_w) + 1.0f;
            const bool pass = rollsFree && rawInvents;
            LOG_INFO("docs §63 SelfTest[5] v_p basis (STEP 4 GATE): correct fpar=%.3f raw fpar=%.3f -> %s",
                fGood.fpar_w, fRaw.fpar_w, pass ? "PASS" : "FAIL");
            ok = ok && pass;
        }
        { // docs §67 - Case 6, the STEP 5 GATE. The redline target is the exact inverse of the
          // gearbox's own RPM relation, so round-tripping it must return the redline. Every
          // algebra slip in that inversion - a ratio on the wrong side, a missing diffRatio, the
          // gear-4 taper folded in - fails this in one line, and none of them would be visible in
          // a driving run except as "the vehicle is a bit slow".
            constexpr float kRpmScaleT = 108.0f / (2.0f * 3.14159265f);
            const float ratios[3] = { 4.0f, 2.5f, 1.5f };
            const float diffRatio = 3.0f, redline = 6000.0f;   // Bug01's real XML values
            bool spinOk = true;
            float first = 0.0f;
            for (int g = 0; g < 3; ++g) {
                const float omegaTarget = redline / (ratios[g] * diffRatio * kRpmScaleT);
                const float roundTrip   = omegaTarget * ratios[g] * diffRatio * kRpmScaleT;
                if (g == 0) first = omegaTarget;
                spinOk = spinOk && std::fabs(roundTrip - redline) < 0.5f;
            }
            // 29.09 rad/s in gear 0 is not a tuned constant - it is what Bug01's own DiffRatio 3.0
            // and MaxEngineRpm 6000 give, and the lost build's log printed target=29.1.
            spinOk = spinOk && std::fabs(first - 29.088f) < 0.05f;
            LOG_INFO("docs §67 SelfTest[6] redline target (STEP 5 GATE): gear0 target=%.3f rad/s "
                     "(expect 29.088) round-trip -> %s", (double) first, spinOk ? "PASS" : "FAIL");
            ok = ok && spinOk;
        }
        LOG_INFO("docs §39 wheelmodel_core SelfTest overall: %s", ok ? "PASS" : "FAIL");
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.jolt.value == 0 || config.jolt_shadow.value == 0)
            return;

        WheelModelSelfTest();

        if (kraken::fix::jolt::GetPhysicsSystem() == nullptr) {
            LOG_ERROR("Feature enabled but Jolt PhysicsSystem is not initialized ([jolt] enabled=1 is required) - skipping");
            return;
        }

        InstallRamDamageDiagnostic();
        // docs §126: re-enabled after the crash post-mortem - wrong VA (public flag-setter, not
        // the work function) AND an unverified 5-byte trampoline slicing a 7-byte instruction in
        // half. Now targets the actual work function with a disassembly-verified 6-byte
        // boundary. See docs/sec126-get-out-of-difficult-place-jolt-fix.md. Verify via the new
        // get_out_of_difficult_place.txt test-harness trigger (deterministic, no keypress
        // simulation) BEFORE trusting this live again.
        InstallGetOutOfDifficultPlaceHook();
        InstallCollideObjsDiagnostic();
        // docs §23.11: registers VehiclePushbackContactListener as the primary ram-pushback
        // detector - see its comment for why this, not CalcDamageToVehiclesHook above, is the
        // one actually relied on. Jolt keeps only one ContactListener pointer at a time;
        // nothing else in this codebase calls SetContactListener, so this simple assignment
        // can't clobber another registration.
        kraken::fix::jolt::GetPhysicsSystem()->SetContactListener(&g_pushbackContactListener);

        if (config.jolt_apply.value != 0) {
            LOG_INFO("Feature enabled - Jolt WILL DRIVE the player's vehicle this run (player_only=%u, max_speed=%.0fm/s gate)",
                config.jolt_player_only.value, (double) config.jolt_wm4_max_speed_mps.value);
        } else {
            LOG_INFO("Feature enabled - shadowing the player's vehicle in a parallel Jolt VehicleConstraint (read-only, logs divergence only)");
        }
        if (config.jolt_player_only.value == 0 && config.jolt_ai.value != 0) {
            LOG_INFO("Stage 3: all AI vehicles (up to %u) will also be shadowed%s",
                (unsigned) kMaxAiShadowsPerFrame, config.jolt_apply.value != 0 ? " and driven" : "");
        }

        g_activeTuning = DefaultTuningParams();
        if (config.jolt_autotune.value != 0) {
            g_autotune.enabled = true;
            LOG_INFO("Autotune (docs section 8.5/16/20.3) enabled - will run a Nelder-Mead simplex search over suspension/friction "
                     "using testharness's scripted-replay protocol once a save is loaded (up to %u trials)",
                config.jolt_autotune_max_trials.value);
        } else {
            LOG_INFO("Suspension/friction tuning: susp_frequency=%.2fHz susp_damping=%.2f friction_long=%.2f friction_lat=%.2f (from kraken.ini, autotune disabled)",
                (double) g_activeTuning.suspensionFrequency, (double) g_activeTuning.suspensionDamping,
                (double) g_activeTuning.frictionLongScale, (double) g_activeTuning.frictionLatScale);
        }

        routines::ChangeCall((void*) 0x005F4260, &StepSceneHook);
    }
}
