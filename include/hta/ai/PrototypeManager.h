#pragma once
#include "hta/CStr.h"
#include "hta/Game.h"

namespace ai
{
	struct PrototypeManager
	{
		inline static PrototypeManager*& Instance = *(PrototypeManager**)0x00A18AA8;

		void GetPrototypeName(CStr* result, int prototypeId)
		{
			FUNC(0x006E72D0, void, __thiscall, _GetPrototypeName, PrototypeManager*, CStr*, int);
			return _GetPrototypeName(this, result, prototypeId);
		}
	};
}