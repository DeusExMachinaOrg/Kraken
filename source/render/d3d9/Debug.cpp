#include "render/d3d9/Shared.hpp"
#include "render/d3d9/Debug.hpp"

namespace kraken::render::debug {
    static wchar_t BUFFER[512] = {};

    void Marker(wchar_t* fmt, ...) {
    #ifdef DEBUG_RENDER
        va_list args;
        va_start(args, fmt);
        vswprintf(BUFFER, std::size(BUFFER), fmt, args);
        va_end(args);
        D3DPERF_SetMarker(D3DCOLOR_XRGB(128, 128, 128), BUFFER);  // ← Fixed
    #endif
    };

    void BeginMarker(const wchar_t* fmt, ...) {
    #ifdef DEBUG_RENDER
        va_list args;
        va_start(args, fmt);
        vswprintf(BUFFER, std::size(BUFFER), fmt, args);  // ← Fixed
        va_end(args);
        D3DPERF_BeginEvent(D3DCOLOR_XRGB(128, 255, 128), BUFFER);
    #endif
    };

    void EndMarker() {
    #ifdef DEBUG_RENDER
        D3DPERF_EndEvent();
    #endif
    };
};