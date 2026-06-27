#define LOGGER "controls"

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cmath>

#include "fix/controls.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

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
            if (!vehicle)
                return;

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

        impulse::Attach(impulse::eImpulseAny, OnImpulse);

        if (InstallControlsHook())
            LOG_INFO("Analog wheel control enabled (device=%u steer=%d throttle=%d brake=%d)",
                     g_device, g_steerAxis, g_throttleAxis, g_brakeAxis);
    }
}
