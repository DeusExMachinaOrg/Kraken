#define LOGGER "skinfix"

// ext
#include "ext/logger.hpp"

// fix
#include "fix/skinfix.hpp"

// hta/ai
#include "ai/PhysicBody.hpp"
#include "ai/PhysicObj.hpp"
#include "ai/Gun.hpp"

// hta
#include "hta/Enums.hpp"

// local
#include "routines.hpp"

namespace kraken::fix::skinfix
{
    void __fastcall SetSkinFixed(hta::ai::PhysicBody* physicBody, int, int skin)
    {
        hta::ai::PhysicObj* owner = physicBody->OwnerPhysicObj;
        if (owner)
        {
            owner->m_skinNumber = skin;
        }

        if (physicBody->Node)
        {
            physicBody->Node->SetProperty(PROP_DM_SKIN, &skin);
        }

        if (physicBody->IsKindOf((hta::m3d::Class*)0x00A02354 /*TODO*/))
        {
            hta::ai::Gun* gun = (hta::ai::Gun*)physicBody;
            if (gun->m_barrelNode)
            {
                gun->m_barrelNode->SetProperty(PROP_DM_SKIN, &skin);
            }
        }
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        routines::Redirect(0x0044, (void*) 0x006165C0, (void*) &SetSkinFixed);
    }
}