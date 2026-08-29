#define LOGGER "uibookstest"

#include <cmath>
#include <cstdint>
#include <string>

#include "ext/logger.hpp"
#include "ext/uibookstest/uibookstest_probes.hpp"
#include "ext/uibooks/uibooks.hpp"

#include "hta/PointBase.hpp"
#include "hta/m3d/AIParam.hpp"
#include "hta/m3d/ui/TextBoxWnd.hpp"
#include "hta/m3d/ui/Wnd.hpp"

namespace kraken::ext::uibookstest::probes {

    namespace {

        constexpr int32_t kHoverNotification = 0;
        constexpr int32_t kClickNotification = 1;
        constexpr int32_t kWheelDelta = 120;
        constexpr int32_t kMaxScrollSteps = 64;
        constexpr int32_t kMaxAutoScrollSteps = 128;
        constexpr int32_t kRowsToMoveBack = 2;
        constexpr float kPositionEpsilon = 0.5f;
        constexpr float kBandSyncLineTolerance = 1.5f;
        constexpr float kHalf = 0.5f;

        int32_t MouseWheel(void* box, int32_t delta, float x, float y) {
            hta::PointBase<float> point;
            point.x = x;
            point.y = y;
            return static_cast<hta::m3d::ui::TextBoxWnd*>(box)->OnMouseWheel(delta, point);
        }

    }

    int32_t NotifyButton(void* box, void* button, uint32_t message) {
        hta::m3d::AIParam param;
        return static_cast<hta::m3d::ui::TextBoxWnd*>(box)->OnWndNotify(
            static_cast<hta::m3d::ui::Wnd*>(button), 0u, message, param);
    }

    void RunNavProbes(void* box, std::string& fail) {
        const int32_t total = kraken::ext::uibooks::LastPageTotal();
        const int32_t last = total - 1;

        void* hoverNext = kraken::ext::uibooks::GetBookNavButton(true);
        if (!hoverNext) {
            fail = "hover_button_missing";
            return;
        }
        const int32_t hoverBefore = kraken::ext::uibooks::LastCurPage();
        const int32_t hoverRet = NotifyButton(box, hoverNext, kHoverNotification);
        const int32_t hoverAfter = kraken::ext::uibooks::LastCurPage();
        LOG_INFO("open_books: nav hover: notify (ret=%d) cur %d -> %d (must stay unchanged)",
                 hoverRet, hoverBefore, hoverAfter);
        if (hoverAfter != hoverBefore) {
            fail = "hover_changed_page";
            return;
        }
        for (int32_t expected = 1; expected <= last; ++expected) {
            const int32_t before = kraken::ext::uibooks::LastCurPage();
            void* button = kraken::ext::uibooks::GetBookNavButton(true);
            if (!button) {
                fail = "next_button_missing";
                return;
            }
            const int32_t ret = NotifyButton(box, button, kClickNotification);
            const int32_t after = kraken::ext::uibooks::LastCurPage();
            LOG_INFO("open_books: nav next%d: click-release (hook ret=%d) cur %d -> %d (expected %d, pages=%d)",
                     expected, ret, before, after, expected, total);
            if (ret != 1 || after != expected) {
                fail = std::string("next_cur_") + std::to_string(after)
                     + "_expected_" + std::to_string(expected)
                     + (ret != 1 ? "_nohook" : "");
                return;
            }
        }
        void* clampButton = kraken::ext::uibooks::GetBookNavButton(true);
        const int32_t clampRet = clampButton ? NotifyButton(box, clampButton, kClickNotification) : 0;
        const int32_t clampAfter = kraken::ext::uibooks::LastCurPage();
        LOG_INFO("open_books: nav clamp: click-release (hook ret=%d) cur %d -> %d (expected %d, pages=%d)",
                 clampRet, last, clampAfter, last, total);
        if (clampRet != 1 || clampAfter != last) {
            fail = "clamp_cur_" + std::to_string(clampAfter);
            return;
        }
        void* prevButton = kraken::ext::uibooks::GetBookNavButton(false);
        const int32_t prevRet = prevButton ? NotifyButton(box, prevButton, kClickNotification) : 0;
        const int32_t prevAfter = kraken::ext::uibooks::LastCurPage();
        LOG_INFO("open_books: nav prev: click-release (hook ret=%d) cur %d -> %d (expected %d, pages=%d)",
                 prevRet, last, prevAfter, last - 1, total);
        if (prevRet != 1 || prevAfter != last - 1) {
            fail = "prev_cur_" + std::to_string(prevAfter);
            return;
        }
        void* nextButton = kraken::ext::uibooks::GetBookNavButton(true);
        const int32_t nextRet = nextButton ? NotifyButton(box, nextButton, kClickNotification) : 0;
        const int32_t nextAfter = kraken::ext::uibooks::LastCurPage();
        LOG_INFO("open_books: nav next-final: click-release (hook ret=%d) cur %d -> %d (expected %d, pages=%d)",
                 nextRet, last - 1, nextAfter, last, total);
        if (nextRet != 1 || nextAfter != last) {
            fail = "next_final_cur_" + std::to_string(nextAfter);
        }
    }

