#define LOGGER "skinfix"

#include "ext/logger.hpp"
#include "fix/skinfix.hpp"
#include "routines.hpp"

#include "render/CDevice.hpp"

#include "hta/m3d/Application.hpp"

namespace kraken::fix::render {
    bool __fastcall Application_createRenderer(hta::m3d::Application* self, void* _) {
        self->m_renderer = new kraken::render::CDevice();
        return true;
    };


    void Apply()
    {
        LOG_INFO("Feature enabled");
        routines::Redirect(0x0044, (void*) 0x0059E170, (void*) &Application_createRenderer);
    }
};