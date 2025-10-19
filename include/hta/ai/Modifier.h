#pragma once
#include "hta/CStr.h"
#include "hta/m3d/AIParam.hpp"

namespace ai
{
	struct Modifier
	{
		float timeOut;
		int Operation;
		int m_magicPrototypeId;
		CStr PropertyName;
		int SenderID;
		m3d::AIParam Value;
	};
}