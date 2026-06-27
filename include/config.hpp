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
        ConfigValue<uint32_t>                 show_load_every; // Update loading screen each N objects (vanilla N = 1)
        ConfigValue<uint32_t>                 cardan_fix;
        ConfigValue<uint32_t>                 tactics;
        ConfigValue<uint32_t>                 tactics_lock;
        ConfigValue<uint32_t>                 wares;
        ConfigValue<uint32_t>                 log_debug; // 0 - debug, 1 - info, 2 - warning, 3 - error, 4 - panic, 5 - none
        ConfigValue<uint32_t>                 cctl_leak_fix;
        ConfigValue<uint32_t>                 mortarvolleylauncherfix;
        ConfigValue<uint32_t>                 gunlights;
        ConfigValue<uint32_t>                 radio_manager_fix; // 0 disables the RadioManager map-transition self-heal
        ConfigValue<uint32_t>                 appendix; // 0 disables Appendix (thorn) logic + the new ram-collision formulas

        // Steering wheel (MOZA R5 and any winmm joystick). See fix/controls.cpp.
        ConfigValue<uint32_t>                 wheel;                 // master enable for analog wheel control
        ConfigValue<uint32_t>                 wheel_device;          // winmm joystick id (0 = first)
        ConfigValue<uint32_t>                 wheel_steer_axis;      // axis index for steering (0=X 1=Y 2=Z 3=R 4=U 5=V)
        ConfigValue<uint32_t>                 wheel_throttle_axis;   // axis index for throttle pedal
        ConfigValue<uint32_t>                 wheel_brake_axis;      // axis index for brake pedal
        ConfigValue<float>                    wheel_deadzone;        // steering deadzone around center [0..1]
        ConfigValue<float>                    wheel_pedal_deadzone;  // pedal deadzone at rest (cuts idle creep) [0..1]
        ConfigValue<float>                    wheel_steer_range;     // multiplier on the engine's full-lock steer magnitude
        ConfigValue<uint32_t>                 wheel_invert_steer;    // 1: flip steering left/right
        ConfigValue<uint32_t>                 wheel_invert_throttle; // 1: pedal rests pressed (invert 0..1)
        ConfigValue<uint32_t>                 wheel_invert_brake;    // 1: pedal rests pressed (invert 0..1)
        ConfigValue<uint32_t>                 wheel_log;             // 1: log applied steer/throttle each frame (needs log_debug=0)
        ConfigValue<uint32_t>                 ffb;                   // force feedback enable (DirectInput8)
        ConfigValue<float>                    ffb_strength;          // master FFB gain [0..2]
        ConfigValue<float>                    ffb_center;            // self-centering strength at standstill [0..1]
        ConfigValue<float>                    ffb_speed_gain;        // extra centering per unit speed [0..1]
        ConfigValue<uint32_t>                 ffb_invert;            // 1: flip force direction
        ConfigValue<uint32_t>                 ffb_log;               // 1: log FFB magnitude (needs log_debug=0)

        // Ram collision model (vehicle/landscape impacts + Appendix thorns).
        // damage_i = clamp( coeff * (v - threshold)^exponent * massShare_i
        //                   * offense_other * thornMult_other * dirArmor_i , 0, max_damage )
        // The engine then scales it by the target's blast resistance (InflictDamage).
        ConfigValue<float>                    ram_speed_threshold;    // min normal approach speed before any damage
        ConfigValue<float>                    ram_damage_coeff;       // master damage scale (K)
        ConfigValue<float>                    ram_speed_exponent;     // p: 1.0 = impulse, 2.0 = energy
        ConfigValue<float>                    ram_max_damage;         // per-contact clamp (anti-oneshot / velocity spikes)
        ConfigValue<float>                    ram_armor_front;        // directional armor: hit from the front (<1 = tougher)
        ConfigValue<float>                    ram_armor_side;         // directional armor: hit from the side
        ConfigValue<float>                    ram_armor_rear;         // directional armor: hit from the rear
        ConfigValue<float>                    ram_landscape_offense;  // "ram offense" of static obstacles (walls/pipes)
        ConfigValue<float>                    ram_thorn_scale;        // thorn multiplier = 1 + ThornForce * this
        ConfigValue<uint32_t>                 ram_wheel_damage;       // 1: ramming a wheel also damages the vehicle (fixes the side-ram dead zone)
        ConfigValue<uint32_t>                 ram_log;                // 1: thorncollide debug logging (RamOffense load + wheel ram) — needs log_debug=0

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