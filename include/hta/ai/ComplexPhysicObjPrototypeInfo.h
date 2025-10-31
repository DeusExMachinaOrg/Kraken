#pragma once
#include "PhysicObjPrototypeInfo.h"
#include "ComplexPhysicObjPartDescription.h"
#include "hta/CVector.h"
#include "stdafx.hpp"
#include "ref_ptr.h"

namespace ai
{
    struct ComplexPhysicObjPrototypeInfo : PhysicObjPrototypeInfo {
        enum MassShapes : int32_t {
            MS_BOX = 0x0000,
            MS_SPHERE = 0x0001,
        };
        /* Size=0x90 */
        /* 0x0000: fields for PhysicObjPrototypeInfo */
        /* 0x0048 */ stable_size_map<CStr, int> m_partPrototypeIds;
        /* 0x0054 */ CVector m_massSize;
        /* 0x0060 */ CVector m_massTranslation;
        /* 0x006c */ ref_ptr<ComplexPhysicObjPartDescription> m_partDescription;
        /* 0x0070 */ stable_size_map<CStr, CStr> m_partPrototypeNames;
        /* 0x007c */ stable_size_vector<CStr> m_allPartNames;
        /* 0x008c */ ComplexPhysicObjPrototypeInfo::MassShapes m_massShape;
        /* Restored */

    };
    static_assert(sizeof(ComplexPhysicObjPrototypeInfo) == 0x90);
}

