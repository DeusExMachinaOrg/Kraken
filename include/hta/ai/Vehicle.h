#pragma once
#include "ComplexPhysicObj.h"
#include "IzvratRepository.h"
#include "ActionType.h"
#include "VehiclePrototypeInfo.h"
#include "Basket.h"
#include "Cabin.h"
#include "Chassis.h"
#include "Path.h"

inline static bool& LightActivated = *(bool*)0x00A418DE;

namespace ai
{
    struct Vehicle : ComplexPhysicObj
    {
        enum TurningBackStatus : __int32
        {                                       // XREF: ai::Vehicle/r
             TURN_BACK_NONE = 0x0,
             TURN_BACK_ENABLED_ACCELERATING = 0x1,
             TURN_BACK_ENABLED_BRAKING = 0x2,
             TURN_BACK_DISABLED = 0x3,
        };

        BYTE _offset[0x100];
        float m_throttle;
        float m_brake;
        float m_realThrottle;
        float m_engineRpm;
        float m_averageEngineRpm;
        float m_driftCoeff;
        float m_averageWheelAVel;
        bool m_bAutoBrake;
        bool m_bHandBrake;
        BYTE _offset2[0x16];
        float m_steerRadians;
        ai::Vehicle::TurningBackStatus m_turningBackStatus;
        BYTE _offset3[0x98];
        ai::Path* m_pPath;
        int m_pathNum;
        BYTE _offset4[0x44];
        IzvratRepository* Repository;
        void* m_groundRepository;
        stable_size_vector<ActionType> m_effectActions;

        float GetHealth()
        {
            FUNC(0x005D0BC0, float, __thiscall, _GetMaxHealth, Vehicle*);
            return _GetMaxHealth(this);
        }

        float GetMaxHealth()
        {
            FUNC(0x005D0C00, float, __thiscall, _GetMaxHealth, Vehicle*);
            return _GetMaxHealth(this);
        }

        float GetFuel()
        {
            FUNC(0x005D0C40, float, __thiscall, _GetFuel, Vehicle*);
            return _GetFuel(this);
        }

        float GetMaxFuel()
        {
            FUNC(0x005D0C80, float, __thiscall, _GetMaxFuel, Vehicle*);
            return _GetMaxFuel(this);
        }

        void ActivateHeadLights(bool bActivate)
        {
            FUNC(0x005DA130, void, __thiscall, _ActivateHeadLights, Vehicle*, bool);
            _ActivateHeadLights(this, bActivate);
        }

        void SetThrottle(float throttle, bool autoBrake)
        {
            FUNC(0x005D1210, void, __thiscall, _SetThrottle, Vehicle*, float, bool);
            _SetThrottle(this, throttle, autoBrake);
        }

        CVector* GetLinearVelocity(CVector* result)
        {
            FUNC(0x005D11C0, CVector*, __thiscall, _GetLinearVelocity, Vehicle*, CVector*);
            return _GetLinearVelocity(this, result);
        }

        const ai::VehiclePrototypeInfo* GetPrototypeInfo()
        {
            FUNC(0x005DC890, const ai::VehiclePrototypeInfo*, __thiscall, _GetPrototypeInfo, Vehicle*);
            return _GetPrototypeInfo(this);
        }

        __int32 GetMoveStatus()
        {
            FUNC(0x005CBB10, __int32, __thiscall, _GetMoveStatus, Vehicle*);
            return _GetMoveStatus(this);
        }

        Basket* GetBasket()
        {
            FUNC(0x005CBA00, ai::Basket*, __thiscall, _GetBasket, Vehicle*);
            return _GetBasket(this);
        }

        Cabin* GetCabin()
        {
            FUNC(0x005CBA00, ai::Cabin*, __thiscall, _GetCabin, Vehicle*);
            return _GetCabin(this);
        }

        Chassis* GetChassis()
        {
            FUNC(0x005CB9A0, ai::Chassis*, __thiscall, _GetChassis, Vehicle*);
            return _GetChassis(this);
        }

        CVector* _CalcSteeringForceToPathPoint(CVector* result, const CVector* point, const CVector* nextPoint)
        {
            FUNC(0x005D62E0, CVector*, __thiscall, __CalcSteeringForceToPathPoint, Vehicle*, CVector*, const CVector*, const CVector*);
            return __CalcSteeringForceToPathPoint(this, result, point, nextPoint);
        }

        bool bIsMovingAlongExternalPath()
        {
            FUNC(0x005CBB90, bool, __thiscall, _bIsMovingAlongExternalPath, Vehicle*);
            return _bIsMovingAlongExternalPath(this);
        }

        void SetHorn(bool bState)
        {
            FUNC(0x005E7510, void, __thiscall, _SetHorn, Vehicle*, bool);
            return _SetHorn(this, bState);
        }

        void SetSteer(float radians)
        {
            FUNC(0x005CC2D0, void, __thiscall, _SetSteer, Vehicle*, float);
            return _SetSteer(this, radians);
        }

        void ReleaseAllPedals()
        {
            FUNC(0x005D1340, void, __thiscall, _ReleaseAllPedals, Vehicle*);
            return _ReleaseAllPedals(this);
        }

        void SetHandBrake()
        {
            FUNC(0x005CC2B0, void, __thiscall, _SetHandBrake, Vehicle*);
            return _SetHandBrake(this);
        }

        void SetTurningToGroundForceAndTorque(const CVector* pos, const CVector* force, const CVector* torque)
        {
            FUNC(0x005CC460, void, __thiscall, _SetTurningToGroundForceAndTorque, Vehicle*, const CVector*, const CVector*, const CVector*);
            return _SetTurningToGroundForceAndTorque(this, pos, force, torque);
        }

        void GetOutOfDifficultPlace()
        {
            FUNC(0x005CBB00, void, __thiscall, _GetOutOfDifficultPlace, Vehicle*);
            return _GetOutOfDifficultPlace(this);
        }
    };
}
ASSERT_SIZE(ai::Vehicle, 0x364);