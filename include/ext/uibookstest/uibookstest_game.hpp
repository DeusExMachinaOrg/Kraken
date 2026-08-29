#pragma once

#include "hta/CStr.hpp"
#include "hta/JournalWnd.hpp"

namespace kraken::ext::uibookstest::game {
    int32_t JournalSetCurTab(hta::JournalWnd* journal, hta::JournalWnd::Tab tab,
                             bool postMessage);
    int32_t JournalAddBook(hta::JournalWnd* journal, const hta::CStr& nameId,
                           const hta::CStr& textId);
    hta::m3d::Class* JournalClassObject();
}
