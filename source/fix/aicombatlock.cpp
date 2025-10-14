// ============================================================================
//  AICombatLock.cpp  -  final version (all RVAs filled) (by Rakap)
// ============================================================================

#include <unordered_map>
#include <cstdint>
#include "routines.hpp"            // kraken::routines::Redirect
#include "hta/ai/Team.h"
#include "hta/ai/Formation.h"
#include "hta/m3d/Kernel.h"
#include "hta/m3d/AIParam.h"

// ============================================================================
//  Minimal shells (only the members we touch)
// ============================================================================

// ============================================================================
//  RVAs from your last message
// ============================================================================
static constexpr uintptr_t RVA_DoUnderAttack = 0x00659030;      // ai::Team::_DoUnderAttack
static constexpr uintptr_t RVA_TeamAIStartAttack = 0x00659500;  // ai::Team::TeamAIOnStartAttack
static constexpr uintptr_t RVA_TeamOnEvent = 0x00657FF0;        // ai::Team::OnEvent
static constexpr uintptr_t RVA_AdjustBehaviour = 0x00658D50;    // ai::Team::_AdjustBehaviour
static constexpr uintptr_t RVA_OnNoticeEnemy = 0x00655C10;     // ai::Team::_OnNoticeEnemy
static constexpr uintptr_t RVA_GetGameTimeInt64 = 0x0062D220;   // returns ms counter
static constexpr uintptr_t RVA_GlobalObjContainer = 0x00A12E98; // pointer to singleton

// ============================================================================
//  Typedefs for originals (we call them directly by RVA)
// ============================================================================
using Fn_DoUnderAttack = void(__thiscall*)(ai::Team*, int);
using Fn_StartAttack = m3d::AIParam* (__fastcall*)(m3d::AIParam*, void*, ai::Team*);
using Fn_TeamOnEvent = int(__fastcall*)(char*, void*, char*);
using Fn_OnSomeoneSight = void(__fastcall*)(ai::Team*, void*, ai::Event*);

static const Fn_DoUnderAttack  Orig_DoUnderAttack =
    reinterpret_cast<Fn_DoUnderAttack>(RVA_DoUnderAttack);
static const Fn_StartAttack    Orig_StartAttack =
    reinterpret_cast<Fn_StartAttack>(RVA_TeamAIStartAttack);
static const Fn_OnSomeoneSight Orig_OnSomeoneSight =
    reinterpret_cast<Fn_OnSomeoneSight>(RVA_OnNoticeEnemy);
static const auto AdjustBehaviour =
    reinterpret_cast<void(__thiscall*)(ai::Team*)>(RVA_AdjustBehaviour);

// ============================================================================
//  Timer helper - calls ObjContainer::GetGameTimeInt64 (ms counter)
// ============================================================================
inline uint32_t GameTimeMs()
{
    using Fn_GetTime = __int64(__thiscall*)(void*);
    static const Fn_GetTime GetTime =
        reinterpret_cast<Fn_GetTime>(RVA_GetGameTimeInt64);

    void* const objContainer =
        *reinterpret_cast<void* const*>(RVA_GlobalObjContainer);

    return static_cast<uint32_t>(GetTime(objContainer));
}

// ============================================================================
//  Combat-lock bookkeeping
// ============================================================================
struct CombatLock { int tgt = 0; uint32_t last = 0; };
static std::unordered_map<ai::Team*, CombatLock> g_lock;
static CombatLock& L(ai::Team* t) { return g_lock[t]; }
static void clear(ai::Team* t) { g_lock.erase(t); }

// ============================================================================
//  Hooks
// ============================================================================
void __fastcall hk_DoUnderAttack(ai::Team* self, void*, int attackerId)
{
    if (attackerId <= 0) return;
    auto& lk = L(self);
    if (lk.tgt == 0) {
        lk.tgt = attackerId;
        lk.last = GameTimeMs();
        self->AttackNow(attackerId); // call vanilla once
    }
    else {
        lk.last = GameTimeMs();                    // refresh only
    }
}

