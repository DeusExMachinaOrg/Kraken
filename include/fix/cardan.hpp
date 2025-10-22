#ifndef __KRAKEN_FIX_CARDAN_HPP__
#define __KRAKEN_FIX_CARDAN_HPP__

namespace ai {
    class Vehicle;
}

namespace kraken::fix::cardan {
    void __fastcall KeepThrottle(ai::Vehicle* vehicle, void*, bool applyActions);
    void Apply();
}

#endif