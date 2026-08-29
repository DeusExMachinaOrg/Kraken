#define LOGGER "uibookstest"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "ext/logger.hpp"
#include "ext/uibookstest/uibookstest_game.hpp"
#include "ext/uibookstest/uibookstest_probes.hpp"
#include "ext/uibookstest/uibookstest.hpp"
#include "ext/uibooks/uibooks.hpp"
#include "ext/uibooks/uibooks_hooks.hpp"
#include "ext/uibooks/uibooks_parser.hpp"
#include "routines.hpp"

#include "hta/CStr.hpp"
#include "hta/CMiracle3d.hpp"
#include "hta/BooksWnd.hpp"
#include "hta/JournalWnd.hpp"
#include "hta/MotherPanel.hpp"
#include "hta/PointBase.hpp"
#include "hta/m3d/Application.hpp"
#include "hta/m3d/AIParam.hpp"
#include "hta/m3d/Enums.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/m3d/ui/TextBoxWnd.hpp"
#include "hta/m3d/ui/Wnd.hpp"
#include "hta/m3d/ui/WndStation.hpp"

namespace kraken::ext::uibookstest {

    namespace detail {

        // ---- game-image addresses (VA = RVA + 0x400000) -----------------------------------
        constexpr uintptr_t CALL_SITE_PROCESS_EVENTS = 0x005A7FFF;

        // God mode (the player dies to enemies while the in-game UI is built):
        // g_Kernel->m_scriptServer runs a Lua buffer
        // directly - no console instance and no script file on disk needed. The god
        // binding is the one data\scripts\cheats.lua god() calls on the player vehicle.
        constexpr const char* GOD_LUA_SRC =
            "anticheat = 0\n"
            "local v = GetPlayerVehicle()\n"
            "if v then v:setGodMode(1) end\n";

        // Primary discovery chain - all offsets/slots verified against the PDB + PE vtable
        // bytes, no tree walking or scanning involved:
        //   CMiracle3d::m_pInterfaceManager @ 0x8B4EC holds the ITruxxUiManager*
        //   (g_pApp's static type is m3d::Application*, the live object is CMiracle3d).
        //   CMiracle3d::m_curGameMode @ 0x8B530 is a CMiracle3d::CurGameMode holding the
        //   GameState (0 = GS_GAME, 2 = GS_MAINMENU, 3 = GS_INITIALIZATION) as an int.
        //   Requests are only honored in the right mode: LoadSavedGame must be called
        //   from the main menu (mode 2), the books trigger from in-game (mode 0).
        //   ITruxxUiManager primary vtable slots (from the concrete vtable image bytes):
        //     15 = GetWindow(int) -> ref_ptr<Wnd> (Wnd*),
        //     49 = Str2WndGuiId(CStr*) -> id or -1.
        //   Str2WndGuiId matches against the uiwindows.xml id strings registered at
        //   level start (BSS table, built at runtime), so the literals below must stay
        //   byte-exact with uiwindows.xml.
        constexpr int32_t   GAME_MODE_GAME           = 0; // GS_GAME
        constexpr int32_t   GAME_MODE_CINEMATIC      = 1; // GS_CINEMATIC
        constexpr int32_t   GAME_MODE_MAINMENU       = 2; // GS_MAINMENU
        constexpr int32_t   GAME_MODE_INITIALIZATION = 3; // GS_INITIALIZATION
        constexpr uint32_t  UIIF_SLOT_GET_WINDOW       = 15;
        constexpr uint32_t  UIIF_SLOT_STR2_GUI_ID      = 49;
        constexpr const char* ID_MOTHER_PANEL          = "IW_DLG_MOTHER_PANEL";
        constexpr const char* ID_JOURNAL_WND           = "IW_WND_JOURNAL";
        constexpr const char* ID_BOOKS_WND             = "IW_WND_BOOKS";

        constexpr const char* DROPBOX_DIR  = "data/uibookstest";
        constexpr const char* NAME_ID      = "kraken_uibookstest_smoke";
        constexpr const char* TEXT_ID      = "kraken_uibookstest_smoke_text";
        constexpr const char* NAME_ID_2    = "kraken_uibookstest_pages";
        constexpr const char* TEXT_ID_2    = "kraken_uibookstest_pages_text";
        constexpr const char* BOOK_NAME_2  = "AUTO-PAGES TEST BOOK";

