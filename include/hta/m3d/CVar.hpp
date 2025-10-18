#pragma once
#include "utils.hpp"
#include "hta/CStr.h"


namespace m3d {
    struct IConHandler;

    struct CVar {
        enum eFlags : int32_t {
            CVAR_ARCHIVE = 0x0001,
            CVAR_READONLY = 0x0002,
            CVAR_DEBUG = 0x0004,
        };
        enum eType : int32_t {
        CVAR_UNDEFINED = 0x0000,
        CVAR_INT = 0x0001,
        CVAR_FLOAT = 0x0002,
        CVAR_BOOL = 0x0003,
        CVAR_STRING = 0x0004,
        CVAR_COLOR = 0x0005,
        };

        /* Size=0x2c */
        /* 0x0000 */ CStr m_name;
        /* 0x000c */ eType m_type;
        /* 0x0010 */ eFlags m_flags;
        union {
            /* 0x0014 */ int32_t m_i;
            /* 0x0014 */ uint32_t m_color;
            /* 0x0014 */ float m_f;
            /* 0x0014 */ bool m_b;
        };
        /* 0x0018 */ CStr m_s;
        /* 0x0024 */ char* m_defaultValue;
        /* 0x0028 */ IConHandler* m_handler;
    
        CVar(const CVar&);
        CVar(const char*, const char*, eType, eFlags);
        CVar();
        ~CVar();
        void Init(const char*, const char*, eType, eFlags);
        int32_t GetI() const;
        float GetF() const;
        bool GetB() const;
        const char* GetS() const;
        uint32_t GetC() const;
        void Set(const char*, bool);
        void SetI(int32_t, bool);
        void SetF(float, bool);
        void SetB(bool, bool);
        void SetC(uint32_t, bool);
        eType GetType() const;
        eFlags GetFlags() const;
        const char* GetDefault() const;
        void ResetToDefault();
        void SetHandler(IConHandler*);
        IConHandler* GetHandler();
        const char* GetName() const;
        bool operator==(const CVar&) const;
    };
};