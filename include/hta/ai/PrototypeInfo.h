#pragma once
#include "hta/CStr.h"
#include "m3d/Object.h"

namespace ai
{

    struct PrototypeInfo {
        /* Size=0x40 */
        /* 0x0004 */ CStr m_className;
        /* 0x0010 */ CStr m_prototypeName;
        /* 0x001c */ int32_t m_prototypeId;
        /* 0x0020 */ int32_t m_resourceId;
        /* 0x0024 */ bool m_bIsUpdating;
        /* 0x0025 */ bool m_bVisibleInEncyclopedia;
        /* 0x0026 */ bool m_bApplyAffixes;
        /* 0x0028 */ uint32_t m_price;
        /* 0x002c */ bool m_bIsAbstract;
        /* 0x0030 */ CStr m_parentPrototypeName;
        /* 0x003c */ m3d::Class* m_protoClassObject;
    };
    static_assert(sizeof(PrototypeInfo) == 0x40);
}
