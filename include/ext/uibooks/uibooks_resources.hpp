#pragma once

#include <cstdint>

namespace kraken::ext::uibooks::resources {
    uint32_t LoadTexture(const char* path);
    uint32_t LoadTextureByResourceId(const char* id);
    bool ReadTextureDimensions(uint32_t rawHandle, float* width, float* height);
    void ReleaseTexture(uint32_t* rawHandle);
    bool ReadImageDimensions(const char* path, float* width, float* height);
}
