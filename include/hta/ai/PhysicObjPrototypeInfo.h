#pragma once
#include "PrototypeInfo.h"

namespace ai
{
    struct PhysicObjPrototypeInfo : PrototypeInfo {
        /* Size=0x48 */
        /* 0x0000: fields for PrototypeInfo */
        /* 0x0040 */ float m_intersectionRadius;
        /* 0x0044 */ float m_lookRadius;

        PhysicObjPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual ~PhysicObjPrototypeInfo();
    };
    static_assert(sizeof(PhysicObjPrototypeInfo) == 0x48);
}