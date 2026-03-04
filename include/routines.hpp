#ifndef __KRAKEN_ROUTINES_HPP__
#define __KRAKEN_ROUTINES_HPP__

#include "stdafx.hpp"

namespace kraken::routines {
    #pragma pack(push, 1)
    struct _Redirect {
        char   op;
        size_t to;
    };
    union _Call {
        struct { uint8_t  cmd; uintptr_t offset;   } direct;
        struct { uint16_t cmd; uintptr_t location; } indirect;
        struct { uint8_t marker[2];                };
    };
    #pragma pack(pop)

    // Replace function
    inline void Redirect(size_t size, void* src, void* tar) {
        DWORD protection;

        if (size < sizeof(_Redirect)) {
            throw std::runtime_error("Insuffient space to apply trampoline!");
        }

        VirtualProtect(src, size, PAGE_EXECUTE_READWRITE, &protection);
        memset(src, 0xCC, size);
        _Redirect* op = (_Redirect*) src;
        op->op = 0xE9;
        op->to = (size_t) tar - (size_t) src - 5;
        VirtualProtect(src, size, protection, &protection);
    };

    // Replace value
    inline void Override(size_t size, void* src, const char* data) {
        DWORD protection;

        VirtualProtect(src, size, PAGE_EXECUTE_READWRITE, &protection);
        memcpy(src, data, size);
        VirtualProtect(src, size, protection, &protection);
    };

    template <typename T>
    void OverrideValue(void* address, T value)
    {
        Override(sizeof(T), address, reinterpret_cast<char*>(&value));
    }

    // Remap pointer
    inline void RemapPtr(void* src, void* tar) {
        DWORD protection;
        size_t temp = (size_t) tar;

        VirtualProtect(src, sizeof(size_t), PAGE_EXECUTE_READWRITE, &protection);
        memcpy(src, &temp, sizeof(size_t));
        VirtualProtect(src, sizeof(size_t), protection, &protection);
    };

    // Change existing call to point to another function
    inline void ChangeCall(void* src, void* tar) {
        DWORD protection;

        VirtualProtect(src, sizeof(_Call), PAGE_EXECUTE_READWRITE, &protection);
        _Call* opcode = (_Call*)src;
        if (opcode->marker[0] == 0xE8) {
            opcode->direct.offset = reinterpret_cast<intptr_t>(tar) - reinterpret_cast<intptr_t>(src) - 5;
        }
        else if (opcode->marker[0] == 0xFF && opcode->marker[1] == 0x15) {
            opcode->indirect.location = reinterpret_cast<intptr_t>(tar);
        }
        else {
            throw std::runtime_error("Invalid operator!");
        }
        VirtualProtect(src, sizeof(_Call), protection, &protection);
    };

    // Create call instruction
    inline void ReplaceCall(void* src, void* tar)
    {
        DWORD protection;

        VirtualProtect(src, sizeof(_Call), PAGE_EXECUTE_READWRITE, &protection);

        unsigned char* p = reinterpret_cast<unsigned char*>(src);
        uintptr_t rel = reinterpret_cast<uintptr_t>(tar) - reinterpret_cast<uintptr_t>(src) - 5;

        p[0] = 0xE8;
        memcpy(p + 1, &rel, 4);
        p[5] = 0x90;

        VirtualProtect(src, sizeof(_Call), protection, &protection);
    }

    inline void Patch(void* address, const void* data, size_t size) {
        DWORD oldProtect;
        VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(address, data, size);
        VirtualProtect(address, size, oldProtect, &oldProtect);
    }

    inline void Nop(void* address, size_t size) {
        DWORD oldProtect;
        VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        memset(address, 0x90, size);
        VirtualProtect(address, size, oldProtect, &oldProtect);
    }
};

#endif