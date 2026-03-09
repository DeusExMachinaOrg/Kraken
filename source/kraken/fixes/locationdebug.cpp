#define LOGGER "locationdebug"

#include "logger/logger.hpp"
#include "fixes/locationdebug.hpp"
#include "common/routines.hpp"

#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/Location.hpp"

void __fastcall RenderDebugInfoHook(hta::ai::DynamicScene* dynamicScene)
{
    dynamicScene->RenderDebugInfo();

    if (hta::m3d::Kernel::Instance()->m_engineConfig->m_ai_location_debug.GetB()) {
        for (hta::ai::Obj* obj : *hta::ai::CServer::Instance()->m_pObjects) {
            if (hta::ai::Location* location = obj->cast<hta::ai::Location>()) {
                location->RenderDebugInfo();
            }
        }
    }
}

namespace kraken::fix::locationdebug
{
    void Apply()
    {
        LOG_INFO("Feature enabled");
        routines::Redirect(0x5, (void*) 0x005C774E, RenderDebugInfoHook);
        routines::OverrideValue((void*) 0x0060AB7C, (uint32_t) 0x00A02819); // Render all quest icons on radar
    }
}