# Control profiles + native Options tab (design & reverse-engineering)

Goal: an in-game **Options tab** where the player creates named **control profiles**
(one for the wheel, one for DualSense, one for XInput, ...), assigns keys / axes /
triggers / joystick buttons per profile, sees **live analog-strength bars** for
axes/triggers/pedals, and switches the active profile. All addresses are VAs for
`hta.exe` (image base 0x400000); RVA = VA − 0x400000.

User decisions (2026-06-30):
- **Full native-looking tab** (not just a selector on the existing Control page).
- A profile carries **device config + its own engine keybindings snapshot**.
- The windows must show **sliders/bars of the live press strength** of analog
  buttons / sticks / pedals.

---

## 1. How the Options menu works (reversed)

Fully native C++ (`truxx/game/uiWindows/MiscWindows/OptionsWnd.cpp`). The menu
layout is XML prefabs (`data/if/dialogs/*.xml`); widget *behaviour* is in native
classes keyed by widget `id`.

### OptionsWnd (ModalWnd, size 636)
Fields:
- `m_curTabId` @0x224 (548), `m_lastTabId` @0x228 (552) — `enum OptionsWnd::Tab`.
- `m_tabButtons` @0x22c (556) — `vector<OptionTabButton*>` (data ptr at +0x230).
- `m_optionWindows` @0x23c (572) — `vector<ref_ptr<Wnd>>` (data ptr at +0x240).
- `m_aif` @0x24c (588) — `OptionsWnd::AuxInfo`, **fixed 48 bytes = 4 tabs × 12**.

`enum OptionsWnd::Tab`: TAB_VIDEO=0, TAB_SOUND=1, TAB_CONTROL=2, TAB_GAME=3,
TAB_NUM_TABS=4, TAB_INVALID=4.

Methods (RVA):
- `GameDataSetup` 0xc30a0 — builds tabs. Loops `i=0..3`; per tab reads a child
  control name from `m_aif[i]` (`GetChildByName`), checks `IsKindOf
  OptionTabButton` (class @0xa01f68), creates the button via
  `OptionTabButton::CreateFromPattern` (0xc3be0), stores it, sets
  `button->m_tabId @+0x240 = i`, maps `GetOptionWindowGuiIdByTabId(i)` and stores
  the page window. **The `4` is hardcoded** (`cmp edi,4`) and `m_aif` is a fixed
  4-slot array → a true in-struct 5th tab is impractical.
- `GetOptionWindowGuiIdByTabId` 0xc3880 — switch: 0→0x95, 1→0x96, 2→0x97, 3→0x98
  (GUI resource ids 149..152 = the page prefabs). default → -1.
- `ShowOptionWindowForTab` 0xc3720 — hide `m_optionWindows[m_lastTabId]` (vtbl
  +0x24), add+`MoveChildToFirstPosition` `m_optionWindows[tabId]` (vtbl +0x20).
- `SetCurTab` 0xc36e0, `SelectTabButton` 0xc3830, `ApplyTabChanges` 0xc3910.
- `OnWndNotify` 0xc3560 — calls base `ModalWnd::OnWndNotify` (0x277750) first;
  then: `id==0x2710` (10000 btnExit) & msg==1 → close (vtbl +0x50);
  `id==0x2711` (10001, **all tab buttons share this id**) & msg==1 &
  `from` IsKindOf OptionTabButton (class @0xa0817c) → `SetCurTab(from->m_tabId
  @+0x240)`. **So tab routing is data-driven by the button's `m_tabId`.**

Tab button prefabs in `data/if/dialogs/optionswnd.xml`: `tabBtnVideo/Sound/
Control/Game` all `id=10001`; `btnExit` `id=10000`. Pages: `controloptionswnd.xml`
(`wndControlOptions`), etc.

### Page class template — ControlOptionsWnd (Wnd, size 632)
Pattern to mirror. Fields = pointers to child widgets resolved by name; embeds
`m_wndKeyBindings : ref_ptr<BindKeysWnd>` @0x274 (628).
- `GameDataSetup` 0xb03c0 — find child widgets by name, store ptrs, wire handlers.
- `OnWndNotify` 0xb0ba0 — dispatch widget notifies by id (slider change, button
  click, check toggle).
