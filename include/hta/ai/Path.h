#pragma once
#include "hta/Func.h"

namespace ai {
	struct Path {
		unsigned int m_size;
		char _offset[0x74];
		void PathDtor()
		{
			FUNC(0x007BECD0, void, __thiscall, _PathDtor, Path*);
			return _PathDtor(this);
		}
	};
}

ASSERT_SIZE(ai::Path, 0x78);