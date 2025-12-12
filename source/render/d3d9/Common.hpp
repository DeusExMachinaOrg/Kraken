#pragma once

#include <stdint.h>

namespace kraken::render::d3d9 {
    union u8color {
        struct { uint8_t  r, g, b, a; };
        struct { uint32_t color;      };
    };
};