#pragma once

#include "utils.hpp"
#include "hta/CStr.h"

namespace m3d {
    template<typename T>
    class CStrHash {
        /* Size=0xc */
        /* 0x0000 */ stable_size_map<CStr,T> m_hash;
    
        ~CStrHash();
        void clear();
        void add(const CStr&, T);
        bool get(const CStr&, T&) const;
        void remove(const CStr&);
        CStrHash(const CStrHash<T>&);
        CStrHash();
    };
}