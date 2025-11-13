#define LOGGER "autobrakefix"

#include "fix/autobrakefix.hpp"
#include "config.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/CVector.hpp"
#include "hta/ai/Path.hpp"
#include "hta/ai/Shared.hpp"
#include "hta/ai/Vehicle.hpp"

namespace kraken::fix::autobrakefix {
    static int g_auto_brake_angle = 50;

    static void __fastcall SetThrottle(hta::ai::Vehicle* vehicle, int, float throttle, bool autobrake) {
        if (!vehicle->m_pPath || throttle < 0 || vehicle->m_turningBackStatus == hta::ai::Vehicle::TURN_BACK_ENABLED_BRAKING) {
            // Стандартное поведение
            vehicle->SetThrottle(throttle, autobrake);
            return;
        }

        hta::ai::DrivingValues dv;
        hta::CVector curPoint, nextPoint;

        // TODO: Move it to `hta/ai/Shared.[hc]pp`
        static auto GetPathItem = (char(__fastcall*)(hta::ai::Path*, uint32_t, hta::CVector*))(0x006A9D60);
        GetPathItem(vehicle->m_pPath, vehicle->m_pathNum, &curPoint);
        GetPathItem(vehicle->m_pPath, vehicle->m_pathNum + 1, &nextPoint);

        if (vehicle->m_pathNum >= vehicle->m_pPath->m_size - 1) {
            nextPoint = curPoint;
        }

        // TODO: Move it to `hta/ai/Shared.[hc]pp`
        static auto CalcDrivingValues = (void(__fastcall*)(const hta::ai::Vehicle&, const hta::CVector&, const hta::CVector&, bool, hta::ai::DrivingValues&))(0x005D57A0);
        CalcDrivingValues(*vehicle, curPoint, nextPoint, true, dv);

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
        LOG_INFO("Feature enabled");
        kraken::routines::ChangeCall((void*)0x005D3137, (void*)&SetThrottle);
    }
}