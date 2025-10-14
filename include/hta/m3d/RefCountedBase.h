#pragma once

namespace m3d
{
    struct RefCountedBase
    {
        RefCountedBase();
        virtual ~RefCountedBase() = default;
        int IncRef();
        int DecRef();
        int GetRefCount() const;

    private:
        int m_refCount = 0;
    };
}