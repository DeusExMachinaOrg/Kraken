#define LOGGER "joltshadow"

// Must precede any transitive <windows.h> include (via config.hpp/routines.hpp/stdafx.hpp)
// in this translation unit - same NOMINMAX gotcha already documented in fix::jolt (jolt.cpp):
// windows.h's min/max macros mangle Jolt's own min()/max() calls into syntax errors otherwise.
#define NOMINMAX

#include "ext/logger.hpp"
#include "fix/joltshadow.hpp"
#include "fix/jolt.hpp"
#include "config.hpp"
#include "routines.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

#include "hta/ai/CServer.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
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
        float suspensionFrequency; // Hz, JPH::SpringSettings::mFrequency (FrequencyAndDamping mode)
        float suspensionDamping;   // damping RATIO (0=undamped, 1=critical), JPH::SpringSettings::mDamping
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
        uint64_t                frameCounter    = 0;
        uint32_t                builtGeneration = 0; // g_tuningGeneration at the time this state's shadow was (re)built
    };

    static ShadowState              g_playerShadow;
    static std::vector<hta::ai::Vehicle*> g_aiTargets;  // which vehicle each g_aiShadows[i] tracks (fixed at selection time)
    static std::vector<ShadowState> g_aiShadows;
    static bool                     g_aiShadowsInitialized = false;

    static JPH::VehicleCollisionTester* g_collisionTester = nullptr; // built once, shared/leaked forever across every rebuild
    static uint32_t                     g_shadowGeneration = 0;      // how many shadow vehicles have been built this process, for logging only

    static hta::ai::Vehicle* GetPlayerVehicle() {
        hta::ai::DynamicScene* scene = hta::ai::DynamicScene::Instance();
        if (scene == nullptr)
            return nullptr;
        return scene->GetVehicleControlledByPlayer();
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
    static bool BuildShadow(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        const uint32_t numWheels = vehicle->GetNumWheels();
        if (numWheels == 0) {
            LOG_WARNING("Shadow build skipped (%s): vehicle has no wheels", label);
            return false;
        }

        JPH::PhysicsSystem* physics = kraken::fix::jolt::GetPhysicsSystem();
        JPH::BodyInterface&  bodyInterface = physics->GetBodyInterface();

        // Chassis: approximate as a single box using the same mass-box dimensions ODE's own
        // RefreshMass (ComplexPhysicObj::RefreshMass, VA 0x2BCAC0) feeds into dMassSetBoxTotal
        // (confirmed via disassembly - m_massSize is the full box side lengths, m_massShape
        // defaults to MS_BOX) - not the real multi-part collision (Chassis/Cabin/Basket boxes
        // + spherical wheels), but close enough for a divergence-logging shadow.
        const hta::ai::VehiclePrototypeInfo* protoInfo = vehicle->GetPrototypeInfo();
        hta::CVector massSize = protoInfo != nullptr ? protoInfo->m_massSize : hta::CVector(2.0f, 1.0f, 4.0f);
        JPH::Vec3 halfExtents(
            std::max(massSize.x * 0.5f, 0.1f),
            std::max(massSize.y * 0.5f, 0.1f),
            std::max(massSize.z * 0.5f, 0.1f));

        // PhysicObj::m_massCenter (local-space) is exactly the offset PhysicObj::GetPosition()
        // itself subtracts from the raw ODE body origin (confirmed via disassembly of
        // PhysicObj::GetPosition, VA 0x5FC410) - i.e. rawOdeOrigin = GetPosition() +
        // rot*massCenter. Placing our Jolt body's origin at GetPosition() and its center-of-
        // mass offset at the same local massCenter vector puts Jolt's world COM at exactly
        // the same point as ODE's actual world COM (dBodyGetPosition).
        hta::CVector localCom = vehicle->GetMassCenter();
        JPH::Vec3 comOffset(localCom.x, localCom.y, localCom.z);

        JPH::ShapeSettings::ShapeResult chassisResult =
            JPH::OffsetCenterOfMassShapeSettings(comOffset, new JPH::BoxShape(halfExtents)).Create();
        if (chassisResult.HasError()) {
            LOG_ERROR("Shadow chassis shape creation failed (%s): %s", label, chassisResult.GetError().c_str());
            return false;
        }

        hta::CVector    pos = vehicle->GetPosition();
        hta::Quaternion rot = vehicle->GetRotation(); // literal dBodyGetQuaternion passthrough (confirmed)

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

        JPH::VehicleConstraintSettings vehicleSettings;
        std::vector<hta::ai::Wheel*>& wheelOrder = state.wheelOrder; // parallel to vehicleSettings.mWheels; also reused by ApplyJoltToVehicle
        wheelOrder.clear();
        wheelOrder.reserve(numWheels);

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

            // WheelPrototypeInfo::m_suspensionRange (confirmed field, offset 0x8c) is the
            // only suspension-travel data exposed anywhere at runtime - ODE's own
            // CFM/ERP-form softness params (m_suspensionCFM/ERP) don't translate to Jolt's
            // frequency/damping spring model without a conversion this session didn't derive,
            // so travel range comes from real per-wheel data but the spring's own stiffness/
            // damping is a single global knob (g_activeTuning) instead - see below.
            const hta::ai::WheelPrototypeInfo* wheelProto = wheel->GetPrototypeInfo();
            const float suspensionRange = wheelProto != nullptr ? std::max(wheelProto->m_suspensionRange, 0.05f) : 0.2f;
            ws->mSuspensionMinLength = 0.05f;
            ws->mSuspensionMaxLength = 0.05f + suspensionRange;

            // Suspension stiffness/damping and tire friction (docs §8.5/§16) - tunable via
            // [jolt_harness], searchable via the in-process autotuner (AutoTune* below), applied
            // identically to every wheel of every shadow vehicle (no per-wheel/per-vehicle
            // tuning - a single global "feel" knob, matching the scope this increment targets).
            ws->mSuspensionSpring.mMode      = JPH::ESpringMode::FrequencyAndDamping;
            ws->mSuspensionSpring.mFrequency = g_activeTuning.suspensionFrequency;
            ws->mSuspensionSpring.mDamping   = g_activeTuning.suspensionDamping;
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
        if (g_collisionTester == nullptr)
            g_collisionTester = new JPH::VehicleCollisionTesterRay(kMovingLayer);
        state.constraint->SetVehicleCollisionTester(g_collisionTester);

        physics->AddConstraint(state.constraint);
        physics->AddStepListener(state.constraint);

        bodyInterface.AddBody(state.bodyId, JPH::EActivation::Activate);

        ++g_shadowGeneration;
        state.builtGeneration = g_tuningGeneration;
        LOG_INFO("Shadow vehicle #%u built (%s): %u wheels (%d driven axle(s)), mass=%.1f, chassis=%.2fx%.2fx%.2f, susp=%.2fHz/%.2f, friction=%.2f/%.2f",
            g_shadowGeneration, label, (uint32_t) vehicleSettings.mWheels.size(), drivenAxles, (double) bodySettings.mMassPropertiesOverride.mMass,
            (double) (halfExtents.GetX() * 2.0f), (double) (halfExtents.GetY() * 2.0f), (double) (halfExtents.GetZ() * 2.0f),
            (double) g_activeTuning.suspensionFrequency, (double) g_activeTuning.suspensionDamping,
            (double) g_activeTuning.frictionLongScale, (double) g_activeTuning.frictionLatScale);

        return true;
    }

    static void UpdateShadowInputs(hta::ai::Vehicle* vehicle, ShadowState& state) {
        if (state.constraint == nullptr)
            return;

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

        vehicle->DisablePhysics();
        vehicle->SetPositionSelf(hta::CVector(joltPos.GetX(), joltPos.GetY(), joltPos.GetZ()));
        vehicle->SetRotationSelf(hta::Quaternion(joltRot.GetX(), joltRot.GetY(), joltRot.GetZ(), joltRot.GetW()));
        vehicle->SetLinearVelocity(hta::CVector(joltVel.GetX(), joltVel.GetY(), joltVel.GetZ()));
        vehicle->SetAngularVelocity(hta::CVector(joltAngVel.GetX(), joltAngVel.GetY(), joltAngVel.GetZ()));

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

            wheel->DisablePhysics();
            wheel->SetPositionSelf(hta::CVector(wheelPos.GetX(), wheelPos.GetY(), wheelPos.GetZ()));
            wheel->SetRotationSelf(hta::Quaternion(wheelRot.GetX(), wheelRot.GetY(), wheelRot.GetZ(), wheelRot.GetW()));
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
    static bool UpdateOneVehiclePreStep(hta::ai::Vehicle* vehicle, ShadowState& state, const char* label) {
        // Rebuilds on either a vehicle swap (level reload, vehicle switch) or a tuning-parameter
        // change (g_tuningGeneration bumped by SetTuningOverride - the autotuner uses this to
        // apply a new candidate suspension/friction set between trials without needing to
        // recreate ShadowState itself). See BuildShadow's comment for why the previous
        // constraint/body is abandoned rather than torn down either way.
        if (vehicle != state.vehicle || state.builtGeneration != g_tuningGeneration) {
            if (!BuildShadow(vehicle, state, label))
                return false; // vehicle data can be not-yet-fully-initialized right after a level
                              // load - just keep retrying on later frames
            state.vehicle = vehicle;
            state.frameCounter = 0;
        }

        UpdateShadowInputs(vehicle, state);
        return true;
    }

    // Second half of the per-vehicle tick (see UpdateOneVehiclePreStep above) - runs AFTER
    // UpdateShadow's single shared StepPhysicsProfiled call for this frame. Only called for
    // vehicles whose pre-step returned true.
    static void UpdateOneVehiclePostStep(hta::ai::Vehicle* vehicle, ShadowState& state, bool allowApply, const char* label) {
        if (allowApply)
            ApplyJoltToVehicleProfiled(vehicle, state, label);
        AccumulateForAutotune(vehicle, state);

        ++state.frameCounter;
        constexpr uint64_t kLogIntervalFrames = 60; // ~once/second at 60fps, ~twice/second at 30 - avoids flooding kraken.log
        if (state.frameCounter % kLogIntervalFrames == 0)
            LogDivergence(vehicle, state, label);
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
    static const float kAxisMin[4] = {0.3f, 0.05f, 0.2f, 0.2f};
    static const float kAxisMax[4] = {8.0f, 1.5f,  3.0f, 3.0f};

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
    static const float kInitialSimplexStep[4] = {0.3f, 0.1f, 0.15f, 0.15f};

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

        LOG_INFO("Autotune trial %u/%u starting (%s): susp=%.2fHz/%.2f friction=%.2f/%.2f",
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

        LOG_INFO("Autotune: starting Nelder-Mead simplex search, up to %u trial(s), baseline susp=%.2fHz/%.2f friction=%.2f/%.2f, "
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
            playerLive = UpdateOneVehiclePreStep(playerVehicle, g_playerShadow, "player");

        for (size_t i = 0; i < aiShadowCount; ++i) {
            std::snprintf(aiLabels[i], sizeof(aiLabels[i]), "ai%zu", i);
            aiLive[i] = UpdateOneVehiclePreStep(g_aiTargets[i], g_aiShadows[i], aiLabels[i]);
        }

        // --- Pass 2: step the shared PhysicsSystem exactly once ---
        bool anyLive = playerLive;
        for (size_t i = 0; i < aiShadowCount; ++i)
            anyLive = anyLive || aiLive[i];
        if (anyLive)
            StepPhysicsProfiled(elapsedTime);

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

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.jolt.value == 0 || config.jolt_shadow.value == 0)
            return;

        if (kraken::fix::jolt::GetPhysicsSystem() == nullptr) {
            LOG_ERROR("Feature enabled but Jolt PhysicsSystem is not initialized ([jolt] enabled=1 is required) - skipping");
            return;
        }

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
