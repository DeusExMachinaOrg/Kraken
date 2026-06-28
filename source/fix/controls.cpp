#define LOGGER "controls"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <stdint.h>
#include <cstring>
#include <cmath>
#include <atomic>

#include "fix/controls.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

#include "hta/CVector.hpp"
#include "hta/ai/Vehicle.hpp"

// Analog steering-wheel control for the player vehicle (MOZA R5 etc.).
//
// Input arrives through the impulse bus (see ext/impulse.cpp, which polls the
// winmm joystick): eImpulseJoyAxis carries each axis in [-1..1], eImpulseJoyConnection
// the connect/disconnect. We cache the latest steer/throttle/brake here.
//
// Vanilla driving (CMiracle3d::Controls, game.cpp:278-467, VA 0x004016B0) is
// purely digital: keyboard actions map to SetSteer(+/-k), SetThrottle(+/-1),
// SetHandBrake(). We detour Controls, let the original run (so weapons, horn,
// gear, camera and the keyboard fallback keep working), then overwrite the
// player vehicle's steer/throttle with our analog wheel values for that frame —
// landing before Vehicle::Update consumes them in the physics step.
//
// All addresses are VAs for hta.exe (image base 0x400000).
namespace kraken::fix::controls {
    namespace {
        // ai::Vehicle* help::GetPlayerVehicle()  (VA 0x005513B0, __fastcall, no args)
        using GetPlayerVehicle_t = hta::ai::Vehicle* (__fastcall*)();
        const GetPlayerVehicle_t GetPlayerVehicle =
            reinterpret_cast<GetPlayerVehicle_t>(0x005513B0);

        // The engine's full-lock steer magnitude (vanilla passes +/-*this* to SetSteer).
        const float* const STEER_MAGNITUDE = reinterpret_cast<const float*>(0x009931F4);

        // CMiracle3d::Controls (VA 0x004016B0). Despite the PDB labelling it
        // __thiscall, it is really a __usercall: it expects ESI = g_pApp on entry
        // (it never loads ESI itself, yet dereferences [esi+...] and even does a
        // virtual call `call [esi]->+0x10` at game.cpp:382). FrameMove supplies
        // ESI. Our detour must therefore restore ESI before running the original.
        constexpr uintptr_t CONTROLS_VA = 0x004016B0;
        // m3d::Application* m3d::Application::g_pApp
        constexpr uintptr_t G_PAPP_VA   = 0x00A0A55C;
        void* g_trampoline = nullptr; // relocated prologue + jmp back to Controls+5

        // --- cached wheel state (all updated on the message-pump thread) ---
        bool  g_present    = false;
        float g_steer      = 0.0f; // [-1..1], 0 = centered
        float g_throttle01 = 0.0f; // [0..1]
        float g_brake01    = 0.0f; // [0..1]

        // --- config snapshot ---
        uint32_t g_device       = 0;
        int      g_steerAxis    = 0;
        int      g_throttleAxis = 2;
        int      g_brakeAxis    = 3;
        float    g_deadzone     = 0.04f;
        float    g_pedalDead    = 0.10f;
        float    g_steerRange   = 1.0f;
        bool     g_invSteer     = false;
        bool     g_invThrottle  = false;
        bool     g_invBrake     = false;
        // Combined L2/R2 trigger axis (DualSense): one axis carries both pedals,
        // resting at center. -1 = off (separate throttle/brake pedals, MOZA-style).
        int      g_triggerAxis  = -1;
        float    g_triggerDead  = 0.06f;
        bool     g_invTrigger   = false;
        // Steering response curve: s = sign(s)*|s|^expo (1 = linear).
        float    g_steerExpo    = 1.0f;
        // Right-stick camera look.
        int      g_camYawAxis   = -1;
        int      g_camPitchAxis = -1;
        float    g_camDeadzone  = 0.15f;
        float    g_camYawSpeed  = 2.5f;
        float    g_camPitchSpeed= 1.8f;
        bool     g_invCamYaw    = false;
        bool     g_invCamPitch  = false;
        float    g_camX         = 0.0f; // right-stick X [-1..1]
        float    g_camY         = 0.0f; // right-stick Y [-1..1]
        bool     g_log          = false;

