#pragma once
#include "Obj.h"
#include "Formation.h"
#include "Vehicle.hpp"
#include "AI.hpp"

namespace ai {
    class CombatMastermind;

    struct Team : Obj {
        bool m_bRemoveWhenChildrenDead;
        bool m_bUseStandardUpdatingBehavior;
        BYTE _offset1[0x2];
        ai::AI m_AI;
        stable_size_vector<ai::Vehicle *> m_vehicles;
        float m_maxTeamSpeed;
        ai::Formation *m_formation;
        ai::CombatMastermind *m_combatMastermind;
        ai::Path *m_pPath;
        stable_size_map<int,CVector> m_steeringForceMap;
        bool m_needAdjustBehaviour;
        bool m_bFrozen;
        BYTE _offset2[0x2];
        CStr m_TeamTacticName;
        int m_TeamTacticId;
        bool m_TeamTacticShouldBeAssigned;
        bool m_bMustMoveToTarget;
        BYTE _offset3[0x2];

        void AttackNow(int id)
        {
            FUNC(0x00658B50, void, __thiscall, _AttackNow, Team*, int);
            return _AttackNow(this, id);
        }
        void _AdjustBehaviour()
        {
            FUNC(0x00658D50, void, __thiscall, AdjustBehaviour, Team*);
            return AdjustBehaviour(this);
        }
        void _AdjustRoles(int id)
        {
            FUNC(0x00657C30, void, __thiscall, AdjustRoles, Team*, int);
            return AdjustRoles(this, id);
        }
    };
    ASSERT_SIZE(ai::Team, 0x168);
}