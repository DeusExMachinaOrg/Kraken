#pragma once

#include <stdint.h>

namespace m3d::cmn {
    template<typename T>
    struct vector { /* Size=0xc */
        /* 0x0000 */ T* m_data;
        /* 0x0004 */ size_t m_numItems;
        /* 0x0008 */ size_t m_maxItems;
        
        vector();
        ~vector();
        void Allocate(size_t);
        void Deallocate();
        void clear();
        T& operator[](size_t);
        const T& operator[](size_t) const;
        void push_back(const T&);
        int32_t empty() const;
        int32_t size() const;
    };
};