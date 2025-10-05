#ifndef KRAKEN_EXT_RUNTIME
#define KRAKEN_EXT_RUNTIME

#include <stdint.h>
#include <functional>

namespace kraken::impulse {
    enum eImpulse: uint32_t {
        eImpulseQuit,
        eImpulseResize,
        eImpulseFocus,
        eImpulseVisible,
        eImpulseFence,
        eImpulseKey,
        eImpulseText,
        eImpulseMouseKey,
        eImpulseMouseMove,
        eImpulseMouseWheel,
        eImpulseJoyConnection,
        eImpulseJoyButton,
        eImpulseJoyAxis,
        eImpulseAction,
        eImpulseAny,
        eImpulseCOUNT,
        eImpulseINVALID,
    };

    enum eKey {
        eKeySpace,
        eKeyApostrophe,
        eKeyComma,
        eKeyMinus,
        eKeyPeriod,
        eKeySlash,
        eKey0,
        eKey1,
        eKey2,
        eKey3,
        eKey4,
        eKey5,
        eKey6,
        eKey7,
        eKey8,
        eKey9,
        eKeySemicolon,
        eKeyEqual,
        eKeyA,
        eKeyB,
        eKeyC,
        eKeyD,
        eKeyE,
        eKeyF,
        eKeyG,
        eKeyH,
        eKeyI,
        eKeyK,
        eKeyL,
        eKeyM,
        eKeyN,
        eKeyO,
        eKeyP,
        eKeyQ,
        eKeyR,
        eKeyS,
        eKeyT,
        eKeyU,
        eKeyV,
        eKeyW,
        eKeyX,
        eKeyY,
        eKeyZ,
        eKeyLeftBracket,
        eKeyRightBracket,
        eKeyBackslash,
        eKeyGraveAccent,
        eKeyWorld1,
        eKeyWorld2,
        eKeyEscape,
        eKeyEnter,
        eKeyTab,
        eKeyBackspace,
        eKeyInsert,
        eKeyDelete,
        eKeyRight,
        eKeyLeft,
        eKeyDown,
        eKeyUp,
        eKeyPageUp,
        eKeyPageDown,
        eKeyHome,
        eKeyEnd,
        eKeyCapsLock,
        eKeyScrollLock,
        eKeyNumLock,
        eKeyPrintScreen,
        eKeyPause,
        eKeyF1,
        eKeyF2,
        eKeyF3,
        eKeyF4,
        eKeyF5,
        eKeyF6,
        eKeyF7,
        eKeyF8,
        eKeyF9,
        eKeyF10,
        eKeyF11,
        eKeyF12,
        eKeyF13,
        eKeyF14,
        eKeyF15,
        eKeyF16,
        eKeyF17,
        eKeyF18,
        eKeyF19,
        eKeyF20,
        eKeyF21,
        eKeyF22,
        eKeyF23,
        eKeyF24,
        eKeyF25,
        eKeyNum0,
        eKeyNum1,
        eKeyNum2,
        eKeyNum3,
        eKeyNum4,
        eKeyNum5,
        eKeyNum6,
        eKeyNum7,
        eKeyNum8,
        eKeyNum9,
        eKeyNumDecimal,
        eKeyNumDivide,
        eKeyNumMultiply,
        eKeyNumSubstract,
        eKeyNumAdd,
        eKeyNumEnter,
        eKeyNumEqual,
        eKeyLeftShift,
        eKeyLeftControl,
        eKeyLeftAlt,
        eKeyLeftSuper,
        eKeyRightShift,
        eKeyRightControl,
        eKeyRightAlt,
        eKeyRightSuper,
        eKeyMenu,
        eKeyMouse0,
        eKeyMouse1,
        eKeyMouse2,
        eKeyMouse3,
        eKeyMouse4,
        eKeyMouse5,
        eKeyMouse6,
        eKeyMouse7,
        eKeyJoyKey0,
        eKeyJoyKey1,
        eKeyJoyKey2,
        eKeyJoyKey3,
        eKeyJoyKey4,
        eKeyJoyKey5,
        eKeyJoyKey6,
        eKeyJoyKey7,
        eKeyJoyKey8,
        eKeyJoyKey9,
        eKeyJoyKey10,
        eKeyJoyKey11,
        eKeyJoyKey12,
        eKeyJoyKey13,
        eKeyJoyKey14,
        eKeyJoyKey15,
        eKeyJoyAxis0,
        eKeyJoyAxis1,
        eKeyJoyAxis2,
        eKeyJoyAxis3,
        eKeyJoyAxis4,
        eKeyJoyAxis5,
        eKeyCOUNT,
        eKeyMouseLeft   = eKeyMouse0,
        eKeyMouseRight  = eKeyMouse1,
        eKeyMouseMiddle = eKeyMouse2,
    };

    enum eJoyAxis {
        eJoyAxis0,
        eJoyAxis1,
        eJoyAxis2,
        eJoyAxis3,
        eJoyAxis4,
        eJoyAxis5,
        eJoyAxisCOUNT,
    };

    enum eJoyStatus {
        eJoyStatusConnected,
        eJoyStatusDisconnected,
        eJoyStatusCOUNT,
    };

    struct Quit {
        int32_t code;
    };

    struct Resize {
        int32_t size_x;
        int32_t size_y;
        int32_t delta_x;
        int32_t delta_y;
    };

    struct Focus {
        bool state;
    };

    struct Visible {
        bool state;
    };

    struct Fence {
    };

    struct Key {
        eKey code;
        bool     repeat;
        bool     down;
        float    position_x;
        float    position_y;
    };

    struct Text {
        char text[5];
    };

    struct MouseKey {
        eKey code;
        bool     repeat;
        bool     down;
        float    position_x;
        float    position_y;
    };
    
    struct MouseMove {
        float position_x;
        float position_y;
        float delta_x;
        float delta_y;
    };

    struct MouseWheel {
        float delta_x;
        float delta_y;
    };

    struct JoyConnect {
        eJoyStatus status;
        uint32_t   device;
    };

    struct JoyAxis {
        eJoyAxis axis;
        float    value;
    };

    struct JoyButton {
        eKey key;
        bool     repeat;
        bool     down;
    };

    struct Action {
        uint32_t action;
        bool     repeat;
        bool     down;
        eKey code;
        float    value;
        float    position_x;
        float    position_y;
    };

    struct Impulse {
        eImpulse type  = eImpulseINVALID;
        uint32_t frame = 0;
        union {
            Quit       quit;
            Resize     resize;
            Focus      focus;
            Visible    visible;
            Fence      fence;
            Key        key;
            Text       text;
            MouseKey   mouse_key;
            MouseMove  mouse_move;
            MouseWheel mouse_wheel;
            JoyConnect joy_connect;
            JoyButton  joy_button;
            JoyAxis    joy_axis;
            Action     action;
        };
    };

    using Listener = void(*)(const Impulse&);

    void Init(void);
    void Free(void);
    void Poll(void);
    void Clear(void);
    void Reset(eImpulse type);
    void Attach(eImpulse type, Listener listener);
    void Detach(eImpulse type, Listener listener);
    void Supress(void);
    void Dispatch(const Impulse& impulse);
    void Immediate(const Impulse& impulse);
};

#endif
