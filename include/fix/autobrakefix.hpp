#pragma once

namespace ai {
    struct Vehicle;
}

namespace kraken::fix::autobrakefix {
    void __fastcall SetThrottle(ai::Vehicle* vehicle, int, float throttle, bool autobrake);

    void Apply();
}