        // The book volume: real ingame texts from data\if\strings\uibooks.xml, three
        // books separated by '#page' page breaks (44 / 14 / 50 parsed lines - the last
        // page overflows the ~25-row viewport, so it exercises the in-page wheel
        // scroll most of all). Markup on top of the engine format: "|" a hard line,
        // "#page" a page break, "*..*" bold (first line of each page), "_.._" italic
        // (the closing line of pages 1 and 3). The first page starts with left/center/
        // right samples, then page 2 switches to center and page 3 to right. The book
        // display mode is registered separately by nameId. The constant is pure ASCII
        // with \uXXXX
        // escapes: with no BOM the compiler reads this file as cp1251 (the system
        // ANSI code page) and converts the escapes into cp1251 bytes directly - the
        // code page the engine's fonts map text through.
        constexpr const char* BOOK_NAME_VALUE =
            "\u0417\u0430\u043f\u0438\u0441\u043d\u044B\u0435 \u043a\u043d\u0438\u0433\u0438 \u043c\u0438\u0440\u0430"; // "Записные книги мира"
        constexpr const char* BOOK_TEXT =
            "#left|LEFT ALIGNMENT SAMPLE|#center|CENTER ALIGNMENT SAMPLE|#right|RIGHT ALIGNMENT SAMPLE|"
            "STYLE NORMAL SAMPLE|*STYLE BOLD SAMPLE*|_STYLE ITALIC SAMPLE_|*_STYLE BOLD ITALIC SAMPLE_*|"
            "@FFFF3333COLOR RED SAMPLE|@FF33FF33COLOR GREEN SAMPLE|@FF3333FFFFCOLOR BLUE SAMPLE|"
            "*\u0422\u0440\u0430\u043D\u0441 \u0441 \u0434\u0435\u0442\u0441\u0442\u0432\u0430 \u0431\u044B\u043B \u043D\u0435 \u0432 \u043C\u0435\u0440\u0443 \u043B\u044E\u0431\u043E\u043F\u044B\u0442\u0435\u043D, \u0437\u0430 \u0447\u0442\u043E*|"
            "\u0440\u0435\u0433\u0443\u043B\u044F\u0440\u043D\u043E \u043F\u043E\u043B\u0443\u0447\u0430\u043B \u043D\u0430\u043A\u0430\u0437\u0430\u043D\u0438\u0435. \u041E\u0434\u043D\u0430\u0436\u0434\u044B \u043E\u043D \u0432\u043E\u0437\u0432\u0440\u0430\u0449\u0430\u043B\u0441\u044F \u0438\u0437|"
            "\u043E\u0431\u044B\u0447\u043D\u043E\u0433\u043E \u043F\u043E\u0445\u043E\u0434\u0430 \u043F\u043E \u043B\u0435\u0441\u0443, \u043A\u043E\u0442\u043E\u0440\u044B\u0435 \u043E\u0431\u044F\u0437\u0430\u043D \u0441\u043E\u0432\u0435\u0440\u0448\u0430\u0442\u044C \u043A\u0430\u0436\u0434\u044B\u0439|"
            "\u0434\u0440\u0443\u0438\u0434 \u0445\u043E\u0442\u044F \u0431\u044B \u0440\u0430\u0437 \u0432 2 \u0434\u043D\u044F. \u0412\u0434\u0440\u0443\u0433 \u0435\u0433\u043E \u0432\u043D\u0438\u043C\u0430\u043D\u0438\u0435 \u043F\u0440\u0438\u0432\u043B\u0451\u043A|"
            "\u0431\u043B\u0435\u0441\u043A \u043D\u0435\u043E\u0431\u044B\u0447\u043D\u043E\u0433\u043E \u043C\u0435\u0442\u0430\u043B\u043B\u0430. \u0420\u0430\u0441\u043A\u0438\u0434\u0430\u0432 \u0433\u0440\u0443\u0434\u0443 \u0433\u043D\u0438\u044E\u0449\u0438\u0445|"
            "\u043B\u0438\u0441\u0442\u044C\u0435\u0432, \u043E\u043D \u0438\u0437\u0432\u043B\u0435\u043A \u0438\u0437 \u043D\u0438\u0448\u0438 \u0432 \u0437\u0435\u043C\u043B\u0435 \u043D\u0435\u043E\u0431\u044B\u0447\u0430\u0439\u043D\u044B\u0439 \u043F\u0440\u0435\u0434\u043C\u0435\u0442:|"
            "\u0441\u0442\u0430\u043B\u044C\u043D\u043E\u0439 \u0446\u0438\u043B\u0438\u043D\u0434\u0440 \u0441 \u0442\u043E\u0440\u0447\u0430\u0449\u0438\u043C\u0438 \u043F\u0440\u043E\u0432\u043E\u0434\u0430\u043C\u0438 \u0438 \u0441\u0442\u0440\u0430\u043D\u043D\u044B\u043C\u0438|"
            "\u043E\u0442\u0432\u0435\u0440\u0441\u0442\u0438\u044F\u043C\u0438. \u041B\u044E\u0431\u043E\u0439 \u0434\u043E\u0431\u0440\u043E\u043F\u043E\u0440\u044F\u0434\u043E\u0447\u043D\u044B\u0439 \u0414\u0440\u0443\u0438\u0434 \u0441 \u043E\u043C\u0435\u0440\u0437\u0435\u043D\u0438\u0435\u043C|"
            "\u043E\u0442\u0431\u0440\u043E\u0441\u0438\u043B \u0431\u044B \u0442\u0430\u043A\u0443\u044E \u043D\u0430\u0445\u043E\u0434\u043A\u0443, \u043D\u043E \u043D\u0435 \u0422\u0440\u0430\u043D\u0441. \u041E\u043D \u0441\u043F\u0440\u044F\u0442\u0430\u043B|"
            "\u0446\u0438\u043B\u0438\u043D\u0434\u0440 \u0443 \u0441\u0435\u0431\u044F \u0432 \u043A\u0435\u043B\u044C\u0435 \u0438 \u043F\u0440\u0435\u0434\u0430\u0432\u0430\u043B\u0441\u044F \u0440\u0430\u0437\u043C\u044B\u0448\u043B\u0435\u043D\u0438\u044F\u043C, \u0433\u043B\u044F\u0434\u044F|"
            "\u043D\u0430 \u0441\u0432\u043E\u0435 \u0442\u0443\u0441\u043A\u043B\u043E\u0435 \u043E\u0442\u0440\u0430\u0436\u0435\u043D\u0438\u0435 \u0432 \u0435\u0433\u043E \u043D\u0435\u0440\u0436\u0430\u0432\u0435\u044E\u0449\u0435\u043C \u043C\u0435\u0442\u0430\u043B\u043B\u0435.|"
            "\u0412 \u043D\u043E\u0447\u044C \u0441\u043E\u043B\u043D\u0446\u0435\u0441\u0442\u043E\u044F\u043D\u0438\u044F \u0435\u043C\u0443 \u043F\u0440\u0438\u0448\u043B\u043E \u043E\u0442\u043A\u0440\u043E\u0432\u0435\u043D\u0438\u0435: \u0437\u0430\u0433\u0430\u0434\u043E\u0447\u043D\u044B\u0435|"
            "\u0443\u0437\u043E\u0440\u044B \u0441\u043B\u043E\u0436\u0438\u043B\u0438\u0441\u044C \u0432 \u043D\u0435\u0447\u0435\u043B\u043E\u0432\u0435\u0447\u0435\u0441\u043A\u0438 \u043F\u0440\u0435\u043A\u0440\u0430\u0441\u043D\u044B\u0435 \u0447\u0435\u0440\u0442\u044B \u043B\u0438\u0446\u0430, \u0430|"
            "\u0442\u043E\u0440\u0447\u0430\u0449\u0438\u0435 \u043F\u0440\u043E\u0432\u043E\u0434\u0430 \u043F\u0440\u0435\u0434\u0441\u0442\u0430\u043B\u0438 \u0436\u0438\u043B\u0430\u043C\u0438 \u0438 \u0441\u043E\u0441\u0443\u0434\u0430\u043C\u0438, \u043D\u0435\u043A\u043E\u0433\u0434\u0430|"
            "\u043F\u0438\u0442\u0430\u044E\u0449\u0438\u043C\u0438 \u043C\u0435\u0442\u0430\u043B\u043B\u0438\u0447\u0435\u0441\u043A\u043E\u0435 \u0442\u0435\u043B\u043E. \u0422\u0440\u0430\u043D\u0441 \u0432\u0441\u043A\u043E\u0447\u0438\u043B \u0438 \u0432 \u0443\u0436\u0430\u0441\u0435|"
            "\u0432\u044B\u0431\u0435\u0436\u0430\u043B \u043F\u043E\u0434 \u0441\u0432\u043E\u0434 \u043E\u0433\u0440\u043E\u043C\u043D\u044B\u0445 \u0431\u0435\u0441\u0447\u0443\u0432\u0441\u0442\u0432\u0435\u043D\u043D\u044B\u0445 \u0434\u0435\u0440\u0435\u0432\u044C\u0435\u0432. \u041E\u043D|"
            "\u0443\u043F\u0430\u043B \u043D\u0430 \u0437\u0435\u043C\u043B\u044E \u0438 \u043F\u044B\u0442\u0430\u043B\u0441\u044F \u043E\u0441\u043E\u0437\u043D\u0430\u0442\u044C \u0438\u0441\u0442\u0438\u043D\u0443, \u043A\u043E\u0442\u043E\u0440\u0430\u044F|"
            "\u043E\u0442\u043A\u0440\u044B\u043B\u0430\u0441\u044C \u0435\u043C\u0443: \u043B\u044E\u0434\u0438 - \u043B\u0438\u0448\u044C \u0436\u0430\u043B\u043A\u043E\u0435 \u043F\u043E\u0434\u043E\u0431\u0438\u0435 \u0441\u0432\u043E\u0438\u0445|"
            "\u043C\u0435\u0442\u0430\u043B\u043B\u0438\u0447\u0435\u0441\u043A\u0438\u0445 \u0445\u043E\u0437\u044F\u0435\u0432, \u043A\u043E\u0442\u043E\u0440\u044B\u0435 \u0440\u0430\u0437\u043E\u0437\u043B\u0438\u043B\u0438\u0441\u044C \u0438 \u043E\u0441\u0442\u0430\u0432\u0438\u043B\u0438|"
            "\u0447\u0435\u043B\u043E\u0432\u0435\u0447\u0435\u0441\u0442\u0432\u043E \u0431\u0435\u0437 \u0441\u0432\u043E\u0435\u0433\u043E \u0440\u0443\u043A\u043E\u0432\u043E\u0434\u0441\u0442\u0432\u0430. \u041A\u043E\u0433\u0434\u0430 \u0448\u043E\u043A \u043E\u0441\u043E\u0437\u043D\u0430\u043D\u0438\u044F|"
            "\u043F\u0440\u043E\u0448\u0435\u043B, \u043F\u0435\u0440\u0435\u0434 \u0435\u0433\u043E \u043C\u044B\u0441\u043B\u0435\u043D\u043D\u044B\u043C \u0432\u0437\u043E\u0440\u043E\u043C \u043F\u0440\u0435\u0434\u0441\u0442\u0430\u043B\u043E \u0440\u0435\u0448\u0435\u043D\u0438\u0435,|"
            "\u0433\u0435\u043D\u0438\u0430\u043B\u044C\u043D\u043E\u0435 \u0432 \u0441\u0432\u043E\u0435\u0439 \u043F\u0440\u043E\u0441\u0442\u043E\u0442\u0435: \u043D\u0443\u0436\u043D\u043E \u0437\u0430\u0441\u0442\u0430\u0432\u0438\u0442\u044C \u0445\u043E\u0437\u044F\u0435\u0432|\u0432\u0435\u0440\u043D\u0443\u0442\u044C\u0441\u044F.|"
            "\u041F\u0440\u0438\u0445\u0432\u0430\u0442\u0438\u0432 \u0441 \u0441\u043E\u0431\u043E\u0439 \u043B\u0438\u0448\u044C \u0434\u0440\u0430\u0433\u043E\u0446\u0435\u043D\u043D\u0443\u044E \u043C\u0435\u0442\u0430\u043B\u043B\u0438\u0447\u0435\u0441\u043A\u0443\u044E \u0433\u043E\u043B\u043E\u0432\u0443,|"
            "\u0422\u0440\u0430\u043D\u0441 \u043F\u043E\u043A\u0438\u043D\u0443\u043B \u0434\u043E\u043C. \u0414\u043E\u043B\u0433\u0438\u0435 \u043C\u0435\u0441\u044F\u0446\u044B \u043E\u043D \u0441\u043A\u0438\u0442\u0430\u043B\u0441\u044F \u0432 \u043F\u043E\u0438\u0441\u043A\u0430\u0445|"
            "\u043C\u0430\u043B\u0435\u0439\u0448\u0438\u0445 \u0441\u043B\u0435\u0434\u043E\u0432 \u0425\u043E\u0437\u044F\u0435\u0432, \u043F\u043E\u043A\u0430 \u043E\u0434\u043D\u0430\u0436\u0434\u044B, \u043F\u0440\u0438\u0432\u043B\u0435\u0447\u0435\u043D\u043D\u044B\u0439|"
            "\u043A\u0430\u043A\u0438\u043C-\u0442\u043E \u0448\u0443\u043C\u043E\u043C, \u043D\u0435 \u0437\u0430\u0431\u0440\u0435\u043B \u0432 \u043D\u0435\u043E\u0431\u044B\u0447\u043D\u0443\u044E \u043F\u0435\u0449\u0435\u0440\u0443. \u0415\u0451 \u0441\u0442\u0435\u043D\u044B \u0438|"
            "\u043F\u043E\u043B \u043F\u043E\u043A\u0440\u044B\u0432\u0430\u043B \u0441\u043B\u043E\u0439 \u0431\u043B\u0435\u0441\u0442\u044F\u0449\u0435\u0433\u043E \u043C\u0435\u0442\u0430\u043B\u043B\u0430, \u0430 \u0438\u0437 \u0433\u043B\u0443\u0431\u0438\u043D\u044B|"
            "\u0434\u043E\u043D\u043E\u0441\u0438\u043B\u043E\u0441\u044C \u043D\u0438\u0437\u043A\u043E\u0435 \u0433\u0443\u0434\u0435\u043D\u0438\u0435, \u043E\u0442 \u043A\u043E\u0442\u043E\u0440\u043E\u0433\u043E \u0432\u043E\u043B\u043E\u0441\u044B \u0432\u0441\u0442\u0430\u0432\u0430\u043B\u0438|"
            "\u0434\u044B\u0431\u043E\u043C. \u041F\u0440\u043E\u0439\u0434\u044F \u043D\u0435\u043C\u043D\u043E\u0433\u043E \u0432\u043F\u0435\u0440\u0451\u0434, \u0422\u0440\u0430\u043D\u0441 \u043E\u0431\u043D\u0430\u0440\u0443\u0436\u0438\u043B \u0437\u0430\u0432\u0430\u043B,|"
            "\u043F\u043E\u043B\u043D\u043E\u0441\u0442\u044C\u044E \u043F\u0435\u0440\u0435\u0433\u043E\u0440\u0430\u0436\u0438\u0432\u0430\u044E\u0449\u0438\u0439 \u043F\u0440\u043E\u0445\u043E\u0434. \u041E\u043D \u0440\u0435\u0448\u0438\u043B \u0431\u044B\u043B\u043E \u0443\u0439\u0442\u0438,|"
            "\u043D\u043E \u0435\u0433\u043E \u043E\u0441\u0442\u0430\u043D\u043E\u0432\u0438\u043B \u0437\u043D\u0430\u043A\u043E\u043C\u044B\u0439 \u0431\u043B\u0435\u0441\u043A: \u0438\u0437 \u043A\u0430\u043C\u043D\u0435\u0439 \u0442\u043E\u0440\u0447\u0430\u043B\u0430 \u043A\u0438\u0441\u0442\u044C|\u043C\u0435\u0442\u0430\u043B\u043B\u0438\u0447\u0435\u0441\u043A\u043E\u0439 \u0440\u0443\u043A\u0438.|"
            "\u0421\u043D\u0430\u0447\u0430\u043B\u0430 \u0422\u0440\u0430\u043D\u0441 \u0432 \u043E\u0434\u0438\u043D\u043E\u0447\u043A\u0443 \u0440\u0430\u0437\u0431\u0438\u0440\u0430\u043B \u0437\u0430\u0432\u0430\u043B, \u043F\u043E\u0442\u043E\u043C \u043A \u043D\u0435\u043C\u0443|"
            "\u043F\u0440\u0438\u0441\u043E\u0435\u0434\u0438\u043D\u0438\u043B\u0438\u0441\u044C \u0435\u0449\u0451 \u043D\u0435\u0441\u043A\u043E\u043B\u044C\u043A\u043E \u0447\u0435\u043B\u043E\u0432\u0435\u043A, \u043F\u0440\u0438\u0432\u043B\u0435\u0447\u0451\u043D\u043D\u044B\u0445 \u0435\u0433\u043E|"
            "\u0431\u0435\u0437\u0437\u0430\u0432\u0435\u0442\u043D\u044B\u043C \u0442\u0440\u0443\u0434\u043E\u043C \u0438 \u0441\u0442\u0440\u0430\u0441\u0442\u043D\u044B\u043C\u0438 \u0440\u0435\u0447\u0430\u043C\u0438. \u0422\u0430\u043A \u043D\u0430\u0447\u0430\u043B\u0430\u0441\u044C|_\u0438\u0441\u0442\u043E\u0440\u0438\u044F \u0434\u0435\u0442\u0435\u0439 \u0436\u0435\u043B\u0435\u0437\u0430._|#page|#center|"
            "*\u041E\u0434\u0438\u043D \u0440\u0430\u0437 \u0432 40 \u043B\u0435\u0442 \u043E\u0441\u043E\u0431\u043E \u0445\u043E\u043B\u043E\u0434\u043D\u043E\u0439 \u0437\u0438\u043C\u043E\u0439 \u043D\u0435\u0431\u043E \u043F\u043E\u043A\u0440\u044B\u0432\u0430\u0435\u0442\u0441\u044F*|"
            "\u0433\u043B\u0443\u0445\u0438\u043C\u0438 \u0447\u0435\u0440\u043D\u044B\u043C\u0438 \u0442\u0443\u0447\u0430\u043C\u0438 \u0442\u0430\u043A, \u0447\u0442\u043E \u0441\u043E\u043B\u043D\u0446\u0430 \u0441\u043E\u0432\u0441\u0435\u043C \u043D\u0435 \u0432\u0438\u0434\u043D\u043E \u0438|"
            "\u0441 \u043D\u0435\u0431\u0430 \u0432\u043C\u0435\u0441\u0442\u043E \u0434\u043E\u0436\u0434\u044F \u043D\u0430\u0447\u0438\u043D\u0430\u044E\u0442 \u043F\u0430\u0434\u0430\u0442\u044C \u0431\u0435\u043B\u044B\u0435 \u043A\u0430\u043A \u0441\u0430\u0432\u0430\u043D|"
            "\u043B\u0435\u0434\u044F\u043D\u044B\u0435 \u0445\u043B\u043E\u043F\u044C\u044F. \u041E\u043D\u0438 \u043F\u043E\u043A\u0440\u044B\u0432\u0430\u044E\u0442 \u0442\u043E\u043B\u0441\u0442\u044B\u043C \u0441\u043B\u043E\u0435\u043C \u0432\u0441\u044E \u0437\u0435\u043C\u043B\u044E.|"
            "\u041A\u0440\u044B\u0448\u0438 \u043F\u0440\u043E\u0432\u0430\u043B\u0438\u0432\u0430\u044E\u0442\u0441\u044F \u043F\u043E\u0434 \u0438\u0445 \u0432\u0435\u0441\u043E\u043C, \u0434\u0435\u0440\u0435\u0432\u044C\u044F \u043B\u043E\u043C\u0430\u044E\u0442\u0441\u044F, \u043A\u0430\u043A|"
            "\u0441\u043F\u0438\u0447\u043A\u0438. \u0427\u0435\u043B\u043E\u0432\u0435\u043A, \u043A\u043E\u0442\u043E\u0440\u043E\u0433\u043E \u044D\u0442\u0430 \u043D\u0430\u043F\u0430\u0441\u0442\u044C \u0437\u0430\u0441\u0442\u0430\u043D\u0435\u0442 \u043D\u0430 \u0443\u043B\u0438\u0446\u0435,|"
            "\u0437\u0430\u043C\u0435\u0440\u0437\u0430\u0435\u0442 \u043D\u0430\u0441\u043C\u0435\u0440\u0442\u044C \u0437\u0430 \u0441\u0447\u0438\u0442\u0430\u043D\u043D\u044B\u0435 \u043C\u0438\u043D\u0443\u0442\u044B. \u042D\u0442\u043E \u043D\u0430\u0437\u044B\u0432\u0430\u0435\u0442\u0441\u044F|\u2018\u2019\u0441\u043D\u0435\u0433\u2019\u2019.|"
            "\u0413\u043E\u0432\u043E\u0440\u044F\u0442, \u0447\u0442\u043E \u0433\u0434\u0435-\u0442\u043E \u0434\u0430\u043B\u0435\u043A\u043E \u043D\u0430 \u0441\u0435\u0432\u0435\u0440\u0435 \u0435\u0441\u0442\u044C \u0446\u0430\u0440\u0441\u0442\u0432\u043E|"
            "\u0432\u0435\u0447\u043D\u043E\u0433\u043E \u0445\u043E\u043B\u043E\u0434\u0430. \u0422\u0430\u043C \u0432 \u043D\u0435\u0431\u043E \u043F\u043E\u0434\u043D\u0438\u043C\u0430\u044E\u0442\u0441\u044F \u043E\u0433\u0440\u043E\u043C\u043D\u044B\u0435 \u043D\u0438\u043A\u043E\u0433\u0434\u0430|"
            "\u043D\u0435 \u0442\u0430\u044E\u0449\u0438\u0435 \u043A\u0443\u0447\u0438 \u2018\u2019\u0441\u043D\u0435\u0433\u0430\u2019\u2019, \u0447\u0451\u0440\u043D\u044B\u0435 \u0434\u0435\u0440\u0435\u0432\u044C\u044F \u0441 \u043B\u0438\u0448\u0435\u043D\u043D\u044B\u043C\u0438|"
            "\u043B\u0438\u0441\u0442\u044C\u0435\u0432 \u0432\u0435\u0442\u0432\u044F\u043C\u0438 \u0441\u0433\u0438\u0431\u0430\u044E\u0442\u0441\u044F \u043F\u043E\u0434 \u0435\u0433\u043E \u0433\u0440\u0443\u0437\u043E\u043C, \u0438 \u043D\u0438\u0433\u0434\u0435 \u043D\u0438|"
            "\u0442\u0440\u0430\u0432\u0438\u043D\u043A\u0438, \u0442\u043E\u043B\u044C\u043A\u043E \u0432\u0435\u0447\u043D\u043E \u0433\u043E\u043B\u043E\u0434\u043D\u044B\u0435 \u0447\u0443\u0434\u043E\u0432\u0438\u0449\u0430 \u0440\u044B\u0449\u0443\u0442 \u0432 \u043F\u043E\u0438\u0441\u043A\u0430\u0445|"
            "\u0434\u043E\u0431\u044B\u0447\u0438. \u041D\u043E \u044D\u0442\u043E, \u0441\u043A\u043E\u0440\u0435\u0435 \u0432\u0441\u0435\u0433\u043E, \u043F\u0440\u043E\u0441\u0442\u043E \u043F\u0443\u0441\u0442\u044B\u0435 \u0440\u043E\u0441\u0441\u043A\u0430\u0437\u043D\u0438,|"
            "\u0432\u0435\u0434\u044C \u0442\u0430\u043A\u0430\u044F \u043A\u0430\u0440\u0442\u0438\u043D\u0430 \u0441\u043B\u0438\u0448\u043A\u043E\u043C \u0441\u0442\u0440\u0430\u0448\u043D\u0430, \u0447\u0442\u043E\u0431\u044B \u0431\u044B\u0442\u044C|\u043F\u0440\u0430\u0432\u0434\u0438\u0432\u043E\u0439.|#page|#right|"
            "*\u0423\u0432\u0430\u0436\u0430\u0435\u043C\u044B\u0435 \u043A\u043E\u043B\u043B\u0435\u0433\u0438, \u0440\u0430\u0437\u0440\u0435\u0448\u0438\u0442\u0435 \u043C\u043D\u0435 \u043F\u0440\u0435\u0434\u0441\u0442\u0430\u0432\u0438\u0442\u044C \u0412\u0430\u0448\u0435\u043C\u0443*|"
            "\u0432\u043D\u0438\u043C\u0430\u043D\u0438\u044E \u0440\u0435\u0437\u0443\u043B\u044C\u0442\u0430\u0442\u044B \u043C\u043E\u0435\u0439 \u043C\u043D\u043E\u0433\u043E\u043B\u0435\u0442\u043D\u0435\u0439 \u0440\u0430\u0431\u043E\u0442\u044B. \u041D\u0435\u0441\u043C\u043E\u0442\u0440\u044F \u043D\u0430|"
            "\u043D\u0435\u043A\u043E\u0442\u043E\u0440\u0443\u044E \u0441\u0443\u043C\u0431\u0443\u0440\u043D\u043E\u0441\u0442\u044C \u0438\u0437\u043B\u043E\u0436\u0435\u043D\u0438\u044F, \u0441\u043F\u0435\u0448\u0443 \u0432\u0430\u0441 \u0437\u0430\u0432\u0435\u0440\u0438\u0442\u044C, \u0447\u0442\u043E|"
            "\u043C\u043E\u0438 \u0438\u0441\u0442\u043E\u0447\u043D\u0438\u043A\u0438 \u0441\u043E\u0432\u0435\u0440\u0448\u0435\u043D\u043D\u043E \u0434\u043E\u0441\u0442\u043E\u0432\u0435\u0440\u043D\u044B.|\u041A\u043E\u0433\u0434\u0430-\u0442\u043E \u0434\u0430\u0432\u043D\u043E \u0441\u0443\u0449\u0435\u0441\u0442\u0432\u043E\u0432\u0430\u043B\u0430 \u0432\u044B\u0441\u0448\u0430\u044F \u0440\u0430\u0441\u0430, \u0445\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u0438|"
            "\u0440\u0430\u0432\u043D\u043E\u0432\u0435\u0441\u0438\u044F, \u043A\u043E\u0442\u043E\u0440\u0430\u044F \u0433\u043B\u0430\u0432\u0435\u043D\u0441\u0442\u0432\u043E\u0432\u0430\u043B\u0430 \u0432\u043E \u0432\u0441\u0435\u043B\u0435\u043D\u043D\u043E\u0439 \u043D\u0435 \u043F\u043E|"
            "\u043F\u0440\u0430\u0432\u0443 \u0441\u0438\u043B\u044C\u043D\u043E\u0433\u043E, \u043D\u043E \u043F\u043E \u043F\u0440\u0430\u0432\u0443 \u043C\u0443\u0434\u0440\u043E\u0433\u043E \u0438 \u0441\u043F\u0440\u0430\u0432\u0435\u0434\u043B\u0438\u0432\u043E\u0433\u043E. \u041D\u043E|"
            "\u043E\u0434\u043D\u0430\u0436\u0434\u044B \u0438 \u044D\u0442\u0438 \u0441\u0443\u0449\u0435\u0441\u0442\u0432\u0430 \u0432\u0441\u0442\u0440\u0435\u0442\u0438\u043B\u0438\u0441\u044C \u0441 \u043E\u043F\u0430\u0441\u043D\u043E\u0441\u0442\u044C\u044E,|\u0441\u043F\u0440\u0430\u0432\u0438\u0442\u044C\u0441\u044F \u0441 \u043A\u043E\u0442\u043E\u0440\u043E\u0439 \u043D\u0435 \u0441\u043C\u043E\u0433\u043B\u0438...|"
            "\u041E\u043D\u0438 \u0441\u043F\u0440\u044F\u0442\u0430\u043B\u0438 \u0441\u0432\u043E\u0438\u0445 \u0434\u0435\u0442\u0435\u0439 \u043D\u0430 \u0434\u0430\u043B\u0435\u043A\u043E\u0439 \u0432\u0441\u0435\u043C\u0438 \u0437\u0430\u0431\u044B\u0442\u043E\u0439|"
            "\u043F\u043B\u0430\u043D\u0435\u0442\u0435 \u0438 \u043E\u0441\u0442\u0430\u0432\u0438\u043B\u0438 \u043D\u0430\u0431\u043B\u044E\u0434\u0430\u0442\u044C \u0437\u0430 \u043D\u0438\u043C\u0438 \u0438 \u043E\u0431\u0435\u0440\u0435\u0433\u0430\u0442\u044C \u0438\u0445|"
            "\u0441\u0432\u043E\u0438\u0445 \u0432\u0435\u0440\u043D\u044B\u0445 \u043F\u043E\u043C\u043E\u0449\u043D\u0438\u043A\u043E\u0432: \u041D\u044C\u0435\u0440\u0438. \u041C\u043D\u043E\u0433\u0438\u0435 \u0433\u043E\u0434\u044B \u043F\u0440\u043E\u0448\u043B\u0438,|"
            "\u0440\u043E\u0434\u0438\u0442\u0435\u043B\u0438 \u0442\u0430\u043A \u0438 \u043D\u0435 \u0432\u0435\u0440\u043D\u0443\u043B\u0438\u0441\u044C. \u0412\u0441\u0435 \u043E\u043D\u0438 \u043F\u0430\u043B\u0438 \u0432 \u0431\u0438\u0442\u0432\u0435 \u0441|"
            "\u0412\u0440\u0430\u0433\u043E\u043C. \u041D\u043E \u041D\u044C\u0435\u0440\u0438 \u043F\u0440\u043E\u0434\u043E\u043B\u0436\u0430\u043B\u0438 \u0441\u043B\u0435\u0434\u0438\u0442\u044C \u0437\u0430 \u043F\u043B\u0430\u043D\u0435\u0442\u043E\u0439 \u0438 \u0435\u0451|"
            "\u043E\u0431\u0438\u0442\u0430\u0442\u0435\u043B\u044F\u043C\u0438 \u2013 \u0442\u0430\u043A \u0432\u0435\u043B\u0438\u043A\u0430 \u0431\u044B\u043B\u0430 \u0438\u0445 \u043F\u0440\u0438\u0432\u044F\u0437\u0430\u043D\u043D\u043E\u0441\u0442\u044C \u043A \u0441\u0442\u0430\u0440\u044B\u043C|"
            "\u0434\u0440\u0443\u0437\u044C\u044F\u043C. \u041A\u0440\u043E\u043C\u0435 \u0442\u043E\u0433\u043E, \u043E\u043D\u0438 \u0437\u043D\u0430\u043B\u0438, \u0447\u0442\u043E \u0432 \u0438\u0445 \u0440\u0443\u043A\u0430\u0445 \u043D\u0430\u0434\u0435\u0436\u0434\u0430|"
            "\u043D\u0430 \u0432\u043E\u0441\u0441\u0442\u0430\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 \u043C\u0438\u0440\u0430 \u0432\u043E \u0432\u0441\u0435\u043B\u0435\u043D\u043D\u043E\u0439.|\u0421\u0442\u0430\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435 \u0445\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u0435\u0439 \u0440\u0430\u0432\u043D\u043E\u0432\u0435\u0441\u0438\u044F \u2013 \u043E\u0447\u0435\u043D\u044C \u0434\u043E\u043B\u0433\u0438\u0439|"
            "\u043C\u043D\u043E\u0433\u043E\u0441\u0442\u0443\u043F\u0435\u043D\u0447\u0430\u0442\u044B\u0439 \u043F\u0440\u043E\u0446\u0435\u0441\u0441. \u0412\u0435\u0434\u044C \u043E\u0431\u0440\u0435\u0442\u0435\u043D\u0438\u0435 \u0431\u0435\u0437\u0433\u0440\u0430\u043D\u0438\u0447\u043D\u043E\u0439|"
            "\u0441\u0438\u043B\u044B \u0442\u0440\u0435\u0431\u0443\u0435\u0442 \u0438 \u043E\u0431\u0440\u0435\u0442\u0435\u043D\u0438\u044F \u0431\u0435\u0437\u0433\u0440\u0430\u043D\u0438\u0447\u043D\u043E\u0433\u043E \u0447\u0443\u0432\u0441\u0442\u0432\u0430|"
            "\u043E\u0442\u0432\u0435\u0442\u0441\u0442\u0432\u0435\u043D\u043D\u043E\u0441\u0442\u0438. \u041F\u043E\u044D\u0442\u043E\u043C\u0443 \u043F\u0440\u0435\u0436\u0434\u0435 \u0447\u0435\u043C \u0441\u0442\u0430\u0442\u044C \u0441\u0430\u043C\u0438\u043C\u0438 \u0441\u043E\u0431\u043E\u0439,|"
            "\u0445\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u0438 \u0434\u043E\u043B\u0436\u043D\u044B \u043F\u0440\u043E\u0439\u0442\u0438 \u043F\u043E\u043B\u043D\u044B\u0439 \u0446\u0438\u043A\u043B \u044D\u0432\u043E\u043B\u044E\u0446\u0438\u0438 \u043E\u0442|"
            "\u043F\u0440\u043E\u0441\u0442\u0435\u0439\u0448\u0438\u0445 \u0441\u0443\u0449\u0435\u0441\u0442\u0432 \u0434\u043E \u0432\u044B\u0441\u043E\u043A\u043E\u0440\u0430\u0437\u0432\u0438\u0442\u044B\u0445 \u043D\u0435\u043C\u0430\u0442\u0435\u0440\u0438\u0430\u043B\u044C\u043D\u044B\u0445|"
            "\u0441\u043E\u0437\u0434\u0430\u043D\u0438\u0439. \u0422\u0430\u043A \u0440\u0435\u0448\u0438\u043B\u0438 \u043E\u043D\u0438 \u0441\u0430\u043C\u0438. \u041C\u043E\u0436\u043D\u043E \u0441\u043A\u0430\u0437\u0430\u0442\u044C, \u0447\u0442\u043E|"
            "\u0425\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u044C \u2013 \u044D\u0442\u043E \u043A\u043E\u043D\u0435\u0447\u043D\u044B\u0439 \u0438\u0442\u043E\u0433 \u0440\u0430\u0437\u0432\u0438\u0442\u0438\u044F \u043B\u044E\u0431\u043E\u0439 \u0436\u0438\u0437\u043D\u0438.|"
            "\u041F\u043B\u0430\u043D\u0435\u0442\u0430 \u0417\u0435\u043C\u043B\u044F, \u0441\u0442\u0430\u0432\u0448\u0430\u044F \u0434\u043E\u043C\u043E\u043C \u0438 \u0434\u0435\u0442\u0441\u043A\u0438\u043C \u0441\u0430\u0434\u043E\u043C \u0434\u043B\u044F|"
            "\u043F\u043E\u0442\u043E\u043C\u043A\u043E\u0432 \u0445\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u0435\u0439, \u0440\u0430\u0441\u0446\u0432\u0435\u043B\u0430 \u0432\u0441\u0435\u043C\u0438 \u043A\u0440\u0430\u0441\u043A\u0430\u043C\u0438 \u0436\u0438\u0437\u043D\u0438. \u041A|"
            "\u0441\u043E\u0436\u0430\u043B\u0435\u043D\u0438\u044E, \u0431\u0435\u0437 \u043F\u043E\u043C\u043E\u0449\u0438 \u0440\u043E\u0434\u0438\u0442\u0435\u043B\u0435\u0439 \u0434\u0435\u0442\u0438 \u0437\u0430\u0431\u044B\u043B\u0438 \u043E \u0441\u0432\u043E\u0435\u043C|"
            "\u043F\u0440\u0435\u0434\u043D\u0430\u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0438 \u0438 \u043F\u0440\u043E\u0434\u043E\u043B\u0436\u0430\u043B\u0438 \u0441\u043E\u0432\u0435\u0440\u0448\u0435\u043D\u0441\u0442\u0432\u043E\u0432\u0430\u0442\u044C\u0441\u044F \u0442\u043E\u043B\u044C\u043A\u043E|"
            "\u043A\u0430\u043A \u043E\u0431\u044B\u0447\u043D\u044B\u0435 \u0441\u0443\u0449\u0435\u0441\u0442\u0432\u0430 \u0438\u0437 \u043F\u043B\u043E\u0442\u0438 \u0438 \u043A\u0440\u043E\u0432\u0438. \u0420\u0435\u0434\u043A\u0438 \u0431\u044B\u043B\u0438|"
            "\u043E\u0437\u0430\u0440\u0435\u043D\u0438\u044F \u0441\u0440\u0435\u0434\u0438 \u043D\u0438\u0445. \u041D\u044C\u0435\u0440\u0438 \u043D\u0438\u0447\u0435\u043C \u043D\u0435 \u043C\u043E\u0433\u043B\u0438 \u0438\u043C \u043F\u043E\u043C\u043E\u0447\u044C, \u0432\u0435\u0434\u044C|"
            "\u0441\u0430\u043C\u0438 \u043E\u043D\u0438 \u0431\u044B\u043B\u0438 \u043B\u0438\u0448\u0435\u043D\u044B \u0441\u043F\u043E\u0441\u043E\u0431\u043D\u043E\u0441\u0442\u0438 \u0432\u043E\u0441\u043F\u0430\u0440\u0438\u0442\u044C \u0434\u0443\u0445\u043E\u043C \u043D\u0430\u0434|"
            "\u0431\u0440\u0435\u043D\u043D\u044B\u043C \u043C\u0438\u0440\u043E\u043C \u0438 \u0441\u0432\u043E\u0431\u043E\u0434\u043D\u043E \u0442\u0432\u043E\u0440\u0438\u0442\u044C \u0441\u0430\u043C\u0438\u0445 \u0441\u0435\u0431\u044F \u0438 \u0432\u0441\u0435\u043B\u0435\u043D\u043D\u0443\u044E|"
            "\u0432\u043E\u043A\u0440\u0443\u0433. \u041E\u043D\u0438 \u043C\u043E\u0433\u043B\u0438 \u043B\u0438\u0448\u044C \u0441\u043B\u0435\u0434\u0438\u0442\u044C \u0437\u0430 \u0444\u0438\u0437\u0438\u0447\u0435\u0441\u043A\u0438\u043C \u0437\u0434\u043E\u0440\u043E\u0432\u044C\u0435\u043C|"
            "\u0434\u0435\u0442\u0435\u0439 \u0438 \u0441\u0442\u0440\u043E\u0433\u043E \u0441\u043B\u0435\u0434\u043E\u0432\u0430\u0442\u044C \u0438\u043D\u0441\u0442\u0440\u0443\u043A\u0446\u0438\u0438, \u0434\u0430\u043D\u043D\u043E\u0439 \u0438\u043C|"
            "\u0445\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u044F\u043C\u0438: \u0436\u0434\u0430\u0442\u044C \u043D\u0430\u0437\u043D\u0430\u0447\u0435\u043D\u043D\u043E\u0433\u043E \u0447\u0430\u0441\u0430 \u0438, \u043A\u043E\u0433\u0434\u0430 \u043E\u043D|\u043D\u0430\u0441\u0442\u0430\u043D\u0435\u0442, \u043F\u043E\u043C\u043E\u0447\u044C \u0434\u0435\u0442\u044F\u043C \u0440\u0430\u0441\u043F\u0440\u0430\u0432\u0438\u0442\u044C \u043A\u0440\u044B\u043B\u044C\u044F.|"
            "\u0423 \u0445\u0440\u0430\u043D\u0438\u0442\u0435\u043B\u0435\u0439 \u0431\u044B\u043B\u043E \u0442\u043E\u043B\u044C\u043A\u043E \u043E\u0434\u043D\u043E \u0441\u043B\u0430\u0431\u043E\u0435 \u043C\u0435\u0441\u0442\u043E. \u041E\u043D\u0438 \u043D\u0435 \u043C\u043E\u0433\u043B\u0438|"
            "\u043F\u0440\u043E\u0439\u0442\u0438 \u043F\u043E\u0441\u043B\u0435\u0434\u043D\u044E\u044E, \u0441\u0430\u043C\u0443\u044E \u0441\u043B\u043E\u0436\u043D\u0443\u044E \u0442\u0440\u0430\u043D\u0441\u0444\u043E\u0440\u043C\u0430\u0446\u0438\u044E|"
            "\u0441\u0430\u043C\u043E\u0441\u0442\u043E\u044F\u0442\u0435\u043B\u044C\u043D\u043E. \u0414\u043B\u044F \u043F\u0435\u0440\u0435\u0445\u043E\u0434\u0430 \u0438\u043C \u043D\u0443\u0436\u0434\u0430 \u0431\u044B\u043B\u0430 \u043E\u043F\u0440\u0435\u0434\u0435\u043B\u0451\u043D\u043D\u0430\u044F|"
            "\u0438\u043D\u0444\u043E\u0440\u043C\u0430\u0446\u0438\u043E\u043D\u043D\u043E-\u044D\u043D\u0435\u0440\u0433\u0435\u0442\u0438\u0447\u0435\u0441\u043A\u0430\u044F \u0441\u0440\u0435\u0434\u0430, \u043A\u043E\u0442\u043E\u0440\u0430\u044F \u043E\u0431\u044B\u0447\u043D\u043E|"
            "\u0441\u043E\u0437\u0434\u0430\u0432\u0430\u043B\u0430\u0441\u044C \u043F\u0440\u0438\u0441\u0443\u0442\u0441\u0442\u0432\u0438\u0435\u043C \u043F\u043E\u0431\u043B\u0438\u0437\u043E\u0441\u0442\u0438 \u0440\u043E\u0434\u0438\u0442\u0435\u043B\u0435\u0439. \u041E\u0434\u043D\u0430\u043A\u043E \u0432|"
            "\u043A\u0440\u0430\u0439\u043D\u0435\u043C \u0441\u043B\u0443\u0447\u0430\u0435 \u0442\u0430\u043A\u0443\u044E \u0441\u0440\u0435\u0434\u0443 \u043C\u043E\u0436\u043D\u043E \u0431\u044B\u043B\u043E \u044D\u043C\u0443\u043B\u0438\u0440\u043E\u0432\u0430\u0442\u044C \u043F\u0440\u0438|\u043F\u043E\u043C\u043E\u0449\u0438 \u0441\u043F\u0435\u0446\u0438\u0430\u043B\u044C\u043D\u043E\u0433\u043E \u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u0430.|"
            "\u0422\u0430\u043A\u0438\u043C\u0438 \u0443\u0441\u0442\u0440\u043E\u0439\u0441\u0442\u0432\u0430\u043C\u0438 \u0431\u044B\u043B\u0438 \u0441\u043D\u0430\u0431\u0436\u0435\u043D\u044B \u0432\u0441\u0435 \u043A\u043E\u0440\u0430\u0431\u043B\u0438 \u041D\u044C\u0435\u0440\u0438,|"
            "\u0447\u0442\u043E\u0431\u044B \u043D\u0438 \u0432 \u043A\u043E\u0435\u043C \u0441\u043B\u0443\u0447\u0430\u0435 \u043D\u0435 \u043F\u0440\u043E\u043F\u0443\u0441\u0442\u0438\u0442\u044C \u043C\u043E\u043C\u0435\u043D\u0442 \u043F\u0435\u0440\u0435\u0445\u043E\u0434\u0430.|"
            "\u0412\u0441\u0435\u0433\u0434\u0430 \u0445\u043E\u0442\u044F \u0431\u044B \u043E\u0434\u0438\u043D \u043A\u043E\u0440\u0430\u0431\u043B\u044C \u0434\u043E\u043B\u0436\u0435\u043D \u0431\u044B\u043B \u043D\u0430\u0445\u043E\u0434\u0438\u0442\u044C\u0441\u044F \u043D\u0430|\u043E\u0440\u0431\u0438\u0442\u0435 \u043F\u043B\u0430\u043D\u0435\u0442\u044B.|"
            "\u041F\u043E\u0441\u0442\u043E\u0439\u0442\u0435, \u043A\u043E\u043B\u043B\u0435\u0433\u0438, \u043A\u0443\u0434\u0430 \u0432\u044B? \u042F \u043D\u0435 \u0440\u0430\u0441\u0441\u043A\u0430\u0437\u0430\u043B \u0441\u0430\u043C\u0443\u044E \u0432\u0430\u0436\u043D\u0443\u044E|"
            "\u0447\u0430\u0441\u0442\u044C! \u0412\u044B \u043c\u043d\u0435 \u043d\u0435 \u0432\u0435\u0440\u0438\u0442\u0435? \u041d\u043e \u044f \u0436\u0435 \u0441\u0430\u043c \u043b\u0438\u0447\u043d\u043e... [\u0417\u0430\u043f\u0438\u0441\u044c|_\u043e\u0431\u0440\u044b\u0432\u0430\u0435\u0442\u0441\u044f]_|"
            "#right|![Druids splash](data\\if\\ico_hd\\Splashes\\druids.dds)";

