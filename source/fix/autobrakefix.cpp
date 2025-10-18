#include "hta/ai/Vehicle.hpp"
#include "hta/ai/DrivingValues.h"
#include "fix/autobrakefix.hpp"
#include "config.hpp"
#include "routines.hpp"

namespace kraken::fix::autobrakefix {
    static int g_auto_brake_angle = 50;

    void __fastcall SetThrottle(ai::Vehicle* vehicle, int, float throttle, bool autobrake)
    {
        if (!vehicle->m_pPath || throttle < 0 || vehicle->m_turningBackStatus == ai::Vehicle::TURN_BACK_ENABLED_BRAKING) {
            // Стандартное поведение
            vehicle->SetThrottle(throttle, autobrake);
            return;
        }

        ai::DrivingValues dv;
        CVector curPoint, nextPoint;

        FUNC(0x006A9D60, void, __fastcall, _GetPathItem, const ai::Path*, int, CVector*);

        _GetPathItem(vehicle->m_pPath, vehicle->m_pathNum, &curPoint);
        _GetPathItem(vehicle->m_pPath, vehicle->m_pathNum + 1, &nextPoint);

        if (vehicle->m_pathNum >= vehicle->m_pPath->m_size - 1) {
            nextPoint = curPoint;
        }

        FUNC(0x005D57A0, void, __fastcall, _CalcDrivingValues, const ai::Vehicle*, const CVector*, const CVector*, bool, ai::DrivingValues*);
        _CalcDrivingValues(vehicle,
            &curPoint,
            &nextPoint,
            true,
            &dv);

        float nextAngle = abs(dv.nextAngle);
        if (fabsf(nextAngle - 3.1415) < 0.01) {
            nextAngle = 3.1415;
        }
        
        
        float dead_angle = Config::Get().auto_brake_angle.value * (3.1415 / 180.0f); // угол, меньше которого торможение не происходит
        if (nextAngle < dead_angle && vehicle->m_pathNum < vehicle->m_pPath->m_size - 1)
            autobrake = false;

        vehicle->SetThrottle(throttle, autobrake);
    }

    void Apply() {
        kraken::routines::ChangeCall((void*)0x005D3137, (void*)&SetThrottle);
    }
}