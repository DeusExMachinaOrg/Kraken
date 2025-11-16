#define LOGGER "luabinds"

#include "fix/luabinds.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/CMiracle3d.hpp"
#include "hta/Impulse.hpp"
#include "hta/m3d/GameImpulse.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/ScriptServer.hpp"

namespace kraken::fix::luabinds {
    static std::vector<std::string> g_scripts;
    void ExecuteLuaScripts() {
        auto impulse = (hta::m3d::GameImpulse*)hta::CMiracle3d::Instance()->m_pImpulses;
        auto scriptServer = hta::m3d::Kernel::Instance()->m_scriptServer;

        for (int impId = hta::IM_DEBUG_0; impId <= hta::IM_DEBUG_9; impId++) {
            if (impulse->GetImpulseStateAndReset(impId)) {
                int scriptIndex = impId - hta::IM_DEBUG_0;
                if (scriptIndex < 0 || scriptIndex >= g_scripts.size())
                    continue;
                scriptServer->execute(g_scripts[scriptIndex].c_str(), "Kraken");
            }
        }
    }

    void __declspec(naked) LuaScripts_Hook() {
        __asm
        {
            pushad;
            call ExecuteLuaScripts;
            popad;

            ret 8;
        }
    }

    void Apply(const Config* config) {
        if (config->lua_enabled.value == 0)
            return;

        LOG_INFO("Feature enabled");
        g_scripts = config->lua_scripts.value;
        kraken::routines::Redirect(5, (void*)0x00401B37, (void*)&LuaScripts_Hook);
    }
}