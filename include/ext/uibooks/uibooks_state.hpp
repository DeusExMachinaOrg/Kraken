#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ext/uibooks/uibooks.hpp"
#include "ext/uibooks/uibooks_parser.hpp"
#include "ext/uibooks/uibooks_resources.hpp"

#include "hta/m3d/ui/ButtonWnd.hpp"
#include "hta/m3d/ui/ScrollWnd.hpp"
#include "hta/m3d/ui/TextBoxWnd.hpp"

namespace kraken::ext::uibooks {
    // Engine-owned footer controls belonging to one book textbox.
    struct FooterButtons {
        hta::m3d::ui::ButtonWnd* prevBtn = nullptr;
        hta::m3d::ui::ButtonWnd* nextBtn = nullptr;
        hta::m3d::ui::TextBoxWnd* parentBox = nullptr;
    };

    struct BookState {
        BookState() = default;
        BookState(const BookState&) = delete;
        BookState& operator=(const BookState&) = delete;
        BookState(BookState&&) = delete;
        BookState& operator=(BookState&&) = delete;

        void ReleaseImageTextures() noexcept {
            for (uint32_t& handle : imageHandles)
                resources::ReleaseTexture(&handle);
        }

        // Engine resources must be released while the owning UI/renderer is
        // still alive. The state map can outlive the engine during DLL teardown,
        // so its destructor must not call back into the engine.
        ~BookState() = default;

        hta::m3d::ui::TextBoxWnd* box = nullptr;

        std::vector<ParsedLine> lines;
        std::vector<bool> lineBreak;
        int32_t align = hta::m3d::TF_LEFT;
        BookMode mode = BookMode::Scroll;
        uint32_t styleMask = 0;
        int32_t styledSegs = 0;
        int32_t coloredSegs = 0;
        std::vector<uint32_t> colors;
        std::string cleanText;

        bool ready = false;
        bool anchorFellBack = false;

        int32_t fontId = 0;
        int32_t styleFontId[4] = { -1, -1, -1, -1 };
        bool styleNeedsSyntheticBold[4] = { false, false, false, false };
        bool styleFontsInitialized = false;

        // Client geometry captured during the last OnPaint. Drawing and mouse
        // coordinates remain client-relative; x0/y0 are retained for the test API.
        float clientX0 = 0.0f;
        float clientY0 = 0.0f;
        float clientW = 0.0f;
        float clientH = 0.0f;
        float lineH = 0.0f;
        float top0 = 0.0f;
        float leftPad = 0.0f;
        float visibleH = 0.0f;

        int32_t rowsPerPage = 0;
        std::vector<int32_t> lineRows;
        std::vector<uint32_t> imageHandles;
        std::vector<float> imageWidths;
        std::vector<float> imageHeights;
        std::vector<int32_t> pageStart;
        std::vector<int32_t> pageEnd;
        int32_t explicitBreaks = 0;

        int32_t curPage = 0;
        float scrollY = 0.0f;
        float footerCaptionW = 0.0f;

        // The engine's own ScrollWnd is synchronized with the custom page offset
        // after the original TextBoxWnd paint has rebuilt its layout.
        hta::m3d::ui::ScrollWnd* vscroll = nullptr;
        float lastPushedPx = 0.0f;
        bool scrollSynced = false;
        std::string lastText;

        FooterButtons navBtns;
    };
}
