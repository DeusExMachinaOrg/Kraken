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

    void MotherOnTown(hta::MotherPanel* mother) {
        // MotherPanel::OnTown reads its object pointer from EAX (prologue: `mov edi, eax`;
        // the object is used as [edi+0x114] and as the receiver of ClearPanels/ShowPanels),
        // not the usual __thiscall ECX. Passing it in ECX fed a garbage object that bailed
        // at the first guard before ShowPanels ran.
        __asm {
            pushad
            mov eax, mother
            mov edx, 0x0045F980
            call edx
            popad
        }
    }

    void MotherSetCurTab(hta::MotherPanel* mother, int32_t tab, bool updatePanels) {
        // MotherPanel::SetCurTab (VA 0x460AE0, __thiscall, ret 4): this=ECX, tab=EAX,
        // bUpdatePanels=stack. Writes [this+0x274]=tab then dispatches on it (jump table at
        // 0x460C08): tab 0=QuestLog, 1=Map, 2=Journal, 3=Shop(goods) [InTown->GetBuildingForTab
        // (3)->OnShop], 4=Workshop, 5=Bar, 6=AdditionalBuilding. Entering a town via the horn
        // opens the panel on a default tab, so we explicitly select tab 3 to make the goods
        // WareList paint. The panel is already modal at this point (the horn's enter-town
        // entered it), so OnShop builds the shop panel within it - no nested modal.
        if (!mother)
            return;
        __asm {
            pushad
            movzx eax, updatePanels
            push eax
            mov eax, tab
            mov ecx, mother
            mov edx, 0x00460AE0
            call edx
            popad
        }
    }

    bool MotherInTown(hta::MotherPanel* mother) {
        // MotherPanel::InTown (VA 0x460D10, __thiscall, ret, bool in EAX). The body ignores
        // `this` entirely (it reads a global [0xa0a55c]->+0x8b4ec then vcalls +0xdc), so the
        // query is a pure "am I inside a town" check - but the calling convention is
        // __thiscall, so we still pass this in ECX. This is the exact gate SetCurTab(3) uses
        // to dispatch OnShop (goods) vs OnInventory, so gating the tab switch on it
        // guarantees the goods tab opens instead of the player's inventory.
        if (!mother)
            return false;
        int32_t result = 0;
        __asm {
            mov ecx, mother
            mov edx, 0x00460D10
            call edx
            mov result, eax
        }
        return result != 0;
    }

    int32_t GetObjId(void* obj) {
        // ai::Obj::GetId (VA 0x403BC10, __thiscall): `mov eax, [ecx+0x34]; ret`.
        int32_t result = -1;
        if (!obj)
            return -1;
        __asm {
            mov ecx, obj
            mov edx, 0x0043BC10
            call edx
            mov result, eax
        }
        return result;
    }

    void* GetShopForTown(void* town) {
        // help::GetShopForTown (VA 0x454C20, __fastcall ecx=town): the town's shop
        // ai::Workshop* (its building of type 2 that IsKindOf class 0xa0203c). Null when the
        // town has no loaded shop - e.g. a far/unstreamed town, whose GetBuildingByType
        // returns nothing. Safe to call on every ai::Town in the registry scan.
        if (!town)
            return nullptr;
        void* result = nullptr;
        __asm {
            pushad
            mov ecx, town
            mov edx, 0x00454C20
            call edx
            mov result, eax
            popad
        }
        return result;
    }

    int32_t CountWorkshopArticles(void* workshop) {
        // ai::Workshop::_GetArticles (VA 0x2B8A10, __thiscall, ret 4): fills a temp
        // std::vector<ai::Article> with the workshop's articles. Count = (end-begin)/sizeof.
        // sizeof(ai::Article)=68 per the PDB; even if it were off, a constant divisor keeps
        // the per-town RANKING correct, which is all the picker needs. The reserved buffer is
        // not freed (tiny one-shot leak in a test helper, acceptable).
        if (!workshop)
            return 0;
        uintptr_t vec[3] = {0, 0, 0};  // std::vector<ai::Article>: begin, end, capacity
        int32_t result = -1;
        __asm {
            mov eax, workshop
            mov ecx, eax
            lea edx, vec
            push edx
            mov edx, 0x002B8A10
            call edx
            ; _GetArticles is __thiscall with `ret 4`: it cleans the 4-byte argument
            ; itself. Adding esp, 4 here too would double-pop and corrupt the stack
            ; (observed: hard fault mid town-scan on the first per-town call).
            mov eax, [vec + 4]
            sub eax, [vec]
            xor edx, edx
            mov ecx, 68
            div ecx
            mov result, eax
        }
        return result;
    }

    bool ObjIsKindOf(void* obj, void* classObject) {
        // m3d::Object::IsKindOf (VA 0x6161e0, NON-virtual __thiscall):
        //   mov edx, [ecx]; push classObject; call [edx+0x34]  ; GetClass()
        //   call m3d::Class::IsKindOf; ret 4
        // Called directly so it uses the binary's real GetClass slot (vft+0x34) instead of
        // the header's vtable-layout dispatch. pushad/popad keeps the caller's ECX (this)
        // intact for the __thiscall contract and isolates our register use.
        int32_t result = 0;
        if (!obj || !classObject)
            return false;
        __asm {
            pushad
            mov eax, classObject
            push eax
            mov ecx, obj
            mov edx, 0x006161E0
            call edx
            mov result, eax
            popad
        }
        return result != 0;
    }
}
