#pragma once
#ifndef KRAKEN_ASSETS_TEXTURE_HPP
#define KRAKEN_ASSETS_TEXTURE_HPP

#include "common/string.hpp"
#include "assets/common.hpp"

#include "hta/m3d/rend/TexHandle.hpp"

namespace kraken::assets {
    class Texture : public Asset {
    public:
        hta::m3d::rend::TexHandle mHandle { };
    public:
        bool IsValid() const { return mHandle != -1; }
    public:
        Texture();
       ~Texture();
    };
};

#endif