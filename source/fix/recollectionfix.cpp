#include "fix/recollectionfix.hpp"
#include "routines.hpp"

#include "ai/VehicleRecollection.hpp"

namespace kraken::fix::recollection {
    void Apply(void) {
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009ACB40), &ai::VehicleRecollection::Update);
    };
};
