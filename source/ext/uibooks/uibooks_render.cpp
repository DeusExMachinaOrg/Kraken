#define LOGGER "uibooks"

#include "ext/uibooks/uibooks_render.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ext/logger.hpp"
#include "ext/uibooks/uibooks_constants.hpp"
#include "ext/uibooks/uibooks_fonts.hpp"
#include "ext/uibooks/uibooks_navigation.hpp"
#include "ext/uibooks/uibooks_scroll.hpp"

#include "hta/BoundsBase.hpp"
#include "hta/CStr.hpp"
#include "hta/PointBase.hpp"
#include "hta/m3d/Enums.hpp"
#include "hta/m3d/rend/IRenderer.hpp"
#include "hta/m3d/ui/Wnd.hpp"

namespace kraken::ext::uibooks::render {
    namespace {
        std::string ColorToken(uint32_t color) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "@%08x", color);
            return buffer;
        }

        void DrawFooter(BookState& state, hta::m3d::ui::GfxServer* gfx,
                        const hta::m3d::ui::DrawInfo& drawInfo, const char* colorToken) {
            const float footerTop = state.clientH - navigation::FooterHeight();
            const float y = footerTop + (navigation::FooterHeight() - state.lineH) * 0.5f;

            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%d/%d", state.curPage + 1,
                          static_cast<int32_t>(state.pageStart.size()));
            std::string captionText = colorToken;
            captionText += buffer;
            const hta::CStr caption(captionText.c_str());
            const hta::PointBase<float> measured = gfx->MeasureText(
                caption, state.fontId, hta::m3d::TW_NOWRAP, state.clientW);
            state.footerCaptionW = (std::isfinite(measured.x) && measured.x > 0.0f)
                ? measured.x : navigation::DefaultCaptionWidth();
            const float bandW = scroll::BandWidth(state.vscroll);
            const navigation::Layout layout = navigation::GetLayout(
                state.clientW, state.clientH, bandW, state.footerCaptionW);
            gfx->AddText(drawInfo, hta::PointBase<float>{ layout.captionX, y }, caption,
                         state.fontId, hta::m3d::TW_NOWRAP, hta::m3d::TF_LEFT);

            if (navigation::IsActive(state.navBtns, state.box))
                return;

            const auto glyphWidth = [&](const char* glyph) {
                const hta::PointBase<float> measured = gfx->MeasureText(
                    hta::CStr(glyph), state.fontId, hta::m3d::TW_NOWRAP, state.clientW);
                return (std::isfinite(measured.x) && measured.x > 0.0f) ? measured.x : 0.0f;
            };
            const float prevGlyphWidth = glyphWidth("<");
            float glyphX = layout.prevX + (layout.buttonW - prevGlyphWidth) * 0.5f;
            std::string glyph = colorToken;
            glyph += "<";
            gfx->AddText(drawInfo, hta::PointBase<float>{ glyphX, y }, hta::CStr(glyph.c_str()),
                         state.fontId, hta::m3d::TW_NOWRAP, hta::m3d::TF_LEFT);
            glyph = colorToken;
            glyph += ">";
            const float nextGlyphWidth = glyphWidth(">");
            glyphX = layout.nextX + (layout.buttonW - nextGlyphWidth) * 0.5f;
            gfx->AddText(drawInfo, hta::PointBase<float>{ glyphX, y }, hta::CStr(glyph.c_str()),
                         state.fontId, hta::m3d::TW_NOWRAP, hta::m3d::TF_LEFT);
        }

        void DrawTextRun(BookState& state, hta::m3d::ui::GfxServer* gfx,
                         const hta::m3d::ui::DrawInfo& drawInfo,
                         const hta::PointBase<float>& position, const std::string& text,
                         uint32_t style, int32_t fontId) {
            const hta::CStr value(text.c_str());
            gfx->AddText(drawInfo, position, value, fontId,
                         hta::m3d::TW_NOWRAP, hta::m3d::TF_LEFT);
            if (!fonts::UsesSyntheticBold(state, style))
                return;

            const float halfOffset = constants::SyntheticBoldOffset * 0.5f;
            gfx->AddText(drawInfo,
                         hta::PointBase<float>{ position.x - halfOffset, position.y },
                         value, fontId, hta::m3d::TW_NOWRAP, hta::m3d::TF_LEFT);
        }

        void DrawClippedImage(hta::m3d::ui::GfxServer* gfx,
                              const hta::m3d::ui::DrawInfo& drawInfo,
                              float x, float y, float width, float height,
                              float clipTop, float clipBottom,
                              hta::m3d::rend::TexHandle texture) {
            const float visibleTop = (std::max)(y, clipTop);
            const float visibleBottom = (std::min)(y + height, clipBottom);
            if (!(visibleBottom > visibleTop) || !(width > 0.0f) || !(height > 0.0f))
                return;

            const float v0 = (visibleTop - y) / height;
            const float v1 = (visibleBottom - y) / height;
            gfx->AddImagedRectGeneral(
                drawInfo,
                hta::BoundsBase<float>{x, visibleTop, width, visibleBottom - visibleTop},
                0, texture, 0.0f, v0, 1.0f, v1);
        }
    }

    std::vector<WrappedRow> WrapStyledLine(hta::m3d::ui::GfxServer* gfx,
                                            BookState& state, const ParsedLine& line,
                                            float wrapWidth) {
        std::vector<WrappedRow> rows;
        WrappedRow current;
        const float maxWidth = (std::max)(1.0f, wrapWidth);
        auto flush = [&]() {
            if (!current.segments.empty()) {
                rows.push_back(std::move(current));
                current = WrappedRow{};
            }
        };
        auto append = [&](const Seg& segment, const std::string& text, float width) {
            if (text.empty())
                return;
            if (!current.segments.empty()
                && current.segments.back().style == segment.style
                && current.segments.back().hasColor == segment.hasColor
                && (!segment.hasColor || current.segments.back().color == segment.color)) {
                current.segments.back().text += text;
                current.segments.back().width += width;
            }
            else {
                current.segments.push_back(WrappedSegment{
                    text, segment.style, segment.color, segment.hasColor, width });
            }
            current.width += width;
        };
        auto measure = [&](const std::string& text, int32_t fontId) {
            const hta::PointBase<float> measured = gfx->MeasureText(
                hta::CStr(text.c_str()), fontId, hta::m3d::TW_NOWRAP, maxWidth);
            return (std::isfinite(measured.x) && measured.x > 0.0f) ? measured.x : 0.0f;
        };

        for (const Seg& segment : line.segs) {
            const int32_t fontId = fonts::EnsureStyleFont(state, segment.style);
            size_t start = 0;
            while (start < segment.text.size()) {
                size_t end = start;
                while (end < segment.text.size()
                       && !std::isspace(static_cast<unsigned char>(segment.text[end])))
                    ++end;
                while (end < segment.text.size()
                       && std::isspace(static_cast<unsigned char>(segment.text[end])))
                    ++end;

                const std::string token = segment.text.substr(start, end - start);
                const float tokenWidth = measure(token, fontId);
                if (!current.segments.empty() && current.width + tokenWidth > maxWidth)
                    flush();

                if (tokenWidth <= maxWidth || token.size() == 1) {
                    append(segment, token, tokenWidth);
                }
                else {
                    // A single long word still needs a character fallback so it
                    // cannot create a row wider than the content rectangle.
                    for (const char ch : token) {
                        const std::string glyph(1, ch);
                        const float glyphWidth = measure(glyph, fontId);
                        if (!current.segments.empty() && current.width + glyphWidth > maxWidth)
                            flush();
                        append(segment, glyph, glyphWidth);
                    }
                }
                start = end;
            }
        }
        flush();
        if (rows.empty())
            rows.push_back(WrappedRow{});
        return rows;
    }

    void DrawBook(BookState& state, const hta::m3d::ui::DrawInfo& drawInfo) {
        if (state.anchorFellBack)
            return;
        hta::m3d::ui::GfxServer* gfx = hta::m3d::ui::Wnd::GetGfxServer();
        if (!gfx)
            return;

        const int32_t pageCount = static_cast<int32_t>(state.pageStart.size());
        if (pageCount == 0)
            return;
        if (state.curPage < 0)
            state.curPage = 0;
        if (state.curPage > pageCount - 1)
            state.curPage = pageCount - 1;

        char colorToken[16];
        const uint32_t textColor = state.box ? state.box->m_textColor : 0;
        std::snprintf(colorToken, sizeof(colorToken), "@%08x",
                      textColor != 0 ? textColor : constants::OpaqueWhiteTextColor);

        const float wrapWidth = (std::max)(1.0f, state.clientW - 2.0f * state.leftPad);
        const int32_t first = state.pageStart[state.curPage];
        const int32_t last = state.pageEnd[state.curPage];
        const float contentBottom = state.top0 + state.visibleH;

        for (int32_t lineIndex = first, row = 0; lineIndex < last;) {
            const ParsedLine& line = state.lines[(size_t) lineIndex];
            const int32_t visualRows = (lineIndex >= 0
                && lineIndex < static_cast<int32_t>(state.lineRows.size()))
                ? state.lineRows[(size_t) lineIndex] : 1;
            if (line.segs.empty() && !line.isImage) {
                row += visualRows;
                ++lineIndex;
                continue;
            }
            const float y = state.top0 + static_cast<float>(row) * state.lineH - state.scrollY;
            const float lineBottom = y + static_cast<float>(visualRows) * state.lineH;
            // A logical line may contain several wrapped visual rows. Do not
            // discard the whole line merely because its last row extends below
            // the viewport: the wrapped rows below are clipped individually,
            // while the custom scroll offset brings the preceding rows into view.
            if (!line.isImage && (lineBottom <= state.top0 || y >= contentBottom)) {
                row += visualRows;
                ++lineIndex;
                continue;
            }

            if (line.isImage) {
                // Images occupy the whole logical line area. Crop the source UVs
                // when scrolling cuts the image at either edge instead of
                // dropping the whole image. The bottom clip is above the footer.
                const float captionH = line.imageAlt.empty() ? 0.0f : state.lineH;
                const uint32_t imageHandle = lineIndex < static_cast<int32_t>(state.imageHandles.size())
                    ? state.imageHandles[(size_t) lineIndex] : 0;
                const float imageW = lineIndex < static_cast<int32_t>(state.imageWidths.size())
                    ? state.imageWidths[(size_t) lineIndex] : 0.0f;
                const float imageH = lineIndex < static_cast<int32_t>(state.imageHeights.size())
                    ? state.imageHeights[(size_t) lineIndex] : 0.0f;
                if (y + imageH <= state.top0 || y >= contentBottom) {
                    row += visualRows;
                    ++lineIndex;
                    continue;
                }
                if (imageHandle && imageW > 0.0f && imageH > 0.0f) {
                    float x = state.leftPad;
                    if (line.align == hta::m3d::TF_RIGHT)
                        x = state.clientW - state.leftPad - imageW;
                    else if (line.align == hta::m3d::TF_CENTER)
                        x = (state.clientW - imageW) * 0.5f;
                    if (x < 1.0f)
                        x = 1.0f;
                    DrawClippedImage(gfx, drawInfo, x, y, imageW, imageH,
                                     state.top0, contentBottom,
                                     hta::m3d::rend::TexHandle((int32_t) imageHandle));
                    if (!line.imageAlt.empty()) {
                        std::string caption = colorToken;
                        caption += line.imageAlt;
                        const float captionY = y + imageH;
                        if (captionY < state.top0
                            || captionY + captionH > contentBottom) {
                            row += visualRows;
                            ++lineIndex;
                            continue;
                        }
                        const float captionX = line.align == hta::m3d::TF_RIGHT
                            ? state.clientW - state.leftPad
                            : line.align == hta::m3d::TF_CENTER
                                ? state.clientW * 0.5f : state.leftPad;
                        gfx->AddText(drawInfo,
                                     hta::PointBase<float>{ captionX, captionY },
                                     hta::CStr(caption.c_str()), state.fontId,
                                     hta::m3d::TW_NOWRAP,
                                     static_cast<hta::m3d::TextFormatFlags>(line.align));
                    }
                }
                else {
                    std::string fallback = colorToken;
                    fallback += line.imageAlt.empty() ? "[image]" : line.imageAlt;
                    const float fallbackX = line.align == hta::m3d::TF_RIGHT
                        ? state.clientW - state.leftPad
                        : line.align == hta::m3d::TF_CENTER ? state.clientW * 0.5f : state.leftPad;
                    gfx->AddText(drawInfo, hta::PointBase<float>{ fallbackX, y },
                                 hta::CStr(fallback.c_str()), state.fontId,
                                 hta::m3d::TW_NOWRAP,
                                 static_cast<hta::m3d::TextFormatFlags>(line.align));
                }
                row += visualRows;
                ++lineIndex;
                continue;
            }

            std::vector<float> widths(line.segs.size());
            float total = 0.0f;
            for (size_t segmentIndex = 0; segmentIndex < line.segs.size(); ++segmentIndex) {
                const int32_t fontId = fonts::EnsureStyleFont(state, line.segs[segmentIndex].style);
                const hta::PointBase<float> measured = gfx->MeasureText(
                    hta::CStr(line.segs[segmentIndex].text.c_str()), fontId,
                    hta::m3d::TW_NOWRAP, wrapWidth);
                widths[segmentIndex] = (std::isfinite(measured.x) && measured.x > 0.0f)
                    ? measured.x : 0.0f;
                total += widths[segmentIndex];
            }

            if (total > wrapWidth) {
                const std::vector<WrappedRow> wrapped = WrapStyledLine(gfx, state, line, wrapWidth);
                for (size_t rowIndex = 0; rowIndex < wrapped.size(); ++rowIndex) {
                    const float rowY = y + static_cast<float>(rowIndex) * state.lineH;
                    if (rowY < state.top0 || rowY + state.lineH > contentBottom)
                        continue;
                    const WrappedRow& wrappedRow = wrapped[rowIndex];
                    float x = line.align == hta::m3d::TF_RIGHT
                        ? state.clientW - state.leftPad - wrappedRow.width
                        : line.align == hta::m3d::TF_CENTER
                            ? (state.clientW - wrappedRow.width) * 0.5f : state.leftPad;
                    if (x < 1.0f)
                        x = 1.0f;
                    for (const WrappedSegment& segment : wrappedRow.segments) {
                        std::string text = segment.hasColor
                            ? ColorToken(segment.color) : colorToken;
                        text += segment.text;
                        DrawTextRun(state, gfx, drawInfo,
                                     hta::PointBase<float>{ x, rowY }, text, segment.style,
                                     fonts::EnsureStyleFont(state, segment.style));
                        x += segment.width;
                    }
                }
                row += visualRows;
                ++lineIndex;
                continue;
            }

            if (lineBottom > contentBottom) {
                row += visualRows;
                ++lineIndex;
                continue;
            }

            float x = line.align == hta::m3d::TF_RIGHT
                ? state.clientW - total - state.leftPad
                : line.align == hta::m3d::TF_CENTER ? (state.clientW - total) * 0.5f : state.leftPad;
            if (x < 1.0f)
                x = 1.0f;
            for (size_t segmentIndex = 0; segmentIndex < line.segs.size(); ++segmentIndex) {
                const int32_t fontId = fonts::EnsureStyleFont(state, line.segs[segmentIndex].style);
                std::string text = line.segs[segmentIndex].hasColor
                    ? ColorToken(line.segs[segmentIndex].color) : colorToken;
                text += line.segs[segmentIndex].text;
                DrawTextRun(state, gfx, drawInfo, hta::PointBase<float>{ x, y }, text,
                            line.segs[segmentIndex].style, fontId);
                x += widths[segmentIndex];
            }
            row += visualRows;
            ++lineIndex;
        }

        if (pageCount >= 2)
            DrawFooter(state, gfx, drawInfo, colorToken);
    }
}
