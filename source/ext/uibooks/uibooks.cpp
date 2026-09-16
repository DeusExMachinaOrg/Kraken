#define LOGGER "uibooks"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ext/logger.hpp"
#include "ext/uibooks/uibooks.hpp"
#include "ext/uibooks/uibooks_config.hpp"
#include "ext/uibooks/uibooks_constants.hpp"
#include "ext/uibooks/uibooks_fonts.hpp"
#include "ext/uibooks/uibooks_hooks.hpp"
#include "ext/uibooks/uibooks_navigation.hpp"
#include "ext/uibooks/uibooks_pagination.hpp"
#include "ext/uibooks/uibooks_parser.hpp"
#include "ext/uibooks/uibooks_render.hpp"
#include "ext/uibooks/uibooks_resources.hpp"
#include "ext/uibooks/uibooks_scroll.hpp"
#include "ext/uibooks/uibooks_state.hpp"
#include "routines.hpp"

#include "hta/CStr.hpp"
#include "hta/PointBase.hpp"
#include "hta/m3d/AIParam.hpp"
#include "hta/m3d/Enums.hpp"
#include "hta/m3d/ui/Enums.hpp"
#include "hta/BooksWnd.hpp"
#include "hta/m3d/ui/ButtonWnd.hpp"
#include "hta/m3d/ui/DrawInfo.hpp"
#include "hta/m3d/ui/FormattedLine.hpp"
#include "hta/m3d/ui/GfxServer.hpp"
#include "hta/m3d/ui/ListBoxWnd.hpp"
#include "hta/m3d/ui/TextBoxWnd.hpp"
#include "hta/m3d/ui/Wnd.hpp"

namespace kraken::ext::uibooks {

    namespace detail {
        using hta::CStr;
        using hta::PointBase;
        using hta::m3d::AIParam;
        using hta::m3d::ui::DrawInfo;
        using hta::m3d::ui::FormattedLine;
        using hta::m3d::ui::ListBoxWnd;
        using hta::m3d::ui::TextBoxWnd;
        using hta::m3d::ui::Wnd;

        using ItemsWnd = ListBoxWnd<FormattedLine>;
        using Item = ItemsWnd::Item;
        static_assert(sizeof(Item) == 0x40, "ListBoxWnd<FormattedLine>::Item layout mismatch");

        // The box's own OnWndNotify slot currently holds the base Wnd::OnWndNotify body
        // (TextBoxWnd does not override it - verified from the PE vtable bytes: slot 50
        // = 0x41C310). The footer nav buttons are children of the box, so the engine's
        // click-release -> CallParentNotify -> WndStation message 40 lands on this slot.
        // Item rects are relative to the client area top-left (the engine subtracts the
        // scroll offset before comparing them against client-space mouse points).
        const Item* ItemsBegin(const TextBoxWnd* self) {
            return self && !self->m_items.empty() ? &self->m_items[0] : nullptr;
        };

        const Item* ItemsEnd(const TextBoxWnd* self) {
            const Item* begin = ItemsBegin(self);
            return begin ? begin + self->m_items.size() : nullptr;
        };

        int32_t ItemCount(const TextBoxWnd* self) {
            const Item* begin = ItemsBegin(self);
            const Item* end = ItemsEnd(self);
            if (!begin || !end || end <= begin)
                return 0;
            return (int32_t)((size_t)((const uint8_t*) end - (const uint8_t*) begin) / sizeof(Item));
        };

    };

    namespace {
        uint32_t  g_enabled = 0;
#if defined(KRAKEN_UIBOOKS_TESTS)
        int32_t   g_lastParseLines = 0;
#endif
        std::unordered_map<hta::m3d::ui::TextBoxWnd*, BookState> g_bookStates;
        hta::m3d::ui::TextBoxWnd* g_activeBookBox = nullptr;
        void ClearBookStates() {
            // Footer buttons are owned by their engine parent, not by this map.
            // Do not call through a cached parent here: Apply() may be called after
            // the journal has already been destroyed. The normal live-window path
            // removes buttons from Hook_SetText before replacing their text.
            g_bookStates.clear();
            g_activeBookBox = nullptr;
        }

