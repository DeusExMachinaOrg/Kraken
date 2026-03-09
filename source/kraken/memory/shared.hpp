#pragma once
#ifndef KRAKEN_MEMORY_SHARED_HPP
#define KRAKEN_MEMORY_SHARED_HPP

#include <mutex>
#include <cstddef>
#include <cstring>
#include <cassert>

namespace kraken::memory {
    using CRT_MALLOC  = void* (__cdecl*)(size_t size);
    using CRT_REALLOC = void* (__cdecl*)(void* data, size_t size);
    using CRT_FREE    = void  (__cdecl*)(void* data);

    extern CRT_MALLOC  gMalloc;
    extern CRT_REALLOC gRealloc;
    extern CRT_FREE    gFree;
};

#endif