        // Camera fields inside CMiracle3d (== g_pApp). Each frame the original
        // Controls does: m_camYaw -= m_flyCamTurn.x; m_camPitch -= m_flyCamTurn.y
        // (plus auto-follow slide), then ValidateCameraAngles clamps and
        // UpdateCameraPosition rebuilds. OnGameMouse feeds m_flyCamTurn from the
        // mouse. We feed the same fields from the right stick *before* the original
        // Controls runs, so it consumes our delta the same frame, on the same code
        // path the mouse uses — staying in sync with the world (no fast-drive jitter).
        constexpr uintptr_t FLYCAMTURN_X_OFF = 0x8b2dc; // m_flyCamTurn.x -> yaw delta
        constexpr uintptr_t FLYCAMTURN_Y_OFF = 0x8b2e0; // m_flyCamTurn.y -> pitch delta
        constexpr uintptr_t CAM_MODE_OFF     = 0x8b290; // m_player.m_cameraMode (look enabled in 2/3)
        // Per-frame look delta = stick * (speed_rad_per_s * NOMINAL_DT). Using a
        // fixed nominal step instead of a measured dt keeps the camera advancing on
        // the engine's own frame cadence (like the mouse's per-event delta), which
        // is what avoids the jitter; turn rate then scales gently with frame rate.
        constexpr float     CAM_NOMINAL_DT    = 1.0f / 60.0f;

        // --- force feedback (DirectInput8) ---
        // winmm can only read the wheel; FFB output requires DirectInput. We load
        // the real dinput8 from System32 (bypassing the game's dinput8 proxy),
        // grab the first attached force-feedback game controller, and drive a
        // single constant-force effect on the X axis that we re-aim every frame
        // to self-center the wheel (stronger with speed).
        bool                   g_ffbEnabled   = false;
        bool                   g_ffbInitTried = false;
        bool                   g_ffbReady     = false;
        float                  g_ffbStrength  = 1.0f;
        float                  g_ffbCenter    = 0.12f;
        float                  g_ffbSpeed     = 0.03f;
        bool                   g_ffbInvert    = false;
        bool                   g_ffbLog       = false;
        IDirectInput8A*        g_di           = nullptr;
        IDirectInputDevice8A*  g_ffbDev       = nullptr;
        IDirectInputEffect*    g_ffbEffect    = nullptr;
        GUID                   g_ffbGuid      = {};
        bool                   g_ffbFound     = false;

        // FFB device I/O runs on its own thread so USB round-trips never stall a
        // frame. The game thread only publishes a target magnitude here.
        std::atomic<LONG>      g_ffbTarget    { 0 };
        volatile bool          g_ffbThreadRun = false;
        HANDLE                 g_ffbThread    = nullptr;

        HWND GameWindow() {
            void* app = *reinterpret_cast<void**>(G_PAPP_VA);
            if (!app)
                return nullptr;
            // m3d::Application::m_renderWindow (same offset borderless.cpp uses)
            HWND wnd = *reinterpret_cast<HWND*>(static_cast<char*>(app) + 0x8B258);
            return (wnd && IsWindow(wnd)) ? wnd : nullptr;
        }

        BOOL CALLBACK EnumFFBDeviceCb(const DIDEVICEINSTANCEA* inst, void*) {
            g_ffbGuid  = inst->guidInstance;
            g_ffbFound = true;
            LOG_INFO("FFB device found: '%s'", inst->tszProductName);
            return DIENUM_STOP; // take the first force-feedback controller
        }

