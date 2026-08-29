#define LOGGER "uibooks"

#include "ext/uibooks/uibooks_scroll.hpp"

#include <cmath>

#include "ext/uibooks/uibooks_pagination.hpp"

#include "hta/m3d/ui/ScrollWnd.hpp"
#include "hta/m3d/ui/TextBoxWnd.hpp"

namespace kraken::ext::uibooks::scroll {
    hta::m3d::ui::ScrollWnd* PickVertical(const hta::m3d::ui::TextBoxWnd* box) {
        if (!box)
            return nullptr;
        hta::m3d::ui::ScrollWnd* vertical = box->m_scrollVWnd;
        hta::m3d::ui::ScrollWnd* horizontal = box->m_scrollHWnd;
        if (vertical && vertical->m_vertical != 0)
            return vertical;
        if (horizontal && horizontal->m_vertical != 0)
            return horizontal;
        return vertical ? vertical : horizontal;
    }

    float CurrentPositionPx(const hta::m3d::ui::ScrollWnd* scroll) {
        const float position = scroll ? scroll->m_curPos * scroll->m_maxPos : 0.0f;
        return std::isfinite(position) ? position : 0.0f;
    }

    float MaxPositionPx(const hta::m3d::ui::ScrollWnd* scroll) {
        const float maximum = scroll ? scroll->m_maxPos : 0.0f;
        return std::isfinite(maximum) ? maximum : 0.0f;
    }

    float BandWidth(const hta::m3d::ui::ScrollWnd* scroll) {
        const float width = scroll ? scroll->m_bounds.width : 0.0f;
        return (std::isfinite(width) && width > 0.0f) ? width : 0.0f;
    }

    float PageContentHeight(const BookState& state, int32_t page) {
        return static_cast<float>(pagination::VisualRows(
            state.pageStart, state.pageEnd, state.lineRows, page)) * state.lineH;
    }

    float MaxScrollY(const BookState& state) {
        if (state.pageStart.empty() || state.curPage < 0
            || state.curPage >= static_cast<int32_t>(state.pageStart.size()))
            return 0.0f;
        const float maximum = PageContentHeight(state, state.curPage) - state.visibleH;
        return maximum > 0.0f ? maximum : 0.0f;
    }

    void SyncBeforePaint(BookState& state, const hta::m3d::ui::TextBoxWnd* box,
                         const hta::m3d::ui::DrawInfo& drawInfo) {
        if (!state.scrollSynced || !(drawInfo.m_clientRect.width > 0.0f)
            || !(drawInfo.m_clientRect.height > 0.0f))
            return;
        hta::m3d::ui::ScrollWnd* vertical = PickVertical(box);
        if (!vertical)
            return;
        state.vscroll = vertical;
        const float maximum = MaxScrollY(state);
        if (maximum <= 0.0f)
            return;
        const float position = CurrentPositionPx(vertical);
        if (std::fabs(position - state.lastPushedPx) > 0.5f) {
            state.scrollY = position > maximum ? maximum : position;
            state.lastPushedPx = state.scrollY;
        }
    }

    void SyncAfterPaint(BookState& state, const hta::m3d::ui::TextBoxWnd* box) {
        hta::m3d::ui::ScrollWnd* vertical = state.vscroll ? state.vscroll : PickVertical(box);
        if (!vertical)
            return;
        state.vscroll = vertical;
        const float maximum = MaxScrollY(state);
        if (maximum > 0.0f) {
            vertical->EnableWindow(true);
            vertical->ShowWindow(true);
            vertical->SetScrollRect(state.clientW, PageContentHeight(state, state.curPage));
            vertical->SetCurPos(state.scrollY);
            state.lastPushedPx = CurrentPositionPx(vertical);
        }
        else {
            state.lastPushedPx = 0.0f;
        }
        state.scrollSynced = true;
    }

    int32_t ScrollPageLines(BookState& state, int32_t lines) {
        const float maximum = MaxScrollY(state);
        const float target = state.scrollY + static_cast<float>(lines) * state.lineH;
        if (target <= 0.0f) {
            state.scrollY = 0.0f;
            return lines < 0 ? -1 : 0;
        }
        if (target >= maximum) {
            state.scrollY = maximum;
            return lines > 0 ? 1 : 0;
        }
        state.scrollY = target;
        return 0;
    }
}