        std::string BookTextForTrigger(const std::string& token) {
            std::string text = BOOK_TEXT;
            if (token == "openauto" || token == "openpages2") {
                const std::string pageBreak = "|#page|";
                size_t at = 0;
                while ((at = text.find(pageBreak, at)) != std::string::npos)
                    text.replace(at, pageBreak.size(), "|");
            }
            return text;
        };

        uint32_t WndGameDataFlags(void* wnd) {
            // The shipped GetGameDataFlags entry is a usercall, not a regular
            // thiscall. Read the already verified Wnd field instead of invoking
            // it through an ABI that the test driver cannot express safely.
            return static_cast<uint32_t>(static_cast<hta::m3d::ui::Wnd*>(wnd)->m_gameDataFlags);
        };

        void AddStationString(void* station, const hta::CStr& key, const hta::CStr& value) {
            if (station)
                static_cast<hta::m3d::ui::WndStation*>(station)->m_strings.add(key, value);
        };

        // ---- the J-key journal-open path -------------------------------------------------
        // Verified chain (PDB + image bytes): J pressed -> ui event 0x10091 ->
        // MotherPanel::GameDataUpdate -> ToggleTab(2) -> SetCurTab(TAB_JOURNAL, 1)
        // -> OnJournal (RVA 0x5EDE0) -> ShowPanels: AddChildPanel(PANEL_PALM = the journal,
        //    wnd 16) + mgr->ShowWindow(7, 1, 0, 0, 1, NULL) for the mother panel. This
        // helper calls SetCurTab (RVA 0x60AE0) directly - the exact game code path, no
        // impulse emulation. SetCurTab is a __usercall: this in ECX, the tab id in EAX,
        // bUpdatePanels is the only stack argument (`ret 4`); its own frame (sub esp,0x10)
        // does not touch the caller's stack, so the bare call below is safe. It is a
        // TOGGLE (ToggleTab closes when the tab is already current), so it may be sent
        // at most once per trigger - a second send would close the journal again.
        void MotherOpenJournal(void* mother) {
            static_cast<hta::MotherPanel*>(mother)->SetCurTab(hta::MotherPanel::TAB_JOURNAL, true);
        };

