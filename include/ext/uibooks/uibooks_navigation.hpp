#pragma once

#include <cstdint>

#include "ext/uibooks/uibooks_state.hpp"

namespace kraken::ext::uibooks::navigation {
    struct Layout {
        float prevX = 0.0f;
        float captionX = 0.0f;
        float nextX = 0.0f;
        float y = 0.0f;
        float buttonW = 0.0f;
        float buttonH = 0.0f;
    };

    float FooterHeight();
    float DefaultCaptionWidth();

    Layout GetLayout(float clientW, float clientH, float bandW, float captionW);

    void EnsureButtons(FooterButtons& buttons, hta::m3d::ui::TextBoxWnd* box,
                       float clientW, float clientH, float bandW, float captionW);
    bool DestroyButtons(FooterButtons& buttons);
    void SyncButtons(const FooterButtons& buttons, hta::m3d::ui::TextBoxWnd* box,
                     float clientW, float clientH, float bandW, float captionW,
                     int32_t pageCount, int32_t currentPage);
    bool IsActive(const FooterButtons& buttons, const hta::m3d::ui::TextBoxWnd* box);
}
