#ifndef __KRAKEN_CONFIG_HPP__
#define __KRAKEN_CONFIG_HPP__

#include "stdafx.hpp"

namespace kraken {
    template <typename T>
    struct ConfigValue {
        const char* section;
        const char* key;
        T           value;
        bool        limited;
        T           min;
        T           max;
    };

    class Config {
    public:
        // Graphics
        ConfigValue<uint32_t> save_width;
        ConfigValue<uint32_t> save_height;
        ConfigValue<uint32_t> view_resolution;

        // Constants
        ConfigValue<float>    gravity;
        ConfigValue<uint32_t> price_fuel;
        ConfigValue<uint32_t> price_paint;
        ConfigValue<float>    keep_throttle;
        ConfigValue<float>    handbrake_power;
        ConfigValue<float>    brake_power;
        ConfigValue<uint32_t> friend_damage;

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