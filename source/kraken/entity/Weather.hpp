#pragma once
#ifndef KRAKEN_ENTITY_WEATHER_HPP
#define KRAKEN_ENTITY_WEATHER_HPP

#include "common/string.hpp"
#include "entity/Entity.hpp"
#include "assets/Weather.hpp"

namespace kraken::entity {
    struct Weather : Entity {
    public:
        Ref<assets::WeatherSet>  mSet     {       };
        const assets::Weather*   mA       { nullptr };
        const assets::Weather*   mB       { nullptr };
        assets::Weather          mCurrent {       };
        float                    mFactor  { 0.0f  };
    public:
        void Blend(const String& type, int hh, int mm);
        void Switch(const String& type, int hh, int mm);
        void Activate();
    public:
        virtual void OnUpdate(float delta);
    };
};

#endif