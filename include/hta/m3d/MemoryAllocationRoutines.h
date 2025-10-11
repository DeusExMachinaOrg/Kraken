#pragma once

namespace m3d {
    struct MemoryAllocationRoutines // sizeof=0xC
    {                                       // XREF: m3d::Kernel/r
        void *(__fastcall *AllocMem)(unsigned int, const char *, int);
        void *(__fastcall *ReallocMem)(void *, unsigned int, const char *, int);
        void (__fastcall *FreeMem)(void *, const char *, int);
    };
}