        int32_t ListItemCount(
            const hta::m3d::ui::ListBoxWnd<hta::BooksWnd::BookListEntry*>* list) {
            return list ? static_cast<int32_t>(list->m_items.size()) : 0;
        };

        constexpr uintptr_t SECTION_TEXT_LO  = 0x00400000;
        constexpr uintptr_t SECTION_TEXT_HI  = 0x00990000;
        constexpr uintptr_t SECTION_RDATA_LO = 0x0098E000;
        constexpr uintptr_t SECTION_RDATA_HI = 0x009FF000;
        constexpr uintptr_t PTR_HEAP_LO      = 0x00100000;
        constexpr uintptr_t PTR_HEAP_HI      = 0x7FFFFFFF;

        struct WalkStats
        {
            int32_t visited = 0;
            int32_t pruned  = 0;
        };

        bool IsHeapPtr(uintptr_t p) {
            return p >= PTR_HEAP_LO && p <= PTR_HEAP_HI;
        };

        bool IsReadable(const void* address, size_t size) {
            if (!address || size == 0)
                return false;
            MEMORY_BASIC_INFORMATION info{};
            const auto queried = VirtualQuery(address, &info, sizeof(info));
            if (queried != sizeof(info) || info.State != MEM_COMMIT
                || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
                return false;
            const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress)
                                      + info.RegionSize;
            return begin <= regionEnd && size <= regionEnd - begin;
        }

