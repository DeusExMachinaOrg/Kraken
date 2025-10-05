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
        eKeyCode code;
        bool     repeat;
        bool     down;
        float    position_x;
        float    position_y;
    };

    struct Text {
        char text[5];
    };

    struct MouseKey {
        eKeyCode code;
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
        eKeyCode key;
        bool     repeat;
        bool     down;
    };

    struct Action {
        uint32_t action;
        bool     repeat;
        bool     down;
        eKeyCode code;
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