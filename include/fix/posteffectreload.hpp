#pragma once
#include "config.hpp"

namespace kraken::fix::posteffectreload {
    void UpdateWeather();
    void Apply(const Config* config);
}