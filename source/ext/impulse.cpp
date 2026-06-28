#define LOGGER "impulse"

#include "ext/impulse.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"

#include <list>
#include <array>

#include <windows.h>
#include <mmsystem.h>   // winmm joystick API (joyGetPosEx / joyGetDevCaps)

namespace kraken::impulse {
    using Listeners = std::list<Listener>;
    using Handlers  = std::array<Listeners, eImpulseCOUNT>;

    static struct {
        Handlers handlers                  {       };
        uint32_t frame_id                  { 0     };
        float    mouse_x                   { 0.0f  };
        float    mouse_y                   { 0.0f  };
        int32_t  size_x                    { 0     };
        int32_t  size_y                    { 0     };
        bool     supress                   { false };
    } self;

    // ---------------------------------------------------------------------
    // Joystick / steering-wheel support (MOZA R5 and any DirectInput device
    // exposed through the legacy winmm joystick API).
    //
    // Why this lives here and not in the WndProc switch above: HID game
    // controllers (wheels, gamepads) do NOT post WM_KEYDOWN/WM_CHAR window
    // messages, so their buttons/axes can never reach the message hook. The
    // engine itself imports no joystick API at all (hta.exe pulls only
    // timeGetTime from WINMM, no DirectInput/RawInput), so there is no native
    // path to lean on either. We therefore poll the device ourselves.
    //
    // The poll is driven by a WM_TIMER on the game window so it runs on the
    // same thread as the message pump (and thus the rest of the impulse
    // listeners) — no extra thread, no cross-thread event delivery.
    // ---------------------------------------------------------------------
    namespace {
        constexpr UINT_PTR JOY_TIMER_ID = 0x4B524A59; // 'KRJY'
        constexpr UINT     JOY_POLL_MS  = 16;         // ~60 Hz
        constexpr int      JOY_MAX_DEV  = 16;
        constexpr int      JOY_BUTTONS  = 16;         // eKeyJoyKey0..15
        constexpr int      JOY_AXES     = 6;          // eKeyJoyAxis0..5 / X Y Z R U V
        constexpr DWORD    JOY_AXIS_DZ  = 768;        // raw [0..65535] noise threshold

        struct JoyDevState {
            bool  present  = false;
            bool  axisInit = false;
            DWORD buttons  = 0;
            DWORD axes[JOY_AXES] = { 0 };
        };

        JoyDevState g_joy[JOY_MAX_DEV] = {};
        bool        g_joyTimerSet      = false;
    }

