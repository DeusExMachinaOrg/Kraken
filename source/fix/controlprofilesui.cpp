#define LOGGER "profilesui"

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "fix/controlprofilesui.hpp"
#include "fix/inputprofiles.hpp"
#include "config.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

#include "hta/CStr.hpp"

// Unified control page + a 5th "Mouse" tab.
//
// The stock Control tab (ControlOptionsWnd) is augmented in place with a
// profile/device header (built lazily). All the mouse-sensitivity / invert
// controls stay native children of ControlOptionsWnd (their engine handlers are
// untouched); we merely toggle a "bindings view" vs "mouse view" by flipping
// widget visibility (Wnd::ShowWindow). A revived 5th tab button "Mouse" (id
// 10006, converted to a real OptionTabButton for the tab look) switches into the
// mouse view; clicking any stock tab returns to the bindings view. See
// Kraken/docs/control-profiles.md §6/§7.
namespace kraken::fix::controlprofilesui {
    namespace {
        // --- engine entry points (VAs, base 0x400000) ---
        using LoadDialog_t = void*(__fastcall*)(void* cstr);
        const LoadDialog_t LoadDialog = reinterpret_cast<LoadDialog_t>(0x006744F0);
        using GetChildByName_t = void*(__thiscall*)(void* self, void* cstr);
        const GetChildByName_t GetChildByName = reinterpret_cast<GetChildByName_t>(0x00435700);
        using ChildOp_t = int(__thiscall*)(void* self, void* child);
        const ChildOp_t AddChild = reinterpret_cast<ChildOp_t>(0x006121D0);
        using MoveChild_t = int(__thiscall*)(void* self, void* child);
        const MoveChild_t MoveChildToFirstPosition = reinterpret_cast<MoveChild_t>(0x00405340);
        // Object::MoveChildToLastPosition @0x467ef0 — makes the child the LAST
        // sibling (drawn on TOP + hit-tested first). Unlike MoveChildToFirstPosition
        // (a normal thiscall), the PDB LIES: it is __usercall with this (parent) in
        // EDI and wnd (child) in ESI, ret 0 (no stack args). Call via a naked thunk.
        const void* MCTL_ADDR = reinterpret_cast<void*>(0x00467EF0);
        __declspec(naked) void MoveChildToLast(void* /*parent*/, void* /*child*/) {
            __asm {
                push esi
                push edi
                mov  edi, [esp + 12]        ; parent (this)
                mov  esi, [esp + 16]        ; child (wnd)
                call dword ptr [MCTL_ADDR]
                pop  edi
                pop  esi
                ret
            }
        }
        // ComboBoxWnd open state: [combo+0x230] == 1 while the dropdown list is up
        // (ComboBoxWnd::IsOpen @0x71efc0).
        constexpr unsigned CB_OPEN_FLAG = 0x230;
        using SetText_t = int(__thiscall*)(void* self, void* cstr);
        const SetText_t SetText = reinterpret_cast<SetText_t>(0x0041CC20);
        using SetPane_t = void(__thiscall*)(void* self, void* cstr);
        const SetPane_t SetPane = reinterpret_cast<SetPane_t>(0x0041CCA0);
        using GetBounds_t = void(__thiscall*)(void* self, float* out4);
        const GetBounds_t GetBounds = reinterpret_cast<GetBounds_t>(0x0041CBD0);
        // Wnd::ShowWindow(bool) @0x41c2e0 — sets/clears the visibility bit
        // ([this+0x89] & 2); ret 4, thiscall (this in ecx, bool on stack).
        using ShowWnd_t = void(__thiscall*)(void* self, int show);
        const ShowWnd_t ShowWnd = reinterpret_cast<ShowWnd_t>(0x0041C2E0);
        // m3d::ui::ComboBoxWnd API (all virtual thiscall). On selection the combo
        // sends msg 5 to its parent via CallParentNotify (see SelectItem).
        using CbAdd_t = int(__thiscall*)(void* self, void* cstr);
        const CbAdd_t CbAddItem = reinterpret_cast<CbAdd_t>(0x00720900);
        using CbVoid_t = int(__thiscall*)(void* self);
        const CbVoid_t CbRemoveAll = reinterpret_cast<CbVoid_t>(0x00720500);
        using CbGetSel_t = int(__thiscall*)(void* self);
        const CbGetSel_t CbGetCurSel = reinterpret_cast<CbGetSel_t>(0x0071FB10);
        using CbSetSel_t = void(__thiscall*)(void* self, int idx);
        const CbSetSel_t CbSetCurSel = reinterpret_cast<CbSetSel_t>(0x0071F090);
        using CbSetData_t = void(__thiscall*)(void* self, int idx, int data);
        const CbSetData_t CbSetItemData = reinterpret_cast<CbSetData_t>(0x00720430);
        using CbGetData_t = int(__thiscall*)(void* self, int idx);
        const CbGetData_t CbGetItemData = reinterpret_cast<CbGetData_t>(0x007203F0);

