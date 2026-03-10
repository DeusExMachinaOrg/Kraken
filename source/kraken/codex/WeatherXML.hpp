#pragma once
#ifndef KRAKEN_CODEX_WEATHER_XML_HPP
#define KRAKEN_CODEX_WEATHER_XML_HPP

#include "assets/common.hpp"
#include "assets/weather.hpp"

namespace kraken::codex {
    struct WeatherXML : public assets::Codec {
        std::span<const String> Exts() const override;
        assets::Asset* Load(const String& path) override;
        void Save(const String& path, const assets::Asset& asset) override;
    };
};

#endif
