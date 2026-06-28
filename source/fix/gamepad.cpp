#define LOGGER "gamepad"

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cstdio>

#include "fix/gamepad.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

#include "hta/CStr.hpp"
#include "hta/m3d/GameImpulse.hpp"

// Gamepad (DualSense / any winmm joystick) button bridge.
//
// The engine already ships with a complete joystick-input subsystem that is
// simply never fed: the key-name table holds JOY_BUTTON_0..9 (engine key-ids
// 0x109..0x112) plus axis keys, the control-settings menu (BindKeysWnd) knows
// how to bind them, and GameImpulse::HandleKeyboardMouseEvent already decodes
// them. It is dormant only because stock hta.exe imports no joystick API, so no
// joystick m3d::Event is ever produced. Kraken already polls the device (see
// ext/impulse.cpp, winmm joyGetPosEx) and broadcasts eImpulseJoyButton.
//
// This module is the missing pump: it turns each Kraken joystick-button change
// into the engine joystick m3d::Event the dormant path expects and hands it to
// WndStation::DispatchJoystick. From there the engine drives whatever action the
// button is bound to (in-game) and the bind menu can capture it (native binding).
//
// Driving (steer/throttle/brake) is intentionally NOT routed here: it stays
// analog through the [wheel] path in fix/controls.cpp. This bridge only carries
// the discrete face/shoulder buttons (inventory, weapons, horn, gears, ...).
//
// All addresses are VAs for hta.exe (image base 0x400000).
namespace kraken::fix::gamepad {
    namespace {
        // m3d::Application* m3d::Application::g_pApp (== CMiracle3d::Instance()).
        constexpr uintptr_t G_PAPP_VA = 0x00A0A55C;
        // Application derives from ui::WndStation; the WndStation base subobject
        // sits at +0x0c inside Application (see Application.hpp layout).
        constexpr uintptr_t WNDSTATION_OFFSET = 0x0C;

        // int __thiscall WndStation::DispatchJoystick(const m3d::Event& ev)
        // (VA 0x0058CB00). Internally fetches g_pApp->m_pImpulses and calls
        // GameImpulse::HandleKeyboardMouseEvent(ev, this), i.e. the action path.
        using DispatchJoystick_t = int(__thiscall*)(void* wndStation, const void* ev);
        const DispatchJoystick_t DispatchJoystick =
            reinterpret_cast<DispatchJoystick_t>(0x0058CB00);

        // HandleKeyboardMouseEvent's jump table maps a joystick-button event to a
        // key-id via keyId = m_eventType + 0xF9. JOY_BUTTON_0..9 = 0x109..0x112,
        // so the event types are 0x10..0x19. The press/release state is read from
        // the event's u16 at byte offset 0x38 (non-zero = pressed).
        constexpr int      JOY_EVENTTYPE_BASE = 0x10; // -> JOY_BUTTON_0
        constexpr int      JOY_BUTTON_COUNT   = 10;   // engine supports buttons 0..9
        constexpr unsigned EVENT_SIZE         = 0x48; // sizeof(m3d::Event)
        constexpr unsigned EVENT_OFF_TYPE     = 0x08; // m_eventType (int32)
        constexpr unsigned EVENT_OFF_STATE    = 0x38; // m_ushortEv[2] (state word)

        // Engine key-id of JOY_BUTTON_0 (table @0x9FFBD0). Button N -> 0x109+N.
        constexpr int      JOY_KEYID_BASE     = 0x109;

        // ---- control-settings menu rebinding -------------------------------
        // The bind dialog row is BindKeysWnd::KeySetButton. Clicking it
        // (OnMouseButton0) arms it: sets the "capturing" flag at +0x27c and
        // captures keyboard focus to itself. A keyboard key then reaches its
        // OnKey, which fills an in-progress key vector and notifies the parent to
        // commit via OnRebind. Joystick events never reach that OnKey (and its
        // capture is byte/scancode based anyway), so when a controller button is
        // pressed while a row is armed we commit the binding ourselves by calling
        // the very function the keyboard path ends at: KeySetButton::OnRebind,
        // handing it a one-element key vector { JOY_BUTTON_N }.
        constexpr uintptr_t WNDSTATION_FOCUS_OFFSET = 0x228; // WndStation::m_wndKbdCapture (GetFocus reads this)
        constexpr uintptr_t KEYSETBUTTON_VTABLE     = 0x009D1890;
        constexpr unsigned  KEYSETBUTTON_CAPTURING  = 0x27C; // +0x27c == 1 while armed
        constexpr unsigned  KEYSETBUTTON_CAPVEC     = 0x26C; // in-progress captured std::vector<int>
        constexpr unsigned  WND_NOTIFY_BINDING      = 0x15;  // CallParentNotify id used by OnKey on capture

