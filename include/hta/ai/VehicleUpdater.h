#pragma once
#include "CVector.h"
#include "Vehicle.h"

namespace ai
{

    class VehicleUpdater
    {
    // ex private
    Vehicle *m_vehicle;
    float m_wheelRadius;
    CVector m_velocity;
    CVector m_relFrontPoint;
    CVector m_relRearPoint;

    public:
        void Update(float);
        VehicleUpdater(Vehicle *);
        CVector GetLinearVelocity() const ;
        ~VehicleUpdater();
        void CalcRpmsAndGear(float &,float &,int &) const ;
        
    // private
    void _UpdateForceAndVelocity(float);
    float _CalcWheelAVel() const ;
    };        

	ASSERT_SIZE(ai::VehicleUpdater, 0x2c);
}