        bool IsReadableCString(const char* text, size_t maxLength = 256) {
            if (!text)
                return false;
            for (size_t i = 0; i < maxLength; ++i) {
                if (!IsReadable(text + i, sizeof(char)))
                    return false;
                if (text[i] == '\0')
                    return i > 0;
            }
            return false;
        }

        // node is a live m3d::Object/ui::Wnd candidate: vtable in .rdata and the GetRtClass
        // slot (vft + 0x34) into .text. Lets IsKindOf be called without risking a bad vcall.
        bool IsValidUiNode(void* node) {
            const uintptr_t p = (uintptr_t) node;
            if (!IsHeapPtr(p) || !IsReadable(node, sizeof(hta::m3d::Object)))
                return false;
            const uintptr_t vft = *(const uintptr_t*) p;
            if (vft < SECTION_RDATA_LO || vft > SECTION_RDATA_HI
                || !IsReadable(reinterpret_cast<const void*>(vft + 0x34), sizeof(uintptr_t)))
                return false;
            const uintptr_t rtClass = *(const uintptr_t*)(vft + 0x34);
            return rtClass >= SECTION_TEXT_LO && rtClass <= SECTION_TEXT_HI;
        };

        bool ObjectIsKindOf(void* object, const hta::m3d::Class* classObject) {
            if (!IsValidUiNode(object) || !classObject)
                return false;
            return static_cast<const hta::m3d::Object*>(object)->IsKindOf(classObject);
        };

        // The live object at g_pApp+0x8B4EC: vtable in .rdata and the two slots we
        // vcall (GetWindow / Str2WndGuiId) into .text.
        bool IsUiManager(void* mgr) {
            const uintptr_t p = (uintptr_t) mgr;
            if (!IsHeapPtr(p) || !IsReadable(mgr, sizeof(uintptr_t)))
                return false;
            const uintptr_t vft = *(const uintptr_t*) p;
            if (vft < SECTION_RDATA_LO || vft > SECTION_RDATA_HI
                || !IsReadable(reinterpret_cast<const void*>(vft + UIIF_SLOT_GET_WINDOW * 4),
                                sizeof(uintptr_t))
                || !IsReadable(reinterpret_cast<const void*>(vft + UIIF_SLOT_STR2_GUI_ID * 4),
                                sizeof(uintptr_t)))
                return false;
            const uintptr_t getWin = *(const uintptr_t*)(vft + UIIF_SLOT_GET_WINDOW * 4);
            const uintptr_t str2Id = *(const uintptr_t*)(vft + UIIF_SLOT_STR2_GUI_ID * 4);
            return getWin >= SECTION_TEXT_LO && getWin <= SECTION_TEXT_HI
                && str2Id >= SECTION_TEXT_LO && str2Id <= SECTION_TEXT_HI;
        };

        int32_t UiStr2GuiId(void* mgr, const char* xmlId) {
            hta::CStr str(xmlId);
            return static_cast<const hta::ITruxxUiManager*>(mgr)->Str2WndGuiId(str);
        };

        // ITruxxUiManager::GetWindow (vtable slot 15) returns ref_ptr<m3d::ui::Wnd>
        // (8 bytes) BY VALUE: MSVC x86 by-value-struct calling convention - the caller
        // reserves an 8-byte slot and pushes its address as a hidden first argument,
        // then wndId; the callee does `ret 8`. Ground truth from the body bytes:
        // TruxxUiManager::GetWindow reads [esp+4]=&slot, [esp+8]=wndId and forwards
        // them (slot first) to GameUiManager::GUI_GetWindow (sub-object at +4), which
        // writes the Wnd* into *slot (and bumps the object refcount). Calling it as a
        // plain `void*(this, int)` lands wndId in the slot position and the game AVs
        // writing the pointer to the id address (observed: write to 0x7).
        void* UiGetWindow(void* mgr, int32_t guiId) {
            if (guiId < 0)
                return nullptr; // Str2WndGuiId miss - do not feed an invalid key into GetWindow
            const ref_ptr<hta::m3d::ui::Wnd> wnd =
                static_cast<const hta::ITruxxUiManager*>(mgr)->GetWindow(guiId);
            return wnd.m_ptr;
        };

        // Depth-first walk of the m3d::Object child tree, first node whose
        // runtime class derives from the given class object. Dangling links are pruned
        // (counted) instead of vcallled.
        void* FindFirstOfKind(void* root, const hta::m3d::Class* classObject, WalkStats* stats) {
            std::vector<void*> stack;
            std::unordered_set<void*> visited;
            stack.push_back(root);
            while (!stack.empty()) {
                void* node = stack.back();
                stack.pop_back();
                if (!node)
                    continue;
                if (!visited.insert(node).second)
                    continue;
                if (!IsValidUiNode(node)) {
                    ++stats->pruned;
                    continue;
                }
                ++stats->visited;
                if (ObjectIsKindOf(node, classObject))
                    return node;
                const hta::m3d::Object* object = static_cast<const hta::m3d::Object*>(node);
                for (const hta::m3d::Object* cur = object->m_firstChild;
                     cur && IsReadable(cur, sizeof(hta::m3d::Object));
                     cur = cur->m_nextSibling)
                    stack.push_back(const_cast<hta::m3d::Object*>(cur));
            }
            return nullptr;
        };

        void* FindFirstOfKindInRegistry(hta::m3d::ui::WndStation* station,
                                         const hta::m3d::Class* classObject, WalkStats* stats) {
            if (!station)
                return nullptr;
            for (const auto& item : station->m_allWindowsById.m_hash) {
                void* wnd = item.second;
                ++stats->visited;
                if (wnd && IsValidUiNode(wnd) && ObjectIsKindOf(wnd, classObject))
                    return wnd;
            }
            return nullptr;
        };

        // CMiracle3d::m_curGameMode @ +0x8B530 holds the GameState as a plain int
        // (0 = GS_GAME, 1 = GS_CINEMATIC, 2 = GS_MAINMENU, 3 = GS_INITIALIZATION).
        // -1 if g_pApp is not populated yet (very early boot). Both drop-box requests
        // are deferrable: a LoadSavedGame issued during GS_INITIALIZATION deadlocks the
        // call, and the books UI simply does not exist outside GS_GAME.
        int32_t CurGameMode() {
            hta::CMiracle3d* app = hta::CMiracle3d::Instance();
            if (!app)
                return -1;
            return static_cast<int32_t>(app->m_curGameMode.m_mode);
        };

        // m3d::ScriptServer::executeBuffer: runs Lua source from our own memory on the
        // engine's script VM (same thread as ProcessAllEvents - no locking needed).
        // Returns m3d::eScriptError (0 = SUCCESS), or -1 if the server is unreachable.
        int32_t ExecScriptBuffer(const char* src, const char* name) {
            hta::m3d::Kernel* kernel = hta::m3d::Kernel::Instance();
            if (!kernel || !kernel->m_scriptServer)
                return -1;
            return static_cast<int32_t>(kernel->m_scriptServer->executeBuffer(
                const_cast<void*>(static_cast<const void*>(src)),
                static_cast<uint32_t>(std::strlen(src)), name));
        };

        // Force the player vehicle's god mode: the body of data\scripts\cheats.lua god()
        // with the testcheat()/anticheat gate forced off. Idempotent, safe to repeat.
        int32_t ApplyGodMode() {
            return ExecScriptBuffer(GOD_LUA_SRC, "kraken_god");
        };

        int32_t CountExpectedLines(const char* src) {
            return src ? static_cast<int32_t>(kraken::ext::uibooks::ParseBookText(src).lines.size()) : 0;
        };
    }

    namespace {
        std::string g_lastSaveToken;
        std::string g_lastTriggerToken;
        std::string g_journalOpenToken;   // the trigger token for which the journal open call was already sent
        std::string g_loadDeferredToken;
        std::string g_triggerDeferredToken;
        int32_t g_triggerRetries = 0;
        uint64_t g_triggerDeadlineMs = 0; // absolute GetTickCount64 deadline for the active token
        std::string g_triggerActiveToken; // the token the deadline belongs to
        std::string g_bookInjectionToken; // token whose book payload was injected
        constexpr uint64_t TRIGGER_RETRY_BUDGET_MS = 15000;
        // The engine can burn an unbounded number of frames inside one keep-alive tick
        // (a level-load catch-up burst exhausted a fixed 30-attempt budget in ~10 ms), so
        // the open-books budget is wall time, not attempt count.

        // The book was injected + the selection notification sent (once per token);
        // the layout + footer-nav checks then run on later ticks until the box's first
        // painted layout exists (LastPageTotal >= 2).
        struct BookProbe {
            bool        active = false;
            std::string token;
            void*       box = nullptr;
            int32_t     expectedLines = 0;
            std::string fail; // terminal failure detail ("") - filled by the stage functions
            // Resumable stages across painted ticks: 0 = footer half-click nav, 1 = engine
            // scroll-band sync (the band must be driven with the real page geometry),
            // 2 = in-page wheel scroll, 3 = issue the < prev footer-button click,
            // 4 = verify it + issue the > next footer-button click, 5 = verify it,
            // 6 = done. A footer-button click release reaches the box through the
            // engine notify chain (Wnd::CallParentNotify -> Application message 40)
            // one pump late, so each button click is issued in its own pass and the
            // page it must produce is checked in the next pass.
            int32_t     stage = 0;
        };
        BookProbe g_probe;

        int32_t g_prevMode = -1;
        bool g_godDone = false;
        uint64_t g_godSessionStartMs = 0;
        uint64_t g_godLastAttemptMs = 0;
        int32_t g_godLastError = 0;
        constexpr uint64_t GOD_RETRY_INTERVAL_MS = 1000;
        constexpr uint64_t GOD_GIVEUP_AFTER_MS   = 10000;

        enum class OpenResult { Done, NotReady, LayoutPending };

