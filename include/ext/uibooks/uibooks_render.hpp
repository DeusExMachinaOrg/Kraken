#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ext/uibooks/uibooks_state.hpp"

#include "hta/m3d/ui/DrawInfo.hpp"
#include "hta/m3d/ui/GfxServer.hpp"

namespace kraken::ext::uibooks::render {
    // WrappedSegment / WrappedRow live in uibooks_state.hpp (BookState stores the
    // cached wrapped layout).
    std::vector<WrappedRow> WrapStyledLine(hta::m3d::ui::GfxServer* gfx,
                                            BookState& state, const ParsedLine& line,
                                            float wrapWidth);
    void DrawBook(BookState& state, const hta::m3d::ui::DrawInfo& drawInfo);
}