        BookState* FindBookState(hta::m3d::ui::TextBoxWnd* box) {
            if (!box)
                return nullptr;
            const auto it = g_bookStates.find(box);
            return it == g_bookStates.end() ? nullptr : &it->second;
        }

        BookState& GetBookState(hta::m3d::ui::TextBoxWnd* box) {
            auto [it, inserted] = g_bookStates.try_emplace(box);
            if (inserted)
                it->second.box = box;
            return it->second;
        }

        BookState* ActiveBookState() {
            return FindBookState(g_activeBookBox);
        }

        struct ResolvedImageSize {
            float width = 0.0f;
            float height = 0.0f;
        };

        ResolvedImageSize ResolveImageSize(const ParsedLine& line, float sourceW, float sourceH,
                                           float maxW, float maxH, bool allowUpscale) {
            const float aspect = sourceW / sourceH;
            const bool widthSet = line.imageWidth.specified;
            const bool heightSet = line.imageHeight.specified;
            const bool widthFill = line.imageWidth.fill;

            float width = widthSet ? (widthFill ? maxW : line.imageWidth.pixels) : sourceW;
            float height = heightSet ? line.imageHeight.pixels : sourceH;

            if (widthSet && !heightSet)
                height = width / aspect;
            else if (!widthSet && heightSet)
                width = height * aspect;
            else if (!widthSet && !heightSet) {
                const float scale = allowUpscale
                    ? (std::min)(maxW / sourceW, maxH / sourceH)
                    : (std::min)(1.0f,
                        (std::min)(maxW / sourceW, maxH / sourceH));
                width = sourceW * scale;
                height = sourceH * scale;
            }

            // Explicit dimensions may stretch the bitmap, but never let it
            // escape the text container. A one-dimensional request keeps the
            // source aspect ratio and is clamped again after deriving the other side.
            width = (std::min)(width, maxW);
            height = (std::min)(height, maxH);
            if (widthSet != heightSet) {
                if (widthSet)
                    height = (std::min)(maxH, width / aspect);
                else
                    width = (std::min)(maxW, height * aspect);
            }

            if (!(width > 0.0f) || !(height > 0.0f)
                || !std::isfinite(width) || !std::isfinite(height))
                return {};
            return { width, height };
        }

        bool RegisteredModeForSelection(hta::BooksWnd* books, detail::Wnd* src, BookMode* out) {
            if (!books || !src || !out)
                return false;
            if (src != static_cast<detail::Wnd*>(books->m_bookList))
                return false;

            const auto* bookList = static_cast<const hta::m3d::ui::ListBoxWnd<hta::BooksWnd::BookListEntry*>*>(src);
            const int32_t selected = bookList->m_curSel;
            if (selected < 0)
                return false;

            const size_t index = static_cast<size_t>(selected);
            if (index >= bookList->m_items.size())
                return false;

            const auto* listEntry = bookList->m_items[index].m_item;
            if (!listEntry)
                return false;
            const auto* book = listEntry->GetBook();
            const char* nameId = book->m_nameId.m_charPtr;
            const std::optional<BookMode> mode = config::FindBookMode(nameId);
            if (mode.has_value()) {
                *out = *mode;
                return true;
            }
            return false;
        };