    void RunScrollProbes(void* box, float contentWidth, float contentHeight, std::string& fail) {
        const int32_t page = kraken::ext::uibooks::LastCurPage();
        const float lineH = kraken::ext::uibooks::GetBookLineHeight();
        const float maxScroll = kraken::ext::uibooks::GetBookMaxScrollY();
        const int32_t pageLines = kraken::ext::uibooks::GetBookPageLines(page);
        const float cx = contentWidth * kHalf, cy = kHalf * contentHeight;
        LOG_INFO("open_books: scroll probe on page %d: %d lines, max scroll %.1f px, row %.1f px",
                 page, pageLines, maxScroll, lineH);

        if (!(lineH > 0.0f) || pageLines < 0) {
            fail = "page_lines_invalid";
            return;
        }
        if (!(maxScroll > kPositionEpsilon)) {
            fail = std::string("page_not_long_lines_") + std::to_string(pageLines);
            return;
        }

        for (int32_t i = 0; i < kMaxScrollSteps; ++i) {
            (void) MouseWheel(box, -kWheelDelta, cx, cy);
            if (kraken::ext::uibooks::GetBookScrollY() >= maxScroll - kPositionEpsilon)
                break;
        }
        const float sBottom = kraken::ext::uibooks::GetBookScrollY();
        if (sBottom < maxScroll - kPositionEpsilon || kraken::ext::uibooks::LastCurPage() != page) {
            fail = std::string("scroll_down_") + std::to_string(static_cast<int>(sBottom));
            return;
        }
        (void) MouseWheel(box, -kWheelDelta, cx, cy);
        if (std::fabs(kraken::ext::uibooks::GetBookScrollY() - maxScroll) > kPositionEpsilon
            || kraken::ext::uibooks::LastCurPage() != page) {
            fail = "scroll_down_edge_clamp";
            return;
        }
        (void) MouseWheel(box, kWheelDelta, cx, cy);
        (void) MouseWheel(box, kWheelDelta, cx, cy);
        const float expectUp = maxScroll - static_cast<float>(kRowsToMoveBack) * lineH;
        if (std::fabs(kraken::ext::uibooks::GetBookScrollY() - expectUp) > kPositionEpsilon
            || kraken::ext::uibooks::LastCurPage() != page) {
            fail = std::string("scroll_up_")
                 + std::to_string(static_cast<int>(kraken::ext::uibooks::GetBookScrollY()));
            return;
        }
        for (int32_t i = 0; i < kMaxScrollSteps && kraken::ext::uibooks::LastCurPage() == page; ++i)
            (void) MouseWheel(box, kWheelDelta, cx, cy);
        if (kraken::ext::uibooks::LastCurPage() != page - 1) {
            fail = std::string("scroll_up_prevpage_") + std::to_string(kraken::ext::uibooks::LastCurPage())
                 + "_scroll_" + std::to_string(static_cast<int>(kraken::ext::uibooks::GetBookScrollY()));
            return;
        }
        if (kraken::ext::uibooks::GetBookScrollY() > kPositionEpsilon) {
            fail = std::string("scroll_up_top_clamp_")
                 + std::to_string(static_cast<int>(kraken::ext::uibooks::GetBookScrollY()));
            return;
        }
        LOG_INFO("open_books: scroll probe ok: down to max (%.1f), clamped, up 2 rows, up across to page %d",
                 maxScroll, page - 1);
    }

