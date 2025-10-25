#define LOGGER "tactics"

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/ai/Event.hpp"
#include "hta/ai/Team.h"
#include "hta/ai/ObjContainer.hpp"

#include "fix/tactics.hpp"

namespace kraken::fix::tactics {
    void __fastcall Fixed_AttackNow(ai::Team* team, int, int id)
    {
        ai::AI* pAI = &team->m_AI;

        team->_AdjustRoles(id);

        m3d::AIParam Param1 = {};
        m3d::AIParam Param2 = {};
        m3d::AIParam Param3 = {};

        Param1.Type = m3d::eAIParamType::AIPARAM_ID;
        Param1.id = id;

        pAI->InsCommand(2, Param1, Param2, Param3);

        Param2.Detach();
        Param3.Detach();
        Param1.Detach();
    }

    bool isSameAttacker(ai::Team* team, const ai::Event* evn)
    {
        ai::AI* pAI = &team->m_AI;
        const CStr& stateName = pAI->GetCurState2Name();
        if (!stateName.m_charPtr || !stateName.Equal("Attack"))
            return false;

        int attacked_vehicle_id = evn->m_senderObjId;
        ai::Vehicle* attacked_vehicle = (ai::Vehicle*)ai::ObjContainer::theObjects->GetEntityByObjId(attacked_vehicle_id);
        if (!attacked_vehicle || attacked_vehicle->m_roleId == -1)
            return false;

        ai::VehicleRole* role = (ai::VehicleRole*)ai::ObjContainer::theObjects->GetEntityByObjId(attacked_vehicle->m_roleId);
        if (!role)
            return false;

        const int currentTarget = role->m_TargetVehicleId;
        int attacker_id = evn->m_param1.id;
        if (attacker_id == currentTarget)
            return true;
        return false;
    }

    int __fastcall Fixed_TeamOnEvent(ai::Team* team, void*, const ai::Event* evn)
    {
        int result; // eax

        result = ((ai::Obj*)team)->ai::Obj::OnEvent(*evn);
        switch (evn->m_eventId) {
        case ai::GE_OBJECT_DIE:
            team->_OnObjectDie(evn);
            result = 1;
            break;
        case ai::GE_UNDER_ATTACK:
        {
            // AdjustRoles only if the attacker is different from the current target
            if (isSameAttacker(team, evn)) {
                result = 1;
                break;
            }
            team->_OnUnderAttack(evn);
            result = 1;
            break;
        }
        case ai::GE_NOTICE_ENEMY:
            team->_OnNoticeEnemy(evn);
            result = 1;
            break;
        case ai::GE_PLAYER_VEHICLE_CHANGED:
            result = 1;
            team->m_needAdjustBehaviour = 1;
            break;
        default:
            return result;
        }
        return result;
    }

    void Apply()
    {
        const kraken::Config& config = kraken::Config::Get();
        if (config.tactics.value == 0)
            return;

        LOG_INFO("Feature enabled");
        //kraken::routines::Redirect(0xBF, (void*) 0x00658B50, Fixed_AttackNow);
        kraken::routines::Redirect(0x79, (void*) 0x00657FF0, Fixed_TeamOnEvent);
    }
};
