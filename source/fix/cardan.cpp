#define LOGGER "cardan"

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"
#include <new>

#include "hta/CVector.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/ActionType.hpp"
#include "hta/ai/Cabin.hpp"
#include "hta/ai/Basket.hpp"
#include "hta/m3d/AnimInfo.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/Shared.hpp"
#include "ode/ode.hpp"

#include "fix/cardan.hpp"
#include "fix/thorncollide.hpp"
#include "fix/wheelmodel.hpp"

#include <cstring>
#include <cstdint>
#include <Windows.h>

namespace kraken::fix::cardan {
    static std::unordered_map<hta::ai::Vehicle*, hta::CVector> surfaceVelocities;
    static std::unordered_map<hta::ai::Vehicle*, int32_t> surfaceWheelsTouchingCount;

    static bool cardan_fix_enabled = false;

    void SetChassisAnimationStopped(hta::ai::Vehicle* vehicle, bool stopped) {
        // ----------------------------------------------
        // Cardan fix
        // stopped = true -> stop chassis animation
        // ----------------------------------------------

        if (!cardan_fix_enabled)
            return;

        hta::ai::Chassis* chassis = vehicle->GetChassis();
        if (chassis) {
            hta::m3d::AnimInfo* info;
            chassis->m_Node->GetProperty(1, &info);
            if (info) {
                bool has_anim = !info->m_Empty;
                if (has_anim) {
                    chassis->SetAnimationStopped(stopped);
                }
            }
        }
    }

