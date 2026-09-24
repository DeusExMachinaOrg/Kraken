#define LOGGER "uibooks"

#include "ext/uibooks/uibooks_navigation.hpp"

#include <cmath>

#include "ext/logger.hpp"
#include "ext/uibooks/uibooks_resources.hpp"

#include "hta/BoundsBase.hpp"
#include "hta/CStr.hpp"
#include "hta/m3d/Enums.hpp"
#include "hta/m3d/rend/IRenderer.hpp"
#include "hta/m3d/ui/ButtonWnd.hpp"

namespace kraken::ext::uibooks::navigation {
    using hta::m3d::ui::ButtonWnd;

    namespace {
        constexpr float FOOTER_HEIGHT = 36.0f;
        constexpr float BUTTON_SIZE = 32.0f;
        constexpr float BUTTON_MARGIN = 2.0f;
        constexpr float FOOTER_GAP = 8.0f;
        constexpr float DEFAULT_CAPTION_WIDTH = 48.0f;

        void DestroyUnattached(ButtonWnd* button) {
            if (!button)
                return;
            button->ReleaseTextures();
            if (button->Valid())
                (void) button->DestroyWnd();
            button->DecRef();
        }

        bool DestroyAttachedButton(ButtonWnd*& button, hta::m3d::ui::Wnd* parent) {
            if (!button)
                return true;

            if (parent && button->m_parent == parent) {
                button->ReleaseTextures();
                if (!parent->RemoveChildForce(button)) {
                    LOG_ERROR("book nav button %p could not be removed from parent %p",
                              button, parent);
                    return false;
                }
            }
            else if (!button->m_parent) {
                DestroyUnattached(button);
            }
            else {
                LOG_ERROR("book nav button %p belongs to unexpected parent %p (expected %p)",
                          button, button->m_parent, parent);
                return false;
            }
            button = nullptr;
            return true;
        }

        bool DestroyAttached(FooterButtons& buttons) {
            bool removed = true;
            if (!buttons.parentBox) {
                removed = DestroyAttachedButton(buttons.prevBtn, nullptr) && removed;
                removed = DestroyAttachedButton(buttons.nextBtn, nullptr) && removed;
                return removed;
            }
            removed = DestroyAttachedButton(buttons.prevBtn, buttons.parentBox) && removed;
            removed = DestroyAttachedButton(buttons.nextBtn, buttons.parentBox) && removed;
            if (!buttons.prevBtn && !buttons.nextBtn)
                buttons.parentBox = nullptr;
            return removed;
        }

        ButtonWnd* CreateButton(const char* name,
                                float x, float y, float w, float h,
                                const char* pane, const char* regularTexture,
                                const char* inTexture, const char* downTexture) {
            auto* button = static_cast<ButtonWnd*>(ButtonWnd::CreateObject());
            if (!button) {
                LOG_WARNING("book nav button '%s': engine allocator returned null", name);
                return nullptr;
            }

            const hta::CStr caption("");
            const hta::BoundsBase<float> bounds{x, y, w, h};
            // Use the engine-assigned window identity instead of inventing a
            // private numeric ID. Navigation is additionally routed by the
            // source button pointer in Hook_BoxOnWndNotify.
            const int32_t uniqueId = button->GetUniqueId();
            if (button->CreateWnd(caption, hta::m3d::ui::WND_STYLE_MAP_BUTTON,
                                  bounds, static_cast<uint32_t>(uniqueId)) != 1
                || !button->Valid()) {
                LOG_WARNING("book nav button '%s': engine CreateWnd failed - glyph fallback stays", name);
                DestroyUnattached(button);
                return nullptr;
            }

            button->SetPane(hta::CStr(pane));
            uint32_t regular = resources::LoadTexture(regularTexture);
            uint32_t inside = resources::LoadTexture(inTexture);
            uint32_t down = resources::LoadTexture(downTexture);
            if (!regular) {
                LOG_WARNING("book nav button '%s': no regular arrow texture - pane only", name);
                resources::ReleaseTexture(&inside);
                resources::ReleaseTexture(&down);
            }
            button->m_isInside = 0;
            if (!regular)
                return button;

            const int32_t result = button->SetImaged(
                hta::m3d::rend::TexHandle(static_cast<int32_t>(regular)),
                hta::m3d::rend::TexHandle(down ? static_cast<int32_t>(down) : -1),
                hta::m3d::rend::TexHandle(inside ? static_cast<int32_t>(inside) : -1),
                hta::m3d::rend::TexHandle(-1));
            if (result == 1)
                return button;

            LOG_WARNING("book nav button '%s': SetImaged failed (%d)", name, result);
            button->ReleaseTextures();
            (void) button->DestroyWnd();
            button->DecRef();
            return nullptr;
        }

        void PlaceButton(ButtonWnd* button, float x, float y, float w, float h,
                         bool visible, bool disabled) {
            if (!button)
                return;
            button->m_bounds.x0 = x;
            button->m_bounds.y0 = y;
            button->m_bounds.width = w;
            button->m_bounds.height = h;
            button->m_baseOrigin.x = x;
            button->m_baseOrigin.y = y;
            uint32_t style = button->m_style;
            style = visible ? (style | hta::m3d::ui::WND_STYLE_VISIBLE)
                            : (style & ~hta::m3d::ui::WND_STYLE_VISIBLE);
            style = disabled ? (style | hta::m3d::ui::WND_STYLE_DISABLED)
                             : (style & ~hta::m3d::ui::WND_STYLE_DISABLED);
            button->m_style = style;
        }
    }