    void _PollJoysticks(void) {
        UINT count = joyGetNumDevs();
        if (count > JOY_MAX_DEV)
            count = JOY_MAX_DEV;

        // joyGetPosEx on absent device slots is comparatively expensive, and this
        // runs on the message-pump thread every tick. So poll already-present
        // devices every tick, and probe for hot-plugged controllers by checking a
        // SINGLE absent slot per scan, round-robin. (Scanning all 16 slots at once
        // — the previous behaviour — produced a frame spike at a regular ~1s
        // interval; probing one slot at a time spreads that cost out.)
        static UINT s_rescan    = 0;
        static UINT s_probeSlot = 0;
        int probeSlot = -1;
        if (s_rescan == 0) {
            s_rescan  = 60;                       // keep the ~1s cadence per slot
            if (count) {
                probeSlot   = static_cast<int>(s_probeSlot % count);
                s_probeSlot = (s_probeSlot + 1) % count;
            }
        } else {
            s_rescan--;
        }

        for (UINT id = 0; id < count; ++id) {
            JoyDevState& st = g_joy[id];
            if (!st.present && static_cast<int>(id) != probeSlot)
                continue;

            JOYINFOEX info = {};
            info.dwSize  = sizeof(info);
            info.dwFlags = JOY_RETURNALL;

            if (joyGetPosEx(id, &info) != JOYERR_NOERROR) {
                if (st.present) {
                    st.present = false;
                    LOG_INFO("Joystick %u disconnected", id);
                    Impulse ev          = {};
                    ev.type             = eImpulseJoyConnection;
                    ev.frame            = self.frame_id;
                    ev.joy_connect.status = eJoyStatusDisconnected;
                    ev.joy_connect.device = id;
                    Immediate(ev);
                }
                continue;
            }

            if (!st.present) {
                st.present  = true;
                st.buttons  = 0;
                st.axisInit = false;

                JOYCAPSA caps = {};
                if (joyGetDevCapsA(id, &caps, sizeof(caps)) == JOYERR_NOERROR) {
                    LOG_INFO("Joystick %u connected: '%s' buttons=%u axes=%u "
                             "X[%lu..%lu] Y[%lu..%lu] Z[%lu..%lu]",
                             id, caps.szPname, caps.wNumButtons, caps.wNumAxes,
                             caps.wXmin, caps.wXmax, caps.wYmin, caps.wYmax,
                             caps.wZmin, caps.wZmax);
                } else {
                    LOG_INFO("Joystick %u connected (caps unavailable)", id);
                }

                Impulse ev            = {};
                ev.type               = eImpulseJoyConnection;
                ev.frame              = self.frame_id;
                ev.joy_connect.status = eJoyStatusConnected;
                ev.joy_connect.device = id;
                Immediate(ev);
            }

            // --- buttons ---
            DWORD changed = info.dwButtons ^ st.buttons;
            if (changed) {
                for (int b = 0; b < JOY_BUTTONS; ++b) {
                    DWORD mask = 1u << b;
                    if (!(changed & mask))
                        continue;
                    bool pressed = (info.dwButtons & mask) != 0;
                    //LOG_INFO("Joy %u button %d %s", id, b, pressed ? "DOWN" : "UP");

                    Impulse ev           = {};
                    ev.type              = eImpulseJoyButton;
                    ev.frame             = self.frame_id;
                    ev.joy_button.key    = (eKey)(eKeyJoyKey0 + b);
                    ev.joy_button.pressed = pressed;
                    ev.joy_button.repeat  = false;
                    Immediate(ev);
                }
                st.buttons = info.dwButtons;
            }

            // --- axes ---
            const DWORD raw[JOY_AXES] = {
                info.dwXpos, info.dwYpos, info.dwZpos,
                info.dwRpos, info.dwUpos, info.dwVpos,
            };

            if (!st.axisInit) {
                for (int a = 0; a < JOY_AXES; ++a)
                    st.axes[a] = raw[a];
                st.axisInit = true;
            } else {
                for (int a = 0; a < JOY_AXES; ++a) {
                    long delta = (long)raw[a] - (long)st.axes[a];
                    if (delta < 0) delta = -delta;
                    if (delta < (long)JOY_AXIS_DZ)
                        continue;
                    st.axes[a] = raw[a];

                    float value = ((float)raw[a] - 32767.5f) / 32767.5f; // -> [-1..1]
                    //LOG_DEBUG("Joy %u axis %d = %lu (%.3f)", id, a, raw[a], value);

                    Impulse ev        = {};
                    ev.type           = eImpulseJoyAxis;
                    ev.frame          = self.frame_id;
                    ev.joy_axis.axis  = (eJoyAxis)a;
                    ev.joy_axis.value = value;
                    Immediate(ev);
                }
            }
        }
    }

