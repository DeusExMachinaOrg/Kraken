#pragma once

#include <stdint.h>

namespace kraken::render::native {
    union u8color {
        struct { uint8_t  r, g, b, a; };
        struct { uint32_t color;      };
    };
};