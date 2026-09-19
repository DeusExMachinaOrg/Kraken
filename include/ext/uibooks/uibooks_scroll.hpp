#pragma once

#include <cstdint>

#include "ext/uibooks/uibooks_state.hpp"

#include "hta/m3d/ui/DrawInfo.hpp"

namespace kraken::ext::uibooks::scroll {
    hta::m3d::ui::ScrollWnd* PickVertical(const hta::m3d::ui::TextBoxWnd* box);
    float CurrentPositionPx(const hta::m3d::ui::ScrollWnd* scroll);
    float MaxPositionPx(const hta::m3d::ui::ScrollWnd* scroll);
    float BandWidth(const hta::m3d::ui::ScrollWnd* scroll);

    float PageContentHeight(const BookState& state, int32_t page);
    float MaxScrollY(const BookState& state);

    void SyncBeforePaint(BookState& state, const hta::m3d::ui::TextBoxWnd* box,
                         const hta::m3d::ui::DrawInfo& drawInfo);
    void SyncAfterPaint(BookState& state, const hta::m3d::ui::TextBoxWnd* box);

    // Returns -1/0/+1 when movement is clamped at the top/inside/bottom edge.
    int32_t ScrollPageLines(BookState& state, int32_t lines);
}