        // --- modal name-entry dialog plumbing ---
        // WndStation::DoModal(modal) runs an *exclusive* nested message loop
        // (StartExclusiveMsgLoop) and only returns once the modal is closed; the
        // return value is the CloseModal() code. The station is a global singleton
        // (Wnd::GetStation just reads [0xA0A2D4]), so the modal needs no parenting.
        using DoModal_t = int(__thiscall*)(void* station, void* modal);
        const DoModal_t WndStationDoModal = reinterpret_cast<DoModal_t>(0x00591A30);
        void** const STATION_PTR = reinterpret_cast<void**>(0x00A0A2D4);
        // ModalWnd::CloseModal(val) — enqueues the close; the exclusive loop then
        // unwinds and DoModal returns `val`.
        using CloseModal_t = int(__thiscall*)(void* modal, int val);
        const CloseModal_t CloseModal = reinterpret_cast<CloseModal_t>(0x00678360);
        // EditWnd text is accessed through its own vtable overrides (the generic
        // Wnd::SetText VA does not update the edit buffer/cursor): GetText = slot
        // 0x4c, SetText = slot 0x48 (confirmed in NewProfileWnd::CreateNewProfile /
        // OnBeforeAddToWndStation). Both take a CStr* (get fills it, set reads it).
        using WndTextFn_t = void(__thiscall*)(void* self, hta::CStr* text);
        constexpr unsigned VT_GETTEXT = 0x4c;
        constexpr unsigned VT_SETTEXT = 0x48;

        void EditGetText(void* edit, hta::CStr* out) {
            if (!edit) return;
            void** vtbl = *reinterpret_cast<void***>(edit);
            reinterpret_cast<WndTextFn_t>(vtbl[VT_GETTEXT / 4])(edit, out);
        }
        void EditSetText(void* edit, hta::CStr* text) {
            if (!edit) return;
            void** vtbl = *reinterpret_cast<void***>(edit);
            reinterpret_cast<WndTextFn_t>(vtbl[VT_SETTEXT / 4])(edit, text);
        }
        // OptionTabButton factory + pattern-init (gives the bordered tab look).
        using OtbCreate_t = void*(__fastcall*)();
        const OtbCreate_t OtbCreateObject = reinterpret_cast<OtbCreate_t>(0x004C3A40);
        constexpr uintptr_t OTB_VTABLE = 0x009D44E0; // OptionTabButton vtable
        const void* CFP_ADDR = reinterpret_cast<void*>(0x004C3BE0);

        // OptionTabButton::CreateFromPattern is __usercall: ESI=this (new otb),
        // EDI=patternWnd (placeholder), bool deleteSrc as the single stack arg,
        // ret 4. See the crash notes in the memory file.
        __declspec(naked) void OtbCreateFromPattern(void* /*otb*/, void* /*patternWnd*/, bool /*deleteSrc*/) {
            __asm {
                push esi
                push edi
                mov esi, [esp + 12]            ; otb (this)
                mov edi, [esp + 16]            ; patternWnd (placeholder)
                movzx eax, byte ptr [esp + 20] ; deleteSrc
                push eax
                call dword ptr [CFP_ADDR]      ; ret 4 cleans the pushed bool
                pop edi
                pop esi
                ret
            }
        }

        // OptionsWnd::SetCurTab @0x4c36e0 is __usercall: this in ECX, tabId in EAX,
        // ret 0. Used to make ControlOptionsWnd (tab 2) the shown page when the
        // Mouse tab is picked from another tab (engine applies the outgoing tab's
        // changes + tracks m_curTabId/m_lastTabId for us).
        const void* SETCURTAB_ADDR = reinterpret_cast<void*>(0x004C36E0);
        __declspec(naked) void CallSetCurTab(void* /*self*/, int /*tabId*/) {
            __asm {
                mov ecx, [esp + 4]            ; self
                mov eax, [esp + 8]            ; tabId
                call dword ptr [SETCURTAB_ADDR]
                ret
            }
        }

        // OptionsWnd fields (vector data ptr sits at field+4).
        constexpr unsigned OPT_CURTABID = 0x224; // m_curTabId
        constexpr unsigned OPT_TABBTNS  = 0x230; // m_tabButtons: begin ptr (OptionTabButton*[4])
        constexpr unsigned OPT_OPTWINS  = 0x240; // m_optionWindows: begin ptr (Wnd*[])
        constexpr unsigned BTN_TABID    = 0x240; // OptionTabButton::m_tabId
        constexpr unsigned BTN_SELFLAG  = 0x23c; // OptionTabButton selected flag
        constexpr int      TAB_CONTROL  = 2;

        // Widget ids.
        constexpr unsigned ID_TAB_MOUSE     = 10006;
        constexpr unsigned ID_STOCK_TAB     = 0x2711; // 10001
        constexpr unsigned ID_STOCK_EXIT    = 0x2710; // 10000
        constexpr unsigned ID_PROFILE_NEW    = 12003;
        constexpr unsigned ID_PROFILE_DELETE = 12004;
        constexpr unsigned ID_CB_PROFILE     = 12010;
        constexpr unsigned ID_CB_DEVICE      = 12011;
        // Name-entry modal (ControlProfileNameWnd.xml): OK=12001 (handled by our
        // ModalWnd::OnWndNotify detour), edit field=12002, Cancel=id 3 (auto-closed
        // by the ModalWnd base).
        constexpr unsigned ID_NAME_OK        = 12001;
        constexpr unsigned ID_NAME_EDIT      = 12002;
        // Stock BindKeysWnd (bindings editor) button ids (bindkeysdlg.xml):
        // Cancel=700000, Apply=700001, Default=700002.
        constexpr unsigned ID_BK_APPLY       = 700001;
        constexpr unsigned ID_BK_DEFAULT     = 700002;
        constexpr unsigned NOTIFY_CLICK      = 1;
        constexpr unsigned NOTIFY_COMBOSEL   = 5; // ComboBoxWnd -> parent on select
        // m_style bit that makes Wnd::OnWndNotify reflect child notifies to the
        // parent (so the header's combo/button notifies reach ControlOptionsWnd).
        constexpr unsigned WND_REFLECT_NOTIFY = 0x00100000;

