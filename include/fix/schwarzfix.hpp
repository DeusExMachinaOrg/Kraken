#pragma once

namespace ai {
    struct Vehicle;
}
namespace kraken::fix::schwarzfix {
    uint32_t __fastcall GetSchwarz(ai::Vehicle* vehicle, void* _);
    
    void Apply();
}