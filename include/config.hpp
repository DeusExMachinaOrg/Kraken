#ifndef __KRAKEN_CONFIG_HPP__
#define __KRAKEN_CONFIG_HPP__

#include "stdafx.hpp"
#include <vector>
#include <unordered_map>
#include "configstructs.hpp"

namespace kraken {
    typedef std::vector<configstructs::WareUnits> WareUnitsList;

    template <typename T>
    struct ConfigValue {
        const char* section;
        const char* key;
        T           value;
        bool        limited;
        T           min;
        T           max;
    };

    template<>
    struct ConfigValue<std::vector<std::string>> {
        const char* section;
        const char* keyPrefix; // e.g. "Script_"
        std::vector<std::string> value;
    };

    template<>
    struct ConfigValue<WareUnitsList> {
        WareUnitsList value;
    };

    class Config {
    private:
        static Config* INSTANCE;
    public:
        static Config& Instance(void) { return *Config::INSTANCE; };
    public:
        // Graphics
        ConfigValue<uint32_t> save_width;
        ConfigValue<uint32_t> save_height;
        ConfigValue<uint32_t> view_resolution;

        // Global Physic Constants
        ConfigValue<float>                    gravity;
        ConfigValue<float>                    contact_surface_layer;
        ConfigValue<float>                    cfm;
        ConfigValue<float>                    erp;

