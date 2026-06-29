#include "config.hpp"
#include "ext/logger.hpp"
#include <assert.h>
#include <string>

#define LOGGER "config"

using kraken::logger::eLogDebug;
using kraken::logger::eLogPanic;

namespace kraken {
    const char* CONFIG_PATH = "./data/kraken.ini";

    template <typename T>
    inline T clamp(T v, T min, T max) {
        if (v < min) return min;
        if (v > max) return max;
        return v;
    };

    Config* Config::INSTANCE = nullptr;

    Config::Config() {
        assert(Config::INSTANCE == nullptr && "Config already created!");

        this->log_debug                         = { "logging",   "log_debug",                       1,     true,  eLogDebug, eLogPanic + 1};
        this->save_width                        = { "graphics",  "save_width",                      512,   true,  256,   2048        };
        this->save_height                       = { "graphics",  "save_height",                     256,   true,  128,   1024        };
        this->view_resolution                   = { "graphics",  "view_resolution",                 2048,  true,  128,   4096        };
        this->gravity                           = { "constants", "gravity",                         -9.81, true,  -100,  0           };
        this->price_fuel                        = { "constants", "price_fuel",                      50,    true,  1,     10000       };
        this->price_paint                       = { "constants", "price_paint",                     50,    true,  1,     10000       };
        this->keep_throttle                     = { "constants", "keep_throttle",                   1.0,   true,  0.0,   1.0         };
        this->handbrake_power                   = { "constants", "handbrake_power",                 1.0,   true,  0.0,   1.0         };
        this->brake_power                       = { "constants", "brake_power",                     -1.0,  true,  -1.0,  0.0         };
        this->friend_damage                     = { "constants", "friend_damage",                   0,     true,  0,     1           };
        this->auto_brake_angle                  = { "constants", "auto_brake_angle",                50,    true,  0,     180         };
        this->lua_enabled                       = { "lua_binds", "Enabled",                         0,     true,  0,     1           };
        this->lua_scripts                       = { "lua_binds", "Script_"                                                           };
        this->posteffectreload                  = { "constants", "posteffectreload",                0,     true,  0,     1           };
        this->ultrawide                         = { "constants", "ultrawide",                       0,     true,  0,     1           };
        this->objcontupgrade                    = { "constants", "obj_cont_upgrade",                1,     true,  0,     1           };
        this->show_load_every                   = { "constants", "show_load_every",                 100,   true,  0,     UINT32_MAX  };
        this->cardan_fix                        = { "constants", "cardan_fix",                      1,     true,  0,     1           };
        this->wares                             = { "constants", "wares",                           0,     true,  0,     1           };
        this->cctl_leak_fix                     = { "constants", "cctl_leak_fix",                   1,     true,  0,     1           };
        this->mortarvolleylauncherfix           = { "constants", "mortarvolleylauncherfix",         1,     true,  0,     1           };
        this->gunlights                         = { "constants", "gunlights",                       1,     true,  0,     1           };
        this->radio_manager_fix                 = { "constants", "radio_manager_fix",               1,     true,  0,     1           };
        this->appendix                          = { "constants", "appendix",                        1,     true,  0,     1           };
        this->wheel                             = { "wheel",     "enabled",                         0,     true,  0,     1           };
        this->wheel_device                      = { "wheel",     "device",                          0,     true,  0,     15          };
        this->wheel_steer_axis                  = { "wheel",     "steer_axis",                      0,     true,  0,     5           };
        this->wheel_throttle_axis               = { "wheel",     "throttle_axis",                   2,     true,  0,     5           };
        this->wheel_brake_axis                  = { "wheel",     "brake_axis",                      3,     true,  0,     5           };
        this->wheel_deadzone                    = { "wheel",     "deadzone",                        0.04f, true,  0.0f,  0.9f        };
        this->wheel_pedal_deadzone              = { "wheel",     "pedal_deadzone",                  0.10f, true,  0.0f,  0.9f        };
        this->wheel_steer_range                 = { "wheel",     "steer_range",                     1.0f,  true,  0.1f,  4.0f        };
        this->wheel_invert_steer                = { "wheel",     "invert_steer",                    0,     true,  0,     1           };
        this->wheel_invert_throttle             = { "wheel",     "invert_throttle",                 0,     true,  0,     1           };
        this->wheel_invert_brake                = { "wheel",     "invert_brake",                    0,     true,  0,     1           };
        this->wheel_auto_brake                  = { "wheel",     "auto_brake",                      0,     true,  0,     1           };
        this->wheel_trigger_axis                = { "wheel",     "trigger_axis",                    -1,    true,  -1,    5           };
        this->wheel_trigger_deadzone            = { "wheel",     "trigger_deadzone",                0.06f, true,  0.0f,  0.9f        };
        this->wheel_invert_trigger              = { "wheel",     "invert_trigger",                  0,     true,  0,     1           };
        this->wheel_steer_expo                  = { "wheel",     "steer_expo",                      1.0f,  true,  0.2f,  4.0f        };
        this->wheel_cam_yaw_axis                = { "wheel",     "cam_yaw_axis",                    -1,    true,  -1,    5           };
        this->wheel_cam_pitch_axis              = { "wheel",     "cam_pitch_axis",                  -1,    true,  -1,    5           };
        this->wheel_cam_deadzone                = { "wheel",     "cam_deadzone",                    0.15f, true,  0.0f,  0.9f        };
        this->wheel_cam_yaw_speed               = { "wheel",     "cam_yaw_speed",                   2.5f,  true,  0.0f,  20.0f       };
        this->wheel_cam_pitch_speed             = { "wheel",     "cam_pitch_speed",                 1.8f,  true,  0.0f,  20.0f       };
        this->wheel_cam_invert_yaw              = { "wheel",     "cam_invert_yaw",                  0,     true,  0,     1           };
        this->wheel_cam_invert_pitch            = { "wheel",     "cam_invert_pitch",                0,     true,  0,     1           };
        this->wheel_cam_return                  = { "wheel",     "cam_return",                      0,     true,  0,     1           };
        this->wheel_cam_return_delay            = { "wheel",     "cam_return_delay",                1.5f,  true,  0.0f,  10.0f       };
        this->wheel_cam_return_speed            = { "wheel",     "cam_return_speed",                3.0f,  true,  0.1f,  20.0f       };
        this->wheel_cam_follow_offset           = { "wheel",     "cam_follow_offset",               0.0f,  true,  -6.3f, 6.3f        };
        this->wheel_log                         = { "wheel",     "log",                             0,     true,  0,     1           };
        this->ffb                               = { "wheel",     "ffb",                             0,     true,  0,     1           };
        this->ffb_strength                      = { "wheel",     "ffb_strength",                    1.0f,  true,  0.0f,  2.0f        };
        this->ffb_center                        = { "wheel",     "ffb_center",                      0.12f, true,  0.0f,  1.0f        };
        this->ffb_speed_gain                    = { "wheel",     "ffb_speed_gain",                  0.03f, true,  0.0f,  1.0f        };
        this->ffb_invert                        = { "wheel",     "ffb_invert",                      0,     true,  0,     1           };
        this->ffb_log                           = { "wheel",     "ffb_log",                         0,     true,  0,     1           };
        this->ffb_damage                        = { "wheel",     "ffb_damage",                      1.0f,  true,  0.0f,  2.0f        };
        this->ffb_collision                     = { "wheel",     "ffb_collision",                   1.0f,  true,  0.0f,  2.0f        };
        this->ffb_offroad                       = { "wheel",     "ffb_offroad",                     1.0f,  true,  0.0f,  2.0f        };
        this->ffb_engine                        = { "wheel",     "ffb_engine",                      0.3f,  true,  0.0f,  2.0f        };
        this->ffb_vibe_hz                       = { "wheel",     "ffb_vibe_hz",                     55.0f, true,  5.0f,  100.0f      };
        this->gamepad                           = { "gamepad",   "enabled",                         0,     true,  0,     1           };
        this->gamepad_log                       = { "gamepad",   "log",                             0,     true,  0,     1           };
        this->gamepad_mode                      = { "gamepad",   "game_mode",                       std::string("GS_GAME"), false      };
        {
            // DualSense winmm button order: 0=Square 1=Cross 2=Circle 3=Triangle
            // 4=L1 5=R1 6=L2 7=R2 8=Share 9=Options. L2/R2 are analog axes (never
            // reported as buttons), so their default action is empty — gas/brake
            // are handled analog in fix/controls.cpp.
            static const char* const kKeys[10] = {
                "button0","button1","button2","button3","button4",
                "button5","button6","button7","button8","button9"
            };
            static const char* const kDefaults[10] = {
                "IM_UI_INVENTORY",     // Square
                "IM_CAR_HAND_BREAK",   // Cross
                "IM_CAR_SWITCHCAMERA", // Circle
                "IM_RELOAD_WEAPON",    // Triangle
                "IM_CAR_FIRE_1",       // L1
                "IM_CAR_FIRE_0",       // R1
                "",                    // L2 (analog brake)
                "",                    // R2 (analog throttle)
                "IM_UI_MAP",           // Share
                "IM_MODE_GAME_MENU"    // Options
            };
            for (int i = 0; i < 10; ++i)
                this->gamepad_button[i] = { "gamepad", kKeys[i], std::string(kDefaults[i]), false };
        }
        this->gamepad_autobind                  = { "gamepad",   "autobind",                        1,     true,  0,     1           };
        this->dualsense                         = { "dualsense", "enabled",                         0,     true,  0,     1           };
        this->dualsense_strength                = { "dualsense", "strength",                        1.0f,  true,  0.0f,  2.0f        };
        this->dualsense_impact                  = { "dualsense", "impact",                          1.0f,  true,  0.0f,  4.0f        };
        this->dualsense_offroad                 = { "dualsense", "offroad",                         1.0f,  true,  0.0f,  4.0f        };
        this->dualsense_damage                  = { "dualsense", "damage",                          1.0f,  true,  0.0f,  4.0f        };
        this->dualsense_damage_full             = { "dualsense", "damage_full",                     0.20f, true,  0.01f, 1.0f        };
        this->dualsense_hid_input               = { "dualsense", "hid_input",                       0,     true,  0,     1           };
        this->dualsense_log                     = { "dualsense", "log",                             0,     true,  0,     1           };
        this->xinput                            = { "xinput",    "enabled",                         0,     true,  0,     1           };
        this->xinput_strength                   = { "xinput",    "strength",                        1.0f,  true,  0.0f,  2.0f        };
        this->xinput_impact                     = { "xinput",    "impact",                          1.0f,  true,  0.0f,  4.0f        };
        this->xinput_offroad                    = { "xinput",    "offroad",                         1.0f,  true,  0.0f,  4.0f        };
        this->xinput_damage                     = { "xinput",    "damage",                          1.0f,  true,  0.0f,  4.0f        };
        this->xinput_damage_full                = { "xinput",    "damage_full",                     0.20f, true,  0.01f, 1.0f        };
        this->xinput_index                      = { "xinput",    "index",                          -1,     true, -1,     3           };
        this->xinput_log                        = { "xinput",    "log",                             0,     true,  0,     1           };
        this->dualsense_triggers                = { "dualsense", "triggers",                        0,     true,  0,     1           };
        this->dualsense_trigger_brake           = { "dualsense", "trigger_brake",                   1.0f,  true,  0.0f,  2.0f        };
        this->dualsense_trigger_throttle        = { "dualsense", "trigger_throttle",                1.0f,  true,  0.0f,  2.0f        };
        this->dualsense_trigger_kick            = { "dualsense", "trigger_kick",                    1.0f,  true,  0.0f,  2.0f        };
        this->dualsense_trigger_damage          = { "dualsense", "trigger_damage",                  1.0f,  true,  0.0f,  2.0f        };
        this->dualsense_trigger_buzz            = { "dualsense", "trigger_buzz",                    1.0f,  true,  0.0f,  2.0f        };
        this->gunreturn                         = { "gunreturn", "enabled",                         0,     true,  0,     1           };
        this->gunreturn_timeout                 = { "gunreturn", "timeout",                         3.0f,  true,  0.0f,  60.0f       };
        this->gunreturn_log                     = { "gunreturn", "log",                             0,     true,  0,     1           };
        this->ram_speed_threshold               = { "thorncollide", "speed_threshold",              5.0f,    true,  0.0f,  1000.0f   };
        this->ram_damage_coeff                  = { "thorncollide", "damage_coeff",                 2.7f,    true,  0.0f,  100000.0f };
        this->ram_speed_exponent                = { "thorncollide", "speed_exponent",               1.0f,    true,  0.1f,  4.0f      };
        this->ram_max_damage                    = { "thorncollide", "max_damage",                   100000.0f,true, 0.0f,  1e9f      };
        this->ram_armor_front                   = { "thorncollide", "armor_front",                  0.8f,    true,  0.01f, 100.0f    };
        this->ram_armor_side                    = { "thorncollide", "armor_side",                   1.25f,   true,  0.01f, 100.0f    };
        this->ram_armor_rear                    = { "thorncollide", "armor_rear",                   1.0f,    true,  0.01f, 100.0f    };
        this->ram_landscape_offense             = { "thorncollide", "landscape_offense",            0.5f,    true,  0.0f,  100.0f    };
        this->ram_thorn_scale                   = { "thorncollide", "thorn_scale",                  0.02f,   true,  0.0f,  100.0f    };
        this->ram_wheel_damage                  = { "thorncollide", "wheel_damage",                 1,       true,  0,     1         };
        this->ram_log                           = { "thorncollide", "log",                          0,       true,  0,     1         };
        this->tactics                           = { "tactics",   "enabled",                         1,     true,  0,     1           };
        this->tactics_lock                      = { "tactics",   "lock_on_player",                  1,     true,  0,     1           };
        this->contact_surface_layer             = { "glob_phys", "contact_surface_layer",           0.01,  true,  0,     1.0         };
        this->cfm                               = { "glob_phys", "cfm",                             0.0001,true,  0,     1.0         };
        this->erp                               = { "glob_phys", "erp",                             0.1,   true,  0,     1.0         };
        this->peace_price_from_schwarz          = { "schwarz",   "calc_peace_price_from_schwarz",   false                            };
        this->no_money_in_player_schwarz        = { "schwarz",   "no_money_in_player_schwarz",      false                            };
        this->complex_schwarz                   = { "schwarz",   "complex_schwarz",                 false                            };
        this->schwarz_overrides                 = { "schwarz_overrides"                                                              };
        this->gun_gadgets_max_schwarz_part      = { "schwarz",   "gun_gadgets_max_schwarz_part",    0.0,   true,  0.0,   10.0        };
        this->common_gadgets_max_schwarz_part   = { "schwarz",   "common_gadgets_max_schwarz_part", 0.0,   true,  0.0,   10.0        };
        this->wares_max_schwarz_part            = { "schwarz",    "wares_max_schwarz_part",         0.0,   true,  0.0,   10.0        };
        Config::INSTANCE = this;

        logger::Init();

        this->Load();
        this->Dump();
    };

