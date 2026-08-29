#pragma once
#include "config.hpp"

namespace kraken::ext::uibooks {
    enum class BookMode : uint8_t {
        Scroll,
        Pages,
    };

    void Apply(const Config* config);

    // Per-book display property. The key is the book's nameId from BooksWnd::Book;
    // #page remains a text directive, but scroll/pages do not belong in the text.
    // Test and extensions may register an override through this API.
    void RegisterBookMode(const char* nameId, BookMode mode);

#if defined(KRAKEN_UIBOOKS_TESTS)
    // Diagnostics for the uibookstest driver.
    int32_t LastParseLines();     // logical lines of the last parsed book SetText (0 = not yet)
    int32_t LastPageTotal();      // pages of the last ready OnPaint layout (0 = not yet)
    int32_t LastCurPage();        // current page index (0-based) of the last book
    int32_t LastStyleFontCount(); // distinct bold/italic style fonts resolved (0..3)
    uint32_t LastStyleMask();      // parsed style presence: FONT_BOLD/FONT_ITALIC bits
    int32_t LastColoredSegments();
    uint32_t LastColor(int32_t index);
    int32_t LastAlignment();      // parsed book alignment (TF_LEFT/TF_CENTER/TF_RIGHT)
    BookMode LastBookMode();      // active book property
    int32_t LastExplicitBreaks(); // count of parsed #page breaks
    uint32_t LastTextColor();     // live book text color token source (Wnd::m_textColor)
    int32_t GetBookPageAlignment(int32_t page); // alignment of the first line on a page
    int32_t GetBookLineAlignment(int32_t line); // alignment of a parsed line
    bool GetBookClientRect(float* x0, float* y0, float* w, float* h);
    // x0/y0 = screen position of the box's client rect origin; w/h = client size.
    // Mouse events and GfxServer::AddText both work 0-based client-relative.
    float GetBookLineHeight();
    float GetBookScrollY();       // client-relative offset inside the current page
    float GetBookMaxScrollY();    // max offset of the current page (0 = fits without scroll)
    int32_t GetBookPageLines(int32_t page); // lines on the page, -1 if page invalid

    // The real engine footer nav buttons (ButtonWnd children of the book box, like the
    // Map page's < / >). Null until the first painted multi-page layout created them.
    void* GetBookNavButton(bool next);            // the live ButtonWnd (engine owns it)

    // Engine scroll-band access for the test driver (null/0 until the box has painted).
    void* GetBookVerticalScroll();     // the book box's live vertical ScrollWnd
    float GetBookEngineScrollMaxPos(); // that scroll's engine m_maxPos in px (0 = fits)
    float GetBookEngineScrollCurPos(); // that scroll's engine position in px (m_curPos*m_maxPos)
#endif
}
