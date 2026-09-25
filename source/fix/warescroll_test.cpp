#define LOGGER "warescroll"

#include "fix/warescroll_test.hpp"

#include "fix/warescroll_internal.hpp"

#include <cstdint>

#include "ext/logger.hpp"
#include "hta/m3d/ui/WndStation.hpp"

namespace kraken::fix::warescroll {
    using hta::PointBase;

    // ---- Automated test API (uibookstest "ware" driver) ------------------------------

    bool Test_GetReport(TestReport& out) {
        out = TestReport{};
        if (!g_testTarget)
            return false;
        State& s = StateFor(g_testTarget);
        out.targetFound = true;
        out.count = s.count;
        out.maxScroll = MaxScroll(s);
        out.scrollY = s.scrollY;
        out.contentH = s.contentH;
        out.viewportH = s.viewportH;
        out.pitch = s.pitch;
        out.hasBar = (s.bar != nullptr);
        out.initialized = s.initialized;
        if (s.baseCount > 0) {
            out.item0BaseTop = s.baseY0[0];
            Wnd* probe[kMaxItems] = {nullptr};
            const int32_t n = ReadItems(g_testTarget, probe, kMaxItems);
            if (n > 0 && probe[0] && SanePtr(reinterpret_cast<uintptr_t>(probe[0])))
                out.item0TopNow = probe[0]->m_bounds.y0;
        }
        return true;
    }

    float Test_StepWheel(int32_t delta120) {
        if (!g_testTarget)
            return 0.0f;
        State& s = StateFor(g_testTarget);
        if (s.pitch > 0.0f && delta120 != 0) {
            s.scrollY -= (static_cast<float>(delta120) / 120.0f) * kWheelRowsPerNotch * s.pitch;
            ClampScroll(s);
        }
        // Apply the shift now, so item0TopNow is current even if the modal panel has
        // stopped repainting (a repaint would otherwise be the only thing to apply it).
        Wnd* items[kMaxItems] = {nullptr};
        ApplyShift(s, items, ReadItems(g_testTarget, items, kMaxItems));
        return s.scrollY;
    }

    void Test_Reset() {
        if (!g_testTarget)
            return;
        State& s = StateFor(g_testTarget);
        s.scrollY = 0.0f;
        Wnd* items[kMaxItems] = {nullptr};
        ApplyShift(s, items, ReadItems(g_testTarget, items, kMaxItems));
    }

