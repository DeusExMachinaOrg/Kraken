#pragma once

#include "utils.hpp"
#include "hta/CStr.h"

namespace m3d
{
    enum eAIParamType : __int32
    {
        AIPARAM_VECTOR      = 0x1,
        AIPARAM_QUATERNION  = 0x2,
        AIPARAM_ID          = 0x3,
        AIPARAM_FLOAT       = 0x4,
        AIPARAM_STRING      = 0x5,
        AIPARAM_ID_LIST     = 0x6,
        AIPARAM_STRING_LIST = 0x7,
        AIPARAM_RANGE       = 0x8,
    };
    union ParamValue
    {
        float x;
        int id;
        float Value;
        stable_size_vector<CStr>* m_NameList;
        stable_size_vector<int>* m_NumList;
        CStr* m_Str;
    };
    struct AIParam
    {
        ParamValue value;
        float y;
        float z;
        float w;
        eAIParamType Type;
        CStr *(__fastcall *NameFromNum)(CStr *result, const m3d::AIParam *, int);
        int (__fastcall *NumFromName)(const m3d::AIParam *, CStr *);
    };
}