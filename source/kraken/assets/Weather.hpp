#pragma once
#ifndef KRAKEN_ASSETS_WEATHER_HPP
#define KRAKEN_ASSETS_WEATHER_HPP

#include <map>

#include "common/string.hpp"
#include "assets/common.hpp"
#include "assets/texture.hpp"

namespace kraken::assets {
    struct Weather {
        static void Lerp(Weather& out, const Weather& a, const Weather& b, float t);
    public:
        uint32_t                mLightSun      { 0xFFFFFFFFu };
        uint32_t                mLightSky      { 0xFFFFFFFFu };
        uint32_t                mLightAmbient  { 0xFFFFFFFFu };
        uint32_t                mLightSpecular { 0xFFFFFFFFu };
        uint32_t                mFogColor      { 0xFFFFFFFFu };
        float                   mFogStart      { 200.0f      };
        float                   mFogEnd        { 400.0f      };
        float                   mShadowFactor  { 1.0f        };
        float                   mWaveSizeA     { 1.0f        };
        float                   mWaveDepthA    { 1.0f        };
        float                   mWaveSizeB     { 1.0f        };
        float                   mWaveDepthB    { 1.0f        };
        float                   mWaveSpeed     { 1.0f        };
        float                   mCloudSpeed    { 1.0f        };
        Ref<Texture>            mCloudTexture  {             };
    };

    struct WeatherSet : public Asset {
        std::map<String, Weather> mKeys;

        void Sample(Weather& out, const String& key) const;
    };
};

#endif