        // Geometry + page plan. The engine keeps a single blank anchor item from
        // which the row geometry (height/margins) is read; the visible content is
        // drawn by DrawBook itself so styles and pages do not depend on the
        // engine's items at all.
        bool TryLayout(BookState& st, hta::m3d::ui::TextBoxWnd* self, float cx0, float cy0, float cw, float ch) {
            st.ready = false;
            if (!(cw > 0.0f) || !(ch > 0.0f) || !std::isfinite(cw) || !std::isfinite(ch))
                return false;

            const detail::Item* begin = detail::ItemsBegin(self);
            const int32_t count = detail::ItemCount(self);
            if (count == 0) {
                // the " " anchor did not produce an item (blank line eaten): put the
                // plain clean text into the engine once and render it as-is (no
                // per-segment styles, single page).
                if (!st.anchorFellBack) {
                    st.anchorFellBack = true;
                    LOG_WARNING("book anchor item missing - falling back to the engine's plain rendering");
                    hta::CStr clean(st.cleanText.empty() ? "" : st.cleanText.c_str());
                    self->TextBoxWnd::SetText(clean);
                }
                return false;
            }

            const float lineH = begin->m_rect.height;
            if (!std::isfinite(lineH) || lineH <= 1.0f || lineH >= ch)
                return false;
            float top0 = begin->m_rect.y0;
            if (!std::isfinite(top0) || top0 < 0.0f)
                top0 = 0.0f;
            float leftPad = begin->m_rect.x0;
            if (!std::isfinite(leftPad) || leftPad < 0.0f)
                leftPad = 0.0f;

            st.lineH = lineH;
            st.top0 = top0;
            st.leftPad = leftPad;
            st.clientX0 = cx0;
            st.clientY0 = cy0;
            st.clientW = cw;
            st.clientH = ch;
            st.fontId = self->m_defFont;

            const int32_t n = (int32_t) st.lines.size();
            int32_t breaks = 0;
            for (bool b : st.lineBreak)
                breaks += b ? 1 : 0;
            // Page mode always has a navigation footer, including when the page
            // boundaries were created automatically. Keep the page capacity in sync
            // with DrawBook(), which clips every multi-page layout above the footer.
            // A one-page auto-scroll book has no arrows/counter, so it still keeps the
            // full height available.
            const bool reserveFooter = breaks > 0 || st.mode == BookMode::Pages;
            const float footerH = reserveFooter ? navigation::FooterHeight() : 0.0f;
            const float contentH = (std::max)(1.0f, ch - top0 - footerH);
            int32_t rows = (int32_t) std::floorf(contentH / lineH);
            if (rows < 1)
                rows = 1;
            st.rowsPerPage = rows;
            st.visibleH = contentH;

            hta::m3d::ui::GfxServer* gfx = hta::m3d::ui::Wnd::GetGfxServer();
            if (!gfx)
                return false;
            st.ReleaseImageTextures();
            st.lineRows.assign((size_t) n, 1);
            st.imageHandles.assign((size_t) n, 0);
            st.imageTextureOwned.assign((size_t) n, false);
            st.imageWidths.assign((size_t) n, 0.0f);
            st.imageHeights.assign((size_t) n, 0.0f);
            const float wrapWidth = (std::max)(1.0f, cw - 2.0f * leftPad);
            const bool imageOnly = n == 1 && st.lines[0].isImage;
            for (int32_t li = 0; li < n; ++li) {
                const ParsedLine& line = st.lines[(size_t) li];
                if (!line.isImage)
                    continue;

                float sourceW = constants::DefaultImageWidth;
                float sourceH = constants::DefaultImageHeight;
                uint32_t imageHandle = 0;
                bool imageTextureOwned = false;
                if (line.imageSource == ImageSource::GameResource) {
                    imageHandle = resources::LoadTextureByResourceId(line.imageReference.c_str());
                    if (imageHandle)
                        (void) resources::ReadTextureDimensions(imageHandle, &sourceW, &sourceH);
                }
                else {
                    (void) resources::ReadImageDimensions(line.imageReference.c_str(), &sourceW, &sourceH);
                    imageHandle = resources::LoadTexture(line.imageReference.c_str());
                    imageTextureOwned = imageHandle != 0;
                }
                if (!(sourceW > 0.0f) || !(sourceH > 0.0f)
                    || !std::isfinite(sourceW) || !std::isfinite(sourceH)) {
                    sourceW = constants::FallbackImageWidth;
                    sourceH = constants::FallbackImageHeight;
                }
                const float captionH = line.imageAlt.empty() ? 0.0f : lineH;
                const float imageMaxW = wrapWidth;
                const float imageMaxH = (std::max)(1.0f, contentH - captionH);
                ResolvedImageSize image = ResolveImageSize(
                    line, sourceW, sourceH, imageMaxW, imageMaxH, imageOnly);
                if (!(image.width > 0.0f) || !(image.height > 0.0f)) {
                    image.width = (std::min)(imageMaxW, constants::FallbackImageWidth);
                    image.height = (std::min)(imageMaxH, constants::FallbackImageHeight);
                }
                st.imageHandles[(size_t) li] = imageHandle;
                st.imageTextureOwned[(size_t) li] = imageTextureOwned;
                st.imageWidths[(size_t) li] = image.width;
                st.imageHeights[(size_t) li] = image.height;
                st.lineRows[(size_t) li] = std::max<int32_t>(
                    1, (int32_t) std::ceil((image.height + captionH) / lineH));
                if (!st.imageHandles[(size_t) li])
                    LOG_WARNING("book image line %d: resource '%s' could not be loaded (alt text will be shown)",
                                li, line.imageReference.c_str());
            }
            if (wrapWidth > 1.0f) {
                for (int32_t li = 0; li < n; ++li) {
                    const ParsedLine& line = st.lines[(size_t) li];
                    if (line.isImage)
                        continue;
                    st.lineRows[(size_t) li] = (int32_t) render::WrapStyledLine(
                        gfx, st, line, wrapWidth).size();
                }
            }

            const pagination::PagePlan pagePlan = pagination::Build(
                st.lineRows, st.lineBreak, st.mode, rows);
            st.pageStart = pagePlan.start;
            st.pageEnd = pagePlan.end;

            st.curPage = 0;
            st.scrollY = 0.0f;
            st.ready = true;
            return true;
        };

