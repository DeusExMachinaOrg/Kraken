#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ext/uibooks/uibooks_state.hpp"

#include "hta/m3d/ui/DrawInfo.hpp"
#include "hta/m3d/ui/GfxServer.hpp"

namespace kraken::ext::uibooks::render {
    struct WrappedSegment {
        std::string text;
        uint32_t style = 0;
        uint32_t color = 0;
        bool hasColor = false;
        float width = 0.0f;
    };

    struct WrappedRow {
        std::vector<WrappedSegment> segments;
        float width = 0.0f;
    };

    std::vector<WrappedRow> WrapStyledLine(hta::m3d::ui::GfxServer* gfx,
                                            BookState& state, const ParsedLine& line,
                                            float wrapWidth);
    void DrawBook(BookState& state, const hta::m3d::ui::DrawInfo& drawInfo);
}
