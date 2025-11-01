#define LOGGER "tactics"

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/ai/Event.hpp"
#include "hta/ai/Team.h"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/StaticAutoGun.hpp"

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

    static bool g_lockOnPlayer = false;

    float GetCurrentHealthPercent(ai::Vehicle* vehicle)
    {
        return vehicle->GetHealth() / vehicle->GetMaxHealth();
    }

    float GetCurrentHealth(m3d::Object* obj)
    {
        if (obj) {
            if (obj->IsKindOf(ai::Vehicle::m_classVehicle)) {
                ai::Vehicle* vehicle = (ai::Vehicle*)obj;
                return vehicle->GetHealth();
            }
            else if (obj->IsKindOf(ai::StaticAutoGun::m_classStaticAutoGun)) {
                ai::StaticAutoGun* autoGun = (ai::StaticAutoGun*)obj;
                return autoGun->Health().m_value.m_value;
            }
            else
                return FLT_MAX; // Not vehicle (breakpoint)
        }
        return FLT_MAX;
    }

    ai::Vehicle* GetVehicle(m3d::Object* obj)
    {
        if (obj) {
            if (obj->IsKindOf(ai::Vehicle::m_classVehicle))
                return (ai::Vehicle*)obj;
            else
                return nullptr; // Not vehicle (breakpoint)
        }
        return nullptr;
    }

    bool isSameAttacker(ai::Team* team, const ai::Event* evn, bool lock_on_player)
    {
        const CStr& stateName = team->m_AI.GetCurState2Name();
        if (!stateName.m_charPtr || !stateName.Equal("Attack"))
            return false;

        int attacked_vehicle_id = evn->m_senderObjId;
        ai::Vehicle* attacked_vehicle = GetVehicle(ai::ObjContainer::theObjects->GetEntityByObjId(attacked_vehicle_id));
        if (!attacked_vehicle || attacked_vehicle->m_roleId == -1)
            return false;

        ai::VehicleRole* role = (ai::VehicleRole*)ai::ObjContainer::theObjects->GetEntityByObjId(attacked_vehicle->m_roleId);
        if (!role)
            return false;

        const int current_target_id = role->m_TargetVehicleId;
        int attacker_id = evn->m_param1.id;

        // Same attacker, skip tactic change
        if (attacker_id == current_target_id)
            return true;

        // Lock on player vehicle
        if (lock_on_player) {
            if (current_target_id == 1) // Ignore non-player attackers
                return true;
            if (attacker_id == 1) // Attack player
                return false;
        }

        ai::Obj* attacker = ai::ObjContainer::theObjects->GetEntityByObjId(attacker_id);
        ai::Obj* current_target = ai::ObjContainer::theObjects->GetEntityByObjId(current_target_id);
        if (!attacker || !current_target)
            return false;

        const float attacker_hp = GetCurrentHealth(attacker);
        const float current_target_hp = GetCurrentHealth(current_target);

        // Change tactic if attacker HP < current target HP
        if (current_target_hp - attacker_hp < 1E-4)
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
            if (isSameAttacker(team, evn, g_lockOnPlayer)) {
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

        if (config.tactics_lock.value != 0) {
            LOG_INFO("Lock on player enabled");
            g_lockOnPlayer = true;
        }
        else {
            LOG_INFO("Lock on player disabled");
            g_lockOnPlayer = false;
        }

        //kraken::routines::Redirect(0xBF, (void*) 0x00658B50, Fixed_AttackNow);
        kraken::routines::Redirect(0x79, (void*) 0x00657FF0, Fixed_TeamOnEvent);
    }
};
