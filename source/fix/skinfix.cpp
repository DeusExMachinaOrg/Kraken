#define LOGGER "skinfix"

#include "ext/logger.hpp"
#include "fix/skinfix.hpp"
#include "routines.hpp"

#include "hta/ai/PhysicBody.hpp"
#include "hta/ai/PhysicObj.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/Enums.hpp"

namespace kraken::fix::skinfix
{
    void __fastcall SetSkinFixed(hta::ai::PhysicBody* physicBody, int, int skin)
    {
        hta::ai::PhysicObj* owner = physicBody->m_ownerPhysicObj;
        if (owner)
        {
            owner->m_skinNumber = skin;
        }

        if (physicBody->m_Node)
        {
            physicBody->m_Node->SetProperty(hta::PROP_DM_SKIN, &skin);
        }

        if (physicBody->IsKindOf((hta::m3d::Class*)0x00A02354 /*TODO*/))
        {
            hta::ai::Gun* gun = (hta::ai::Gun*)physicBody;
            if (gun->m_barrelNode)
            {
                gun->m_barrelNode->SetProperty(hta::PROP_DM_SKIN, &skin);
            }
        }
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        routines::Redirect(0x0044, (void*) 0x006165C0, (void*) &SetSkinFixed);
    }
}