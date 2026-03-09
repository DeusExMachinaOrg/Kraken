#ifndef __KRAKEN_CONFIGSTRUCTS_HPP__
#define __KRAKEN_CONFIGSTRUCTS_HPP__

#include "common/stdafx.hpp"

namespace kraken::configstructs {
    constexpr const char* REFUEL = "REFUEL_";
    constexpr const char* REPAIR = "REPAIR_";
    enum class WareType {
        REPAIR,
        REFUEL
    };
    struct WareUnits {
        WareUnits(float units, float armor, const std::string& ware, WareType type)
            : Units(units), Armor(armor), Ware(ware), Type(type)
        {
        }
        float Units;
        float Armor;
        std::string Ware;
        WareType Type;
    };
};

#endif