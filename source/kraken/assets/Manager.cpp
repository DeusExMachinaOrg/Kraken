#include "assets/Manager.hpp"
#include "codex/TextureDX9.hpp"
#include "codex/WeatherJSON.hpp"
#include "codex/WeatherXML.hpp"

namespace kraken::assets {
    Group<Texture>    textures;
    Group<WeatherSet> weathers;

    static codex::TextureDX9  sTextureDX9;
    static codex::WeatherJSON sWeatherJSON;
    static codex::WeatherXML  sWeatherXML;

    void Initialize() {
        textures.AttachCodec(&sTextureDX9);
        weathers.AttachCodec(&sWeatherJSON);
        weathers.AttachCodec(&sWeatherXML);
    }

    void Unload() {
        weathers.Unload();
        textures.Unload();
    }

    void Shutdown() {
        Unload();
        weathers.ResetCodec();
        textures.ResetCodec();
    }
}