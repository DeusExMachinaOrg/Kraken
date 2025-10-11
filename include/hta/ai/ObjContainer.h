#pragma once
#include <utils.hpp>
#include "Obj.h"


namespace ai
{
	struct ObjContainer
	{
		float GetGameTimeDiff()
		{
			FUNC(0x0062D230, float, __thiscall, _GetGameTimeDiff, ObjContainer*);
			return _GetGameTimeDiff(this);
		}

		Obj* GetEntityByObjId(std::int32_t objId)
		{
			FUNC(0x0040C310, Obj*, __thiscall, _GetEntityByObjId, ObjContainer*, std::int32_t);
			return _GetEntityByObjId(this, objId);
		}

		static inline ObjContainer*& theObjects = *(ObjContainer**)0x00A12E98;
		std::uint8_t _padding[0x0120];
	};
}
ASSERT_SIZE(ai::ObjContainer, 0x0120);
