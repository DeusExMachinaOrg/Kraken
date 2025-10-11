#include "fix/fastloading.hpp"
#include "hta/m3d/Application.h"
#include "routines.hpp"
#include "config.hpp"

using PresentScene_t = int (__thiscall *)(m3d::rend::IRenderer *); 

namespace kraken::fix::fastloading
{
    static int limit;
    void SkipLoadingPresentScene()
    {
        static int counter = 0;
        counter++;
        if (counter < limit)
            return;

        counter = 0;

        m3d::rend::IRenderer* renderer = m3d::Application::Instance->Renderer;
        void** vtable = *(void***)renderer;
        PresentScene_t presentScene = *reinterpret_cast<PresentScene_t*>(
            reinterpret_cast<std::uintptr_t*>(vtable) + (0x378/sizeof(void*))
        );
        presentScene(renderer);
    }

    void Apply()
    {
        const kraken::Config& config = kraken::Config::Get();
        if (config.fastloading_enable.value == 0)
            return;
        limit = config.fastloading_speed.value;
        routines::ReplaceCall((void*)0x004C8BBE, SkipLoadingPresentScene);
    }
}