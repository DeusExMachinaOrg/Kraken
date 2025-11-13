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

        hta::m3d::rend::IRenderer* renderer = hta::m3d::Application::Instance->Renderer;
        void** vtable = *(void***)renderer;
        PresentScene_t presentScene = *reinterpret_cast<PresentScene_t*>(
            reinterpret_cast<std::uintptr_t*>(vtable) + (0x378/sizeof(void*))
        );
        presentScene(renderer);
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        const kraken::Config& config = kraken::Config::Get();
        limit = config.show_load_every.value;
        routines::ReplaceCall((void*)0x004C8BBE, SkipLoadingPresentScene);
    }
}