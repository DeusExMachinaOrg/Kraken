#pragma once
#include "PhysicObjPrototypeInfo.h"
#include "ComplexPhysicObjPartDescription.h"
#include "hta/CVector.h"
#include "stdafx.hpp"

namespace ai
{
	struct ComplexPhysicObjPrototypeInfo : PhysicObjPrototypeInfo
	{
		stable_size_map<CStr, int> m_partPrototypeIds;
		CVector m_massSize;
		CVector m_massTranslation;
		BYTE _offset[0x24];
	};
}

ASSERT_SIZE(ai::ComplexPhysicObjPrototypeInfo, 0x90);
