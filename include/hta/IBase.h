#pragma once

struct IBase
{
    virtual ~IBase() = default;
    virtual int DecRef() = 0 /* 0x04 */;
    virtual int IncRef() = 0 /* 0x08 */;
    virtual void* QueryIface(const char*) = 0 /* 0x0c */;
};