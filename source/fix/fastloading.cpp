#include "fix/fastloading.hpp"
#include "hta/m3d/Application.h"
#include "routines.hpp"
#include "config.hpp"

using PresentScene_t = int (__thiscall *)(m3d::rend::IRenderer *); 

namespace kraken::fix::fastloading
{
    static size_t limit;
    void SkipLoadingPresentScene()
    {
        static size_t counter = 0;

        if (counter++ % limit)
            return;

        m3d::rend::IRenderer* renderer = m3d::Application::Instance->m_renderer;
        void** vtable = *(void***)renderer;
        PresentScene_t presentScene = *reinterpret_cast<PresentScene_t*>(
            reinterpret_cast<std::uintptr_t*>(vtable) + (0x378/sizeof(void*))
        );
        presentScene(renderer);
    }

    void Apply()
    {
        const kraken::Config& config = kraken::Config::Get();
        limit = config.show_load_every.value;
        routines::ReplaceCall((void*)0x004C8BBE, SkipLoadingPresentScene);
    }
}