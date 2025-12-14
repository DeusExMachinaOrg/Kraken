#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <string>

#include "render/native/Shared.hpp"

#define DEBUG_RENDER

namespace kraken::render::native::debug {
    void Marker(wchar_t* fmt, ...);
    void BeginMarker(const wchar_t* fmt, ...);
    void EndMarker();
};