        std::string ReadFirstLine(const char* path) {
            std::ifstream f(path);
            if (!f.is_open())
                return "";
            std::string line;
            std::getline(f, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                line.pop_back();
            return line;
        };

        void Finish(const std::string& token, const std::string& status) {
            g_lastTriggerToken = token; // every Finish consumes the token
            const std::string done = std::string(detail::DROPBOX_DIR) + "/output_" + token + ".done";
            std::ofstream f(done);
            if (f.is_open())
                f << status << "\n";
            else
                LOG_ERROR("open_books: cannot write %s (is %s present?)", done.c_str(), detail::DROPBOX_DIR);
            LOG_INFO("open_books: done status=%s", status.c_str());
        };

        std::string ResolveSaveDir(const std::string& saveName) {
            namespace fs = std::filesystem;
            std::error_code ec;
            for (const auto& profileEntry : fs::directory_iterator("data/profiles", ec)) {
                if (ec || !profileEntry.is_directory())
                    continue;
                const fs::path candidate = profileEntry.path() / "saves" / saveName;
                std::error_code existsEc;
                if (fs::is_directory(candidate, existsEc))
                    return (candidate / "maps").string();
            }
            return "";
        };

        void CheckSaveLoadFile(void* self) {
            const std::string token = ReadFirstLine((std::string(detail::DROPBOX_DIR) + "/load_save.txt").c_str());
            if (token.empty() || token == g_lastSaveToken)
                return;

            // LoadSavedGame must be issued from the main menu or in-world. During startup the game
        // sits in GS_INITIALIZATION, where the same request wedges the call (observed:
        // process alive, no exception, engine log silent afterwards). Boot reaches the
        // menu in two steps: GS_INITIALIZATION -> GS_MAINMENU (menu level load, ~2 s) and
        // then automatically GS_MAINMENU -> GS_CINEMATIC (CinematicInit, the idle menu's
        // background cinematic) - the menu state the game actually rests in. GS_GAME is
        // the in-world state (in-world reloads work too). The token is not consumed until
        // it actually runs, so it is retried on every tick until a legal state is seen.
        const int32_t mode = detail::CurGameMode();
        if (mode != detail::GAME_MODE_GAME && mode != detail::GAME_MODE_CINEMATIC) {
            if (token != g_loadDeferredToken) {
                g_loadDeferredToken = token;
                LOG_WARNING("load_save: deferring '%s' until the menu/in-world is up (current mode %d)",
                            token.c_str(), mode);
            }
            return;
        }
            g_lastSaveToken = token;

            // "<saveName>#<nonce>" - same convention as the jolt testharness.
            const size_t hash = token.find('#');
            const std::string saveName = token.substr(0, hash);
            const std::string dir = ResolveSaveDir(saveName);
            if (dir.empty()) {
                LOG_WARNING("load_save: no save directory named '%s' under data/profiles/*/saves - ignored", saveName.c_str());
                LOG_INFO("load_save: LoadSavedGame returned 0 for '%s'", saveName.c_str());
                return;
            }

            LOG_INFO("load_save: loading '%s'", dir.c_str());
            hta::CStr saveDir(dir.c_str());
            const bool ok = static_cast<hta::CMiracle3d*>(self)->LoadSavedGame(saveDir);
            LOG_INFO("load_save: LoadSavedGame returned %d for '%s'", ok ? 1 : 0, saveName.c_str());
        };

        OpenResult TryOpenBooks(const std::string& token, bool verbose) {
            const int32_t expected = detail::CountExpectedLines(detail::BOOK_TEXT);
            const std::string bookText = detail::BookTextForTrigger(token);
            if (verbose)
                LOG_INFO("open_books: trigger '%s' - running open-books sequence (expected %d parsed lines, mode=%s)",
                         token.c_str(), expected, token == "openpages" ? "pages" : "scroll");
            LOG_INFO("open_books: resolving application instance");
            hta::CMiracle3d* app = hta::CMiracle3d::Instance();
            LOG_INFO("open_books: application instance=%p", app);
            hta::m3d::ui::WndStation* stationA = app ? app->GetStation() : nullptr;
            hta::m3d::ui::WndStation* stationB = app ? static_cast<hta::m3d::ui::WndStation*>(app) : nullptr;
            hta::m3d::ui::WndStation* station = stationA ? stationA : stationB;
            if (!station) {
                Finish(token, "no_station");
                return OpenResult::Done;
            }
            if (verbose)
                LOG_INFO("open_books: g_wndStation=%p g_pApp=%p g_pApp+0xC=%p (station root %p)",
                         stationA, static_cast<void*>(app), stationB, station);

            detail::WalkStats stats;
            void* journal = nullptr;
            void* booksChain = nullptr;

            // Primary path: the PDB-verified CMiracle3d -> ITruxxUiManager chain - the
            // same window lookup the game's own UI code uses. The field holds a raw
            // pointer, so the manager object itself is one dereference deeper.
            void* mgrField = app ? static_cast<void*>(app->m_pInterfaceManager) : nullptr;
            void* mgr = mgrField;
            void* mother = nullptr;
            bool mgrValid = false;
            if (mgr && detail::IsUiManager(mgr)) {
                mgrValid = true;
                const int32_t idMother = detail::UiStr2GuiId(mgr, detail::ID_MOTHER_PANEL);
                const int32_t idJournal = detail::UiStr2GuiId(mgr, detail::ID_JOURNAL_WND);
                const int32_t idBooks = detail::UiStr2GuiId(mgr, detail::ID_BOOKS_WND);
                mother = detail::UiGetWindow(mgr, idMother);
                journal = detail::UiGetWindow(mgr, idJournal);
                booksChain = detail::UiGetWindow(mgr, idBooks);
                LOG_INFO("open_books: ui manager %p: Str2WndGuiId('%s')=%d, '%s'=%d, '%s'=%d -> mother=%p journal=%p books=%p",
                         mgr, detail::ID_MOTHER_PANEL, idMother,
                         detail::ID_JOURNAL_WND, idJournal,
                         detail::ID_BOOKS_WND, idBooks,
                         mother, journal, booksChain);
            } else {
                uintptr_t vft = 0;
                if (mgr)
                    vft = *(const uintptr_t*) mgr;
                LOG_WARNING("open_books: no valid ITruxxUiManager (field g_pApp+0x8B4EC %p, object %p, vft=%p) - falling back to the tree walk",
                             mgrField, mgr, reinterpret_cast<void*>(vft));
            }
            if (!mother) {
                mother = detail::FindFirstOfKind(station, hta::MotherPanel::ClassObject(), &stats);
                if (!mother && stationB && stationB != station)
                    mother = detail::FindFirstOfKind(stationB, hta::MotherPanel::ClassObject(), &stats);
                if (!mother)
                    mother = detail::FindFirstOfKindInRegistry(station, hta::MotherPanel::ClassObject(), &stats);
                if (!mother && stationB && stationB != station)
                    mother = detail::FindFirstOfKindInRegistry(stationB, hta::MotherPanel::ClassObject(), &stats);
            }
            if (!mother) {
                LOG_WARNING("open_books: MotherPanel not found (manager chain + walk %d visited / %d pruned) - UI may still be building",
                            stats.visited, stats.pruned);
                return OpenResult::NotReady;
            }
            LOG_INFO("open_books: mother=%p (walk: %d visited / %d pruned)", mother, stats.visited, stats.pruned);
            if (detail::WndGameDataFlags(mother) == 0) {
                LOG_WARNING("open_books: MotherPanel %p not inited yet", mother);
                return OpenResult::NotReady;
            }
            // State snapshot flushed before the journal-open call, so the panel state is in
            // the log even if the call misbehaves.
            LOG_INFO("open_books: pre-open mother=%p flags@0x114=%08X | journal=%p flags=%08X | books=%p flags=%08X",
                     mother, detail::WndGameDataFlags(mother),
                     journal,     journal    ? detail::WndGameDataFlags(journal) : 0,
                     booksChain,  booksChain ? detail::WndGameDataFlags(booksChain) : 0);
            if (!mgrValid) {
                LOG_WARNING("open_books: no valid ITruxxUiManager - the journal window cannot be found by id. Retrying.");
                return OpenResult::NotReady;
            }

            // Open the journal the way the game does: MotherPanel::SetCurTab(TAB_JOURNAL, true)
            // - the exact dispatch the J key ends up at (GameDataUpdate -> ToggleTab ->
            // SetCurTab -> OnJournal -> ShowPanels). The call is sent exactly once per token:
            // the dispatch is a TOGGLE, so a re-send would close the journal; the later
            // retries only re-check the windows.
            if (g_journalOpenToken != token) {
                detail::MotherOpenJournal(mother);
                g_journalOpenToken = token;
                LOG_INFO("open_books: MotherPanel::SetCurTab(TAB_JOURNAL=%d, true) sent | post mother=%p flags=%08X journal=%p flags=%08X",
                         static_cast<int32_t>(hta::MotherPanel::TAB_JOURNAL),
                         mother,    detail::WndGameDataFlags(mother),
                         journal,   journal ? detail::WndGameDataFlags(journal) : 0);
            }

            if (!journal) {
                journal = detail::FindFirstOfKind(station, game::JournalClassObject(), &stats);
                if (!journal && stationB && stationB != station)
                    journal = detail::FindFirstOfKind(stationB, game::JournalClassObject(), &stats);
                if (!journal)
                    journal = detail::FindFirstOfKindInRegistry(station, game::JournalClassObject(), &stats);
                if (!journal && stationB && stationB != station)
                    journal = detail::FindFirstOfKindInRegistry(stationB, game::JournalClassObject(), &stats);
                if (journal)
                    LOG_INFO("open_books: journal taken from the walk fallback: %p", journal);
            }
            if (!journal) {
                return OpenResult::NotReady;
            }
            if (!(detail::WndGameDataFlags(journal) & 1)) {
                LOG_WARNING("open_books: JournalWnd %p not inited yet", journal);
                return OpenResult::NotReady;
            }
            game::JournalSetCurTab(static_cast<hta::JournalWnd*>(journal),
                                     hta::JournalWnd::TAB_BOOKS, true);
            LOG_INFO("open_books: JournalWnd::SetCurTab(TAB_BOOKS, true)");

            auto* journalWnd = static_cast<hta::JournalWnd*>(journal);
            void* books = journalWnd->BooksTab();
            if (books && !detail::ObjectIsKindOf(books, hta::BooksWnd::ClassObject()))
                books = nullptr;
            if (!books) {
                books = booksChain;
                if (books)
                    LOG_INFO("open_books: books=%p taken from the ui manager chain (journal m_tabs[1] was not a BooksWnd)", books);
            }
            if (!books || !detail::ObjectIsKindOf(books, hta::BooksWnd::ClassObject())) {
                return OpenResult::NotReady;
            }
            if (!(detail::WndGameDataFlags(books) & 1)) {
                LOG_WARNING("open_books: BooksWnd %p not inited yet", books);
                return OpenResult::NotReady;
            }

            // Inject the book name into the station localization table too: the
            // journal checklist renders the item title through the same
            // WndStation::m_strings lookup (a miss would show "MISSING!" in the
            // books list).
            hta::CStr nameKey(detail::NAME_ID);
            hta::CStr nameValue(detail::BOOK_NAME_VALUE);
            detail::AddStationString(station, nameKey, nameValue);
            if (stationB && stationB != station)
                detail::AddStationString(stationB, nameKey, nameValue);

            const bool addAutoPagesBook = token == "openpages2";
            kraken::ext::uibooks::RegisterBookMode(
                detail::NAME_ID,
                token == "openpages" ? kraken::ext::uibooks::BookMode::Pages
                                      : kraken::ext::uibooks::BookMode::Scroll);
            if (addAutoPagesBook) {
                kraken::ext::uibooks::RegisterBookMode(
                    detail::NAME_ID_2, kraken::ext::uibooks::BookMode::Pages);
                hta::CStr nameKey2(detail::NAME_ID_2);
                hta::CStr nameValue2(detail::BOOK_NAME_2);
                detail::AddStationString(station, nameKey2, nameValue2);
                if (stationB && stationB != station)
                    detail::AddStationString(stationB, nameKey2, nameValue2);
            }

            // Inject the book text into the station localization table under the TEXT id:
            // BooksWnd::ShowBook looks the body up via WndStation::GetStringByStringId0
            // (WndStation::m_strings) with the book's textId, and renders
            // the literal "MISSING!" on a miss (seen as a single parsed line).
            const std::string firstBookText = detail::BookTextForTrigger("open1");
            hta::CStr textKey(detail::TEXT_ID);
            hta::CStr textView((addAutoPagesBook ? firstBookText : bookText).c_str());
            detail::AddStationString(station, textKey, textView);
            if (stationB && stationB != station)
                detail::AddStationString(stationB, textKey, textView);
            if (addAutoPagesBook) {
                hta::CStr textKey2(detail::TEXT_ID_2);
                hta::CStr textView2(bookText.c_str());
                detail::AddStationString(station, textKey2, textView2);
                if (stationB && stationB != station)
                    detail::AddStationString(stationB, textKey2, textView2);
            }
            LOG_INFO("open_books: injected string id '%s' -> %u-char book text", detail::TEXT_ID,
                     (uint32_t) (addAutoPagesBook ? firstBookText.size() : bookText.size()));
            if (addAutoPagesBook)
                LOG_INFO("open_books: injected second book '%s' -> %u-char text with pages property and no #page markers",
                         detail::BOOK_NAME_2, (uint32_t) bookText.size());
            LOG_INFO("open_books: visual check words: COLOR RED SAMPLE, COLOR GREEN SAMPLE, COLOR BLUE SAMPLE; mode is book property");

            hta::CStr nameIdArg(detail::NAME_ID);
            hta::CStr textIdArg(detail::TEXT_ID);
            if (g_bookInjectionToken != token) {
                const int32_t added = game::JournalAddBook(journalWnd, nameIdArg, textIdArg);
                if (added == 0) {
                    Finish(token, "add_book_failed");
                    return OpenResult::Done;
                }
                LOG_INFO("open_books: JournalWnd::AddBook ok (%d)", added);
                if (addAutoPagesBook) {
                    hta::CStr nameIdArg2(detail::NAME_ID_2);
                    hta::CStr textIdArg2(detail::TEXT_ID_2);
                    const int32_t added2 = game::JournalAddBook(journalWnd, nameIdArg2, textIdArg2);
                    if (added2 == 0) {
                        Finish(token, "add_second_book_failed");
                        return OpenResult::Done;
                    }
                    LOG_INFO("open_books: second JournalWnd::AddBook ok (%d)", added2);
                }
                g_bookInjectionToken = token;
            }

            auto* booksWnd = static_cast<hta::BooksWnd*>(books);
            auto* checkList = booksWnd->m_bookList;
            LOG_INFO("open_books: books state books=%d list=%p list_items=%d list_sel=%d box=%p",
                     (int32_t) booksWnd->m_books.size(), checkList,
                     checkList ? (int32_t) checkList->m_items.size() : 0,
                     checkList ? checkList->m_curSel : -1, booksWnd->m_book);
            if (!checkList) {
                if (verbose)
                    LOG_WARNING("open_books: checklist object is not ready yet");
                return OpenResult::NotReady;
            }

            const int32_t modelCount = static_cast<int32_t>(booksWnd->m_books.size());
            if (modelCount <= 0) {
                if (verbose)
                    LOG_WARNING("open_books: book model has no items yet; retrying after UI refresh");
                return OpenResult::NotReady;
            }
            const int32_t itemCount = detail::ListItemCount(checkList);
            if (itemCount <= 0) {
                Finish(token, "checklist_empty");
                return OpenResult::Done;
            }

            const int32_t curSel = itemCount - 1;
            if (curSel < 0 || curSel >= itemCount) {
                Finish(token, "checklist_selection_invalid");
                return OpenResult::Done;
            }

            // The engine resolves the selected book by the nameId stored in the
            // visible checklist row. The model can contain books that are not in
            // that list, so match a valid visible name to the model by name.
            const char* targetNameId = addAutoPagesBook ? detail::NAME_ID_2 : detail::NAME_ID;
            const char* selectedNameId = nullptr;
            const hta::BooksWnd::Book* selectedBook = nullptr;
            int32_t selectedRow = -1;
            for (size_t row = 0; row < checkList->m_items.size() && !selectedBook; ++row) {
                const auto* listEntry = checkList->m_items[row].m_item;
                const uintptr_t item = reinterpret_cast<uintptr_t>(listEntry);
                if (!detail::IsHeapPtr(item))
                    continue;
                const auto* listBook = listEntry->GetBook();
                if (!detail::IsReadable(listBook, sizeof(*listBook))
                    || !detail::IsReadableCString(listBook->m_nameId.m_charPtr)
                    || std::strcmp(listBook->m_nameId.m_charPtr, targetNameId) != 0)
                    continue;
                for (size_t i = 0; i < booksWnd->m_books.size(); ++i) {
                    const hta::BooksWnd::Book& modelBook = booksWnd->m_books[i];
                    if (!detail::IsReadableCString(modelBook.m_nameId.m_charPtr)
                        || std::strcmp(modelBook.m_nameId.m_charPtr, listBook->m_nameId.m_charPtr) != 0)
                        continue;
                    selectedNameId = modelBook.m_nameId.m_charPtr;
                    selectedBook = &modelBook;
                    selectedRow = static_cast<int32_t>(row);
                    break;
                }
            }
            if (!selectedBook || !detail::IsReadableCString(selectedBook->m_textId.m_charPtr)) {
                Finish(token, "checklist_book_not_found");
                return OpenResult::Done;
            }

            // Use the selected model record's real textId. This exercises the same
            // ShowBook/GetStringByStringId0 path as a normal game book and avoids
            // mutating the engine-owned CStr/list storage from the test.
            const bool pagesMode = token == "openpages" || token == "openpages2";
            kraken::ext::uibooks::RegisterBookMode(
                selectedNameId,
                pagesMode ? kraken::ext::uibooks::BookMode::Pages
                          : kraken::ext::uibooks::BookMode::Scroll);
            hta::CStr selectedTextKey(selectedBook->m_textId.m_charPtr);
            hta::CStr selectedTextValue(bookText.c_str());
            detail::AddStationString(station, selectedTextKey, selectedTextValue);
            if (stationB && stationB != station)
                detail::AddStationString(stationB, selectedTextKey, selectedTextValue);
            checkList->m_curSel = selectedRow;
            LOG_INFO("open_books: selected visible row by name '%s', model textId '%s' (row %d/%d)",
                     selectedNameId, selectedBook->m_textId.m_charPtr, selectedRow, itemCount);

            // The TextBoxWnd that will receive the book text (ShowBook takes it from the
            // same slot; the uibooks feature caches it through its OnWndNotify hook).
            void* box = booksWnd->m_book;
            if (!box || !detail::IsValidUiNode(box)) {
                Finish(token, "bookbox_missing");
                return OpenResult::Done;
            }

            // Synthesize the CheckList selection notification through the (uibooks-patched)
            // BooksWnd vtable slot 50: original OnWndNotify -> ShowBook ->
            // GetStringByStringId0 -> TextBoxWnd::SetText (slot 18, patched by the uibooks
            // feature, full parse/pagination path). `data` is unused by the original.
            hta::m3d::AIParam notifyParam;
            (void) kraken::ext::uibooks::Hook_BooksOnWndNotify(
                booksWnd, checkList, hta::BooksWnd::BOOK_LIST_ID,
                hta::m3d::ui::WNM_CHANGE, notifyParam);
            LOG_INFO("open_books: synthesized BooksWnd selection notification (item %d/%d), box=%p", selectedRow, itemCount, box);

            // SetText ran inside the notify call, so the parse diagnostics are final now.
            // The layout + footer-nav probes need a painted frame, so the rest of the
            // sequence continues on later ticks under the same token and budget.
            g_probe.active = true;
            g_probe.token = token;
            g_probe.box = box;
            g_probe.expectedLines = expected;
            g_probe.fail.clear();
            g_probe.stage = 0;
            LOG_INFO("open_books: book injected (parsed lines so far: %d, expected %d) - waiting for the painted layout",
                     kraken::ext::uibooks::LastParseLines(), expected);
            return OpenResult::LayoutPending;
        };

        // Layout + nav verification for the injected token. Consumes the token (via
        // Finish) on every terminal outcome - "ok" or a specific failure status. Does
        // nothing and leaves the probe active while a painted layout has not landed
        // or a later stage (engine sync / wheel scroll) is still waiting on a paint.
        void CheckBookLayout(const std::string& token, bool verbose) {
            const int32_t lines = kraken::ext::uibooks::LastParseLines();
            if (lines <= 0) {
                if (verbose)
                    LOG_WARNING("open_books: book SetText has not run yet - waiting");
                return;
            }
            if (lines != g_probe.expectedLines) {
                Finish(token, "settext_lines_" + std::to_string(lines) + "_expected_" + std::to_string(g_probe.expectedLines));
                return;
            }

            const bool autoScrollProbe = token == "openauto";
            const int32_t total = kraken::ext::uibooks::LastPageTotal();
            if (total < (autoScrollProbe ? 1 : 2)) {
                if (verbose)
                    LOG_WARNING("open_books: painted layout not ready yet (pages=%d) - waiting", total);
                return;
            }

            float x0, y0, cw, ch;
            if (!kraken::ext::uibooks::GetBookClientRect(&x0, &y0, &cw, &ch) || !(cw > 0.0f) || !(ch > 0.0f)) {
                if (verbose)
                    LOG_WARNING("open_books: book client rect not ready yet - waiting");
                return;
            }
            const float lineH = kraken::ext::uibooks::GetBookLineHeight();
            if (!(lineH > 0.0f)) {
                if (verbose)
                    LOG_WARNING("open_books: book line height not ready yet - waiting");
                return;
            }

            void* box = g_probe.box;
            if (!box || !detail::IsValidUiNode(box)) {
                g_probe.active = false;
                Finish(token, "probe_box_invalid");
                return;
            }

            if (g_probe.stage == 0) {
                const bool pagesModeProbe = token == "openpages" || token == "openpages2";
                const bool pagesAutoBreakProbe = token == "openpages2";
                const int32_t alignment = kraken::ext::uibooks::LastAlignment();
                const kraken::ext::uibooks::BookMode bookMode = kraken::ext::uibooks::LastBookMode();
                const int32_t explicitBreaks = kraken::ext::uibooks::LastExplicitBreaks();
                const uint32_t textColor = kraken::ext::uibooks::LastTextColor();
                const uint32_t styleMask = kraken::ext::uibooks::LastStyleMask();
                const int32_t coloredSegments = kraken::ext::uibooks::LastColoredSegments();
                const uint32_t color0 = kraken::ext::uibooks::LastColor(0);
                const uint32_t color1 = kraken::ext::uibooks::LastColor(1);
                const uint32_t color2 = kraken::ext::uibooks::LastColor(2);
                const int32_t pageAlign0 = kraken::ext::uibooks::GetBookPageAlignment(0);
                const int32_t pageAlign1 = kraken::ext::uibooks::GetBookPageAlignment(1);
                const int32_t pageAlign2 = kraken::ext::uibooks::GetBookPageAlignment(2);
                const int32_t pageAlign3 = pagesModeProbe ? kraken::ext::uibooks::GetBookPageAlignment(3) : -1;
                const int32_t pageAlign4 = pagesModeProbe ? kraken::ext::uibooks::GetBookPageAlignment(4) : -1;
                const int32_t sampleAlign0 = kraken::ext::uibooks::GetBookLineAlignment(0);
                const int32_t sampleAlign1 = kraken::ext::uibooks::GetBookLineAlignment(1);
                const int32_t sampleAlign2 = kraken::ext::uibooks::GetBookLineAlignment(2);
                LOG_INFO("open_books: text directives default_align=%d first_page_samples=%d/%d/%d page_alignments=%d/%d/%d/%d/%d mode=%s explicit_breaks=%d styles=0x%X colors=%d color=@%08X/@%08X/@%08X base=@%08X",
                         alignment, sampleAlign0, sampleAlign1, sampleAlign2,
                         pageAlign0, pageAlign1, pageAlign2, pageAlign3, pageAlign4,
                         bookMode == kraken::ext::uibooks::BookMode::Scroll ? "scroll" : "pages", explicitBreaks, styleMask,
                         coloredSegments, color0, color1, color2, textColor);
                const bool pageAlignmentOk = autoScrollProbe
                    ? pageAlign0 == hta::m3d::TF_LEFT
                    : pagesAutoBreakProbe
                    ? pageAlign0 == hta::m3d::TF_LEFT
                    : pagesModeProbe
                    ? pageAlign0 == hta::m3d::TF_LEFT
                        && pageAlign1 == hta::m3d::TF_RIGHT
                        && pageAlign2 == hta::m3d::TF_CENTER
                        && pageAlign3 == hta::m3d::TF_RIGHT
                        && pageAlign4 == hta::m3d::TF_RIGHT
                    : pageAlign0 == hta::m3d::TF_LEFT
                        && pageAlign1 == hta::m3d::TF_CENTER
                        && pageAlign2 == hta::m3d::TF_RIGHT;
                if (sampleAlign0 != hta::m3d::TF_LEFT
                    || sampleAlign1 != hta::m3d::TF_CENTER
                    || sampleAlign2 != hta::m3d::TF_RIGHT
                    || !pageAlignmentOk)
                {
                    g_probe.active = false;
                    Finish(token, "page_alignment_" + std::to_string(pageAlign0) + "_"
                           + std::to_string(pageAlign1) + "_" + std::to_string(pageAlign2));
                    return;
                }
                if (pagesModeProbe
                    ? bookMode != kraken::ext::uibooks::BookMode::Pages
                    : bookMode != kraken::ext::uibooks::BookMode::Scroll)
                {
                    g_probe.active = false;
                    Finish(token, pagesModeProbe ? "mode_not_pages" : "mode_not_scroll");
                    return;
                }
                const int32_t expectedBreaks = (autoScrollProbe || pagesAutoBreakProbe) ? 0 : 2;
                if (explicitBreaks != expectedBreaks)
                {
                    g_probe.active = false;
                    Finish(token, "page_breaks_" + std::to_string(explicitBreaks)
                           + "_expected_" + std::to_string(expectedBreaks));
                    return;
                }
                if (textColor == 0 || textColor == 0xFFFFFFFFu)
                {
                    g_probe.active = false;
                    Finish(token, "text_color_invalid");
                    return;
                }
                if ((styleMask & (hta::m3d::FONT_BOLD | hta::m3d::FONT_ITALIC))
                    != (hta::m3d::FONT_BOLD | hta::m3d::FONT_ITALIC))
                {
                    g_probe.active = false;
                    Finish(token, "style_mask_" + std::to_string(styleMask));
                    return;
                }
                if (coloredSegments < 3 || color0 != 0xFFFF3333u
                    || color1 != 0xFF33FF33u || color2 != 0xFF3333FFu) {
                    g_probe.active = false;
                    Finish(token, "colors_not_parsed");
                    return;
                }
                if (autoScrollProbe) {
                    probes::RunAutoScrollProbe(box, cw, ch, g_probe.fail);
                    if (!g_probe.fail.empty()) {
                        g_probe.active = false;
                        Finish(token, "auto_scroll_fail_" + g_probe.fail);
                        return;
                    }
                    g_probe.stage = 6;
                } else {
                    probes::RunNavProbes(box, g_probe.fail);
                }
                if (!g_probe.fail.empty()) {
                    g_probe.active = false;
                    Finish(token, "nav_fail_" + g_probe.fail);
                    return;
                }
                if (autoScrollProbe) {
                    // The one-page auto-scroll probe is complete; there is no footer
                    // navigation stage for a book without #page breaks.
                } else if (pagesModeProbe) {
                    probes::RunPagesModeProbe(g_probe.fail);
                    if (!g_probe.fail.empty()) {
                        g_probe.active = false;
                        Finish(token, "pages_fail_" + g_probe.fail);
                        return;
                    }
                    // RunNavProbes ends on the last page; the footer-button stage below
                    // deliberately starts one page earlier, matching the scroll probe.
                    void* prev = kraken::ext::uibooks::GetBookNavButton(false);
                    if (!prev || probes::NotifyButton(box, prev, 1u) != 1
                        || kraken::ext::uibooks::LastCurPage() != total - 2) {
                        g_probe.active = false;
                        Finish(token, "pages_prev_to_penultimate_failed");
                        return;
                    }
                    g_probe.stage = 3;
                } else {
                    g_probe.stage = 1;
                }
            }

            if (g_probe.stage == 1) {
                if (!probes::RunBandSyncProbe(g_probe.fail)) {
                    g_probe.active = false;
                    Finish(token, g_probe.fail);
                    return;
                }
                g_probe.stage = 2;
            }

            if (g_probe.stage == 2) {
                probes::RunScrollProbes(box, cw, ch, g_probe.fail);
                if (!g_probe.fail.empty()) {
                    g_probe.active = false;
                    Finish(token, "nav_fail_" + g_probe.fail);
                    return;
                }
                g_probe.stage = 3;
            }

// Stages 3: the real footer < / > buttons, driven programmatically through
            // the box's OnWndNotify (the engine's notify chain, no mouse). The scroll
            // stage leaves the box on page total-2 (one short of the last after the
            // wheel up-cross), so the next button must land at the last page and the
            // prev button must land back at total-2.
            if (g_probe.stage == 3) {
                int32_t cur = kraken::ext::uibooks::LastCurPage();
                if (total < 3) {
                    g_probe.active = false;
                    Finish(token, std::string("nav_btn_start_total_") + std::to_string(total));
                    return;
                }
                if (token == "openpages" && cur == total - 1) {
                    void* pagesPrev = kraken::ext::uibooks::GetBookNavButton(false);
                    if (!pagesPrev || probes::NotifyButton(box, pagesPrev, 1u) != 1) {
                        g_probe.active = false;
                        Finish(token, "pages_nav_prev_missing");
                        return;
                    }
                    cur = kraken::ext::uibooks::LastCurPage();
                }
                if (cur != total - 2) {
                    // a SetText re-issue may have reset the reading position mid-probe:
                    // walk forward with the next arrow until the expected page.
                    void* recoveryNext = kraken::ext::uibooks::GetBookNavButton(true);
                    if (!recoveryNext) {
                        g_probe.active = false;
                        Finish(token, "nav_btn_missing_recovery_next");
                        return;
                    }
                    for (int32_t i = 0; i < total + 2 && cur != total - 2; ++i) {
                        (void) probes::NotifyButton(box, recoveryNext, 1u);
                        cur = kraken::ext::uibooks::LastCurPage();
                    }
                    if (cur != total - 2) {
                        g_probe.active = false;
                        Finish(token, std::string("nav_btn_start_cur_") + std::to_string(cur)
                                 + "_total_" + std::to_string(total));
                        return;
                    }
                }
                void* next = kraken::ext::uibooks::GetBookNavButton(true);
                if (!next) {
                    g_probe.active = false;
                    Finish(token, "nav_btn_missing_next");
                    return;
                }
                const int32_t ret = probes::NotifyButton(box, next, 1u);
                const int32_t after = kraken::ext::uibooks::LastCurPage();
                LOG_INFO("open_books: nav next button: notify (ret=%d) cur %d -> %d (expected %d, pages=%d)",
                         ret, cur, after, total - 1, total);
                if (ret != 1 || after != total - 1) {
                    g_probe.active = false;
                    Finish(token, "nav_btn_next_cur_" + std::to_string(after) + "_expected_" + std::to_string(total - 1));
                    return;
                }
                void* prev = kraken::ext::uibooks::GetBookNavButton(false);
                if (!prev) {
                    g_probe.active = false;
                    Finish(token, "nav_btn_missing_prev");
                    return;
                }
                const int32_t ret2 = probes::NotifyButton(box, prev, 1u);
                const int32_t after2 = kraken::ext::uibooks::LastCurPage();
                LOG_INFO("open_books: nav prev button: notify (ret=%d) cur %d -> %d (expected %d, pages=%d)",
                         ret2, after, after2, total - 2, total);
                if (ret2 != 1 || after2 != total - 2) {
                    g_probe.active = false;
                    Finish(token, "nav_btn_prev_cur_" + std::to_string(after2) + "_expected_" + std::to_string(total - 2));
                    return;
                }
                LOG_INFO("open_books: both footer buttons verified programmatically via the engine notify chain (cur=%d)", after2);
                g_probe.stage = 6;
            }

            if (g_probe.stage == 6) {
                const int32_t styled = kraken::ext::uibooks::LastStyleFontCount();
                if (styled < 2) {
                    Finish(token, "style_fonts_" + std::to_string(styled));
                    return;
                }
                LOG_INFO("open_books: verified %d parsed lines, %d pages, %d style fonts, per-line/page alignment, "
                         "book mode property/#page directives, inline color samples, gray text color, 9 footer-nav interaction probes, "
                         "%s",
                         lines, total, styled,
                         token == "openpages" ? "pages" : "scroll",
                         token == "openpages" ? "no in-page scroll" : "engine scroll-band sync and in-page wheel scroll");
                Finish(token, "ok");
            }
        };

        // The retry budget is gone: name the stage that never became ready.
        void FinishBudgetExhausted(const std::string& token) {
            const int32_t lines = kraken::ext::uibooks::LastParseLines();
            const int32_t total = kraken::ext::uibooks::LastPageTotal();
            const int32_t styled = kraken::ext::uibooks::LastStyleFontCount();
            const int32_t expected = detail::CountExpectedLines(detail::BOOK_TEXT);
            LOG_ERROR("open_books: sequence incomplete after %u ms (parsed lines=%d, pages=%d, style fonts=%d)",
                      TRIGGER_RETRY_BUDGET_MS, lines, total, styled);
            g_probe.active = false;
            if (lines <= 0)
                Finish(token, "settext_not_run");
            else if (lines != expected)
                Finish(token, "settext_lines_" + std::to_string(lines) + "_expected_" + std::to_string(expected));
            else if (total < 2)
                Finish(token, "layout_pages_" + std::to_string(total));
            else
                Finish(token, "style_fonts_" + std::to_string(styled));
        };

        // Apply god mode to the player vehicle as soon as a world session starts
        // (mode -> GS_GAME), independent of the books trigger: the player dies to
        // enemies during the UI build otherwise. One session = the first GS_GAME tick
        // after a non-GS_GAME one; retries at a 1 s cadence while the player vehicle
        // script object is not (yet) reachable.
        void CheckGodMode() {
            const int32_t mode = detail::CurGameMode();
            if (mode != detail::GAME_MODE_GAME) {
                g_prevMode = mode;
                return;
            }
            const uint64_t nowMs = GetTickCount64();
            if (g_prevMode != detail::GAME_MODE_GAME) {
                g_godDone = false;
                g_godSessionStartMs = nowMs;
                g_godLastAttemptMs = 0;
                g_godLastError = 0;
                LOG_INFO("open_books: world session started (mode %d -> 0)", g_prevMode);
            }
            g_prevMode = mode;
            if (g_godDone)
                return;
            if (nowMs - g_godLastAttemptMs < GOD_RETRY_INTERVAL_MS)
                return;
            g_godLastAttemptMs = nowMs;
            const int32_t err = detail::ApplyGodMode();
            if (err == 0) {
                g_godDone = true;
                LOG_INFO("open_books: god mode applied (GetPlayerVehicle():setGodMode(1))");
            } else {
                g_godLastError = err;
                if (nowMs - g_godSessionStartMs >= GOD_GIVEUP_AFTER_MS)
                    LOG_WARNING("open_books: god mode not applied within %u ms (eScriptError %d, -1 = script server unreachable)",
                                (uint32_t) GOD_GIVEUP_AFTER_MS, g_godLastError);
            }
        };

        void CheckTriggerFile() {
            const std::string token = ReadFirstLine((std::string(detail::DROPBOX_DIR) + "/trigger.txt").c_str());
            if (token.empty() || token == g_lastTriggerToken)
                return;

            // The books UI (MotherPanel / JournalWnd) only exists once the level is loaded
            // and in-game. Defer the request - the token is retried on later frames until
            // the game reaches GS_GAME.
            const int32_t mode = detail::CurGameMode();
            if (mode != detail::GAME_MODE_GAME) {
                if (token != g_triggerDeferredToken) {
                    g_triggerDeferredToken = token;
                    LOG_WARNING("open_books: deferring trigger '%s' until GS_GAME (current mode %d)",
                                token.c_str(), mode);
                }
                return;
            }

            // The in-game UI is built asynchronously right after the load completes; if the
            // windows are not (fully) there yet, retry the whole sequence on the next tick.
            // The journal-open call is sent exactly once per token (the dispatch is a
            // TOGGLE), and the book injection + selection notification fire exactly once
            // per token (one-shot SetText hook); the later retries only re-check the
            // windows and wait for the painted layout + run the footer-nav probes.
            const uint64_t nowMs = GetTickCount64();
            if (g_triggerActiveToken != token) {
                g_triggerActiveToken = token;
                g_triggerDeadlineMs = 0;
                g_triggerRetries = 0;
            }
            if (g_triggerDeadlineMs == 0)
                g_triggerDeadlineMs = nowMs + TRIGGER_RETRY_BUDGET_MS;
            if (nowMs >= g_triggerDeadlineMs) {
                FinishBudgetExhausted(token);
                g_triggerDeadlineMs = 0;
                g_triggerRetries = 0;
                return;
            }
            ++g_triggerRetries;
            const bool verbose = (g_triggerRetries == 1 || g_triggerRetries % 5 == 0);

            if (g_probe.active && g_probe.token == token) {
                // Book injected + notify sent on an earlier tick: wait for the first
                // painted layout, then run the footer-nav probes.
                try {
                    CheckBookLayout(token, verbose);
                }
                catch (const std::exception& e) {
                    LOG_ERROR("open_books: layout check threw: %s", e.what());
                    g_probe.active = false;
                    Finish(token, "exception");
                    return;
                }
                if (token == g_lastTriggerToken)
                    return; // terminal status (or ok) was written
                if (verbose)
                    LOG_WARNING("open_books: painted layout not ready yet (attempt %d, ~%u ms left) - retrying",
                                g_triggerRetries, (uint32_t)(g_triggerDeadlineMs - GetTickCount64()));
                return;
            }

            try {
                const OpenResult result = TryOpenBooks(token, verbose);
                if (result == OpenResult::NotReady && verbose)
                    LOG_WARNING("open_books: UI not ready yet (attempt %d, ~%u ms left) - retrying on the next tick",
                                g_triggerRetries, (uint32_t)(g_triggerDeadlineMs - GetTickCount64()));
                // LayoutPending: probe armed, the layout check picks up on later ticks.
                // Done: a terminal status was written by TryOpenBooks.
            }
            catch (const std::exception& e) {
                LOG_ERROR("open_books: sequence threw: %s", e.what());
                Finish(token, "exception");
                return;
            }
        };
    }

    // Single pointer parameter -> __fastcall and __thiscall place it identically (ecx).
    static void __fastcall HookProcessAllEvents(void* self) {
        static_cast<hta::m3d::Application*>(self)->Application::ProcessAllEvents();
        CheckSaveLoadFile(self);
        CheckGodMode();
        CheckTriggerFile();
    };

    void Apply(const Config* config) {
        if (!config->uibookstest_enabled.value) {
            LOG_INFO("Test module disabled (enabled=0)");
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(detail::DROPBOX_DIR, ec);

        try {
            routines::ChangeCall(reinterpret_cast<void*>(detail::CALL_SITE_PROCESS_EVENTS), &HookProcessAllEvents);
        }
        catch (const std::exception& e) {
            LOG_ERROR("ChangeCall(ProcessAllEvents) failed: %s", e.what());
            return;
        }

        LOG_INFO("ProcessAllEvents hook installed (drop-box: %s, load_save.txt + trigger.txt)", detail::DROPBOX_DIR);
    };
};
