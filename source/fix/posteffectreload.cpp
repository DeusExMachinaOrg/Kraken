#define LOGGER "posteffectreload"

#include "ext/logger.hpp"
#include "fix/posteffectreload.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/m3d/CClient.h"
#include "hta/m3d/WeatherManager.h"
#include "routines.hpp"
#include "config.hpp"

void UpdateWeather_Hook();

namespace kraken::fix::posteffectreload {
    void UpdateWeather()
    {
        ai::CServer::Instance()->GetWorld()->GetWeatherManager().UpdateDayTime();
    }

    void Apply(const Config* config)
    {
        if (!config->posteffectreload.value)
            return;

        LOG_INFO("Feature enabled");
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