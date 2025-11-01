#include "hta/ai/Vehicle.hpp"

namespace ai {
    float Vehicle::GetHealth() const {
        FUNC(0x005D0BC0, float, __thiscall, _GetMaxHealth, const Vehicle*);
        return _GetMaxHealth(this);
    };

    float Vehicle::GetMaxHealth() const {
        FUNC(0x005D0C00, float, __thiscall, _GetMaxHealth, const Vehicle*);
        return _GetMaxHealth(this);
    };

    float Vehicle::GetFuel() const {
        FUNC(0x005D0C40, float, __thiscall, _GetFuel, const Vehicle*);
        return _GetFuel(this);
    };

    float Vehicle::GetMaxFuel() const {
        FUNC(0x005D0C80, float, __thiscall, _GetMaxFuel, const Vehicle*);
        return _GetMaxFuel(this);
    };

	float Vehicle::GetMaxFullDurability() const {
		FUNC(0x0005D0D70, float, __thiscall, _GetMaxFullDurability, const Vehicle*);
		return _GetMaxFullDurability(this);
	}

	float Vehicle::GetFullDurability() const {
		FUNC(0x0005D0CF0, float, __thiscall, _GetFullDurability, const Vehicle*);
		return _GetFullDurability(this);
	}

    void Vehicle::ActivateHeadLights(bool bActivate) {
        FUNC(0x005DA130, void, __thiscall, _ActivateHeadLights, Vehicle*, bool);
        _ActivateHeadLights(this, bActivate);
    };

    void Vehicle::SetThrottle(float throttle, bool autoBrake) {
        FUNC(0x005D1210, void, __thiscall, _SetThrottle, Vehicle*, float, bool);
        _SetThrottle(this, throttle, autoBrake);
    };

    CVector Vehicle::GetLinearVelocity() const {
        FUNC(0x005D11C0, CVector*, __thiscall, _GetLinearVelocity, const Vehicle*, CVector*);
        CVector result;
        _GetLinearVelocity(this, &result);
        return result;
    };

    const VehiclePrototypeInfo* Vehicle::GetPrototypeInfo() const {
        FUNC(0x005DC890, const VehiclePrototypeInfo*, __thiscall, _GetPrototypeInfo, const Vehicle*);
        return _GetPrototypeInfo(this);
    };

    Vehicle::VehicleMoveStatus Vehicle::GetMoveStatus() const {
        FUNC(0x005CBB10, VehicleMoveStatus, __thiscall, _GetMoveStatus, const Vehicle*);
        return _GetMoveStatus(this);
    };

    const Basket* Vehicle::GetBasket() const {
        FUNC(0x005CBA90, Basket*, __thiscall, _GetBasket, const Vehicle*);
        return _GetBasket(this);
    };

    Basket* Vehicle::GetBasket() {
        FUNC(0x005CBA00, Basket*, __thiscall, _GetBasket, Vehicle*);
        return _GetBasket(this);
    }

    const Cabin* Vehicle::GetCabin() const
    {
        FUNC(0x005CBA60, Cabin*, __thiscall, _GetCabin, const Vehicle*);
        return _GetCabin(this);
    }

    Cabin* Vehicle::GetCabin() {
        FUNC(0x005CB9D0, Cabin*, __thiscall, _GetCabin, Vehicle*);
        return _GetCabin(this);
    };

    const Chassis* Vehicle::GetChassis() const {
        FUNC(0x005CBA30, Chassis*, __thiscall, _GetChassis, const Vehicle*);
        return _GetChassis(this);
    }

    Chassis* Vehicle::GetChassis() {
        FUNC(0x005CB9A0, Chassis*, __thiscall, _GetChassis, Vehicle*);
        return _GetChassis(this);
    };

    CVector Vehicle::_CalcSteeringForceToPathPoint(const CVector& point, const CVector& nextPoint) const {
        FUNC(0x005D62E0, CVector*, __thiscall, __CalcSteeringForceToPathPoint, const Vehicle*, CVector*, const CVector*, const CVector*);
        CVector result;
        __CalcSteeringForceToPathPoint(this, &result, &point, &nextPoint);
        return result;
    };
};