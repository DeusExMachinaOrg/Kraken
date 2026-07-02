#define LOGGER "borderless"

#include <windows.h>
#include <stdint.h>

#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

// Reliable Alt+Enter "fullscreen" toggle for the Ex Machina / HTA engine.
//
// The engine's own Alt+Enter (gated by the g_altEnterAllow console var) drives a
// D3D9 *exclusive* fullscreen device reset through dxrender9.dll. That path is
// fragile (works one way / on some machines, reliably fails coming back to
// fullscreen). Instead we toggle a borderless window that fills the monitor and
// never use exclusive fullscreen — only the engine's reliable *windowed* device
// reset (Application::SwitchDisplayModes with bFullScreen = false) is used, which
// also resizes the backbuffer to the monitor resolution for a crisp image.
//
// All addresses are VAs for hta.exe (image base 0x400000).
namespace kraken::fix::borderless {
    namespace {
        // m3d::Application* m3d::Application::g_pApp
        void** const G_PAPP = reinterpret_cast<void**>(0x00A0A55C);
        // m3d::Application::m_renderWindow (HWND), offset inside the Application object
        constexpr uintptr_t RENDER_WINDOW_OFF = 0x8B258;

        // int __thiscall m3d::Application::SwitchDisplayModes(HWND, int w, int h, bool fullScreen)
        using SwitchDisplayModes_t = int(__thiscall*)(void* self, HWND wnd, int w, int h, int fullScreen);
        const SwitchDisplayModes_t SwitchDisplayModes =
            reinterpret_cast<SwitchDisplayModes_t>(0x005A8150);

        bool g_borderless   = false;
        bool g_inToggle     = false;
        LONG g_savedStyle   = 0;
        LONG g_savedExStyle = 0;
        RECT g_savedRect    = {};
        int  g_savedW       = 0;
        int  g_savedH       = 0;

        HWND GameWindow() {
            void* app = *G_PAPP;
            if (!app)
                return nullptr;
            HWND wnd = *reinterpret_cast<HWND*>(static_cast<char*>(app) + RENDER_WINDOW_OFF);
            return (wnd && IsWindow(wnd)) ? wnd : nullptr;
        }

        void Toggle() {
            if (g_inToggle)
                return;

            void* app = *G_PAPP;
            HWND  wnd = GameWindow();
            if (!app || !wnd) {
                LOG_WARNING("Alt+Enter ignored: render window not ready yet");
                return;
            }

            g_inToggle = true;

            if (!g_borderless) {
                // --- windowed -> borderless fullscreen ---
                RECT client;
                GetClientRect(wnd, &client);
                g_savedW       = client.right - client.left;
                g_savedH       = client.bottom - client.top;
                g_savedStyle   = GetWindowLong(wnd, GWL_STYLE);
                g_savedExStyle = GetWindowLong(wnd, GWL_EXSTYLE);
                GetWindowRect(wnd, &g_savedRect);

                HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(mon, &mi);
                const int monW = mi.rcMonitor.right  - mi.rcMonitor.left;
                const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

                // Leave exclusive fullscreen (if launched there) the reliable way and
                // size the windowed backbuffer to the monitor resolution.
                SwitchDisplayModes(app, wnd, monW, monH, 0);

                // The engine restyles to a normal titled window; override it with a
                // borderless popup covering the whole monitor.
                SetWindowLong(wnd, GWL_STYLE,
                    (g_savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX
                                      | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP | WS_VISIBLE);
                SetWindowLong(wnd, GWL_EXSTYLE,
                    g_savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE
                                       | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
                SetWindowPos(wnd, HWND_TOP,
                    mi.rcMonitor.left, mi.rcMonitor.top, monW, monH,
                    SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

                g_borderless = true;
                LOG_INFO("Borderless ON (%dx%d)", monW, monH);
            } else {
                // --- borderless fullscreen -> windowed ---
                const int w = g_savedW > 0 ? g_savedW : 1024;
                const int h = g_savedH > 0 ? g_savedH : 768;

                SwitchDisplayModes(app, wnd, w, h, 0);

                SetWindowLong(wnd, GWL_STYLE,   g_savedStyle);
                SetWindowLong(wnd, GWL_EXSTYLE, g_savedExStyle);
                SetWindowPos(wnd, HWND_NOTOPMOST,
                    g_savedRect.left, g_savedRect.top,
                    g_savedRect.right - g_savedRect.left,
                    g_savedRect.bottom - g_savedRect.top,
                    SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

                g_borderless = false;
                LOG_INFO("Borderless OFF (%dx%d)", w, h);
            }

            g_inToggle = false;
        }

        void OnKey(const impulse::Impulse& ev) {
            if (ev.type != impulse::eImpulseKey)
                return;

            const impulse::Key& k = ev.key;
            if (!k.pressed || k.repeat)
                return;
            if (k.code != impulse::eKeyEnter && k.code != impulse::eKeyNumEnter)
                return;
            // VK_MENU (Alt) is not surfaced as an impulse key, so query the OS state.
            if ((GetKeyState(VK_MENU) & 0x8000) == 0)
                return;

            Toggle();
            impulse::Suppress(); // consume Alt+Enter, don't forward to the game
        }
    }

    void Apply() {
        const Config& config = Config::Instance();
        if (config.borderless.value == 0)
            return;

        LOG_INFO("Feature enabled (Alt+Enter borderless toggle)");

        // Neutralise the engine's own Alt+Enter handler so a stray g_altEnterAllow=1
        // can't fire the broken exclusive-fullscreen path alongside our toggle.
        // m3d::Application::HandleEvent: cmp word ptr [ebx+0x34], 0x0804  ->  0xFFFF
        // (the Alt+Enter key code can never equal 0xFFFF, so the handler is skipped).
        routines::OverrideValue<uint16_t>((void*)0x005A888B, (uint16_t)0xFFFF);

        impulse::Attach(impulse::eImpulseKey, OnKey);
    }
}