    Config::~Config() {
        this->Dump();

        Config::INSTANCE = nullptr;
    };

    void Config::Load() {
        this->LoadValue(&this->log_debug);
        this->LoadValue(&this->save_width);
        this->LoadValue(&this->save_height);
        this->LoadValue(&this->view_resolution);
        this->LoadValue(&this->gravity);
        this->LoadValue(&this->price_fuel);
        this->LoadValue(&this->price_paint);
        this->LoadValue(&this->keep_throttle);
        this->LoadValue(&this->handbrake_power);
        this->LoadValue(&this->brake_power);
        this->LoadValue(&this->friend_damage);
        this->LoadValue(&this->auto_brake_angle);
        this->LoadValue(&this->lua_enabled);
        this->LoadValue(&this->lua_scripts);
        this->LoadValue(&this->posteffectreload);
        this->LoadValue(&this->ware_units);
        this->LoadValue(&this->ultrawide);
        this->LoadValue(&this->objcontupgrade);
        this->LoadValue(&this->show_load_every);
        this->LoadValue(&this->cardan_fix);
        this->LoadValue(&this->contact_surface_layer);
        this->LoadValue(&this->cfm);
        this->LoadValue(&this->erp);
        this->LoadValue(&this->tactics);
        this->LoadValue(&this->tactics_lock);
        this->LoadValue(&this->complex_schwarz);
        this->LoadValue(&this->gun_gadgets_max_schwarz_part);
        this->LoadValue(&this->common_gadgets_max_schwarz_part);
        this->LoadValue(&this->wares_max_schwarz_part);
        this->LoadValue(&this->peace_price_from_schwarz);
        this->LoadValue(&this->no_money_in_player_schwarz);
        this->LoadValue(&this->schwarz_overrides);
        this->LoadValue(&this->wares);
        this->LoadValue(&this->cctl_leak_fix);
        this->LoadValue(&this->mortarvolleylauncherfix);
        this->LoadValue(&this->gunlights);
        this->LoadValue(&this->radio_manager_fix);
        this->LoadValue(&this->appendix);
        this->LoadValue(&this->wheel);
        this->LoadValue(&this->wheel_device);
        this->LoadValue(&this->wheel_steer_axis);
        this->LoadValue(&this->wheel_throttle_axis);
        this->LoadValue(&this->wheel_brake_axis);
        this->LoadValue(&this->wheel_deadzone);
        this->LoadValue(&this->wheel_pedal_deadzone);
        this->LoadValue(&this->wheel_steer_range);
        this->LoadValue(&this->wheel_invert_steer);
        this->LoadValue(&this->wheel_invert_throttle);
        this->LoadValue(&this->wheel_invert_brake);
        this->LoadValue(&this->wheel_auto_brake);
        this->LoadValue(&this->wheel_trigger_axis);
        this->LoadValue(&this->wheel_trigger_deadzone);
        this->LoadValue(&this->wheel_invert_trigger);
        this->LoadValue(&this->wheel_steer_expo);
        this->LoadValue(&this->wheel_cam_yaw_axis);
        this->LoadValue(&this->wheel_cam_pitch_axis);
        this->LoadValue(&this->wheel_cam_deadzone);
        this->LoadValue(&this->wheel_cam_yaw_speed);
        this->LoadValue(&this->wheel_cam_pitch_speed);
        this->LoadValue(&this->wheel_cam_invert_yaw);
        this->LoadValue(&this->wheel_cam_invert_pitch);
        this->LoadValue(&this->wheel_cam_return);
        this->LoadValue(&this->wheel_cam_return_delay);
        this->LoadValue(&this->wheel_cam_return_speed);
        this->LoadValue(&this->wheel_cam_follow_offset);
        this->LoadValue(&this->wheel_log);
        this->LoadValue(&this->ffb);
        this->LoadValue(&this->ffb_strength);
        this->LoadValue(&this->ffb_center);
        this->LoadValue(&this->ffb_speed_gain);
        this->LoadValue(&this->ffb_invert);
        this->LoadValue(&this->ffb_log);
        this->LoadValue(&this->ffb_damage);
        this->LoadValue(&this->ffb_collision);
        this->LoadValue(&this->ffb_offroad);
        this->LoadValue(&this->ffb_engine);
        this->LoadValue(&this->ffb_vibe_hz);
        this->LoadValue(&this->gamepad);
        this->LoadValue(&this->gamepad_log);
        this->LoadValue(&this->gamepad_mode);
        for (int i = 0; i < 10; ++i)
            this->LoadValue(&this->gamepad_button[i]);
        this->LoadValue(&this->gamepad_autobind);
        this->LoadValue(&this->dualsense);
        this->LoadValue(&this->dualsense_strength);
        this->LoadValue(&this->dualsense_impact);
        this->LoadValue(&this->dualsense_offroad);
        this->LoadValue(&this->dualsense_damage);
        this->LoadValue(&this->dualsense_damage_full);
        this->LoadValue(&this->dualsense_hid_input);
        this->LoadValue(&this->dualsense_log);
        this->LoadValue(&this->xinput);
        this->LoadValue(&this->xinput_strength);
        this->LoadValue(&this->xinput_impact);
        this->LoadValue(&this->xinput_offroad);
        this->LoadValue(&this->xinput_damage);
        this->LoadValue(&this->xinput_damage_full);
        this->LoadValue(&this->xinput_index);
        this->LoadValue(&this->xinput_log);
        this->LoadValue(&this->dualsense_triggers);
        this->LoadValue(&this->dualsense_trigger_brake);
        this->LoadValue(&this->dualsense_trigger_throttle);
        this->LoadValue(&this->dualsense_trigger_kick);
        this->LoadValue(&this->dualsense_trigger_damage);
        this->LoadValue(&this->dualsense_trigger_buzz);
        this->LoadValue(&this->gunreturn);
        this->LoadValue(&this->gunreturn_timeout);
        this->LoadValue(&this->gunreturn_log);
        this->LoadValue(&this->ram_speed_threshold);
        this->LoadValue(&this->ram_damage_coeff);
        this->LoadValue(&this->ram_speed_exponent);
        this->LoadValue(&this->ram_max_damage);
        this->LoadValue(&this->ram_armor_front);
        this->LoadValue(&this->ram_armor_side);
        this->LoadValue(&this->ram_armor_rear);
        this->LoadValue(&this->ram_landscape_offense);
        this->LoadValue(&this->ram_thorn_scale);
        this->LoadValue(&this->ram_wheel_damage);
        this->LoadValue(&this->ram_log);
    };

