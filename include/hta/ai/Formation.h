#pragma once
#include "Obj.h"
#include "Path.h"
#include "hta/CVector.h"
#include "hta/CVector2.h"

namespace ai {
    struct Formation : Obj {
    float m_distBetweenVehicles;
    const unsigned int m_maxVehicles;
    float m_linearVelocity;
    float m_angularVelocity;
    CVector m_position;
    CVector m_direction;
    std::vector<CVector2> m_positions;
    ai::Path* m_pPath;
    int m_numPathPoint;
    //std::list<ai::Vehicle *> m_vehicles;

		void SetPath(ai::Path* pPath, bool bForceResetPathNum)
		{
			FUNC(0x007ECB70, void, __thiscall, _SetPath, Formation*, ai::Path*, bool);
			return _SetPath(this, pPath, bForceResetPathNum);
		}
	};
}