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
    static constexpr uint32_t kMaxWheelsPerVehicleGroupFilter = 16; // generous upper bound - largest real vehicle seen so far (6-wheel truck) is well under this
    static JPH::GroupFilterTable* g_wheelProxyGroupFilter = nullptr; // built once, shared/leaked forever across every rebuild - same convention as g_collisionTester below
    static JPH::GroupFilterTable* GetWheelProxyGroupFilter() {
        if (g_wheelProxyGroupFilter == nullptr) {
            g_wheelProxyGroupFilter = new JPH::GroupFilterTable(kMaxWheelsPerVehicleGroupFilter + 1);
            for (uint32_t i = 1; i <= kMaxWheelsPerVehicleGroupFilter; ++i)
                g_wheelProxyGroupFilter->DisableCollision(0, i);
        }
        return g_wheelProxyGroupFilter;
    }

    // Not in extern/hta's ode.hpp yet - declared locally rather than editing that submodule.
    // Confirmed via disassembly (docs §22.3): counts the body's dxJointNode linked list at
    // [body+0x14]/[node+8], i.e. exactly ODE's real, live joint count - not cached anywhere.
    static const auto dBodyGetNumJoints = (int (__fastcall*)(dxBody*))(0x007C4B90);

    // hta::ai::Wheel::STEERING_LIMIT is declared in the header port (Wheel.hpp) but has no
    // backing definition anywhere in extern/hta (confirmed via grep of Wheel.cpp) - likely
    // folded into an immediate at each use site in the original binary with no addressable
    // storage, so its value hasn't been recovered. Use a plausible generic max steer angle
    // instead for both the wheel's own limit and for normalizing m_steerRadians into the
    // controller's -1..1 "right" input - approximate, not gameplay-affecting at this stage.
    static constexpr float kApproxMaxSteerAngleRadians = 35.0f * (3.14159265f / 180.0f);

    // "Body is flying" gate (docs/jolt-integration-techanalysis.md Stage 2 section, ported
    // from the wheelmodel branch's own safety-rail precedent, source/fix/wheelmodel.cpp on
    // git branch spring_wheel) - if a Jolt chassis' own linear speed exceeds this, or any
    // component of its position/rotation/velocity is non-finite, ApplyJoltToVehicle skips
    // writing this frame entirely and leaves ODE in sole control - self-healing per frame,
    // no persistent "disabled" state, matching wheelmodel's own pattern exactly.
    static constexpr float kMaxAppliedSpeedMps = 60.0f;

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
    struct ShadowState {
        hta::ai::Vehicle*       vehicle    = nullptr; // vehicle this state was last successfully built for
        JPH::BodyID             bodyId;
        JPH::VehicleConstraint* constraint = nullptr;
        std::vector<hta::ai::Wheel*> wheelOrder; // parallel to constraint's internal wheel array, index-for-index
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
        uint32_t consecutiveSlowFrames  = 0;

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
    // Self-collision against the chassis is excluded explicitly via GetWheelProxyGroupFilter
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
        // see GetWheelProxyGroupFilter's comment. Falls back to geometric-only separation
        // (silently, but logged) if wheelIndex somehow exceeds the table's generous slot count.
        if (wheelIndex + 1 <= kMaxWheelsPerVehicleGroupFilter) {
            proxyBody->SetCollisionGroup(JPH::CollisionGroup(GetWheelProxyGroupFilter(), collisionGroupId, wheelIndex + 1));
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
                                         const hta::CVector& pos, const hta::Quaternion& rot);

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
        // rot*massCenter. Placing our Jolt body's origin at GetPosition() and its center-of-
        // mass offset at the same local massCenter vector puts Jolt's world COM at exactly
        // the same point as ODE's actual world COM (dBodyGetPosition).
        hta::CVector localCom = vehicle->GetMassCenter();
        JPH::Vec3 comOffset(localCom.x, localCom.y, localCom.z);

        JPH::ShapeSettings::ShapeResult chassisResult =
            JPH::OffsetCenterOfMassShapeSettings(comOffset, chassisBaseShape.GetPtr()).Create();
        if (chassisResult.HasError()) {
            LOG_ERROR("Shadow chassis shape creation failed (%s): %s", label, chassisResult.GetError().c_str());
            return false;
        }

        JPH::BodyCreationSettings bodySettings(
            chassisResult.Get(),
            JPH::RVec3(pos.x, pos.y, pos.z),
            JPH::Quat(rot.x, rot.y, rot.z, rot.w),
            JPH::EMotionType::Dynamic, kMovingLayer);
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = std::max(vehicle->GetMass(), 100.0f);

        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (body == nullptr) {
            LOG_ERROR("Shadow chassis body creation failed (%s, out of bodies?)", label);
            return false;
        }
        state.bodyId = body->GetID();
        // docs §23.11: lets VehiclePushbackContactListener map a contact straight back to the
        // hta::ai::Vehicle* that owns this DYNAMIC body, without a per-frame lookup - same
        // technique used for the kinematic mirrors below (MirrorOtherVehicles).
        body->SetUserData(reinterpret_cast<uint64_t>(vehicle));
        // docs §37 item 3: chassis is always subgroup 0 within this vehicle's own GroupID - see
        // GetWheelProxyGroupFilter's comment above.
        body->SetCollisionGroup(JPH::CollisionGroup(GetWheelProxyGroupFilter(), collisionGroupId, 0));

        // docs §27: read once here (constant for the whole body, not per-wheel) - used by the
        // per-wheel suspension-frequency derivation below. Valid immediately after CreateBody:
        // EOverrideMassProperties::CalculateInertia computes these as part of shape/mass
        // processing, no AddBody/simulation step required first.
        const JPH::MotionProperties* motionProps = body->GetMotionProperties();
        const float          chassisInvMass    = motionProps->GetInverseMass();
        const JPH::Mat44      chassisInvInertia = motionProps->GetLocalSpaceInverseInertia();

        JPH::VehicleConstraintSettings vehicleSettings;
        std::vector<hta::ai::Wheel*>& wheelOrder = state.wheelOrder; // parallel to vehicleSettings.mWheels; also reused by ApplyJoltToVehicle
        std::vector<uint32_t>& wheelSourceIndex = state.wheelSourceIndex; // parallel to wheelOrder - see its field comment
        wheelOrder.clear();
        wheelOrder.reserve(numWheels);
        wheelSourceIndex.clear();
        wheelSourceIndex.reserve(numWheels);
        state.wheelBaselineJointCount.clear();      // re-captured lazily in ApplyJoltToVehicle (docs §22.3)
        state.wheelHadExtraJointLastFrame.clear();
        state.wheelProxiesBuilt     = false; // docs §38.9: (re)built lazily once this new chassis settles
        state.collisionGroupId      = collisionGroupId;
        state.consecutiveSlowFrames = 0;

        // docs §31: suspension stiffness/damping are now read directly from real ODE data per
        // wheel (see the per-wheel derivation below) - superseding §29's front/rear axle weight-
        // share pre-pass that used to live here. That pre-pass only ever existed to feed §27's
        // target-rest-fraction formula, which is no longer the primary path; kGravity is kept
        // for the rare defensive fallback inside the wheel loop.
        constexpr float kGravity = 9.81f;

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
            ws->mSuspensionMinLength = 0.05f;
            ws->mSuspensionMaxLength = 0.05f + suspensionRange;

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
        state.wheelModelMode = (kraken::Config::Instance().jolt_wheelmodel.value == 2);
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
            InitWheelModelSuspension(physics, state, pos, rot);
            LOG_INFO("Shadow (%s): wheelmodel APPLY mode - VehicleConstraint built but NOT simulated; chassis driven by wheelmodel_core (%zu wheels)",
                label, nw);
        }

        bodyInterface.AddBody(state.bodyId, JPH::EActivation::Activate);

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
        kraken::fix::jolt::GetPhysicsSystem()->GetBodyInterface().ActivateBody(state.bodyId);

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
                                         const hta::CVector& pos, const hta::Quaternion& rot) {
        if (physics == nullptr || state.constraint == nullptr)
            return;
        state.wmGear = 0; // docs §41: start in 1st gear, matching a vehicle about to move off from rest
        state.wmEngineRpm = 0.0f;
        const size_t nw = std::min(state.wmSuspLen.size(), (size_t) state.constraint->GetWheels().size());
        const JPH::RMat44 bxform = JPH::RMat44::sRotationTranslation(
            JPH::Quat(rot.x, rot.y, rot.z, rot.w), JPH::RVec3(pos.x, pos.y, pos.z));
        const JPH::BroadPhaseLayerFilter& bpF = physics->GetDefaultBroadPhaseLayerFilter(kWheelQueryLayer);
        const JPH::DefaultObjectLayerFilter olF = physics->GetDefaultLayerFilter(kWheelQueryLayer);
        for (size_t i = 0; i < nw; ++i) {
            const JPH::WheelSettings* s = state.constraint->GetWheel((JPH::uint) i)->GetSettings();
            const float maxLen = s ? s->mSuspensionMaxLength : 0.5f;
            const float R = s ? s->mRadius : 0.3f;
            float suspLen = maxLen; // airborne fallback
            if (s != nullptr) {
                const JPH::RVec3 attachW  = bxform * s->mPosition;
                const JPH::Vec3  suspDirW = bxform.Multiply3x3(s->mSuspensionDirection).Normalized();
                const float rayLen = maxLen + R + 1.0f;
                JPH::RRayCast ray{ attachW, suspDirW * rayLen };
                JPH::RayCastResult hitR;
                if (physics->GetNarrowPhaseQuery().CastRay(ray, hitR, bpF, olF)) {
                    const float d = rayLen * hitR.mFraction; // attachment -> ground distance
                    suspLen = std::clamp(d - R, s->mSuspensionMinLength, maxLen); // wheel bottom at ground
                }
            }
            state.wmSuspLen[i] = suspLen;
            state.wmRestLen[i] = suspLen; // spring zero-force point = where the wheel starts on the ground
            if (i < state.wmSuspVel.size()) state.wmSuspVel[i] = 0.0f;
            if (i < state.wmOmega.size())   state.wmOmega[i]   = 0.0f;
        }
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
    static float StepWheelModelGearbox(hta::ai::Vehicle* vehicle, ShadowState& state, float dt) {
        float sumOmega = 0.0f;
        int   nDriven  = 0;
        const size_t nw = std::min(state.wmOmega.size(), state.wheelOrder.size());
        for (size_t i = 0; i < nw; ++i) {
            const hta::ai::Wheel* hw = state.wheelOrder[i];
            if (hw != nullptr && hw->m_driven) {
                sumOmega += std::fabs(state.wmOmega[i]);
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

        return wheelTorque;
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
                const int32_t geomClass = dGeomGetClass(geom);
                if (geomClass == dGeomTransformClass) {
                    ++realTransformCount;
                    dxGeom* inner = dGeomTransformGetGeom(geom);
                    const int32_t innerClass = inner != nullptr ? dGeomGetClass(inner) : -1;
                    const float* innerPos = inner != nullptr ? dGeomGetPosition(inner) : nullptr;
                    if (innerPos != nullptr)
                        LOG_INFO("docs §33: real ODE chassis geom (%s) #%d = transform, inner class=%d pos=(%.3f,%.3f,%.3f)",
                            label, realGeomCount, innerClass, (double) innerPos[0], (double) innerPos[1], (double) innerPos[2]);
                    else
                        LOG_INFO("docs §33: real ODE chassis geom (%s) #%d = transform, inner class=%d",
                            label, realGeomCount, innerClass);
                } else {
                    const float* pos = dGeomGetPosition(geom);
                    const double radius = geomClass == dSphereClass ? dGeomSphereGetRadius(geom) : -1.0;
                    LOG_INFO("docs §33: real ODE chassis geom (%s) #%d = class %d (not a transform) pos=(%.3f,%.3f,%.3f) radius=%.3f",
                        label, realGeomCount, geomClass, (double) pos[0], (double) pos[1], (double) pos[2], radius);
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
        if (joltVel.Length() > kMaxAppliedSpeedMps) {
            LOG_WARNING("Shadow apply (%s): Jolt speed %.1f m/s exceeds %.1f m/s cap, skipping this frame (leaving ODE in control)",
                label, (double) joltVel.Length(), (double) kMaxAppliedSpeedMps);
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
        if (vehicle->m_body != nullptr && vehicle->m_body->_id != nullptr) {
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
            if (wheel->m_body != nullptr && wheel->m_body->_id != nullptr) {
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
    };
    static JoltProfileState g_joltProfile;

    // Thin wrapper around kraken::fix::jolt::StepPhysics - identical call/args/return (none) to
    // the call it replaces; only measures wall time when profiling is on.
    static void StepPhysicsProfiled(float elapsedTime) {
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

        LOG_INFO("[jolt_profile] physics_update_avg_ms=%.2f applyvehicle_avg_ms=%.2f applyvehicle_calls_per_frame=%.2f frames=%llu",
            physicsAvgMs, applyAvgMs, callsPerFrame, (unsigned long long) g_joltProfile.frames);

        g_joltProfile.physicsUpdateMs   = 0.0;
        g_joltProfile.applyVehicleMs    = 0.0;
        g_joltProfile.applyVehicleCalls = 0;
        g_joltProfile.frames            = 0;
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
        if (state.wheelModelMode) {
            StepWheelModel(vehicle, state, label, dt);
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
        // docs §39.2: wheelmodel mode has no VehicleConstraint, so the constraint-driven
        // Jolt->ODE writeback (ApplyJoltToVehicle) can't run - it's a pure shadow for now
        // (evaluate the model's own behaviour vs ODE; writeback is a later step).
        if (allowApply && !state.wheelModelMode)
            ApplyJoltToVehicleProfiled(vehicle, state, label);
        AccumulateForAutotune(vehicle, state);

        ++state.frameCounter;
        constexpr uint64_t kLogIntervalFrames = 60; // ~once/second at 60fps, ~twice/second at 30 - avoids flooding kraken.log
        if (state.frameCounter % kLogIntervalFrames == 0) {
            LogDivergence(vehicle, state, label);
            LogWheelState(vehicle, state, label);
            LogRealOdeWheelState(vehicle, label);
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
    // 0, 16}`), which clamps the ini value to 0..16 before InitAiShadowsIfNeeded ever selects
    // vehicles, so g_aiShadows/g_aiTargets never actually grow past this. Sized as a fixed stack
    // array (not a per-frame heap allocation - this whole refactor exists to keep this hot path
    // allocation-free) with a defensive runtime clamp below in case that config limit is ever
    // loosened without this array being resized to match.
    static constexpr size_t kMaxAiShadowsPerFrame = 16;

    // Forward-declared: defined alongside VehiclePushbackContactListener (docs §23.11), well
    // after UpdateShadow in this file, but needs calling from UpdateShadow's Pass 2 below,
    // right after the one place PhysicsSystem::Update() actually runs for this frame.
    static void DrainPendingPushbacks();

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

        hta::ai::Vehicle* playerVehicle = GetPlayerVehicle();

        // Stage 3 requires player_only=0 - player_only=1 (the default) means "only the
        // player, full stop", so ai_count is deliberately inert unless that's turned off too.
        // Applying (vs. shadow-only logging) additionally still requires apply=1, same as the
        // player path.
        const uint32_t aiCount = config.jolt_player_only.value == 0 ? config.jolt_ai_count.value : 0;
        if (aiCount > 0)
            InitAiShadowsIfNeeded(playerVehicle, aiCount);

        assert(g_aiShadows.size() <= kMaxAiShadowsPerFrame);
        const size_t aiShadowCount = aiCount > 0 ? std::min(g_aiShadows.size(), kMaxAiShadowsPerFrame) : 0;

        bool playerLive = false;
        bool aiLive[kMaxAiShadowsPerFrame] = {};
        char aiLabels[kMaxAiShadowsPerFrame][16]; // filled in pass 1, reused as-is (same index) in pass 3

        // --- Pass 1 (pre-step) ---
        if (playerVehicle != nullptr)
            playerLive = UpdateOneVehiclePreStep(playerVehicle, g_playerShadow, "player", 0, elapsedTime);

        for (size_t i = 0; i < aiShadowCount; ++i) {
            std::snprintf(aiLabels[i], sizeof(aiLabels[i]), "ai%zu", i);
            // docs §37 item 3: player is always GroupID 0 (above); AI shadow slot i gets i+1 -
            // a stable, distinct GroupID per vehicle so GetWheelProxyGroupFilter's shared table
            // only ever suppresses a vehicle's own chassis-vs-own-proxy pairs, never cross-
            // vehicle ones (see its comment).
            aiLive[i] = UpdateOneVehiclePreStep(g_aiTargets[i], g_aiShadows[i], aiLabels[i], static_cast<uint32_t>(i) + 1, elapsedTime);
        }

        // --- Pass 2: step the shared PhysicsSystem exactly once ---
        bool anyLive = playerLive;
        for (size_t i = 0; i < aiShadowCount; ++i)
            anyLive = anyLive || aiLive[i];
        if (anyLive) {
            MirrorOtherVehicles(playerVehicle, elapsedTime);
            StepPhysicsProfiled(elapsedTime);
            // docs §23.11: safe to drain here - PhysicsSystem::Update() (inside
            // StepPhysicsProfiled) has fully returned, so every worker thread that may have
            // queued a contact via VehiclePushbackContactListener during this step has
            // finished; back to single-threaded, safe to call into ODE (PhysicObj::AddImpulse
            // etc.) again.
            DrainPendingPushbacks();
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
        scene->StepScene(elapsedTime);
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
            InitWheelModelSuspension(physics, g_playerShadow, pos, rot);
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
    // extra layer, but empirically (live testing this session, apply=1, several minutes across
    // both the natural AI pile-up and a dedicated ram_test approach) this hook NEVER fired with
    // the player as a party while apply=1 was active, despite firing readily under apply=0 -
    // i.e. whatever dispatches CollideVehiclePartAndVehiclePart appears to skip a body once
    // ApplyJoltToVehicle's per-frame DisablePhysics() takes effect, unlike ai::NearCallback
    // (confirmed in §22.3 to NOT respect body-enabled state). Not conclusively root-caused -
    // would need more disassembly of the dispatch path than this pass budgeted - so this stays
    // installed as a freebie, but VehiclePushbackContactListener below (registered directly on
    // Jolt's own PhysicsSystem, the mechanism already proven to fire via "other vehicles push
    // the player") is the detector actually relied on.
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

    class VehiclePushbackContactListener final : public JPH::ContactListener {
    public:
        void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold, JPH::ContactSettings&) override {
            HandleContact(inBody1, inBody2, inManifold);
        }
        void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2,
                const JPH::ContactManifold& inManifold, JPH::ContactSettings&) override {
            HandleContact(inBody1, inBody2, inManifold);
        }

    private:
        static void HandleContact(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold) {
            const bool body1Dynamic = inBody1.GetMotionType() == JPH::EMotionType::Dynamic;
            const bool body2Dynamic = inBody2.GetMotionType() == JPH::EMotionType::Dynamic;
            if (body1Dynamic == body2Dynamic)
                return; // both or neither dynamic - native Jolt already resolves dynamic-vs-dynamic correctly; static/kinematic-vs-kinematic never needs a push
            const JPH::Body& dynamicBody   = body1Dynamic ? inBody1 : inBody2;
            const JPH::Body& kinematicBody = body1Dynamic ? inBody2 : inBody1;
            if (kinematicBody.GetMotionType() != JPH::EMotionType::Kinematic)
                return; // the non-dynamic side must specifically be a vehicle mirror, not a static body

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
        // docs §23.11: registers VehiclePushbackContactListener as the primary ram-pushback
        // detector - see its comment for why this, not CalcDamageToVehiclesHook above, is the
        // one actually relied on. Jolt keeps only one ContactListener pointer at a time;
        // nothing else in this codebase calls SetContactListener, so this simple assignment
        // can't clobber another registration.
        kraken::fix::jolt::GetPhysicsSystem()->SetContactListener(&g_pushbackContactListener);

        if (config.jolt_apply.value != 0) {
            LOG_INFO("Feature enabled - Jolt WILL DRIVE the player's vehicle this run (player_only=%u, max_speed=%.0fm/s gate)",
                config.jolt_player_only.value, (double) kMaxAppliedSpeedMps);
        } else {
            LOG_INFO("Feature enabled - shadowing the player's vehicle in a parallel Jolt VehicleConstraint (read-only, logs divergence only)");
        }
        if (config.jolt_player_only.value == 0 && config.jolt_ai_count.value > 0) {
            LOG_INFO("Stage 3: up to %u AI vehicle(s) will also be shadowed%s",
                config.jolt_ai_count.value, config.jolt_apply.value != 0 ? " and driven" : "");
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
