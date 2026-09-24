#pragma once

#include <cstdint>

namespace kraken::fix::warescroll {
    // Automated-test API for the warescroll feature (driven by the uibookstest "ware" trigger).
    // Kept out of the public warescroll.hpp so the production feature header stays clean.

    // Snapshot of the auto-detected goods WareList (the list with the most items), read from the
    // live per-list scroll state. Used by the automated uibookstest "ware" driver.
    struct TestReport {
        bool    targetFound = false;
        int32_t count = 0;        // item count in the target list
        float   maxScroll = 0.0f; // contentH - viewportH, clamped >= 0
        float   scrollY = 0.0f;   // current scroll offset (px)
        float   contentH = 0.0f;
        float   viewportH = 0.0f;
        float   pitch = 0.0f;     // row height (px)
        float   item0BaseTop = 0.0f; // items[0] unscrolled y0
        float   item0TopNow = 0.0f;  // items[0].m_bounds.y0 after the latest Drive
        bool    hasBar = false;   // a ScrollWnd thumb is attached
        bool    initialized = false;
    };

    // Fill `out` from the target list. Returns true only if a target was detected.
    bool  Test_GetReport(TestReport& out);
    // Advance the target's scroll like the wheel hook (delta in 120-wheeldelta units;
    // negative = scroll down). Returns the resulting scrollY (px).
    float Test_StepWheel(int32_t delta120);
    // Snap the target's scroll back to the top.
    void  Test_Reset();
    // #3 diagnostic: ask the station's hit-test which list child owns a point placed at the
    // scrollbar's own centre, and log the result (does the click route to the bar, or is it
    // shadowed by an un-narrowed row?). Pure query; safe to call while the window is open.
    void  Test_HitTestOnBar();
    // #3 fix verification: drive the bar's arrow/drag math with synthetic list-local points and
    // log the result. Returns true if the down-arrow step, the top->bottom thumb drag, and the
    // out-of-column rejection all behaved. Pure (no real mouse); the routing itself is confirmed
    // by the list-mouse hook logs on a genuine click.
    bool  Test_BarSimulate();
}
