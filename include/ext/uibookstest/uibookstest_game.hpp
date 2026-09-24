#pragma once

#include "hta/CStr.hpp"
#include "hta/JournalWnd.hpp"
#include "hta/MotherPanel.hpp"

namespace kraken::ext::uibookstest::game {
    int32_t JournalSetCurTab(hta::JournalWnd* journal, hta::JournalWnd::Tab tab,
                             bool postMessage);
    int32_t JournalAddBook(hta::JournalWnd* journal, const hta::CStr& nameId,
                           const hta::CStr& textId);
    hta::m3d::Class* JournalClassObject();
    // MotherPanel::OnTown (VA 0x45F980, __thiscall, no stack args): builds the town
    // panels (incl. the trade WareWnd / goods WareList) from the player's current town.
    void MotherOnTown(hta::MotherPanel* mother);
    // MotherPanel::SetCurTab (VA 0x460AE0): select a panel tab. tab 3 = the goods shop
    // (OnShop), whose WareList is the scroll target. updatePanels=true rebuilds it.
    void MotherSetCurTab(hta::MotherPanel* mother, int32_t tab, bool updatePanels);
    // MotherPanel::InTown (VA 0x460D10, __thiscall, bool): true iff the player is inside
    // a town. The same check SetCurTab(3) uses to pick OnShop (goods) over OnInventory, so
    // the test driver gates the goods-tab switch on this - it fires only after the truck has
    // driven into the city (after the 0->1->0 arrival transition), never on the road.
    bool MotherInTown(hta::MotherPanel* mother);
    // ai::Obj::GetId (VA 0x43BC10, __thiscall): returns the object's registry objId
    // (m_objId at +0x34) - the same id space TownDlg::GetTown() keys on via m_townId.
    int32_t GetObjId(void* obj);
    // help::GetShopForTown (VA 0x454C20, __fastcall ecx=town): the town's shop
    // ai::Workshop*, or null when the town has no loaded shop (far/unstreamed).
    void* GetShopForTown(void* town);
    // Count a workshop's articles via ai::Workshop::_GetArticles (VA 0x2B8A10). 0 if null.
    // Used by the test town-picker to find the goods-heavy city (the 10-15-goods setup).
    int32_t CountWorkshopArticles(void* workshop);
    // m3d::Object::IsKindOf (VA 0x6161e0, NON-virtual __thiscall). The header declares
    // GetClass() virtual, so a C++ obj->IsKindOf() dispatches GetClass through the
    // header's vtable layout, which does not match the binary's real slot order and can
    // jump to a garbage slot on a stale-registry object (in-scan crash). This thunks the
    // game's own non-virtual function, which uses the correct GetClass slot (vft+0x34)
    // internally. Caller must still guard obj (vtable in .rdata, GetClass slot in .text).
    bool ObjIsKindOf(void* obj, void* classObject);
}
