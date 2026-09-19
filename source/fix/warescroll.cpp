#define LOGGER "warescroll"

#include "fix/warescroll.hpp"
#include "fix/warescroll_internal.hpp"

#include "config.hpp"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/BoundsBase.hpp"
#include "hta/CStr.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/m3d/ui/Enums.hpp"
#include "hta/m3d/ui/ScrollWnd.hpp"
#include "hta/m3d/ui/Wnd.hpp"
#include "hta/m3d/ui/WndStation.hpp"

namespace kraken::fix::warescroll {
    using hta::BoundsBase;
    using hta::PointBase;
    using hta::m3d::ui::DrawInfo;
    using hta::m3d::ui::ScrollWnd;
    using hta::m3d::ui::Wnd;

    namespace {
        // Vtable slots (VA) confirmed to hold the m3d::ui::Wnd base implementations:
        // WareList slot 34 = Wnd::OnPaint, slot 46 = Wnd::OnMouseWheel; WareItem
        // slot 46 = Wnd::OnMouseWheel. WareList/WareItem do not override these, so
        // patching the slot replaces the inherited behaviour.
        constexpr uintptr_t kWareListOnPaintSlot = 0x009CDB30;
        constexpr uintptr_t kWareListWheelSlot   = 0x009CDB60;
        constexpr uintptr_t kWareItemWheelSlot   = 0x009CDC80;

        // Design geometry (workshopwnd.xml): the goods-list viewport is 583px tall
        // and each row (wndItemPattern) is 56px tall, so ~10.41 rows fit and the 11th
        // clips. The list scales with resolution, so the live viewport is derived
        // from the measured row pitch (pitch * 583/56), not a fixed pixel count.
        constexpr float kDesignViewportH = 583.0f;
        constexpr float kDesignRowH      = 56.0f;
        // Goods-list width in the 16:9 design (workshopwnd.xml wndWareList). Layout-fixed and
        // scales with the window, so (listW / kDesignListW) is the live resolution scale.
        constexpr float kDesignListW     = 813.0f;
        // Right margin added to the reserved scrollbar column. The "Scroll1" track's right bevel
        // lands exactly on m_bounds.width, so without a margin the outermost bevel pixel(s) get
        // clipped ("slightly cut off on the right"). The track is drawn from the resource at a
        // fixed bar-local x, so widening the column by a few px reveals the bevel without shifting
        // the track; rows narrow to listW - (barW + pad).
        constexpr float kBarRightPad       = 4.0f;
        // Gutter between the rightmost goods (the amount "N" column) and the scrollbar is handled
        // by moving the amount COLUMN (the wndWareAmount template cell in workshopwnd.xml) left,
        // NOT by shifting the row. The row's cells are separate child windows placed by the
        // template at fixed x; the whole row's origin (m_bounds.x0) cannot be shifted left to make
        // a right-side gutter because that would drag the icon and name left too. (Narrowing the
        // row width only moves the gray background bevel, not the cells.)
        // (kWheelRowsPerNotch - rows per wheel notch - lives in warescroll_internal.hpp so the
        // wheel hooks here and the test API step identically.)

        // #4: the bar's arrow buttons (m_btn0/m_btn1) don't route through OnMouseButton0 - a
        // button click posts a notification to the parent bar, handled in ScrollWnd::OnWndNotify.
        // CONFIRMED from the real log (see Hook_ScrollWndOnWndNotify): message==1 is the "step"
        // event (7/8 are press/release), and idFrom selects the arrow - 256 = up, 257 = down.
        // The native steps the thumb FRACTION m_curPos by (clientSize / m_maxPos) - a full
        // viewport "page". m_curPos is a fraction clamped to [0,1]; on the 16-item list
        // maxPos(343px) < viewportH(583px), so that step is ~1.7 and clamps to 1.0: one arrow
        // tap jumped the list straight to its very top/bottom. We wrap the shared ScrollWnd vtable
        // OnWndNotify slot and, for message==1 && idFrom in {256,257}, advance only a few rows.
        using BarOnWndNotifyFn =
            int32_t (__thiscall *)(ScrollWnd* self, Wnd* from, uint32_t idFrom, uint32_t message,
                                    const hta::m3d::AIParam& data);
        BarOnWndNotifyFn g_barOnWndNotifyOrig = nullptr;
        bool             g_barOnWndNotifyPatched = false;
        // Test-only: selects the automated test's target list (the goods WareList with the most
        // items). Compiled out of the normal build.
#if KRAKEN_TESTS
        void NoteTargetCandidate(Wnd* list, int32_t count) {
            if (count >= 10 && count > g_testTargetCount) {
                g_testTarget = list;
                g_testTargetCount = count;
            }
        }
#endif // KRAKEN_TESTS

