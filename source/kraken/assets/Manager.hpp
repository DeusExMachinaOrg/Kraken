#pragma once
#ifndef KRAKEN_ASSETS_MANAGER_HPP
#define KRAKEN_ASSETS_MANAGER_HPP

#include "assets/common.hpp"
#include "assets/texture.hpp"
#include "assets/weather.hpp"

namespace kraken::assets {
    extern Group<Texture>    textures;
    extern Group<WeatherSet> weathers;

    void Initialize();
    void Shutdown();
    void Unload();
};

#endif