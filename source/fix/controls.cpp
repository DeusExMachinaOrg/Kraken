#define LOGGER "controls"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <stdint.h>
#include <cstring>
#include <cmath>
#include <atomic>

#include "fix/controls.hpp"
#include "fix/dualsense.hpp"
#include "fix/xinputrumble.hpp"
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

        // Live enable + one-time install state (so a control-profile switch can turn
        // the wheel path on without a restart; the detour, once installed, stays).
        bool  g_enabled    = false;
        bool  g_installed  = false;

        // --- cached wheel state (all updated on the message-pump thread) ---
        bool  g_present    = false;
        float g_steer      = 0.0f; // [-1..1], 0 = centered
        float g_throttle01 = 0.0f; // [0..1]
        float g_brake01    = 0.0f; // [0..1]

        // Latest raw value per axis of the selected controller [-1..1] (before
        // deadzone/mapping), for the profile UI's live position bars. Written on the
        // message-pump thread from OnImpulse; read on the same thread during menu
        // paint. Kept as atomics so a future off-thread reader stays well-defined.
        constexpr int    AXIS_LIVE_COUNT = 6;
        std::atomic<float> g_axisLive[AXIS_LIVE_COUNT] = {};

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
        // SetThrottle's autoBrake flag. false = coast when throttle is released
        // (sim pedals); true = vanilla engine auto-brake on release.
        bool     g_autoBrake    = false;
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
        // Camera auto-return behind the car after the look is idle. Device-agnostic
        // (mouse / wheel / gamepad), gated only by g_camReturn.
        bool     g_camReturn      = false;
        float    g_camReturnDelay = 1.5f; // idle seconds before returning
        float    g_camReturnSpeed = 3.0f; // ease rate toward "behind"
        float    g_camIdle        = 0.0f; // seconds since last look input
        float    g_camFollowOffset= 0.0f; // calibrate which orbit yaw is "behind"
        float    g_camHeading     = 0.0f; // last known car heading (held while stopped)
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
        constexpr uintptr_t CAM_YAW_OFF      = 0x8b278; // persistent camera yaw offset (0 = behind the car)
        constexpr uintptr_t CAM_PITCH_OFF    = 0x8b27c; // persistent camera pitch offset
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
        // Vibration channel: gains for each source + the shared frequency.
        float                  g_ffbDamage    = 1.0f;
        float                  g_ffbCollision = 1.0f;
        float                  g_ffbOffroad   = 1.0f;
        float                  g_ffbEngine    = 0.3f;
        DWORD                  g_ffbVibePeriod = 18000; // microseconds (from ffb_vibe_hz)
        IDirectInput8A*        g_di           = nullptr;
        IDirectInputDevice8A*  g_ffbDev       = nullptr;
        IDirectInputEffect*    g_ffbEffect    = nullptr; // constant force (centering)
        IDirectInputEffect*    g_ffbVibeEffect = nullptr; // periodic sine (vibration)
        GUID                   g_ffbGuid      = {};
        bool                   g_ffbFound     = false;

        // FFB device I/O runs on its own thread so USB round-trips never stall a
        // frame. The game thread only publishes target magnitudes here: the signed
        // centering force and the unsigned vibration amplitude (0..DI_FFNOMINALMAX).
        std::atomic<LONG>      g_ffbTarget    { 0 };
        std::atomic<LONG>      g_ffbVibe      { 0 };
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

            // Vibration channel: a periodic sine on the same X axis. The driver
            // superimposes it on the centering force; we only drive its amplitude
            // (and period) from game events. Non-fatal if the wheel rejects it.
            DIPERIODIC pf = {};
            pf.dwMagnitude = 0;
            pf.lOffset     = 0;
            pf.dwPhase     = 0;
            pf.dwPeriod    = g_ffbVibePeriod;
            LONG vdir[1]   = { 1 };
            DIEFFECT veff  = eff;            // reuse axes/flags/duration from above
            veff.rglDirection          = vdir;
            veff.cbTypeSpecificParams  = sizeof(DIPERIODIC);
            veff.lpvTypeSpecificParams = &pf;
            if (SUCCEEDED(g_ffbDev->CreateEffect(GUID_Sine, &veff, &g_ffbVibeEffect, nullptr))
                && g_ffbVibeEffect) {
                g_ffbVibeEffect->Start(1, 0);
            } else {
                g_ffbVibeEffect = nullptr;
                LOG_WARNING("FFB: CreateEffect(Sine) failed — vibration effects disabled");
            }

            LOG_INFO("Force feedback enabled (strength=%.2f center=%.2f speed_gain=%.2f vibe=%uHz)",
                     g_ffbStrength, g_ffbCenter, g_ffbSpeed,
                     g_ffbVibePeriod ? 1000000u / g_ffbVibePeriod : 0u);
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

        // Push the vibration amplitude (and current period) to the sine effect.
        void PushVibe(LONG mag) {
            if (!g_ffbVibeEffect)
                return;
            DIPERIODIC pf = {};
            pf.dwMagnitude = mag;            // 0..DI_FFNOMINALMAX
            pf.lOffset     = 0;
            pf.dwPhase     = 0;
            pf.dwPeriod    = g_ffbVibePeriod;
            DIEFFECT eff = {};
            eff.dwSize                = sizeof(DIEFFECT);
            eff.cbTypeSpecificParams  = sizeof(DIPERIODIC);
            eff.lpvTypeSpecificParams = &pf;
            if (FAILED(g_ffbVibeEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS))) {
                // Recovery is driven by the constant-force path (PushFFB / the
                // health check below); just retry a start here.
                g_ffbVibeEffect->Start(1, 0);
                g_ffbVibeEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
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
                if (SUCCEEDED(g_ffbDev->Acquire())) {
                    g_ffbEffect->Start(1, 0);
                    if (g_ffbVibeEffect) g_ffbVibeEffect->Start(1, 0);
                }
                return;
            }
            if (state & DIGFFS_ACTUATORSOFF)
                g_ffbDev->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
            if (state & DIGFFS_PAUSED)
                g_ffbDev->SendForceFeedbackCommand(DISFFC_CONTINUE);
            if (state & (DIGFFS_STOPPED | DIGFFS_EMPTY)) {
                g_ffbEffect->Start(1, 0);
                if (g_ffbVibeEffect) g_ffbVibeEffect->Start(1, 0);
            }
        }

        DWORD WINAPI FFBThread(LPVOID) {
            LONG lastMag  = 0x7fffffff;
            LONG lastVibe = 0x7fffffff;
            int  hb       = 0;
            while (g_ffbThreadRun) {
                LONG mag = g_ffbTarget.load(std::memory_order_relaxed);
                if (mag != lastMag) {
                    PushFFB(mag);
                    lastMag = mag;
                }
                LONG vibe = g_ffbVibe.load(std::memory_order_relaxed);
                if (vibe != lastVibe) {
                    PushVibe(vibe);
                    lastVibe = vibe;
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
        // --- vibration-channel state (game thread) ---
        bool         g_ffbHaveVel    = false;
        hta::CVector g_ffbPrevVel;
        bool         g_ffbHaveHealth = false;
        float        g_ffbPrevHealth = 0.0f;
        float        g_ffbImpactEnv  = 0.0f; // collisions/rams (accel spike)
        float        g_ffbOffroadEnv = 0.0f; // rough-ground buzz (smoothed)
        float        g_ffbDamageEnv  = 0.0f; // taking damage (health drop)

        void ReleaseFFB() {
            g_ffbTarget.store(0, std::memory_order_relaxed);
            g_ffbVibe.store(0, std::memory_order_relaxed);
            // Re-baseline next time there's a live vehicle, so respawn/repair (a
            // health jump) and the velocity discontinuity don't fire a fake pulse.
            g_ffbHaveVel    = false;
            g_ffbHaveHealth = false;
            g_ffbImpactEnv  = 0.0f;
            g_ffbOffroadEnv = 0.0f;
            g_ffbDamageEnv  = 0.0f;
        }

        float FfbDt() {
            static LARGE_INTEGER freq = {};
            static LARGE_INTEGER last = {};
            if (freq.QuadPart == 0)
                QueryPerformanceFrequency(&freq);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = last.QuadPart
                ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart)
                : 0.016f;
            last = now;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.1f)   dt = 0.1f;
            return dt;
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

            // ---- vibration channel (periodic sine), built from four sources ----
            float dt = FfbDt();

            if (!g_ffbHaveVel) { g_ffbPrevVel = vel; g_ffbHaveVel = true; }
            hta::CVector dv = vel - g_ffbPrevVel;
            g_ffbPrevVel = vel;
            float accel = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z) / dt;

            // Collision/ram: sharp envelope on large acceleration spikes.
            float impact = (accel - 60.0f) / 240.0f;
            if (impact < 0.0f) impact = 0.0f;
            if (impact > 1.0f) impact = 1.0f;
            g_ffbImpactEnv *= std::exp(-dt / 0.10f); // ~100 ms decay
            if (impact > g_ffbImpactEnv) g_ffbImpactEnv = impact;

            // Rough ground: sustained moderate jitter scaled by speed, smoothed.
            float buzz = (accel - 12.0f) / 50.0f;
            if (buzz < 0.0f) buzz = 0.0f;
            if (buzz > 1.0f) buzz = 1.0f;
            float speedFactor = speed / 20.0f;
            if (speedFactor > 1.0f) speedFactor = 1.0f;
            float rough = buzz * speedFactor;
            float k = dt / 0.05f;
            if (k > 1.0f) k = 1.0f;
            g_ffbOffroadEnv += (rough - g_ffbOffroadEnv) * k;

            // Damage: a punchy pulse when the vehicle's health drops.
            float health = vehicle->GetHealth();
            if (!g_ffbHaveHealth) { g_ffbPrevHealth = health; g_ffbHaveHealth = true; }
            float drop = g_ffbPrevHealth - health;
            g_ffbPrevHealth = health;
            g_ffbDamageEnv *= std::exp(-dt / 0.22f); // ~220 ms decay
            if (drop > 0.0001f) {
                float maxH = vehicle->GetMaxHealth();
                if (maxH > 0.0f) {
                    float dmg = (drop / maxH) / 0.20f; // 20% of max HP -> full pulse
                    if (dmg > 1.0f) dmg = 1.0f;
                    if (dmg > g_ffbDamageEnv) g_ffbDamageEnv = dmg;
                }
            }

            // Engine: gentle always-on rumble while moving (idle + speed).
            float engine = g_ffbEngine > 0.0f ? (0.10f + 0.35f * speedFactor) : 0.0f;

            // Combine: the strongest sustained source plus the transient kicks.
            float vibe = engine * g_ffbEngine;
            float off  = g_ffbOffroadEnv * g_ffbOffroad;
            if (off > vibe) vibe = off;
            vibe += g_ffbImpactEnv * g_ffbCollision; // collisions add on top
            vibe += g_ffbDamageEnv * g_ffbDamage;    // damage adds on top
            vibe *= g_ffbStrength;
            if (vibe < 0.0f) vibe = 0.0f;
            if (vibe > 1.0f) vibe = 1.0f;

            g_ffbVibe.store(static_cast<LONG>(vibe * DI_FFNOMINALMAX),
                            std::memory_order_relaxed);

            if (g_ffbLog && vibe > 0.001f)
                LOG_DEBUG("ffb vibe=%.2f (eng=%.2f off=%.2f imp=%.2f dmg=%.2f) speed=%.1f",
                          vibe, engine * g_ffbEngine, off, g_ffbImpactEnv, g_ffbDamageEnv, speed);
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
                    // Only the profile's selected controller drives the vehicle —
                    // ignore axes from any other connected device.
                    if (ev.joy_axis.device != g_device)
                        break;
                    int   a = static_cast<int>(ev.joy_axis.axis);
                    float v = ev.joy_axis.value; // [-1..1]
                    // Publish the raw value for the profile UI's live bars.
                    if (a >= 0 && a < AXIS_LIVE_COUNT)
                        g_axisLive[a].store(v, std::memory_order_relaxed);
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
            if (!g_enabled)
                return;
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

        // Camera auto-return / chase-follow: when the look has been idle for
        // cam_return_delay, drive the camera orbit yaw to the car's heading so the
        // view sits behind the car and tracks it through turns (comfortable for
        // wheel driving with no mouse). m_camYaw is an absolute world yaw (world is
        // Y-up, horizontal plane XZ), so "behind" = the car's heading, taken from
        // the velocity direction. cam_follow_offset / cam_follow_invert calibrate
        // which way is "behind". Runs before the original Controls so the eased
        // angle is used the same frame. Device-agnostic: any look input (mouse via
        // m_flyCamTurn, or the right stick) resets the idle timer.
        void CameraReturn() {
            if (!g_camReturn)
                return;
            char* app = static_cast<char*>(*reinterpret_cast<void**>(G_PAPP_VA));
            if (!app)
                return;
            int mode = *reinterpret_cast<int*>(app + CAM_MODE_OFF);
            if (mode != 2 && mode != 3) {
                g_camIdle = 0.0f;
                return;
            }

            static LARGE_INTEGER freq = {};
            static LARGE_INTEGER last = {};
            if (freq.QuadPart == 0)
                QueryPerformanceFrequency(&freq);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = last.QuadPart
                ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart)
                : 0.016f;
            last = now;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.1f)   dt = 0.1f;

            // m_flyCamTurn still holds this frame's mouse delta here (we run before
            // ApplyCameraPre overwrites it); g_camX/g_camY hold the right stick.
            float ftx = *reinterpret_cast<float*>(app + FLYCAMTURN_X_OFF);
            float fty = *reinterpret_cast<float*>(app + FLYCAMTURN_Y_OFF);
            bool mouseActive = (ftx != 0.0f || fty != 0.0f);
            bool stickActive = (g_camX >  g_camDeadzone || g_camX < -g_camDeadzone ||
                                g_camY >  g_camDeadzone || g_camY < -g_camDeadzone);
            if (mouseActive || stickActive) {
                g_camIdle = 0.0f;
                return;
            }

            g_camIdle += dt;
            if (g_camIdle < g_camReturnDelay)
                return;

            // Car heading from the horizontal velocity direction (XZ plane, Y-up).
            // Hold the last heading while nearly stopped so the camera doesn't spin.
            hta::ai::Vehicle* vehicle = GetPlayerVehicle();
            if (!vehicle)
                return;
            hta::CVector vel = vehicle->GetLinearVelocity();
            float hspeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
            if (hspeed > 1.0f)
                g_camHeading = -std::atan2(vel.x, vel.z); // sign that matches the orbit yaw

            float target = g_camHeading + g_camFollowOffset;

            float* yaw   = reinterpret_cast<float*>(app + CAM_YAW_OFF);
            float* pitch = reinterpret_cast<float*>(app + CAM_PITCH_OFF);

            // Ease yaw to the target along the shortest angular path.
            float d = target - *yaw;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            float k = g_camReturnSpeed * dt;
            if (k > 1.0f) k = 1.0f;
            *yaw   += d * k;
            *pitch -= *pitch * k; // level the pitch behind the car
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
                dualsense::Idle();
                xinputrumble::Idle();
                return;
            }

            // Drive override is gated so a profile that turns the wheel off (e.g.
            // switching to a keyboard profile) hands steering/throttle back to the
            // original Controls. Rumble/FFB below keep their own enable flags.
            if (g_enabled) {
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

                // Throttle and brake share the engine's single throttle axis:
                // gas pushes forward, brake pulls back (vanilla reverse/brake).
                float thr = g_throttle01 - g_brake01;
                if (thr >  1.0f) thr =  1.0f;
                if (thr < -1.0f) thr = -1.0f;

                // Coexist with the keyboard: the original (digital) Controls has
                // already applied WASD steer/throttle this frame. Only *override*
                // it where the analog control is actually engaged — otherwise a
                // resting stick/idle pedals would stamp 0 over the keyboard every
                // frame and kill WASD. Steer and drive are gated independently so
                // you can, e.g., hold the stick while braking on the keyboard.
                bool steerActive = (s != 0.0f);
                bool driveActive = (g_throttle01 > 0.0f || g_brake01 > 0.0f);
                if (steerActive)
                    vehicle->SetSteer(s * g_steerRange * (*STEER_MAGNITUDE));
                if (driveActive)
                    // autoBrake=false -> releasing coasts (engine auto-brake-on-zero).
                    // Explicit braking still works via a negative throttle (L2/brake).
                    vehicle->SetThrottle(thr, g_autoBrake);

                if (g_log && (steerActive || driveActive || g_camX != 0.0f || g_camY != 0.0f))
                    LOG_DEBUG("steer=%.3f throttle=%.3f (gas=%.2f brake=%.2f) cam=(%.2f,%.2f) [s=%d d=%d]",
                              s, thr, g_throttle01, g_brake01, g_camX, g_camY,
                              (int)steerActive, (int)driveActive);
            }

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

            dualsense::Update(vehicle);    // DualSense HID feedback (no-op unless enabled)
            xinputrumble::Update(vehicle); // XInput pad rumble (no-op unless enabled)
        }

        // Naked detour: preserve Controls' __usercall contract.
        //   incoming: ESI=g_pApp, ECX=this, [esp]=ret, [esp+4]=tlen, [esp+8]=a1
        // We reload ESI from g_pApp (robust against any caller), re-push the two
        // float args so the trampoline sees them at the right offsets, run the
        // original, then apply the analog wheel override. Callee-cleans (ret 8).
        __declspec(naked) void Controls_Detour() {
            __asm {
                call    CameraReturn                 ; idle look -> ease camera behind the car
                                                     ; (reads m_flyCamTurn before ApplyCameraPre)
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

    // Re-read the [wheel] config snapshot into the live globals. Shared by Apply()
    // and Reapply() (control-profile switch). Pure data refresh — no hooks here.
    static void LoadConfig() {
        const Config& config = Config::Instance();
        g_enabled      = config.wheel.value != 0;
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
        g_autoBrake    = config.wheel_auto_brake.value != 0;
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
        g_camReturn      = config.wheel_cam_return.value != 0;
        g_camReturnDelay = config.wheel_cam_return_delay.value;
        g_camReturnSpeed = config.wheel_cam_return_speed.value;
        g_camFollowOffset= config.wheel_cam_follow_offset.value;
        g_log          = config.wheel_log.value != 0;

        g_ffbEnabled   = config.ffb.value != 0;
        g_ffbStrength  = config.ffb_strength.value;
        g_ffbCenter    = config.ffb_center.value;
        g_ffbSpeed     = config.ffb_speed_gain.value;
        g_ffbInvert    = config.ffb_invert.value != 0;
        g_ffbLog       = config.ffb_log.value != 0;
        g_ffbDamage    = config.ffb_damage.value;
        g_ffbCollision = config.ffb_collision.value;
        g_ffbOffroad   = config.ffb_offroad.value;
        g_ffbEngine    = config.ffb_engine.value;
        {
            float hz = config.ffb_vibe_hz.value;
            if (hz < 1.0f) hz = 1.0f;
            g_ffbVibePeriod = static_cast<DWORD>(1000000.0f / hz); // Hz -> microseconds
        }

        // Reset the cached analog inputs to neutral. Axis values only refresh on a
        // *change* event (impulse fires per axis delta), so a controller resting at
        // center sends nothing — and a stale value carried over from the previous
        // device/profile (e.g. a right turn held on the XInput pad before switching
        // to the native DualSense) would keep steering with no live input. Clearing
        // on every LoadConfig (profile switch) guarantees a neutral start.
        g_steer      = 0.0f;
        g_throttle01 = 0.0f;
        g_brake01    = 0.0f;
        g_camX       = 0.0f;
        g_camY       = 0.0f;
    }

    // Install the impulse listener + Controls detour exactly once. ApplyWheel /
    // ApplyCameraPre gate on g_enabled, so once installed the hook can be toggled
    // live by a profile switch without uninstalling.
    static void Install() {
        if (g_installed)
            return;
        impulse::Attach(impulse::eImpulseAny, OnImpulse);
        if (InstallControlsHook()) {
            g_installed = true;
            if (g_triggerAxis >= 0)
                LOG_INFO("Analog wheel control enabled (device=%u steer=%d trigger_axis=%d [combined L2/R2])",
                         g_device, g_steerAxis, g_triggerAxis);
            else
                LOG_INFO("Analog wheel control enabled (device=%u steer=%d throttle=%d brake=%d)",
                         g_device, g_steerAxis, g_throttleAxis, g_brakeAxis);
        }
    }

    void Apply() {
        LoadConfig();
        if (g_enabled)
            Install();
    }

    // Control-profile switch: refresh the snapshot and, if the new profile turns
    // the wheel on for the first time, install the hook now.
    void Reapply() {
        LoadConfig();
        if (g_enabled && !g_installed)
            Install();
    }

    float AxisLive(int axis) {
        if (axis < 0 || axis >= AXIS_LIVE_COUNT)
            return 0.0f;
        return g_axisLive[axis].load(std::memory_order_relaxed);
    }
}