    void Config::Dump() {
        this->DumpValue(&this->log_debug);
        this->DumpValue(&this->save_width);
        this->DumpValue(&this->save_height);
        this->DumpValue(&this->view_resolution);
        this->DumpValue(&this->gravity);
        this->DumpValue(&this->price_fuel);
        this->DumpValue(&this->price_paint);
        this->DumpValue(&this->keep_throttle);
        this->DumpValue(&this->handbrake_power);
        this->DumpValue(&this->brake_power);
        this->DumpValue(&this->friend_damage);
        this->DumpValue(&this->auto_brake_angle);
        this->DumpValue(&this->lua_enabled);
        this->DumpValue(&this->lua_scripts);
        this->DumpValue(&this->posteffectreload);
        this->DumpValue(&this->ware_units);
        this->DumpValue(&this->ultrawide);
        this->DumpValue(&this->objcontupgrade);
        this->DumpValue(&this->show_load_every);
        this->DumpValue(&this->cardan_fix);
        this->DumpValue(&this->contact_surface_layer);
        this->DumpValue(&this->cfm);
        this->DumpValue(&this->erp);
        this->DumpValue(&this->tactics);
        this->DumpValue(&this->tactics_lock);
        this->DumpValue(&this->complex_schwarz);
        this->DumpValue(&this->gun_gadgets_max_schwarz_part);
        this->DumpValue(&this->common_gadgets_max_schwarz_part);
        this->DumpValue(&this->wares_max_schwarz_part);
        this->DumpValue(&this->peace_price_from_schwarz);
        this->DumpValue(&this->no_money_in_player_schwarz);
        this->DumpValue(&this->schwarz_overrides);
        this->DumpValue(&this->wares);
        this->DumpValue(&this->cctl_leak_fix);
        this->DumpValue(&this->mortarvolleylauncherfix);
        this->DumpValue(&this->gunlights);
        this->DumpValue(&this->radio_manager_fix);
        this->DumpValue(&this->appendix);
        this->DumpValue(&this->wheel);
        this->DumpValue(&this->wheel_device);
        this->DumpValue(&this->wheel_steer_axis);
        this->DumpValue(&this->wheel_throttle_axis);
        this->DumpValue(&this->wheel_brake_axis);
        this->DumpValue(&this->wheel_deadzone);
        this->DumpValue(&this->wheel_pedal_deadzone);
        this->DumpValue(&this->wheel_steer_range);
        this->DumpValue(&this->wheel_invert_steer);
        this->DumpValue(&this->wheel_invert_throttle);
        this->DumpValue(&this->wheel_invert_brake);
        this->DumpValue(&this->wheel_auto_brake);
        this->DumpValue(&this->wheel_trigger_axis);
        this->DumpValue(&this->wheel_trigger_deadzone);
        this->DumpValue(&this->wheel_invert_trigger);
        this->DumpValue(&this->wheel_steer_expo);
        this->DumpValue(&this->wheel_cam_yaw_axis);
        this->DumpValue(&this->wheel_cam_pitch_axis);
        this->DumpValue(&this->wheel_cam_deadzone);
        this->DumpValue(&this->wheel_cam_yaw_speed);
        this->DumpValue(&this->wheel_cam_pitch_speed);
        this->DumpValue(&this->wheel_cam_invert_yaw);
        this->DumpValue(&this->wheel_cam_invert_pitch);
        this->DumpValue(&this->wheel_cam_return);
        this->DumpValue(&this->wheel_cam_return_delay);
        this->DumpValue(&this->wheel_cam_return_speed);
        this->DumpValue(&this->wheel_cam_follow_offset);
        this->DumpValue(&this->wheel_log);
        this->DumpValue(&this->ffb);
        this->DumpValue(&this->ffb_strength);
        this->DumpValue(&this->ffb_center);
        this->DumpValue(&this->ffb_speed_gain);
        this->DumpValue(&this->ffb_invert);
        this->DumpValue(&this->ffb_log);
        this->DumpValue(&this->ffb_damage);
        this->DumpValue(&this->ffb_collision);
        this->DumpValue(&this->ffb_offroad);
        this->DumpValue(&this->ffb_engine);
        this->DumpValue(&this->ffb_vibe_hz);
        this->DumpValue(&this->gamepad);
        this->DumpValue(&this->gamepad_log);
        this->DumpValue(&this->gamepad_mode);
        for (int i = 0; i < 10; ++i)
            this->DumpValue(&this->gamepad_button[i]);
        this->DumpValue(&this->gamepad_autobind);
        this->DumpValue(&this->dualsense);
        this->DumpValue(&this->dualsense_strength);
        this->DumpValue(&this->dualsense_impact);
        this->DumpValue(&this->dualsense_offroad);
        this->DumpValue(&this->dualsense_damage);
        this->DumpValue(&this->dualsense_damage_full);
        this->DumpValue(&this->dualsense_hid_input);
        this->DumpValue(&this->dualsense_log);
        this->DumpValue(&this->xinput);
        this->DumpValue(&this->xinput_strength);
        this->DumpValue(&this->xinput_impact);
        this->DumpValue(&this->xinput_offroad);
        this->DumpValue(&this->xinput_damage);
        this->DumpValue(&this->xinput_damage_full);
        this->DumpValue(&this->xinput_index);
        this->DumpValue(&this->xinput_log);
        this->DumpValue(&this->dualsense_triggers);
        this->DumpValue(&this->dualsense_trigger_brake);
        this->DumpValue(&this->dualsense_trigger_throttle);
        this->DumpValue(&this->dualsense_trigger_kick);
        this->DumpValue(&this->dualsense_trigger_damage);
        this->DumpValue(&this->dualsense_trigger_buzz);
        this->DumpValue(&this->gunreturn);
        this->DumpValue(&this->gunreturn_timeout);
        this->DumpValue(&this->gunreturn_log);
        this->DumpValue(&this->ram_speed_threshold);
        this->DumpValue(&this->ram_damage_coeff);
        this->DumpValue(&this->ram_speed_exponent);
        this->DumpValue(&this->ram_max_damage);
        this->DumpValue(&this->ram_armor_front);
        this->DumpValue(&this->ram_armor_side);
        this->DumpValue(&this->ram_armor_rear);
        this->DumpValue(&this->ram_landscape_offense);
        this->DumpValue(&this->ram_thorn_scale);
        this->DumpValue(&this->ram_wheel_damage);
        this->DumpValue(&this->ram_log);
    };

