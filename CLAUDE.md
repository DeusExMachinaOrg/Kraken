# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Kraken is a **binary patch / mod** for the 32-bit Win32 game `hta.exe` (the "Hard Truck
Apocalypse" / "Ex Machina" engine, internally `m3d` / Miracle3D). It builds to `kraken.dll`,
which is injected into the game by patching the EXE. At load the DLL's `EntryPoint` installs a
series of in-memory patches ("fixes") over the running game code, plus a from-scratch
reimplementation of the Direct3D9 renderer.

The game executable is **native and unmodified at the source level** — Kraken does not have the
game's source. It hooks the game by writing trampolines/values at hardcoded absolute addresses
(e.g. `0x005D3137`) that correspond to a specific retail build of `hta.exe`. Those addresses are
load-bearing; they only line up with that exact binary.

## Build

- **Toolchain:** MSVC, C++20, **32-bit (x86) only**. The output must be a Win32 DLL because it is
  injected into a 32-bit process and uses 32-bit absolute addresses. Configure with an x86
  generator/preset (e.g. `-A Win32`).
- **Hard dependency:** Microsoft **DirectX SDK (June 2010)**. CMake reads `DXSDK_DIR` from the
  environment (falls back to `C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)` or
  `C:/DXSDK`). Without it, configure fails. Links `d3d9`, `d3dx9`.
- A CMake build MCP server (`cmake-build`) is available in this environment — prefer
  `mcp__cmake-build__configure` / `mcp__cmake-build__build` and `mcp__cmake-build__errors` /
  `linker_errors` over raw shell when iterating. Equivalent manual flow:
  ```
  cmake -S . -B .build -A Win32
  cmake --build .build --config Release
  ```
- There are no automated tests. Verification is done by running the patched game.

### Deploying / running the patched game (`patcher.py`)

`patcher.py` rewrites a copy of `hta.exe` in place so it loads `kraken.dll` and calls
`EntryPoint`. It hardcodes a hand-assembled `Init Kraken` thunk plus the `kraken.dll` import
string at specific file offsets, and flips the **4GB LAA** flag and a **4MB stack reserve** in the
PE header. Run it from a directory containing `hta.exe` (it does not back up automatically —
`hta.dat` is referenced as the backup name). The game reads runtime settings from
`./data/kraken.ini` (see Config below) and reimplemented shaders from `./data/shaders/*.fx`.

## Architecture

### Entry flow

`source/entry.cpp` → `EntryPoint(module)` is the injected entry point. It:
1. Inits infrastructure: `logger::Init()`, `runtime::Init()`, `impulse::Init()`.
2. Calls `ConstantHotfix()` — a batch of one-shot value pokes that point game code at fields of the
   global `Config` (resolution, gravity, fuel price, brake power, etc.) via `routines::Override`/
   `RemapPtr`.
3. Calls every `fix::<name>::Apply()` in turn. **This list is the feature registry** — to add or
   disable a feature, add/comment a line here (and add the `.cpp` to `CMakeLists.txt`). Some fixes
   are gated at runtime by a `Config` flag.

### The "fix" pattern (the core idiom)

Each feature lives in `source/fix/<name>.cpp` + `include/fix/<name>.hpp` inside namespace
`kraken::fix::<name>`, exposing a single `Apply()`. A fix typically:
- Defines a replacement function, usually `__fastcall` (so `this` arrives in `ecx`; the unused
  `edx` slot is a `void* _` second param). Calling-convention shims to match the game's ABI are
  normal here.
- Installs itself with helpers from `include/routines.hpp`, all of which `VirtualProtect` →
  patch → restore:
  - `Redirect(size, src, tar)` — overwrite `src` with a `jmp tar` trampoline (replaces a function).
  - `ChangeCall(src, tar)` / `ReplaceCall(src, tar)` — repoint an existing `call`, or write a new one.
  - `Override(size, src, data)` / `OverrideValue(addr, value)` — splice raw bytes/values into code.
  - `RemapPtr(src, tar)` — overwrite a pointer operand.
  - `Patch(addr, data, size)` / `Nop(addr, size)` — generic byte patch / NOP-fill.
- References game functions/data by casting hardcoded addresses to function-pointer types, e.g.
  `static auto F = (char(__fastcall*)(...))(0x006A9D60);`. `autobrakefix.cpp` is a clean,
  representative example.

When working in a fix, keep the absolute addresses and the calling convention exactly as the game
expects — a wrong convention or stack-cleanup is a silent crash, not a compile error.

### `extern/hta` — engine type bindings (git submodule)

Submodule of the `DeusExMachinaOrg/Headers` repo, namespace `hta::` (with `hta::ai`, `hta::m3d`,
`hta::m3d::rend`, `hta::geom2d`, etc.; ~960 headers). These are reverse-engineered declarations of
the game's own classes/structs (e.g. `hta::ai::Vehicle`, `hta::m3d::Application`,
`hta::m3d::rend::IRenderer`) with fields laid out to match the binary's memory layout and methods
declared at their real addresses. Including a header lets Kraken code treat live game objects as
typed C++. `infos/meta.json` maps mangled symbol → calling convention → address for the target
build. The submodule is bumped frequently ("Bump hta" commits) as more of the engine is mapped;
treat it as the source of truth for layouts and update it there, not with ad-hoc local structs.