        bool InitFFB() {
            HWND wnd = GameWindow();
            if (!wnd) {
                g_ffbInitTried = false; // window not ready yet — retry next frame
                return false;
            }

            wchar_t path[MAX_PATH];
            UINT n = GetSystemDirectoryW(path, MAX_PATH);
            wcscpy_s(path + n, MAX_PATH - n, L"\\dinput8.dll");
            HMODULE dll = LoadLibraryW(path);
            if (!dll) {
                LOG_ERROR("FFB: cannot load %ls", path);
                return false;
            }

            using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
            auto DI8Create = reinterpret_cast<DI8Create_t>(GetProcAddress(dll, "DirectInput8Create"));
            if (!DI8Create) {
                LOG_ERROR("FFB: DirectInput8Create not found");
                return false;
            }

            HINSTANCE hinst = GetModuleHandleW(nullptr);
            if (FAILED(DI8Create(hinst, DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                 reinterpret_cast<void**>(&g_di), nullptr))) {
                LOG_ERROR("FFB: DirectInput8Create failed");
                return false;
            }

            g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumFFBDeviceCb, nullptr,
                              DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
            if (!g_ffbFound) {
                LOG_WARNING("FFB: no force-feedback controller attached");
                return false;
            }

            if (FAILED(g_di->CreateDevice(g_ffbGuid, &g_ffbDev, nullptr))) {
                LOG_ERROR("FFB: CreateDevice failed");
                return false;
            }
            g_ffbDev->SetDataFormat(&c_dfDIJoystick);
            g_ffbDev->SetCooperativeLevel(wnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);

            // Disable the device's own auto-center; we provide centering ourselves.
            DIPROPDWORD ac = {};
            ac.diph.dwSize       = sizeof(DIPROPDWORD);
            ac.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            ac.diph.dwObj        = 0;
            ac.diph.dwHow        = DIPH_DEVICE;
            ac.dwData            = DIPROPAUTOCENTER_OFF;
            g_ffbDev->SetProperty(DIPROP_AUTOCENTER, &ac.diph);

            g_ffbDev->Acquire();

            DICONSTANTFORCE cf = {};
            cf.lMagnitude = 0;
            DWORD axes[1]  = { DIJOFS_X };
            LONG  dir[1]   = { 0 };
            DIEFFECT eff   = {};
            eff.dwSize                = sizeof(DIEFFECT);
            eff.dwFlags               = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
            eff.dwDuration            = INFINITE;
            eff.dwSamplePeriod        = 0;
            eff.dwGain                = DI_FFNOMINALMAX;
            eff.dwTriggerButton       = DIEB_NOTRIGGER;
            eff.dwTriggerRepeatInterval = 0;
            eff.cAxes                 = 1;
            eff.rgdwAxes              = axes;
            eff.rglDirection          = dir;
            eff.lpEnvelope            = nullptr;
            eff.cbTypeSpecificParams  = sizeof(DICONSTANTFORCE);
            eff.lpvTypeSpecificParams = &cf;
            eff.dwStartDelay          = 0;

            if (FAILED(g_ffbDev->CreateEffect(GUID_ConstantForce, &eff, &g_ffbEffect, nullptr))
                || !g_ffbEffect) {
                LOG_ERROR("FFB: CreateEffect(ConstantForce) failed");
                return false;
            }
            g_ffbEffect->Start(1, 0);

            LOG_INFO("Force feedback enabled (strength=%.2f center=%.2f speed_gain=%.2f)",
                     g_ffbStrength, g_ffbCenter, g_ffbSpeed);
            return true;
        }

        // ---- worker-thread side: all DirectInput device I/O lives here ----------

        // Push a force value to the wheel; recover exclusive access on failure
        // (Alt+Tab → INPUTLOST / NOTACQUIRED / NOTEXCLUSIVEACQUIRED). A bare
        // Acquire() on an already (non-exclusively) acquired device returns S_FALSE
        // without upgrading, so Unacquire first.
        void PushFFB(LONG mag) {
            DICONSTANTFORCE cf = {};
            cf.lMagnitude = mag;
            DIEFFECT eff = {};
            eff.dwSize                = sizeof(DIEFFECT);
            eff.cbTypeSpecificParams  = sizeof(DICONSTANTFORCE);
            eff.lpvTypeSpecificParams = &cf;

            HRESULT hr = g_ffbEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
            if (FAILED(hr)) {
                g_ffbDev->Unacquire();
                if (SUCCEEDED(g_ffbDev->Acquire())) {
                    g_ffbEffect->Start(1, 0);
                    g_ffbEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
                }
            }
        }

        // Periodic health check: a power-cycled wheel can come back acquired but
        // with actuators OFF or the effect STOPPED (SetParameters then succeeds yet
        // no force is felt). Switch actuators back on / restart as needed.
        void EnsureFFBLive() {
            DWORD   state = 0;
            HRESULT hr    = g_ffbDev->GetForceFeedbackState(&state);
            if (FAILED(hr)) {
                g_ffbDev->Unacquire();
                if (SUCCEEDED(g_ffbDev->Acquire()))
                    g_ffbEffect->Start(1, 0);
                return;
            }
            if (state & DIGFFS_ACTUATORSOFF)
                g_ffbDev->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
            if (state & DIGFFS_PAUSED)
                g_ffbDev->SendForceFeedbackCommand(DISFFC_CONTINUE);
            if (state & (DIGFFS_STOPPED | DIGFFS_EMPTY))
                g_ffbEffect->Start(1, 0);
        }

