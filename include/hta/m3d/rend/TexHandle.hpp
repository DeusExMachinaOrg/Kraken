#pragma once
#include "hta/m3d/rend/Handle.hpp"

namespace m3d::rend {
    struct TexHandle : public Handle<TexHandle> { /* Size=0x4 */
    /* 0x0000: fields for Handle<TexHandle> */
    
        TexHandle(const TexHandle&);
        TexHandle();
    };
};