        void Recapture(Wnd* list, State& s, Wnd* items[], int32_t n) {
            s.baseCount = n;
            s.listTop = list->m_bounds.y0;
            s.listX0 = list->m_bounds.x0;
            s.listW = list->m_bounds.width;
            s.listH = list->m_bounds.height;

            float pitch = 0.0f;
            if (n >= 2)
                pitch = items[1]->m_bounds.y0 - items[0]->m_bounds.y0;
            else if (n == 1 && items[0]->m_bounds.height > 1.0f)
                pitch = items[0]->m_bounds.height;
            if (!(pitch > 1.0f))
                pitch = kDesignRowH;
            s.pitch = pitch;
            // The list auto-sizes its HEIGHT to its content (m_bounds.height == contentH), so
            // it cannot be the viewport - using it makes maxScroll structurally zero. The fixed
            // viewport is the list's DESIGN area (583px, workshopwnd.xml wndWareList) scaled to
            // the live resolution via the measured list width. Observed live: clip height 585
            // (design 583) while content reads 578 (10 normal rows, fits) or 818 (tall rows,
            // overflows). A degenerate width falls back to the unscaled design value.
            s.viewportH = (s.listW > 1.0f)
                ? kDesignViewportH * (s.listW / kDesignListW)
                : kDesignViewportH;

            for (int32_t i = 0; i < n; ++i) {
                s.baseY0[i] = items[i]->m_bounds.y0;
            }
            if (n > 0) {
                const float lastH = items[n - 1]->m_bounds.height > 1.0f
                    ? items[n - 1]->m_bounds.height : pitch;
                s.contentH = (s.baseY0[n - 1] - s.baseY0[0]) + lastH;
            }
            else
                s.contentH = 0.0f;
            // Rows just re-cloned their template cells (un-shifted) - re-capture the numeric base.
            s.numBaseCaptured = false;
        }

        // See the comment above BarOnWndNotifyFn: the native arrow step is a full viewport "page"
        // that clamps to the end on a short list. Let the native handler run (it performs the
        // thumb repaint + parent-notify side-effects), then rewrite m_curPos to advance only
        // kArrowStepRows worth from the pre-step fraction, and drive the rows immediately so the
        // step shows now rather than only on the next Drive(paint).
        int32_t Hook_ScrollWndOnWndNotify(ScrollWnd* self, Wnd* from, uint32_t idFrom, uint32_t message,
                                          const hta::m3d::AIParam& data) {
            // The bar's arrow buttons post a notify to the bar. CONFIRMED from the real log:
            //   message == 1    = the "step" event (the only value that scrolls; 7/8 are press/release)
            //   idFrom   == 256 = UP arrow    (native: m_curPos -= step)
            //   idFrom   == 257 = DOWN arrow  (native: m_curPos += step)
            // The native (scroll.cpp:161-210) then steps the thumb fraction m_curPos by a full
            // viewport "page" (clientSize / m_maxPos), which clamps to the very end on a short
            // 16-goods list (maxPos ~343px < viewport ~583px). We override ONLY that step: advance
            // a few rows from the pre-step fraction and drive the rows now, and SKIP the native for
            // the arrow case (its page-step + CallParentNotify(message=5) would jump the list to the
            // end first). Every other notify (press/release, ...) goes to the native untouched.
            const bool isArrow = (message == 1) && (idFrom == 256 || idFrom == 257);
            const float prevCur = self->m_curPos;
            if (isArrow) {
                auto* list = static_cast<Wnd*>(self->m_parent);
                if (list) {
                    State& s = StateFor(list);
                    // Only the bar we actually drive (State initialised and owns this bar). Other
                    // ScrollWnds share this vtable slot - fall through to the native for them.
                    if (s.initialized && s.bar == self) {
                        const float maxPos = self->m_maxPos;
                        if (maxPos > 0.0f) {
                            const float pitch = (s.pitch > 1.0f) ? s.pitch : 16.0f;
                            const float stepFrac = (kArrowStepRows * pitch) / maxPos;
                            // idFrom 256 = up (m_curPos -= step), 257 = down (m_curPos += step) -
                            // same direction as the native; only the STEP SIZE differs (a few rows,
                            // not the full viewport page that clamps to the end on a short list).
                            float cur = prevCur + (idFrom == 257 ? stepFrac : -stepFrac);
                            if (cur < 0.0f) cur = 0.0f;
                            if (cur > 1.0f) cur = 1.0f;
                            self->m_curPos = cur;
                            s.scrollY = cur * maxPos;
                            ClampScroll(s);
                            Wnd* items[kMaxItems] = {nullptr};
                            ApplyShift(s, items, ReadItems(list, items, kMaxItems));
                            return 1;   // handled; do NOT run the native page-step
                        }
                    }
                }
            }
            // Non-arrow notify, or our bar not ready / a different ScrollWnd: native handles it.
            return g_barOnWndNotifyOrig
                ? g_barOnWndNotifyOrig(self, from, idFrom, message, data) : 1;
        }

