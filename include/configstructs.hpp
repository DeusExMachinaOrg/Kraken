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
        WareUnits(float units, const std::string& ware, WareType type)
            : Units(units), Ware(ware), Type(type)
        {
        }
        float Units;
        std::string Ware;
        WareType Type;
    };

    struct SchwarzOverride {
        SchwarzOverride(uint32_t price, const std::string& prototype_name)
            : Price(price), PrototypeName(prototype_name)
        {   
        }
        uint32_t Price;
        std::string PrototypeName;
    };
};

#endif