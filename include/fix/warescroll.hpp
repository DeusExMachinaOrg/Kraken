#pragma once

#include <cstdint>

#include "hta/PointBase.hpp"
#include "hta/m3d/ui/DrawInfo.hpp"
#include "hta/m3d/ui/Wnd.hpp"

#include "config.hpp"

namespace kraken::fix::warescroll {
    int32_t Hook_WareListOnPaint(hta::m3d::ui::Wnd* self, const hta::m3d::ui::DrawInfo& drawInfo);
    int32_t Hook_WareListOnWheel(hta::m3d::ui::Wnd* self, int32_t delta,
                                  const hta::PointBase<float>& point);
    int32_t Hook_WareItemOnWheel(hta::m3d::ui::Wnd* self, int32_t delta,
                                  const hta::PointBase<float>& point);
    int32_t Hook_WareListOnMouseButton0(hta::m3d::ui::Wnd* self, uint32_t state,
                                        const hta::PointBase<float>& at);
    int32_t Hook_WareListOnMouseMove(hta::m3d::ui::Wnd* self,
                                     const hta::PointBase<float>& pt,
                                     const hta::PointBase<float>& deltas);

    // ABI adapters: non-static members compile as __thiscall on the target build,
    // which is the calling convention the patched vtable slots dispatch through.
    struct WareList_Hooked {
        int32_t OnPaint(const hta::m3d::ui::DrawInfo& drawInfo) {
            return Hook_WareListOnPaint(reinterpret_cast<hta::m3d::ui::Wnd*>(this), drawInfo);
        }
        int32_t OnMouseWheel(int32_t delta, const hta::PointBase<float>& point) {
            return Hook_WareListOnWheel(reinterpret_cast<hta::m3d::ui::Wnd*>(this), delta, point);
        }
        // #3: the list's OnMouseButton0 (vtable+0xa4) / OnMouseMove (vtable+0xb0) slots, patched
        // to handle bar-column input (the bar is a visible leaf the station's hit-test skips).
        int32_t OnMouseButton0(uint32_t state, const hta::PointBase<float>& at) {
            return Hook_WareListOnMouseButton0(reinterpret_cast<hta::m3d::ui::Wnd*>(this), state, at);
        }
        int32_t OnMouseMove(const hta::PointBase<float>& pt, const hta::PointBase<float>& deltas) {
            return Hook_WareListOnMouseMove(reinterpret_cast<hta::m3d::ui::Wnd*>(this), pt, deltas);
        }
    };

    struct WareItem_Hooked {
        int32_t OnMouseWheel(int32_t delta, const hta::PointBase<float>& point) {
            return Hook_WareItemOnWheel(reinterpret_cast<hta::m3d::ui::Wnd*>(this), delta, point);
        }
    };

    void Apply(const Config* config);
}
