#define LOGGER "fastloading"

#include "logger/logger.hpp"
#include "fixes/fastloading.hpp"
#include "common/routines.hpp"
#include "config/config.hpp"

#include "hta/m3d/Application.hpp"
#include "hta/m3d/rend/IRenderer.hpp"

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