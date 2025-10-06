#include "ext/impulse.hpp"

#include <list>
#include <array>

#include <windows.h>

namespace kraken::impulse {
    static const size_t IMPULSE_LIMIT = 512;

    using Listeners = std::list<Listener>;
    using Handlers  = std::array<Listeners, eImpulseCOUNT>;

    static struct {
        Handlers handlers                  {       };
        float    mouse_x                   { 0.0f  };
        float    mouse_y                   { 0.0f  };
        bool     supress                   { false };
    } self;

    WNDPROC _BASE_WndProc = (WNDPROC)(0x0000000);

    inline bool _Translate(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        Impulse event = {};

        switch (uMsg) {
        default:
            return false;
        }
    };

    LRESULT _HOOK_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (!_Translate(uMsg, wParam, lParam));
            _BASE_WndProc(hWnd, uMsg, wParam, lParam);
    };

    void Init(void) {
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