        struct BarOnWndNotify_Hooked {
            int32_t OnWndNotify(Wnd* from, uint32_t idFrom, uint32_t message, const hta::m3d::AIParam& data) {
                return Hook_ScrollWndOnWndNotify(reinterpret_cast<ScrollWnd*>(this), from, idFrom, message, data);
            }
        };

        void HookBarOnWndNotify(ScrollWnd* bar) {
            if (g_barOnWndNotifyPatched)
                return;
            const uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(bar);  // bar->[0]
            if (!SanePtr(vtbl)) {
                LOG_WARNING("bar vtable 0x%llX not sane; skip OnWndNotify hook",
                            static_cast<unsigned long long>(vtbl));
                return;
            }
            const uintptr_t kTargetVA = 0x6FA980;  // ScrollWnd::OnWndNotify (RVA 0x2FA980)
            uintptr_t slotOff = 0;
            for (slotOff = 0; slotOff <= 0x140; slotOff += 4)
                if (*reinterpret_cast<const uintptr_t*>(vtbl + slotOff) == kTargetVA)
                    break;
            if (slotOff > 0x140) {
                LOG_WARNING("OnWndNotify VA 0x%llX not found in bar vtable (first 0x140 bytes)",
                            static_cast<unsigned long long>(kTargetVA));
                return;
            }
            g_barOnWndNotifyOrig = reinterpret_cast<BarOnWndNotifyFn>(kTargetVA);
            routines::OverrideValue(reinterpret_cast<void*>(vtbl + slotOff), &BarOnWndNotify_Hooked::OnWndNotify);
            g_barOnWndNotifyPatched = true;
            LOG_INFO("patched ScrollWnd vtable slot 0x%X (OnWndNotify VA 0x%llX) for arrow step fix",
                     (unsigned)slotOff, static_cast<unsigned long long>(kTargetVA));
        }

        // Wire up the bar's arrow-step hook - the only bar-side patch. The rest of the bar's
        // input (thumb drag, track clicks, press/release) is the native ScrollWnd behaviour,
        // which works now that Drive keeps m_maxPos authoritative.
        void SetupBarHooks(ScrollWnd* bar) {
            HookBarOnWndNotify(bar);
        }

