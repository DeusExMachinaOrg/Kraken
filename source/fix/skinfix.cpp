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

        hta::ai::Gun* gun = physicBody->cast<hta::ai::Gun>();
        if (gun && gun->m_barrelNode)
        {
            gun->m_barrelNode->SetProperty(hta::PROP_DM_SKIN, &skin);
        }
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        // PhysicBody::SetSkin is 0x21 bytes at 0x006165C0; the next exported
        // method, GetSkin, starts at 0x006165F0.  The old 0x44-byte patch
        // crossed that boundary and filled GetSkin's entry with INT3.  A
        // direct relative jump is the complete detour and owns exactly five
        // bytes at the target entry.
        routines::Redirect(sizeof(routines::_Redirect),
                           reinterpret_cast<void*>(0x006165C0),
                           reinterpret_cast<void*>(&SetSkinFixed));
    }
}
