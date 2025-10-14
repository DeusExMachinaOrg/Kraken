#pragma once
#include "PhysicObj.h"
#include "VehiclePart.h"
#include "IPriceCoeffProvider.h"

namespace ai
{
	struct ComplexPhysicObj : ai::PhysicObj
	{
		stable_size_map<CStr, ai::VehiclePart*> m_vehicleParts;
		uint32_t m_contourColor;
		float m_contourWidth;
		bool m_isContoured;
		int32_t m_targetId;
		float m_timeoutForReAimGuns;
		CVector m_currentTargetPos;

		ai::VehiclePart* GetPartByName(const CStr* partName)
		{
			FUNC(0x006BF720, ai::VehiclePart*, __thiscall, _GetPartByName, ai::ComplexPhysicObj*, const CStr*);
			return _GetPartByName(this, partName);
		}

		unsigned int GetPrice(const ai::IPriceCoeffProvider* provider)
		{
			FUNC(0x006BFA30, unsigned int, __thiscall, _GetPrice, ComplexPhysicObj*, const ai::IPriceCoeffProvider*);
			return _GetPrice(this, provider);
		}
	};
	ASSERT_SIZE(ai::ComplexPhysicObj, 0x14C);
}