        void EnsureBar(Wnd* list, State& s) {
            if (s.bar)
                return;
            // Reuse an engine-created vertical scroll window if one is already attached.
            auto* engineBar = list->m_scrollVWnd;
            if (engineBar && engineBar->m_vertical != 0) {
                s.bar = engineBar;
                const auto eb = engineBar->GetBodyRect();
                if (std::isfinite(eb.y0) && eb.height > 1.0f) {
                    s.barTrackTop = eb.y0;
                    s.barTrackH   = eb.height;
                }
                SetupBarHooks(engineBar);
                return;
            }
            auto* bar = static_cast<ScrollWnd*>(ScrollWnd::CreateObject());
            if (!bar) {
                LOG_WARNING("ScrollWnd::CreateObject returned null");
                return;
            }
            // The bar is a CHILD of the list, so its bounds are in the list's LOCAL
            // coordinate space (origin at the list's top-left, where item 0 sits at y0=0 -
            // the same space the engine uses for the row items). Placing it at y0=0,
            // x0=listW-barW spans exactly the design viewport (top to the list's bottom).
            //
            // Two things must match the "Scroll1" resource's ACTUAL track geometry, which
            // OnPaint/RecalcLayot derive from GetScrollPane("Scroll1") (NOT m_bounds.width):
            //   - the reserved COLUMN (barW) so the rows leave room and the track doesn't overflow;
            //   - the bar's m_bounds.width so the resource's ~35px track is NOT clipped to a sliver.
            // We create with a provisional width, fully set up the bar (pane + style + parented),
            // then measure the real track via GetBodyRect() under those known-good conditions and
            // size the column to it. The resource's track (bar-local: x0 = left inset, width =
            // track) is independent of m_bounds.width, so the provisional width doesn't bias it.
            BoundsBase<float> provisional{ s.listW - kThumbW, 0.0f, kThumbW, s.viewportH };
            bar->Create(provisional, 1);
            if (!bar->Valid()) {
                LOG_WARNING("ScrollWnd Create(bounds,1) produced an invalid window");
                return;
            }
            const hta::CStr scrollPane("Scroll1");
            bar->SetScrollPane(scrollPane);
            bar->m_style |= hta::m3d::ui::WndStyle::WND_STYLE_VISIBLE;
            list->AddChild(bar);
            const auto body = bar->GetBodyRect();
            const float rawTrack = (std::isfinite(body.x0) && std::isfinite(body.width))
                ? (body.x0 + body.width) : 0.0f;
            // Track geometry for the bar-interaction math (shared header helpers + test API):
            // bar-local y == list-local y (bar at y0=0). GetBodyRect is the draggable track
            // (excludes the arrow buttons), so the up-arrow is the strip [0, body.y0] and the
            // down-arrow the strip [body.y0+body.height, viewportH].
            if (std::isfinite(body.y0) && body.height > 1.0f) {
                s.barTrackTop = body.y0;
                s.barTrackH   = body.height;
            }
            // Reserve the measured track PLUS a small right margin (kBarRightPad) so the track's
            // right bevel - which lands exactly on m_bounds.width - isn't clipped. The track is
            // drawn from the resource at a fixed bar-local x, so the extra column width only
            // reveals the bevel; rows narrow to listW - barW (s.barW now includes the pad).
            const float barW = (rawTrack > 8.0f) ? (rawTrack + kBarRightPad) : kThumbW;
            s.barW = barW;
            const float barX = s.listW - barW - kBarRightInset;
            BoundsBase<float> bounds{ barX, 0.0f, barW, s.viewportH };
            bar->SetBounds(bounds, true);
            s.bar = bar;
            s.lastBarX = barX;
            const auto thumb = bar->GetThumbRect();
            LOG_INFO("created ScrollWnd bar x0=%.1f y0=%.1f w=%.1f h=%.1f m_vertical=%d "
                     "body=(%.1f,%.1f %.1fx%.1f) thumb=(%.1f,%.1f %.1fx%.1f)",
                     bounds.x0, bounds.y0, bounds.width, bounds.height, (int)bar->m_vertical,
                     body.x0, body.y0, body.width, body.height,
                     thumb.x0, thumb.y0, thumb.width, thumb.height);
            SetupBarHooks(bar);
        }

        float BarPositionPx(const ScrollWnd* bar) {
            const float p = bar ? bar->m_curPos * bar->m_maxPos : 0.0f;
            return std::isfinite(p) ? p : 0.0f;
        }

