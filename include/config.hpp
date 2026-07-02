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
        // Ware units use synthesized per-entry sections (e.g. "repair1"), so this
        // section is unused for routing; nullptr keeps it on the global kraken.ini.
        const char*   section = nullptr;
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
        ConfigValue<uint32_t>                 wheel_auto_brake;      // 0: coast when throttle released; 1: engine auto-brake
        // Combined throttle/brake on a single axis (DualSense L2/R2 share one
        // axis that rests at center): positive half = throttle (R2), negative
        // half = brake (L2). -1 = off (use separate throttle_axis/brake_axis).
        ConfigValue<int32_t>                  wheel_trigger_axis;    // combined L2/R2 axis index, or -1
        ConfigValue<float>                    wheel_trigger_deadzone;// center deadzone for the combined trigger axis [0..1]
        ConfigValue<uint32_t>                 wheel_invert_trigger;  // 1: swap which half is throttle vs brake
        // Steering response curve: s = sign(s)*|s|^expo after deadzone. 1.0 = linear
        // (wheels). >1 softens the center so a short-throw spring stick gives fine
        // control instead of feeling all-or-nothing (WASD-like).
        ConfigValue<float>                    wheel_steer_expo;      // steering response exponent (1 = linear)
        // Right-stick camera look. Each frame the stick is written into the engine's
        // persistent camera yaw/pitch (the same fields mouse-look drives), so the
        // engine's clamping/follow keeps working. -1 axis = disabled.
        ConfigValue<int32_t>                  wheel_cam_yaw_axis;    // axis for camera yaw (right-stick X), or -1
        ConfigValue<int32_t>                  wheel_cam_pitch_axis;  // axis for camera pitch (right-stick Y), or -1
        ConfigValue<float>                    wheel_cam_deadzone;    // right-stick deadzone [0..1]
        ConfigValue<float>                    wheel_cam_yaw_speed;   // camera yaw speed at full deflection (rad/s)
        ConfigValue<float>                    wheel_cam_pitch_speed; // camera pitch speed at full deflection (rad/s)
        ConfigValue<uint32_t>                 wheel_cam_invert_yaw;  // 1: flip horizontal look
        ConfigValue<uint32_t>                 wheel_cam_invert_pitch;// 1: flip vertical look
        // Auto-return the camera behind the car after the look is idle (mouse /
        // wheel / gamepad — device-agnostic, gated only by this flag).
        ConfigValue<uint32_t>                 wheel_cam_return;       // 1: enable camera auto-return / chase-follow
        ConfigValue<float>                    wheel_cam_return_delay; // idle seconds before returning
        ConfigValue<float>                    wheel_cam_return_speed; // ease rate toward behind
        ConfigValue<float>                    wheel_cam_follow_offset;// radians added to car heading to mean "behind" (calibration)
        ConfigValue<uint32_t>                 wheel_log;             // 1: log applied steer/throttle + raw axes each frame (needs log_debug=0)
        ConfigValue<uint32_t>                 ffb;                   // force feedback enable (DirectInput8)
        ConfigValue<float>                    ffb_strength;          // master FFB gain [0..2]
        ConfigValue<float>                    ffb_center;            // self-centering strength at standstill [0..1]
        ConfigValue<float>                    ffb_speed_gain;        // extra centering per unit speed [0..1]
        ConfigValue<uint32_t>                 ffb_invert;            // 1: flip force direction
        ConfigValue<uint32_t>                 ffb_log;               // 1: log FFB magnitude (needs log_debug=0)
        // Vibration channel (periodic effect layered on the centering force).
        ConfigValue<float>                    ffb_damage;            // shake on taking damage (health drop) [0..2]
        ConfigValue<float>                    ffb_collision;         // jolt on collisions/rams (accel spike) [0..2]
        ConfigValue<float>                    ffb_offroad;           // rough-ground vibration, scaled by speed [0..2]
        ConfigValue<float>                    ffb_engine;            // continuous engine rumble from throttle/speed [0..2]
        ConfigValue<float>                    ffb_vibe_hz;           // vibration frequency in Hz [5..100]

        // Gamepad (DualSense and any winmm joystick). See fix/gamepad.cpp.
        // Bridges controller buttons into the engine's built-in (but dormant)
        // joystick input, so JOY_BUTTON_0..9 become bindable to any action in the
        // in-game control-settings menu. Driving axes (steer/throttle/brake) stay
        // analog through the [wheel] path in fix/controls.cpp.
        // Active control profile (folder name under <playerProfile>/input_profiles).
        // Lives in the global kraken.ini [input] section; the input device sections
        // below ([wheel]/[gamepad]/[dualsense]/[xinput]) are read from that
        // profile's input.ini instead of kraken.ini. See fix/inputprofiles.cpp.
        ConfigValue<std::string>              active_input_profile;

        ConfigValue<uint32_t>                 gamepad;               // master enable for the gamepad button bridge
        ConfigValue<uint32_t>                 gamepad_log;           // 1: log each button->engine event (needs log_debug=0)
        // Per-button action binding. The engine's own joystick-binding UI is a
        // dead subsystem in this build, so we bind JOY_BUTTON_0..9 to engine
        // impulses ourselves (re-applied after every binding load). Value = an
        // engine impulse name, e.g. IM_UI_INVENTORY / IM_CAR_HORN / IM_CAR_FIRE_0
        // (empty = leave unbound). DualSense winmm order: 0=Square 1=Cross
        // 2=Circle 3=Triangle 4=L1 5=R1 6=L2 7=R2 8=Share 9=Options.
        ConfigValue<std::string>              gamepad_mode;          // engine game-mode for the bindings (GS_GAME)
        ConfigValue<std::string>              gamepad_button[10];    // button N -> impulse name
        ConfigValue<uint32_t>                 gamepad_autobind;      // 1: apply [gamepad] button* on every load (ini is authority). 0: let the in-game menu / saved profile own the bindings.

        // DualSense haptic feedback (HID rumble; adaptive triggers later). See
        // fix/dualsense.cpp. Force is derived from the player vehicle's motion.
        ConfigValue<uint32_t>                 dualsense;             // master enable for DualSense feedback
        ConfigValue<float>                    dualsense_strength;    // overall rumble gain [0..2]
        ConfigValue<float>                    dualsense_impact;      // impact (strong motor) gain
        ConfigValue<float>                    dualsense_offroad;     // rough-ground buzz (weak motor) gain
        ConfigValue<float>                    dualsense_damage;      // pulse gain when the vehicle takes damage (health drop)
        ConfigValue<float>                    dualsense_damage_full; // fraction of max HP lost (in one hit) that gives a full pulse
        ConfigValue<uint32_t>                 dualsense_hid_input;   // 1: read sticks/buttons from HID and inject (wireless; bypass winmm)
        ConfigValue<uint32_t>                 dualsense_log;         // 1: log accel/speed/rumble (needs log_debug=0)
        // Adaptive triggers (L2/R2 resistance + buzz/kick), encoded in the same HID
        // output report. See the trigger model in fix/dualsense.cpp.
        // XInput rumble (for a pad bridged to a virtual Xbox controller via DSX /
        // Steam Input, where the native DualSense HID is unavailable). See
        // fix/xinputrumble.cpp. Same motion-derived force model as [dualsense].
        ConfigValue<uint32_t>                 xinput;                // master enable for XInput rumble
        ConfigValue<float>                    xinput_strength;       // overall rumble gain [0..2]
        ConfigValue<float>                    xinput_impact;         // impact (strong motor) gain
        ConfigValue<float>                    xinput_offroad;        // rough-ground buzz (weak motor) gain
        ConfigValue<float>                    xinput_damage;         // pulse gain when the vehicle takes damage
        ConfigValue<float>                    xinput_damage_full;    // fraction of max HP lost (one hit) for a full pulse
        ConfigValue<int32_t>                  xinput_index;          // XInput slot 0..3, or -1 = auto-detect
        ConfigValue<uint32_t>                 xinput_log;            // 1: log accel/speed/rumble (needs log_debug=0)

        ConfigValue<uint32_t>                 dualsense_triggers;        // master enable for adaptive triggers
        ConfigValue<float>                    dualsense_trigger_brake;   // L2 brake-pedal resistance gain [0..2]
        ConfigValue<float>                    dualsense_trigger_throttle;// R2 throttle (engine-load) resistance gain [0..2]
        ConfigValue<float>                    dualsense_trigger_kick;    // collision/impact trigger kick gain [0..2]
        ConfigValue<float>                    dualsense_trigger_damage;  // taking-damage trigger kick gain [0..2]
        ConfigValue<float>                    dualsense_trigger_buzz;    // rough-ground trigger buzz gain [0..2]

        // Gun auto-return: ease the player's guns back to forward when idle.
        ConfigValue<uint32_t>                 gunreturn;             // master enable
        ConfigValue<float>                    gunreturn_timeout;     // idle seconds before guns return
        ConfigValue<uint32_t>                 gunreturn_log;         // reserved debug flag

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

        // Control-profile support (see fix/inputprofiles.cpp). The input device
        // sections are sourced from a per-profile input.ini whose full path is set
        // here; an empty path falls back to the global kraken.ini (back-compat /
        // before the engine profile is up).
        void SetInputSource(const std::string& iniPath);
        void ReloadInput(); // re-read only the input sections (after a profile switch)
        void DumpInput();   // write only the input sections back (save / migrate)

    private:
        template<typename T>
        void LoadValue(ConfigValue<T>* value);

        template<typename T>
        void DumpValue(ConfigValue<T>* value);
    };
};

#endif