#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>

#include "hta/m3d/ui/ScrollWnd.hpp"
#include "hta/m3d/ui/Wnd.hpp"

namespace kraken::fix::warescroll {
    using hta::m3d::ui::ScrollWnd;
    using hta::m3d::ui::Wnd;

    // Shared between the feature code (warescroll.cpp: paint/wheel/bar-arrow hooks) and the
    // automated test API (warescroll_test.cpp). Everything here is inline so both TUs get a
    // single definition and a single set of globals. Production-only helpers (Recapture,
    // EnsureBar, Drive, the vtable-patch constants) stay in warescroll.cpp.

    // WareList item vector (std::vector<WareItem*>) begin/end in the LIVE binary. The PDB and
    // the AddItem disasm both put the vector at +0x220, but the running binary is shifted +4:
    // RecalcLayot (the engine's own row-iterator) and the first (non-crashing) build both read
    // begin=@0x224 / end=@0x228. So _Myfirst is at +0x224 in this build.
    inline constexpr std::size_t kItemsBegin = 0x224;
    inline constexpr std::size_t kItemsEnd   = 0x228;

    // Fallback reserved scrollbar-column width. The real column width (s.barW) is measured from
    // the "Scroll1" resource's track (GetBodyRect) at Create time; kThumbW only covers the
    // pre-Create frame.
    inline constexpr float kThumbW = 17.0f;
    // Inset the bar's right edge OFF the list's own right clip boundary so the track's 3D bevel
    // clears the clip and renders in full.
    inline constexpr float kBarRightInset = 3.0f;
    inline constexpr int   kMaxItems      = 64;
    // The scrollbar's arrow strips and the track above/below the thumb page the list by a few
    // rows per click instead of jumping to the list's end - with 16+ goods a full proportional
    // page-jump on every arrow tap was disorienting. Only grabbing and dragging the thumb moves
    // continuously across the whole range.
    inline constexpr int   kArrowStepRows     = 3;
    // Rows advanced per wheel tick. The engine's OnMouseWheel hands a NORMALISED delta of ±1 per
    // tick (not the 120-unit WM_MOUSEWHEEL value; see log "wheel delta=-1"), and each goods row is
    // ~58px tall on a list that only scrolls ~343px. 1 row/tick (58px, ~6 ticks to span the whole
    // list) is a calm standard feel; 5 rows/tick (290px) teleported the list almost to its end.
    // Shared by the wheel hooks (warescroll.cpp) and the test API (warescroll_test.cpp) so both
    // step identically.
    inline constexpr float kWheelRowsPerNotch = 1.0f;

    struct State {
        Wnd*        list = nullptr;
        ScrollWnd*  bar  = nullptr;
        uintptr_t   itemsBegin = 0;
        int32_t     count = -1;
        float       baseY0[kMaxItems] = {0};
        int32_t     baseCount = 0;
        float       listTop = 0.0f;
        float       listX0  = 0.0f;
        float       listW   = 0.0f;
        float       listH   = 0.0f;
        float       pitch   = 0.0f;
        float       viewportH = 0.0f;
        float       contentH  = 0.0f;
        float       scrollY   = 0.0f;
        float       lastPushedPx = 0.0f;
        // Reserved scrollbar-column width in list-local px. Measured from the "Scroll1"
        // resource's actual track geometry (GetBodyRect) so the column exactly fits the drawn
        // bar; kThumbW is only a fallback for the pre-Create frame.
        float       barW = 0.0f;
        // Last bar origin x we pushed via SetBounds, so we only reposition when listW moves.
        float       lastBarX = -1.0f;
        // Bar interaction (list-local px). The bar column is x in [listW-barW, listW]. The
        // draggable TRACK sub-rect is measured from GetBodyRect (bar-local y == list-local y,
        // since the bar sits at y0=0); the up-arrow strip is [0,barTrackTop] and the
        // down-arrow strip is [barTrackTop+barTrackH, viewportH].
        float       barTrackTop = 0.0f;
        float       barTrackH   = 0.0f;
        bool        barDragActive = false;
        float       barGrabDY = 0.0f;
        bool        initialized = false;
        bool        logged = false;
        // Base (un-shifted) list-local x0 of the three numeric row cells (sell/buy price and
        // amount - the only children carrying style 0x400), captured once per item-set so the
        // numeric re-centre shift in Drive is idempotent (Set, not accumulate).
        float       baseNumX0[3] = {0};
        bool        numBaseCaptured = false;
    };