        void Drive(Wnd* list, const DrawInfo& di) {
            Wnd* items[kMaxItems] = {nullptr};
            const int32_t n = ReadItems(list, items, kMaxItems);
            auto* listBase = reinterpret_cast<uint8_t*>(list);
            const uintptr_t beginNow = *reinterpret_cast<uintptr_t*>(listBase + kItemsBegin);

            // Never shift or read a row whose pointer looks corrupt — the vector can hold
            // stale slots mid-rebuild. Bail out (no scroll this frame) instead of deref'ing.
            bool itemsOk = true;
            for (int32_t i = 0; i < n; ++i)
                if (!SanePtr(reinterpret_cast<uintptr_t>(items[i]))) { itemsOk = false; break; }
            if (!itemsOk && n > 0)
                return;

            State& s = StateFor(list);
            if (!s.initialized || beginNow != s.itemsBegin || n != s.count) {
                Recapture(list, s, items, n);
                s.itemsBegin = beginNow;
                s.count = n;
                s.scrollY = 0.0f;
                s.lastPushedPx = 0.0f;
                s.initialized = true;
                if (!s.logged && n > 0) {
                    s.logged = true;
                    LOG_INFO("WareList top=%.1f x0=%.1f w=%.1f h=%.1f pitch=%.2f "
                             "viewportH=%.2f contentH=%.2f maxScroll=%.2f items=%d",
                             s.listTop, s.listX0, s.listW, s.listH, s.pitch, s.viewportH,
                             s.contentH, MaxScroll(s), n);
                }
            }

            if (n < 2)
                return;

            // Re-center the numeric columns while this fix is active. Reserving the scrollbar
            // column narrows each row (ApplyShift); the engine draws the column divider boxes
            // relative to that narrowed width, but the price/amount text stays at its fixed
            // template x - so the values look pushed right of centre. Slide the three numeric
            // cells (the only row children carrying style 0x400; the name is 0xB00, icons
            // 0x100/0x300) left by a per-column offset so the numbers sit back in the middle of
            // their columns. Offsets are design units (measured at scale 1.0) scaled to the live
            // resolution. Set (not accumulate) so it is idempotent across frames.
            {
                const int32_t cnt = (n < s.baseCount) ? n : s.baseCount;
                const float nscale = (s.listW > 1.0f) ? (s.listW / kDesignListW) : 1.0f;
                static const float kNumCenterShift[3] = {22.0f, 28.0f, 0.0f}; // [sell, buy, amount]
                for (int32_t i = 0; i < cnt; ++i) {
                    auto* row = items[i];
                    if (!SanePtr(reinterpret_cast<uintptr_t>(row)))
                        continue;
                    Wnd* num[3];
                    int nc = 0;
                    for (auto* c = row->m_firstChild; c && nc < 3; c = c->m_nextSibling) {
                        auto* wc = reinterpret_cast<Wnd*>(c);
                        if (!SanePtr(reinterpret_cast<uintptr_t>(wc)))
                            continue;
                        if ((wc->m_style & 0x400u) != 0)
                            num[nc++] = wc;
                    }
                    if (nc != 3)
                        continue;
                    // Order by x0 ascending: sell (smallest) < buy < amount (largest).
                    for (int a = 0; a < 3; ++a)
                        for (int b = a + 1; b < 3; ++b)
                            if (num[b]->m_bounds.x0 < num[a]->m_bounds.x0) {
                                Wnd* t = num[a]; num[a] = num[b]; num[b] = t;
                            }
                    if (!s.numBaseCaptured) {
                        for (int j = 0; j < 3; ++j)
                            s.baseNumX0[j] = num[j]->m_bounds.x0;
                        s.numBaseCaptured = true;
                    }
                    for (int j = 0; j < 3; ++j)
                        num[j]->m_bounds.x0 = s.baseNumX0[j] - kNumCenterShift[j] * nscale;
                }
            }

            ClampScroll(s);

            // Adopt a thumb drag that happened since the previous frame (drag->content).
            if (s.bar) {
                const float px = BarPositionPx(s.bar);
                if (std::fabs(px - s.lastPushedPx) > 0.5f) {
                    s.scrollY = px;
                    ClampScroll(s);
                }
            }

            if (MaxScroll(s) > 0.0f)
                EnsureBar(list, s);

            // Track window resize: Recapture only fires on item-set change, so a pure resize is
            // caught here. If the live list width moved, refresh the derived geometry and
            // reposition the bar so the reserved column and the (narrowed) rows stay in sync.
            const float liveW = list->m_bounds.width;
            if (std::isfinite(liveW) && std::fabs(liveW - s.listW) > 0.5f) {
                s.listW = liveW;
                s.viewportH = (s.listW > 1.0f)
                    ? kDesignViewportH * (s.listW / kDesignListW) : kDesignViewportH;
                if (s.bar && s.barW > 1.0f) {
                    const float barX = s.listW - s.barW - kBarRightInset;
                    BoundsBase<float> b{ barX, 0.0f, s.barW, s.viewportH };
                    s.bar->SetBounds(b, true);
                    s.lastBarX = barX;
                }
                ClampScroll(s);
            }

            // Shift the rows up by the current offset. The engine's fixed viewport
            // clip trims both edges (confirmed by the 11th row clipping unmodified).
            ApplyShift(s, items, n);

            // Push the offset back into the native scrollbar (wheel-driven case).
            if (s.bar && MaxScroll(s) > 0.0f) {
                s.bar->EnableWindow(true);
                s.bar->ShowWindow(true);
                s.bar->SetScrollRect(s.contentH, 0.0f);
                // SetScrollRect computes m_maxPos = contentH - viewportH but only STORES it when
                // the field is NaN (ucomiss/lahf guard). The field initialises to 0, not NaN, so
                // the store never fires and m_maxPos stays 0 - which deads the whole native bar:
                // the mouse handlers set a valid m_curPos [0,1] fraction, but BarPositionPx
                // (m_curPos * m_maxPos) reads 0 so Drive never adopts it into s.scrollY (rows
                // don't move), and SetCurPos(px) with m_maxPos==0 just forces m_curPos=0 (thumb
                // snaps back, wheel not reflected). Establish the range directly, authoritative
                // (after SetScrollRect, before SetCurPos which reads it).
                s.bar->m_maxPos = MaxScroll(s);
                s.bar->SetCurPos(s.scrollY);
                s.lastPushedPx = BarPositionPx(s.bar);
            }
        }
    }