        // Russian UI text in windows-1251 (set from code; see the encoding note in
        // the memory file — literal Cyrillic in the UTF-8-saved XML is mis-decoded).
        const char* const RU_PROFILE = "\xCF\xF0\xEE\xF4\xE8\xEB\xFC:";             // Профиль:
        const char* const RU_DEVICE  = "\xD3\xF1\xF2\xF0\xEE\xE9\xF1\xF2\xE2\xEE:"; // Устройство:
        const char* const RU_NEW     = "\xCD\xEE\xE2\xFB\xE9";                      // Новый
        const char* const RU_DELETE  = "\xD3\xE4\xE0\xEB\xE8\xF2\xFC";              // Удалить
        const char* const RU_MOUSE   = "\xCC\xFB\xF8\xFC";                          // Мышь
        const char* const RU_NAME_TITLE = "\xC8\xEC\xFF \xEF\xF0\xEE\xF4\xE8\xEB\xFF"; // Имя профиля
        const char* const RU_OK      = "\xCE\xCA";                                  // ОК
        const char* const RU_CANCEL  = "\xCE\xF2\xEC\xE5\xED\xE0";                  // Отмена

        // --- state (single Options menu, single thread) ---
        void* g_cbProfile      = nullptr; // profile dropdown (ComboBoxWnd)
        void* g_cbDevice       = nullptr; // device dropdown (ComboBoxWnd)
        std::vector<std::string> g_profileNames; // parallel to cbProfile items
        std::vector<uint32_t>    g_deviceIds;    // parallel to cbDevice items
        void* g_header         = nullptr; // wndProfileHeader
        void* g_bindKeys       = nullptr; // dlgBindKey / BindKeysWnd
        int   g_viewApplied    = 0;       // 0 = bindings on-screen, 1 = mouse view
        float g_mouseUp        = 257.0f;  // px to lift the mouse block up in mouse view
        bool  g_mouseView      = false;
        bool  g_populating     = false;   // suppress combo select-notify while filling
        // Name-entry modal: our ModalWnd::OnWndNotify detour is shared by every
        // modal in the game, so it acts only while g_nameModalActive AND the sender
        // is g_nameModal. On OK it stashes the typed name here for NewProfile() to
        // consume after DoModal returns.
        void* g_nameModal      = nullptr;
        bool  g_nameModalActive = false;
        bool  g_nameAccepted   = false;
        std::string g_nameResult;

        // Origin y of a Wnd (m_bounds.y). Children render relative to the parent's
        // origin (see Wnd::ToScreen), so shifting a container's y moves its whole
        // subtree — the non-destructive way to hide/reposition without detaching.
        void MoveWndY(void* wnd, float dy) {
            if (wnd) *reinterpret_cast<float*>(static_cast<char*>(wnd) + 0x90) += dy;
        }

        void* FindChild(void* parent, const char* name) {
            if (!parent) return nullptr;
            hta::CStr n = name;
            return GetChildByName(parent, &n);
        }
        void SetWndText(void* wnd, const char* text1251) {
            if (!wnd) return;
            hta::CStr s = text1251;
            SetText(wnd, &s);
        }
        void SetWndPane(void* wnd, const char* pane) {
            if (!wnd) return;
            hta::CStr p = pane;
            SetPane(wnd, &p);
        }
        void ShowWndSafe(void* wnd, bool show) {
            if (wnd) ShowWnd(wnd, show ? 1 : 0);
        }
        int& OptCurTab(void* opt) {
            return *reinterpret_cast<int*>(static_cast<char*>(opt) + OPT_CURTABID);
        }
        void* StockPage(void* opt, int tab) {
            if (tab < 0 || tab > 3) return nullptr;
            void** begin = *reinterpret_cast<void***>(static_cast<char*>(opt) + OPT_OPTWINS);
            return begin ? begin[tab] : nullptr;
        }
        bool IsTabButton(void* w) {
            return w && *reinterpret_cast<uintptr_t*>(w) == OTB_VTABLE;
        }
        // Select/deselect a tab button. Set the selected flag (+0x23c, read by
        // IsSelected) AND the BASE pane via Wnd::SetPane — the engine's own
        // SelectTabButton only sets the *display* pane (vtbl+0x3c), which a later
        // mouse-out repaint reverts to the XML base pane; setting the base pane too
        // makes the selected look stick when the cursor leaves the button.
        void SelectTab(void* btn, bool sel) {
            if (!IsTabButton(btn)) return;
            *(reinterpret_cast<uint8_t*>(btn) + BTN_SELFLAG) = sel ? 1 : 0;
            SetWndPane(btn, sel ? "PaneBtnOptionsSel" : "PaneBtnOptionsUnsel");
        }
        void DeselectStockTabs(void* opt) {
            void** begin = *reinterpret_cast<void***>(static_cast<char*>(opt) + OPT_TABBTNS);
            if (!begin) return;
            for (int i = 0; i < 4; ++i)
                if (begin[i]) SelectTab(begin[i], false);
        }
        void SelectStockTab(void* opt, int tab) {
            if (tab < 0 || tab > 3) return;
            void** begin = *reinterpret_cast<void***>(static_cast<char*>(opt) + OPT_TABBTNS);
            if (begin && begin[tab]) SelectTab(begin[tab], true);
        }

