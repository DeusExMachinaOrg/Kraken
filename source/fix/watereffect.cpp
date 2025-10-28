#define LOGGER "water effect"

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/m3d/Object.h"
#include "ode/ode.hpp"

namespace kraken::fix::watereffect {

    int __fastcall Fixed_CollidePOAndWater(
        m3d::Object* obj1,
        int a2,
        dContact* contacts,
        unsigned int* numContacts,
        bool reverse
    )
    {
        return 0;
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        kraken::routines::Redirect(0xC0, (int*) 0x00890DD0, Fixed_CollidePOAndWater);
    }
}