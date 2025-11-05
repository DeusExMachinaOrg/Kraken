#include "hta/m3d/WeatherManager.h"

namespace m3d {
    int32_t WeatherManager::UpdateDayTime() {
        FUNC(0x00660060, int32_t, __thiscall, _UpdateDayTime, WeatherManager*);
        return _UpdateDayTime(this);
    }

    int32_t WeatherManager::ReadFromXmlFile(const char* name)
    {
        FUNC(0x00660300, int32_t, __thiscall, _ReadFromXmlFile, WeatherManager*, const char*);
        return _ReadFromXmlFile(this, name);
    }

    void WeatherManager::SetActiveWeather(unsigned int Cur)
    {
        FUNC(0x0065ED70, void, __thiscall, _SetActiveWeather, WeatherManager*, unsigned int);
        _SetActiveWeather(this, Cur);
    }
};