        const char* PrefabPath(void* wnd) {
            float b[4] = {0, 0, 0, 0};
            GetBounds(wnd, b);
            float maxv = 0.0f;
            for (int i = 0; i < 4; ++i) if (b[i] > maxv) maxv = b[i];
            return (maxv > 1500.0f) ? "data\\if\\dialogs_16_9\\ControlProfileHeaderWnd.xml"
                                    : "data\\if\\dialogs\\ControlProfileHeaderWnd.xml";
        }

        static const char* const kMouseWidgets[] = {
            "sliderMouseSensitivity", "btnMouseSensitivityPrev", "btnMouseSensitivityNext",
            "lblMouseSensitivity", "checkMouseFlipX", "checkMouseFlipY",
            "lblMouseFlipX", "lblMouseFlipY", "emboss_checkMouseFlipX",
            "emboss_checkMouseFlipY", "wndLine2",
        };

        // Toggle bindings-view vs mouse-view on the Control page.
        //  - Mouse widgets are LEAF windows: ShowWindow hides them (it gates a
        //    window's own paint). In mouse view they're also lifted up into the
        //    freed top area (they sit at the page bottom in the stock layout).
        //  - The bindings group (our header + the dlgBindKey list) are CONTAINERS;
        //    ShowWindow does NOT hide a container's children. Detaching them clears
        //    the shared BindKeysWnd list, so instead we move them off-screen (the
        //    whole subtree follows the parent origin — see Wnd::ToScreen).
        // The move/lift is guarded by g_viewApplied so it applies once per switch;
        // visibility is (idempotently) set every call.
        void SetView(void* controlPage, bool mouse) {
            if (!controlPage) return;
            for (const char* n : kMouseWidgets)
                ShowWndSafe(FindChild(controlPage, n), mouse);

            int want = mouse ? 1 : 0;
            if (g_viewApplied == want) return;
            float dy = mouse ? 6000.0f : -6000.0f;   // hide/show the bindings group
            MoveWndY(g_bindKeys, dy);
            MoveWndY(g_header, dy);
            float mdy = mouse ? -g_mouseUp : g_mouseUp; // lift/restore the mouse block
            for (const char* n : kMouseWidgets)
                MoveWndY(FindChild(controlPage, n), mdy);
            g_viewApplied = want;
        }

        void CbAdd(void* combo, const char* text1251) {
            hta::CStr s = text1251;
            CbAddItem(combo, &s);
        }

        // The header is a full-screen container, so it may sit on TOP of the
        // bindings frame ONLY while a combo dropdown is open (otherwise its root
        // would swallow every click meant for the bindings list). Flip its z-order
        // among ControlOptionsWnd's children to match the open state: last = drawn
        // on top + hit-tested first (so the open list is clickable), first = behind.
        void SyncHeaderZOrder() {
            if (!g_header) return;
            void* page = *reinterpret_cast<void**>(static_cast<char*>(g_header) + 0x18);
            if (!page) return;
            auto open = [](void* cb) {
                return cb && *reinterpret_cast<int*>(static_cast<char*>(cb) + CB_OPEN_FLAG) == 1;
            };
            if (open(g_cbProfile) || open(g_cbDevice))
                MoveChildToLast(page, g_header);
            else
                MoveChildToFirstPosition(page, g_header);
        }

