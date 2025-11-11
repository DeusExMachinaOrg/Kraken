#include "hta/ai/StaticAutoGun.hpp"

namespace ai {
    ai::NumericInRange<float>& ai::StaticAutoGun::Health() {
        FUNC(0x00742020, ai::NumericInRange<float>&, __thiscall, _Health, const StaticAutoGun*);
        return _Health(this);
    }
}