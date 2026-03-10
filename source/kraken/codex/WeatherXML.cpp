#include "codex/WeatherXML.hpp"

#include <cstdio>
#include <string>
#include <pugixml.hpp>

namespace kraken::codex {
    static const String sExts[] = { ".xml" };

    std::span<const String> WeatherXML::Exts() const {
        return { sExts };
    }

    static const char* sTodNames[] = { "sunrise", "day", "sunset", "night" };
    static const char* sTodAttrs[] = { "sunriseColor", "dayColor", "sunsetColor", "nightColor" };

    static uint32_t ParseColor(const char* str) {
        float r = 0, g = 0, b = 0;
        std::sscanf(str, "%f %f %f", &r, &g, &b);
        uint32_t ri = static_cast<uint32_t>(r) & 0xFF;
        uint32_t gi = static_cast<uint32_t>(g) & 0xFF;
        uint32_t bi = static_cast<uint32_t>(b) & 0xFF;
        return 0xFF000000u | (ri << 16) | (gi << 8) | bi;
    }

    static String TimeToKey(const char* name, float time) {
        int hh = static_cast<int>(time);
        int mm = static_cast<int>((time - hh) * 60.0f + 0.5f);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s_%02d:%02d", name, hh, mm);
        return String(buf);
    }

    static pugi::xml_node FindWeatherItem(const pugi::xml_document& doc, const char* name) {
        for (auto item : doc.child("Weather").children("WeatherItem")) {
            if (std::strcmp(item.attribute("name").as_string(), name) == 0)
                return item;
        }
        return {};
    }

    assets::Asset* WeatherXML::Load(const String& path) {
        // Load weatherdetail.xml (entry point)
        pugi::xml_document detail;
        if (!detail.load_file(static_cast<const char*>(path)))
            return nullptr;

        // Global weather definitions
        pugi::xml_document global;
        if (!global.load_file("data/weather.xml"))
            return nullptr;

        // Read TOD times from WeatherCommonSets
        auto common = global.child("Weather").child("WeatherCommonSets");
        float todTimes[4] = {
            common.attribute("sunriseTime").as_float(5.5f),
            common.attribute("dayTime").as_float(11.5f),
            common.attribute("sunsetTime").as_float(19.5f),
            common.attribute("nightTime").as_float(23.5f),
        };

        auto* set = new assets::WeatherSet();

        // For each WeatherItem in weatherdetail
        for (auto detailItem : detail.child("Weather").children("WeatherItem")) {
            const char* name = detailItem.attribute("name").as_string();

            // Find matching item in global weather.xml
            auto globalItem = FindWeatherItem(global, name);
            if (globalItem.empty())
                continue;

            // Build 4 keyframes (one per TOD)
            for (int i = 0; i < 4; ++i) {
                assets::Weather w;

                // Colors from global
                auto sun      = globalItem.child("Sun");
                auto sky      = globalItem.child("Sky");
                auto ambient  = globalItem.child("Ambient");
                auto specular = globalItem.child("Specular");
                auto fog      = globalItem.child("Fog");
                auto shadow   = globalItem.child("ShadowTransparency");
                auto clouds   = globalItem.child("cloudsSpeed");

                if (sun)      w.mLightSun      = ParseColor(sun.attribute(sTodAttrs[i]).as_string());
                if (sky)      w.mLightSky      = ParseColor(sky.attribute(sTodAttrs[i]).as_string());
                if (ambient)  w.mLightAmbient  = ParseColor(ambient.attribute(sTodAttrs[i]).as_string());
                if (specular) w.mLightSpecular = ParseColor(specular.attribute(sTodAttrs[i]).as_string());
                if (fog)      w.mFogColor      = ParseColor(fog.attribute(sTodAttrs[i]).as_string());
                if (shadow)   w.mShadowFactor  = shadow.attribute(sTodNames[i]).as_float(1.0f);
                if (clouds)   w.mCloudSpeed    = clouds.attribute(sTodNames[i]).as_float(1.0f);

                // Water from global item attributes
                w.mWaveSizeA  = globalItem.attribute("waterWaveSizeBig").as_float(1.0f);
                w.mWaveDepthA = globalItem.attribute("waterWaveHBig").as_float(1.0f);
                w.mWaveSizeB  = globalItem.attribute("waterWaveSizeSmall").as_float(1.0f);
                w.mWaveDepthB = globalItem.attribute("waterWaveHSmall").as_float(1.0f);
                w.mWaveSpeed  = globalItem.attribute("waterSpeed").as_float(1.0f);

                // Textures from weatherdetail
                for (auto ct : detailItem.children("ColorType")) {
                    if (std::strcmp(ct.attribute("name").as_string(), sTodNames[i]) == 0) {
                        // TODO: load CloudsTexture via assets::textures
                        break;
                    }
                }

                String key = TimeToKey(name, todTimes[i]);
                set->mKeys[key] = w;
            }
        }

        // Label from first WeatherItem name or path
        auto firstItem = detail.child("Weather").child("WeatherItem");
        if (firstItem)
            set->mLabel = firstItem.attribute("name").as_string();

        return set;
    }

    void WeatherXML::Save(const String& path, const assets::Asset& asset) {
        // XML save not supported (use JSON for new data)
    }
}