        // Fill both dropdowns from the current profile list + connected devices,
        // selecting the active profile and its assigned device. SetCurSel/
        // RemoveAllItems fire the combo's select-notify synchronously, which would
        // re-enter On*Selected -> PopulateCombos -> ... (infinite recursion, the
        // "device set to winmm 1" spin that froze the game). Suppress it while
        // filling; On*Selected early-outs on g_populating.
        void PopulateCombos() {
            g_populating = true;
            std::string active = inputprofiles::Active();

            g_profileNames.clear();
            if (g_cbProfile) {
                CbRemoveAll(g_cbProfile);
                int sel = -1, i = 0;
                for (const auto& p : inputprofiles::List()) {
                    std::string label = p.display.empty() ? p.name : p.display;
                    CbAdd(g_cbProfile, label.c_str());
                    g_profileNames.push_back(p.name);
                    if (p.name == active) sel = i;
                    ++i;
                }
                if (sel >= 0) CbSetCurSel(g_cbProfile, sel);
            }

            g_deviceIds.clear();
            if (g_cbDevice) {
                CbRemoveAll(g_cbDevice);
                uint32_t curDev = Config::Instance().wheel_device.value;
                std::string curName = inputprofiles::DeviceNameFor(inputprofiles::Active());
                auto devs = inputprofiles::Devices();
                int sel = -1, i = 0;
                for (const auto& d : devs) {
                    CbAdd(g_cbDevice, d.name);
                    CbSetItemData(g_cbDevice, i, static_cast<int>(d.id));
                    g_deviceIds.push_back(d.id);
                    if (d.id == curDev) sel = i;
                    ++i;
                }
                LOG_INFO("PopulateCombos device: curDev=%u curName='%s' items=%d sel=%d",
                         curDev, curName.c_str(), i, sel);
                if (sel < 0) {
                    // The profile's device id didn't match a connected device.
                    // Before adding a phantom "[X]" entry, try to reconcile by
                    // NAME: the device is really present but under a different id
                    // (e.g. wheel_device holds a stale winmm id while the native
                    // DualSense now enumerates as id 100). Select that entry and
                    // repair the profile so the id stops drifting.
                    if (!curName.empty()) {
                        for (int j = 0; j < (int)devs.size(); ++j) {
                            if (_stricmp(devs[j].name, curName.c_str()) == 0) {
                                sel = j;
                                LOG_INFO("PopulateCombos: reconciled by name -> "
                                         "id %u (was %u)", devs[j].id, curDev);
                                if (devs[j].id != curDev)
                                    inputprofiles::SetDevice(inputprofiles::Active(),
                                                             devs[j].id);
                                break;
                            }
                        }
                    }
                }
                if (sel < 0) {
                    // Truly not connected — still list it, marked "[X]", using the
                    // name we saved when it was selected (fallback to its id).
                    LOG_INFO("PopulateCombos: [X] fallback nm='%s' curDev=%u",
                             curName.c_str(), curDev);
                    char buf[160];
                    if (!curName.empty())
                        std::snprintf(buf, sizeof(buf), "[X] %s", curName.c_str());
                    else
                        std::snprintf(buf, sizeof(buf), "[X] #%u", curDev);
                    int idx = static_cast<int>(g_deviceIds.size());
                    CbAdd(g_cbDevice, buf);
                    CbSetItemData(g_cbDevice, idx, static_cast<int>(curDev));
                    g_deviceIds.push_back(curDev);
                    sel = idx;
                }
                if (sel >= 0) CbSetCurSel(g_cbDevice, sel);
            }
            g_populating = false;
        }

        void OnProfileSelected() {
            if (g_populating || !g_cbProfile) return;
            int idx = CbGetCurSel(g_cbProfile);
            if (idx < 0 || idx >= static_cast<int>(g_profileNames.size())) return;
            if (g_profileNames[idx] == inputprofiles::Active()) return; // no change
            inputprofiles::Switch(g_profileNames[idx]);
            PopulateCombos(); // device may differ per profile
        }

        void OnDeviceSelected() {
            if (g_populating || !g_cbDevice) return;
            int idx = CbGetCurSel(g_cbDevice);
            if (idx < 0 || idx >= static_cast<int>(g_deviceIds.size())) return;
            if (g_deviceIds[idx] == Config::Instance().wheel_device.value) return; // no change
            std::string active = inputprofiles::Active();
            if (!active.empty())
                inputprofiles::SetDevice(active, g_deviceIds[idx]);
        }

        // Open the native name-entry modal (ControlProfileNameWnd.xml) and block
        // until the user confirms or cancels. `initial` prefills the edit field
        // (for a future rename). Returns true + the typed text on OK, false on
        // Cancel/Esc. The actual text is read by ModalNotify_Hook while the modal
        // is still alive (the engine frees it on close).
        bool PromptProfileName(const char* title1251, const std::string& initial,
                               std::string& out) {
            const char* file = (g_mouseUp > 300.0f)
                ? "data\\if\\dialogs_16_9\\ControlProfileNameWnd.xml"
                : "data\\if\\dialogs\\ControlProfileNameWnd.xml";
            hta::CStr path = file;
            void* modal = LoadDialog(&path);
            if (!modal) {
                LOG_ERROR("LoadDialog(ControlProfileNameWnd) failed");
                return false;
            }
            SetWndText(FindChild(modal, "lblTitle"),   title1251);
            SetWndText(FindChild(modal, "btnOk"),      RU_OK);
            SetWndText(FindChild(modal, "btnCancel"),  RU_CANCEL);
            if (!initial.empty()) {
                hta::CStr s = initial.c_str();
                EditSetText(FindChild(modal, "editName"), &s);
            }
            g_nameModal       = modal;
            g_nameAccepted    = false;
            g_nameResult.clear();
            g_nameModalActive = true;
            WndStationDoModal(*STATION_PTR, modal); // blocks (exclusive msg loop)
            g_nameModalActive = false;
            g_nameModal       = nullptr;
            if (g_nameAccepted) { out = g_nameResult; return true; }
            return false;
        }

        void NewProfile() {
            if (!inputprofiles::Available()) return;
            std::string name;
            if (!PromptProfileName(RU_NAME_TITLE, std::string(), name))
                return; // cancelled
            if (name.empty()) return;
            if (inputprofiles::Create(name, "custom"))
                inputprofiles::Switch(name);
            else
                LOG_WARNING("Could not create profile '%s' (invalid name or already exists)",
                            name.c_str());
            PopulateCombos();
        }

        void DeleteProfile() {
            if (!inputprofiles::Available()) return;
            std::string active = inputprofiles::Active();
            if (!active.empty())
                inputprofiles::Delete(active); // switches to a fallback profile internally
            PopulateCombos();
        }

