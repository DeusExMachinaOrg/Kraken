#include "utils.hpp"
#include "config.hpp"
#include <assert.h>

namespace kraken {
    const char* CONFIG_PATH = "./data/kraken.ini";

    Config* Config::INSTANCE = nullptr;

    Config::Config() {
        assert(Config::INSTANCE == nullptr && "Config already created!");

        //                         section      key                base   limit  min    max
        this->save_width       = { "graphics",  "save_width",      512,   true,  256,   2048  };
        this->save_height      = { "graphics",  "save_height",     256,   true,  128,   1024  };
        this->view_resolution  = { "graphics",  "view_resolution", 2048,  true,  128,   4096  };
        this->gravity          = { "constants", "gravity",         -9.81, true,  -100,  0     };
        this->price_fuel       = { "constants", "price_fuel",      50,    true,  1,     10000 };
        this->price_paint      = { "constants", "price_paint",     50,    true,  1,     10000 };
        this->keep_throttle    = { "constants", "keep_throttle",   1.0,   true,  0.0,   1.0   };
        this->handbrake_power  = { "constants", "handbrake_power", 1.0,   true,  0.0,   1.0   };
        this->brake_power      = { "constants", "brake_power",     -1.0,  true,  -1.0,  0.0   };
        this->friend_damage    = { "constants", "friend_damage",   0,     true,  0,     1     };
		this->auto_brake_angle = { "constants", "auto_brake_angle",50,    true,  0,     180   };
        this->lua_enabled      = { "lua_binds", "Enabled"         ,0,     true,  0,     1     };
        this->lua_scripts      = { "lua_binds", "Script_" };
        this->posteffectreload = { "constants", "posteffectreload",0,     true,  0,     1     };
        this->show_load_every  = { "constants", "show_load_every", 100,   true,  0,     uint32_t(-1)};

        Config::INSTANCE = this;

        this->Load();
        this->Dump();
    };

    Config::~Config() {        
        this->Dump();

        Config::INSTANCE = nullptr;
    };

    void Config::Load() {
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
        this->LoadValue(&this->show_load_every);
    };

    void Config::Dump() {
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
        this->DumpValue(&this->show_load_every);
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

                    GetPrivateProfileStringA(key, "Ware", "", buffer, sizeof(buffer), CONFIG_PATH);
                    if (strnlen_s(buffer, sizeof(buffer)) == 0)
                        continue;
                    std::string ware = buffer;

                    configstructs::WareType type = (strcmp(prefix, configstructs::REPAIR) == 0) ? configstructs::WareType::REPAIR : configstructs::WareType::REFUEL;

                    value->value.emplace_back(units, ware, type);
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
            std::snprintf(buffer, sizeof(buffer), "%.03f", value->value);
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
        else if constexpr (std::is_same_v<std::vector<configstructs::WareUnits>, T>) {
            int repairIndex = 1;
            int refuelIndex = 1;
            for (const auto& wareUnit : value->value) {
                char key[128];
                if (wareUnit.Type == configstructs::WareType::REPAIR) {
                    std::snprintf(key, sizeof(key), "%s%d", configstructs::REPAIR, repairIndex++);
                }
                else {
                    std::snprintf(key, sizeof(key), "%s%d", configstructs::REFUEL, refuelIndex++);
                }
                std::snprintf(buffer, sizeof(buffer), "%.03f", wareUnit.Units);
                WritePrivateProfileStringA(key, "Units", buffer, CONFIG_PATH);
                WritePrivateProfileStringA(key, "Ware", wareUnit.Ware.c_str(), CONFIG_PATH);
            }
		}
        else {
            throw "Unsupported type";
        }
    };
};