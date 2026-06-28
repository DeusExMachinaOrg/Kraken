#define LOGGER "dualsense"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdint.h>
#include <cstring>
#include <cstddef>
#include <cctype>
#include <cmath>
#include <atomic>

#include "fix/dualsense.hpp"
#include "config.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

#include "hta/CVector.hpp"
#include "hta/ai/Vehicle.hpp"

// DualSense (PS5) haptic feedback over HID.
//
// The game reads the pad through winmm (input only); it has no rumble path. We
// open the controller's HID device directly and push output reports on a worker
// thread (USB transfers / BT writes can block, so never on the render thread).
//
// The force itself is derived from the player vehicle's motion in Update(): a
// sharp envelope on the strong (low-frequency) motor for impacts/collisions, and
// a speed-scaled buzz on the weak (high-frequency) motor for rough ground.
//
// Report layout follows the DualSenseWindows project. Bluetooth (report 0x31)
// carries a 78-byte report with the "common" output block at offset 2 and a
// CRC32 (seed 0xA2) in the last four bytes; USB (report 0x02) is 48 bytes with
// the common block at offset 1 and no CRC.
namespace kraken::fix::dualsense {
    namespace {
        constexpr USHORT VID_SONY        = 0x054C;
        constexpr USHORT PID_DUALSENSE   = 0x0CE6;
        constexpr USHORT PID_DS_EDGE     = 0x0DF2;
        constexpr int    BT_REPORT_LEN   = 78;
        constexpr int    USB_REPORT_LEN  = 48;

        // --- config snapshot ---
        bool  g_enabled      = false;
        float g_strength     = 1.0f;
        float g_impactGain   = 1.0f;
        float g_offroadGain  = 1.0f;
        float g_damageGain   = 1.0f;
        float g_damageFull   = 0.20f; // fraction of max HP lost that gives a full pulse
        bool  g_log          = false;

        // --- device ---
        constexpr int OUT_BUF_MAX = 1024;
        HANDLE g_dev        = INVALID_HANDLE_VALUE;
        bool   g_bluetooth  = true;   // 0x31 (BT) vs 0x02 (USB) report
        int    g_writeLen   = BT_REPORT_LEN; // == HID OutputReportByteLength (Windows requires this exact size)
        int    g_inputLen   = BT_REPORT_LEN; // == HID InputReportByteLength
        bool   g_ready      = false;

        // --- direct HID input (wireless: bypass winmm) ---
        bool   g_hidInput   = false;  // [dualsense] hid_input
        uint32_t g_injDevice = 0;     // device id used for injected JoyConnection (== [wheel] device)
        CRITICAL_SECTION g_lock;
        bool   g_lockInit   = false;
        struct Snapshot {
            uint8_t lx = 128, ly = 128, rx = 128, ry = 128; // sticks (center 128)
            uint8_t l2 = 0, r2 = 0;                         // analog triggers
            uint8_t btn0 = 0, btn1 = 0, btn2 = 0;           // button bytes
            bool    valid = false;
        };
        Snapshot g_snap;
        volatile bool g_readerRun = false;
        HANDLE        g_reader    = nullptr;
        // pump diff state (message-pump thread only)
        bool    g_pumpConnected = false;
        bool    g_havePrev      = false;
        float   g_prevAxis[6]   = {0,0,0,0,0,0};
        uint32_t g_prevButtons  = 0;

        // --- published actuator targets (game thread -> worker thread) ---
        std::atomic<int> g_motorStrong { 0 }; // low-frequency (left), 0..255
        std::atomic<int> g_motorWeak   { 0 }; // high-frequency (right), 0..255
        volatile bool    g_workerRun   = false;
        HANDLE           g_worker      = nullptr;

        // ---- CRC32 (IEEE 802.3, reflected poly) for the Bluetooth report ----
        uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len) {
            for (size_t i = 0; i < len; ++i) {
                crc ^= data[i];
                for (int b = 0; b < 8; ++b)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return crc;
        }

        // Case-insensitive substring search (device paths embed vid/pid as text).
        bool PathHas(const char* path, const char* needle) {
            for (const char* p = path; *p; ++p) {
                const char* a = p;
                const char* b = needle;
                while (*b && *a && (std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b))) {
                    ++a; ++b;
                }
                if (!*b)
                    return true;
            }
            return false;
        }

