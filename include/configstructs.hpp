#ifndef __KRAKEN_CONFIGSTRUCTS_HPP__
#define __KRAKEN_CONFIGSTRUCTS_HPP__

#include "stdafx.hpp"

namespace kraken::configstructs {
    constexpr const char* REFUEL = "REFUEL_";
    constexpr const char* REPAIR = "REPAIR_";
    enum class WareType {
        REPAIR,
        REFUEL
    };
    struct WareUnits {
        WareUnits(float units, float armor, const std::string& ware, WareType type, const std::string& sound, const std::string& script, bool consume)
            : Units(units), Armor(armor), Ware(ware), Type(type), Sound(sound), Script(script), Consume(consume)
        {
        }
        float Units;
        float Armor;
        std::string Ware;
        WareType Type;
        std::string Sound;
        std::string Script;  // Optional Lua script executed on use (empty = none)
        bool Consume;        // Whether using the ware removes it from the inventory
    };
};

#endif