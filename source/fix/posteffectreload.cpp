#define LOGGER "posteffectreload"

#include "ext/logger.hpp"
#include "fix/posteffectreload.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "vc3/vector"

#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/EngineConfig.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/m3d/CClient.hpp"
#include "hta/m3d/WeatherManager.hpp"

void UpdateWeather_Hook();

const auto clearWeatherStorage = (void (__fastcall*)(::vc3::vector<hta::m3d::Weather*>*))(0x0065F1F0);

namespace kraken::fix::posteffectreload {
    void UpdateWeather()
    {
        hta::m3d::WeatherManager& weather_manager = hta::ai::CServer::Instance()->GetWorld()->GetWeatherManager();
        uint32_t current_weather_index = 0;

        if (!weather_manager.m_curWeatherStorage.empty() && weather_manager.m_currentWeather) {
            const hta::m3d::Weather* current_weather = weather_manager.m_currentWeather;
            for (auto iter = weather_manager.m_curWeatherStorage.begin(); iter != weather_manager.m_curWeatherStorage.end(); ++iter) {
                if (*iter == weather_manager.m_currentWeather) {
                    current_weather_index = iter - weather_manager.m_curWeatherStorage.begin();
                }
            }
        }

        clearWeatherStorage(&weather_manager.m_curWeatherStorage);

        const hta::m3d::EngineConfig& cfg = hta::m3d::Kernel::Instance()->GetEngineCfg();
        weather_manager.ReadFromXmlFile(cfg.m_weather_ConfigFile.m_s.m_charPtr);
        weather_manager.SetActiveWeather(current_weather_index);
        weather_manager.UpdateDayTime();
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