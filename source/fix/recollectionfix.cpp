#include "fix/recollectionfix.hpp"
#include "routines.hpp"

#include "hta/ai/VehicleRecollection.hpp"

namespace kraken::fix::recollection {
    void Apply(void) {
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009ACB40), &ai::VehicleRecollection::Update);
    };
};
