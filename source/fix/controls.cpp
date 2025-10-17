#include "fix/controls.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "hta/m3d/CMiracle3d.h"
#include "hta/m3d/CClient.h"
#include "hta/m3d/IInput.h"
#include "hta/m3d/CServer.h"

namespace kraken::fix::controls {
    int __stdcall MyControlsHandler(
        CMiracle3d* thisPtr, int ebxVal, int ediVal, CMiracle3d* esiVal,
        double t0, double tlen);

    __declspec(naked) void Hook_Controls()
    {
        __asm {
            // Вход: ecx=this, ebx, edi, esi заданы регистрами
            // На стеке (esp): [ret], [tlen], [t0]

            push    ebp
            mov     ebp, esp

            fld     qword ptr [ebp + 0Ch]    // t0
            sub     esp, 8
            fstp    qword ptr [esp]

            fld     qword ptr [ebp + 4]      // tlen
            sub     esp, 8
            fstp    qword ptr [esp]

            push    esi
            push    edi
            push    ebx
            push    ecx
            call    MyControlsHandler

            mov     esp, ebp
            pop     ebp
            ret     8
        }
    }

    int __stdcall MyControlsHandler(
    CMiracle3d* thisPtr,
    int ebxVal,
    int ediVal,
    CMiracle3d* esiVal,
    double t0,
    double tlen)
    {
        ai::Vehicle* vehicle = m3d::CClient::Instance->m_world->GetVehicleControlledByPlayer();
        CMiracle3d* app = CMiracle3d::Instance;

        if (vehicle && !vehicle->bIsMovingAlongExternalPath()) {
            // Weapon controls
            WeaponGroupManager* weaponManager = CMiracle3d::Instance->m_pInterfaceManager->pWeaponGroupManager;
            static constexpr auto KeepFire = 0x005866E0;
            __asm {
                mov ecx, weaponManager  // this
                mov edi, weaponManager  // a2
                call KeepFire
            }

            // Horn control
            bool hornState = m3d::Application::Instance->m_pImpulses->GetImpulseState(28);
            vehicle->SetHorn(hornState);

            // Steering
            m3d::IImpulse* impulses = m3d::Application::Instance->m_pImpulses;
            m3d::input::IInput* input = m3d::Application::Instance->m_input;

            if (impulses->GetImpulseState(26) || input->GetParam(m3d::input::DeviceParam::DP_JOY_X) < -300) {
                vehicle->SetSteer(0.78539819f);
            }
            else if (impulses->GetImpulseState(27) || input->GetParam(m3d::input::DeviceParam::DP_JOY_X) > 300) {
                vehicle->SetSteer(-0.78539819f);
            }
            else {
                vehicle->SetSteer(0.0f);
            }

            // Throttle and braking
            if (impulses->GetImpulseState(23)) {
                vehicle->SetThrottle(1.0f, true);
            }
            else if (impulses->GetImpulseState(24)) {
                vehicle->SetThrottle(-1.0f, true);
            }
            else {
                vehicle->ReleaseAllPedals();
            }

            if (impulses->GetImpulseState(25)) {
                vehicle->SetHandBrake();
            }

            // Special controls
            if (impulses->GetImpulseState(29)) {
                const m3d::CVar& turnToWheelsAllowed = m3d::Kernel::g_Kernel->GetEngineCfg().m_ai_turntowheels_allowed;
                if (turnToWheelsAllowed.m_b) {
                    CVector torque{ 0.0f, 0.0f, 0.0f };
                    CVector force{ 0.0f, 1.0f, 0.0f };
                    CVector pos{ 1.0f, 0.0f, 0.0f };
                    vehicle->SetTurningToGroundForceAndTorque(&pos, &force, &torque);
                }
            }

            if (impulses->GetImpulseState(31)) {
                impulses->ResetImpulseWithoutNotification(31);
                LightActivated = !LightActivated;
                vehicle->ActivateHeadLights(LightActivated);
            }

            if (impulses->GetImpulseState(32)) {
                impulses->ResetImpulseWithoutNotification(32);
                vehicle->GetOutOfDifficultPlace();
            }
        }

        // Game pause toggle
        if (app->m_curGameMode.m_mode == CMiracle3d::GameState::GS_GAME &&
            app->m_pImpulses->GetImpulseStateAndReset(51)) {
            ai::CServer::Instance->fPause = !ai::CServer::Instance->fPause;
        }

        // Fly camera movement
        app->m_flyCamMove = { 0.0f, 0.0f, 0.0f };
        auto* impulses = app->m_pImpulses;

        if (impulses->GetImpulseState(4)) app->m_flyCamMove.z += 1.0f;
        if (impulses->GetImpulseState(5)) app->m_flyCamMove.z -= 1.0f;
        if (impulses->GetImpulseState(7)) app->m_flyCamMove.x += 1.0f;
        if (impulses->GetImpulseState(6)) app->m_flyCamMove.x -= 1.0f;

        // Apply camera speed and time delta
        float cameraSpeed = app->m_cameraSpeed.m_f;
        float scale = cameraSpeed * static_cast<float>(t0);

        app->m_flyCamMove.x *= scale;
        app->m_flyCamMove.y *= scale;
        app->m_flyCamMove.z *= scale;

        // Camera rotation
        if (app->m_player.m_cameraMode != CM_BUMPER) {
            float mouseSensitivity = app->GetMouseSensitivity();

            app->m_curCamera.m_rotPitch += (app->m_gameSlideAuto.z * 0.00015000001f) - app->m_flyCamTurn.y;
            app->m_curCamera.m_rotYaw += (-app->m_flyCamTurn.x) - (app->m_gameSlideAuto.x * 0.00030000001f);
            app->m_curCamera.m_rotRoll = 0.0f;

            // Clamp pitch in follow mode
            if (app->m_player.m_cameraMode == CM_FOLLOWMODE) {
                app->m_curCamera.m_rotPitch = clamp(app->m_curCamera.m_rotPitch, -0.44879895f, 0.44879895f);
            }
        }

        static constexpr auto ValidateCameraAngles = 0x004024A0;
        __asm {
            mov ecx, app    // this
            mov eax, app    // a2
            call ValidateCameraAngles // CMiracle3d::ValidateCameraAngles
        }

        // Update camera position
        if (app->m_curGameMode.m_mode == CMiracle3d::GameState::GS_GAME ||
            (app->m_curGameMode.m_mode == CMiracle3d::GameState::GS_MAINMENU && !app->m_bDoNotLoadMainmenuLevel)) {
            static constexpr auto UpdateCameraPosition = 0x00401B40;
            __asm {
                mov eax, app          // this
                mov ebx, vehicle   // trackedObj
                call UpdateCameraPosition       // CMiracle3d::UpdateCameraPosition
            }
        }

        return 1;
    }

    bool __fastcall processWinMessages(m3d::Application* app, int)
    {
        tagMSG msg; // [esp+Ch] [ebp-1Ch] BYREF

        app->m_mouseInfo.m_deltaDuringGameFrame.x = 0;
        app->m_mouseInfo.m_deltaDuringGameFrame.y = 0;
        static auto wmQuitMsg = (bool *)0x00A0A978;
        if (!PeekMessageA(&msg, 0, 0, 0, 1u))
        {
            return !*wmQuitMsg;
        }
        while (msg.message != 18)
        {
            if (!app->m_isAppActive && GetMessageA(&msg, 0, 0, 0))
            {
                do
                {
                    if (app->m_isAppActive)
                        break;
                    DispatchMessageA(&msg);
                } while (GetMessageA(&msg, 0, 0, 0));
            }
            DispatchMessageA(&msg);
            if (!PeekMessageA(&msg, 0, 0, 0, 1u))
                return !*wmQuitMsg;
        }
        *wmQuitMsg = 1;
        return 0;
    }

    void Apply() {
        kraken::routines::Redirect(0x48A, (void*)0x004016B0, (void*)&Hook_Controls);
    }
}