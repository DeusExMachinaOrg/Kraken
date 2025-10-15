#include "fix/posteffectreload.hpp"
#include "hta/m3d/CClient.h"
#include "routines.hpp"
#include "config.hpp"
#include "hta/m3d/Kernel.h"
#include "hta/m3d/EngineConfig.h"

void UpdateWeather_Hook();

const auto clearWeatherStorage = (void (__fastcall*)(stable_size_vector<m3d::Weather*>*))(0x0065F1F0);

namespace kraken::fix::posteffectreload {
    static unsigned int current_weather_index = 0;
    void UpdateWeather()
    {
        m3d::EngineConfig& cfg = m3d::Kernel::g_Kernel->GetEngineCfg();

        m3d::WeatherManager& weather_manager = m3d::CClient::Instance->m_world->m_weatherManager;

        if (!weather_manager.m_curWeatherStorage.empty()) {
            m3d::Weather* current_weather = weather_manager.m_currentWeather;
            for (auto iter = weather_manager.m_curWeatherStorage.begin(); iter != weather_manager.m_curWeatherStorage.end(); ++iter) {
                if (*iter == weather_manager.m_currentWeather) {
                    current_weather_index = iter - weather_manager.m_curWeatherStorage.begin();
                }
            }
        }
        else {
            current_weather_index = 0;
        }

        clearWeatherStorage(&weather_manager.m_curWeatherStorage);

        m3d::CClient::Instance->m_world->m_weatherManager.ReadFromXmlFile(cfg.m_weather_ConfigFile.m_s.charPtr);
        m3d::CClient::Instance->m_world->m_weatherManager.UpdateDayTime();
    }

    void __fastcall SetActiveWeather(void* weatherManager, int, unsigned int Cur)
    {
		m3d::CClient::Instance->m_world->m_weatherManager.SetActiveWeather(current_weather_index);
	}

    void Apply(const Config* config)
    {
        if (!config->posteffectreload.value)
            return;

        kraken::routines::Redirect(9, (void*)0x0041F433, (void*)&UpdateWeather_Hook);
		kraken::routines::ChangeCall((void*)0x00661331, (void*)&SetActiveWeather);
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