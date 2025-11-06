#pragma once
#include "config.hpp"
#include "hta/m3d/CWorld.hpp"

namespace ai {
    struct Vehicle;
}
namespace kraken::fix::timeofday {
    void __fastcall UpdateSun(m3d::CWorld* world, void* _);
    
    void Apply();
}