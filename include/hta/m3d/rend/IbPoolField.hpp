#pragma once
#include "hta/m3d/rend/IbHandle.hpp"

#include <stdint.h>

namespace m3d::rend {
    struct IbPoolField {
        /* Size=0x10 */
        /* 0x0000 */ public: uint32_t Offset;
        /* 0x0004 */ public: uint32_t Size;
        /* 0x0008 */ public: uint32_t RealOffset;
        /* 0x000c */ public: IbHandle Ib;
    
        uint32_t GetOffset() const;
        uint32_t GetSize() const;
        IbHandle GetIbHandle() const;
        IbPoolField(const IbPoolField&);
        IbPoolField();
    };
};