#pragma once

#include <cstdint>

namespace kraken::ext::uibooks::constants {
    // The game UI text resources are encoded in Windows-1251.
    enum BookTextCodePage : uint32_t {
        BOOK_TEXT_CODE_PAGE_WINDOWS_1251 = 1251u,
    };

    // Engine UI text uses opaque ARGB; this is the canonical opaque white.
    inline constexpr uint32_t OpaqueWhiteTextColor = 0xFFFFFFFFu;

    // Defensive parser limit for image dimensions in book markup.
    inline constexpr uint32_t ImageDimensionLimit = 16384u;

    inline constexpr float MaxImageWidth = 480.0f;
    inline constexpr float MaxImageHeight = 220.0f;
    inline constexpr float DefaultImageWidth = 16.0f;
    inline constexpr float DefaultImageHeight = 9.0f;
    inline constexpr float FallbackImageWidth = 320.0f;
    inline constexpr float FallbackImageHeight = 180.0f;

    // Extra pass spacing used when the native engine cannot provide a styled font.
    inline constexpr float SyntheticBoldOffset = 0.75f;
}