    eKey _MapKeyCode(uint32_t wparam, uint32_t lparam) {
        bool extended = (lparam >> 24) & 0x1;

        switch (wparam) {
            case VK_SPACE:      return eKeySpace;
            case VK_OEM_7:      return eKeyApostrophe;
            case VK_OEM_COMMA:  return eKeyComma;
            case VK_OEM_MINUS:  return eKeyMinus;
            case VK_OEM_PERIOD: return eKeyPeriod;
            case VK_OEM_2:      return eKeySlash;
            case '0':           return eKey0;
            case '1':           return eKey1;
            case '2':           return eKey2;
            case '3':           return eKey3;
            case '4':           return eKey4;
            case '5':           return eKey5;
            case '6':           return eKey6;
            case '7':           return eKey7;
            case '8':           return eKey8;
            case '9':           return eKey9;
            case VK_OEM_1:      return eKeySemicolon;
            case VK_OEM_PLUS:   return eKeyEqual;
            case 'A':           return eKeyA;
            case 'B':           return eKeyB;
            case 'C':           return eKeyC;
            case 'D':           return eKeyD;
            case 'E':           return eKeyE;
            case 'F':           return eKeyF;
            case 'G':           return eKeyG;
            case 'H':           return eKeyH;
            case 'I':           return eKeyI;
            case 'J':           return eKeyJ;
            case 'K':           return eKeyK;
            case 'L':           return eKeyL;
            case 'M':           return eKeyM;
            case 'N':           return eKeyN;
            case 'O':           return eKeyO;
            case 'P':           return eKeyP;
            case 'Q':           return eKeyQ;
            case 'R':           return eKeyR;
            case 'S':           return eKeyS;
            case 'T':           return eKeyT;
            case 'U':           return eKeyU;
            case 'V':           return eKeyV;
            case 'W':           return eKeyW;
            case 'X':           return eKeyX;
            case 'Y':           return eKeyY;
            case 'Z':           return eKeyZ;
            case VK_OEM_4:      return eKeyLeftBracket;
            case VK_OEM_6:      return eKeyRightBracket;
            case VK_OEM_5:      return eKeyBackslash;
            case VK_OEM_3:      return eKeyGraveAccent;
            case VK_OEM_8:      return eKeyWorld1;
            case VK_OEM_102:    return eKeyWorld2;
            case VK_ESCAPE:     return eKeyEscape;
            case VK_RETURN:     return extended ? eKeyNumEnter : eKeyEnter;
            case VK_TAB:        return eKeyTab;
            case VK_BACK:       return eKeyBackspace;
            case VK_INSERT:     return eKeyInsert;
            case VK_DELETE:     return eKeyDelete;
            case VK_RIGHT:      return eKeyRight;
            case VK_LEFT:       return eKeyLeft;
            case VK_DOWN:       return eKeyDown;
            case VK_UP:         return eKeyUp;
            case VK_PRIOR:      return eKeyPageUp;
            case VK_NEXT:       return eKeyPageDown;
            case VK_HOME:       return eKeyHome;
            case VK_END:        return eKeyEnd;
            case VK_CAPITAL:    return eKeyCapsLock;
            case VK_SCROLL:     return eKeyScrollLock;
            case VK_NUMLOCK:    return eKeyNumLock;
            case VK_SNAPSHOT:   return eKeyPrintScreen;
            case VK_PAUSE:      return eKeyPause;
            case VK_F1:         return eKeyF1;
            case VK_F2:         return eKeyF2;
            case VK_F3:         return eKeyF3;
            case VK_F4:         return eKeyF4;
            case VK_F5:         return eKeyF5;
            case VK_F6:         return eKeyF6;
            case VK_F7:         return eKeyF7;
            case VK_F8:         return eKeyF8;
            case VK_F9:         return eKeyF9;
            case VK_F10:        return eKeyF10;
            case VK_F11:        return eKeyF11;
            case VK_F12:        return eKeyF12;
            case VK_F13:        return eKeyF13;
            case VK_F14:        return eKeyF14;
            case VK_F15:        return eKeyF15;
            case VK_F16:        return eKeyF16;
            case VK_F17:        return eKeyF17;
            case VK_F18:        return eKeyF18;
            case VK_F19:        return eKeyF19;
            case VK_F20:        return eKeyF20;
            case VK_F21:        return eKeyF21;
            case VK_F22:        return eKeyF22;
            case VK_F23:        return eKeyF23;
            case VK_F24:        return eKeyF24;
            case VK_NUMPAD0:    return eKeyNum0;
            case VK_NUMPAD1:    return eKeyNum1;
            case VK_NUMPAD2:    return eKeyNum2;
            case VK_NUMPAD3:    return eKeyNum3;
            case VK_NUMPAD4:    return eKeyNum4;
            case VK_NUMPAD5:    return eKeyNum5;
            case VK_NUMPAD6:    return eKeyNum6;
            case VK_NUMPAD7:    return eKeyNum7;
            case VK_NUMPAD8:    return eKeyNum8;
            case VK_NUMPAD9:    return eKeyNum9;
            case VK_DECIMAL:    return eKeyNumDecimal;
            case VK_DIVIDE:     return eKeyNumDivide;
            case VK_MULTIPLY:   return eKeyNumMultiply;
            case VK_SUBTRACT:   return eKeyNumSubstract;
            case VK_ADD:        return eKeyNumAdd;
            case VK_LSHIFT:     return eKeyLeftShift;
            case VK_LCONTROL:   return eKeyLeftControl;
            case VK_LMENU:      return eKeyLeftAlt;
            case VK_LWIN:       return eKeyLeftSuper;
            case VK_RSHIFT:     return eKeyRightShift;
            case VK_RCONTROL:   return eKeyRightControl;
            case VK_RMENU:      return eKeyRightAlt;
            case VK_RWIN:       return eKeyRightSuper;
            case VK_APPS:       return eKeyMenu;
            default:            return eKeyINVALID;
        }
    };