    void RunPagesModeProbe(std::string& fail) {
        const float maxScroll = kraken::ext::uibooks::GetBookMaxScrollY();
        const int32_t page = kraken::ext::uibooks::LastCurPage();
        const int32_t pageLines = kraken::ext::uibooks::GetBookPageLines(page);
        const int32_t total = kraken::ext::uibooks::LastPageTotal();
        LOG_INFO("open_books: pages-mode probe on page %d: %d lines, %d total pages, max scroll %.1f px (must be zero)",
                 page, pageLines, total, maxScroll);
        if (maxScroll > kPositionEpsilon) {
            fail = std::string("pages_mode_scroll_") + std::to_string(static_cast<int>(maxScroll));
            return;
        }
        for (int32_t p = 0; p < total; ++p) {
            const int32_t lines = kraken::ext::uibooks::GetBookPageLines(p);
            if (lines <= 0) {
                fail = std::string("pages_mode_empty_page_") + std::to_string(p);
                return;
            }
            LOG_INFO("open_books: pages-mode page %d/%d contains %d lines", p + 1, total, lines);
        }
        LOG_INFO("open_books: pages-mode probe ok: long content split into pages without in-page scrolling");
    }

    void RunAutoScrollProbe(void* box, float contentWidth, float contentHeight, std::string& fail) {
        const int32_t page = kraken::ext::uibooks::LastCurPage();
        const float lineH = kraken::ext::uibooks::GetBookLineHeight();
        const float maxScroll = kraken::ext::uibooks::GetBookMaxScrollY();
        const int32_t pageLines = kraken::ext::uibooks::GetBookPageLines(page);
        const float cx = contentWidth * kHalf, cy = kHalf * contentHeight;
        LOG_INFO("open_books: auto-scroll probe on page %d: %d lines, max scroll %.1f px, row %.1f px",
                 page, pageLines, maxScroll, lineH);
        if (page != 0 || !(lineH > 0.0f) || !(maxScroll > kPositionEpsilon)) {
            fail = "auto_scroll_not_long";
            return;
        }
        for (int32_t i = 0; i < kMaxAutoScrollSteps; ++i) {
            (void) MouseWheel(box, -kWheelDelta, cx, cy);
            if (kraken::ext::uibooks::GetBookScrollY() >= maxScroll - kPositionEpsilon)
                break;
        }
        const float bottom = kraken::ext::uibooks::GetBookScrollY();
        if (bottom < maxScroll - kPositionEpsilon || kraken::ext::uibooks::LastCurPage() != 0) {
            fail = "auto_scroll_down_" + std::to_string(static_cast<int>(bottom));
            return;
        }
        for (int32_t i = 0; i < kMaxAutoScrollSteps; ++i) {
            (void) MouseWheel(box, kWheelDelta, cx, cy);
            if (kraken::ext::uibooks::GetBookScrollY() <= kPositionEpsilon)
                break;
        }
        const float top = kraken::ext::uibooks::GetBookScrollY();
        if (top > kPositionEpsilon || kraken::ext::uibooks::LastCurPage() != 0) {
            fail = "auto_scroll_up_" + std::to_string(static_cast<int>(top));
            return;
        }
        LOG_INFO("open_books: auto-scroll probe ok: one long page reached bottom and returned to top without page changes");
    }

    bool RunBandSyncProbe(std::string& fail) {
        void* vs = kraken::ext::uibooks::GetBookVerticalScroll();
        if (!vs) {
            fail = "engine_fail_vscroll_missing";
            return false;
        }
        const float maxPos = kraken::ext::uibooks::GetBookEngineScrollMaxPos();
        if (maxPos <= kPositionEpsilon) {
            LOG_ERROR("open_books: engine scroll band maxPos=%.1f on the long page - after-paint sync not taking effect", maxPos);
            fail = "engine_fail_maxpos_" + std::to_string(static_cast<int>(maxPos));
            return false;
        }
        const float curPx = kraken::ext::uibooks::GetBookEngineScrollCurPos();
        const float sy = kraken::ext::uibooks::GetBookScrollY();
        const float lineH = kraken::ext::uibooks::GetBookLineHeight();
        if (std::fabs(curPx - sy) > kBandSyncLineTolerance * lineH) {
            LOG_ERROR("open_books: engine band curPos %.1f px out of sync with book scrollY %.1f (max %.1f)",
                      curPx, sy, maxPos);
            fail = "engine_fail_cursync_" + std::to_string(static_cast<int>(curPx * 2));
            return false;
        }
        LOG_INFO("open_books: engine scroll band synced (maxPos=%.1f px, curPos=%.1f ~ book scrollY %.1f)", maxPos, curPx, sy);
        return true;
    }

}
