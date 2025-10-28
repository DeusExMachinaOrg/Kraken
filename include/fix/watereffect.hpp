#pragma once
#include "config.hpp"
#include "hta/m3d/Object.h"
#include "ode/ode.hpp"

namespace kraken::fix::watereffect {
    int Fixed_CollidePOAndWater(m3d::Object* obj1, int a2, dContact* contacts, unsigned int* numContacts, bool reverse);
    void Apply();
}