    inline std::unordered_map<uintptr_t, State> g_states;

    // Test target: the goods WareList with the most items. Written by the paint hook
    // (warescroll.cpp), read by the test API (warescroll_test.cpp). Test-only: compiled out
    // of the normal build (KRAKEN_TESTS).
#if KRAKEN_TESTS
    inline Wnd*    g_testTarget = nullptr;
    inline int32_t g_testTargetCount = -1;
#endif // KRAKEN_TESTS

    // Valid x86 user-mode heap pointers; anything outside is a corrupt/zero field read.
    inline bool SanePtr(uintptr_t p) { return p >= 0x10000 && p < 0x7FFE0000; }

    inline State& StateFor(Wnd* list) {
        const auto key = reinterpret_cast<uintptr_t>(list);
        auto it = g_states.find(key);
        if (it != g_states.end())
            return it->second;
        State s;
        s.list = list;
        return g_states.emplace(key, s).first->second;
    }

    inline int32_t ReadItems(Wnd* list, Wnd** out, int32_t maxItems) {
        auto* base = reinterpret_cast<uint8_t*>(list);
        const uintptr_t begin = *reinterpret_cast<uintptr_t*>(base + kItemsBegin);
        const uintptr_t end   = *reinterpret_cast<uintptr_t*>(base + kItemsEnd);
        if (!SanePtr(begin))
            return 0;  // empty/unbuilt vector: never deref a null or corrupt base
        int32_t count = (end > begin)
            ? static_cast<int32_t>((end - begin) / sizeof(void*)) : 0;
        if (count > maxItems)
            count = maxItems;
        auto* rowPtrs = reinterpret_cast<Wnd**>(begin);
        for (int32_t i = 0; i < count; ++i)
            out[i] = rowPtrs[i];
        return count;
    }

    inline int32_t ItemCount(Wnd* list) {
        auto* base = reinterpret_cast<uint8_t*>(list);
        const uintptr_t begin = *reinterpret_cast<uintptr_t*>(base + kItemsBegin);
        const uintptr_t end   = *reinterpret_cast<uintptr_t*>(base + kItemsEnd);
        if (!SanePtr(begin))
            return 0;
        const int32_t count = (end > begin)
            ? static_cast<int32_t>((end - begin) / sizeof(void*)) : 0;
        return count > kMaxItems ? kMaxItems : count;
    }

    inline float MaxScroll(const State& s) {
        const float m = s.contentH - s.viewportH;
        return m > 0.0f ? m : 0.0f;
    }

    inline void ClampScroll(State& s) {
        if (s.scrollY < 0.0f)
            s.scrollY = 0.0f;
        else if (s.scrollY > MaxScroll(s))
            s.scrollY = MaxScroll(s);
    }

