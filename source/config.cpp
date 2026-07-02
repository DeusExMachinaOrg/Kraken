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
        this->show_load_every                   = { "constants", "show_load_every",                 100,   true,  0,     UINT32_MAX  };
        this->cardan_fix                        = { "constants", "cardan_fix",                      1,     true,  0,     1           };
        this->wares                             = { "constants", "wares",                           0,     true,  0,     1           };
        this->cctl_leak_fix                     = { "constants", "cctl_leak_fix",                   1,     true,  0,     1           };
        this->mortarvolleylauncherfix           = { "constants", "mortarvolleylauncherfix",         1,     true,  0,     1           };
        this->gunlights                         = { "constants", "gunlights",                       1,     true,  0,     1           };
        this->hbao                              = { "render",    "hbao",                            0,     true,  0,     1           };
        this->hbao_radius                       = { "render",    "hbao_radius",                     1.5,   true,  0.0,   100.0       };
        this->hbao_intensity                    = { "render",    "hbao_intensity",                  2.0,   true,  0.0,   10.0        };
        this->hbao_bias                         = { "render",    "hbao_bias",                       0.1,   true,  0.0,   1.0         };
        this->hbao_fade_start                   = { "render",    "hbao_fade_start",                 50.0,  true,  0.0,   100000.0    };
        this->hbao_fade_end                     = { "render",    "hbao_fade_end",                   300.0, true,  0.0,   100000.0    };
        this->grass_clip                        = { "render",    "grass_clip",                      1,     true,  0,     1           };
        this->shadow_csm                        = { "render",    "shadow_csm",                      1,     true,  0,     1           };
        this->shadow_csm_resolution             = { "render",    "shadow_csm_resolution",           1536,  true,  512,   4096        };
        this->shadow_csm_dist0                  = { "render",    "shadow_csm_dist0",                256.0, true,  10.0,  100000.0    };
        this->shadow_csm_dist1                  = { "render",    "shadow_csm_dist1",                512.0, true,  10.0,  100000.0    };
        this->shadow_csm_dist2                  = { "render",    "shadow_csm_dist2",                1024.0, true, 10.0,  100000.0    };
        this->shadow_csm_depth_range            = { "render",    "shadow_csm_depth_range",          600.0, true,  50.0,  100000.0    };
        this->shadow_csm_bias                   = { "render",    "shadow_csm_bias",                 0.0015, true, 0.0,   1.0         };
        this->shadow_csm_strength               = { "render",    "shadow_csm_strength",             0.5,   true,  0.0,   1.0         };
        this->shadow_csm_fade                   = { "render",    "shadow_csm_fade",                 0.15,  true,  0.0,   1.0         };
        this->shadow_csm_pcf                    = { "render",    "shadow_csm_pcf",                  1.5,   true,  0.0,   16.0        };
        this->shadow_csm_blend                  = { "render",    "shadow_csm_blend",                0.1,   true,  0.0,   0.5         };
        this->shadow_csm_objects                = { "render",    "shadow_csm_objects",              1,     true,  0,     1           };
        this->shadow_csm_caster_fov             = { "render",    "shadow_csm_caster_fov",           3.0,   true,  0.5,   16.0        };
        this->shadow_csm_debug                  = { "render",    "shadow_csm_debug",                0,     true,  0,     1           };
        this->shadow_native                     = { "render",    "shadow_native",                   1,     true,  0,     1           };
        this->shadow_contact                    = { "render",    "shadow_contact",                  1,     true,  0,     1           };
        this->shadow_contact_steps              = { "render",    "shadow_contact_steps",            16,    true,  1,     64          };
        this->shadow_contact_length             = { "render",    "shadow_contact_length",           2.0,   true,  0.0,   100.0       };
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

        this->gui_resolution_x                  = { "interface", "resolution_x",                    1024, false, 0, 0 };
        this->gui_resolution_y                  = { "interface", "resolution_y",                    768,  false, 0, 0 };
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
        this->LoadValue(&this->gui_resolution_x);
        this->LoadValue(&this->gui_resolution_y);
        this->LoadValue(&this->cctl_leak_fix);
        this->LoadValue(&this->mortarvolleylauncherfix);
        this->LoadValue(&this->gunlights);
        this->LoadValue(&this->hbao);
        this->LoadValue(&this->hbao_radius);
        this->LoadValue(&this->hbao_intensity);
        this->LoadValue(&this->hbao_bias);
        this->LoadValue(&this->hbao_fade_start);
        this->LoadValue(&this->hbao_fade_end);
        this->LoadValue(&this->grass_clip);
        this->LoadValue(&this->shadow_csm);
        this->LoadValue(&this->shadow_csm_resolution);
        this->LoadValue(&this->shadow_csm_dist0);
        this->LoadValue(&this->shadow_csm_dist1);
        this->LoadValue(&this->shadow_csm_dist2);
        this->LoadValue(&this->shadow_csm_depth_range);
        this->LoadValue(&this->shadow_csm_bias);
        this->LoadValue(&this->shadow_csm_fade);
        this->LoadValue(&this->shadow_csm_pcf);
        this->LoadValue(&this->shadow_csm_blend);
        this->LoadValue(&this->shadow_csm_objects);
        this->LoadValue(&this->shadow_csm_caster_fov);
        this->LoadValue(&this->shadow_csm_debug);
        this->LoadValue(&this->shadow_native);
        this->LoadValue(&this->shadow_csm_strength);
        this->LoadValue(&this->shadow_contact);
        this->LoadValue(&this->shadow_contact_steps);
        this->LoadValue(&this->shadow_contact_length);
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
        this->DumpValue(&this->gui_resolution_x);
        this->DumpValue(&this->gui_resolution_y);
        this->DumpValue(&this->cctl_leak_fix);
        this->DumpValue(&this->mortarvolleylauncherfix);
        this->DumpValue(&this->gunlights);
        this->DumpValue(&this->hbao);
        this->DumpValue(&this->hbao_radius);
        this->DumpValue(&this->hbao_intensity);
        this->DumpValue(&this->hbao_bias);
        this->DumpValue(&this->hbao_fade_start);
        this->DumpValue(&this->hbao_fade_end);
        this->DumpValue(&this->grass_clip);
        this->DumpValue(&this->shadow_csm);
        this->DumpValue(&this->shadow_csm_resolution);
        this->DumpValue(&this->shadow_csm_dist0);
        this->DumpValue(&this->shadow_csm_dist1);
        this->DumpValue(&this->shadow_csm_dist2);
        this->DumpValue(&this->shadow_csm_depth_range);
        this->DumpValue(&this->shadow_csm_bias);
        this->DumpValue(&this->shadow_csm_strength);
        this->DumpValue(&this->shadow_csm_fade);
        this->DumpValue(&this->shadow_csm_pcf);
        this->DumpValue(&this->shadow_csm_blend);
        this->DumpValue(&this->shadow_csm_objects);
        this->DumpValue(&this->shadow_csm_caster_fov);
        this->DumpValue(&this->shadow_csm_debug);
        this->DumpValue(&this->shadow_native);
        this->DumpValue(&this->shadow_contact);
        this->DumpValue(&this->shadow_contact_steps);
        this->DumpValue(&this->shadow_contact_length);
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
