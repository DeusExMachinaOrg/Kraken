#pragma once

#include "hta/native.hpp"
#include "hta/ai/Vehicle.hpp"

namespace hta::ai {
    struct KVehiclePrototypeInfo : VehiclePrototypeInfo
    {
        virtual Obj* CreateTargetObject() const;
    };

    CLASS(KVehicle, Vehicle)
        CVector m_sumSurfaceVelocities;
    END_CLASS
}