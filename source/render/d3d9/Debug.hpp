#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <string>

#include "render/d3d9/Shared.hpp"

#define DEBUG_RENDER

namespace kraken::render::d3d9::debug {
    void Marker(wchar_t* fmt, ...);
    void BeginMarker(const wchar_t* fmt, ...);
    void EndMarker();
};