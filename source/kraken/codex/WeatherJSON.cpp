#include "codex/WeatherJSON.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace kraken::codex {
    static const String sExts[] = { ".json" };

    std::span<const String> WeatherJSON::Exts() const {
        return { sExts };
    }

    static uint32_t ParseColor(const json& val) {
        uint32_t r = val[0].get<uint32_t>();
        uint32_t g = val[1].get<uint32_t>();
        uint32_t b = val[2].get<uint32_t>();
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    static assets::Weather ParseKey(const json& obj) {
        assets::Weather w;
        if (obj.contains("lightSun"))      w.mLightSun      = ParseColor(obj["lightSun"]);
        if (obj.contains("lightSky"))      w.mLightSky      = ParseColor(obj["lightSky"]);
        if (obj.contains("lightAmbient"))  w.mLightAmbient  = ParseColor(obj["lightAmbient"]);
        if (obj.contains("lightSpecular")) w.mLightSpecular = ParseColor(obj["lightSpecular"]);
        if (obj.contains("fogColor"))      w.mFogColor      = ParseColor(obj["fogColor"]);
        if (obj.contains("fogStart"))      w.mFogStart      = obj["fogStart"].get<float>();
        if (obj.contains("fogEnd"))        w.mFogEnd        = obj["fogEnd"].get<float>();
        if (obj.contains("shadowFactor"))  w.mShadowFactor  = obj["shadowFactor"].get<float>();
        if (obj.contains("waveSizeA"))     w.mWaveSizeA     = obj["waveSizeA"].get<float>();
        if (obj.contains("waveDepthA"))    w.mWaveDepthA    = obj["waveDepthA"].get<float>();
        if (obj.contains("waveSizeB"))     w.mWaveSizeB     = obj["waveSizeB"].get<float>();
        if (obj.contains("waveDepthB"))    w.mWaveDepthB    = obj["waveDepthB"].get<float>();
        if (obj.contains("waveSpeed"))     w.mWaveSpeed     = obj["waveSpeed"].get<float>();
        if (obj.contains("cloudSpeed"))    w.mCloudSpeed    = obj["cloudSpeed"].get<float>();
        return w;
    }

    assets::Asset* WeatherJSON::Load(const String& path) {
        std::ifstream file(static_cast<const char*>(path));
        if (!file.is_open())
            return nullptr;

        json root;
        file >> root;

        auto* set = new assets::WeatherSet();
        set->mLabel = path;

        for (auto& [key, keyObj] : root.items()) {
            set->mKeys[String(key.c_str())] = ParseKey(keyObj);
        }

        return set;
    }

    static json ColorToJson(uint32_t color) {
        return json::array({
            (color >> 16) & 0xFF,
            (color >>  8) & 0xFF,
            (color      ) & 0xFF
        });
    }

    static json KeyToJson(const assets::Weather& w) {
        json obj;
        obj["lightSun"]      = ColorToJson(w.mLightSun);
        obj["lightSky"]      = ColorToJson(w.mLightSky);
        obj["lightAmbient"]  = ColorToJson(w.mLightAmbient);
        obj["lightSpecular"] = ColorToJson(w.mLightSpecular);
        obj["fogColor"]      = ColorToJson(w.mFogColor);
        obj["fogStart"]      = w.mFogStart;
        obj["fogEnd"]        = w.mFogEnd;
        obj["shadowFactor"]  = w.mShadowFactor;
        obj["waveSizeA"]     = w.mWaveSizeA;
        obj["waveDepthA"]    = w.mWaveDepthA;
        obj["waveSizeB"]     = w.mWaveSizeB;
        obj["waveDepthB"]    = w.mWaveDepthB;
        obj["waveSpeed"]     = w.mWaveSpeed;
        obj["cloudSpeed"]    = w.mCloudSpeed;
        return obj;
    }

    void WeatherJSON::Save(const String& path, const assets::Asset& asset) {
        auto& set = static_cast<const assets::WeatherSet&>(asset);

        json root;
        for (auto& [key, weather] : set.mKeys) {
            root[static_cast<const char*>(key)] = KeyToJson(weather);
        }

        std::ofstream file(static_cast<const char*>(path));
        file << root.dump(2);
    }
}