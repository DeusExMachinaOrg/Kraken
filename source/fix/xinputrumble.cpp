#define LOGGER "xinput"

#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <cmath>
#include <atomic>

#include "fix/xinputrumble.hpp"
#include "config.hpp"
#include "ext/logger.hpp"

#include "hta/CVector.hpp"
#include "hta/ai/Vehicle.hpp"

// XInput rumble for the player vehicle.
//
// When the DualSense (or any pad) runs in XInput mode — e.g. DSX / Steam Input
// bridging it to a virtual Xbox 360 controller — its native HID is held
// exclusively, so the direct-HID rumble path (fix/dualsense.cpp) can't open it.
// XInput exposes the two motors instead (left = low-frequency, right = high-
// frequency), so we drive those. The force model mirrors the DualSense one: a
// sharp envelope on acceleration spikes (impacts) on the strong motor, a speed-
// scaled buzz on the weak motor (rough ground), and a punchy pulse on both when
// the vehicle takes damage.
//
// XInput is loaded dynamically (no import dependency); the actuator I/O runs on a
// worker thread so a slow USB round-trip never stalls a frame.
namespace kraken::fix::xinputrumble {
    namespace {
        // --- config snapshot ---
        bool  g_enabled     = false;
        float g_strength    = 1.0f;
        float g_impactGain  = 1.0f;
        float g_offroadGain = 1.0f;
        float g_damageGain  = 1.0f;
        float g_damageFull  = 0.20f;
        int   g_cfgIndex    = -1;   // [xinput] index: -1 = auto-detect, 0..3 explicit
        bool  g_log         = false;

        // --- XInput entry points (resolved at runtime) ---
        using XInputSetState_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
        using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        XInputSetState_t g_setState = nullptr;
        XInputGetState_t g_getState = nullptr;
        bool             g_loaded   = false;
        bool             g_loadTried = false;

        // --- published actuator targets (game thread -> worker thread), 0..65535 ---
        std::atomic<int> g_motorStrong { 0 }; // left  (low frequency)
        std::atomic<int> g_motorWeak   { 0 }; // right (high frequency)
        volatile bool    g_workerRun   = false;
        HANDLE           g_worker      = nullptr;
        std::atomic<int> g_padIndex    { -1 }; // currently driven XInput slot, or -1

        bool LoadXInput() {
            if (g_loadTried)
                return g_loaded;
            g_loadTried = true;
            const wchar_t* names[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
            for (const wchar_t* name : names) {
                HMODULE dll = LoadLibraryW(name);
                if (!dll)
                    continue;
                g_setState = reinterpret_cast<XInputSetState_t>(GetProcAddress(dll, "XInputSetState"));
                g_getState = reinterpret_cast<XInputGetState_t>(GetProcAddress(dll, "XInputGetState"));
                if (g_setState && g_getState) {
                    g_loaded = true;
                    LOG_INFO("XInput loaded (%ls)", name);
                    return true;
                }
            }
            LOG_WARNING("XInput: no xinput dll found — rumble disabled");
            return false;
        }

        // Find a connected XInput pad: the configured slot if valid, else the first
        // connected one. Returns -1 if none. Cheap enough to retry when we have none.
        int FindPad() {
            if (!g_getState)
                return -1;
            XINPUT_STATE st;
            if (g_cfgIndex >= 0 && g_cfgIndex <= 3) {
                if (g_getState(static_cast<DWORD>(g_cfgIndex), &st) == ERROR_SUCCESS)
                    return g_cfgIndex;
                return -1;
            }
            for (DWORD i = 0; i < 4; ++i) {
                if (g_getState(i, &st) == ERROR_SUCCESS)
                    return static_cast<int>(i);
            }
            return -1;
        }

        bool AnyPadConnected() {
            if (!LoadXInput())
                return false;
            XINPUT_STATE st;
            for (DWORD i = 0; i < 4; ++i)
                if (g_getState(i, &st) == ERROR_SUCCESS)
                    return true;
            return false;
        }

        DWORD WINAPI Worker(LPVOID) {
            int  lastS = -1, lastW = -1;
            int  heartbeat = 0;
            int  rescan    = 0;
            while (g_workerRun) {
                int idx = g_padIndex.load(std::memory_order_relaxed);
                // Re-find the pad about twice a second when we don't have one (it may
                // connect after launch, or the slot can change).
                if (idx < 0 && ++rescan >= 60) {
                    rescan = 0;
                    idx = FindPad();
                    g_padIndex.store(idx, std::memory_order_relaxed);
                }

                int s = g_motorStrong.load(std::memory_order_relaxed);
                int w = g_motorWeak.load(std::memory_order_relaxed);
                if (idx >= 0 && (s != lastS || w != lastW || ++heartbeat >= 20)) {
                    XINPUT_VIBRATION v;
                    v.wLeftMotorSpeed  = static_cast<WORD>(s);
                    v.wRightMotorSpeed = static_cast<WORD>(w);
                    if (g_setState(static_cast<DWORD>(idx), &v) != ERROR_SUCCESS) {
                        // Pad went away — drop it so the rescan picks a new one.
                        g_padIndex.store(-1, std::memory_order_relaxed);
                    }
                    lastS = s;
                    lastW = w;
                    heartbeat = 0;
                }
                Sleep(8); // ~120 Hz
            }
            if (int idx = g_padIndex.load(std::memory_order_relaxed); idx >= 0 && g_setState) {
                XINPUT_VIBRATION off = {};
                g_setState(static_cast<DWORD>(idx), &off); // release on shutdown
            }
            return 0;
        }

        bool EnsureReady() {
            if (!g_loaded) {
                if (!LoadXInput())
                    return false;
            }
            if (!g_worker) {
                g_padIndex.store(FindPad(), std::memory_order_relaxed);
                g_workerRun = true;
                g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
            }
            return g_worker != nullptr;
        }

        // ---- feedback model (game thread) -------------------------------------
        float        g_impactEnv  = 0.0f;
        float        g_weakSmooth = 0.0f;
        float        g_damageEnv  = 0.0f;
        bool         g_haveVel    = false;
        hta::CVector g_prevVel;
        bool         g_haveHealth = false;
        float        g_prevHealth = 0.0f;

        float QpcDt() {
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
    }

    void Update(hta::ai::Vehicle* vehicle) {
        if (!g_enabled || !vehicle)
            return;
        if (!EnsureReady())
            return;

        float dt = QpcDt();

        hta::CVector vel = vehicle->GetLinearVelocity();
        if (!g_haveVel) { g_prevVel = vel; g_haveVel = true; }
        hta::CVector dv = vel - g_prevVel;
        g_prevVel = vel;

        float accel = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z) / dt;
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);

