#pragma once

#include "CompoundVehiclePart.hpp"
#include "Gun.hpp"
#include "Obj.hpp"
// #include "containers.hpp"
#include "hta/m3d/Object.h"
#include "stdafx.hpp"

namespace ai {
    struct CompoundGunPrototypeInfo : CompoundVehiclePartPrototypeInfo {
        /* Size=0x11c */
        /* 0x0000: fields for CompoundVehiclePartPrototypeInfo */

        FiringTypes GetFiringType() const;
        virtual Obj* CreateTargetObject() const;
        CompoundGunPrototypeInfo();
        virtual ~CompoundGunPrototypeInfo();
    };
    static_assert(sizeof(CompoundGunPrototypeInfo) == 0x11c);

    struct CompoundGun : CompoundVehiclePart {
        /* Size=0x2d4 */
        /* 0x0000: fields for CompoundVehiclePart */
        static m3d::Class m_classCompoundGun;

        virtual ~CompoundGun();
        CompoundGun(const CompoundGunPrototypeInfo&);
        CompoundGun(const CompoundGun&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const CompoundGunPrototypeInfo* GetPrototypeInfo() const;
        void SetProperTargetId(int32_t, int32_t);
        bool Fire(bool);
        void LookAtPoint(const CVector&, float);
        bool isLookAtPoint(const CVector&, float) const;
        bool PointIsReachable(const CVector&, const stable_size_vector<int>&) const;
        bool CanShotToTarget(int32_t) const;
        float EstimateDamage(const CVector&, const stable_size_vector<int>&) const;
        float EstimateDamage() const;
        float EstimateDamageFromPosition(const CVector&, const CVector&, const stable_size_vector<int>&) const;
        float GetDamage() const;
        DamageType GetDamageType() const;
        float GetFiringRate() const;
        float GetFiringRange() const;
        float GetRechargingTime() const;
        uint32_t GetChargeSize() const;
        void SetChargeState(Gun::ChargeState);
        uint32_t GetShellsPoolSize() const;
        bool IsWithCharging() const;
        bool IsWithShellsPoolLimit() const;
        void SetShellsInCurrentCharge(uint32_t);
        void SetShellsInPool(uint32_t);
        Gun::ChargeState GetChargeState() const;
        float GetCurrentRechargingTime() const;
        void Recharge();
        virtual uint32_t GetPrice(const IPriceCoeffProvider*) const;
        bool IsDurabilityEnoughForFiring() const;
        virtual bool CanFire() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();

		const ai::CompoundGunPrototypeInfo* GetPrototypeInfo()
		{
			FUNC(0x006E5530, const ai::CompoundGunPrototypeInfo*, __thiscall, _GetPrototypeInfo, VehiclePart*);
			return _GetPrototypeInfo(this);
		}

		bool IsWithCharging()
		{
			FUNC(0x006E5350, bool, __thiscall, _IsWithCharging, CompoundGun*);
			return _IsWithCharging(this);
		}

		bool IsWithShellsPoolLimit()
		{
			FUNC(0x006E5360, bool, __thiscall, _IsWithShellsPoolLimit, CompoundGun*);
			return _IsWithShellsPoolLimit(this);
		}

        int32_t GetShellPrototypeId() const
		{
			FUNC(0x006E5370, int32_t, __thiscall, _GetShellPrototypeId, const CompoundGun*);
			return _GetShellPrototypeId(this);
		}

        uint32_t GetShellsInPool() const
        {
			FUNC(0x006E5330, uint32_t, __thiscall, _GetShellsInPool, const CompoundGun*);
			return _GetShellsInPool(this);
        }

        uint32_t GetShellsInCurrentCharge() const
        {
			FUNC(0x006E5320, uint32_t, __thiscall, _GetShellsInPool, const CompoundGun*);
			return _GetShellsInPool(this);
        }
        
    };
    static_assert(sizeof(CompoundGun) == 0x2d4);
};