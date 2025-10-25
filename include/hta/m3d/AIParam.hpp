#pragma once

#include "utils.hpp"
#include "hta/CStr.h"
#include "hta/Quaternion.h"
#include "hta/CVector.h"
#include "hta/CVector2.hpp"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace m3d {
    enum eAIParamType : int32_t {
        AIPARAM_UNDEFINE = 0x0000,
        AIPARAM_VECTOR = 0x0001,
        AIPARAM_QUATERNION = 0x0002,
        AIPARAM_ID = 0x0003,
        AIPARAM_FLOAT = 0x0004,
        AIPARAM_STRING = 0x0005,
        AIPARAM_ID_LIST = 0x0006,
        AIPARAM_STRING_LIST = 0x0007,
        AIPARAM_RANGE = 0x0008,
    };


    struct AIParam { /* Size=0x1c */
        union {
            struct {
            /* 0x0000 */ float x;
            /* 0x0004 */ float y;
            /* 0x0008 */ float z;
            /* 0x000c */ float w;
            };
            /* 0x0000 */ int32_t id;
            /* 0x0000 */ float Value;
            /* 0x0000 */ stable_size_vector<CStr>* m_NameList;
            /* 0x0000 */ stable_size_vector<int>* m_NumList;
            /* 0x0000 */ CStr* m_Str;
        };
        /* 0x0010 */ eAIParamType Type;
        /* 0x0014 */ CStr (* NameFromNum)(const AIParam*, int32_t);
        /* 0x0018 */ int32_t (* NumFromName)(const AIParam*, CStr&);
        
        void Detach();
        void Copy(const AIParam&);
        void ConvertFromString(void*, eAIParamType) const;
        void ReadFromString(const CStr&);
        void Clear();
        ~AIParam() = default;
        AIParam(const float&);
        AIParam(const Quaternion&);
        AIParam(const CVector&);
        AIParam(const CVector2&);
        AIParam(const int32_t&);
        AIParam(const uint32_t&);
        AIParam(const CStr&);
        AIParam() = default;
        AIParam(const AIParam&);
        AIParam(const stable_size_vector<int>&);
        bool operator>(const AIParam&);
        bool operator<(const AIParam&);
        bool operator>=(const AIParam&);
        bool operator<=(const AIParam&);
        bool operator==(const AIParam&);
        bool operator!=(const AIParam&);
        void Init();
        AIParam& operator=(const float&);
        AIParam& operator=(const Quaternion&);
        AIParam& operator=(const CVector&);
        AIParam& operator=(const CVector2&);
        AIParam& operator=(const int32_t&);
        AIParam& operator=(const uint32_t&);
        AIParam& operator=(const CStr&);
        AIParam& operator=(const stable_size_vector<CStr>&);
        AIParam& operator=(const stable_size_vector<int>&);
        CVector GetAsVector() const;
        Quaternion GetAsQuaternion() const;
        CVector2 GetAsRange() const;
        int32_t GetAsID() const;
        float GetAsFloat() const;
        stable_size_vector<int> GetAsIdList() const;
        stable_size_vector<CStr> GetAsStringList() const;
        void SetType(eAIParamType);
        eAIParamType GetType() const;
        CStr ToStr() const;
        CStr GetAsStr() const;
        void LoadFromXML(cmn::XmlFile*, const cmn::XmlNode*);
        void SaveToXML(cmn::XmlFile*, cmn::XmlNode*) const;
    
        static int32_t CompareInt(const void*, const void*);
        static int32_t CompareStr(const void*, const void*);
        static int32_t CompareRepEl(const void*, const void*);
    };
};