    // #3 diagnostic: does a mouse press on the scrollbar actually route to the bar?
    //
    // WndStation::GetWndForMousePoint(curWnd, pt, affectAll=false) walks curWnd's children in
    // list order and returns the FIRST visible child (deepest match) whose bounds contain pt,
    // passing pt UNTRANSFORMED down the recursion. The bar is the LAST list child, so an earlier
    // row shadows it only if that row's right edge still covers pt (i.e. the row has NOT been
    // narrowed below the bar's column). We probe two points in the bar's own bounds - dead-centre
    // (on the track/thumb, below the arrow buttons) and the upper strip (on the up-arrow button
    // area). isBar==true means a press there reaches the bar; a row returned instead means the
    // row overlaps the bar's column (ApplyShift narrowing not taking effect).
    void Test_HitTestOnBar() {
        if (!g_testTarget)
            return;
        State& s = StateFor(g_testTarget);
        if (!s.bar) {
            LOG_WARNING("hit-test diag: no bar yet");
            return;
        }
        auto* station = s.list->GetStation();
        if (!station) {
            LOG_WARNING("hit-test diag: list->GetStation() is null");
            return;
        }
        const auto& b = s.bar->m_bounds;
        // ptTop sits 5px below the bar's top edge - inside the up-arrow button (~40px tall),
        // so it routes to the button (not the track). Fixed offset avoids the Windows.h min macro.
        const PointBase<float> ptCenter{ b.x0 + b.width * 0.5f, b.y0 + b.height * 0.5f };
        const PointBase<float> ptTop   { b.x0 + b.width * 0.5f, b.y0 + 5.0f };
        Wnd* hitC = station->GetWndForMousePoint(s.list, ptCenter, false);
        Wnd* hitT = station->GetWndForMousePoint(s.list, ptTop, false);
        const bool isBarC = (hitC == reinterpret_cast<Wnd*>(s.bar));
        const bool isBarT = (hitT == reinterpret_cast<Wnd*>(s.bar));
        auto isSiblingRow = [&](Wnd* w) -> bool {
            if (!w || w == reinterpret_cast<Wnd*>(s.bar))
                return false;
            for (auto* c = s.list->m_firstChild; c; c = c->m_nextSibling)
                if (reinterpret_cast<Wnd*>(c) == w)
                    return true;
            return false;
        };
        LOG_INFO("hit-test diag: bar bounds=(%.1f,%.1f %.1fx%.1f) | centre=(%.1f,%.1f) -> 0x%llX "
                 "isBar=%d row=%d | top=(%.1f,%.1f) -> 0x%llX isBar=%d row=%d",
                 b.x0, b.y0, b.width, b.height,
                 ptCenter.x, ptCenter.y,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hitC)),
                 (int)isBarC, (int)isSiblingRow(hitC),
                 ptTop.x, ptTop.y,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hitT)),
                 (int)isBarT, (int)isSiblingRow(hitT));
    }

    // #3 verification: exercise the bar-interaction math (down-arrow step, thumb drag top->bottom,
    // out-of-column rejection) with synthetic list-local points and log the results. Pure (no real
    // mouse), so the automated driver can confirm the drag/arrow computations; the actual routing
    // (the list really receiving the engine's mouse events) is confirmed by the "list-mouse" log
    // lines on a genuine click.
    bool Test_BarSimulate() {
        if (!g_testTarget)
            return false;
        State& s = StateFor(g_testTarget);
        if (!s.bar || s.barTrackH <= 1.0f || s.listW <= 1.0f) {
            LOG_INFO("bar-sim: skipped (bar=%p trackH=%.1f listW=%.1f)",
                     (const void*)s.bar, s.barTrackH, s.listW);
            return false;
        }
        const float maxScroll = MaxScroll(s);
        if (maxScroll <= 1.0f) {
            LOG_INFO("bar-sim: skipped (maxScroll=%.2f too small)", maxScroll);
            return false;
        }
        Wnd* items[kMaxItems] = {nullptr};
        const int32_t n = ReadItems(g_testTarget, items, kMaxItems);
        const float barX0 = s.listW - s.barW;
        const float cx = barX0 + s.barW * 0.5f;
        const float savedY = s.scrollY;

        // 1) Down-arrow press (below the track) scrolls down by ~pitch.
        s.scrollY = 0.0f;
        const float downArrowY = s.barTrackTop + s.barTrackH + 10.0f;
        const bool consumedDown = BarHandlePress(s, cx, downArrowY);
        const float afterDown = s.scrollY;
        ApplyShift(s, items, n);
        LOG_INFO("bar-sim down-arrow: consumed=%d after=%.2f (expect >0, ~%d rows x pitch %.2f)",
                 (int)consumedDown, afterDown, kArrowStepRows, s.pitch);

        // 2) Thumb drag from the top down to the bottom reaches ~maxScroll.
        s.scrollY = 0.0f;
        const float thumbTop0 = ThumbTopFor(s, 0.0f);
        BarHandlePress(s, cx, thumbTop0 + 2.0f);
        const int steps = 8;
        for (int i = 1; i <= steps; ++i)
            BarHandleMove(s, cx, s.barTrackTop + (s.barTrackH * (float)i / (float)steps), true);
        const float afterDrag = s.scrollY;
        ApplyShift(s, items, n);
        LOG_INFO("bar-sim drag: thumbTop0=%.2f maxScroll=%.2f afterDrag=%.2f (expect ~%.2f)",
                 thumbTop0, maxScroll, afterDrag, maxScroll);

        // 3) A press outside the bar column (left of it) must NOT be consumed.
        s.scrollY = 0.0f;
        const bool consumedLeft = BarHandlePress(s, barX0 - 5.0f, thumbTop0 + 2.0f);
        const float afterLeft = s.scrollY;
        LOG_INFO("bar-sim out-of-column: consumed=%d after=%.2f (expect 0 / 0)",
                 (int)consumedLeft, afterLeft);

        s.scrollY = savedY;
        ApplyShift(s, items, n);

        const bool ok = consumedDown && (afterDown > 1.0f)
                       && (afterDrag > maxScroll * 0.85f)
                       && (!consumedLeft) && (afterLeft < 1.0f);
        LOG_INFO("bar-sim OK=%d", (int)ok);
        return ok;
    }
}