        DWORD WINAPI FFBThread(LPVOID) {
            LONG lastMag = 0x7fffffff;
            int  hb      = 0;
            while (g_ffbThreadRun) {
                LONG mag = g_ffbTarget.load(std::memory_order_relaxed);
                if (mag != lastMag) {
                    PushFFB(mag);
                    lastMag = mag;
                }
                if (++hb >= 60) { // health check ~ twice a second
                    hb = 0;
                    EnsureFFBLive();
                }
                Sleep(8); // ~120 Hz
            }
            return 0;
        }

        void StartFFBThread() {
            if (g_ffbThread)
                return;
            g_ffbThreadRun = true;
            g_ffbThread = CreateThread(nullptr, 0, FFBThread, nullptr, 0, nullptr);
        }

        // ---- game-thread side: just publish the desired force (no device I/O) ----

        // Let go of the wheel (no force) — used when there is no live player
        // vehicle, so death/menus don't leave the wheel slammed against a stop.
        void ReleaseFFB() {
            g_ffbTarget.store(0, std::memory_order_relaxed);
        }

        void UpdateFFB(hta::ai::Vehicle* vehicle) {
            hta::CVector vel = vehicle->GetLinearVelocity();
            float   speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);

            // Self-centering: force opposes wheel deflection, scaled by speed.
            float frac = -(g_ffbCenter + g_ffbSpeed * speed) * g_steer;
            if (g_ffbInvert) frac = -frac;
            frac *= g_ffbStrength;
            if (frac >  1.0f) frac =  1.0f;
            if (frac < -1.0f) frac = -1.0f;

            g_ffbTarget.store(static_cast<LONG>(frac * DI_FFNOMINALMAX),
                              std::memory_order_relaxed);
        }

        float PedalValue(float axis, bool invert) {
            float p = (axis + 1.0f) * 0.5f; // [-1..1] -> [0..1]
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
            if (invert) p = 1.0f - p;
            // Bottom deadzone: pedals rarely rest at exactly 0 (idle creep).
            if (p <= g_pedalDead)
                return 0.0f;
            return (p - g_pedalDead) / (1.0f - g_pedalDead);
        }

        void OnImpulse(const impulse::Impulse& ev) {
            using namespace kraken::impulse;
            switch (ev.type) {
                // FFB device recovery after Alt+Tab is handled by the worker
                // thread's heartbeat (EnsureFFBLive), so no focus handling here.
                case eImpulseJoyConnection:
                    if (ev.joy_connect.device == g_device)
                        g_present = (ev.joy_connect.status == eJoyStatusConnected);
                    break;
                case eImpulseJoyAxis: {
                    int   a = static_cast<int>(ev.joy_axis.axis);
                    float v = ev.joy_axis.value; // [-1..1]
                    // Per-axis diagnostic: with log=1 this prints which axis moves
                    // when you press R2/L2, so trigger_axis can be set correctly.
                    if (g_log)
                        LOG_DEBUG("axis %d = %.3f", a, v);
                    if (a == g_steerAxis)
                        g_steer = v;
                    if (g_triggerAxis >= 0) {
                        // Combined trigger axis: one axis, resting at center, with
                        // R2 deflecting it one way (throttle) and L2 the other
                        // (brake). DualSense exposes its two triggers this way.
                        if (a == g_triggerAxis) {
                            float t = g_invTrigger ? -v : v;
                            if (t > g_triggerDead) {
                                g_throttle01 = (t - g_triggerDead) / (1.0f - g_triggerDead);
                                g_brake01    = 0.0f;
                            } else if (t < -g_triggerDead) {
                                g_brake01    = (-t - g_triggerDead) / (1.0f - g_triggerDead);
                                g_throttle01 = 0.0f;
                            } else {
                                g_throttle01 = 0.0f;
                                g_brake01    = 0.0f;
                            }
                            if (g_throttle01 > 1.0f) g_throttle01 = 1.0f;
                            if (g_brake01    > 1.0f) g_brake01    = 1.0f;
                        }
                    } else if (a == g_throttleAxis) {
                        g_throttle01 = PedalValue(v, g_invThrottle);
                    } else if (a == g_brakeAxis) {
                        g_brake01 = PedalValue(v, g_invBrake);
                    }
                    // Right-stick camera (independent of the throttle/brake mapping).
                    if (a == g_camYawAxis)
                        g_camX = v;
                    if (a == g_camPitchAxis)
                        g_camY = v;
                    break;
                }
                default:
                    break;
            }
        }

        // Apply a deadzone to a single stick axis and rescale the remainder to
        // keep the full [0..1] range past the deadzone.
        float CamAxis(float v) {
            if (v > -g_camDeadzone && v < g_camDeadzone)
                return 0.0f;
            float sign = (v < 0.0f) ? -1.0f : 1.0f;
            v = (v - sign * g_camDeadzone) / (1.0f - g_camDeadzone);
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            return v;
        }