- `InitControls`/`UpdateControls`/`ApplyChanges`. Mouse sensitivity uses
  `SliderWnd` (id 10400) + prev/next buttons (10401/10402) + flip checks
  (10403/10404). Good reference for driving a SliderWnd and CheckWnd.

### Key bindings (keyboard/mouse only)
`BindKeysWnd` / `BindKeysList` — `StringsListBoxWnd` of `KeySetButton` rows
(`bindkeysdlg.xml`, list id 700003, Apply 700001, Default 700002). Native joystick
binding is dead in this build (see `[[dualsense-gamepad]]` memory): KeySetButton::
OnKey is scancode-based; we already commit JOY_BUTTON_* from Kraken in
`fix/gamepad.cpp` (TryCaptureForBind).

---

## 2. Chosen approach — "native tab button → Kraken-driven page"

> **SUPERSEDED (2026-07-02) — see §6.** The separate 5th "Профили" tab (below)
> shipped and works, but fragments one profile's config across two screens. Final
> design repurposes the stock **Control tab** into a single unified page. Keep §2
> only as the reversing record for the overlay technique.


A true 5th tab inside OptionsWnd is blocked by the fixed 4-slot `m_aif` + hardcoded
loop bounds + jump tables. Instead:

1. **Add a 5th tab button** `tabBtnProfiles` to `optionswnd.xml` with a **unique id
   (e.g. 10006)** — NOT 10001, so the stock tab loop/`OnWndNotify` ignore it.
2. **Add a page prefab** `profileoptionswnd.xml`: a plain `class="Wnd"` page using
   only stock widgets (StringsListBoxWnd for the profile list + the binding rows,
   ButtonWnd for New/Delete/Rename/Apply/Default, EditWnd for the name, SliderWnd /
   image bars for the live analog strength). No new RTTI class.
3. **Detour `OptionsWnd::OnWndNotify` (0xc3560)**: 
   - `id==10006` & msg==1 → show our page (add as direct child + MoveChildTo
     FirstPosition; hide the current native page) and mark our tab active.
   - When a native tab is selected (the stock path runs), hide our page.
   We add/instantiate our page lazily on `OptionsWnd` open (detour
   `GameDataSetup` tail or `OnBeforeAddToWndStation`), loading the prefab via the
   GUI resource system the same way pages 0x95..0x98 are created.
4. **Drive all page logic from Kraken** (mirrors `fix/gamepad.cpp`): find child
   widgets by name after the page is built, read/write SliderWnd & list & edit,
   intercept row clicks for capture. This is `fix/controlprofiles.cpp` (new).

### Layer-2 primitives (reversed — confirmed)
GUI registry: `data/if/dialogs/uiwindows.xml` = `GuiResourceInfo` items mapping
`class` (RTTI) + `id` (IW_WND_OPTIONS_* string) + `file` (prefab). `GameUiManager`
(@`g_pApp+0x8b4ec`) holds `m_windows : map<int, ref_ptr<Wnd>>`. We do **not** need a
registered gui id — build our page from its prefab file directly:
- **`m3d::ui::LoadDialog(CStr* file)` @0x2744f0 (`__fastcall`, ecx=CStr*) → Wnd\***
  — builds a full window tree from a prefab XML path. This is THE page-creation
  primitive. (`LoadExistingDialog(Wnd*, CStr*)` @0x2746e0 fills an existing one.)
- `GameUiManager::GUI_CreateWindow(needShow, CStr* file, ww, CStr*)` @0x142d70 and
  `GUI_AddWindow(w, &id, persistent, show)` @0x1419e0 (auto dynamic id) — higher-level
  alternatives if we want the manager to own it.
- Add to OptionsWnd as a child: vtbl **+0x20 = AddChild(Wnd\*)**, **+0x24 = hide/remove**,
  `Object::MoveChildToFirstPosition` @0x405340, `Object::GetChildByName` @0x435700,
  `Object::IsDirectChild` @0x4054f0.
