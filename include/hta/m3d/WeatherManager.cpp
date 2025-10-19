#include "hta/m3d/WeatherManager.h"

namespace m3d {
    int32_t WeatherManager::UpdateDayTime() {
        FUNC(0x00660060, int32_t, __thiscall, _UpdateDayTime, WeatherManager*);
        return _UpdateDayTime(this);
    }
};