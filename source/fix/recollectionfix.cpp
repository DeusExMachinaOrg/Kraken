#include "fix/recollectionfix.hpp"
#include "hta/ai/VehicleRecollection.h"
#include "routines.hpp"

namespace kraken::fix::recollection {
    void Apply(void) {
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009ACB40), unsafe_cast<void*>(&ai::VehicleRecollection::Update));
    };
};