### `source/render` — reimplemented D3D9 renderer

This is the one large piece that is **rewritten rather than trampolined**. `fix/render.cpp`
redirects `m3d::Application::createRenderer` so the game instantiates `kraken::render::CDevice`
(in `source/render/CDevice.cpp`) — a full implementation of the engine's `IRenderer` interface —
instead of the stock renderer. Because of this, **rendering bugs almost always live in
`CDevice`/`source/render`, not in the game binary.** `source/render/native/` holds supporting
pieces (`Image`, `Uniform`, `Debug`, shared D3D state). The renderer loads HLSL effects from
`data/shaders/*.fx`.

#### Shaders

Three shader kinds, three load paths in `CDevice`:
- **`.fx` D3DX effects** → `EffectImpl::LoadFromFile` → `D3DXCreateEffect` (CDevice.cpp ~544).
- **Raw `.ps`/`.vs` HLSL** (entry + profile supplied by engine call site) → `HlslShaderImpl::LoadFromFile`
  → `D3DXCompileShader` (~1148).
- **`.asm`/`.ps`/`.vs` assembly** → `AsmShaderImpl::LoadFromFile` → `D3DXAssembleShader` (~1333).

The stock engine shaders are `ps_1_x` / `vs_1_x`, but the **d3dx9 (June 2010) HLSL compiler cannot
target `ps_1_x` at all** (`error X3539`). So the HLSL paths **force-upgrade** every pixel profile to
`ps_2_0`/`ps_3_0` (CDevice.cpp ~1117) and compile with `D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY`
(`/Gec`) to preserve `ps_1_x` semantics. Assembly is *not* upgraded — `ps_1_x` asm still assembles and
runs.

Migrating a shader to **native `ps_2_0`** (the current direction — `/Gec` is a crutch, not a fix) means
restoring what `ps_1_x` provided implicitly:
- **Pin sampler registers** — `sampler S : register(sN)` in declaration order. The `DECLARE_*_SAMPLER`
  macros in `libsamplers.fx` don't pin, so a native `ps_2_0` compile lets the optimizer drop *unused*
  samplers and shift the rest down (e.g. `landscapefp_ps11.ps`), making the engine's per-stage
  `SetTexture(n)` feed the wrong sampler (the "blend mask renders as color" / yellow-leak bug). `.fx`
  effects are insulated by the custom `StateManager` (texture→stage), but raw `.ps`/`.vs` bind by stage
  index and **must** be pinned. **Verify emitted registers with `fxc /T ps_2_0 /Gec /E <entry> /Fc`
  before deploying.**
- **Restore clamping** with explicit `saturate()` where additive/`*N` accumulation relied on the old
  implicit `[0,1]`/`[-1,1]` clamp; keep `oFog`/`: FOG` for vertex fog.

`ReloadShaders()` (CDevice.cpp ~10447) hot-reloads all registered shaders from disk — invaluable for
iterating on shader edits without relaunching.

Shaders live in the **game's** `data/shaders/` (the Steam install, where the game loads them); the repo
`data/shaders/` is the staging package, and `.build/stock_shaders/` is the pristine backup. Two `fxc`
are available for offline checks: WinSDK 10 (`d3dcompiler_47`, no SM1) and DirectX SDK June 2010
(`…/Utilities/bin/x86/fxc.exe`, matches the renderer's `d3dx9_43`). `lib.fx` holds shared helpers
(`bx2`, `pow16`, `VertexFog`, `REAL` types); `libsamplers.fx` the sampler macros.

### `source/ext` — runtime infrastructure

- `logger` — leveled logging (`LOG_DEBUG`..`LOG_PANIC`). Each `.cpp` `#define LOGGER "tag"` before
  including the header to tag its lines; level threshold comes from `Config::log_debug`.
- `runtime` — wraps the game's Lua `ScriptServer`. `OnInit`/`OnLoad` register callbacks that fire
  when the script VM initializes/loads; `GetRuntime()` returns the live `lua_State*`. Lua-facing
  features (`fix/luabinds.cpp`) hang off this.
- `impulse` — input/event ("impulse") hooks.

### Config

`include/config.hpp` + `source/config.cpp`. A single global `kraken::Config` (`Config::Instance()`)
backed by the INI at `./data/kraken.ini` via Win32 `GetPrivateProfileString`/`WritePrivateProfileString`.
Each setting is a templated `ConfigValue<T>` carrying `{section, key, value, limited, min, max}`;
`Load()` reads + clamps, `Dump()` writes back (so a missing INI is recreated with defaults, and
out-of-range values are corrected on next run). Specializations exist for `std::vector<std::string>`
(indexed `Script_1`, `Script_2`…), a `WareUnits` list, and a string→uint override map. To add a
setting: declare the field, initialize it in the `Config` ctor, and add matching `LoadValue`/
`DumpValue` lines.

## Conventions

- Comments and TODOs are a mix of English and Russian — both are fine; match the surrounding file.
- New game-code offsets, when discovered, belong in the `extern/hta` headers (as typed
  declarations) rather than scattered as raw casts, where practical.
