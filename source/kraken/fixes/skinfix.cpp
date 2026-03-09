#define LOGGER "skinfix"

#include "logger/logger.hpp"
#include "common/routines.hpp"
#include "fixes/skinfix.hpp"

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

        hta::ai::Gun* gun = physicBody->cast<hta::ai::Gun>();
        if (gun && gun->m_barrelNode)
        {
            gun->m_barrelNode->SetProperty(hta::PROP_DM_SKIN, &skin);
        }
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        routines::Redirect(0x0044, (void*) 0x006165C0, (void*) &SetSkinFixed);
    }
}