        // void __thiscall std::vector<int>::push_back(const int* val) @0x0042D270
        using PushBackInt_t = void(__thiscall*)(void* vec, const int* val);
        const PushBackInt_t VecPushBackInt = reinterpret_cast<PushBackInt_t>(0x0042D270);
        // int __thiscall Wnd::CallParentNotify(uint msg, AIParam* data, bool urgent) @0x00676AA0
        using CallParentNotify_t = int(__thiscall*)(void* wnd, unsigned int msg, void* data, bool urgent);
        const CallParentNotify_t CallParentNotify = reinterpret_cast<CallParentNotify_t>(0x00676AA0);
        // void __thiscall AIParam::Detach() @0x00404FF0 (cleanup for a stack AIParam)
        using AIParamDetach_t = void(__thiscall*)(void* aiParam);
        const AIParamDetach_t AIParamDetach = reinterpret_cast<AIParamDetach_t>(0x00404FF0);

        bool g_log = false;

        void* GameApp() { return *reinterpret_cast<void**>(G_PAPP_VA); }

        void SendButton(int button, bool pressed) {
            void* app = GameApp();
            if (!app)
                return; // application/window not up yet
            void* wndStation = static_cast<char*>(app) + WNDSTATION_OFFSET;

            // The joystick-button path reads only m_eventType (+0x08) and the
            // state word (+0x38); the CStr/AIParam members the constructor would
            // init are never touched, so a zeroed buffer is safe and avoids any
            // CStr ABI concerns.
            alignas(8) unsigned char ev[EVENT_SIZE] = {0};
            *reinterpret_cast<int32_t*>(ev + EVENT_OFF_TYPE)   = JOY_EVENTTYPE_BASE + button;
            *reinterpret_cast<uint16_t*>(ev + EVENT_OFF_STATE) = pressed ? 1 : 0;

            DispatchJoystick(wndStation, ev);

            if (g_log)
                LOG_DEBUG("button %d %s -> eventType 0x%02X (JOY_BUTTON_%d)",
                          button, pressed ? "down" : "up",
                          JOY_EVENTTYPE_BASE + button, button);
        }

        // If the control-settings bind dialog has an armed row (a focused
        // KeySetButton in capturing mode), commit JOY_BUTTON_<button> to it and
        // report true (so the press is consumed instead of firing an action).
        bool TryCaptureForBind(int button) {
            void* app = GameApp();
            if (!app)
                return false;
            char* wndStation = static_cast<char*>(app) + WNDSTATION_OFFSET;
            void* focus = *reinterpret_cast<void**>(wndStation + WNDSTATION_FOCUS_OFFSET);
            if (!focus)
                return false;
            // Must be a KeySetButton (its vtable) currently armed for capture.
            if (*reinterpret_cast<uintptr_t*>(focus) != KEYSETBUTTON_VTABLE)
                return false;
            if (*(reinterpret_cast<uint8_t*>(focus) + KEYSETBUTTON_CAPTURING) != 1)
                return false;

            // Drive the exact path the keyboard capture uses on a keypress: append
            // the key-id to the row's in-progress captured vector (+0x26c, empty
            // while armed) and raise the binding notify so the engine runs
            // BindKeysList::ProcessBinding. That is what writes the binding LIVE
            // into GameImpulse (Apply = SaveToProfile just persists that state) and
            // also commits the row display + releases the capture via OnRebind.
            // Calling OnRebind directly only updated the display, so "Apply" saved
            // an unchanged GameImpulse and the binding never took.
            int kid = JOY_KEYID_BASE + button;
            VecPushBackInt(static_cast<char*>(focus) + KEYSETBUTTON_CAPVEC, &kid);

            unsigned char aiParam[0x1c] = {0}; // sizeof(m3d::AIParam); zeroed = empty
            CallParentNotify(focus, WND_NOTIFY_BINDING, aiParam, false);
            AIParamDetach(aiParam);

            LOG_INFO("menu: assigned focused control slot -> JOY_BUTTON_%d", button);
            return true;
        }

        // ---- binding (config-driven) ---------------------------------------
        //
        // The engine's joystick-binding *menu* is dead code in this build (the
        // bind button's only capture handler, OnKey, is scancode/byte-based and
        // cannot represent joystick key-ids 0x109+, and the "AnotherInput" feeder
        // is never invoked). So we create the JOY_BUTTON_* bindings ourselves via
        // the engine's own binding API (GameImpulse::BindKey1, the very call the
        // game's defaultkeybindings.lua uses), reading button->impulse names from
        // the [gamepad] config.
        //
        // Bindings live in GameImpulse and are wiped by the UnbindAll the game
        // runs whenever it (re)loads its key bindings, so we re-apply right after
        // each load by detouring LoadFromDefaults / LoadFromProfile. Once present
        // the binding also shows up in the control-settings menu (the engine can
        // render JOY_BUTTON_* names), it just can't be captured there.

        using LoadFn = int(__thiscall*)(void* self);
        LoadFn g_origLoadDefaults = nullptr;
        LoadFn g_origLoadProfile  = nullptr;