        void GoToPageShifted(BookState& st, int32_t shift) {
            const int32_t npages = (int32_t) st.pageStart.size();
            if (npages < 2)
                return;
            int32_t p = st.curPage + shift;
            if (p < 0)
                p = 0;
            if (p > npages - 1)
                p = npages - 1;
            if (p != st.curPage) {
                st.curPage = p;
                st.scrollY = 0.0f; // a new page is always entered at its top
                // Do not let the old page's native scrollbar position overwrite
                // the new page's top offset during the next paint pass.
                st.lastPushedPx = 0.0f;
                st.scrollSynced = false;
                if (st.vscroll)
                    st.vscroll->SetCurPos(0.0f);
            }
        };

    }

    int32_t Hook_BooksOnWndNotify(void* self, hta::m3d::ui::Wnd* src, uint32_t a, uint32_t b, const hta::m3d::AIParam& param) {
        auto* books = static_cast<hta::BooksWnd*>(self);
        hta::m3d::ui::Wnd* box = books->m_book;
        const bool isBookSelection = a == hta::BooksWnd::BOOK_LIST_ID
            && b == hta::m3d::ui::WNM_CHANGE;
        if (isBookSelection) {
            auto* bookBox = static_cast<hta::m3d::ui::TextBoxWnd*>(box);
            if (box) {
                if (g_activeBookBox != bookBox) {
                    hta::m3d::ui::TextBoxWnd* previousBox = g_activeBookBox;
                    if (previousBox) {
                        // The previous box may already have been destroyed while
                        // the journal was closed. Its children follow the engine
                        // parent lifetime, so discard only our non-owning links.
                        const auto previousIt = g_bookStates.find(previousBox);
                        if (previousIt != g_bookStates.end()) {
                            previousIt->second.ReleaseImageTextures();
                            g_bookStates.erase(previousIt);
                        }
                    }
                }
                g_activeBookBox = bookBox;
                (void) GetBookState(bookBox);
            }

            // The original BooksWnd::ShowBook resolves the selected Book and only then
            // sends its textId to TextBoxWnd::SetText. Resolve our sidecar property from
            // the same selected Book before calling the original body, so the renderer
            // receives the mode without putting a mode marker into the text.
            config::LoadBookModes();
            BookMode mode = BookMode::Scroll;
            (void) RegisteredModeForSelection(books, src, &mode);
            if (bookBox)
                GetBookState(bookBox).mode = mode;

            return static_cast<hta::BooksWnd*>(self)->BooksWnd::OnWndNotify(src, a, b, param);
        }

        return static_cast<hta::BooksWnd*>(self)->BooksWnd::OnWndNotify(src, a, b, param);
    };

