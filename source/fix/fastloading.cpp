#define LOGGER "fastloading"

#include "ext/logger.hpp"
#include "fix/fastloading.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/m3d/Application.hpp"
#include "hta/m3d/rend/IRenderer.hpp"

using PresentScene_t = int (__thiscall *)(hta::m3d::rend::IRenderer *); 

namespace kraken::fix::fastloading
{
    static size_t limit;
    void SkipLoadingPresentScene()
    {
        static size_t counter = 0;

        if (counter++ % limit)
            return;

        hta::m3d::Application::Instance()->m_renderer->PresentScene();
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        const kraken::Config& config = kraken::Config::Instance();
        limit = config.show_load_every.value;
        routines::ReplaceCall((void*)0x004C8BBE, SkipLoadingPresentScene);
    }
}