    WNDPROC _BASE_WndProc = (WNDPROC)(0x005A8CC0);

    LRESULT __stdcall _HOOK_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        // New messages will newer be suppressed by default
        self.supress  = false;
        Impulse event = {};

        // Arm the joystick poll timer once we have a live window on this
        // (the message-pump) thread. The game window does not exist yet at
        // impulse::Init time, so this is the first safe opportunity.
        if (!g_joyTimerSet && hWnd) {
            SetTimer(hWnd, JOY_TIMER_ID, JOY_POLL_MS, nullptr);
            g_joyTimerSet = true;
            LOG_INFO("Joystick poll timer armed (%u ms, devs=%u)", JOY_POLL_MS, joyGetNumDevs());
        }

        switch (uMsg) {
            case WM_TIMER: {
                if (wParam == JOY_TIMER_ID) {
                    _PollJoysticks();
                    return 0; // our timer — consume it, don't bother the engine
                }
                break;    // someone else's timer — forward to the base proc
            }
            case WM_CLOSE: {
                event.type      = eImpulseQuit;
                event.frame     = self.frame_id;
                event.quit.code = 0;
                Immediate(event);
                break;
            }
            case WM_DESTROY: {
                event.type      = eImpulseQuit;
                event.frame     = self.frame_id;
                event.quit.code = -1;
                Immediate(event);
                break;
            }
            case WM_SIZE: {
                int32_t current_x    = LOWORD(lParam);
                int32_t current_y    = HIWORD(lParam);
                event.type           = eImpulseResize;
                event.frame          = self.frame_id;
                event.resize.size_x  = current_x;
                event.resize.size_y  = current_y;
                event.resize.delta_x = current_x - self.size_x;
                event.resize.delta_y = current_y - self.size_y;
                self.size_x          = current_x;
                self.size_y          = current_y;
                Immediate(event);
                break;
            }
            case WM_SETFOCUS:
            case WM_KILLFOCUS: {
                event.type        = eImpulseFocus;
                event.frame       = self.frame_id;
                event.focus.state = uMsg == WM_SETFOCUS;
                Immediate(event);
                break;
            }
            case WM_SHOWWINDOW: {
                event.type          = eImpulseVisible;
                event.frame         = self.frame_id;
                event.visible.state = (wParam != 0);
                Immediate(event);
                break;
            }
            case WM_KEYDOWN:
            case WM_KEYUP: {
                event.type           = eImpulseKey;
                event.frame          = self.frame_id;
                event.key.code       = _MapKeyCode(wParam, lParam);
                event.key.repeat     = uMsg == WM_KEYDOWN && (lParam & KF_REPEAT) == KF_REPEAT;
                event.key.pressed    = uMsg == WM_KEYDOWN;
                event.key.position_x = self.mouse_x;
                event.key.position_y = self.mouse_y;
                Immediate(event);
                break;
            }
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP: {
                event.type           = eImpulseKey;
                event.frame          = self.frame_id;
                event.key.code       = _MapKeyCode(wParam, lParam);
                event.key.repeat     = uMsg == WM_SYSKEYDOWN && (lParam & KF_REPEAT) == KF_REPEAT;
                event.key.pressed    = uMsg == WM_SYSKEYDOWN;
                event.key.position_x = self.mouse_x;
                event.key.position_y = self.mouse_y;
                Immediate(event);
                break;
            }
            case WM_CHAR: {
                event.type        = eImpulseText;
                event.frame       = self.frame_id;
                event.text.symbol = wParam;
                event.text.size   = WideCharToMultiByte(CP_UTF8, 0, (wchar_t*)&wParam, 1, event.text.text, 4, NULL, NULL);
                event.text.text[event.text.size > 0 ? event.text.size : 0] = 0;
                Immediate(event);
                break;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP: {
                float current_x             = (float)LOWORD(lParam);
                float current_y             = (float)HIWORD(lParam);
                event.type                  = eImpulseMouseMove;
                event.frame                 = self.frame_id;
                event.mouse_move.position_x = current_x;
                event.mouse_move.position_y = current_y;
                event.mouse_move.delta_x    = current_x - self.mouse_x;
                event.mouse_move.delta_y    = current_y - self.mouse_y;
                self.mouse_x                = current_x;
                self.mouse_y                = current_y;
                Immediate(event);
                event.type                 = eImpulseMouseKey;
                event.frame                = self.frame_id;
                event.mouse_key.code       = eKeyMouse0;
                event.mouse_key.repeat     = (uMsg == WM_LBUTTONDOWN) && (wParam & MK_LBUTTON);
                event.mouse_key.pressed    = (uMsg == WM_LBUTTONDOWN);
                event.mouse_key.position_x = current_x;
                event.mouse_key.position_y = current_y;
                Immediate(event);
                break;
            }
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP: {
                float current_x             = (float)LOWORD(lParam);
                float current_y             = (float)HIWORD(lParam);
                event.type                  = eImpulseMouseMove;
                event.frame                 = self.frame_id;
                event.mouse_move.position_x = current_x;
                event.mouse_move.position_y = current_y;
                event.mouse_move.delta_x    = current_x - self.mouse_x;
                event.mouse_move.delta_y    = current_y - self.mouse_y;
                self.mouse_x                = current_x;
                self.mouse_y                = current_y;
                Immediate(event);
                event.type                 = eImpulseMouseKey;
                event.frame                = self.frame_id;
                event.mouse_key.code       = eKeyMouse1;
                event.mouse_key.repeat     = (uMsg == WM_RBUTTONDOWN) && (wParam & MK_RBUTTON);
                event.mouse_key.pressed    = (uMsg == WM_RBUTTONDOWN);
                event.mouse_key.position_x = current_x;
                event.mouse_key.position_y = current_y;
                Immediate(event);
                break;
            }
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP: {
                float current_x             = (float)LOWORD(lParam);
                float current_y             = (float)HIWORD(lParam);
                event.type                  = eImpulseMouseMove;
                event.frame                 = self.frame_id;
                event.mouse_move.position_x = current_x;
                event.mouse_move.position_y = current_y;
                event.mouse_move.delta_x    = current_x - self.mouse_x;
                event.mouse_move.delta_y    = current_y - self.mouse_y;
                self.mouse_x                = current_x;
                self.mouse_y                = current_y;
                Immediate(event);
                event.type                 = eImpulseMouseKey;
                event.frame                = self.frame_id;
                event.mouse_key.code       = eKeyMouse2;
                event.mouse_key.repeat     = (uMsg == WM_MBUTTONDOWN) && (wParam & MK_MBUTTON);
                event.mouse_key.pressed    = (uMsg == WM_MBUTTONDOWN);
                event.mouse_key.position_x = current_x;
                event.mouse_key.position_y = current_y;
                Immediate(event);
                break;
            }
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP: {
                float current_x             = (float)LOWORD(lParam);
                float current_y             = (float)HIWORD(lParam);
                event.type                  = eImpulseMouseMove;
                event.frame                 = self.frame_id;
                event.mouse_move.position_x = current_x;
                event.mouse_move.position_y = current_y;
                event.mouse_move.delta_x    = current_x - self.mouse_x;
                event.mouse_move.delta_y    = current_y - self.mouse_y;
                self.mouse_x                = current_x;
                self.mouse_y                = current_y;
                Immediate(event);
                WORD xbutton               = GET_XBUTTON_WPARAM(wParam);
                event.type                 = eImpulseMouseKey;
                event.frame                = self.frame_id;
                event.mouse_key.code       = (xbutton == XBUTTON1) ? eKeyMouse3 : eKeyMouse4;
                event.mouse_key.repeat     = (uMsg == WM_XBUTTONDOWN) && ((xbutton == XBUTTON1 && (wParam & MK_XBUTTON1)) || (xbutton == XBUTTON2 && (wParam & MK_XBUTTON2)));
                event.mouse_key.pressed    = (uMsg == WM_XBUTTONDOWN);
                event.mouse_key.position_x = current_x;
                event.mouse_key.position_y = current_y;
                Immediate(event);
                break;
            }
            case WM_MOUSEMOVE: {
                float current_x             = (float)LOWORD(lParam);
                float current_y             = (float)HIWORD(lParam);
                event.type                  = eImpulseMouseMove;
                event.frame                 = self.frame_id;
                event.mouse_move.position_x = current_x;
                event.mouse_move.position_y = current_y;
                event.mouse_move.delta_x    = current_x - self.mouse_x;
                event.mouse_move.delta_y    = current_y - self.mouse_y;
                self.mouse_x                = current_x;
                self.mouse_y                = current_y;
                Immediate(event);
                break;
            }
            case WM_MOUSEWHEEL: {
                event.type                = eImpulseMouseWheel;
                event.frame               = self.frame_id;
                event.mouse_wheel.delta_x = 0.0f;
                event.mouse_wheel.delta_y = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                Immediate(event);
                break;
            }
            case WM_MOUSEHWHEEL: {
                event.type                = eImpulseMouseWheel;
                event.frame               = self.frame_id;
                event.mouse_wheel.delta_x = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                event.mouse_wheel.delta_y = 0.0f;
                Immediate(event);
                break;
            }
            default: {
                event.type = eImpulseINVALID;
                break;
            }
        }
        if (!self.supress)
            return _BASE_WndProc(hWnd, uMsg, wParam, lParam);
        return 0;
    };

    void Init(void) {
        routines::RemapPtr((void*)0x005A8F88, _HOOK_WndProc);
    };

    void Clear(void) {
        for (auto& listeners : self.handlers)
            listeners.clear();
    };

    void Reset(eImpulse type) {
        self.handlers[type].clear();
    };

    void Attach(eImpulse type, Listener listener) {
        for (auto method : self.handlers[type])
            if (method == listener)
                return;
        self.handlers[type].push_back(listener);
    };

    void Detach(eImpulse type, Listener listener) {
        self.handlers[type].remove(listener);
    };

    void Suppress(void) {
        self.supress = true;
    };

    bool Immediate(const Impulse& impulse) {
        for (auto method : self.handlers[impulse.type]) {
            method(impulse);
            if (self.supress) return true;
        }
        for (auto method : self.handlers[eImpulseAny]) {
            method(impulse);
            if (self.supress) return true;
        }
        return false;
    };
};