    int32_t Hook_OnBeforeRemoveFromWndStation(hta::m3d::ui::TextBoxWnd* self) {
        if (self) {
            const auto it = g_bookStates.find(self);
            if (it != g_bookStates.end()) {
                // This hook runs while the textbox still owns its children. Remove
                // our buttons now, then erase every non-owning engine link before
                // the engine starts destroying the journal hierarchy.
                (void) navigation::DestroyButtons(it->second.navBtns);
                it->second.ReleaseImageTextures();
                g_bookStates.erase(it);
            }
            if (g_activeBookBox == self)
                g_activeBookBox = nullptr;
        }
        return self ? self->Wnd::OnBeforeRemoveFromWndStation() : 0;
    };

    // The box's own OnWndNotify (vtable slot 50 - the base Wnd::OnWndNotify body in the
    // shipped binary, TextBoxWnd does not override it). The footer button click
    // release reaches it through the engine chain ButtonWnd::OnMouseButton0 ->
    // Wnd::OnMouseButton0 -> CallParentNotify -> WndStation::AddNotifyForWnd ->
    // Application message 40 -> this slot, with `src` the button wnd (matched by
    // pointer) and `a`/`b` its id / the passed message. The base Wnd::OnMouseButton0
    // passes WNM_CLICK for a plain click release; the src match is what routes it.
    int32_t Hook_BoxOnWndNotify(hta::m3d::ui::TextBoxWnd* self, hta::m3d::ui::Wnd* src, uint32_t a, uint32_t b, const hta::m3d::AIParam& param) {
        BookState* st = FindBookState(self);
        // ButtonWnd can notify the parent for hover/enter state changes as well as
        // the actual click release. Page navigation is release-only (the engine's
        // plain click message is WNM_CLICK); routing every notification makes merely
        // moving the cursor over an arrow turn the page.
        if (g_enabled && st && st->ready && src && b == hta::m3d::ui::WNM_CLICK) {
            if (src == static_cast<hta::m3d::ui::Wnd*>(st->navBtns.prevBtn)) {
                GoToPageShifted(*st, -1);
                return 1;
            }
            if (src == static_cast<hta::m3d::ui::Wnd*>(st->navBtns.nextBtn)) {
                GoToPageShifted(*st, +1);
                return 1;
            }
        }
        return static_cast<hta::m3d::ui::Wnd*>(self)->Wnd::OnWndNotify(src, a, b, param);
    };

    int32_t Hook_SetText(hta::m3d::ui::TextBoxWnd* self, const hta::CStr& text) {
        const char* s = text.m_charPtr;
        BookState* state = FindBookState(self);
        if (!g_enabled)
            return self->TextBoxWnd::SetText(text);
        if (!state)
            return self->TextBoxWnd::SetText(text);
        g_activeBookBox = self;
        BookState& st = *state;

        ParseResult pr = ParseBookText(s);

        // The journal can re-issue the identical book text (re-render when the books
        // tab is shown again). The layout rebuilds identically on the next paint, so
        // keep the reading position instead of resetting it (page, scroll, band state).
        const std::string newText = (s != nullptr) ? s : "";
        const bool sameText = st.lastText == newText;
        const int32_t savedPage = st.curPage;
        const float savedScrollY = st.scrollY;
        st.box = self;
        if (!sameText)
            navigation::DestroyButtons(st.navBtns);
        st.lines = std::move(pr.lines);
        st.lineBreak = std::move(pr.lineBreak);
        st.align = pr.align;
        st.styleMask = pr.styleMask;
        st.styledSegs = pr.styledSegs;
        st.coloredSegs = pr.coloredSegs;
        st.colors = std::move(pr.colors);
        st.ReleaseImageTextures();
        st.imageHandles.clear();
        st.imageTextureOwned.clear();
        st.imageWidths.clear();
        st.imageHeights.clear();
        st.cleanText = pr.clean;
        st.pageStart.clear();
        st.pageEnd.clear();
        st.ready = false;
        st.anchorFellBack = false;
        st.curPage = 0;
        st.scrollY = 0.0f;
        st.footerCaptionW = 0.0f;
        st.styleFontsInitialized = false;
        st.vscroll = nullptr;
        st.lastPushedPx = 0.0f;
        st.scrollSynced = false;
        st.styleFontId[0] = st.styleFontId[1] = st.styleFontId[2] = st.styleFontId[3] = -1;
        st.styleNeedsSyntheticBold[0] = st.styleNeedsSyntheticBold[1]
            = st.styleNeedsSyntheticBold[2] = st.styleNeedsSyntheticBold[3] = false;
        if (sameText) {
            st.curPage = savedPage;
            st.scrollY = savedScrollY;
        }
        st.lastText = newText;

        int32_t breaks = 0;
        for (bool b : st.lineBreak)
            breaks += b ? 1 : 0;
        st.explicitBreaks = breaks;
        self->m_textFormat = static_cast<hta::m3d::TextFormatFlags>(pr.align);

        // The visible content is rendered by DrawBook; the engine keeps only a
        // single blank anchor item used for the row geometry.
        const int32_t result = self->TextBoxWnd::SetText(hta::CStr(" "));
#if defined(KRAKEN_UIBOOKS_TESTS)
        g_lastParseLines = static_cast<int32_t>(st.lines.size());
#endif
        return result;
    };

