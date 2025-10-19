#pragma once

#include <stdint.h>

namespace m3d::rend {
    struct IRenderResource {
        /* Size=0x8 */
        /* 0x0004 */ int32_t m_refCount;
    
        IRenderResource(const IRenderResource&);
        IRenderResource();
        virtual int32_t AddRef();
        virtual int32_t Release();
        virtual int32_t GetRefCount();
        virtual bool IsValid() const;
        virtual ~IRenderResource();
    };
};