        // Right-stick camera. Runs *before* the original Controls so it consumes
        // our delta this frame, exactly like the mouse. We write m_flyCamTurn (the
        // mouse-look input field), not the persistent angles, so the engine does the
        // accumulate/clamp/rebuild on its normal path. Only orbit/look camera modes
        // (2,3) take a look input, matching OnGameMouse. When the stick is centered
        // we leave m_flyCamTurn alone so the mouse keeps working; on release we clear
        // it once so any residual delta doesn't keep the camera drifting.
        void ApplyCameraPre() {
            if (g_camYawAxis < 0 && g_camPitchAxis < 0)
                return;
            char* app = static_cast<char*>(*reinterpret_cast<void**>(G_PAPP_VA));
            if (!app)
                return;

            static bool wasActive = false;
            int mode = *reinterpret_cast<int*>(app + CAM_MODE_OFF);
            if (mode != 2 && mode != 3) {
                wasActive = false;
                return;
            }

            float rx = CamAxis(g_camX);
            float ry = CamAxis(g_camY);
            if (g_invCamYaw)   rx = -rx;
            if (g_invCamPitch) ry = -ry;

            float* ftx = reinterpret_cast<float*>(app + FLYCAMTURN_X_OFF);
            float* fty = reinterpret_cast<float*>(app + FLYCAMTURN_Y_OFF);

            if (rx != 0.0f || ry != 0.0f) {
                *ftx = rx * g_camYawSpeed   * CAM_NOMINAL_DT;
                *fty = ry * g_camPitchSpeed * CAM_NOMINAL_DT;
                wasActive = true;
            } else if (wasActive) {
                *ftx = 0.0f;
                *fty = 0.0f;
                wasActive = false;
            }
        }

        void ApplyWheel() {
            if (!g_present)
                return;
            hta::ai::Vehicle* vehicle = GetPlayerVehicle();

            // No live player vehicle (menus, or the player just died): let go of
            // the wheel and don't push any steer/throttle. Otherwise the dead
            // car's lingering speed keeps the centering force pinned to a stop.
            if (!vehicle || vehicle->IsHealthZero()) {
                if (g_ffbReady)
                    ReleaseFFB();
                return;
            }

            // Steering: deadzone around center, then rescale to keep full range.
            float s = g_steer;
            if (s > -g_deadzone && s < g_deadzone) {
                s = 0.0f;
            } else {
                float sign = (s < 0.0f) ? -1.0f : 1.0f;
                s = (s - sign * g_deadzone) / (1.0f - g_deadzone);
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                // Response curve: ease the center so a short-throw spring stick
                // gives proportional control instead of feeling all-or-nothing.
                if (g_steerExpo != 1.0f)
                    s = sign * std::pow(s < 0.0f ? -s : s, g_steerExpo);
            }
            if (g_invSteer)
                s = -s;
            vehicle->SetSteer(s * g_steerRange * (*STEER_MAGNITUDE));

            // Throttle and brake share the engine's single throttle axis:
            // gas pushes forward, brake pulls back (vanilla reverse/brake).
            float thr = g_throttle01 - g_brake01;
            if (thr >  1.0f) thr =  1.0f;
            if (thr < -1.0f) thr = -1.0f;
            vehicle->SetThrottle(thr, true);

            if (g_log)
                LOG_DEBUG("steer=%.3f throttle=%.3f (gas=%.2f brake=%.2f) cam=(%.2f,%.2f)",
                          s, thr, g_throttle01, g_brake01, g_camX, g_camY);

            if (g_ffbEnabled) {
                if (!g_ffbReady && !g_ffbInitTried) {
                    g_ffbInitTried = true;
                    g_ffbReady     = InitFFB();
                    if (g_ffbReady)
                        StartFFBThread();
                }
                if (g_ffbReady)
                    UpdateFFB(vehicle);
            }
        }

