#pragma once

#include <cstdint>

namespace kraken::ext::uibooks::resources {
    uint32_t LoadTexture(const char* path);
    void ReleaseTexture(uint32_t* rawHandle);
    bool ReadImageDimensions(const char* path, float* width, float* height);
}
