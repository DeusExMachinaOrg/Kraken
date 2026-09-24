#include "ext/uibookstest/uibookstest_game.hpp"

namespace kraken::ext::uibookstest::game {
    int32_t JournalSetCurTab(hta::JournalWnd* journal, hta::JournalWnd::Tab tab,
                             bool postMessage) {
        int32_t result = 0;
        __asm {
            pushad
            movzx eax, postMessage
            push eax
            mov eax, tab
            push eax
            mov eax, journal
            mov edx, 0x004E22D0
            call edx
            mov result, eax
            popad
        }
        return result;
    }

    int32_t JournalAddBook(hta::JournalWnd* journal, const hta::CStr& nameId,
                           const hta::CStr& textId) {
        int32_t result = 0;
        __asm {
            pushad
            mov eax, textId
            push eax
            mov eax, nameId
            push eax
            mov esi, journal
            mov edx, 0x004E2950
            call edx
            mov result, eax
            popad
        }
        return result;
    }

    hta::m3d::Class* JournalClassObject() {
        return reinterpret_cast<hta::m3d::Class*>(0x00A0844C);
    }
}
