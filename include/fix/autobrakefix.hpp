#pragma once

namespace ai {
    class Vehicle;
}

namespace kraken::fix::autobrakefix {
    void __fastcall SetThrottle(ai::Vehicle* vehicle, int, float throttle, bool autobrake);

    void Apply(int auto_brake_angle);
}