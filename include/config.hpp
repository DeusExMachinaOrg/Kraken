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
        ConfigValue<uint32_t>                 jolt;
        ConfigValue<uint32_t>                 jolt_threads;
        ConfigValue<uint32_t>                 jolt_shadow;
        ConfigValue<uint32_t>                 jolt_apply;
        ConfigValue<uint32_t>                 jolt_player_only;
        ConfigValue<uint32_t>                 jolt_ai_count;
        ConfigValue<float>                    jolt_susp_frequency;
        ConfigValue<float>                    jolt_susp_rest_fraction;
        ConfigValue<float>                    jolt_susp_damping;
        ConfigValue<float>                    jolt_susp_reference_hz;
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
        // DEFAULT 0, on the measurement, and the reason is worth reading before flipping it: the
        // port is exact (constants read from the binary, the force reproduces to the newton) and
        // it still made divergence WORSE - travel ratio 1.01 -> 1.09, pos-div 2.62 -> 8.03 over 3
        // interleaved repeats. The downforce adds grip, the drive servo converts grip into speed,
        // and the thing that restrains the reference at that speed - the §95.3 speed governor in
        // _KeepGearBox - is still not ported. Porting a SUBSET of the per-frame layer is not a
        // partial improvement, it is a bias. Turn this on together with the governor, not before.
        ConfigValue<uint32_t>                 jolt_wm4_assists;
        // docs §102 (§95.3 ported): the speed governor _KeepGearBox applies before handing torque
        // to the joint - above GetMaxSpeed() (player/attacking) or m_cruisingSpeed (AI), measured
        // on the WHEEL SURFACE speed and only while the driver is not braking, the drive is cut to
        // zero. Its own lever rather than a part of wm4_assists, so the two can be A/B'd
        // separately - §101 measured that the assists without this one are a net loss.
        ConfigValue<uint32_t>                 jolt_wm4_governor;
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