    int32_t Hook_WareListOnPaint(Wnd* self, const DrawInfo& di) {
#if KRAKEN_TESTS
        NoteTargetCandidate(self, ItemCount(self));
#endif // KRAKEN_TESTS
        Drive(self, di);
        return self->Wnd::OnPaint(di);
    }

    // Advance s.scrollY by `delta` wheel units. The engine may hand a sub-notch delta (a fine /
    // normalised wheel value), which the old delta/120.0 scale turned into almost no movement.
    // Guarantee at least one notch per event so a single tick always advances kWheelRowsPerNotch
    // rows; a standard 120-unit wheel gives exactly one notch (unchanged behaviour). Shared by
    // both wheel hooks so they step identically.
    void StepWheel(State& s, int32_t delta) {
        if (s.pitch <= 0.0f || delta == 0)
            return;
        const int32_t adelta = (delta < 0) ? -delta : delta;
        const int32_t notches = (adelta >= 120) ? (adelta / 120) : 1;
        const int32_t dir = (delta > 0) ? 1 : -1;
        s.scrollY -= static_cast<float>(dir * notches) * kWheelRowsPerNotch * s.pitch;
        ClampScroll(s);
    }

    int32_t Hook_WareListOnWheel(Wnd* self, int32_t delta, const PointBase<float>& point) {
        (void) point;
        State& s = StateFor(self);
        StepWheel(s, delta);
        // Apply the shift now rather than waiting for the next engine paint: the shop panel can
        // repaint at a throttled rate while open, and deferring the wheel there makes it feel laggy.
        Wnd* items[kMaxItems] = {nullptr};
        ApplyShift(s, items, ReadItems(self, items, kMaxItems));
        return 1;
    }

    int32_t Hook_WareItemOnWheel(Wnd* self, int32_t delta, const PointBase<float>& point) {
        (void) point;
        auto* list = static_cast<Wnd*>(self->m_parent);
        if (!list)
            return 0;
        State& s = StateFor(list);
        StepWheel(s, delta);
        Wnd* items[kMaxItems] = {nullptr};
        ApplyShift(s, items, ReadItems(list, items, kMaxItems));
        return 1;
    }

    // WareList::CreateItems (warewnd.cpp:1374) hard-caps the goods list to the first 10
    // prototypes: `if (ids.size() > 10) ids.resize(10)`. The guard is `cmp eax,0xa; jbe skip`
    // (byte 0x76) at VA 0x47FD8D; turning the conditional jump into an unconditional `jmp`
    // (0xEB) removes the cap. rel8 (0x10) is untouched. The loop then shows every ware the
    // shop actually stocks (the per-warehouse GetArticle/IsSellable check still filters).
    // This is the same path vanilla takes for <=10 goods, so it is well-exercised.
    void Apply(const Config* config) {
        if (!config->warescroll_enabled.value) {
            LOG_INFO("warescroll disabled by config ([warescroll] enabled=0 in data/kraken.ini); skipping all patches");
            return;
        }
        routines::OverrideValue(reinterpret_cast<void*>(kWareListOnPaintSlot),
                                &WareList_Hooked::OnPaint);
        routines::OverrideValue(reinterpret_cast<void*>(kWareListWheelSlot),
                                &WareList_Hooked::OnMouseWheel);
        routines::OverrideValue(reinterpret_cast<void*>(kWareItemWheelSlot),
                                &WareItem_Hooked::OnMouseWheel);
        routines::OverrideValue(reinterpret_cast<void*>(0x0047FD8D), (uint8_t)0xEB);
        LOG_INFO("patched 3 vtable slots (WareList OnPaint/OnMouseWheel, WareItem OnMouseWheel)");
        LOG_INFO("patched WareList::CreateItems cap: jbe->jmp @ 0x47FD8D (goods list no longer capped at 10)");
    }
}
