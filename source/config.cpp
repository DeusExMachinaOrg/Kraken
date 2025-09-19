#include "utils.hpp"
#include "config.hpp"

namespace kraken {
    const char* CONFIG_PATH = "./data/kraken.ini";

    Config::Config() {
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

        this->Load();
        this->Dump();
    };

    Config::~Config() {
        this->Dump();
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
    };

    template<typename T>
    void Config::LoadValue(ConfigValue<T>* value) {
        char buffer[1024] = {0};

        GetPrivateProfileStringA(value->section, value->key, "", buffer, sizeof(buffer), CONFIG_PATH);

        if (strnlen_s(buffer, sizeof(buffer)) == 0)
            return;

        if constexpr (std::is_same_v<int32_t, T>) {
            value->value = std::strtol(buffer, 0, 10);
            if (value->limited)
                value->value = clamp<int32_t>(value->value, value->min, value->max);
        }
        else if constexpr (std::is_same_v<uint32_t, T>) {
            value->value = std::strtoul(buffer, 0, 10);
            if (value->limited)
                value->value = clamp<uint32_t>(value->value, value->min, value->max);
        }
        else if constexpr (std::is_same_v<float, T>) {
            value->value = std::strtof(buffer, 0);
            if (value->limited)
                value->value = clamp<float>(value->value, value->min, value->max);
        }
        else if constexpr (std::is_same_v<double, T>) {
            value->value = std::strtod(buffer, 0);
            if (value->limited)
                value->value = clamp<double>(value->value, value->min, value->max);
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
        }
        else if constexpr (std::is_same_v<uint32_t, T>) {
            std::snprintf(buffer, sizeof(buffer), "%d", value->value);
        }
        else if constexpr (std::is_same_v<float, T>) {
            std::snprintf(buffer, sizeof(buffer), "%.03f", value->value);
        }
        else if constexpr (std::is_same_v<double, T>) {
            std::snprintf(buffer, sizeof(buffer), "%.06f", value->value); 
        }
        else if constexpr (std::is_same_v<bool, T>) {
            std::snprintf(buffer, sizeof(buffer), "%s", value->value ? "true" : "false");
        }
        else {
            throw "Unsupported type";
        }

        if (strnlen_s(buffer, sizeof(buffer)) == 0)
            return;

        WritePrivateProfileStringA(value->section, value->key, buffer, CONFIG_PATH);
    };
};