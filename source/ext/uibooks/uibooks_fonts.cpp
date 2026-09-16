#define LOGGER "uibooks"

#include "ext/uibooks/uibooks_fonts.hpp"

#include <cmath>
#include <new>
#include <vector>

#include "ext/uibooks/uibooks_constants.hpp"
#include "hta/m3d/Enums.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/ui/Font.hpp"
#include "hta/m3d/ui/FontManager.hpp"
#include "hta/m3d/ui/FontParams.hpp"
#include "hta/m3d/ui/GfxServer.hpp"
#include "hta/m3d/ui/Wnd.hpp"

namespace kraken::ext::uibooks::fonts {
    namespace {
        int32_t FindStyleFont(hta::m3d::ui::GfxServer* gfx, int32_t baseFontId, uint32_t style) {
            hta::m3d::ui::Font* font = gfx->GetFontById((uint32_t) baseFontId);
            if (!font)
                return -1;
            hta::m3d::ui::FontParams params(style, constants::BOOK_TEXT_CODE_PAGE_WINDOWS_1251);
            const int32_t id = gfx->GetFontId(font->m_nameShort, font->m_heightUnscaled,
                                              font->m_type, params);
            const hta::m3d::ui::Font* styled = id >= 0
                ? gfx->GetFontById((uint32_t) id) : nullptr;
            return styled && styled->m_style == style ? id : -1;
        }

        int32_t StyleSlot(uint32_t style) {
            switch (style) {
                case hta::m3d::FONT_BOLD:
                    return 1;
                case hta::m3d::FONT_ITALIC:
                    return 2;
                case hta::m3d::FONT_BOLD | hta::m3d::FONT_ITALIC:
                    return 3;
                default:
                    return -1;
            }
        }

        const char* SystemFontFileName(uint32_t style) {
            switch (style) {
                case hta::m3d::FONT_BOLD:
                    return "arialbd.ttf";
                case hta::m3d::FONT_ITALIC:
                    return "ariali.ttf";
                case hta::m3d::FONT_BOLD | hta::m3d::FONT_ITALIC:
                    return "arialbi.ttf";
                default:
                    return nullptr;
            }
        }

        std::string SystemFontPath(uint32_t style) {
            const char* fileName = SystemFontFileName(style);
            if (!fileName)
                return {};

            std::vector<char> windowsDirectory(MAX_PATH);
            UINT length = GetWindowsDirectoryA(
                windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
            if (length == 0)
                return {};
            if (length >= windowsDirectory.size()) {
                windowsDirectory.resize(static_cast<size_t>(length) + 1);
                length = GetWindowsDirectoryA(
                    windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
                if (length == 0 || length >= windowsDirectory.size())
                    return {};
            }

            std::string path(windowsDirectory.data(), length);
            if (path.empty() || path.back() != '\\')
                path += '\\';
            path += "Fonts\\";
            path += fileName;
            return path;
        }

        int32_t AppendStyleFont(hta::m3d::ui::GfxServer* gfx, int32_t baseFontId,
                                uint32_t style) {
            hta::m3d::ui::FontManager* manager = gfx ? gfx->m_fontManager : nullptr;
            if (!manager)
                return -1;
            const hta::m3d::ui::Font* base = gfx->GetFontById((uint32_t) baseFontId);
            const std::string path = SystemFontPath(style);
            if (!base || path.empty())
                return -1;

            hta::m3d::Kernel* kernel = hta::m3d::Kernel::Instance();
            if (!kernel)
                return -1;
            void* memory = kernel->g_mar.AllocMem(sizeof(hta::m3d::ui::Font), "", 0);
            if (!memory)
                return -1;
            hta::m3d::ui::Font* font = new (memory) hta::m3d::ui::Font();
            const int32_t result = font->CreateFromTtf(
                hta::CStr(path.c_str()), base->m_heightUnscaled, style,
                constants::BOOK_TEXT_CODE_PAGE_WINDOWS_1251);
            if (result < 0 || font->m_nameFull.empty()
                || !std::isfinite(font->m_heightUnscaled)
                || font->m_heightUnscaled <= 0.0f || font->m_symbols.empty())
            {
                font->~Font();
                kernel->g_mar.FreeMem(font, "", 0);
                return -1;
            }

            const int32_t fontId = manager->AddFont(font);
            if (fontId < 0)
            {
                font->~Font();
                kernel->g_mar.FreeMem(font, "", 0);
                return -1;
            }
            return fontId;
        }

        void ResolveStyleFont(BookState& state, hta::m3d::ui::GfxServer* gfx,
                              uint32_t style) {
            const int32_t slot = StyleSlot(style);
            if (slot < 0 || !gfx)
                return;

            const int32_t nativeFontId = FindStyleFont(gfx, state.fontId, style);
            const int32_t fontId = nativeFontId >= 0
                ? nativeFontId : AppendStyleFont(gfx, state.fontId, style);
            state.styleFontId[slot] = fontId >= 0 ? fontId : state.fontId;
            state.styleNeedsSyntheticBold[slot] = fontId < 0
                && (style & hta::m3d::FONT_BOLD) != 0;
        }
    }

    int32_t EnsureStyleFont(BookState& state, uint32_t style) {
        if (!style)
            return state.fontId;
        const int32_t styleSlot = StyleSlot(style);
        if (styleSlot < 0)
            return state.fontId;
        int32_t& slot = state.styleFontId[styleSlot];
        if (slot < 0) {
            hta::m3d::ui::GfxServer* gfx = hta::m3d::ui::Wnd::GetGfxServer();
            if (!state.styleFontsInitialized && gfx) {
                state.styleFontsInitialized = true;
                for (uint32_t candidate = 1; candidate < 4; ++candidate)
                    ResolveStyleFont(state, gfx, candidate);
            }
            if (slot < 0 && gfx)
                ResolveStyleFont(state, gfx, style);
        }
        return slot >= 0 ? slot : state.fontId;
    }

    bool UsesSyntheticBold(const BookState& state, uint32_t style) {
        const int32_t slot = StyleSlot(style);
        return slot >= 0 && state.styleNeedsSyntheticBold[slot];
    }
}