        // Constants
        ConfigValue<uint32_t>                 price_fuel;
        ConfigValue<uint32_t>                 price_paint;
        ConfigValue<float>                    keep_throttle;
        ConfigValue<float>                    handbrake_power;
        ConfigValue<float>                    brake_power;
        ConfigValue<uint32_t>                 friend_damage;
        ConfigValue<uint32_t>                 auto_brake_angle; // If angle to the next path point is bigger than this value (in degrees), autobrake will be applied
        ConfigValue<std::vector<std::string>> lua_scripts;
        ConfigValue<int32_t>                  lua_enabled;
        ConfigValue<int32_t>                  posteffectreload;
        ConfigValue<WareUnitsList>            ware_units;
        ConfigValue<uint32_t>                 ultrawide;
        ConfigValue<uint32_t>                 objcontupgrade;
        ConfigValue<uint32_t>                 cardan_fix;
        ConfigValue<uint32_t>                 tactics;
        ConfigValue<uint32_t>                 tactics_lock;
        ConfigValue<uint32_t>                 wares;
        ConfigValue<uint32_t>                 log_debug; // 0 - debug, 1 - info, 2 - warning, 3 - error, 4 - panic, 5 - none
        ConfigValue<uint32_t>                 cctl_leak_fix;
        ConfigValue<uint32_t>                 mortarvolleylauncherfix;
        ConfigValue<uint32_t>                 gunlights;
        ConfigValue<uint32_t>                 boss01_fast_charge;
        ConfigValue<uint32_t>                 testharness;
        ConfigValue<uint32_t>                 testharness_autoload;
        ConfigValue<uint32_t>                 testharness_god_mode;
        ConfigValue<uint32_t>                 testharness_perfmon;
        ConfigValue<float>                    testharness_perfmon_interval;
        // Debug-only, one-shot: at this scenario clock time, call DetachFromPhysicObj() on the
        // target vehicle's wheel 0 - lets a real wheel-tear event be triggered deterministically
        // for testing (docs §22.2's UAF fix), instead of needing to wait for real combat damage.
        // -1 (default) disables it.
        ConfigValue<float>                    testharness_tear_wheel_at_t;
        // Debug-only: redirects the scenario/telemetry harness to drive the nearest OTHER live
        // vehicle instead of the player, teleporting it directly behind the player (facing the
        // same way, so throttle alone drives it straight into the player) and holding the player
        // parked (throttle=0, handbrake) every tick. Lets an ODE-vs-Jolt-disabled-body ram be
        // reproduced deterministically instead of waiting for real AI/combat (docs §22.4).
        ConfigValue<uint32_t>                 testharness_ram_test;
        ConfigValue<float>                    testharness_ram_test_offset;
        // docs §46e (task #59): its own section, deliberately NOT under [jolt]/[jolt_harness] -
        // this diagnostic (fix::odediag) hooks independently of [jolt]enabled specifically so it
        // can log ODE chassis coordinates (GetPosition/GetMassCenterPosition) with Jolt fully
        // OFF, for a direct jolt=0 vs jolt=1 comparison across two separate launches.
        ConfigValue<uint32_t>                 ode_diag;
        ConfigValue<uint32_t>                 jolt;
        ConfigValue<uint32_t>                 jolt_threads;
        ConfigValue<uint32_t>                 jolt_shadow;
        ConfigValue<uint32_t>                 jolt_apply;
        ConfigValue<uint32_t>                 jolt_player_only;
        ConfigValue<uint32_t>                 jolt_ai;
        ConfigValue<float>                    jolt_susp_frequency;
        ConfigValue<float>                    jolt_susp_rest_fraction;
        ConfigValue<float>                    jolt_susp_damping;
        ConfigValue<float>                    jolt_susp_reference_hz;
        // Per-prototype residual suspension multipliers. ODE's CFM/ERP remain the base
        // values; these are only the final vehicle-specific fine-tune on top of them.
        ConfigValue<float>                    jolt_susp_ural_frequency;
        ConfigValue<float>                    jolt_susp_ural_damping;
        ConfigValue<float>                    jolt_susp_bug_frequency;
        ConfigValue<float>                    jolt_susp_bug_damping;
        ConfigValue<float>                    jolt_susp_molokovoz_frequency;
        ConfigValue<float>                    jolt_susp_molokovoz_damping;
        ConfigValue<float>                    jolt_susp_ural_max_scale;
        ConfigValue<float>                    jolt_susp_bug_max_scale;
        ConfigValue<float>                    jolt_susp_molokovoz_max_scale;
        ConfigValue<uint32_t>                  jolt_wheel_proxy;
        ConfigValue<uint32_t>                 jolt_collision_cylinder;
        ConfigValue<uint32_t>                 jolt_wheelmodel;            // docs §39: spring_wheel model on the Jolt vehicle. 0=off, 1=log-only eval, 2=apply (drive chassis, no VehicleConstraint), 4=wheel bodies (docs §58, Этап 1)
        // docs §58 (Этап 1, шаг 2): splits the suspension travel range into compression and droop
        // headroom for the SixDOF TranslationZ limits, relative to the SPAWN pose. 0.5 puts the
        // spawn point in the middle of travel. The compiled default of the lost build is not
        // recoverable (MSVC loads float config defaults from .rdata via xmm rather than as
        // immediates), so 0.5 here is the value its kraken.ini carried; the effective clamp to
        // [0.05, 0.95] lives in the code that reads it, exactly as the recovered build did it.
        ConfigValue<float>                    jolt_wm4_compress_fraction;
        // docs §46g/§46i (task #59): scales mSuspensionMaxLength from the invented
        // "0.05 + suspensionRange" ceiling (ODE has no real hard stop to derive it from - see
        // that constant's own comment in joltshadow.cpp). Vehicle-specific calibration uses the
        // full authored range (1.0); the global value remains a fallback for uncalibrated types.
        ConfigValue<float>                    jolt_wm4_susp_max_scale;
        // docs §75: where the SixDOF anchor sits. 0 (default, and what the recovered build
        // shipped) anchors at the WHEEL CENTRE, which is what makes the travel limits and the
        // motor target "relative to spawn" coherent. 1 restores the plan's original wording,
        // anchoring at the chassis mount - kept as the lever back to it. NOTE the plan §2.2 and
        // the recovered binary disagree here and the binary is newer.
        ConfigValue<uint32_t>                 jolt_wm4_joint_at_mount;
        // docs §95.4/§96 (task #126): whether mode 4 takes the wheel bodies' mass back OUT of the
        // chassis body. 1 (default) is the CORRECT total; 0 restores the double count and exists
        // only to measure what the fix changed. ai::Vehicle::GetMass (RVA 0x1d5bd0, disassembled)
        // returns the chassis body's mass PLUS every wheel body's mass, while ODE's own chassis
        // body is set from _CalcMassForBody, which excludes wheels - so handing GetMass() to the
        // Jolt chassis and then adding real wheel bodies counts the wheels twice. The lost build
        // shipped that bug for months: Bug01 168 kg against ODE's 132 (+27%), Molokovoz 211 vs
        // 167 (+26%), Scout01 106 vs 100 (+6%). Mode 2 builds no wheel bodies and was always
        // correct, which is why docs §86's step-7 comparison was confounded.
        ConfigValue<uint32_t>                 jolt_wm4_chassis_mass_excl_wheels;
        // docs §67 (Этап 1, шаг 5): the spin degree of freedom. 1 (default, and what the lost
        // build shipped) makes RotationX a free twist axis and drives it with a TORQUE-LIMITED
        // VELOCITY MOTOR - the literal semantics of ai::Vehicle::_KeepGearBox, which drives the
        // ODE Hinge2 through dParamVel2/dParamFMax2. 0 restores the step-4 topology where the
        // wheel is locked to the chassis about its axle, and is the rollback lever for the whole
        // step. NOT the same thing as [wheelmodel] own_spin, which gates mode 2's scalar spin
        // integrator: that one stays, because mode 2 remains the rollback path on every step.
        ConfigValue<uint32_t>                 jolt_wm4_spin;
        // docs §68 (Этап 1, шаг 6): the steering degree of freedom. 1 turns RotationZ into a
        // limited swing axis with a Position motor for wheels the prototype marks steerable; 0
        // leaves RotationZ MakeFixedAxis for every wheel, which is the step-5 topology. Not
        // per-wheel: hw->m_steering still decides which wheels are steerable, independently.
        //
        // DEFAULT 0, and that is a deliberate divergence from the lost build, which shipped 1.
        // Step 6 is implemented and its sign, plumbing and tracking are verified (the motor
        // follows to 0.10 deg when undisturbed), but it does NOT pass the plan's own acceptance
        // gate: steered wheels are still driven to the invented +-35 deg stop by contact-patch
        // torque, and merely enabling it moves the straight-line baseline from ratio 1.01 /
        // pos-div 2.77 m (step 5, 3 repeats) to 0.92 / 24.80 m. The recovered build did not pass
        // that bar either - docs §98/§99. Defaulting a step ON when it measurably degrades the
        // last verified one would hide a regression behind a completed checkbox; the lever is
        // here so the work that fixes it can turn it on in one line.
        ConfigValue<uint32_t>                 jolt_wm4_steer;
        // docs §101 (§94 ported): the two ARCADE ASSISTS ai::Vehicle::_ApplyStabilizingForces
        // applies to every ODE vehicle every frame and the shadow never reproduced - a
        // speed-proportional downforce and a steering-proportional yaw torque. This is not an
        // engine artefact: PressingForce and DriftCoeff sit in vehicles.xml next to DiffRatio and
        // MaxEngineRpm, which we already port. We took the drivetrain and skipped the handling
        // layer. 1 applies them to the shadow chassis, 0 is the pre-§94 behaviour.
        //
        // DEFAULT 1, but only as part of a BUNDLE with wm4_governor and wm4_soildrag - see §101
        // and §103. On its own this key measured clearly WORSE (ratio 1.01 -> 1.09, pos-div 2.62
        // -> 8.03): the downforce adds grip, the drive servo turns grip into speed, and nothing
        // took the speed back. With all three on, the layer is complete (§95's reverse pass proved
        // the force-site list is closed) and the post-throttle attitude blow-up halves, 167.6 deg
        // -> 80.0 deg. Turning this one on alone is a measured regression, not a partial win.
        ConfigValue<uint32_t>                 jolt_wm4_assists;
        // docs §109: sub-lever of wm4_assists that gates ONLY the yaw torque, leaving the downforce
        // alone. The two assists share one binary function but are unrelated mechanisms - one loads
        // the tyres, the other turns the chassis past them - and wm4_assists cannot separate them:
        // switching it off removes the downforce too, and the downforce is the half that carried
        // §103's win. Without this key the yaw torque cannot be measured at all, and it is the one
        // piece of the ported handling layer that never has been (§101/§103 both measured the
        // downforce and said so).
        //
        // DEFAULT 1 = the faithful port, i.e. no behaviour change from before this key existed. It
        // is a MEASUREMENT lever, not a tuning one; the sub-gate exists so the arm can be built.
        ConfigValue<uint32_t>                 jolt_wm4_assist_yaw;
        // docs §120 (§117/§118): ENGINE BRAKING. The reference's Hinge2 force cap has no throttle
        // factor - it is built from the gearing and the WHEEL SURFACE SPEED - so off throttle the
        // servo still brakes the wheel toward the engine-RPM target. The port gates its cap on
        // |throttle|, releasing the wheel entirely: measured, the shadow keeps 18% of its wheel
        // speed over an 8 s coast where the reference reaches zero.
        //
        // DEFAULT 0 because the MAGNITUDE is not recovered - §118 resolved the shape of the
        // reference's expression but left two of its terms unidentified, so there is no faithful
        // constant to port and inventing one silently would be worse than leaving a lever. The
        // scale multiplies the gearbox's own per-wheel torque.
        ConfigValue<uint32_t>                 jolt_wm4_engine_brake;
        ConfigValue<float>                    jolt_wm4_engine_brake_scale;
        // docs §112 (Этап 1, шаг 8): Jolt's PhysicsSettings::mStepListenersBatchSize. Step
        // listeners are handed to workers in batches of this size (Jolt default 8); with 75 shadows
        // each owning a VehicleStepListener that is ~10 batches, so this sets how finely the wheel
        // work can spread across cores. 0 = leave Jolt's default untouched.
        //
        // A GLOBAL knob, not a per-shadow one - it is a PhysicsSettings field affecting every step
        // listener in the world, which is why the plan calls it out as needing its own measurement
        // rather than being folded into a shadow-side lever.
        ConfigValue<uint32_t>                 jolt_step_listener_batch;
        // docs §112 (Этап 1, шаг 8): let a shadow whose REFERENCE has been idle fall asleep instead
        // of force-waking every body every frame. At 74 AI shadows the forced-awake path keeps 75
        // chassis and ~300 wheel bodies permanently in the active set.
        //
        // DEFAULT 0, because getting this wrong re-arms §97's latch: a mode-4 wheel body is driven
        // only from a step listener, Jolt skips step listeners for sleeping bodies, so a wrong wake
        // condition means "asleep forever", silently, for the rest of the session. The wake
        // condition reads the REFERENCE's inputs and speed - never the shadow's own velocity, which
        // a sleeping shadow reports as zero by construction.
        ConfigValue<uint32_t>                 jolt_wm4_sleep;
        // docs §102 (§95.3 ported): the speed governor _KeepGearBox applies before handing torque
        // to the joint - above GetMaxSpeed() (player/attacking) or m_cruisingSpeed (AI), measured
        // on the WHEEL SURFACE speed and only while the driver is not braking, the drive is cut to
        // zero. Its own lever rather than a part of wm4_assists, so the two can be A/B'd
        // separately - §101 measured that the assists without this one are a net loss.
        ConfigValue<uint32_t>                 jolt_wm4_governor;
        // docs §103 (§95.3 ported): soil rolling drag, F = -SoilProps::m_resistance * wheelMass *
        // v / R, applied per wheel at the end of CollideWheelAndLandscape. The last un-ported
        // every-frame force, and the third lever of the same bundle as wm4_assists/wm4_governor.
        ConfigValue<uint32_t>                 jolt_wm4_soildrag;
        // Stage 2 §2.5: "body is flying" write-back safety cap, was a hardcoded 60.0 constexpr
        // (kMaxAppliedSpeedMps). The cross-vehicle sweep found ArcadeScout01 - deliberately much
        // faster than the rest of the roster (vehicles.xml: MaxEngineRpm 10000 vs Scout's 7000,
        // DriftCoeff ~0) - legitimately clearing 60 m/s under sustained full throttle and tripping
        // the gate every frame while at speed. Default raised to comfortably clear that (observed
        // peak 62 m/s) while staying far below actual blown-up magnitudes (458 m/s+ measured
        // during the §-1/Step-2 constraint-explosion bugs) - the gate must still catch a real
        // blowup, just not a fast car.
        ConfigValue<float>                    jolt_wm4_max_speed_mps;
        // docs §122: ODE's GLOBAL body damping, which the port never had. ai::DynamicScene::InitOnce
        // calls dWorldSetDampingFlag(1) and dWorldSetDampingParameters(linear 0.1, angular 0.3), and
        // dBodyCreate copies those into EVERY body at construction - chassis, wheels, debris alike,
        // with no per-body override anywhere in the game. dxStepBody (RVA 0x4fc8f0) then applies
        //     lvel *= (1 - linear_scale * h)   avel *= (1 - angular_scale * h)
        // once per body per step, with NO velocity threshold (dxDamping in this build is a bare
        // {float,float}; the threshold fields are a later ODE revision). The stepsize is multiplied
        // in explicitly, so 0.1 and 0.3 are rates in 1/s, not a per-tick haircut.
        //
        // Jolt's law is the same expression - MotionProperties.inl:143 does
        // `v *= max(0, 1 - c*dt)` - so this is an exact mapping rather than an approximation, and
        // the two scales below are the ODE values verbatim. Only the max(0,...) clamp differs, and
        // at c*dt of 0.003-0.01 it never engages.
        //
        // Off by default because it changes the drive model on a closed stage. §122 measured that
        // this plus the §103 soil drag accounts for 63-79% of the reference's coast resistance,
        // against 26% for soil drag alone - so the expected effect is on the post-throttle tail,
        // the worst part of every run in §103 and §108.
        ConfigValue<uint32_t>                 jolt_body_damping;
        // docs §122.10/§123: the diagnostic block (§30/§32/§66/§67/§101-§103/§122...) fires every
        // kLogIntervalFrames physics steps, 60 by default (~1 Hz at 60fps) - fine for eyeballing a
        // log, useless for telling a wheel that chatters at its suspension's natural frequency
        // (2-4 Hz here) from one that takes a single multi-second hop, since 1 Hz sampling aliases
        // both into the same handful of contact/no-contact snapshots. Lowering this for a short,
        // scripted measurement raises the sample rate without touching the physics or the per-step
        // harvest itself - the counters being read are already safe to read at any cadence (docs
        // §59/§63: read only after PhysicsSystem::Update has returned, no worker thread live).
        // Left at 60 by default: a permanently denser log is not what this lever is for.
        ConfigValue<uint32_t>                 jolt_wm4_diag_interval;
        // docs §122.15/§122.17: the shadow chassis normally gets its inertia tensor from
        // JPH::EOverrideMassProperties::CalculateInertia over the REAL compound collision shape
        // (BuildChassisCompoundShape - Chassis/Cabin/Basket/etc boxes+spheres+capsules from
        // ComplexPhysicObj::m_vehicleParts, §23.10). The reference never does this: ai::
        // ComplexPhysicObj::RefreshMass (RVA 0x2bcac0) computes ODE's real dMass with a single
        // dMassSetBoxTotal over m_massSize - no dMassAdd, ever, so the reference's inertia is
        // always the uniform-box formula, regardless of how many collision parts the vehicle has.
        // A 5-vehicle sweep (§122.17) found the two computations mostly disagree, sometimes by an
        // order of magnitude on pitch (Ixx) - Molokovoz01's 6% match (the only vehicle §122.14/
        // §122.15 originally checked) turned out to be the best-agreeing case in the sample, not
        // a representative one. Worst seen: 10.52x (Scout01). Roll (Izz) is inflated on every one
        // of the 5 vehicles tested (1.04x-2.83x), independent of how well pitch agrees.
        // 1 makes the chassis body's MASS PROPERTIES (not its collision shape - that stays the
        // real compound geometry either way) exactly the same uniform box ODE's RefreshMass
        // computes: JPH::MassProperties::SetMassAndInertiaOfSolidBox(massSize, 1.0f) then
        // ScaleToMass(vehicle mass), via JPH::EOverrideMassProperties::MassAndInertiaProvided.
        // Off by default, same reasoning as jolt_body_damping: this is a drive-model change on a
        // closed stage, and it should be measured per-vehicle before its default changes, not
        // flipped on the strength of a 5-vehicle static comparison alone.
        ConfigValue<uint32_t>                 jolt_chassis_inertia_ode_box;
        // The two ODE scales, exposed so an arm can isolate linear from angular. Angular acts on
        // WHEEL SPIN in mode 4 (real wheel bodies) and has nowhere to go in mode 2, which is a real
        // asymmetry between the modes and not a bug in either.
        ConfigValue<float>                    jolt_damping_linear;
        ConfigValue<float>                    jolt_damping_angular;
        // docs §124: the wheel's ground-contact force (normal spring + Pacejka friction,
        // wm::GeneralizedContactForce) is computed once per step from the PREVIOUS step's
        // harvested contact and applied as a plain AddForce on the wheel body, entirely outside
        // the solver - unlike the strut's SixDOFConstraint, which IS a real constraint and gets
        // properly Gauss-Seidel-solved. The reference (ODE) solves its suspension joint and its
        // ground contact together in one LCP per step; this is the one-step decoupling §123.7
        // named as the remaining architectural gap. 1 routes the SAME force math through a real
        // JPH::Constraint (WheelContactConstraint) instead, so it iteratively co-converges with
        // the strut within a step. Staged behind this flag per docs §124's plan - starts as a
        // pure no-op stub (step 1) before any force actually moves.
        ConfigValue<uint32_t>                 jolt_wm4_contact_constraint;
        // docs §139 (fleet friction calibration): wheelmodel_core's Pacejka grip (WMParams::mu,
        // "grip"/jolt_wm_grip) is ONE global number shared by every wheel of every vehicle under
        // wheelmodel=4 - unlike the reference ODE, which already differs friction per wheel type
        // via WheelPrototypeInfo::m_mU (vehicleparts.xml). That real per-vehicle data is already
        // read into this file (see StepWheelModel's own §42.9 fix, which does exactly this for
        // the mode-2 path) but was never wired into the mode-4 (wheelBodyMode) path that's
        // actually shipped. 1 multiplies each wheel's WMParams::mu by its own m_mU, mirroring
        // §42.9 exactly; m_mU=1.0 for most wheels, so this is a no-op for the common case. Off by
        // default pending the A/B measurement this flag exists to make possible.
        ConfigValue<uint32_t>                 jolt_wm4_per_wheel_mu;
        // docs §140.5 (task #65): how wheelmodel_core combines longitudinal and lateral slip.
        // 0 (legacy) evaluates the Pacejka curve TWICE - once on the slip ratio, once on the slip
        // ANGLE - as if the two were independent channels, then rescales the pair back onto the
        // friction circle. At large slip angle both evaluations saturate near +-D and the rescale
        // leaves ~0.7*D of longitudinal force whose SIGN is set by whatever noisy v_par the scrub
        // produces. §140.2j measured the consequence on Mirotvorec01 at full steer lock: 2874 N of
        // gross longitudinal effort across 6 wheels self-cancelling to -127 N net (96%), the
        // shadow stopping dead while the ODE reference drove on. 1 builds ONE theoretical slip
        // vector, evaluates Phi once on its magnitude and points the force along it, so the
        // longitudinal share falls away as the slip goes lateral - which is what Coulomb friction
        // actually does. Identical to the legacy form to first order at small slip angle; the two
        // only diverge where the legacy form is wrong. Off by default pending the A/B on the
        // §140 repro and the 23-vehicle fleet sweep.
        ConfigValue<uint32_t>                 jolt_wm4_iso_slip;
        // docs §140.9 (task #67): how the tyre's tangential force reaches the solver.
        // 0 (legacy) recomputes the full Pacejka force every velocity iteration and adds a whole
        // Ftang*dt each time, with no accumulated lambda and no decaying increment. Since
        // SolveVelocityConstraint runs ~20x per step (measured exactly 20.00 via the §124.7
        // counters), the delivered impulse scales with the iteration count instead of the force:
        // §140.8 measured real per-wheel friction reaching +-4600 N on a 4032 N vehicle, and
        // §140.8a measured travel collapsing monotonically as iterations rise (10 -> 7.1 m,
        // 12 -> 5.0, 16 -> 1.2, 20 -> 0.8 stuck 3/3).
        // 1 makes friction two tangential AxisConstraintParts per slot with accumulated lambda,
        // clamped to the friction circle mu*lambda_n, exactly as Jolt's own
        // ContactConstraintManager does contact friction. The per-iteration increment then decays
        // to zero as it converges and the total impulse stops depending on the iteration count.
        // NOTE this changes what the tyre model contributes on this path: the constraint drives
        // contact slip toward zero (stick) and the Pacejka peak mu becomes the LIMIT, rather than
        // the curve shape being summed twenty times. Rolling resistance, which was folded into
        // Phi's input shift, does not carry over. Off by default pending the gates in task #67.
        ConfigValue<uint32_t>                 jolt_wm4_friction_constraint;
        // docs §140.15 (task #70): order of the two mode-2 tangential AxisConstraintPart solves.
        // 0 solves rolling/longitudinal first (current behaviour); 1 solves lateral first.
        // Diagnostic A/B only, off by default as a separate axis-order experiment.
        ConfigValue<uint32_t>                 jolt_wm4_fric_axis_order;
        // docs §140.10 (task #68): the minimum share of the friction circle each of the two
        // tangential axes is guaranteed, expressed as a fraction of mu*lambda_n. Exists because
        // under throttle the longitudinal axis sits at the cap and, without a floor, leaves the
        // lateral axis nothing - the shadow then drives correctly and cannot corner (§140.9a
        // measured powered straightness 0.85 against ODE's 0.57, while the SAME turn taken
        // coasting matched ODE exactly).
        // The first attempt hardcoded 0.3 and did not help, which I wrongly read as refuting
        // starvation. The arithmetic says otherwise: at grip 2.0 a 0.3 floor caps powered lateral
        // grip at an effective mu of 0.6, below the ODE reference's own ~1.0 per-wheel m_mU, so
        // that value sat on the wrong side of the reference and could not distinguish the
        // hypothesis either way. Parity needs >= 0.5 here.
        // 1.0 removes the coupling entirely (each axis independently capped at the full budget,
        // a box rather than a circle, admitting |F| up to sqrt(2)*budget) - useful as a
        // diagnostic upper bound, not as a shipping value.
        ConfigValue<float>                    jolt_wm4_fric_floor;
        // docs §105/§106: how the steer DOF holds its angle. 2 = the LITERAL port - the RotationZ
        // motor is switched OFF and the wheel's orientation is ASSIGNED each frame from the
        // commanded steer and its current spin, exactly as _TurnWheelByAngle ->
        // PhysicObj::SetRotation does. Not the same as a very stiff motor: a stiff motor buys the
        // rigid wheel by reacting the disturbance into the chassis, and §106's frequency sweep
        // measured that cost as monotone (pos-div 12.4 -> 35.1 m from 20 to 240 Hz).
        // 1 (default) makes the RotationZ motor
        // effectively kinematic - a torque limit far above any measured contact disturbance -
        // which is the dynamic stand-in for what the reference does: ai::Vehicle::_KeepSteer
        // ASSIGNS the wheel's orientation via PhysicObj::SetRotation every frame and never writes
        // an axis-1 joint motor at all. 0 restores the finite load-derived cap, which §104
        // measured losing to an 8591 Nm disturbance while the driver was not even steering.
        ConfigValue<uint32_t>                 jolt_wm4_steer_kinematic;
        // docs §106: the steering Position motor's spring. NOT recovered - the plan writes
        // "~20 Гц / ζ=1" with a tilde and no format string, config key or log field carries either
        // number, so unlike the torque cap there is no evidence to contradict by moving it. §105.4
        // showed it is now what limits the steer angle: k = I_z*(2*pi*f)^2, and holding the
        // measured 8591 Nm disturbance to 1 deg needs about 93 Hz. Swept, not guessed.
        ConfigValue<float>                    jolt_wm4_steer_hz;
        ConfigValue<float>                    jolt_wm4_steer_damping;
        // docs §139.9 (task #63, Mirotvorec01 drive-motor chatter): the wheel SixDOF constraint's
        // solver-step override was previously hardcoded (20/10, joltshadow.cpp) - config-driven so
        // a live A/B doesn't need a rebuild per candidate value. Defaults match the prior literal,
        // so leaving this untouched changes nothing.
        ConfigValue<uint32_t>                 jolt_wm4_drive_vel_steps;
        ConfigValue<uint32_t>                 jolt_wm4_drive_pos_steps;
        // docs §107: VARIANT SHADOWS - several shadows of the same vehicle in one game launch,
        // each with its own parameters, so an A/B costs one launch instead of one per arm.
        // Prefix list, same mechanism as lua_binds Script_N: [jolt_harness] wm4_variant_1, _2, ...
        // Each value is a comma-separated k=v list over the mode-4 levers, e.g.
        //     wm4_variant_1=label=steerOn,steer=1
        //     wm4_variant_2=label=steerOff,steer=0
        // Keys: label, spin, steer, steer_mode, assists, governor, soildrag, steer_hz,
        // steer_damping. Anything omitted is inherited from the ordinary config values, so a
        // variant only has to state what it changes. An empty list leaves the whole feature inert.
        ConfigValue<std::vector<std::string>> jolt_wm4_variants;
        // docs §39: wheelmodel_core (Pacejka contact) params - same [wheelmodel] names as spring_wheel so tuning transfers.
        ConfigValue<float>                    jolt_wm_tyre_stiffness;
        ConfigValue<float>                    jolt_wm_tyre_thickness;
        ConfigValue<float>                    jolt_wm_tyre_damping;
        ConfigValue<float>                    jolt_wm_hard_core_lambda;
        ConfigValue<float>                    jolt_wm_grip;
        ConfigValue<float>                    jolt_wm_pac_B;
        ConfigValue<float>                    jolt_wm_pac_C;
        ConfigValue<float>                    jolt_wm_pac_E;
        ConfigValue<float>                    jolt_wm_slip_floor;
        ConfigValue<float>                    jolt_wm_stick_speed;
        ConfigValue<float>                    jolt_wm_wheel_inertia;
        // docs §42: Crr, rolling-resistance coefficient folded directly into the Pacejka slip
        // curve (see wheelmodel_core.hpp's GeneralizedContactForce) instead of a bolted-on
        // chassis-level drag force - per user request to keep Pacejka the foundation rather
        // than adding a separate constant.
        ConfigValue<float>                    jolt_wm_rolling_resist;
        ConfigValue<float>                    jolt_wm_react_scale;
        ConfigValue<uint32_t>                 jolt_wm_own_spin;
        // docs §41: drive torque is now generated by a real multi-gear model (see
        // StepWheelModelGearbox in joltshadow.cpp) calibrated from the vehicle's OWN real
        // GEAR_RATIOS/diffRatio/GetMaxTorque()/shift-RPM data - superseded the flat
        // jolt_wm_drive_torque constant, docs §40's jolt_wm_torque_falloff_omega curve, and the
        // never-wired-up jolt_wm_motor_gain (reserved for a different, real-ODE-omega-chasing
        // design that was never built), all removed rather than left as dead/unused knobs.
        ConfigValue<float>                    jolt_wm_max_g;
        // docs §39: my suspension travel DOF (spring_wheel used ODE's Hinge2 for this; the Jolt port has no wheel body so models it explicitly).
        // docs §42.5: stiffness/damping are no longer flat config knobs - read PER-WHEEL from
        // the real CFM/ERP-derived s->mSuspensionSpring (see StepWheelModel), same real data
        // §31 already computes for the VehicleConstraint built (but not simulated) in wheelmodel
        // mode. jolt_wm_susp_stiffness/damping removed rather than left as dead/unused knobs.
        // docs §43: jolt_wm_susp_travel/unsprung_mass removed the same way - travel is now
        // restLen - s->mSuspensionMinLength (real per-wheel geometry, already on hand) and
        // unsprung mass is hta::ai::Wheel::GetMass() (real per-wheel-type data), both computed
        // directly in StepWheelModel rather than kept as flat guessed constants.
        ConfigValue<float>                    jolt_friction_long;
        ConfigValue<float>                    jolt_friction_lat;
        ConfigValue<uint32_t>                 jolt_autotune;
        ConfigValue<uint32_t>                 jolt_autotune_max_trials;
        ConfigValue<float>                    jolt_pushback_min_dspeed;
        ConfigValue<float>                    jolt_pushback_scale;
        // docs §58 (user: "калибруй"): multiplies the Jolt-side impact energy - now
        // vehicle->GetMass() * closingSpeed^2, confirmed via tools/lora disasm_typed of the
        // live binary to be the EXACT native break-gate formula from
        // ai::CollideVehicleAndBreakableObject (VA 0x492a00, source line 125) - before it's
        // compared against each BreakableObject's own GetCriticalHitEnergy()
        // (fix/joltshadow.cpp's HandleBreakableContact). Base formula is a verified match, not
        // a guess, but "verified" means read from disassembly, not cross-checked against a live
        // native firing - the native path itself still never fires under apply=1 (docs §56.6),
        // so this scale remains the dial for whatever gap that leaves (e.g. this game build's
        // actual chassis mass vs vehicle->GetMass()'s exact return value was not independently
        // re-measured on the Jolt side this pass).
        ConfigValue<float>                    jolt_break_energy_scale;
        // docs §64 (goal: "деревья падали как в ODE"): §63 found the tree stays perfectly rigid
        // (rot unchanged) through 2.3s of a real player ram, and even in a scripted 25s sustained
        // ram nothing moved for the first ~5s - because a tree is a Jolt STATIC body (never
        // pushed by Jolt's own solver, by definition) and SetJointAnchor alone imparts zero
        // velocity. Under pure ODE the vehicle and tree share ONE simulation, so continuous
        // contact resolution pushes the tree every step automatically; under Jolt that link is
        // missing entirely - nothing was injecting the vehicle's actual momentum into the tree's
        // now-real ODE body. Same class of problem ApplyRamPushback already solved for
        // vehicle-vs-vehicle (AddImpulse there); this is AddImpulseAtPos applied every step
        // contact persists (HandleTreeEnableContact, joltshadow.cpp) rather than once, so the
        // scale needs to be MUCH smaller than jolt_pushback_scale's 1.0 default - applied at
        // ~90-100Hz, not once per contact. 0.02 is a rough starting guess (not yet calibrated
        // against a live "does it feel like a normal ram" pass) - the whole point of a separate,
        // live-tunable scale here, same as jolt_break_energy_scale's own history.
        ConfigValue<float>                    jolt_tree_push_scale;
        // docs §52 (Этап 0): gates per-frame hot-path DIAGNOSTICS (dBodyGetNumJoints ramming
        // probes in ApplyJoltToVehicle, etc.) that must NOT be paid for during a perf-profiling
        // run. Default 0 (off) - a clean [jolt_profile] measurement should not include the cost
        // of instrumentation that only exists to answer already-answered structural questions.
        ConfigValue<uint32_t>                 jolt_hotpath_diag;
        // docs §54.4 (Этап 1, шаг -1F): opt in to actually destroying abandoned shadow
        // bodies/constraints (deferred to just after PhysicsSystem::Update) instead of the
        // historical leak-forever-on-rebuild behaviour. Default 0 - see DrainPendingJoltDestroys
        // for why this is opt-in rather than simply switched on.
        ConfigValue<uint32_t>                 jolt_deferred_destroy;

        // Schwarz
        ConfigValue<bool>                     complex_schwarz;
        ConfigValue<float>                    gun_gadgets_max_schwarz_part;
        ConfigValue<float>                    common_gadgets_max_schwarz_part;
        ConfigValue<float>                    wares_max_schwarz_part;
        ConfigValue<bool>                     peace_price_from_schwarz;
        ConfigValue<bool>                     no_money_in_player_schwarz;
        ConfigValue<std::unordered_map<std::string, uint32_t, std::hash<std::string_view>, std::equal_to<>>> schwarz_overrides;

    public:
         Config();
        ~Config();

        void Load();
        void Dump();

    private:
        template<typename T>
        void LoadValue(ConfigValue<T>* value);

        template<typename T>
        void DumpValue(ConfigValue<T>* value);
    };
};

#endif
