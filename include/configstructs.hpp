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
        WareUnits(float units, float armor, const std::string& ware, WareType type, const std::string& sound)
            : Units(units), Armor(armor), Ware(ware), Type(type), Sound(sound)
        {
        }
        float Units;
        float Armor;
        std::string Ware;
        WareType Type;
        std::string Sound;
    };
};

#endif