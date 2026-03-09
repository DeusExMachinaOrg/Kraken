#define LOGGER "memory"

#include "logger/logger.hpp"
#include "fixes/memory.hpp"
#include "common/routines.hpp"
#include "memory/memory.hpp"

namespace kraken::fix::memory {
    // Proxy for AllocateMemory(uint size, const char* file, int line)
    void* __fastcall ProxyAllocate(unsigned int size, const char*, int) {
        return kraken::memory::Memory::Instance().Malloc(size);
    }

    // Proxy for ReallocateMemory(void* ptr, uint size, const char* file, int line)
    void* __fastcall ProxyReallocate(void* ptr, unsigned int size, const char*, int) {
        return kraken::memory::Memory::Instance().Realloc(ptr, size);
    }

    // Proxy for FreeMemory(void* ptr, const char* file, int line)
    void __fastcall ProxyFree(void* ptr, const char*, int) {
        kraken::memory::Memory::Instance().Free(ptr);
    }

    void __fastcall CheckNodeValidity(void*, void*) {
    };

    void Apply() {
        LOG_INFO("Feature enabled");

        // Redirect AllocateMemory/ReallocateMemory/FreeMemory
        routines::Redirect(0x020, (void*)0x00589410, (void*)&ProxyAllocate);
        routines::Redirect(0x020, (void*)0x00589440, (void*)&ProxyReallocate);
        routines::Redirect(0x010, (void*)0x00589430, (void*)&ProxyFree);
        routines::Redirect(0x490, (void*)0x006368C0, (void*)&CheckNodeValidity);
    };
};
