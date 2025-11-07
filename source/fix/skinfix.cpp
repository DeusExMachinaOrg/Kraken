#define LOGGER "skinfix"

// ext
#include "ext/logger.hpp"

// fix
#include "fix/skinfix.hpp"

// hta/ai
#include "hta/ai/PhysicBody.h"
#include "hta/ai/PhysicObj.h"
#include "hta/ai/Gun.hpp"

// hta
#include "hta/Enums.hpp"

// local
#include "routines.hpp"

namespace kraken::fix::skinfix
{
    void __fastcall SetSkinFixed(ai::PhysicBody* physicBody, int, int skin)
    {
        ai::PhysicObj* owner = physicBody->OwnerPhysicObj;
        if (owner)
        {
            owner->m_skinNumber = skin;
        }

        if (physicBody->Node)
        {
            physicBody->Node->SetProperty(PROP_DM_SKIN, &skin);
        }

        if (physicBody->IsKindOf((m3d::Class*)0x00A02354 /*TODO*/))
        {
            ai::Gun* gun = (ai::Gun*)physicBody;
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