m3d::AIParam* __fastcall hk_StartAttack(m3d::AIParam* ret, ai::Team* team)
{
    auto it = g_lock.find(team);
    if (it != g_lock.end() && it->second.tgt) {
        ret->value.id = 0;
        ret->y = 0;
        ret->w = 0;
        ret->Type = m3d::AIPARAM_ID;
        ret->NameFromNum = 0;
        ret->NumFromName = 0;
        ret->value.id = it->second.tgt;
       return ret;                                  // skip tactic change
    }
    ai::Formation* m_formation; // ecx
    ai::Path* m_pPath; // ebx

    m_formation = team->m_formation;
    if (m_formation)
        m_formation->SetPath(0, 1);
    m_pPath = team->m_pPath;
    if ( m_pPath )
    {
        m_pPath->PathDtor();
        m3d::Kernel::Instance->g_mar.FreeMem(m_pPath, 0, 0);
    }
    team->m_pPath = 0;
    team->_AdjustBehaviour();

    ret->value.id = 0;
    ret->y = 0;
    ret->w = 0;
    ret->Type = m3d::AIPARAM_ID;
    ret->NameFromNum = 0;
    ret->NumFromName = 0;
    ret->value.id = 1;
    return ret;
}

void __fastcall hk_OnNoticeEnemy(ai::Team* self, void*, const ai::Event* evn)
{
    if (!evn)
        return;

    if (L(self).tgt == 0 &&
        evn->m_param1.Type == m3d::eAIParamType::AIPARAM_ID &&
        evn->m_param1.value.id > 0) {
        L(self).tgt = evn->m_param1.value.id;
        self->_AdjustBehaviour();                      // wake squad
    }
    int AsID;
    AsID = evn->m_param1.GetAsID();
    self->_DoNoticeEnemy(AsID);
}

int __fastcall Orig_TeamOnEvent(ai::Team* team, void*, const ai::Event* evn)
{
    int result; // eax
    int AsID; // eax

    result = ((ai::Obj*)team)->OnEvent(evn);
    switch (evn->m_eventId) {
    case ai::GE_OBJECT_DIE:
        team->_OnObjectDie(evn);
        result = 1;
        break;
    case ai::GE_UNDER_ATTACK:
        team->_OnUnderAttack(evn);
        result = 1;
        break;
    case ai::GE_NOTICE_ENEMY:
        hk_OnNoticeEnemy(team, nullptr, evn);
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

int __fastcall hk_TeamOnEvent(ai::Team* self, void*, const ai::Event* evn)
{
    int rv = Orig_TeamOnEvent(self, nullptr, evn);
    auto* team = reinterpret_cast<ai::Team*>(self);
    auto* ev = evn;
    auto& lk = L(team);

    switch (ev->m_eventId) {
    case 0x0009: case 0x0011:                    // OBJECT_DIE / ENEMY_DESTROYED 
        if (ev->m_senderObjId == lk.tgt) clear(team);
        break;
    case 0x0032:                                // NOTICE_ENEMY
        if (lk.tgt == 0) AdjustBehaviour(team);
        break;
    default: break;
    }
    return rv;
}

// ============================================================================
//  Patch installers - call these once during DLL init
// ============================================================================
namespace kraken::fix::aicombatlockfix {
    void Patch_DoUnderAttack() {
        kraken::routines::Redirect(0x5, (void*)RVA_DoUnderAttack, (void*)&hk_DoUnderAttack);
    }
    void Patch_TeamAI_OnStartAttack() {
        kraken::routines::Redirect(0x81, (void*)RVA_TeamAIStartAttack, (void*)&hk_StartAttack);
    }
    void Patch_Team_OnEvent() {
        kraken::routines::Redirect(0x79, (void*)RVA_TeamOnEvent, (void*)&hk_TeamOnEvent);
    }
    void Patch_OnNoticeEnemy() {
        kraken::routines::Redirect(0xDD, (void*)RVA_OnNoticeEnemy, (void*)&hk_OnNoticeEnemy);
    }

    void Apply()
    {
        Patch_DoUnderAttack();
        Patch_TeamAI_OnStartAttack();
        Patch_Team_OnEvent();
        Patch_OnNoticeEnemy();
    }
}

// ============================================================================
//  END
//
//  Squads now stick to one tactic per firefight and wake instantly
//  when the next threat appears.
// ============================================================================