- `OptionsWnd::OnWndNotify` @0xc3560 is a clean `__thiscall(this, Wnd* from, uint id,
  uint msg, AIParam* data)` ret 0x10 — trampoline-detourable. Buttons notify with
  **msg==1**; tab buttons share id 0x2711, btnExit 0x2710. Our tab button uses a
  unique id (e.g. 10006) so the stock loop ignores it and our detour routes it.

### List widget API — `ListBoxWnd<CStr>` (base of StringsListBoxWnd, item rows)
`m_items` @+0x220 (544), `m_curSel` @+0x230 (560). All `__thiscall`:
- `AddItem(CStr*)` @0xabf40 → idx; `InsertItem(CStr*, idx)` @0x320520;
  `RemoveAllItems()` @0xac000; `RemoveItem(idx)` @0xac950; `GetCount()` @0xac080.
- `GetItem(idx)` @0xabfd0 → CStr; `SetItem(idx, CStr*)` @0x320120.
- `GetCurSel()` @0xac0b0; `SetCurSel(int)` @0x31e990 (virtual).
- `SetItemData(item, int)` @0x320180 / `GetItemData(item)` @0x320160 — stash the
  winmm device id / profile index per row.
StringsListBoxWnd vtable/class id needed to recognise our list child by name via
GetChildByName (name from our prefab).

Still to reverse when wiring: SliderWnd get/set pos + range (live strength bars +
sensitivity-style controls), EditWnd get/set text (profile name entry), CheckWnd
get/set (invert toggles), and the engine's text-input/msgbox for naming.

---

## 3. Profile data model + runtime switching (Kraken core, engine-independent)

A **profile** = a folder `data/input_profiles/<name>/` with:
- `input.ini` — the device sections `[wheel] [gamepad] [dualsense] [xinput]`
  (+ `gamepad` button*  bindings). Same keys as today's `kraken.ini`.
