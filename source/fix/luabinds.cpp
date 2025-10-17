#include "fix/luabinds.hpp"
#include "fix/impulse.hpp"
#include "hta/m3d/GameImpulse.h"
#include "hta/m3d/Kernel.h"
#include "hta/m3d/CMiracle3d.h"
#include "routines.hpp"

namespace kraken::fix::luabinds {
    static std::vector<std::string> g_scripts;
    void ExecuteLuaScripts()
    {
        auto impulse = (m3d::GameImpulse*)CMiracle3d::Instance->m_pImpulses;
        auto scriptServer = m3d::Kernel::g_Kernel->m_scriptServer;

        for (int impId = IM_DEBUG_0; impId <= IM_DEBUG_9; impId++) {
            if (impulse->GetImpulseStateAndReset(impId)) {
                int scriptIndex = impId - IM_DEBUG_0;
                if (scriptIndex < 0 || scriptIndex >= g_scripts.size())
                    continue;
                scriptServer->Execute(g_scripts[scriptIndex].c_str(), "Kraken");
            }
        }
    }

    void __declspec(naked) LuaScripts_Hook()
    {
        __asm
        {
            pushad;
            call ExecuteLuaScripts;
            popad;

            ret 8;
        }
    }

    void Apply(const Config* config)
    {
        if (config->lua_enabled.value == 0)
            return;

        g_scripts = config->lua_scripts.value;
        kraken::routines::Redirect(5, (void*)0x00401B37, (void*)&LuaScripts_Hook);
    }
}