#pragma once
#include "hta/CStr.h"
#include "hta/Game.h"
#include "hta/m3d/CStrHash.hpp"
#include "hta/ai/PrototypeInfo.h"

namespace ai
{
	struct PrototypeManager
	{ /* Size=0x38 */
	/* 0x0000 */ public: m3d::CStrHash<CStr> m_prototypeFullNames;
	/* 0x000c */ public: m3d::CStrHash<unsigned int> m_prototypeFullNamesLocalizedForms;
	/* 0x0018 */ public: m3d::CStrHash<int> m_prototypeNamesToIds;
	/* 0x0024 */ public: stable_size_vector<ai::PrototypeInfo*> m_prototypes;
	/* 0x0034 */ public: LONG m_loadingLock;

	inline static PrototypeManager*& Instance = *(PrototypeManager**)0x00A18AA8;

	void GetPrototypeName(CStr* result, int prototypeId)
	{
		FUNC(0x006E72D0, void, __thiscall, _GetPrototypeName, PrototypeManager*, CStr*, int);
		return _GetPrototypeName(this, result, prototypeId);
	}
	};
}