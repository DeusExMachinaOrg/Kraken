#pragma once
#include "config.hpp"

namespace ai {
    struct Vehicle;
}
namespace kraken::fix::schwarzfix {
    uint32_t __fastcall GetComplexSchwarz(ai::Vehicle* vehicle, void* _);
    uint32_t __fastcall GetSchwarz(ai::Vehicle* vehicle, void* _);
    
    void Apply();
}