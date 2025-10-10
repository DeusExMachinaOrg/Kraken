#pragma once
#include "utils.hpp"


namespace ai
{
	struct GlobalProperties
	{
		static inline GlobalProperties* theGlobProp = (GlobalProperties*)0x00A1A0F0;

        /* 0x0000 */ PointBase<int> m_izvratRepositoryMaxSize;
        /* 0x0008 */ PointBase<int> m_groundRepositorySize;
        /* 0x0010 */ CStr m_pathToRelationship;
        /* 0x001c */ CStr m_pathToGameObjects;
        /* 0x0028 */ CStr m_pathToQuests;
        /* 0x0034 */ CStr m_pathToResourceTypes;
        /* 0x0040 */ CStr m_pathToAffixes;
        /* 0x004c */ CStr m_pathToVehiclePartTypes;
		/* 0x0058 */ float m_gameTimeMult;
		/* 0x005c */ std::uint8_t _padding2[0xF4];
	}; 
}
ASSERT_SIZE(ai::GlobalProperties, 0x150);