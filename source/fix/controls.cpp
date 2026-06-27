#define LOGGER "controls"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <stdint.h>
#include <cstring>
#include <cmath>

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
        bool     g_log          = false;

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

        void SetFFBMagnitude(LONG mag) {
            if (!g_ffbEffect)
                return;
            DICONSTANTFORCE cf = {};
            cf.lMagnitude = mag;
            DIEFFECT eff = {};
            eff.dwSize                = sizeof(DIEFFECT);
            eff.cbTypeSpecificParams  = sizeof(DICONSTANTFORCE);
            eff.lpvTypeSpecificParams = &cf;
            HRESULT hr = g_ffbEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                // Lost the device (e.g. Alt+Tab). Reacquire, restart and retry so
                // the force resumes the same frame focus returns.
                g_ffbDev->Acquire();
                g_ffbEffect->Start(1, 0);
                g_ffbEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
            }
        }

        // Let go of the wheel (no force) — used when there is no live player
        // vehicle, so death/menus don't leave the wheel slammed against a stop.
        void ReleaseFFB() {
            SetFFBMagnitude(0);
        }

        // Self-heal the device every frame. Alt+Tab (and the game's own focus /
        // D3D device-reset handling) can silently unacquire the device, stop the
        // effect, pause it, or switch the actuators off — and the triggering
        // window message isn't always one we see. So instead of relying on a
        // focus event, we poll the FF state each frame and revive whatever is
        // wrong. Cheap, and independent of how focus actually changed.
        void EnsureFFBLive() {
            if (!g_ffbDev || !g_ffbEffect)
                return;

            DWORD   state = 0;
            HRESULT hr    = g_ffbDev->GetForceFeedbackState(&state);
            if (FAILED(hr)) {
                // Any failure after Alt+Tab — INPUTLOST, NOTACQUIRED, or
                // NOTEXCLUSIVEACQUIRED (0x80040205): the device dropped or got
                // re-acquired NON-exclusively, and FFB needs exclusive. A bare
                // Acquire() on an already (non-exclusively) acquired device just
                // returns S_FALSE and does NOT upgrade it, so we must Unacquire
                // first to force a fresh exclusive acquisition.
                g_ffbDev->Unacquire();
                HRESULT ah = g_ffbDev->Acquire();
                if (SUCCEEDED(ah))
                    g_ffbEffect->Start(1, 0);
                if (g_ffbLog)
                    LOG_DEBUG("FFB revive: getstate=0x%08lX acquire=0x%08lX", hr, ah);
                return;
            }

            if (state & DIGFFS_ACTUATORSOFF) {
                if (g_ffbLog) LOG_DEBUG("FFB actuators off -> on (state=0x%lX)", state);
                g_ffbDev->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
            }
            if (state & DIGFFS_PAUSED) {
                if (g_ffbLog) LOG_DEBUG("FFB paused -> continue (state=0x%lX)", state);
                g_ffbDev->SendForceFeedbackCommand(DISFFC_CONTINUE);
            }
            if (state & (DIGFFS_STOPPED | DIGFFS_EMPTY)) {
                if (g_ffbLog) LOG_DEBUG("FFB stopped/empty -> Start (state=0x%lX)", state);
                g_ffbEffect->Start(1, 0); // (re)downloads + plays the effect
            }
        }

        void UpdateFFB(hta::ai::Vehicle* vehicle) {
            if (!g_ffbEffect)
                return;

            EnsureFFBLive();

            hta::CVector vel = vehicle->GetLinearVelocity();
            float   speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);

            // Self-centering: force opposes wheel deflection, scaled by speed.
            float frac = -(g_ffbCenter + g_ffbSpeed * speed) * g_steer;
            if (g_ffbInvert) frac = -frac;
            frac *= g_ffbStrength;
            if (frac >  1.0f) frac =  1.0f;
            if (frac < -1.0f) frac = -1.0f;
            LONG mag = static_cast<LONG>(frac * DI_FFNOMINALMAX);

            SetFFBMagnitude(mag);

            if (g_ffbLog)
                LOG_DEBUG("ffb mag=%ld (speed=%.2f steer=%.3f)", mag, speed, g_steer);
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
                case eImpulseFocus:
                    // Alt+Tab unacquires the exclusive FFB device; reacquire and
                    // re-download the effect when the window regains focus.
                    if (ev.focus.state && g_ffbReady && g_ffbDev) {
                        g_ffbDev->Acquire();
                        if (g_ffbEffect)
                            g_ffbEffect->Start(1, 0);
                    }
                    break;
                case eImpulseJoyConnection:
                    if (ev.joy_connect.device == g_device)
                        g_present = (ev.joy_connect.status == eJoyStatusConnected);
                    break;
                case eImpulseJoyAxis: {
                    int   a = static_cast<int>(ev.joy_axis.axis);
                    float v = ev.joy_axis.value; // [-1..1]
                    if (a == g_steerAxis)
                        g_steer = v;
                    else if (a == g_throttleAxis)
                        g_throttle01 = PedalValue(v, g_invThrottle);
                    else if (a == g_brakeAxis)
                        g_brake01 = PedalValue(v, g_invBrake);
                    break;
                }
                default:
                    break;
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
                LOG_DEBUG("steer=%.3f throttle=%.3f (gas=%.2f brake=%.2f)",
                          s, thr, g_throttle01, g_brake01);

            if (g_ffbEnabled) {
                if (!g_ffbReady && !g_ffbInitTried) {
                    g_ffbInitTried = true;
                    g_ffbReady     = InitFFB();
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
        g_log          = config.wheel_log.value != 0;

        g_ffbEnabled   = config.ffb.value != 0;
        g_ffbStrength  = config.ffb_strength.value;
        g_ffbCenter    = config.ffb_center.value;
        g_ffbSpeed     = config.ffb_speed_gain.value;
        g_ffbInvert    = config.ffb_invert.value != 0;
        g_ffbLog       = config.ffb_log.value != 0;

        impulse::Attach(impulse::eImpulseAny, OnImpulse);

        if (InstallControlsHook())
            LOG_INFO("Analog wheel control enabled (device=%u steer=%d throttle=%d brake=%d)",
                     g_device, g_steerAxis, g_throttleAxis, g_brakeAxis);
    }
}
