#include "fix/posteffectreload.hpp"
#include "hta/m3d/CClient.h"
#include "routines.hpp"
#include "config.hpp"

void UpdateWeather_Hook();

namespace kraken::fix::posteffectreload {
    void UpdateWeather()
    {
        m3d::CClient::Instance->m_world->m_weatherManager.UpdateDayTime();
    }

    void Apply(const Config* config)
    {
        if (!config->posteffectreload.value)
            return;

        kraken::routines::Redirect(9, (void*)0x0041F433, (void*)&UpdateWeather_Hook);
    }
}

void __declspec(naked) UpdateWeather_Hook()
{
    __asm {

        pushad;
        call kraken::fix::posteffectreload::UpdateWeather;
        popad;

        add esp, 0x2C0;

        retn 8;
    }
}