    float FooterHeight() {
        return FOOTER_HEIGHT;
    }

    float DefaultCaptionWidth() {
        return DEFAULT_CAPTION_WIDTH;
    }

    Layout GetLayout(float clientW, float clientH, float bandW, float captionW) {
        Layout layout;
        layout.buttonW = BUTTON_SIZE;
        layout.buttonH = BUTTON_SIZE;
        layout.y = clientH - layout.buttonH - BUTTON_MARGIN;

        const float availableW = (clientW - bandW > 0.0f) ? (clientW - bandW) : 0.0f;
        const float counterW = (std::isfinite(captionW) && captionW > 0.0f)
            ? captionW : DEFAULT_CAPTION_WIDTH;
        const float groupW = layout.buttonW + FOOTER_GAP + counterW
                           + FOOTER_GAP + layout.buttonW;
        float groupX = (availableW - groupW) * 0.5f;
        if (groupX < BUTTON_MARGIN)
            groupX = BUTTON_MARGIN;

        layout.prevX = groupX;
        layout.captionX = groupX + layout.buttonW + FOOTER_GAP;
        layout.nextX = layout.captionX + counterW + FOOTER_GAP;
        return layout;
    }

    void EnsureButtons(FooterButtons& buttons, hta::m3d::ui::TextBoxWnd* box,
                       float clientW, float clientH, float bandW, float captionW) {
        if (buttons.parentBox == box && buttons.prevBtn && buttons.nextBtn)
            return;
        if (buttons.prevBtn || buttons.nextBtn) {
            if (!DestroyButtons(buttons))
                return;
        }
        const Layout layout = GetLayout(clientW, clientH, bandW, captionW);
        buttons.prevBtn = CreateButton(
            "btnBookPrev",
            layout.prevX, layout.y, layout.buttonW, layout.buttonH,
            "PaneArrowGrayLeft3", "data\\if\\ico\\LocalMap\\scroll_left_e.dds",
            "data\\if\\ico\\LocalMap\\scroll_left_s.dds",
            "data\\if\\ico\\LocalMap\\scroll_left_p.dds");
        buttons.nextBtn = CreateButton(
            "btnBookNext",
            layout.nextX, layout.y, layout.buttonW, layout.buttonH,
            "PaneArrowGrayRight3", "data\\if\\ico\\LocalMap\\scroll_right_e.dds",
            "data\\if\\ico\\LocalMap\\scroll_right_s.dds",
            "data\\if\\ico\\LocalMap\\scroll_right_p.dds");
        if (!buttons.prevBtn || !buttons.nextBtn) {
            LOG_ERROR("book nav buttons not created (prev %p, next %p) - glyph fallback stays",
                      buttons.prevBtn, buttons.nextBtn);
            DestroyUnattached(buttons.prevBtn);
            DestroyUnattached(buttons.nextBtn);
            buttons.prevBtn = nullptr;
            buttons.nextBtn = nullptr;
            return;
        }

        const bool addPrev = box->AddChild(buttons.prevBtn);
        const bool addNext = box->AddChild(buttons.nextBtn);
        if (!addPrev || !addNext) {
            LOG_ERROR("book nav buttons not attached (prev %d, next %d) - glyph fallback stays",
                      addPrev ? 1 : 0, addNext ? 1 : 0);
            if (addPrev) {
                buttons.prevBtn->ReleaseTextures();
                if (box->RemoveChildForce(buttons.prevBtn))
                    buttons.prevBtn = nullptr;
            }
            else {
                DestroyUnattached(buttons.prevBtn);
                buttons.prevBtn = nullptr;
            }
            if (addNext) {
                buttons.nextBtn->ReleaseTextures();
                if (box->RemoveChildForce(buttons.nextBtn))
                    buttons.nextBtn = nullptr;
            }
            else {
                DestroyUnattached(buttons.nextBtn);
                buttons.nextBtn = nullptr;
            }
            if (buttons.prevBtn || buttons.nextBtn)
                buttons.parentBox = box;
            else
                buttons.parentBox = nullptr;
            return;
        }
        buttons.parentBox = box;
        LOG_INFO("book nav buttons created, centered around page counter, %0.fx%0.f px",
                 layout.buttonW, layout.buttonH);
    }

    bool DestroyButtons(FooterButtons& buttons) {
        const bool removed = DestroyAttached(buttons);
        if (removed) {
            buttons.prevBtn = nullptr;
            buttons.nextBtn = nullptr;
            buttons.parentBox = nullptr;
        }
        return removed;
    }

    void SyncButtons(const FooterButtons& buttons, hta::m3d::ui::TextBoxWnd* box,
                     float clientW, float clientH, float bandW, float captionW,
                     int32_t pageCount, int32_t currentPage) {
        if (!buttons.prevBtn || !buttons.nextBtn || buttons.parentBox != box)
            return;
        const Layout layout = GetLayout(clientW, clientH, bandW, captionW);
        const bool show = pageCount >= 2;
        PlaceButton(buttons.prevBtn, layout.prevX, layout.y, layout.buttonW, layout.buttonH,
                    show, !show || currentPage <= 0);
        PlaceButton(buttons.nextBtn, layout.nextX, layout.y, layout.buttonW, layout.buttonH,
                    show, !show || currentPage >= pageCount - 1);
    }

    bool IsActive(const FooterButtons& buttons, const hta::m3d::ui::TextBoxWnd* box) {
        return buttons.prevBtn && buttons.nextBtn && buttons.parentBox == box;
    }
}
