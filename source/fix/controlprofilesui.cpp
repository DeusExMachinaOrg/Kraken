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
        constexpr unsigned ID_PROFILE_PREV   = 12001;
        constexpr unsigned ID_PROFILE_NEXT   = 12002;
        constexpr unsigned ID_PROFILE_NEW    = 12003;
        constexpr unsigned ID_PROFILE_DELETE = 12004;
        constexpr unsigned ID_DEVICE_PREV    = 12005;
        constexpr unsigned ID_DEVICE_NEXT    = 12006;
        constexpr unsigned NOTIFY_CLICK      = 1;

        // Russian UI text in windows-1251 (set from code; see the encoding note in
        // the memory file — literal Cyrillic in the UTF-8-saved XML is mis-decoded).
        const char* const RU_PROFILE = "\xCF\xF0\xEE\xF4\xE8\xEB\xFC:";             // Профиль:
        const char* const RU_DEVICE  = "\xD3\xF1\xF2\xF0\xEE\xE9\xF1\xF2\xE2\xEE:"; // Устройство:
        const char* const RU_NEW     = "\xCD\xEE\xE2\xFB\xE9";                      // Новый
        const char* const RU_DELETE  = "\xD3\xE4\xE0\xEB\xE8\xF2\xFC";              // Удалить
        const char* const RU_MOUSE   = "\xCC\xFB\xF8\xFC";                          // Мышь

        // --- state (single Options menu, single thread) ---
        void* g_lblProfileName = nullptr;
        void* g_lblDeviceName  = nullptr;
        void* g_header         = nullptr; // wndProfileHeader
        void* g_bindKeys       = nullptr; // dlgBindKey / BindKeysWnd
        int   g_viewApplied    = 0;       // 0 = bindings on-screen, 1 = mouse view
        float g_mouseUp        = 257.0f;  // px to lift the mouse block up in mouse view
        bool  g_mouseView      = false;

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

        void RefreshHeader() {
            std::string active = inputprofiles::Active();
            std::string label = active;
            for (const auto& p : inputprofiles::List()) {
                if (p.name == active) { label = p.display.empty() ? p.name : p.display; break; }
            }
            SetWndText(g_lblProfileName, label.c_str());

            uint32_t dev = Config::Instance().wheel_device.value;
            std::string devLabel;
            for (const auto& d : inputprofiles::Devices()) {
                if (d.id == dev) { devLabel = d.name; break; }
            }
            if (devLabel.empty()) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "#%u", dev);
                devLabel = buf;
            }
            SetWndText(g_lblDeviceName, devLabel.c_str());
        }

        void CycleProfile(int dir) {
            if (!inputprofiles::Available()) return;
            auto list = inputprofiles::List();
            if (list.empty()) return;
            std::string active = inputprofiles::Active();
            int idx = 0;
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].name == active) { idx = static_cast<int>(i); break; }
            int n = static_cast<int>(list.size());
            idx = (idx + dir + n) % n;
            inputprofiles::Switch(list[idx].name);
            RefreshHeader();
        }

        void NewProfile() {
            if (!inputprofiles::Available()) return;
            char name[32];
            for (int n = 1; n <= 99; ++n) {
                std::snprintf(name, sizeof(name), n == 1 ? "new profile" : "new profile %d", n);
                if (inputprofiles::Create(name, "custom")) {
                    inputprofiles::Switch(name);
                    break;
                }
            }
            RefreshHeader();
        }

        void DeleteProfile() {
            if (!inputprofiles::Available()) return;
            std::string active = inputprofiles::Active();
            if (!active.empty())
                inputprofiles::Delete(active); // switches to a fallback profile internally
            RefreshHeader();
        }

        void CycleDevice(int dir) {
            if (!inputprofiles::Available()) return;
            std::string active = inputprofiles::Active();
            if (active.empty()) return;
            auto devs = inputprofiles::Devices();
            if (devs.empty()) return;
            uint32_t cur = Config::Instance().wheel_device.value;
            int idx = 0;
            for (size_t i = 0; i < devs.size(); ++i)
                if (devs[i].id == cur) { idx = static_cast<int>(i); break; }
            int n = static_cast<int>(devs.size());
            idx = (idx + dir + n) % n;
            inputprofiles::SetDevice(active, devs[idx].id);
            RefreshHeader();
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
            g_lblProfileName = FindChild(header, "lblProfileName");
            g_lblDeviceName  = FindChild(header, "lblDeviceName");
            SetWndText(FindChild(header, "lblProfileCaption"), RU_PROFILE);
            SetWndText(FindChild(header, "lblDeviceCaption"),  RU_DEVICE);
            SetWndText(FindChild(header, "btnProfileNew"),     RU_NEW);
            SetWndText(FindChild(header, "btnProfileDelete"),  RU_DELETE);
            RefreshHeader();
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
                if (g_mouseView && (id == ID_STOCK_TAB || id == ID_STOCK_EXIT)) {
                    bool controlClicked = (id == ID_STOCK_TAB) && from &&
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
        void EnterMouseView(void* opt); // fwd

        int __fastcall OnBeforeAdd_Hook(void* self, void* /*edx*/) {
            int r = g_origOnBeforeAdd(self);
            if (g_mouseView) {
                void* cp = StockPage(self, TAB_CONTROL);
                if (cp && FindChild(cp, "wndProfileHeader")) {
                    g_header   = FindChild(cp, "wndProfileHeader");
                    g_bindKeys = FindChild(cp, "dlgBindKey");
                    EnterMouseView(self);
                }
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
                switch (id) {
                    case ID_PROFILE_PREV:   CycleProfile(-1); return 1;
                    case ID_PROFILE_NEXT:   CycleProfile(+1); return 1;
                    case ID_PROFILE_NEW:    NewProfile();     return 1;
                    case ID_PROFILE_DELETE: DeleteProfile();  return 1;
                    case ID_DEVICE_PREV:    CycleDevice(-1);  return 1;
                    case ID_DEVICE_NEXT:    CycleDevice(+1);  return 1;
                    default: break;
                }
            }
            return r;
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
    }

    void Apply() {
        g_origSetup = reinterpret_cast<GameDataSetup_t>(
            InstallDetour(0x004C30A0, 6, (void*)&GameDataSetup_Hook));
        g_origOptNotify = reinterpret_cast<OptOnWndNotify_t>(
            InstallDetour(0x004C3560, 5, (void*)&OptOnWndNotify_Hook));
        g_origOnAfterRemove = reinterpret_cast<OnAfterRemove_t>(
            InstallDetour(0x004C3640, 5, (void*)&OnAfterRemove_Hook));
        g_origNotify = reinterpret_cast<OnWndNotify_t>(
            InstallDetour(0x004B0BA0, 5, (void*)&OnWndNotify_Hook));
        g_origApplyBindings = reinterpret_cast<ApplyBindings_t>(
            InstallDetour(0x004A64E0, 5, (void*)&ApplyBindings_Hook));
        if (g_origSetup && g_origOptNotify && g_origNotify)
            LOG_INFO("Unified control page + Mouse tab installed");
        if (g_origApplyBindings)
            LOG_INFO("Bind-keys Apply sync installed (keeps active profile's bindings current)");
    }
}
