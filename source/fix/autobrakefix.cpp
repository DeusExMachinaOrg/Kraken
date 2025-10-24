#define LOGGER "autobrakefix"

#include "hta/ai/Vehicle.hpp"
#include "hta/ai/DrivingValues.h"
#include "ext/logger.hpp"
#include "fix/autobrakefix.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"

namespace kraken::fix::autobrakefix {
    static int g_auto_brake_angle = 50;

void __fastcall _ApplyStabilizingForces(ai::Vehicle *vehicle)
{
    unsigned int flags = vehicle->m_flags;
    
    // Check if vehicle is not alive or has parent repository
    if ((flags & 8) != 0 || (flags & 2) != 0 || vehicle->GetParentRepository())
    {
        LOG_ERROR("IsAlive()");
        return;
        // temporary, check if we need any additional early exit actions
    }
    
    CVector velocity = vehicle->GetLinearVelocity();
    
    // Apply downforce (pressing force in Targem's therms) if wheels are touching ground
    if (vehicle->m_numWheelsTouchingGround > 0)
    {
        float horizontalVel = sqrt(velocity.z * velocity.z + velocity.x * velocity.x);
        
        if (horizontalVel > 5.0)
        {
            float pressingForce = vehicle->GetPrototypeInfo()->m_pressingForce;
            float mass = vehicle->GetMass();
            
            CVector downforce;
            downforce.x = 0.0f;
            downforce.y = mass * pressingForce * horizontalVel * -0.1962f;
            downforce.z = 0.0f;
            
            vehicle->AddForce(downforce);
        }
    }
    
    // Calculate total velocity magnitude
    float totalVel = sqrt(velocity.y * velocity.y + velocity.z * velocity.z + velocity.x * velocity.x);
    
    // Apply counter-steering stabilization torque
    if (vehicle->m_numWheelsTouchingGround > 0)
    {
        ai::Vehicle::WheelRuntimeInfo *wheelInfo = vehicle->m_wheels._Myfirst;
        
        // Find first valid wheel
        if (wheelInfo != vehicle->m_wheels._Mylast)
        {
            while (!wheelInfo->m_wheel)
            {
                wheelInfo++;
                if (wheelInfo == vehicle->m_wheels._Mylast)
                {
                    vehicle->m_numWheelsTouchingGround = 0;
                    return;
                }
            }
            
            CVector UP_DIRECTION_VECTOR = CVector(0.0, 1.0, 0.0);

            ai::Wheel *wheel = wheelInfo->m_wheel;
            if (wheel)
            {
                float wheelAngle = wheel->m_curAngle;
                
                CVector direction;
                vehicle->GetDirection(&direction);
                
                float throttleFactor = vehicle->m_throttle * 0.5f;
                
                // Determine if moving forward or backward
                float dotProduct = (direction.y * velocity.y) + 
                                   (direction.z * velocity.z) +
                                   (direction.x * velocity.x);
                int directionSign = (dotProduct >= 0.0f) ? 1 : -1;
                
                // Calculate counter-steering torque around Y-axis (yaw)
                CVector torque; // x, z - always 0
                torque.y = -UP_DIRECTION_VECTOR.y * wheelAngle;
                
                float mass = vehicle->GetMass();
                float driftCoeff = vehicle->m_driftCoeff;
                
                // Apply mass, velocity, and drift coefficient
                torque.y = (torque.y * mass * totalVel) * driftCoeff;
                
                float cabinControlCoeff = vehicle->_GetCabinControlCoeff();
                // Default Targem logic - more response when accelerating on braking
                float throttleMultiplier = fabs(throttleFactor) + 0.5f; // Range: [0.5, 1.0]
                
                // Alternative, probably more realistic
                // Scale inversely with speed for better low-speed control
                // float speedInfluence = 1.0f / (1.0f + totalVel * 0.1f);
                
                // Apply cabin control coefficient and throttle
                torque.y *= cabinControlCoeff * directionSign * throttleMultiplier; // or speedInfluence
                vehicle->AddRelTorque(torque);
            }
        }
    }
    
    vehicle->m_numWheelsTouchingGround = 0;
    LOG_DEBUG("_ApplyStabilizingForces");
}

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
        LOG_INFO("Feature enabled");
        kraken::routines::ChangeCall((void*)0x005D3137, (void*)&SetThrottle);
        kraken::routines::ChangeCall((void*)0x005EC7A4, (void*)&_ApplyStabilizingForces);
    }
}