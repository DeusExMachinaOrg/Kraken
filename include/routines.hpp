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

    inline void Override(size_t size, void* src, char* data) {
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

    inline void RemapPtr(void* src, void* tar) {
        DWORD protection;
        size_t temp = (size_t) tar;

        VirtualProtect(src, sizeof(size_t), PAGE_EXECUTE_READWRITE, &protection);
        memcpy(src, &temp, sizeof(size_t));
        VirtualProtect(src, sizeof(size_t), protection, &protection);
    };

    inline void MakeCall(void* src, void* tar) {
        DWORD protection;


        VirtualProtect(src, sizeof(_Call), PAGE_EXECUTE_READWRITE, &protection);
        _Call* opcode = (_Call*)src;
        if (opcode->marker[0] == 0xE8) {
            opcode->direct.offset = reinterpret_cast<intptr_t>(tar) - reinterpret_cast<intptr_t>(src) - 5;
        }
        else if (opcode->marker[0] == 0xFF && opcode->marker[1] == 0x15) {
            opcode->indirect.location = reinterpret_cast<intptr_t>(tar);
        }
        else if (opcode->marker[0] == 0xFF && opcode->marker[1] == 0x96 || opcode->marker[1] == 0x92) {
            unsigned char* p = reinterpret_cast<unsigned char*>(src);
            int32_t rel = static_cast<int32_t>((uintptr_t)tar - (uintptr_t)p - 5);

            p[0] = 0xE8;
            memcpy(p + 1, &rel, 4);
            p[5] = 0x90;
        }
        else {
            throw std::runtime_error("Invalid operator!");
        }
        VirtualProtect(src, sizeof(_Call), protection, &protection);
    };
};

#endif