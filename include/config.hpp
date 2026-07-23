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
        ConfigValue<uint32_t>                 jolt_wheelmodel;            // docs §39: spring_wheel model on the Jolt vehicle. 0=off, 1=log-only eval, 2=apply (drive chassis, no VehicleConstraint)
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
        ConfigValue<float>                    jolt_wm_susp_stiffness;     // k_susp (N/m) soft suspension spring (gives ride travel; series with the stiff tyre)
        ConfigValue<float>                    jolt_wm_susp_damping;       // c_susp (N.s/m) suspension damper
        ConfigValue<float>                    jolt_wm_susp_travel;        // max suspension travel (m)
        ConfigValue<float>                    jolt_wm_unsprung_mass;      // effective wheel-side mass for the DOF integrator (kg)
        ConfigValue<float>                    jolt_friction_long;
        ConfigValue<float>                    jolt_friction_lat;
        ConfigValue<uint32_t>                 jolt_autotune;
        ConfigValue<uint32_t>                 jolt_autotune_max_trials;
        ConfigValue<float>                    jolt_pushback_min_dspeed;
        ConfigValue<float>                    jolt_pushback_scale;

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