        // ---- device discovery -------------------------------------------------
        bool OpenDevice() {
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO set = SetupDiGetClassDevsA(&hidGuid, nullptr, nullptr,
                                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE)
                return false;

            HANDLE best      = INVALID_HANDLE_VALUE;
            int    bestLen   = 0;
            int    bestInLen = 0;
            bool   bestIsBT  = true;
            int    candidates = 0;

            SP_DEVICE_INTERFACE_DATA ifd = {};
            ifd.cbSize = sizeof(ifd);
            for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailA(set, &ifd, nullptr, 0, &need, nullptr);
                if (!need)
                    continue;
                auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(std::malloc(need));
                if (!detail)
                    continue;
                // SetupAPI quirk: cbSize must be the unpadded struct size (DWORD +
                // first path char = 5 on Win32 ANSI), NOT sizeof() which the
                // compiler pads to 8 -> the call would fail with ERROR_INVALID_USER_BUFFER.
                detail->cbSize = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + sizeof(char);

                if (SetupDiGetDeviceInterfaceDetailA(set, &ifd, detail, need, nullptr, nullptr)) {
                    // Filter to Sony DualSense by the vid/pid embedded in the path,
                    // before opening (so we don't probe every HID device).
                    if (PathHas(detail->DevicePath, "vid&0002054c") || PathHas(detail->DevicePath, "vid_054c")) {
                        if (PathHas(detail->DevicePath, "0ce6") || PathHas(detail->DevicePath, "0df2")) {
                            ++candidates;
                            HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                   OPEN_EXISTING, 0, nullptr);
                            if (h == INVALID_HANDLE_VALUE) {
                                // Retry write-only (some stacks deny read on the BT HID).
                                h = CreateFileA(detail->DevicePath, GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                OPEN_EXISTING, 0, nullptr);
                            }
                            if (h == INVALID_HANDLE_VALUE) {
                                DWORD e = GetLastError();
                                LOG_WARNING("DualSense candidate open failed (err=%lu): %s", e, detail->DevicePath);
                                if (e == ERROR_SHARING_VIOLATION) // 32
                                    LOG_WARNING("DualSense is held exclusively by another app "
                                                "(DS4Windows / DSX / Steam Input). Close it so Kraken "
                                                "can drive rumble/triggers directly.");
                            } else {
                                int outLen = 0, inLen = 0;
                                PHIDP_PREPARSED_DATA pp = nullptr;
                                if (HidD_GetPreparsedData(h, &pp)) {
                                    HIDP_CAPS caps = {};
                                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                                        outLen = caps.OutputReportByteLength;
                                        inLen  = caps.InputReportByteLength;
                                    }
                                    HidD_FreePreparsedData(pp);
                                }
                                LOG_INFO("DualSense candidate: outLen=%d inLen=%d  %s",
                                         outLen, inLen, detail->DevicePath);
                                if (outLen > bestLen) {
                                    if (best != INVALID_HANDLE_VALUE)
                                        CloseHandle(best);
                                    best      = h;
                                    bestLen   = outLen;
                                    bestInLen = inLen;
                                    // Bluetooth HID interface paths carry the BT base UUID.
                                    bestIsBT = PathHas(detail->DevicePath, "00805f9b34fb");
                                } else {
                                    CloseHandle(h);
                                }
                            }
                        }
                    }
                }
                std::free(detail);
            }
            SetupDiDestroyDeviceInfoList(set);

            if (best == INVALID_HANDLE_VALUE) {
                if (candidates == 0)
                    LOG_WARNING("DualSense: no HID interface with Sony VID/PID found");
                return false;
            }

            g_dev       = best;
            g_bluetooth = bestIsBT;
            // Windows requires the WriteFile length to equal OutputReportByteLength;
            // the 0x31/0x02 report sits at the start and the rest is zero padding.
            g_writeLen  = (bestLen > 0 && bestLen <= OUT_BUF_MAX) ? bestLen
                        : (bestIsBT ? BT_REPORT_LEN : USB_REPORT_LEN);
            g_inputLen  = (bestInLen > 0 && bestInLen <= OUT_BUF_MAX) ? bestInLen : BT_REPORT_LEN;
            LOG_INFO("DualSense opened (%s, writeLen=%d, inputLen=%d)",
                     g_bluetooth ? "Bluetooth" : "USB", g_writeLen, g_inputLen);
            return true;
        }

        // ---- output report ----------------------------------------------------
        void SendReport(uint8_t strong, uint8_t weak) {
            if (g_dev == INVALID_HANDLE_VALUE)
                return;

            uint8_t buf[OUT_BUF_MAX] = {0};
            uint8_t* common;
            if (g_bluetooth) {
                buf[0]  = 0x31;        // report id (BT)
                buf[1]  = 0x02;        // protocol/seq tag
                common  = buf + 2;
            } else {
                buf[0]  = 0x02;        // report id (USB)
                common  = buf + 1;
            }

            common[0] = 0x03;          // flag0: COMPATIBLE_VIBRATION | HAPTICS_SELECT
            common[1] = 0x00;          // flag1
            common[2] = strong;        // left  motor (low frequency)
            common[3] = weak;          // right motor (high frequency)

            if (g_bluetooth) {
                uint8_t  seed = 0xA2;
                uint32_t crc  = Crc32(0xFFFFFFFFu, &seed, 1);
                crc = Crc32(crc, buf, BT_REPORT_LEN - 4);
                crc = ~crc;
                buf[74] = static_cast<uint8_t>(crc        & 0xFF);
                buf[75] = static_cast<uint8_t>((crc >> 8)  & 0xFF);
                buf[76] = static_cast<uint8_t>((crc >> 16) & 0xFF);
                buf[77] = static_cast<uint8_t>((crc >> 24) & 0xFF);
            }

            DWORD written = 0;
            if (!WriteFile(g_dev, buf, g_writeLen, &written, nullptr)) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    LOG_WARNING("DualSense WriteFile failed (err=%lu, len=%d, %s)",
                                GetLastError(), g_writeLen, g_bluetooth ? "BT" : "USB");
                }
            }
        }

        DWORD WINAPI Worker(LPVOID) {
            int lastS = -1, lastW = -1;
            int heartbeat = 0;
            while (g_workerRun) {
                int s = g_motorStrong.load(std::memory_order_relaxed);
                int w = g_motorWeak.load(std::memory_order_relaxed);
                // Resend on change, plus a periodic heartbeat so a dropped BT
                // packet doesn't leave a stale force latched.
                if (s != lastS || w != lastW || ++heartbeat >= 20) {
                    SendReport(static_cast<uint8_t>(s), static_cast<uint8_t>(w));
                    lastS = s;
                    lastW = w;
                    heartbeat = 0;
                }
                Sleep(8); // ~120 Hz
            }
            SendReport(0, 0); // release on shutdown
            return 0;
        }

        // ---- input side: read the 0x31 report, parse, inject into impulse ------

        // Parse a raw HID input report into the shared snapshot. DualSense layout
        // (Linux hid-playstation): the input struct sits at +2 over Bluetooth
        // (after 0x31 + a tag byte) and +1 over USB (after 0x01); fields are
        // LX,LY,RX,RY,L2,R2,seq,buttons0,buttons1,buttons2.
        void ParseInput(const uint8_t* buf, DWORD n) {
            int off;
            if (g_bluetooth) {
                if (buf[0] != 0x31 || n < 12) return; // ignore the pre-switch 0x01 minimal report
                off = 2;
            } else {
                if (buf[0] != 0x01 || n < 11) return;
                off = 1;
            }
            EnterCriticalSection(&g_lock);
            g_snap.lx   = buf[off + 0];
            g_snap.ly   = buf[off + 1];
            g_snap.rx   = buf[off + 2];
            g_snap.ry   = buf[off + 3];
            g_snap.l2   = buf[off + 4];
            g_snap.r2   = buf[off + 5];
            g_snap.btn0 = buf[off + 7];
            g_snap.btn1 = buf[off + 8];
            g_snap.btn2 = buf[off + 9];
            g_snap.valid = true;
            LeaveCriticalSection(&g_lock);
        }

        DWORD WINAPI ReaderThread(LPVOID) {
            uint8_t buf[OUT_BUF_MAX];
            int want = (g_inputLen > 0 && g_inputLen <= OUT_BUF_MAX) ? g_inputLen : BT_REPORT_LEN;
            while (g_readerRun) {
                DWORD got = 0;
                if (ReadFile(g_dev, buf, want, &got, nullptr) && got >= 12)
                    ParseInput(buf, got);
                else
                    Sleep(2);
            }
            return 0;
        }

        // Pack the DualSense button bytes into our button-index order, matching the
        // [gamepad] config (0=Square 1=Cross 2=Circle 3=Triangle 4=L1 5=R1 6=L2
        // 7=R2 8=Create 9=Options 10=L3 11=R3 12=PS 13=Touchpad).
        uint32_t ButtonMask(const Snapshot& s) {
            uint32_t b = 0;
            if (s.btn0 & 0x10) b |= 1u << 0;
            if (s.btn0 & 0x20) b |= 1u << 1;
            if (s.btn0 & 0x40) b |= 1u << 2;
            if (s.btn0 & 0x80) b |= 1u << 3;
            if (s.btn1 & 0x01) b |= 1u << 4;
            if (s.btn1 & 0x02) b |= 1u << 5;
            if (s.btn1 & 0x04) b |= 1u << 6;
            if (s.btn1 & 0x08) b |= 1u << 7;
            if (s.btn1 & 0x10) b |= 1u << 8;
            if (s.btn1 & 0x20) b |= 1u << 9;
            if (s.btn1 & 0x40) b |= 1u << 10;
            if (s.btn1 & 0x80) b |= 1u << 11;
            if (s.btn2 & 0x01) b |= 1u << 12;
            if (s.btn2 & 0x02) b |= 1u << 13;
            return b;
        }

        // ---- feedback model (game thread) -------------------------------------
        float       g_impactEnv  = 0.0f; // decaying envelope for the strong motor
        float       g_weakSmooth = 0.0f; // smoothed weak-motor level
        float       g_damageEnv  = 0.0f; // decaying envelope for taking damage
        bool        g_haveVel    = false;
        hta::CVector g_prevVel;
        bool        g_haveHealth = false;
        float       g_prevHealth = 0.0f;

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

        bool EnsureReady() {
            if (g_ready)
                return true;
            // Retry device discovery roughly twice a second until the pad shows up.
            static int retry = 0;
            if (retry-- > 0)
                return false;
            retry = 30;
            if (OpenDevice()) {
                g_workerRun = true;
                g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
                if (g_hidInput) {
                    // Sending an output report flips a Bluetooth DualSense into
                    // full-report mode; the reader then gets 0x31 input reports.
                    g_readerRun = true;
                    g_reader = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
                }
                g_ready = true;
            }
            return g_ready;
        }
    }

    void PumpInput() {
        if (!g_enabled || !g_hidInput)
            return;
        if (!EnsureReady())
            return;

        Snapshot s;
        EnterCriticalSection(&g_lock);
        s = g_snap;
        LeaveCriticalSection(&g_lock);
        if (!s.valid)
            return; // still waiting for the first full-mode report

        using namespace kraken::impulse;

        // Re-assert the connection every pump so g_present (which gates the control
        // path) can't get latched false by a winmm disconnect of the same physical
        // pad once it's in full-report mode. Cheap: a listener just sets a bool.
        {
            Impulse ev = {};
            ev.type               = eImpulseJoyConnection;
            ev.joy_connect.status = eJoyStatusConnected;
            ev.joy_connect.device = g_injDevice;
            Immediate(ev);
        }

        // Axes 0=LX 1=LY 2=RX 3=RY 4=L2 5=R2 in [-1..1]. Triggers rest at -1.
        float axes[6] = {
            (s.lx - 127.5f) / 127.5f,
            (s.ly - 127.5f) / 127.5f,
            (s.rx - 127.5f) / 127.5f,
            (s.ry - 127.5f) / 127.5f,
            (s.l2 / 255.0f) * 2.0f - 1.0f,
            (s.r2 / 255.0f) * 2.0f - 1.0f,
        };
        for (int a = 0; a < 6; ++a) {
            float d = axes[a] - g_prevAxis[a];
            if (d < 0) d = -d;
            if (!g_havePrev || d > 0.012f) {
                g_prevAxis[a] = axes[a];
                Impulse ev = {};
                ev.type           = eImpulseJoyAxis;
                ev.joy_axis.axis  = static_cast<eJoyAxis>(a);
                ev.joy_axis.value = axes[a];
                Immediate(ev);
            }
        }

        uint32_t btn     = ButtonMask(s);
        uint32_t changed = g_havePrev ? (btn ^ g_prevButtons) : btn;
        for (int b = 0; b < 14; ++b) {
            uint32_t m = 1u << b;
            if (changed & m) {
                Impulse ev = {};
                ev.type               = eImpulseJoyButton;
                ev.joy_button.key     = static_cast<eKey>(eKeyJoyKey0 + b);
                ev.joy_button.pressed = (btn & m) != 0;
                ev.joy_button.repeat  = false;
                Immediate(ev);
            }
        }
        g_prevButtons = btn;
        g_havePrev    = true;
    }

    void Update(hta::ai::Vehicle* vehicle) {
        if (!g_enabled || !vehicle)
            return;
        if (!EnsureReady())
            return;

        float dt = QpcDt();

        hta::CVector vel = vehicle->GetLinearVelocity();
        if (!g_haveVel) {
            g_prevVel = vel;
            g_haveVel = true;
        }
        hta::CVector dv = vel - g_prevVel;
        g_prevVel = vel;

        float accel = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z) / dt;
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);

        // Strong motor: sharp envelope on large acceleration spikes (impacts).
        float impact = (accel - 60.0f) / 240.0f * g_impactGain;
        if (impact < 0.0f) impact = 0.0f;
        if (impact > 1.0f) impact = 1.0f;
        g_impactEnv *= std::exp(-dt / 0.12f); // ~120 ms decay
        if (impact > g_impactEnv)
            g_impactEnv = impact;

        // Weak motor: sustained moderate jitter scaled by speed (rough ground).
        float buzz = (accel - 12.0f) / 50.0f * g_offroadGain;
        if (buzz < 0.0f) buzz = 0.0f;
        if (buzz > 1.0f) buzz = 1.0f;
        float speedFactor = speed / 20.0f;
        if (speedFactor > 1.0f) speedFactor = 1.0f;
        float weak = buzz * speedFactor;
        // light low-pass so the buzz isn't gritty frame-to-frame
        float a = dt / 0.05f;
        if (a > 1.0f) a = 1.0f;
        g_weakSmooth += (weak - g_weakSmooth) * a;

        // Damage: a punchy pulse on both motors when the vehicle's health drops
        // (catches weapon hits, which barely move the vehicle so the impact model
        // above misses them). Scaled by the fraction of max health lost.
        float health = vehicle->GetHealth();
        if (!g_haveHealth) {
            g_prevHealth = health;
            g_haveHealth = true;
        }
        float drop = g_prevHealth - health;
        g_prevHealth = health;
        g_damageEnv *= std::exp(-dt / 0.25f); // ~250 ms decay
        if (drop > 0.0001f) {
            float maxH = vehicle->GetMaxHealth();
            if (maxH > 0.0f && g_damageFull > 0.0f) {
                // Full pulse when the frame's health loss reaches damage_full of max HP.
                float dmg = (drop / maxH) / g_damageFull * g_damageGain;
                if (dmg > 1.0f) dmg = 1.0f;
                if (dmg > g_damageEnv) g_damageEnv = dmg;
            }
        }

        float strong = g_impactEnv;
        if (g_damageEnv > strong) strong = g_damageEnv;          // hits drive the strong motor
        float weakOut = g_weakSmooth;
        if (g_damageEnv * 0.6f > weakOut) weakOut = g_damageEnv * 0.6f; // + a sharp high-freq bite
        strong  *= g_strength;
        weakOut *= g_strength;
        if (strong  > 1.0f) strong  = 1.0f;
        if (weakOut > 1.0f) weakOut = 1.0f;

        g_motorStrong.store(static_cast<int>(strong  * 255.0f), std::memory_order_relaxed);
        g_motorWeak.store  (static_cast<int>(weakOut * 255.0f), std::memory_order_relaxed);

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
        g_haveHealth = false; // re-baseline health next time, so respawn/heal doesn't pulse
        g_motorStrong.store(0, std::memory_order_relaxed);
        g_motorWeak.store(0, std::memory_order_relaxed);
    }

    void Apply() {
        const Config& config = Config::Instance();
        if (config.dualsense.value == 0)
            return;

        g_enabled     = true;
        g_strength    = config.dualsense_strength.value;
        g_impactGain  = config.dualsense_impact.value;
        g_offroadGain = config.dualsense_offroad.value;
        g_damageGain  = config.dualsense_damage.value;
        g_damageFull  = config.dualsense_damage_full.value;
        g_log         = config.dualsense_log.value != 0;
        g_hidInput    = config.dualsense_hid_input.value != 0;
        g_injDevice   = config.wheel_device.value;

        if (!g_lockInit) {
            InitializeCriticalSection(&g_lock);
            g_lockInit = true;
        }

        // Device is opened lazily on the first Update (the pad may connect after
        // launch), so just announce intent here.
        LOG_INFO("DualSense feedback enabled (strength=%.2f impact=%.2f offroad=%.2f hid_input=%d)",
                 g_strength, g_impactGain, g_offroadGain, g_hidInput ? 1 : 0);
    }
}
