#pragma once
#include "config.hpp"
#include "hta/m3d/CWorld.hpp"

namespace kraken::fix::timeofday {
    void __fastcall UpdateSun(hta::m3d::CWorld* world, void* _);

    void Apply();
}
