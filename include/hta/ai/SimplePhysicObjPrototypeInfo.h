#pragma once
#include "PhysicObjPrototypeInfo.h"
#include "utils.hpp"
#include "CollisionInfo.h"

namespace ai
{
	struct SimplePhysicObjPrototypeInfo : PhysicObjPrototypeInfo
	{
		stable_size_vector<ai::CollisionInfo> m_collisionInfos;
		bool m_bCollisionTrimeshAllowed;
		int m_geomType;// ai::GeomType
		CStr m_engineModelName;
		CVector m_size;
		float m_radius;
		float m_massValue;
	};
}