    int32_t Hook_OnPaint(hta::m3d::ui::TextBoxWnd* self, const hta::m3d::ui::DrawInfo& di) {
        BookState* state = FindBookState(self);
        if (g_enabled && state && state->ready && !state->pageStart.empty())
            scroll::SyncBeforePaint(*state, self, di);

        const int32_t result = self->TextBoxWnd::OnPaint(di);
        if (!g_enabled || !state)
            return result;

        BookState& st = *state;
        const float cx0 = di.m_clientRect.x0;
        const float cy0 = di.m_clientRect.y0;
        const float cw = di.m_clientRect.width;
        const float ch = di.m_clientRect.height;
        if (!(cw > 0.0f) || !(ch > 0.0f))
            return result;

        const auto geometryChanged = [](float lhs, float rhs) {
            return std::fabs(lhs - rhs) > 0.01f;
        };
        if (!st.ready || geometryChanged(cx0, st.clientX0) || geometryChanged(cy0, st.clientY0)
            || geometryChanged(cw, st.clientW) || geometryChanged(ch, st.clientH))
            (void) TryLayout(st, self, cx0, cy0, cw, ch);
        if (!st.ready)
            return result;

        render::DrawBook(st, di);
        scroll::SyncAfterPaint(st, self);

        // The real footer < / > buttons: created on the first painted multi-page
        // layout, positioned on every painted pass after that.
        if (st.pageStart.size() >= 2) {
            const float bandW = scroll::BandWidth(st.vscroll);
            navigation::EnsureButtons(st.navBtns, self, st.clientW, st.clientH, bandW,
                                      st.footerCaptionW);
            navigation::SyncButtons(st.navBtns, self, st.clientW, st.clientH, bandW,
                                    st.footerCaptionW, (int32_t) st.pageStart.size(), st.curPage);
        }
        return result;
    };

    int32_t Hook_OnMouseButton0(hta::m3d::ui::TextBoxWnd* self, uint32_t btns, const hta::PointBase<float>& pt) {
        BookState* state = FindBookState(self);
        if (g_enabled && state && state->ready && state->pageStart.size() >= 2
            && (btns & hta::m3d::ui::MOUSE_BUTTON0)) {
            BookState& st = *state;
            // mouse pts are client-relative (0-based, pre-scroll): the engine's own
            // ItemFromPoint compares them against the item rects after subtracting the
            // scroll position only - no client-origin subtraction.
            // With live engine buttons, only their click-release notifications should
            // navigate. Keep the broad half-click only as a fallback if button
            // creation/attachment failed and the text glyphs are being drawn instead.
            if (!navigation::IsActive(st.navBtns, st.box)
                && pt.y >= st.clientH - navigation::FooterHeight() && pt.y <= st.clientH) {
                const float bandW = scroll::BandWidth(st.vscroll);
                const navigation::Layout layout = navigation::GetLayout(
                    st.clientW, st.clientH, bandW, st.footerCaptionW);
                if (pt.x >= layout.prevX && pt.x <= layout.prevX + layout.buttonW) {
                    GoToPageShifted(st, -1);
                    return 1;
                }
                if (pt.x >= layout.nextX && pt.x <= layout.nextX + layout.buttonW) {
                    GoToPageShifted(st, +1);
                    return 1;
                }
            }
        }
        // The book uses one blank engine item as a geometry anchor. Letting the
        // stock list-box handler process clicks on the rendered text selects that
        // anchor and scrolls it into view, which resets the custom offset to zero.
        // Book text is not interactive, so consume content clicks here. Scrollbar
        // input is handled by the child ScrollWnd and footer buttons by notify.
        if (g_enabled && state && state->ready
            && (btns & hta::m3d::ui::MOUSE_BUTTON0)
            && pt.y < state->clientH - navigation::FooterHeight())
            return 1;
        auto* items = static_cast<detail::ItemsWnd*>(self);
        return items->detail::ItemsWnd::OnMouseButton0(btns, pt);
    };

