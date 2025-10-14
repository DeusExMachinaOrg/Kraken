#pragma once
#include "config.hpp"

namespace ai {
    struct Vehicle;
}

namespace kraken::fix::luabinds {
    void ExecuteLuaScripts();
    void Apply(const Config* config);
}