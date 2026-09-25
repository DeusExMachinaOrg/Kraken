#define LOGGER "radiofix"

#include "ext/logger.hpp"
#include "fix/radiomanagerfix.hpp"
#include "config.hpp"

#include "hta/ai/Player.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "routines.hpp"

#include <cstring>

// Fix: radio phrases stop working after a map transition (and only come back
// after a save/load).
//
// The player's RadioManager (ai::Player::m_radioManager, +0x13c) is NOT a child
// of the player and is NOT serialized; it is a free-standing ObjContainer object
// that Player::_InternalPostLoad (0x006514A0) creates from the "radioManager"
// prototype and whose obj-id it stores back into the player. _InternalPostLoad
// runs from the PostLoad phase (Obj+0x50 flag), i.e. on every real save-game load
// — which is exactly why a save/load restores the radio.
//
// A map transition takes a different path: CServer::Clear (0x005F9580) wipes the
// world with ObjContainer::Clear (the free radio object dies) AND destroys
// theProcessManager, where every radio event subscription lives. The player team
// is then carried to the new map as a *live* object (no re-PostLoad), so
// _InternalPostLoad never re-runs and the player keeps a dangling radio id. Stock
// HTA never notices, so the radio stays dead until the next save/load.
//
// Self-heal it: each Player::Update, if the stored radio id no longer resolves to
// a live object, re-run _InternalPostLoad — the same recovery a save/load performs.
// Once the radio object exists again, the vehicle radar re-wires everything on its
// own: Vehicle::SubscribeRadioManagerOnNearbyObjId fetches GetRadioManagerId()
// live each time it re-detects an object on the fresh map, so subscriptions rebuild
// against the new radio without any extra work here.
namespace kraken::fix::radiomanagerfix {
    namespace {
        // ai::Player::Update (0x00652990). First instruction is
        //   movss xmm0, dword ptr [0x009e59a0]   (f3 0f 10 05 a0 59 9e 00, 8 bytes)
        // an absolute global load -> position-independent, safe to relocate into a
        // trampoline.
        constexpr uintptr_t kPlayerUpdate = 0x00652990;
        static uint8_t s_updateTrampoline[16];

        // True while the player's radio-manager id still points at a live object.
        // GetEntityByObjId is null-safe for 0 / -1 / generation-stale ids, so a
        // purged radio (or none yet) reads back as null.
        bool HasLiveRadioManager(hta::ai::Player* player) {
            hta::ai::CServer* server = hta::ai::CServer::Instance();
            if (!server || !server->m_pObjects) {
                return true; // no world container yet — nothing to heal
            }
            return server->m_pObjects->GetEntityByObjId(player->GetRadioManagerId()) != nullptr;
        }

        void __fastcall PlayerUpdate_Hook(hta::ai::Player* self, void* edx,
                                          float elapsedTime, uint32_t workTime) {
            // _InternalPostLoad creates the radio unconditionally, so guard on the
            // id actually being dead to avoid leaking a live one. Creating an object
            // mid-Update is safe here — the engine's own generators do the same.
            if (!HasLiveRadioManager(self)) {
                self->_InternalPostLoad();
            }

            using Fn = void(__fastcall*)(hta::ai::Player*, void*, float, uint32_t);
            reinterpret_cast<Fn>(static_cast<void*>(s_updateTrampoline))(self, edx, elapsedTime, workTime);
        }
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (!config.radio_manager_fix.value) {
            return;
        }
        LOG_INFO("Feature enabled");

        // Trampoline ai::Player::Update: copy the displaced 8-byte movss, then jump
        // back to the rest of the body (origUpdate + 8). Update is a plain thiscall
        // (ends in `ret 8`), so the __fastcall hook signature balances the stack.
        void* const origUpdate = reinterpret_cast<void*>(kPlayerUpdate);
        DWORD oldProt;
        VirtualProtect(s_updateTrampoline, sizeof(s_updateTrampoline), PAGE_EXECUTE_READWRITE, &oldProt);
        std::memcpy(s_updateTrampoline, origUpdate, 8);
        s_updateTrampoline[8] = 0xE9; // jmp rel32
        const uintptr_t jmpSrc = reinterpret_cast<uintptr_t>(s_updateTrampoline) + 13;
        const uintptr_t jmpTarget = reinterpret_cast<uintptr_t>(origUpdate) + 8;
        *reinterpret_cast<int32_t*>(s_updateTrampoline + 9) = static_cast<int32_t>(jmpTarget - jmpSrc);

        kraken::routines::Redirect(8, origUpdate, reinterpret_cast<void*>(&PlayerUpdate_Hook));
    }
}