    template<typename T>
    void Config::LoadValue(ConfigValue<T>* value) {
        char buffer[1024] = {0};

        if constexpr (std::is_same_v<int32_t, T>) {
            GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);
            if (strnlen_s(buffer, sizeof(buffer)) > 0) {
                value->value = std::strtol(buffer, nullptr, 10);
                if (value->limited)
                    value->value = clamp<int32_t>(value->value, value->min, value->max);
            }
        }
        else if constexpr (std::is_same_v<uint32_t, T>) {
            GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);
            if (strnlen_s(buffer, sizeof(buffer)) > 0) {
                value->value = std::strtoul(buffer, nullptr, 10);
                if (value->limited)
                    value->value = clamp<uint32_t>(value->value, value->min, value->max);
            }
        }
        else if constexpr (std::is_same_v<float, T>) {
            GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);
            if (strnlen_s(buffer, sizeof(buffer)) > 0) {
                value->value = std::strtof(buffer, nullptr);
                if (value->limited)
                    value->value = clamp<float>(value->value, value->min, value->max);
            }
        }
        else if constexpr (std::is_same_v<double, T>) {
            GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);
            if (strnlen_s(buffer, sizeof(buffer)) > 0) {
                value->value = std::strtod(buffer, nullptr);
                if (value->limited)
                    value->value = clamp<double>(value->value, value->min, value->max);
            }
        }
        else if constexpr (std::is_same_v<bool, T>) {
            GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);
            if (strnlen_s(buffer, sizeof(buffer)) > 0)
                if (strcmp(buffer, "true") || strcmp(buffer, "1")) {
                    value->value = true;
                }
                else if (strcmp(buffer, "false") || strcmp(buffer, "0")) {
                    value->value = false;
                }
        }
        else if constexpr (std::is_same_v<std::string, T>) {
            GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);
            if (strnlen_s(buffer, sizeof(buffer)) > 0)
                value->value = buffer;
        }
        else if constexpr (std::is_same_v<std::vector<std::string>, T>) {
            value->value.clear();
            for (int i = 1; i < 128; ++i) {
                char key[128];
                std::snprintf(key, sizeof(key), "%s%d", value->keyPrefix, i);

                GetPrivateProfileStringA(value->section, key, "", buffer, sizeof(buffer), CONFIG_PATH);
                if (strnlen_s(buffer, sizeof(buffer)) == 0)
                    break;
                value->value.emplace_back(buffer);
            }
        }
        else if constexpr (std::is_same_v<std::unordered_map<std::string, uint32_t, std::hash<std::string_view>, std::equal_to<>>, T>) {
            value->value.clear();
            char keysBuffer[32768];
            DWORD keysLength = GetPrivateProfileStringA(value->section, NULL, "", keysBuffer, sizeof(keysBuffer), CONFIG_PATH);

            if (keysLength > 0) {
                // Parse the null-separated list of keys
                const char* key = keysBuffer;
                while (*key != '\0') {
                    GetPrivateProfileStringA(value->section, key, "", buffer, sizeof(buffer), CONFIG_PATH);

                    if (strnlen_s(buffer, sizeof(buffer)) > 0) {
                        try {
                            uint32_t mapValue = std::stoul(buffer);
                            value->value[key] = mapValue;
                        } catch (const std::exception&) {
                            // Skipping invalid number format, maybe better to raise exception?
                        }
                    }

                    key += strlen(key) + 1;
                }
            }
        }
        else if constexpr (std::is_same_v<std::vector<configstructs::WareUnits>, T>) {
            value->value.clear();
            for (int i = 1; i < 128; ++i) {
                char key[128];
                for (const auto& prefix : { configstructs::REPAIR, configstructs::REFUEL }) {
                    std::snprintf(key, sizeof(key), "%s%d", prefix, i);

                    GetPrivateProfileStringA(key, "Units", "", buffer, sizeof(buffer), CONFIG_PATH);
                    if (strnlen_s(buffer, sizeof(buffer)) == 0)
                        continue;
                    float units = std::strtof(buffer, nullptr);

                    float armor = 0.0f;
                    GetPrivateProfileStringA(key, "Armor", "", buffer, sizeof(buffer), CONFIG_PATH);
                    if (strnlen_s(buffer, sizeof(buffer)) != 0)
                        armor = std::strtof(buffer, nullptr);

                    GetPrivateProfileStringA(key, "Ware", "", buffer, sizeof(buffer), CONFIG_PATH);
                    if (strnlen_s(buffer, sizeof(buffer)) == 0)
                        continue;
                    std::string ware = buffer;

                    GetPrivateProfileStringA(key, "Sound", "", buffer, sizeof(buffer), CONFIG_PATH);
                    std::string sound = buffer;

                    GetPrivateProfileStringA(key, "Script", "", buffer, sizeof(buffer), CONFIG_PATH);
                    std::string script = buffer;

                    // Consume defaults to true (legacy behaviour: a used ware is always spent).
                    bool consume = true;
                    GetPrivateProfileStringA(key, "Consume", "", buffer, sizeof(buffer), CONFIG_PATH);
                    if (strnlen_s(buffer, sizeof(buffer)) != 0)
                        consume = (buffer[0] == '1' || buffer[0] == 't' || buffer[0] == 'T' || buffer[0] == 'y' || buffer[0] == 'Y');

                    configstructs::WareType type = (strcmp(prefix, configstructs::REPAIR) == 0) ? configstructs::WareType::REPAIR : configstructs::WareType::REFUEL;

                    LOG_INFO("Loaded ware unit: Type=%s, Units=%.03f, Armor=%.03f, Ware=%s, Sound=%s, Script=%s, Consume=%s", (type == configstructs::WareType::REPAIR) ? "REPAIR" : "REFUEL", units, armor, ware.c_str(), sound.c_str(), script.c_str(), consume ? "true" : "false");

                    value->value.emplace_back(units, armor, ware, type, sound, script, consume);
                }
            }
        }
        else {
            throw "Unsupported type";
        }
    };

    template<typename T>
    void Config::DumpValue(ConfigValue<T>* value) {
        char buffer[1024] = {0};

        if constexpr (std::is_same_v<int32_t, T>) {
            std::snprintf(buffer, sizeof(buffer), "%d", value->value);
            WritePrivateProfileStringA(value->section, value->key, buffer, CONFIG_PATH);
        }
        else if constexpr (std::is_same_v<uint32_t, T>) {
            std::snprintf(buffer, sizeof(buffer), "%u", value->value);
            WritePrivateProfileStringA(value->section, value->key, buffer, CONFIG_PATH);
        }
        else if constexpr (std::is_same_v<float, T>) {
            std::snprintf(buffer, sizeof(buffer), "%.06f", value->value);
            WritePrivateProfileStringA(value->section, value->key, buffer, CONFIG_PATH);
        }
        else if constexpr (std::is_same_v<double, T>) {
            std::snprintf(buffer, sizeof(buffer), "%.06f", value->value);
            WritePrivateProfileStringA(value->section, value->key, buffer, CONFIG_PATH);
        }
        else if constexpr (std::is_same_v<bool, T>) {
            std::snprintf(buffer, sizeof(buffer), "%s", value->value ? "true" : "false");
            WritePrivateProfileStringA(value->section, value->key, buffer, CONFIG_PATH);
        }
        else if constexpr (std::is_same_v<std::string, T>) {
            WritePrivateProfileStringA(value->section, value->key, value->value.c_str(), CONFIG_PATH);
        }
        else if constexpr (std::is_same_v<std::vector<std::string>, T>) {
            for (size_t i = 0; i < value->value.size(); ++i) {
                char key[128];
                std::snprintf(key, sizeof(key), "%s%zu", value->keyPrefix, i + 1);
                WritePrivateProfileStringA(value->section, key, value->value[i].c_str(), CONFIG_PATH);
            }
        }
        else if constexpr (std::is_same_v<std::unordered_map<std::string, uint32_t, std::hash<std::string_view>, std::equal_to<>>, T>) {
            char val[128];
            for(const auto& [k, v] : value->value) {
                std::sprintf(val, "%ld", v);
                WritePrivateProfileStringA(value->section, k.c_str(), val, CONFIG_PATH);
            }
        }
        else if constexpr (std::is_same_v<std::vector<configstructs::WareUnits>, T>) {
            int repairIndex = 1;
            int refuelIndex = 1;
            for (const configstructs::WareUnits& wareUnit : value->value) {
                char key[128];
                if (wareUnit.Type == configstructs::WareType::REPAIR) {
                    std::snprintf(key, sizeof(key), "%s%d", configstructs::REPAIR, repairIndex++);
                }
                else {
                    std::snprintf(key, sizeof(key), "%s%d", configstructs::REFUEL, refuelIndex++);
                }

                // Units
                std::snprintf(buffer, sizeof(buffer), "%.03f", wareUnit.Units);
                WritePrivateProfileStringA(key, "Units", buffer, CONFIG_PATH);

                // Armor
                if (wareUnit.Type == configstructs::WareType::REPAIR) {
                    std::snprintf(buffer, sizeof(buffer), "%.03f", wareUnit.Armor);
                    WritePrivateProfileStringA(key, "Armor", buffer, CONFIG_PATH);
                }

                // Ware
                WritePrivateProfileStringA(key, "Ware", wareUnit.Ware.c_str(), CONFIG_PATH);

                // Sound
                WritePrivateProfileStringA(key, "Sound", wareUnit.Sound.c_str(), CONFIG_PATH);

                // Script
                WritePrivateProfileStringA(key, "Script", wareUnit.Script.c_str(), CONFIG_PATH);

                // Consume
                WritePrivateProfileStringA(key, "Consume", wareUnit.Consume ? "1" : "0", CONFIG_PATH);
            }
        }
        else {
            throw "Unsupported type";
        }
    };
};
