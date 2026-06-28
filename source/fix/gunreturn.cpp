#define LOGGER "gunreturn"

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cmath>

#include "fix/gunreturn.hpp"
#include "config.hpp"
#include "ext/logger.hpp"

#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/m3d/SgNode.hpp"
#include "hta/m3d/GameImpulse.hpp"

// Port of VariousHacks/GunRotation.h: when the player's guns sit idle (not firing
// and the player isn't holding a fire button) for `timeout` seconds, ease them
// back to their forward rest orientation, instead of leaving them pointed wherever
// they last aimed. We detour Gun::LookAtPoint; for the non-idle case (and all
// non-player guns) we just call the original aiming, so normal behaviour is
// untouched and only the idle "return" branch is ours.
namespace kraken::fix::gunreturn {
    namespace {
        constexpr uintptr_t LOOKATPOINT_VA = 0x006E1C80; // virtual void Gun::LookAtPoint(const CVector&, float)
        constexpr uintptr_t G_PAPP_VA      = 0x00A0A55C;
        constexpr uintptr_t IMPULSES_OFF   = 0x8B240;    // CMiracle3d::m_pImpulses

        // void __fastcall ai::CommonGeomMovedCallback(dxGeom*)  (refresh a moved geom)
        using CommonGeomMoved_t = void(__fastcall*)(void*);
        const CommonGeomMoved_t CommonGeomMovedCallback =
            reinterpret_cast<CommonGeomMoved_t>(0x007D2F60);
        // ai::Vehicle* __fastcall help::GetPlayerVehicle()
        using GetPlayerVehicle_t = hta::ai::Vehicle* (__fastcall*)();
        const GetPlayerVehicle_t GetPlayerVehicle =
            reinterpret_cast<GetPlayerVehicle_t>(0x005513B0);

        float g_timeout = 3.0f;
        bool  g_log     = false;

        // Original Gun::LookAtPoint, reached through a trampoline. thiscall is hooked
        // as __fastcall to capture the `this` pointer (ecx); edx is unused.
        using LookAtFn = void(__fastcall*)(void* gun, void* edx, const hta::CVector* lookAt, float dt);
        LookAtFn g_orig = nullptr;

        // Fire impulses to treat as "the player is trying to fire" (so the guns don't
        // snap back between shots of a slow weapon while the trigger is held).
        int  g_fireIds[6] = {0, 0, 0, 0, 0, 0};
        bool g_idsResolved = false;

        hta::m3d::GameImpulse* GetImpulses() {
            void* app = *reinterpret_cast<void**>(G_PAPP_VA);
            if (!app)
                return nullptr;
            return *reinterpret_cast<hta::m3d::GameImpulse**>(static_cast<char*>(app) + IMPULSES_OFF);
        }

        bool IsTryingToFire(hta::m3d::GameImpulse* imp) {
            if (!g_idsResolved) {
                static const char* kNames[6] = {
                    "IM_CAR_FIRE_0", "IM_CAR_FIRE_1", "IM_CAR_FIRE_2",
                    "IM_CAR_FIRE_3", "IM_CAR_FIRE_4", "IM_CAR_FIRE_ALL",
                };
                for (int i = 0; i < 6; ++i)
                    g_fireIds[i] = imp->GetImpulseIdByName(hta::CStr(kNames[i]));
                g_idsResolved = true;
            }
            for (int i = 0; i < 6; ++i)
                if (g_fireIds[i] > 0 && imp->GetImpulseState(g_fireIds[i]))
                    return true;
            return false;
        }

        void __fastcall LookAtPoint_Hook(hta::ai::Gun* gun, void* edx,
                                         const hta::CVector* lookAt, float dt) {
            hta::ai::Vehicle* player = GetPlayerVehicle();
            bool isPlayerGun = player &&
                reinterpret_cast<void*>(gun->GetOwner()) == reinterpret_cast<void*>(player);

            bool timedOut = isPlayerGun && gun->m_timeFromLastShot > g_timeout;
            if (timedOut) {
                if (hta::m3d::GameImpulse* imp = GetImpulses()) {
                    if (IsTryingToFire(imp)) {
                        gun->m_timeFromLastShot = 0.0f; // holding fire: keep aiming
                        timedOut = false;
                    }
                }
            }

            if (!timedOut) {
                g_orig(gun, edx, lookAt, dt);
                return;
            }

            // --- idle: ease the gun back to forward rest ---
            float t = dt * gun->m_turningSpeed * 2.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;

            // Horizontal: a pure Y rotation by the gun's initial mount angle.
            float ha = gun->m_initialHorizAngle * 0.5f;
            hta::Quaternion horizTarget(0.0f, std::sin(ha), 0.0f, std::cos(ha));
            hta::Quaternion cur = gun->GetNodeRelativeRotation();
            gun->SetNodeRelativeRotation(hta::Quaternion::SLerp(cur, horizTarget, t));

            // Elevation: barrel back to identity (level).
            if (hta::m3d::SgNode* barrel = gun->GetBarrelNode()) {
                hta::Quaternion identity(0.0f, 0.0f, 0.0f, 1.0f);
                hta::Quaternion barrelRot = barrel->GetRotation();
                hta::Quaternion newElev = hta::Quaternion::SLerp(barrelRot, identity, t);
                barrel->SetRotation(newElev);
            }

            // Keep the collision geom in step with the moved gun.
            if (!gun->m_pGeoms.empty()) {
                hta::ai::GeomTransform* gt = gun->m_pGeoms.front();
                if (gt && gt->m_geomId)
                    CommonGeomMovedCallback(gt->m_geomId);
            }
        }

        // 7-byte trampoline detour (mirrors fix/gamepad.cpp). Prologue is
        // `sub esp,0x20` (3) + `mov edx,[esp+0x28]` (4); both are position
        // independent (esp-relative / immediate), so they relocate cleanly. The
        // esp-relative read still lands correctly because the hook re-passes the
        // same (lookAt, dt) args when calling the trampoline.
        bool InstallHook() {
            constexpr int kPrologue = 7;
            uint8_t* fn = reinterpret_cast<uint8_t*>(LOOKATPOINT_VA);

            uint8_t* tramp = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, kPrologue + 5, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE));
            if (!tramp) {
                LOG_ERROR("Failed to allocate gun-return trampoline");
                return false;
            }
            std::memcpy(tramp, fn, kPrologue);
            tramp[kPrologue] = 0xE9; // jmp back to LookAtPoint + kPrologue
            *reinterpret_cast<int32_t*>(tramp + kPrologue + 1) =
                static_cast<int32_t>((fn + kPrologue) - (tramp + kPrologue + 5));
            g_orig = reinterpret_cast<LookAtFn>(tramp);

            DWORD prot;
            VirtualProtect(fn, kPrologue, PAGE_EXECUTE_READWRITE, &prot);
            fn[0] = 0xE9; // jmp LookAtPoint_Hook
            *reinterpret_cast<int32_t*>(fn + 1) =
                static_cast<int32_t>(reinterpret_cast<uint8_t*>(&LookAtPoint_Hook) - (fn + 5));
            for (int i = 5; i < kPrologue; ++i)
                fn[i] = 0x90; // pad the tail of the relocated instruction
            VirtualProtect(fn, kPrologue, prot, &prot);
            return true;
        }
    }

    void Apply() {
        const Config& config = Config::Instance();
        if (config.gunreturn.value == 0)
            return;

        g_timeout = config.gunreturn_timeout.value;
        g_log     = config.gunreturn_log.value != 0;

        if (InstallHook())
            LOG_INFO("Gun auto-return enabled (timeout=%.1fs)", g_timeout);
    }
}
