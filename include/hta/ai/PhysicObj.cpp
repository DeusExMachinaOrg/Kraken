#include "hta/ai/PhysicObj.h"

namespace ai {
    void PhysicObj::AddRelTorque(const CVector& torque)
    {
        FUNC(0x005FAA20, void, __thiscall, _AddRelTorque, PhysicObj*, const CVector&);
        _AddRelTorque(this, torque);
    };

    void PhysicObj::AddForce(const CVector& force)
    {
        FUNC(0x005FA7C0, void, __thiscall, _ActivateHeadLights, PhysicObj*, const CVector&);
        _ActivateHeadLights(this, force);
    };
};