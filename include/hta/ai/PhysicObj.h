#pragma once
#include "Obj.h"
#include "hta/Quaternion.h"

namespace ai
{
	struct PhysicObj : ai::Obj
	{
		int m_postActionFlags;
		Quaternion m_postRotation;
		CVector m_postPosition;
		int m_physicBehaviorFlags;
		CVector m_massCenter;
		int m_spaceId;
		bool m_bIsSpaceOwner;
		void* m_body;
		void* m_intersectionObstacle;
		int m_lookSphere;
		int m_boundSphere;
		int m_physicState;
		int m_bIsUpdatingByODE;
		int m_enabledCellsCount;
		bool m_bBodyEnabledLastFrame;
		int SkinNumber;
		int m_timeFromLastCollisionEffect;

		CVector* GetDirection(CVector* result)
		{
			FUNC(0x005FA360, CVector*, __thiscall, _GetDirection, PhysicObj*, CVector*);
			return _GetDirection(this, result);
		}

		CVector GetPosition()
		{
			FUNC(0x005FC410, CVector*, __thiscall, _GetPosition, PhysicObj*, CVector*);
			
			CVector result;
			_GetPosition(this, &result);
			return result;
		}

        CVector GetGeometricCenter()
        {
            FUNC(0x005FC6B0, CVector*, __thiscall, _GetGeometricCenter, PhysicObj*, CVector*);

            CVector result;
            _GetGeometricCenter(this, &result);
            return result;
        }
	};
}