        // Naked detour: preserve Controls' __usercall contract.
        //   incoming: ESI=g_pApp, ECX=this, [esp]=ret, [esp+4]=tlen, [esp+8]=a1
        // We reload ESI from g_pApp (robust against any caller), re-push the two
        // float args so the trampoline sees them at the right offsets, run the
        // original, then apply the analog wheel override. Callee-cleans (ret 8).
        __declspec(naked) void Controls_Detour() {
            __asm {
                call    ApplyCameraPre               ; feed m_flyCamTurn from the right stick
                                                     ; BEFORE the original consumes it (cdecl,
                                                     ; preserves esi/edi/ebx)
                mov     eax, 0x00A0A55C              ; &g_pApp (MSVC inline asm: a bracketed
                mov     esi, dword ptr [eax]         ; literal is an immediate, so deref via reg)
                push    dword ptr [esp + 8]          ; a1
                push    dword ptr [esp + 8]          ; tlen
                mov     eax, dword ptr [g_trampoline]
                call    eax                          ; original Controls -> int in eax, ret 8
                push    eax                          ; preserve return value
                call    ApplyWheel
                pop     eax
                ret     8
            }
        }

        // 5-byte trampoline hook. Controls' first instruction is
        // `mov eax, [0xA1185C]` (A1 + abs32) = exactly 5 bytes and position
        // independent, so it relocates cleanly into the trampoline.
        bool InstallControlsHook() {
            uint8_t* fn = reinterpret_cast<uint8_t*>(CONTROLS_VA);

            uint8_t* tramp = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!tramp) {
                LOG_ERROR("Failed to allocate trampoline");
                return false;
            }

            std::memcpy(tramp, fn, 5);                  // relocated `mov eax, [abs32]`
            tramp[5] = 0xE9;                            // jmp back to Controls+5
            *reinterpret_cast<int32_t*>(tramp + 6) =
                static_cast<int32_t>((fn + 5) - (tramp + 10));
            g_trampoline = tramp;

            DWORD prot;
            VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &prot);
            fn[0] = 0xE9;                               // jmp Controls_Detour
            *reinterpret_cast<int32_t*>(fn + 1) =
                static_cast<int32_t>(reinterpret_cast<uint8_t*>(&Controls_Detour) - (fn + 5));
            VirtualProtect(fn, 5, prot, &prot);
            return true;
        }
    }

    void Apply() {
        const Config& config = Config::Instance();
        if (config.wheel.value == 0)
            return;

        g_device       = config.wheel_device.value;
        g_steerAxis    = static_cast<int>(config.wheel_steer_axis.value);
        g_throttleAxis = static_cast<int>(config.wheel_throttle_axis.value);
        g_brakeAxis    = static_cast<int>(config.wheel_brake_axis.value);
        g_deadzone     = config.wheel_deadzone.value;
        g_pedalDead    = config.wheel_pedal_deadzone.value;
        g_steerRange   = config.wheel_steer_range.value;
        g_invSteer     = config.wheel_invert_steer.value != 0;
        g_invThrottle  = config.wheel_invert_throttle.value != 0;
        g_invBrake     = config.wheel_invert_brake.value != 0;
        g_triggerAxis  = config.wheel_trigger_axis.value;
        g_triggerDead  = config.wheel_trigger_deadzone.value;
        g_invTrigger   = config.wheel_invert_trigger.value != 0;
        g_steerExpo    = config.wheel_steer_expo.value;
        g_camYawAxis   = config.wheel_cam_yaw_axis.value;
        g_camPitchAxis = config.wheel_cam_pitch_axis.value;
        g_camDeadzone  = config.wheel_cam_deadzone.value;
        g_camYawSpeed  = config.wheel_cam_yaw_speed.value;
        g_camPitchSpeed= config.wheel_cam_pitch_speed.value;
        g_invCamYaw    = config.wheel_cam_invert_yaw.value != 0;
        g_invCamPitch  = config.wheel_cam_invert_pitch.value != 0;
        g_log          = config.wheel_log.value != 0;

        g_ffbEnabled   = config.ffb.value != 0;
        g_ffbStrength  = config.ffb_strength.value;
        g_ffbCenter    = config.ffb_center.value;
        g_ffbSpeed     = config.ffb_speed_gain.value;
        g_ffbInvert    = config.ffb_invert.value != 0;
        g_ffbLog       = config.ffb_log.value != 0;

        impulse::Attach(impulse::eImpulseAny, OnImpulse);

        if (InstallControlsHook()) {
            if (g_triggerAxis >= 0)
                LOG_INFO("Analog wheel control enabled (device=%u steer=%d trigger_axis=%d [combined L2/R2])",
                         g_device, g_steerAxis, g_triggerAxis);
            else
                LOG_INFO("Analog wheel control enabled (device=%u steer=%d throttle=%d brake=%d)",
                         g_device, g_steerAxis, g_throttleAxis, g_brakeAxis);
        }
    }
}
