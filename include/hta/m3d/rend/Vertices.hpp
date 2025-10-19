#pragma once

#include "hta/CVector.h"

#include <stdint.h>

namespace m3d::rend {
    struct VertexLandscape {
        /* Size=0x8 */
        /* 0x0000 */ float y;
        /* 0x0004 */ int16_t xz;
        /* 0x0006 */ int16_t uv;
    };

    struct VertexXYZNCT2 {
        /* Size=0x2c */
        /* 0x0000 */ float x;
        /* 0x0004 */ float y;
        /* 0x0008 */ float z;
        /* 0x000c */ float nx;
        /* 0x0010 */ float ny;
        /* 0x0014 */ float nz;
        /* 0x0018 */ uint32_t c;
        /* 0x001c */ float tu0;
        /* 0x0020 */ float tv0;
        /* 0x0024 */ float tu1;
        /* 0x0028 */ float tv1;
    
        void xyz(const CVector&);
        void xyz(float, float, float);
        void n(float, float, float);
        void n(const CVector&);
        void uv0(float, float);
        void uv1(float, float);
    };
};