        // Build the header once, as an extra child of ControlOptionsWnd. Also sets
        // the initial bindings view (hides the stock mouse widgets on the page).
        void BuildHeader(void* controlPage) {
            if (!controlPage || FindChild(controlPage, "wndProfileHeader"))
                return;
            hta::CStr path = PrefabPath(controlPage);
            void* header = LoadDialog(&path);
            if (!header) {
                LOG_ERROR("LoadDialog(ControlProfileHeaderWnd) failed");
                return;
            }
            AddChild(controlPage, header);
            MoveChildToFirstPosition(controlPage, header);
            // Make the header reflect its children's notifies up to
            // ControlOptionsWnd::OnWndNotify (combo selection + New/Delete clicks).
            *reinterpret_cast<uint32_t*>(static_cast<char*>(header) + 0x88) |= WND_REFLECT_NOTIFY;
            g_cbProfile = FindChild(header, "cbProfile");
            g_cbDevice  = FindChild(header, "cbDevice");
            SetWndText(FindChild(header, "lblProfileCaption"), RU_PROFILE);
            SetWndText(FindChild(header, "lblDeviceCaption"),  RU_DEVICE);
            SetWndText(FindChild(header, "btnProfileNew"),     RU_NEW);
            SetWndText(FindChild(header, "btnProfileDelete"),  RU_DELETE);
            PopulateCombos();
            // Cache the movable groups; both are on-screen now, so the view starts
            // in the bindings state. Pick the mouse-block lift for this resolution.
            g_header   = header;
            g_bindKeys = FindChild(controlPage, "dlgBindKey");
            {
                float b[4] = {0, 0, 0, 0};
                GetBounds(controlPage, b);
                float maxv = 0.0f;
                for (int i = 0; i < 4; ++i) if (b[i] > maxv) maxv = b[i];
                g_mouseUp = (maxv > 1500.0f) ? 360.0f : 257.0f;
            }
            g_viewApplied = 0;
            SetView(controlPage, false); // hide the mouse leaf widgets initially
            LOG_INFO("Control-profile header attached to the Control tab");
        }

        // Convert the placeholder ButtonWnd "tabBtnMouse" into a real
        // OptionTabButton (tab border/pane look) and give it the Russian caption.
        // Runs from a GameDataSetup detour so it is correct from the first frame;
        // idempotent (skips if already converted).
        void FixupMouseTab(void* optionsWnd) {
            void* btn = FindChild(optionsWnd, "tabBtnMouse");
            if (!btn || IsTabButton(btn))
                return;
            void* otb = OtbCreateObject();
            if (otb) {
                OtbCreateFromPattern(otb, btn, true);
                SetWndText(otb, RU_MOUSE);
                SetWndPane(otb, "PaneBtnOptionsUnsel");
                LOG_INFO("Mouse tab button converted to OptionTabButton");
            } else {
                SetWndText(btn, RU_MOUSE);
                LOG_WARNING("FixupMouseTab: CreateObject failed, caption only");
            }
        }

        void EnterMouseView(void* opt) {
            CallSetCurTab(opt, TAB_CONTROL);   // ensure ControlOptionsWnd is shown
            void* cp = StockPage(opt, TAB_CONTROL);
            if (!cp) return;
            DeselectStockTabs(opt);            // un-highlight the Control tab
            SelectTab(FindChild(opt, "tabBtnMouse"), true);
            SetView(cp, true);
            g_mouseView = true;
        }

        // Return to bindings view. If the Control tab was the one clicked, the
        // engine's SetCurTab(2) early-outs (no re-highlight), so re-highlight it;
        // for other stock tabs the engine already highlighted the new one.
        void LeaveMouseView(void* opt, bool controlClicked) {
            void* cp = StockPage(opt, TAB_CONTROL);
            SelectTab(FindChild(opt, "tabBtnMouse"), false);
            if (cp) SetView(cp, false);
            if (controlClicked)
                SelectStockTab(opt, TAB_CONTROL);
            g_mouseView = false;
        }