        // Strong motor: sharp envelope on large acceleration spikes (impacts).
        float impact = (accel - 60.0f) / 240.0f;
        if (impact < 0.0f) impact = 0.0f;
        if (impact > 1.0f) impact = 1.0f;
        g_impactEnv *= std::exp(-dt / 0.12f); // ~120 ms decay
        if (impact > g_impactEnv) g_impactEnv = impact;

        // Weak motor: sustained moderate jitter scaled by speed (rough ground).
        float buzz = (accel - 12.0f) / 50.0f;
        if (buzz < 0.0f) buzz = 0.0f;
        if (buzz > 1.0f) buzz = 1.0f;
        float speedFactor = speed / 20.0f;
        if (speedFactor > 1.0f) speedFactor = 1.0f;
        float weak = buzz * speedFactor;
        float a = dt / 0.05f;
        if (a > 1.0f) a = 1.0f;
        g_weakSmooth += (weak - g_weakSmooth) * a;

        // Damage: a punchy pulse on both motors when the vehicle's health drops.
        float health = vehicle->GetHealth();
        if (!g_haveHealth) { g_prevHealth = health; g_haveHealth = true; }
        float drop = g_prevHealth - health;
        g_prevHealth = health;
        g_damageEnv *= std::exp(-dt / 0.25f); // ~250 ms decay
        if (drop > 0.0001f) {
            float maxH = vehicle->GetMaxHealth();
            if (maxH > 0.0f && g_damageFull > 0.0f) {
                float dmg = (drop / maxH) / g_damageFull * g_damageGain;
                if (dmg > 1.0f) dmg = 1.0f;
                if (dmg > g_damageEnv) g_damageEnv = dmg;
            }
        }

        float strong = g_impactEnv * g_impactGain;
        if (g_damageEnv > strong) strong = g_damageEnv;                 // hits drive the strong motor
        float weakOut = g_weakSmooth * g_offroadGain;
        if (g_damageEnv * 0.6f > weakOut) weakOut = g_damageEnv * 0.6f; // + a sharp high-freq bite
        strong  *= g_strength;
        weakOut *= g_strength;
        if (strong  > 1.0f) strong  = 1.0f;
        if (weakOut > 1.0f) weakOut = 1.0f;

        g_motorStrong.store(static_cast<int>(strong  * 65535.0f), std::memory_order_relaxed);
        g_motorWeak.store  (static_cast<int>(weakOut * 65535.0f), std::memory_order_relaxed);

        if (g_log && (strong > 0.001f || weakOut > 0.001f))
            LOG_DEBUG("accel=%.1f speed=%.1f dmgEnv=%.2f strong=%.2f weak=%.2f",
                      accel, speed, g_damageEnv, strong, weakOut);
    }

    void Idle() {
        if (!g_enabled)
            return;
        g_impactEnv  = 0.0f;
        g_weakSmooth = 0.0f;
        g_damageEnv  = 0.0f;
        g_haveVel    = false;
        g_haveHealth = false;
        g_motorStrong.store(0, std::memory_order_relaxed);
        g_motorWeak.store(0, std::memory_order_relaxed);
    }

    // Re-read the [xinput] config snapshot. Shared by Apply()/Reapply() so a
    // control-profile switch picks up new gains (or enable/disable) live.
    static void LoadConfig() {
        const Config& config = Config::Instance();
        g_enabled     = config.xinput.value != 0;
        g_strength    = config.xinput_strength.value;
        g_impactGain  = config.xinput_impact.value;
        g_offroadGain = config.xinput_offroad.value;
        g_damageGain  = config.xinput_damage.value;
        g_damageFull  = config.xinput_damage_full.value;
        g_cfgIndex    = static_cast<int>(config.xinput_index.value);
        g_log         = config.xinput_log.value != 0;
    }

    void Apply() {
        LoadConfig();
        if (!g_enabled)
            return;
        // Device/worker are brought up lazily on the first Update (the pad may
        // connect after launch).
        LOG_INFO("XInput rumble enabled (strength=%.2f impact=%.2f offroad=%.2f damage=%.2f index=%d)",
                 g_strength, g_impactGain, g_offroadGain, g_damageGain, g_cfgIndex);
    }

    void Reapply() {
        LoadConfig(); // device/worker stay lazy; Update respects g_enabled
    }

    bool AnyConnected() {
        return AnyPadConnected();
    }
}
