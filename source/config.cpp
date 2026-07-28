#include "config.hpp"
#include "ext/logger.hpp"
#include <assert.h>
#include <string>

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
        this->cardan_fix                        = { "constants", "cardan_fix",                      1,     true,  0,     1           };
        this->wares                             = { "constants", "wares",                           0,     true,  0,     1           };
        this->cctl_leak_fix                     = { "constants", "cctl_leak_fix",                   1,     true,  0,     1           };
        this->mortarvolleylauncherfix           = { "constants", "mortarvolleylauncherfix",         1,     true,  0,     1           };
        this->gunlights                         = { "constants", "gunlights",                       1,     true,  0,     1           };
        this->boss01_fast_charge                = { "constants", "boss01_fast_charge",              1,     true,  0,     1           };
        this->testharness                       = { "testharness","enabled",                        0,     true,  0,     1           };
        this->testharness_autoload              = { "testharness","autoload_save",                  0,     true,  0,     1           };
        this->testharness_god_mode              = { "testharness","god_mode",                       0,     true,  0,     1           };
        this->testharness_perfmon               = { "testharness","perfmon",                        0,     true,  0,     1           };
        this->testharness_perfmon_interval      = { "testharness","perfmon_interval",                5.0,   true,  0.5,   60.0        };
        this->testharness_tear_wheel_at_t       = { "testharness","tear_wheel_at_t",                 -1.0,  true, -1.0,   3600.0       };
        this->testharness_ram_test              = { "testharness","ram_test",                        0,     true,  0,     1           };
        this->testharness_ram_test_offset       = { "testharness","ram_test_offset",                15.0,   true,  3.0,   50.0        };
        this->jolt                              = { "jolt",       "enabled",                         0,     true,  0,     1           };
        this->jolt_threads                      = { "jolt",       "threads",                         0,     true,  0,     64          };
        this->jolt_shadow                       = { "jolt_harness","shadow",                         0,     true,  0,     1           };
        this->jolt_apply                        = { "jolt_harness","apply",                          0,     true,  0,     1           };
        this->jolt_player_only                  = { "jolt_harness","player_only",                    1,     true,  0,     1           };
        this->jolt_ai_count                     = { "jolt_harness","ai_count",                       0,     true,  0,     128         };
        this->jolt_susp_frequency               = { "jolt_harness","susp_frequency",                 1.0,   true,  0.5,   2.0         };
        this->jolt_susp_rest_fraction           = { "jolt_harness","susp_rest_fraction",             0.07,  true,  0.02,  0.4         };
        this->jolt_susp_damping                 = { "jolt_harness","susp_damping",                   1.0,   true,  0.2,   3.0         };
        this->jolt_susp_reference_hz            = { "jolt_harness","susp_reference_hz",               60.0,  true,  20.0,  144.0       };
        this->jolt_wheel_proxy                   = { "jolt_harness","wheel_proxy",                     1,     true,  0,     1           };
        this->jolt_collision_cylinder            = { "jolt_harness","collision_cylinder",               0,     true,  0,     1           };
        this->jolt_wheelmodel                    = { "jolt_harness","wheelmodel",                       0,     true,  0,     4           };
        this->jolt_wm4_compress_fraction         = { "jolt_harness","wm4_compress_fraction",           0.5f,  true,  0.0f,  1.0f        };
        this->jolt_wm4_joint_at_mount            = { "jolt_harness","wm4_joint_at_mount",              0,     true,  0,     1           };
        this->jolt_wm4_chassis_mass_excl_wheels  = { "jolt_harness","wm4_chassis_mass_excl_wheels",    1,     true,  0,     1           };
        this->jolt_wm4_spin                      = { "jolt_harness","wm4_spin",                        1,     true,  0,     1           };
        this->jolt_wm4_steer                     = { "jolt_harness","wm4_steer",                       0,     true,  0,     1           };
        this->jolt_wm4_assists                   = { "jolt_harness","wm4_assists",                     1,     true,  0,     1           };
        this->jolt_wm4_governor                  = { "jolt_harness","wm4_governor",                    1,     true,  0,     1           };
        this->jolt_wm4_soildrag                  = { "jolt_harness","wm4_soildrag",                    1,     true,  0,     1           };
        this->jolt_wm_tyre_stiffness             = { "wheelmodel","tyre_stiffness",   120000.0f, true, 1.0f,     1e9f    };
        this->jolt_wm_tyre_thickness             = { "wheelmodel","tyre_thickness",   0.1f,      true, 0.0f,     10.0f   };
        this->jolt_wm_tyre_damping               = { "wheelmodel","tyre_damping",     0.5f,      true, 0.0f,     10.0f   };
        this->jolt_wm_hard_core_lambda           = { "wheelmodel","hard_core_lambda", 20.0f,     true, 1.0f,     1000.0f };
        this->jolt_wm_grip                       = { "wheelmodel","grip",             2.0f,      true, 0.0f,     10.0f   };
        this->jolt_wm_pac_B                      = { "wheelmodel","pac_B",            8.0f,      true, 0.1f,     100.0f  };
        this->jolt_wm_pac_C                      = { "wheelmodel","pac_C",            1.5f,      true, 0.1f,     5.0f    };
        this->jolt_wm_pac_E                      = { "wheelmodel","pac_E",            0.97f,     true, -10.0f,   10.0f   };
        this->jolt_wm_slip_floor                 = { "wheelmodel","slip_floor",       0.5f,      true, 0.01f,    100.0f  };
        this->jolt_wm_stick_speed                = { "wheelmodel","stick_speed",      0.5f,      true, 0.01f,    100.0f  };
        this->jolt_wm_wheel_inertia              = { "wheelmodel","wheel_inertia",    30.0f,     true, 0.001f,   10000.0f};
        this->jolt_wm_rolling_resist             = { "wheelmodel","rolling_resist",   0.02f,     true, 0.0f,     0.5f    };
        this->jolt_wm_react_scale                = { "wheelmodel","react_scale",      1.0f,      true, 0.0f,     10.0f   };
        this->jolt_wm_own_spin                   = { "wheelmodel","own_spin",         1,         true, 0,        1       };
        this->jolt_wm_max_g                      = { "wheelmodel","max_g",            6.0f,      true, 1.0f,     100.0f  };
        this->jolt_friction_long                = { "jolt_harness","friction_long",                  1.0,   true,  0.2,   3.0         };
        this->jolt_friction_lat                 = { "jolt_harness","friction_lat",                   1.0,   true,  0.2,   3.0         };
        this->jolt_autotune                     = { "jolt_harness","autotune",                       0,     true,  0,     1           };
        this->jolt_autotune_max_trials          = { "jolt_harness","autotune_max_trials",             24,    true,  1,     500         };
        this->jolt_pushback_min_dspeed          = { "jolt_harness","pushback_min_dspeed",              0.8,   true,  0.0,   50.0        };
        this->jolt_pushback_scale               = { "jolt_harness","pushback_scale",                   1.0,   true,  0.0,   5.0         };
        this->jolt_hotpath_diag                 = { "jolt_harness","hotpath_diag",                     0,     true,  0,     1           };
        this->jolt_deferred_destroy             = { "jolt_harness","deferred_destroy",                0,     true,  0,     1           };
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
        this->LoadValue(&this->boss01_fast_charge);
        this->LoadValue(&this->testharness);
        this->LoadValue(&this->testharness_autoload);
        this->LoadValue(&this->testharness_god_mode);
        this->LoadValue(&this->testharness_perfmon);
        this->LoadValue(&this->testharness_perfmon_interval);
        this->LoadValue(&this->testharness_tear_wheel_at_t);
        this->LoadValue(&this->testharness_ram_test);
        this->LoadValue(&this->testharness_ram_test_offset);
        this->LoadValue(&this->jolt);
        this->LoadValue(&this->jolt_threads);
        this->LoadValue(&this->jolt_shadow);
        this->LoadValue(&this->jolt_apply);
        this->LoadValue(&this->jolt_player_only);
        this->LoadValue(&this->jolt_ai_count);
        this->LoadValue(&this->jolt_susp_frequency);
        this->LoadValue(&this->jolt_susp_rest_fraction);
        this->LoadValue(&this->jolt_susp_damping);
        this->LoadValue(&this->jolt_susp_reference_hz);
        this->LoadValue(&this->jolt_wheel_proxy);
        this->LoadValue(&this->jolt_collision_cylinder);
        this->LoadValue(&this->jolt_wheelmodel);
        this->LoadValue(&this->jolt_wm4_chassis_mass_excl_wheels);
        this->LoadValue(&this->jolt_wm4_spin);
        this->LoadValue(&this->jolt_wm4_steer);
        this->LoadValue(&this->jolt_wm4_assists);
        this->LoadValue(&this->jolt_wm4_governor);
        this->LoadValue(&this->jolt_wm4_soildrag);
        this->LoadValue(&this->jolt_wm4_joint_at_mount);
        this->LoadValue(&this->jolt_wm4_compress_fraction);
        this->LoadValue(&this->jolt_wm_tyre_stiffness);
        this->LoadValue(&this->jolt_wm_tyre_thickness);
        this->LoadValue(&this->jolt_wm_tyre_damping);
        this->LoadValue(&this->jolt_wm_hard_core_lambda);
        this->LoadValue(&this->jolt_wm_grip);
        this->LoadValue(&this->jolt_wm_pac_B);
        this->LoadValue(&this->jolt_wm_pac_C);
        this->LoadValue(&this->jolt_wm_pac_E);
        this->LoadValue(&this->jolt_wm_slip_floor);
        this->LoadValue(&this->jolt_wm_stick_speed);
        this->LoadValue(&this->jolt_wm_wheel_inertia);
        this->LoadValue(&this->jolt_wm_rolling_resist);
        this->LoadValue(&this->jolt_wm_react_scale);
        this->LoadValue(&this->jolt_wm_own_spin);
        this->LoadValue(&this->jolt_wm_max_g);
        this->LoadValue(&this->jolt_friction_long);
        this->LoadValue(&this->jolt_friction_lat);
        this->LoadValue(&this->jolt_autotune);
        this->LoadValue(&this->jolt_autotune_max_trials);
        this->LoadValue(&this->jolt_pushback_min_dspeed);
        this->LoadValue(&this->jolt_pushback_scale);
        this->LoadValue(&this->jolt_hotpath_diag);
        this->LoadValue(&this->jolt_deferred_destroy);
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
        this->DumpValue(&this->boss01_fast_charge);
        this->DumpValue(&this->testharness);
        this->DumpValue(&this->testharness_autoload);
        this->DumpValue(&this->testharness_god_mode);
        this->DumpValue(&this->testharness_perfmon);
        this->DumpValue(&this->testharness_perfmon_interval);
        this->DumpValue(&this->testharness_tear_wheel_at_t);
        this->DumpValue(&this->testharness_ram_test);
        this->DumpValue(&this->testharness_ram_test_offset);
        this->DumpValue(&this->jolt);
        this->DumpValue(&this->jolt_threads);
        this->DumpValue(&this->jolt_shadow);
        this->DumpValue(&this->jolt_apply);
        this->DumpValue(&this->jolt_player_only);
        this->DumpValue(&this->jolt_ai_count);
        this->DumpValue(&this->jolt_susp_frequency);
        this->DumpValue(&this->jolt_susp_rest_fraction);
        this->DumpValue(&this->jolt_susp_damping);
        this->DumpValue(&this->jolt_susp_reference_hz);
        this->DumpValue(&this->jolt_wheel_proxy);
        this->DumpValue(&this->jolt_collision_cylinder);
        this->DumpValue(&this->jolt_wheelmodel);
        this->DumpValue(&this->jolt_wm4_chassis_mass_excl_wheels);
        this->DumpValue(&this->jolt_wm4_spin);
        this->DumpValue(&this->jolt_wm4_steer);
        this->DumpValue(&this->jolt_wm4_assists);
        this->DumpValue(&this->jolt_wm4_governor);
        this->DumpValue(&this->jolt_wm4_soildrag);
        this->DumpValue(&this->jolt_wm4_joint_at_mount);
        this->DumpValue(&this->jolt_wm4_compress_fraction);
        this->DumpValue(&this->jolt_wm_tyre_stiffness);
        this->DumpValue(&this->jolt_wm_tyre_thickness);
        this->DumpValue(&this->jolt_wm_tyre_damping);
        this->DumpValue(&this->jolt_wm_hard_core_lambda);
        this->DumpValue(&this->jolt_wm_grip);
        this->DumpValue(&this->jolt_wm_pac_B);
        this->DumpValue(&this->jolt_wm_pac_C);
        this->DumpValue(&this->jolt_wm_pac_E);
        this->DumpValue(&this->jolt_wm_slip_floor);
        this->DumpValue(&this->jolt_wm_stick_speed);
        this->DumpValue(&this->jolt_wm_wheel_inertia);
        this->DumpValue(&this->jolt_wm_rolling_resist);
        this->DumpValue(&this->jolt_wm_react_scale);
        this->DumpValue(&this->jolt_wm_own_spin);
        this->DumpValue(&this->jolt_wm_max_g);
        this->DumpValue(&this->jolt_friction_long);
        this->DumpValue(&this->jolt_friction_lat);
        this->DumpValue(&this->jolt_autotune);
        this->DumpValue(&this->jolt_autotune_max_trials);
        this->DumpValue(&this->jolt_pushback_min_dspeed);
        this->DumpValue(&this->jolt_pushback_scale);
        this->DumpValue(&this->jolt_hotpath_diag);
        this->DumpValue(&this->jolt_deferred_destroy);
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

                    configstructs::WareType type = (strcmp(prefix, configstructs::REPAIR) == 0) ? configstructs::WareType::REPAIR : configstructs::WareType::REFUEL;

                    value->value.emplace_back(units, armor, ware, type);
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
            }
        }
        else {
            throw "Unsupported type";
        }
    };
};