    int32_t Hook_OnMouseWheel(hta::m3d::ui::TextBoxWnd* self, int32_t delta, const hta::PointBase<float>& pt) {
        BookState* state = FindBookState(self);
        if (g_enabled && state && state->ready && !state->pageStart.empty() && delta != 0) {
            BookState& st = *state;
            const int32_t dir = (delta > 0) ? -1 : 1; // up = back, down = forward;
            // One line of scroll per event. Wheel input is deliberately scoped
            // to the current page; reaching an edge clamps the offset and never
            // changes the page. Pages are changed only by the footer buttons.
            scroll::ScrollPageLines(st, dir);
            return 1;
        }
        auto* items = static_cast<detail::ItemsWnd*>(self);
        return items->detail::ItemsWnd::OnMouseWheel(delta, pt);
    };

    void RegisterBookMode(const char* nameId, BookMode mode) {
        if (!nameId || !*nameId) {
            LOG_WARNING("Cannot register book property without a nameId");
            return;
        }
        config::RegisterBookMode(nameId, mode);
        LOG_INFO("Book property registered: nameId='%s', mode=%s", nameId,
                 mode == BookMode::Scroll ? "scroll" : "pages");
    };

    void Apply(const Config* config) {
        if (!config)
            return;
        g_enabled = config->uibooks_enabled.value;

        if (!g_enabled) {
            ClearBookStates();
            config::ClearBookModes();
            LOG_INFO("Feature disabled");
            return;
        }

        routines::OverrideValue(reinterpret_cast<void*>(0x009A09A8), &TextBoxWnd_Hooked::SetText);
        // TextBoxWnd vtable slot 28: Wnd::OnBeforeRemoveFromWndStation.
        routines::OverrideValue(reinterpret_cast<void*>(0x009A09D0),
                                 &TextBoxWnd_Hooked::OnBeforeRemoveFromWndStation);
        routines::OverrideValue(reinterpret_cast<void*>(0x009A09E8), &TextBoxWnd_Hooked::OnPaint);
        routines::OverrideValue(reinterpret_cast<void*>(0x009A0A04), &TextBoxWnd_Hooked::OnMouseButton0);
        routines::OverrideValue(reinterpret_cast<void*>(0x009A0A18), &TextBoxWnd_Hooked::OnMouseWheel);
        routines::OverrideValue(reinterpret_cast<void*>(0x009A0A28), &TextBoxWnd_Hooked::OnWndNotify);
        routines::OverrideValue(reinterpret_cast<void*>(0x009D5570), &BooksWnd_Hooked::OnWndNotify);

        LOG_INFO("Feature enabled (unregistered books use scroll mode)");
    };

#if defined(KRAKEN_UIBOOKS_TESTS)
    int32_t LastParseLines() {
        return g_lastParseLines;
    };

    int32_t LastPageTotal() {
        const BookState* state = ActiveBookState();
        return state && state->ready ? (int32_t) state->pageStart.size() : 0;
    };

    int32_t LastCurPage() {
        const BookState* state = ActiveBookState();
        return state ? state->curPage : 0;
    };

