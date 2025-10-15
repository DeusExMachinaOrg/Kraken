#pragma once

#include <cstdint>
#include <array>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace m3d {
    struct DbgCounter;

    struct DbgCounter {
        enum eType : int32_t {
            DBG_COUNTER_STRING = 0x0000,
            DBG_COUNTER_INT = 0x0001,
            DBG_COUNTER_FLOAT = 0x0002,
            DBG_COUNTER_BOOL = 0x0003,
        };
        /* Size=0x40 */
        /* 0x0000 */ eType m_curType;
        union {
        /* 0x0004 */ int32_t m_i;
        /* 0x0004 */ float m_f;
        /* 0x0004 */ bool m_b;
        };
        /* 0x0008 */ std::basic_string<char,std::char_traits<char>,std::allocator<char> > m_s;
        /* 0x0024 */ std::basic_string<char,std::char_traits<char>,std::allocator<char> > m_name;
    
        void SetS(const char*);
        void SetI(int32_t);
        void SetF(float);
        void SetB(bool);
        void IncI();
        void DecI();
        const char* GetS() const;
        int32_t GetI() const;
        float GetF() const;
        bool GetB() const;
        DbgCounter::eType GetType() const;
        void Reset();
        const char* GetName() const;
        DbgCounter(const DbgCounter&);
        DbgCounter();
        ~DbgCounter();
        void SetName(const char*);
    };
};