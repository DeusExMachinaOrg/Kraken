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
        static Config& Get(void) { return *Config::INSTANCE; };
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