        // --- detour plumbing (raw-byte trampoline) ---
        void* InstallDetour(uintptr_t addr, int prologueLen, void* hook) {
            uint8_t* fn = reinterpret_cast<uint8_t*>(addr);
            uint8_t* tramp = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, prologueLen + 5, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE));
            if (!tramp) {
                LOG_ERROR("Failed to allocate trampoline @ 0x%p", (void*)addr);
                return nullptr;
            }
            std::memcpy(tramp, fn, prologueLen);
            tramp[prologueLen] = 0xE9;
            *reinterpret_cast<int32_t*>(tramp + prologueLen + 1) =
                static_cast<int32_t>((fn + prologueLen) - (tramp + prologueLen + 5));
            DWORD prot;
            VirtualProtect(fn, prologueLen, PAGE_EXECUTE_READWRITE, &prot);
            fn[0] = 0xE9;
            *reinterpret_cast<int32_t*>(fn + 1) =
                static_cast<int32_t>(reinterpret_cast<uint8_t*>(hook) - (fn + 5));
            for (int i = 5; i < prologueLen; ++i) fn[i] = 0x90;
            VirtualProtect(fn, prologueLen, prot, &prot);
            return tramp;
        }

        // OptionsWnd::GameDataSetup @0x4c30a0 (6-byte prologue: sub esp,0xb8).
        using GameDataSetup_t = int(__thiscall*)(void*);
        GameDataSetup_t g_origSetup = nullptr;

        int __fastcall GameDataSetup_Hook(void* self, void* /*edx*/) {
            int r = g_origSetup(self);
            FixupMouseTab(self);
            return r;
        }

        // OptionsWnd::OnWndNotify @0x4c3560 (5-byte prologue: mov eax,[esp+0x10] /
        // push ebx). Lazy header build + Mouse-tab routing.
        using OptOnWndNotify_t = int(__thiscall*)(void*, void*, unsigned, unsigned, void*);
        OptOnWndNotify_t g_origOptNotify = nullptr;

        int __fastcall OptOnWndNotify_Hook(void* self, void* /*edx*/, void* from,
                                           unsigned id, unsigned msg, void* data) {
            int r = g_origOptNotify(self, from, id, msg, data);
            void* cp = StockPage(self, TAB_CONTROL);
            if (cp && !FindChild(cp, "wndProfileHeader"))
                BuildHeader(cp); // (re)build if missing — survives menu recreation
            if (msg == NOTIFY_CLICK) {
                if (id == ID_TAB_MOUSE) {
                    EnterMouseView(self);
                    return 1;
                }
                // Only a real tab click leaves the mouse view. Exit (and Esc) do
                // NOT: g_mouseView stays set so the next open re-enters the mouse
                // view (see OnBeforeAdd_Hook).
                if (g_mouseView && id == ID_STOCK_TAB) {
                    bool controlClicked = from &&
                        *reinterpret_cast<int*>(static_cast<char*>(from) + BTN_TABID) == TAB_CONTROL;
                    LeaveMouseView(self, controlClicked);
                }
            }
            return r;
        }

        // OptionsWnd::OnBeforeAddToWndStation @0x4c38c0 (10-byte prologue:
        // push ebx / push esi / mov esi,ecx / mov eax,[esi+0x228] — no relative
        // branches). Fires when the menu OPENS; the original shows+selects
        // m_lastTabId. If we were in the mouse view when the menu was last closed
        // (g_mouseView sticks — Esc/Exit don't clear it), re-assert the mouse view
        // AFTER the engine's selection so the reopen lands on Mouse with only that
        // tab highlighted (fixes both tabs lighting up on reopen).
        using OnBeforeAdd_t = int(__thiscall*)(void*);
        OnBeforeAdd_t g_origOnBeforeAdd = nullptr;

        int __fastcall OnBeforeAdd_Hook(void* self, void* /*edx*/) {
            int r = g_origOnBeforeAdd(self);
            void* cp = StockPage(self, TAB_CONTROL);
            void* header = cp ? FindChild(cp, "wndProfileHeader") : nullptr;
            if (header) {
                // Re-cache (the menu may have been recreated) and refresh the combos
                // so controllers connected/disconnected since the menu was last open
                // are reflected. If the header isn't built yet it is created lazily
                // later (which also populates the combos).
                g_header    = header;
                g_bindKeys  = FindChild(cp, "dlgBindKey");
                g_cbProfile = FindChild(header, "cbProfile");
                g_cbDevice  = FindChild(header, "cbDevice");
                PopulateCombos();
                if (g_mouseView)
                    EnterMouseView(self);
            }
            return r;
        }

        // ControlOptionsWnd::OnWndNotify @0x4b0ba0. Steal only 5 bytes (byte 20+ is
        // a relative je — relocating it breaks the target). Handles our header
        // buttons; the stock mouse-widget notifies pass through to the original.
        using OnWndNotify_t = int(__thiscall*)(void*, void*, unsigned, unsigned, void*);
        OnWndNotify_t g_origNotify = nullptr;

        int __fastcall OnWndNotify_Hook(void* self, void* /*edx*/, void* from,
                                        unsigned id, unsigned msg, void* data) {
            int r = g_origNotify(self, from, id, msg, data);
            if (msg == NOTIFY_CLICK) {
                if (id == ID_PROFILE_NEW)    { NewProfile();    return 1; }
                if (id == ID_PROFILE_DELETE) { DeleteProfile(); return 1; }
            } else if (msg == NOTIFY_COMBOSEL) {
                // Selecting an item closes the dropdown — drop the header back
                // behind so the bindings list is clickable again.
                if (id == ID_CB_PROFILE) { OnProfileSelected(); SyncHeaderZOrder(); return 1; }
                if (id == ID_CB_DEVICE)  { OnDeviceSelected();  SyncHeaderZOrder(); return 1; }
            }
            return r;
        }

        // m3d::ui::ComboBoxWnd::OnMouseButton0 @0x71f120 (5-byte prologue:
        // mov eax,[esp+8] / push esi — no relative branches). Shared by every
        // combo; for OUR two dropdowns we re-sync the header z-order after the
        // click toggles the list open/closed (front while open so the list draws
        // on top of the bindings frame and is clickable, behind otherwise).
        using CbMouseBtn_t = int(__thiscall*)(void*, unsigned, void*);
        CbMouseBtn_t g_origCbMouseBtn = nullptr;

        int __fastcall CbMouseBtn_Hook(void* self, void* /*edx*/, unsigned state, void* at) {
            int r = g_origCbMouseBtn(self, state, at);
            if (self == g_cbProfile || self == g_cbDevice)
                SyncHeaderZOrder();
            return r;
        }

        // m3d::ui::ModalWnd::OnWndNotify @0x677750 (5-byte prologue: push ebx /
        // mov ebx,[esp+0x10] — no relative branches). This handler is shared by
        // every modal in the game, so we act ONLY for our own name-entry modal
        // (g_nameModalActive && sender is g_nameModal) on the OK button (id 12001):
        // read the typed name while the modal is still alive, mark accepted, and
        // close it (DoModal then returns to PromptProfileName). Everything else —
        // including Cancel (id 3) and Esc — tail-calls the original, which
        // auto-closes ids 1..3.
        using ModalNotify_t = int(__thiscall*)(void*, void*, unsigned, unsigned, void*);
        ModalNotify_t g_origModalNotify = nullptr;

        int __fastcall ModalNotify_Hook(void* self, void* /*edx*/, void* from,
                                        unsigned id, unsigned msg, void* data) {
            if (g_nameModalActive && self == g_nameModal &&
                msg == NOTIFY_CLICK && id == ID_NAME_OK) {
                hta::CStr text;
                EditGetText(FindChild(self, "editName"), &text);
                const char* c = text.c_str();
                g_nameResult   = c ? c : "";
                g_nameAccepted = true;
                CloseModal(self, 1);
                return 1;
            }
            return g_origModalNotify(self, from, id, msg, data);
        }

        // BindKeysWnd::ApplyBindings @0x4a64e0 — __stdcall(void* self) despite the
        // PDB's thiscall label (this on the stack, ret 4). After the stock Apply
        // commits the live edit, mirror it into the active profile's keybindings.
        using ApplyBindings_t = void(__stdcall*)(void* self);
        ApplyBindings_t g_origApplyBindings = nullptr;

        void __stdcall ApplyBindings_Hook(void* self) {
            g_origApplyBindings(self);
            std::string active = inputprofiles::Active();
            if (!active.empty())
                inputprofiles::SyncBindingsToProfile(active);
        }

        // BindKeysWnd::OnWndNotify @0x4a6310 (5-byte prologue: cmp [esp+0xc],1 — no
        // relative branches) — the bindings-editor button handler. It calls the
        // bindings manager ([app+0x8b240]) INLINE: Cancel(700000)->vtbl+0x2c,
        // Apply(700001)->+0x34, Default(700002)->+0x28. The Default path loads the
        // game's stock bindings into the LIVE GameImpulse, which fires inputprofiles'
        // LoadFromDefaults detour; left alone that reloads the active profile's
        // keybindings.lua over the defaults — wiping everything when that file is
        // empty. So around the Default click we suppress the profile re-apply (the
        // defaults stand) and then persist them into the active profile. On Apply we
        // also mirror the committed bindings into the active profile (the editor's
        // Apply button reaches the manager directly, not BindKeysWnd::ApplyBindings,
        // so our ApplyBindings detour does not catch it).
        using BindKeysNotify_t = int(__thiscall*)(void*, void*, unsigned, unsigned, void*);
        BindKeysNotify_t g_origBindKeysNotify = nullptr;

        int __fastcall BindKeysNotify_Hook(void* self, void* /*edx*/, void* from,
                                           unsigned id, unsigned msg, void* data) {
            if (msg == NOTIFY_CLICK && id == ID_BK_DEFAULT) {
                inputprofiles::SuppressReapply(true);
                int r = g_origBindKeysNotify(self, from, id, msg, data);
                inputprofiles::SuppressReapply(false);
                std::string active = inputprofiles::Active();
                if (!active.empty()) {
                    inputprofiles::SyncBindingsToProfile(active);
                    // "По умолчанию" also resets the axis layout to the detected
                    // controller's default (native DualSense vs XInput) — the only
                    // place we auto-apply a preset, so custom mappings survive.
                    inputprofiles::ApplyDeviceDefaults(active);
                }
                return r;
            }
            int r = g_origBindKeysNotify(self, from, id, msg, data);
            if (msg == NOTIFY_CLICK && id == ID_BK_APPLY) {
                std::string active = inputprofiles::Active();
                if (!active.empty())
                    inputprofiles::SyncBindingsToProfile(active);
            }
            return r;
        }
    }

    void Apply() {
        g_origSetup = reinterpret_cast<GameDataSetup_t>(
            InstallDetour(0x004C30A0, 6, (void*)&GameDataSetup_Hook));
        g_origOptNotify = reinterpret_cast<OptOnWndNotify_t>(
            InstallDetour(0x004C3560, 5, (void*)&OptOnWndNotify_Hook));
        g_origOnBeforeAdd = reinterpret_cast<OnBeforeAdd_t>(
            InstallDetour(0x004C38C0, 10, (void*)&OnBeforeAdd_Hook));
        g_origNotify = reinterpret_cast<OnWndNotify_t>(
            InstallDetour(0x004B0BA0, 5, (void*)&OnWndNotify_Hook));
        g_origModalNotify = reinterpret_cast<ModalNotify_t>(
            InstallDetour(0x00677750, 5, (void*)&ModalNotify_Hook));
        g_origBindKeysNotify = reinterpret_cast<BindKeysNotify_t>(
            InstallDetour(0x004A6310, 5, (void*)&BindKeysNotify_Hook));
        g_origCbMouseBtn = reinterpret_cast<CbMouseBtn_t>(
            InstallDetour(0x0071F120, 5, (void*)&CbMouseBtn_Hook));
        g_origApplyBindings = reinterpret_cast<ApplyBindings_t>(
            InstallDetour(0x004A64E0, 5, (void*)&ApplyBindings_Hook));
        if (g_origSetup && g_origOptNotify && g_origNotify)
            LOG_INFO("Unified control page + Mouse tab installed");
        if (g_origApplyBindings)
            LOG_INFO("Bind-keys Apply sync installed (keeps active profile's bindings current)");
    }
}
