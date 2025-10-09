#pragma once
#include "stdafx.hpp"
#include "PhysicObj.h"
#include "CollisionInfo.h"

namespace ai
{
	struct SimplePhysicObj : PhysicObj
	{
		void* m_physicBody;
		stable_size_vector<ai::CollisionInfo> m_collisionInfos;
		float m_scale;
		bool m_deadTimerActive;
		float m_deadTimer;
		bool m_testVisibility;
	};
}

ASSERT_SIZE(ai::SimplePhysicObj, 0x144);