- `keybindings.lua` — snapshot of the engine keyboard/mouse bindings for this
  profile (written via the engine's save path; loaded via `GameImpulse::
  LoadFromProfile`-style replay or by executing the lua through ScriptServer).
- `meta.ini` — display name, device kind (wheel/dualsense/xinput/custom).

`Config` today: Win32 `GetPrivateProfileStringA` against one `CONFIG_PATH =
./data/kraken.ini`; each `ConfigValue` holds its `section`/`key`. Refactor:
- Tag the input ConfigValues (the `[wheel]/[gamepad]/[dualsense]/[xinput]`
  sections) as **profile-sourced**. Add a per-load path override so those values
  read from the active profile's `input.ini`, everything else from `kraken.ini`.
- `kraken.ini [input] active_profile=<name>` selects the active profile.
- `Config::ReloadInput()` re-reads only the input sections from a given profile.

### Engine profile system (reversed — for the chosen "inside player profile" storage)
Profiles live under the **active player profile folder**: store ours at
`<playerProfileFolder>/input_profiles/<name>/`.
- `CMiracle3d::GetProfileManager` 0x4fc0.
- `TruxxImpulse::GetProfileFolder` 0x1508c0 → returns the active player's folder
  CStr. Mechanics: `mgr = *(g_pApp + 0x8b52c)`; current profile **name** CStr at
  `mgr + 0x44`; `ProfileManager::_GetProfileByName(mgr, &name)` 0xfcd0 → `Profile*`;
  the **folder path** is `Profile + 0x40` (CStr). `g_pApp = *0xa0a55c`.
  → Kraken can get the active folder by replaying this (no need to detour).
- `Profile::GetName` 0xdb20, `ProfileManager::GetProfileByName` 0xfcc0,
  `m3d::GameImpulse::LoadFromProfile` 0x197ab0 (loads that profile's keybindings).
- **`ChangeProfileWnd`** — existing native window that creates/deletes/lists
  *player* profiles: `CreateProfile` 0xab950, `DeleteProfile` 0xab760, `Clear`
  0xab460, `FullUpdate` 0xabad0, `OnWndNotify` 0xab550. **Best UI + flow template
  for our ProfileOptionsWnd** (list widget population, create/delete, name entry).

`ProfileManager` (new): enumerate `<playerFolder>/input_profiles/*`, create (clone defaults),
rename, delete, get/set active. On switch:
1. `Config::ReloadInput(profile)`.
2. Re-apply the input modules: `fix::controls` (axes/trigger/cam/ffb re-read),
   `fix::gamepad` (re-bind JOY_BUTTON_*), `fix::dualsense`, `fix::xinputrumble`.
   Each gets a `Reapply()` that recomputes from `Config` without a full restart.
3. Load the profile's engine keybindings.

---

## 4. Live analog-strength bars

The page shows a bar per analog control (steer, throttle, brake, trigger, look X/Y).
`fix/controls.cpp` already computes normalized axis values each frame; expose them
(atomics) and, on a UI timer while the page is visible, set each SliderWnd position
(`SliderWnd::SetPos`-equivalent) from the live value. Doubles as the capture aid:
"move an axis → it lights up → assign it to the focused binding row".

---

## 5. Build / deploy
See `[[kraken-build-deploy]]`. Reverse via lora MCP (alias `hta` = game.pdb,
base 0x400000); see `[[lora-pdb-harness]]`.

---

## 6. FINAL DESIGN (2026-07-02) — one unified Control page (repurpose the stock "Управление" tab)

Decision (user, 2026-07-02): **drop the separate 5th "Профили" tab + overlay hack.**
Repurpose the stock **Control tab (TAB_CONTROL=2)** so clicking "Управление" opens a
single unified page carrying **profile selector + device picker + section switcher
(Клавиши / Оси / Устройство)**. One entry point, zero fragmentation — the whole
identity of one controller is configured on one screen (like other games' Controls).

### 6.1 Why repurposing slot 2 is safe (all reversed this session)
- `OptionsWnd::ShowOptionWindowForTab` (0xc3720) show/hides `m_optionWindows[tabId]`
  **generically**: hide last via vtbl **+0x24**, show new via vtbl **+0x20**
  (AddChild) + `MoveChildToFirstPosition`. It never assumes the page's concrete type.
- `OptionsWnd::ApplyTabChanges` (0xc3910) reaches slot 2 **only** through
  `IsKindOf(ControlOptionsWnd)` (and slot 0 via IsKindOf VideoOptionsWnd). A plain
  `Wnd` in slot 2 → guard fails → `ControlOptionsWnd::ApplyChanges()` is **skipped,
  no crash**. We run our own apply (see 6.5).
- `GetOptionWindowGuiIdByTabId` (0xc3880): tab 2 → gui id 0x97; `OptionsWnd::
  GameDataSetup` (0xc30a0) builds each page from that id into `m_optionWindows[i]`.

### 6.2 SHIPPED — "augment ControlOptionsWnd in place" (simpler than the slot-swap
plan below; no tab interception, no ref_ptr surgery, no reparenting)
Actually implemented (2026-07-02), after finding that both risky options in the
original §6.2 draft (GUI-id redirect / slot swap) were unnecessary:
- **Never intercept the Control tab click at all.** The stock flow (`SetCurTab(2)`
  → `ShowOptionWindowForTab` shows `m_optionWindows[2]`) runs completely unchanged.
  `ApplyTabChanges`'s `IsKindOf(ControlOptionsWnd)` guard still passes normally, so
  mouse-sensitivity Apply keeps working natively — zero risk of skipping it.
- Build the header **lazily**, triggered from `OptionsWnd::OnWndNotify`
  (VA `0x4C3560`, 5-byte prologue, same address the original 5th-tab prototype
  used): call orig first, then (once, guarded by a bool) `FindChild(self,
  "wndControlOptions")` and if found, `AddChild` our header prefab
  (`ControlProfileHeaderWnd.xml`) into it + `MoveChildToFirstPosition` so it draws
  above the bindkeys list. Firing after the menu has processed a notify (i.e. is
  live) is the conservative time to `LoadDialog`+`AddChild`.
  - **NB — the real startup-crash cause was NOT timing.** A first pass hooked
    `ControlOptionsWnd::GameDataSetup` and crashed at `0x00EF0029` (inside our
    VirtualAlloc'd trampoline) with `this=5`, AV writing `0x28A0`, surfacing deep
    in `WndStation::ProcessEvent`. Root cause: the detour installer copied a
    **22-byte** prologue of `ControlOptionsWnd::OnWndNotify` (0x4b0ba0), and byte
    20 onward is a relative `je 0x4b0bc9`. Relocating a rel8/rel32 branch into the
    trampoline breaks its target → garbage execution. **Fix**: steal only the
    first **5** bytes (`push ebx` + `mov ebx,[esp+0xc]`, boundary at 0x4b0ba5, no
    position-relative bytes). Lesson: a stolen prologue must end on an instruction
    boundary AND contain no rip/rel-relative jmp/call/jcc — the naive raw-byte
    trampoline here does not fix up relocations.
- **Hook `ControlOptionsWnd::OnWndNotify`** (VA `0x4B0BA0`, 22-byte prologue) to
  catch clicks on the header's own new widget ids (12001-12006); call orig first
  (harmless no-op for our ids, preserves native 10400-10404 mouse-control dispatch
  untouched), then handle ours.
- **BindKeysWnd and the mouse SliderWnd/CheckWnd controls are never touched** — no
  reparenting, no forwarding hack needed. This eliminates the entire "mouse settings
  become unreachable" and "reparent BindKeysWnd" risk classes from the original plan.

### 6.3 Keys section = the native BindKeysWnd, unchanged
`ControlOptionsWnd::GameDataSetup` (0xb03c0) fetches the shared registered
**BindKeysWnd**, checks `IsKindOf(BindKeysWnd)`, stores it in `m_wndKeyBindings
@0x274`, and `AddChild`s it (vtbl+0x20) — exactly where it always was. We add our
header as a sibling on top, nothing more. Full native "click action → press
key/button → rebind" flow (incl. `JOY_BUTTON_*`) keeps working as-is. Per-profile
scoping: `Switch` = `UnbindAll`+`LoadFromFile`; stock Apply (id 700001, hooked at
`BindKeysWnd::ApplyBindings` 0x4A64E0) → `SyncBindingsToProfile`.

### 6.4 Mouse settings — untouched, still fully reachable
Because ControlOptionsWnd is never replaced or hidden, `sliderMouseSensitivity`
(10400) + prev/next (10401/2) + `checkMouseFlipX/Y` (10403/4) keep working exactly
as before, dispatched by the native jump table inside `ControlOptionsWnd::
OnWndNotify`. No carry-over work needed.

### 6.5 Header layout (v3 — the Control page has NO free band; make one)
Key realisation after v2: `ControlOptionsWnd` is packed top-to-bottom with **no**
empty strip anywhere — title (y137-187) → bindkeys list (y213-469, from the shared
`BindKeysWnd`/`bindkeysdlg.xml`) → mouse sensitivity+flip (y497-565) → line2 (y579)
→ the bindkeys **Apply/Default buttons (y590)**. v1 overlapped the list top; v2's
"bottom strip" landed straight on the Apply/Default buttons. Both wrong.
- **v3 carves space**: edit `bindkeysdlg.xml` (both variants — it's a singleton
  `IW_DLG_BINDKEYS` used *only* by the control page, comment confirms) to push the
  list **down from the top**: 4:3 `lstBindings` y213→272 h256→197, `wndBindingsFrame`
  y205→264 h276→217 (bottom edges unchanged); 16:9 y300→383 / 288→371. This frees a
  ~75px band directly under the title.
- The header (`ControlProfileHeaderWnd.xml`, full-screen root `Wnd`, `AddChild`'d
  into `ControlOptionsWnd`, 16:9 via `x'=x*1.40625+240, y'=y*1.40625` verified vs
  stock tabs) sits in that band, **two rows at y=192 / y=226 (4:3)**, no background
  strip (labels on the page like the stock mouse controls):
- Row 1: "Профиль:" + name label + Prev/Next (12001/12002, reuse
  `PaneBtnSliderPrev/Next`) + "Новый"/"Удалить" (12003/12004).
- Row 2: "Устройство:" + name label + Prev/Next (12005/12006), cycling
  `inputprofiles::Devices()` → `SetDevice` on the active profile.
Prev/Next act immediately (carousel, like the game's mouse-sensitivity prev/next) —
no separate "Activate". **Captions are set from C++ in windows-1251** (`RU_PROFILE`
/`RU_DEVICE`/`RU_NEW`/`RU_DELETE` in controlprofilesui.cpp) — the XML `caption=""`
is left empty because the tooling saves the .xml UTF-8, so literal Cyrillic there is
mis-decoded by the engine (this was the "broken encoding" bug in v1).

### 6.6 Axes / Device sections (NOT YET BUILT — Phase B/C)
- **Оси**: rows руль/газ/тормоз/обзор X/Y — bound axis (capture by moving it), invert
  check, **live strength bar** (resize a colored panel child each frame from
  `fix/controls.cpp` normalized values), deadzone + sensitivity SliderWnd. Needs a
  per-frame tick while the page is visible (reuse the existing controls per-frame
  hook; gate on page visibility). Doubles as capture aid.
- **Устройство** (by `device_kind`): wheel→FFB strength/effects; XInput→rumble;
  DualSense→rumble + adaptive triggers + haptics.

### 6.7 Backlog extras
Duplicate profile; per-section reset-to-default; live input-test/diagnostic view
(every axis/button lights up — verifies "does the game see my wheel"); auto-switch
profile on controller connect (winmm enum already exists); sensitivity curve.

### 6.8 Phasing
- **Phase A — SHIPPED (2026-07-02)**: profile+device header on the Control tab
  (§6.2-6.5); old 5th-tab overlay code (`OptionTabButton` conversion, synthetic
  `tabBtnProfiles`, `OptionsWnd::OnWndNotify`/`GameDataSetup` hooks) fully removed
  from `fix/controlprofilesui.cpp`; `tabBtnProfiles` nodes removed from
  `optionswnd.xml` (both variants); `ProfileOptionsWnd.xml` (both variants) deleted,
  replaced by `ControlProfileHeaderWnd.xml`. Built (Debug) and deployed. **Not yet
  tested in-game** — header layout (§6.5) is a first pass, expect visual iteration.
- **Phase B — Axes** (next): axes section + live bars + capture + deadzone/sens/invert.
- **Phase C — Device**: per-`device_kind` tuning section (FFB/rumble/haptics).
- **Phase D — polish**: rename text-entry, duplicate, reset, input-test, auto-switch.

### 6.9 Mouse settings → 5th "Мышь" tab (user request, 2026-07-02)
The Control page was still too cramped, so mouse **sensitivity + invert X/Y** moved
off the bindings view onto a revived **5th tab "Мышь"**. Key insight that made this
clean: `m3d::ui::Wnd::ShowWindow(bool)` @ **0x41c2e0** is a non-destructive
visibility toggle (sets/clears `[this+0x89] & 2`, `ret 4`, thiscall). So we do NOT
move the mouse widgets to a new window (which would break their native handlers'
cached widget pointers) — they stay children of `ControlOptionsWnd`; we just flip a
**bindings view / mouse view** by `ShowWindow`-ing two widget groups:
- mouse group: `sliderMouseSensitivity`,`btnMouseSensitivity{Prev,Next}`,
  `lblMouseSensitivity`,`check/lbl/emboss MouseFlip{X,Y}`,`wndLine2`.
- bindings group: our `wndProfileHeader` + the embedded `dlgBindKey` (BindKeysWnd).
Native mouse handlers (`OnSliderMouseSensitivityChange`, `OnCheckMouseFlip*`) are
untouched — zero re-implementation of the sensitivity math.

**IMPORTANT — `ShowWindow` only gates a window's OWN paint, not its children's.**
`ShowWindow(false)` sets bit `0x200` at `[this+0x88]` (read by `IsVisible`
@0x6f840); the draw path skips that window's own OnPaint but still recurses its
children. So it hides *leaf* widgets (the mouse slider/checks/labels → the
"free space" that appeared in the bindings view) but NOT *containers*: hiding
`dlgBindKey` left the bindings list + Apply/Default drawn (v1 bug — "Mouse panel
still shows the Control panel"). Fix: hide the bindings **containers** (our
`wndProfileHeader` and `dlgBindKey`) by **detaching** them —
`RemoveChild`/`AddChild`, the engine's own page-hide mechanism — guarded by a
one-int state so we never double add/remove, with their Object refcounts **pinned**
(`++[obj+4]`) in BuildHeader so detaching can't drop the last ref and destroy them
(`dlgBindKey` is also ref-held by ControlOptionsWnd's `m_wndKeyBindings`; the header
is not, hence the pin). Leaf mouse widgets keep using `ShowWindow`.
With the mouse settings gone from the bindings view, the bindings list is grown
down into the freed bottom space (bindkeysdlg.xml: 4:3 `lstBindings` h197→300,
frame h217→312; 16:9 h277→420 / 305→432; stays above Apply/Default).

Tab plumbing (revived from the old Profiles-tab prototype):
- `tabBtnMouse` id **10006** in optionswnd.xml (both variants), at the old Profiles
  slot (y448/630) where the panel.dds shadow already exists; converted to a real
  `OptionTabButton` (border/pane look) in a `OptionsWnd::GameDataSetup` (0x4c30a0)
  detour via `OtbCreateObject`+`OtbCreateFromPattern` (the __usercall thunk); caption
  "Мышь" set from code.
- `OptionsWnd::OnWndNotify` (0x4c3560) detour: id 10006 → `EnterMouseView`
  (`SetCurTab(2)` via a this=ECX/tabId=EAX thunk to show ControlOptionsWnd + apply
  the outgoing tab's changes, then highlight Мышь / deselect Control, `SetView(mouse)`);
  a stock tab / Exit while in mouse view → `LeaveMouseView` (restore bindings view;
  re-highlight Control if it was the Control tab, since `SetCurTab(2)` early-outs and
  won't re-highlight). Button `m_tabId` @ **from+0x240** (verified in OnWndNotify).
- Header build is now guarded by the header's *existence* (`FindChild
  wndProfileHeader`) rather than a one-shot flag, so it survives any menu recreation.

Reused engine addresses (VA base 0x400000): Wnd::ShowWindow 0x41c2e0, SetCurTab
0x4c36e0 (this=ECX,tabId=EAX), OptionsWnd::GameDataSetup 0x4c30a0,
OtbCreateObject 0x4c3a40, OtbCreateFromPattern 0x4c3be0, OTB vtable 0x9d44e0,
tab pane CStrs sel 0xa45424 / unsel 0xa45430.

Reused engine addresses (VA base 0x400000): OptionsWnd::OnWndNotify 0x4c3560
(5-byte prologue, lazy build trigger only), ControlOptionsWnd::OnWndNotify 0x4b0ba0
(22-byte prologue, our widget clicks), BindKeysWnd::ApplyBindings 0x4a64e0
(__stdcall despite PDB, `ret 4`), GetChildByName 0x435700, AddChild vtbl+0x20
(0x6121d0), MoveChildToFirstPosition 0x405340, LoadDialog 0x2744f0, SetText
0x41cc20, GetBounds 0x41cbd0.
Superseded/no-longer-used: ControlOptionsWnd::GameDataSetup 0x4b03c0 (tried first —
runs at AT_APP_START, too early, caused a delayed corruption crash; see §6.2),
ShowOptionWindowForTab 0xc3720, ApplyTabChanges 0xc3910, SetCurTab 0xc36e0,
GetOptionWindowGuiIdByTabId 0xc3880, OptionsWnd::GameDataSetup 0xc30a0 — kept here
only as the reversing record from §2/§6.1's exploration of the discarded slot-swap
path.
