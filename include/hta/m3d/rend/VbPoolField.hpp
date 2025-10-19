#pragma once
#include "hta/m3d/rend/Common.hpp"
#include "hta/m3d/rend/VbHandle.hpp"

#include <stdint.h>

namespace m3d::rend {
    struct VbPoolField {
        /* Size=0x14 */
        /* 0x0000 */ uint32_t Offset;
        /* 0x0004 */ uint32_t Size;
        /* 0x0008 */ uint32_t RealOffset;
        /* 0x000c */ VertexType VertType;
        /* 0x0010 */ VbHandle Vb;
        
        uint32_t GetOffset() const;
        uint32_t GetSize() const;
        VertexType GetVertexType() const;
        VbHandle GetVbHandle() const;
        VbPoolField(const VbPoolField&);
        VbPoolField();
    };
};