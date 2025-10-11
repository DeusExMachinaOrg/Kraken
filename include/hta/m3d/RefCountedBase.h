#pragma once
#include "hta/Func.h"

namespace m3d
{
	struct RefCountedBase
	{
		virtual void Dtor() = 0;

		int refCount;
	};
	ASSERT_SIZE(m3d::RefCountedBase, 0x8);
}