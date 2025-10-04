#pragma once
#include "hta/m3d/Object.h"

namespace ai
{
	struct ComplexPhysicObjPartDescription : m3d::Object
	{
		int m_partResourceId;
		stable_size_vector<CStr> m_lpNames;
	};
}

ASSERT_SIZE(ai::ComplexPhysicObjPartDescription, 0x48);