    static void __fastcall KeepThrottle(hta::ai::Vehicle* vehicle, void*, bool applyActions)
    {
        // -------------------------------------------------
        // Original function code from IDA
        // with modifications to fix Cardan animation issue
        // -------------------------------------------------
        int v3; // ecx
        int v4; // eax
        int v5; // eax
        int v6; // eax
        hta::CVector velocity2; // eax
        float x; // xmm2_4
        float y; // xmm1_4
        float z; // xmm0_4
        unsigned int v11; // eax
        hta::ActionType* v12; // eax
        hta::ai::Basket* v13; // eax
        hta::ai::Basket* v14; // edi
        hta::ai::Cabin* v15; // eax
        hta::ai::Cabin* v16; // edi
        hta::ActionType* v17; // eax
        hta::ai::Basket* v18; // edi
        hta::ai::Cabin* v19; // eax
        hta::ai::Cabin* v20; // edi
        unsigned int m_flags; // eax
        hta::ActionType* Myfirst; // eax
        hta::ai::Basket* Basket; // eax
        hta::ai::Basket* v24; // edi
        hta::ai::Cabin* Cabin; // eax
        hta::ai::Cabin* v26; // edi
        float wheelRpm; // [esp+8h] [ebp-28h]
        float vel; // [esp+Ch] [ebp-24h] BYREF
        float v29; // [esp+10h] [ebp-20h]
        float v30; // [esp+14h] [ebp-1Ch]
        hta::CVector dir; // [esp+18h] [ebp-18h] BYREF
        char v32[12]; // [esp+24h] [ebp-Ch] BYREF

        wheelRpm = fabs(vehicle->m_averageWheelAVel) * 9.5492964;
        hta::CVector velocity = vehicle->GetLinearVelocity();

        const int32_t surfaceWheelCount = surfaceWheelsTouchingCount[vehicle];
        if (surfaceWheelCount > 0) {
            hta::CVector surfaceVel = surfaceVelocities[vehicle] * (float)(1.0 / surfaceWheelCount);
            velocity = velocity - surfaceVel;
        }

        vel = velocity.x;
        v29 = velocity.y;
        v30 = velocity.z;

        dir = vehicle->GetDirection();
        if (vehicle->m_bAutoBrake) {
            if (wheelRpm > 5.0
                && (vehicle->m_engineRpm <= 0.000001 ? (vehicle->m_engineRpm >= -0.000001 ? (v3 = 0) : (v3 = -1)) : (v3 = 1),
                    vehicle->m_throttle <= 0.000001 ? (vehicle->m_throttle >= -0.000001 ? (v4 = 0) : (v4 = -1)) : (v4 = 1),
                    v3 * v4 <= 0)
                || wheelRpm <= 5.0
                && ((v5 = hta::RoughSign(vehicle->m_throttle)) == 0
                    || ((((dir.z * v30) + (dir.y * v29)) + (dir.x * vel)) * v5) < -1.5707964)) {
                vehicle->m_throttle = 0.0;
                vehicle->m_brake = 1.0f;
            }
        }
        if (vehicle->m_throttle <= 0.000001 && vehicle->m_throttle >= -0.000001 && sqrt(v30 * v30 + v29 * v29 + vel * vel) < 0.5)
            vehicle->m_bHandBrake = 1;

        if (vehicle->m_bHandBrake) {
            vehicle->m_throttle = 0.0;
            vehicle->m_brake = 1.0f;
        }

        if (vehicle->m_engineRpm <= 0.000001) {
            if (vehicle->m_engineRpm >= -0.000001)
                v6 = 0;
            else
                v6 = -1;
        }
        else {
            v6 = 1;
        }

        vehicle->m_realThrottle = vehicle->m_throttle - ((v6 * vehicle->m_brake) * 10.0);
        if (applyActions) {
            if (vehicle->m_brake <= 0.0001) {
                m_flags = vehicle->GetMoveStatus();
                if ((m_flags & 8) == 0 && (m_flags & 2) == 0 && !vehicle->m_parentRepository) {
                    Myfirst = &vehicle->m_effectActions[0];
                    if (Myfirst && *Myfirst != hta::AT_MOVE1) {
                        *Myfirst = hta::AT_MOVE1;
                        Basket = vehicle->GetBasket();
                        v24 = Basket;
                        if (Basket) {
                            Basket->SetEffectActions(vehicle->m_effectActions);
                            v24->SetNodeAnimAction(hta::ActionType(2), 1);
                        }

                        Cabin = vehicle->GetCabin();
                        v26 = Cabin;
                        if (Cabin) {
                            Cabin->SetEffectActions(vehicle->m_effectActions);
                            v26->SetNodeAnimAction(hta::ActionType(2), 1);
                        }

                        // Cardan fix
                        SetChassisAnimationStopped(vehicle, 0);
                    }
                }
            }
            else {
                velocity2 = vehicle->GetLinearVelocity();
                x = velocity2.x;
                y = velocity2.y;
                z = velocity2.z;
                v11 = vehicle->GetMoveStatus();
                if ((((x * x) + (y * y)) + (z * z)) >= 0.1) {
                    if ((v11 & 8) == 0 && (v11 & 2) == 0 && !vehicle->m_parentRepository) {
                        v17 = &vehicle->m_effectActions[0];
                        if (v17 && *v17 != hta::AT_MOVE2) {
                            *v17 = hta::AT_MOVE2;
                            v18 = vehicle->GetBasket();
                            if (v18) {
                                v18->SetEffectActions(vehicle->m_effectActions);
                                v18->SetNodeAnimAction(hta::ActionType(3), 1);
                            }

                            v19 = vehicle->GetCabin();
                            v20 = v19;
                            if (v19) {
                                v19->SetEffectActions(vehicle->m_effectActions);
                                v20->SetNodeAnimAction(hta::ActionType(3), 1);
                            }

                            // Cardan fix
                            SetChassisAnimationStopped(vehicle, 0);
                        }
                    }
                }
                else if ((v11 & 8) == 0 && (v11 & 2) == 0 && !vehicle->m_parentRepository) {
                    v12 = &vehicle->m_effectActions[0];
                    if (v12 && *v12) {
                        *v12 = hta::AT_STAND1;
                        v13 = vehicle->GetBasket();
                        v14 = v13;
                        if (v13) {
                            v13->SetEffectActions(vehicle->m_effectActions);
                            v14->SetNodeAnimAction(hta::ActionType(0), 1);
                        }

                        v15 = vehicle->GetCabin();
                        v16 = v15;
                        if (v15) {
                            v15->SetEffectActions(vehicle->m_effectActions);
                            v16->SetNodeAnimAction(hta::ActionType(0), 1);
                        }

                        // Cardan fix
                        SetChassisAnimationStopped(vehicle, 1);
                    }
                }
            }
        }
    }

    struct CollideVelocityInfo {
        hta::CVector velocity;
        bool bWheelsCollided;

        CollideVelocityInfo() : velocity(), bWheelsCollided(false) {}
    };

    void __fastcall HookVehicleDtor(hta::ai::Vehicle* self, void*, bool bHorn)
    {
        if (!self) return;

        surfaceVelocities.erase(self);
        surfaceWheelsTouchingCount.erase(self);

        if (self->m_bIsControlledByPlayer) {
            self->SetHorn(bHorn);
        }
    }