    // Bar-interaction math (thumb geometry + press/move handlers). Used ONLY by the automated
    // test (Test_BarSimulate in warescroll_test.cpp); the production bar drag/arrow path drives
    // the native ScrollWnd (Drive adopts its position, Hook_ScrollWndOnWndNotify fixes the step).
    // Compiled out of the normal build.
#if KRAKEN_TESTS
    // ---- Bar interaction (list-local px) ------------------------------------------
    // The thumb height is the visible fraction of the track (viewportH/contentH * trackH);
    // the thumb top tracks scrollY/maxScroll across (trackH - thumbH) px of travel.
    inline float ThumbHFor(const State& s) {
        if (s.barTrackH <= 1.0f || s.contentH <= 1.0f)
            return s.barTrackH;
        float h = s.barTrackH * (s.viewportH / s.contentH);
        if (h < 16.0f) h = 16.0f;       // min grabbable thumb
        if (h > s.barTrackH) h = s.barTrackH;
        return h;
    }
    inline float ThumbTopFor(const State& s, float scrollY) {
        const float maxScroll = MaxScroll(s);
        const float thumbH = ThumbHFor(s);
        const float travel = s.barTrackH - thumbH;
        if (maxScroll <= 0.0f || travel <= 0.0f)
            return s.barTrackTop;
        float frac = scrollY / maxScroll;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        return s.barTrackTop + frac * travel;
    }
    // Handle a left-button press (list-local point). Returns true if the point fell in the bar
    // column and was acted on (a few-row step, or a thumb grab for dragging). Grabbing the thumb
    // starts a drag; any other point in the column (the up/down arrow strips, and the track
    // above/below the thumb) steps the list by kArrowStepRows toward that end - deliberately NOT
    // a proportional page-jump, which on a 16+ goods list sent the list straight to the end.
    inline bool BarHandlePress(State& s, float px, float py) {
        if (!s.bar || s.barTrackH <= 1.0f)
            return false;
        const float barX0 = s.listW - s.barW;
        if (px < barX0 || px > s.listW || py < 0.0f || py > s.viewportH)
            return false;
        if (MaxScroll(s) <= 0.0f)
            return false;   // nothing to scroll
        const float thumbTop = ThumbTopFor(s, s.scrollY);
        const float thumbH   = ThumbHFor(s);
        if (py >= thumbTop && py <= thumbTop + thumbH) {   // grabbed the thumb: drag it
            s.barGrabDY = py - thumbTop;
            s.barDragActive = true;
            return true;
        }
        const float step = kArrowStepRows * ((s.pitch > 1.0f) ? s.pitch : 16.0f);
        s.scrollY += (py < thumbTop) ? -step : step;       // above thumb -> up, below -> down
        ClampScroll(s);
        return true;
    }
    // Handle a mouse move (list-local point) while a drag is active. lbtnDown = left held.
    inline void BarHandleMove(State& s, float px, float py, bool lbtnDown) {
        if (!s.bar || !s.barDragActive)
            return;
        if (!lbtnDown) { s.barDragActive = false; return; }
        const float barX0 = s.listW - s.barW;
        if (px < barX0 || px > s.listW) { s.barDragActive = false; return; }  // left the column
        const float maxScroll = MaxScroll(s);
        if (maxScroll <= 0.0f)
            return;
        const float travel = s.barTrackH - ThumbHFor(s);
        if (travel <= 1.0f)
            return;
        const float newTop = py - s.barGrabDY;
        s.scrollY = (newTop - s.barTrackTop) / travel * maxScroll;
        ClampScroll(s);
    }
#endif // KRAKEN_TESTS

    // Write the shifted row positions (baseY0 - scrollY) into the live items. Per-item
    // pointer guard: a mid-rebuild vector can hold a stale slot; skip those rather than
    // deref. Shared by Drive (the paint path) and the test API (which must apply the shift
    // even when the modal panel has stopped repainting).
    inline void ApplyShift(const State& s, Wnd* items[], int32_t n) {
        const int32_t cnt = (n < s.baseCount) ? n : s.baseCount;
        // Keep each row's gray background from running under the scrollbar column: end the
        // row background a few px left of the bar's left edge (barX = listW - barW -
        // kBarRightInset). This moves only the row BACKGROUND bevel, not the content cells
        // (those are separate template child windows - the amount-column gutter is set in
        // workshopwnd.xml). x0 is left at the engine's value so the list does not drift.
        const float barW = (s.barW > 1.0f) ? s.barW : kThumbW;
        const float reserved = barW + kBarRightInset;
        const float rowRight = (s.listW > reserved + 1.0f) ? (s.listW - reserved) : s.listW;
        for (int32_t i = 0; i < cnt; ++i) {
            if (!SanePtr(reinterpret_cast<uintptr_t>(items[i])))
                continue;
            items[i]->m_bounds.y0 = s.baseY0[i] - s.scrollY;
            const float x0 = items[i]->m_bounds.x0;
            if (rowRight > x0)
                items[i]->m_bounds.width = rowRight - x0;
        }
    }
}
