#define LOGGER "tactics"

#include "config.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/ai/CServer.hpp"
#include "hta/ai/Event.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/StaticAutoGun.hpp"
#include "hta/ai/Team.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/m3d/Kernel.hpp"

#include "fix/tactics.hpp"

namespace kraken::fix::tactics {
    void __fastcall Fixed_AttackNow(hta::ai::Team* team, int, int id) {
        hta::ai::AI* pAI = &team->m_AI;

        team->_AdjustRoles(id);

        hta::m3d::AIParam Param1 = {};
        hta::m3d::AIParam Param2 = {};
        hta::m3d::AIParam Param3 = {};

        Param1.Type = hta::m3d::eAIParamType::AIPARAM_ID;
        Param1.id = id;

        pAI->InsCommand(2, Param1, Param2, Param3);

        Param2.Detach();
        Param3.Detach();
        Param1.Detach();
    }

    static bool g_lockOnPlayer = false;

    float GetCurrentHealthPercent(hta::ai::Vehicle* vehicle) {
        return vehicle->GetHealth() / vehicle->GetMaxHealth();
    }

    float GetCurrentHealth(hta::m3d::Object* obj) {
        if (obj) {
            if      (obj->cast<hta::ai::Vehicle>())       return obj->cast<hta::ai::Vehicle>()->GetHealth();
            else if (obj->cast<hta::ai::StaticAutoGun>()) return obj->cast<hta::ai::StaticAutoGun>()->Health().m_value.m_value;
            else                                          return FLT_MAX;
        }
        return FLT_MAX;
    }

    hta::ai::Vehicle* GetVehicle(hta::m3d::Object* obj) {
        return obj ? obj->cast<hta::ai::Vehicle>() : nullptr;
    }

    bool isSameAttacker(hta::ai::Team* team, const hta::ai::Event* evn, bool lock_on_player) {
        const hta::CStr& stateName = team->m_AI.GetCurState2Name();
        if (!stateName.m_charPtr || stateName != "Attack")
            return false;

        hta::ai::ObjContainer* objcont = hta::ai::CServer::Instance()->m_pObjects;
        int attacked_vehicle_id = evn->m_senderObjId;
        hta::ai::Vehicle* attacked_vehicle = GetVehicle(objcont->GetEntityByObjId(attacked_vehicle_id));
        if (!attacked_vehicle || attacked_vehicle->m_roleId == -1)
            return false;

        hta::ai::VehicleRole* role = objcont->GetEntityByObjId(attacked_vehicle->m_roleId)->cast<hta::ai::VehicleRole>();
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

        hta::ai::Obj* attacker = objcont->GetEntityByObjId(attacker_id);
        hta::ai::Obj* current_target = objcont->GetEntityByObjId(current_target_id);
        if (!attacker || !current_target)
            return false;

        const float attacker_hp = GetCurrentHealth(attacker);
        const float current_target_hp = GetCurrentHealth(current_target);

        // Change tactic if attacker HP < current target HP
        if (current_target_hp - attacker_hp < 1E-4)
            return true;

        return false;
    }

    int __fastcall Fixed_TeamOnEvent(hta::ai::Team* team, void*, const hta::ai::Event* evn) {
        int result; // eax

        result = static_cast<hta::ai::Obj*>(team)->OnEvent(*evn);
        switch (evn->m_eventId) {
        case hta::ai::GE_OBJECT_DIE:
            team->_OnObjectDie(*evn);
            result = 1;
            break;
        case hta::ai::GE_UNDER_ATTACK: {
            // AdjustRoles only if the attacker is different from the current target
            if (isSameAttacker(team, evn, g_lockOnPlayer)) {
                result = 1;
                break;
            }
            team->_OnUnderAttack(*evn);
            result = 1;
            break;
        }
        case hta::ai::GE_NOTICE_ENEMY:
            team->_OnNoticeEnemy(*evn);
            result = 1;
            break;
        case hta::ai::GE_PLAYER_VEHICLE_CHANGED:
            result = 1;
            team->m_needAdjustBehaviour = 1;
            break;
        default:
            return result;
        }
        return result;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.tactics.value == 0)
            return;

        LOG_INFO("Feature enabled");

        if (config.tactics_lock.value != 0) {
            LOG_INFO("Lock on player enabled");
            g_lockOnPlayer = true;
        } else {
            LOG_INFO("Lock on player disabled");
            g_lockOnPlayer = false;
        }

        // kraken::routines::Redirect(0xBF, (void*) 0x00658B50, Fixed_AttackNow);
        kraken::routines::Redirect(0x79, (void*)0x00657FF0, Fixed_TeamOnEvent);
    }
};
