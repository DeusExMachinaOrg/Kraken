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
        this->wheelmodel_enabled                = { "wheelmodel", "enabled",                       0,        true,  0,      1         };
        this->wheelmodel_apply                  = { "wheelmodel", "apply",                         0,        true,  0,      1         };
        this->wheelmodel_player_only            = { "wheelmodel", "player_only",                   1,        true,  0,      1         };
        this->wheelmodel_log                    = { "wheelmodel", "log",                           0,        true,  0,      1         };
        this->wheelmodel_max_g                  = { "wheelmodel", "max_g",                         6.0f,     true,  1.0f,   100.0f    };
        this->wheelmodel_max_speed              = { "wheelmodel", "max_speed",                     80.0f,    true,  1.0f,   10000.0f  };
        this->wheelmodel_react_scale            = { "wheelmodel", "react_scale",                   1.0f,     true,  0.0f,   10.0f     };
        this->wheelmodel_tyre_stiffness         = { "wheelmodel", "tyre_stiffness",                120000.0f,true,  1.0f,   1e9f      };
        this->wheelmodel_tyre_thickness         = { "wheelmodel", "tyre_thickness",                0.1f,     true,  0.0f,   10.0f     };
        this->wheelmodel_tyre_damping           = { "wheelmodel", "tyre_damping",                  0.5f,     true,  0.0f,   10.0f     };
        this->wheelmodel_hard_core_lambda       = { "wheelmodel", "hard_core_lambda",              20.0f,    true,  1.0f,   1000.0f   };
        this->wheelmodel_grip                   = { "wheelmodel", "grip",                          1.5f,     true,  0.0f,   10.0f     };
        this->wheelmodel_pac_B                  = { "wheelmodel", "pac_B",                         8.0f,     true,  0.1f,   100.0f    };
        this->wheelmodel_pac_C                  = { "wheelmodel", "pac_C",                         1.5f,     true,  0.1f,   5.0f      };
        this->wheelmodel_pac_E                  = { "wheelmodel", "pac_E",                         0.97f,    true,  -10.0f, 10.0f     };
        this->wheelmodel_slip_floor             = { "wheelmodel", "slip_floor",                    0.5f,     true,  0.01f,  100.0f    };
        this->wheelmodel_stick_speed            = { "wheelmodel", "stick_speed",                   0.5f,     true,  0.01f,  100.0f    };
        this->wheelmodel_wheel_inertia          = { "wheelmodel", "wheel_inertia",                 5.0f,     true,  0.001f, 10000.0f  };
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
        this->LoadValue(&this->wheelmodel_enabled);
        this->LoadValue(&this->wheelmodel_apply);
        this->LoadValue(&this->wheelmodel_player_only);
        this->LoadValue(&this->wheelmodel_log);
        this->LoadValue(&this->wheelmodel_max_g);
        this->LoadValue(&this->wheelmodel_max_speed);
        this->LoadValue(&this->wheelmodel_react_scale);
        this->LoadValue(&this->wheelmodel_tyre_stiffness);
        this->LoadValue(&this->wheelmodel_tyre_thickness);
        this->LoadValue(&this->wheelmodel_tyre_damping);
        this->LoadValue(&this->wheelmodel_hard_core_lambda);
        this->LoadValue(&this->wheelmodel_grip);
        this->LoadValue(&this->wheelmodel_pac_B);
        this->LoadValue(&this->wheelmodel_pac_C);
        this->LoadValue(&this->wheelmodel_pac_E);
        this->LoadValue(&this->wheelmodel_slip_floor);
        this->LoadValue(&this->wheelmodel_stick_speed);
        this->LoadValue(&this->wheelmodel_wheel_inertia);
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
        this->DumpValue(&this->wheelmodel_enabled);
        this->DumpValue(&this->wheelmodel_apply);
        this->DumpValue(&this->wheelmodel_player_only);
        this->DumpValue(&this->wheelmodel_log);
        this->DumpValue(&this->wheelmodel_max_g);
        this->DumpValue(&this->wheelmodel_max_speed);
        this->DumpValue(&this->wheelmodel_react_scale);
        this->DumpValue(&this->wheelmodel_tyre_stiffness);
        this->DumpValue(&this->wheelmodel_tyre_thickness);
        this->DumpValue(&this->wheelmodel_tyre_damping);
        this->DumpValue(&this->wheelmodel_hard_core_lambda);
        this->DumpValue(&this->wheelmodel_grip);
        this->DumpValue(&this->wheelmodel_pac_B);
        this->DumpValue(&this->wheelmodel_pac_C);
        this->DumpValue(&this->wheelmodel_pac_E);
        this->DumpValue(&this->wheelmodel_slip_floor);
        this->DumpValue(&this->wheelmodel_stick_speed);
        this->DumpValue(&this->wheelmodel_wheel_inertia);
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
