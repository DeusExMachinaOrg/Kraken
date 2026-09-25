#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hta/m3d/Enums.hpp"

namespace kraken::ext::uibooks {
    struct ImageDimension {
        float pixels = 0.0f;
        bool specified = false;
        bool fill = false;
    };

    enum class ImageSource : uint8_t {
        Path,
        GameResource,
    };

    struct Seg {
        std::string text;
        uint32_t style = 0;
        uint32_t color = 0;
        bool hasColor = false;
    };

    struct ParsedLine {
        int32_t align = hta::m3d::TF_LEFT;
        std::vector<Seg> segs;
        bool isImage = false;
        std::string imageAlt;
        std::string imageReference;
        ImageSource imageSource = ImageSource::Path;
        ImageDimension imageWidth;
        ImageDimension imageHeight;
    };

    struct ParseResult {
        int32_t align = hta::m3d::TF_LEFT;
        uint32_t styleMask = 0;
        std::string clean;
        std::vector<ParsedLine> lines;
        std::vector<bool> lineBreak;
        int32_t styledSegs = 0;
        int32_t coloredSegs = 0;
        std::vector<uint32_t> colors;
        int32_t imageCount = 0;
    };

    ParseResult ParseBookText(const char* src);

}
