#pragma once

#include <stdint.h>

namespace m3d {
    struct Class;
    struct Object;
    struct ExportInfo;

    struct Class { /* Size=0x1c */
    /* 0x0000 */ public: const char* m_className;
    /* 0x0004 */ public: int32_t m_classSize;
    /* 0x0008 */ public: Object* (* m_fnCreateObject)();
    /* 0x000c */ public: Class* (* m_fnGetBaseClass)();
    /* 0x0010 */ public: int32_t m_index;
    /* 0x0014 */ public: m3d::ExportInfo* m_lExports;
    /* 0x0018 */ public: void* m_scriptHandle;
    };
};