        void ApplyBindings(void* impulsesRaw) {
            auto* impulses = reinterpret_cast<hta::m3d::GameImpulse*>(impulsesRaw);
            if (!impulses || !impulses->m_isInited)
                return; // BindKey1 would just log "impulses not inited"

            const Config& config = Config::Instance();
            if (config.gamepad_autobind.value == 0)
                return; // ini is not the authority: leave bindings to the menu / saved profile
            hta::CStr mode = config.gamepad_mode.value.c_str();

            int bound = 0;
            for (int i = 0; i < 10; ++i) {
                const std::string& imp = config.gamepad_button[i].value;
                if (imp.empty())
                    continue;
                char keyName[16];
                std::snprintf(keyName, sizeof(keyName), "JOY_BUTTON_%d", i);
                hta::CStr key     = keyName;
                hta::CStr impName = imp.c_str();
                impulses->BindKey1(mode, key, impName);
                ++bound;
                if (g_log)
                    LOG_INFO("bound JOY_BUTTON_%d -> %s", i, imp.c_str());
            }
            if (bound)
                LOG_INFO("applied %d gamepad button binding(s) [%s]",
                         bound, config.gamepad_mode.value.c_str());
        }

        int __fastcall LoadDefaults_Hook(void* self, void*) {
            int r = g_origLoadDefaults(self);
            ApplyBindings(self);
            return r;
        }

        int __fastcall LoadProfile_Hook(void* self, void*) {
            int r = g_origLoadProfile(self);
            ApplyBindings(self);
            return r;
        }

        // Entry detour with a runtime trampoline (mirrors fix/controls.cpp). The
        // trampoline holds the relocated prologue + a jmp back so the hook can run
        // the original; prologueLen must end on an instruction boundary.
        LoadFn InstallLoadDetour(uintptr_t addr, int prologueLen, void* hook) {
            uint8_t* fn = reinterpret_cast<uint8_t*>(addr);
            uint8_t* tramp = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, prologueLen + 5, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE));
            if (!tramp) {
                LOG_ERROR("Failed to allocate load-detour trampoline @ 0x%p", (void*)addr);
                return nullptr;
            }
            std::memcpy(tramp, fn, prologueLen);
            tramp[prologueLen] = 0xE9;
            *reinterpret_cast<int32_t*>(tramp + prologueLen + 1) =
                static_cast<int32_t>((fn + prologueLen) - (tramp + prologueLen + 5));

            DWORD prot;
            VirtualProtect(fn, prologueLen, PAGE_EXECUTE_READWRITE, &prot);
            fn[0] = 0xE9; // jmp hook
            *reinterpret_cast<int32_t*>(fn + 1) =
                static_cast<int32_t>(reinterpret_cast<uint8_t*>(hook) - (fn + 5));
            for (int i = 5; i < prologueLen; ++i)
                fn[i] = 0x90; // tidy any tail byte of the last relocated instruction
            VirtualProtect(fn, prologueLen, prot, &prot);
            return reinterpret_cast<LoadFn>(tramp);
        }

        void OnImpulse(const impulse::Impulse& e) {
            if (e.type != impulse::eImpulseJoyButton)
                return;
            // Kraken encodes the winmm button index as eKeyJoyKey0 + index.
            int button = static_cast<int>(e.joy_button.key) -
                         static_cast<int>(impulse::eKeyJoyKey0);
            if (button < 0 || button >= JOY_BUTTON_COUNT)
                return; // engine only has names/ids for buttons 0..9

            // On press, first offer the button to the control-settings bind dialog
            // (if a row is armed). If it takes it, consume the press so it doesn't
            // also fire whatever the button is currently bound to.
            if (e.joy_button.pressed && TryCaptureForBind(button))
                return;

            SendButton(button, e.joy_button.pressed);
        }
    }

    void Apply() {
        const Config& config = Config::Instance();
        if (config.gamepad.value == 0)
            return;

        g_log = config.gamepad_log.value != 0;

        // Runs on the message-pump thread (impulse fires from the WM_TIMER joy
        // poll), the same thread the engine's keyboard WndProc dispatch uses — so
        // feeding GameImpulse from here is no less safe than the keyboard path.
        impulse::Attach(impulse::eImpulseJoyButton, OnImpulse);

        // Re-apply our JOY_BUTTON_* -> impulse bindings after the engine (re)loads
        // its key bindings. LoadFromProfile prologue is 5 bytes
        // (sub esp,0x48 / push ebx / push esi); LoadFromDefaults is 6
        // (sub esp,0x18 / push esi / mov esi,ecx).
        g_origLoadDefaults = InstallLoadDetour(0x00597990, 6, (void*)&LoadDefaults_Hook);
        g_origLoadProfile  = InstallLoadDetour(0x00597AB0, 5, (void*)&LoadProfile_Hook);

        LOG_INFO("Gamepad bridge enabled (buttons 0..%d -> engine JOY_BUTTON_*)",
                 JOY_BUTTON_COUNT - 1);
    }
}
