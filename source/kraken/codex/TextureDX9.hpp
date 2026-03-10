#pragma once
#ifndef KRAKEN_CODEX_TEXTURE_DX9_HPP
#define KRAKEN_CODEX_TEXTURE_DX9_HPP

#include "assets/common.hpp"
#include "assets/texture.hpp"

namespace kraken::codex {
    struct TextureDX9 : public assets::Codec {
        std::span<const String> Exts() const override;
        assets::Asset* Load(const String& path) override;
        void Save(const String& path, const assets::Asset& asset) override;
    };
};

#endif