    hta::ai::Vehicle* __fastcall VehiclePrototypeInfo_CreateTargetObject(hta::ai::VehiclePrototypeInfo* self, void*)
    {
        hta::ai::Vehicle* vehicle = (hta::ai::Vehicle*)hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(hta::ai::Vehicle), nullptr, 0);
        if (vehicle) {
            ::new (vehicle) hta::ai::Vehicle(*self);

            surfaceVelocities[vehicle] = hta::CVector();
            surfaceWheelsTouchingCount[vehicle] = 0;
        }
        return vehicle;
    }

    void AddSurfaceVelocity(hta::ai::Vehicle* vehicle, hta::ai::PhysicObj* surface)
    {
        hta::CVector velocity = surface->GetLinearVelocity();
        if (velocity.Length() > 0.1f) {
            surfaceVelocities[vehicle] += velocity;
            surfaceWheelsTouchingCount[vehicle] += 1;
        }
    }

    void __stdcall HandleWheelSurfaceTouch(hta::ai::Wheel* wheel, hta::m3d::Object* surface)
    {
        hta::ai::Vehicle* vehicle = wheel->GetVehicle();

        if (vehicle) {
            // Engine resets m_numWheelsTouchingGround each update cycle.
            // First contact in cycle must reset our moving-surface accumulators.
            if (!vehicle->m_numWheelsTouchingGround) {
                surfaceVelocities[vehicle] = hta::CVector();
                surfaceWheelsTouchingCount[vehicle] = 0;
            }

            vehicle->IncNumWheelsTouchingGround();

            if (surface) {
                if (hta::ai::PhysicObj* obj = surface->cast<hta::ai::PhysicObj>()) {
                    AddSurfaceVelocity(vehicle, obj);
                }
            }
        }
    }

    // Trampoline holding ai::CollideWheelDefault's original 5-byte prologue
    // (83 EC 24 53 55 = sub esp,0x24; push ebx; push ebp) + a jmp back to 0x00891435,
    // so the wrapper below can run the stock tyre logic and still execute code afterwards.
    using WheelCollideFn = int(__fastcall*)(hta::ai::Wheel*, void*, dContact*, unsigned int*, bool);
    static uint8_t s_wheelCollideTramp[16];

    // Shared hook for ai::CollideWheelDefault (0x00891430). Two features need this entry:
    //   1) cardan — moving-surface velocity bookkeeping (HandleWheelSurfaceTouch);
    //   2) thorncollide — ram damage when a wheel is hit by another vehicle.
    // cardan owns the single redirect (it installs unconditionally; thorncollide only when
    // [constants] appendix is set) and forwards to thorncollide::OnWheelCollision.
    // self=ecx (Wheel), surface=edx (the struck object) — the engine's dispatch convention.
    int __fastcall Hook_CollideWheelDefault(hta::ai::Wheel* self, void* surface,
                                            dContact* contacts, unsigned int* numContacts, bool reverse) {
        auto* surfaceObj = reinterpret_cast<hta::m3d::Object*>(surface);
        HandleWheelSurfaceTouch(self, surfaceObj);

        const int result = reinterpret_cast<WheelCollideFn>(static_cast<void*>(s_wheelCollideTramp))(
            self, surface, contacts, numContacts, reverse);

        kraken::fix::thorncollide::OnWheelCollision(self, surfaceObj, contacts, numContacts);

        // Wheel model v3. Feeds the ODE manifold to the penalty model. With apply
        // off it only logs; with apply=1 it applies the §3 force and zeros
        // *numContacts (suppressing ODE's wheel response). No-op unless enabled.
        kraken::fix::wheelmodel::OnWheelContacts(self, contacts, numContacts);
        return result;
    }

    void Apply()
    {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.cardan_fix.value == true) {
            LOG_INFO("Feature enabled");
            cardan_fix_enabled = true;
        }

        kraken::routines::ChangeCall((void*) 0x005EC7AD, KeepThrottle);
        kraken::routines::Nop((void*)0x005ECCD3, 2);
        kraken::routines::ChangeCall((void*) 0x005ECCD6, HookVehicleDtor);
        kraken::routines::Redirect(0x26, (void*) 0x005EDD60, VehiclePrototypeInfo_CreateTargetObject);

        // Build the trampoline from the original prologue BEFORE redirecting (otherwise we
        // would copy our own jmp), then point 0x00891430 at the C wrapper.
        void* const origWheelCollide = reinterpret_cast<void*>(0x00891430);
        DWORD oldProt;
        VirtualProtect(s_wheelCollideTramp, sizeof(s_wheelCollideTramp), PAGE_EXECUTE_READWRITE, &oldProt);
        std::memcpy(s_wheelCollideTramp, origWheelCollide, 5);
        s_wheelCollideTramp[5] = 0xE9;
        *reinterpret_cast<int32_t*>(s_wheelCollideTramp + 6) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(origWheelCollide) + 5
            - (reinterpret_cast<uintptr_t>(s_wheelCollideTramp) + 10));
        kraken::routines::Redirect(5, origWheelCollide, reinterpret_cast<void*>(&Hook_CollideWheelDefault));
    }
}