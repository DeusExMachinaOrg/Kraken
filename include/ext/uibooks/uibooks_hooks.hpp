#pragma once

#include <cstdint>

#include "hta/BooksWnd.hpp"
#include "hta/CStr.hpp"
#include "hta/PointBase.hpp"
#include "hta/m3d/AIParam.hpp"
#include "hta/m3d/ui/DrawInfo.hpp"
#include "hta/m3d/ui/TextBoxWnd.hpp"
#include "hta/m3d/ui/Wnd.hpp"

namespace kraken::ext::uibooks {
    int32_t Hook_BooksOnWndNotify(void* self, hta::m3d::ui::Wnd* src,
                                   uint32_t a, uint32_t b, const hta::m3d::AIParam& param);
    int32_t Hook_BoxOnWndNotify(hta::m3d::ui::TextBoxWnd* self, hta::m3d::ui::Wnd* src,
                                uint32_t a, uint32_t b, const hta::m3d::AIParam& param);
    int32_t Hook_SetText(hta::m3d::ui::TextBoxWnd* self, const hta::CStr& text);
    int32_t Hook_OnBeforeRemoveFromWndStation(hta::m3d::ui::TextBoxWnd* self);
    int32_t Hook_OnPaint(hta::m3d::ui::TextBoxWnd* self, const hta::m3d::ui::DrawInfo& drawInfo);
    int32_t Hook_OnMouseButton0(hta::m3d::ui::TextBoxWnd* self, uint32_t buttons,
                                const hta::PointBase<float>& point);
    int32_t Hook_OnMouseWheel(hta::m3d::ui::TextBoxWnd* self, int32_t delta,
                              const hta::PointBase<float>& point);

    // ABI adapters: non-static members compile as __thiscall on the target build,
    // which is the calling convention required by the patched vtable slots.
    struct TextBoxWnd_Hooked {
        int32_t SetText(const hta::CStr& text) {
            return Hook_SetText(reinterpret_cast<hta::m3d::ui::TextBoxWnd*>(this), text);
        }
        int32_t OnBeforeRemoveFromWndStation() {
            return Hook_OnBeforeRemoveFromWndStation(
                reinterpret_cast<hta::m3d::ui::TextBoxWnd*>(this));
        }
        int32_t OnPaint(const hta::m3d::ui::DrawInfo& drawInfo) {
            return Hook_OnPaint(reinterpret_cast<hta::m3d::ui::TextBoxWnd*>(this), drawInfo);
        }
        int32_t OnMouseButton0(uint32_t buttons, const hta::PointBase<float>& point) {
            return Hook_OnMouseButton0(reinterpret_cast<hta::m3d::ui::TextBoxWnd*>(this), buttons, point);
        }
        int32_t OnMouseWheel(int32_t delta, const hta::PointBase<float>& point) {
            return Hook_OnMouseWheel(reinterpret_cast<hta::m3d::ui::TextBoxWnd*>(this), delta, point);
        }
        int32_t OnWndNotify(hta::m3d::ui::Wnd* src, uint32_t a, uint32_t b,
                            const hta::m3d::AIParam& param) {
            return Hook_BoxOnWndNotify(
                reinterpret_cast<hta::m3d::ui::TextBoxWnd*>(this), src, a, b, param);
        }
    };

    struct BooksWnd_Hooked {
        BooksWnd_Hooked() = delete;

        int32_t OnWndNotify(hta::m3d::ui::Wnd* src, uint32_t a, uint32_t b,
                            const hta::m3d::AIParam& param) {
            return Hook_BooksOnWndNotify(reinterpret_cast<hta::BooksWnd*>(this), src, a, b, param);
        }
    };
}