    int32_t LastStyleFontCount() {
        const BookState* state = ActiveBookState();
        if (!state)
            return 0;
        int32_t n = 0;
        for (size_t i = 0; i < std::size(state->styleFontId); ++i)
            if ((state->styleFontId[i] > 0 && state->styleFontId[i] != state->fontId)
                || state->styleNeedsSyntheticBold[i])
                n++;
        return n;
    };

    uint32_t LastStyleMask() {
        const BookState* state = ActiveBookState();
        return state ? state->styleMask : 0;
    };

    int32_t LastColoredSegments() {
        const BookState* state = ActiveBookState();
        return state ? state->coloredSegs : 0;
    };

    uint32_t LastColor(int32_t index) {
        const BookState* state = ActiveBookState();
        if (!state || index < 0 || index >= static_cast<int32_t>(state->colors.size()))
            return 0;
        return state->colors[static_cast<size_t>(index)];
    };

    int32_t LastAlignment() {
        const BookState* state = ActiveBookState();
        return state ? state->align : hta::m3d::TF_LEFT;
    };

    BookMode LastBookMode() {
        const BookState* state = ActiveBookState();
        return state ? state->mode : BookMode::Scroll;
    };

    int32_t LastExplicitBreaks() {
        const BookState* state = ActiveBookState();
        return state ? state->explicitBreaks : 0;
    };

    int32_t GetBookPageAlignment(int32_t page) {
        const BookState* state = ActiveBookState();
        if (!state || !state->ready || page < 0 || page >= (int32_t) state->pageStart.size())
            return -1;
        const int32_t line = state->pageStart[page];
        if (line < 0 || line >= (int32_t) state->lines.size())
            return -1;
        return state->lines[line].align;
    };

    int32_t GetBookLineAlignment(int32_t line) {
        const BookState* state = ActiveBookState();
        if (!state || !state->ready || line < 0 || line >= (int32_t) state->lines.size())
            return -1;
        return state->lines[line].align;
    };

    uint32_t LastTextColor() {
        const BookState* state = ActiveBookState();
        return state && state->box ? state->box->m_textColor : 0;
    };

    bool GetBookClientRect(float* x0, float* y0, float* w, float* h) {
        const BookState* state = ActiveBookState();
        if (!state || !state->ready || !state->box || !x0 || !y0 || !w || !h)
            return false;
        *x0 = state->clientX0;
        *y0 = state->clientY0;
        *w = state->clientW;
        *h = state->clientH;
        return true;
    };

    float GetBookLineHeight() {
        const BookState* state = ActiveBookState();
        return state && state->ready ? state->lineH : 0.0f;
    };

    float GetBookScrollY() {
        const BookState* state = ActiveBookState();
        return (state && state->ready && !state->pageStart.empty()) ? state->scrollY : 0.0f;
    };

    float GetBookMaxScrollY() {
        const BookState* state = ActiveBookState();
        return (state && state->ready && !state->pageStart.empty()) ? scroll::MaxScrollY(*state) : 0.0f;
    };

    void* GetBookVerticalScroll() {
        const BookState* state = ActiveBookState();
        return (state && state->ready && state->vscroll) ? static_cast<void*>(state->vscroll) : nullptr;
    };

    float GetBookEngineScrollMaxPos() {
        const BookState* state = ActiveBookState();
        return (state && state->ready && state->vscroll) ? scroll::MaxPositionPx(state->vscroll) : 0.0f;
    };

    float GetBookEngineScrollCurPos() {
        const BookState* state = ActiveBookState();
        return (state && state->ready && state->vscroll) ? scroll::CurrentPositionPx(state->vscroll) : 0.0f;
    };

    int32_t GetBookPageLines(int32_t page) {
        const BookState* state = ActiveBookState();
        if (!state || !state->ready || page < 0 || page >= (int32_t) state->pageStart.size())
            return -1;
        return state->pageEnd[page] - state->pageStart[page];
    };

    void* GetBookNavButton(bool next) {
        const BookState* state = ActiveBookState();
        if (!state || state->navBtns.parentBox != state->box)
            return nullptr;
        return next ? state->navBtns.nextBtn : state->navBtns.prevBtn;
    };
#endif

};
