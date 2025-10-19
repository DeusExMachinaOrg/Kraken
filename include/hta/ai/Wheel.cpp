#include "hta/ai/Wheel.hpp"

namespace ai {
    float Wheel::GetRadius() const {
        const auto jump = (double (__thiscall*)(const Wheel*))(0x005DF090);
        return jump(this);
    };
    
    float Wheel::GetWidth() const {
        const auto jump = (double (__thiscall*)(const Wheel*))(0x005EE340);
        return jump(this);
    };

    Vehicle* Wheel::GetVehicle() const {
        return (Vehicle*) this->GetParent();
    };
};