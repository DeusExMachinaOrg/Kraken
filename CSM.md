# Cascaded Shadow Maps (CSM) — Implementation & Status

> **Status: WIP — not working yet.** The depth-generation half exists; the
> sample/apply half does not. The latest fit rewrite ("draw-disc camera-centered")
> is **unverified in-game** (see [Current problem](#current-problem)).

CSM is a from-scratch, depth-map-based sun shadow system layered onto the
reimplemented D3D9 renderer. It runs **alongside** the game's native shadows
(which are a different technique — see [Native reference](#native-engine-reference)),
and currently produces only offscreen depth maps + debug thumbnails.

---

## Where it lives & how it's gated

CSM depth-gen runs inside `Landscape_Render` — our reimplementation of the game's
`m3d::Landscape::Render`, installed via `routines::ChangeCall` at the
`CWorld::Render` call site (`0x005C7742`).

- File: [`source/fix/hbao.cpp`](source/fix/hbao.cpp)
- Two gates, **both required**:
  - `hbao=1` — installs the `Landscape::Render` reimpl at all (`Apply()`).
  - `shadow_csm=1` — enables the CSM block inside it.
- Runs each frame **after** the opaque scene pass, before grass/transparents.

---

## Architecture (two halves)

### 1. Per-cascade draw loop — `source/fix/hbao.cpp`

For each cascade the loop:
1. reads camera pos (`r->MatGetOrgInv()`), sun dir (`w->m_sunDir`), ground height
   under camera (`Landscape::GetHeight`, engine-sanctioned);
2. builds a sun **view** + **ortho proj** (see [Fit](#cascade-fit));
3. `dev->CsmBeginCascade(i)` — bind cascade `i`'s target;
4. sets the sun camera on the renderer stacks:
   - `dev->MatSet(view)` — the Mat stack, which the terrain shader reads as
     `mViewProj = MatGet()*MatGetProj()`,
   - `r->MatSetProj(proj)`,
   - `r->SetViewMatrix(view)` — for `ViewPos`/clip planes,
5. `L->DrawSolidLandscape(LRM_DIRECT, 0)` — native terrain draw;
6. `dev->CsmEndCascade()` — restore.

Wrapped in `PushBlend(BM_NONE)/PushZbState(ZB_ENABLE)/PushFog(false)/PushCull(NONE)`
and `D3DPERF` markers (`"CSM gen"` > `"CSM cascade N"`) for RenderDoc.

### 2. Resources & target binding — `source/render/CDevice.cpp` / `.hpp`

Fields ([`CDevice.hpp` ~L696](source/render/CDevice.hpp)):
```cpp
static constexpr int CSM_CASCADES = 3;
IDirect3DTexture9* m_csmDepthTex[CSM_CASCADES];   // sampleable INTZ depth (the shadow data)
IDirect3DSurface9* m_csmDepthSurf[CSM_CASCADES];
IDirect3DTexture9* m_csmColorTex[CSM_CASCADES];   // per-cascade color RT (depth-only pass still needs one)
IDirect3DSurface9* m_csmColorSurf[CSM_CASCADES];
```

- `EnsureCsm(res)` — lazily creates per-cascade INTZ depth + color RTs (gated on
  INTZ support via `CheckDeviceFormat`). Min res 512; config default 1536.
- `CsmBeginCascade(i)` / `CsmEndCascade()` — bind/restore following the native
  `RenderToTexStart` discipline:
  - **unbind the depth-stencil before swapping the color RT** (D3D9 requires
    `DS >= RT` in both dims; a 1536² cascade over a smaller backbuffer depth made
    the raw `SetRenderTarget` fail with `D3DERR_INVALIDCALL`), same in reverse on end;
  - viewport via `CDevice::SetViewport` so `m_curViewportD3D` tracks the cascade res;
  - HRESULT checks that log on failure;
  - **DEBUG:** forces color-write on + a magenta `Clear` so an empty cascade is obvious.

---

## Cascade fit

**Principle: optimize on what the engine already clips, don't rebuild a frustum.**

`DrawSolidLandscape` draws a **camera-centered disc** of terrain cells of radius
`m_drawRadius * 24` (VISCELL_EDGE_LENGTH). Verified in the retail decompile:
`SortedCellsFetch` (`0x203a00`) just walks radius rings `0..m_drawRadius+1` with no
per-view frustum test, and `SortedCellsPrepare` (`0x234ec0`) builds the grid around
`MatGetOrgInv()` (camera pos). So the drawn set is camera-centered and independent
of whatever view/proj we set.

Therefore each cascade is a **camera-centered nested square** sized to that disc:

```
drawDisc     = m_drawRadius * 24
groundCenter = (camPos.x, groundY, camPos.z)
half[i]      = drawDisc * (i+1) / CSM_CASCADES        // nested: R/3, 2R/3, R
eye          = groundCenter + sunDir * (half + depthRange)
view         = lookAtLH(eye, groundCenter, up=(0,0,1))
proj         = orthoLH(2*half, 2*half, 0, 2*half + 2*depthRange)
```

Cascade 0 is the tightest square around the camera → the close chunk is in it by
construction; cascade 2 spans the whole disc.

**Dropped vs older attempts:** PSSM `split[]`, `shadow_csm_split_lambda`,
`shadow_csm_distance` (extent is now the draw radius), the `tanHalf`/`znCam` frustum
reconstruction, `MatGetBasis`, the 8-corner bounding-sphere fit, and the
"frustum-wedge-on-ground" fit — that last one is what dropped the close chunk.

---

## Debug visualization

`RenderCsmDebug()` ([`CDevice.cpp` ~L6706](source/render/CDevice.cpp)) blits each
cascade's **own** color RT (`m_csmColorTex[i]`) as three bottom-left thumbnails,
drawn last over the finished scene. Gated by `shadow_csm`.

- Solid **magenta** thumbnail = terrain did **not** rasterize (only the clear).
- Terrain-colored thumbnail = the cascade captured geometry.
- The live game world visible *above* the strip is **not** cascade content.

---

## Config (`[render]` in `data/kraken.ini`)

| Key | Default | Used? | Meaning |
|---|---|---|---|
| `shadow_csm` | 1 | ✅ | master enable (needs `hbao=1` too) |
| `shadow_csm_resolution` | 1536 | ✅ | per-cascade square depth res |
| `shadow_csm_depth_range` | 2000 | ✅ | along-sun depth slab (headroom each side) |
| `shadow_csm_distance` | 500 | ❌ | **unused** — extent is now the draw disc |
| `shadow_csm_split_lambda` | 0.7 | ❌ | **unused** — no PSSM split anymore |

Declared in [`include/config.hpp`](include/config.hpp) / [`source/config.cpp`](source/config.cpp).

---

## Native engine reference

Reverse-engineered from `hta.exe` + `game.pdb` (retail, base `0x400000`,
`RVA = VA - 0x400000`). `game.pdb` is fully symboled for game/scene/landscape code.

- **Offscreen render is always via `IRenderer::RenderToTexStart`/`RenderToTexFinish`.**
  It saves RT/DS/viewport, binds a size-matched depth, pushes the Mat/Proj stacks,
  and sets the viewport through the renderer. Our `CsmBeginCascade` mirrors this
  discipline but pokes the device (our CSM textures are raw handles, not engine
  `TexHandle`s from `mActiveTextures`).
- **`SceneGraph::DrawShadowsToTexture` (`0x4a7b00`) is NOT a CSM.** It renders shadow
  *casters* into a **color silhouette atlas** (`RenderToTexStart(..., wantDepth=0)`,
  `LightSwitchOffAllLights` + full ambient → flat white) and projects it onto the
  ground via `PutShadowTextureToLandscapeAndRoad` (`0x4a7180`) /
  `PutShadowTextureToGrass` (`0x4a2ee0`). **Terrain is a receiver, never a caster.**
  So there is no native CSM to copy — only the RenderToTex + push/pop-state envelope.
- **`Landscape::DrawSolidLandscape` (`0x3a77b0`)** sets HLSL `mViewProj =
  MatGet()*MatGetProj()` and `ViewPos = MatGet().getOrgInv()` — Mat/Proj stacks only,
  `SetViewMatrix` is irrelevant to terrain. Draws the camera-sorted cell disc.
- Matrix helpers (verified): `CMatrix::lookAtLH` `0x405B90`, `orthoLH` `0x8A1DA0`,
  `perspectiveFovLH` `0x41D7F0`, `Landscape::GetHeight` `0x5AEA30`.

---

## Current problem

**The fix isn't working, and CSM shows no shadows.** Broken down:

### A. Immediate — the fit rewrite is unverified
The `pwsh.exe` post-build **deploy step fails**, so `.build/Release/kraken.dll` is
*not* auto-copied to the Steam folder. **The most likely reason "the fix doesn't
work" is that the game is still running the OLD DLL.** This must be ruled out first:
manually copy the DLL, then re-check.

### B. RESOLVED — the per-cell visibility gate (this was the "won't draw" root cause)
`DrawSolidLandscape` skips every cell that fails a camera-visibility gate: at
**`0x7A86D4`** `je` (`if (vis == 0) skip draw`), where
`vis = m_enableMap[cellY*256+cellX] & m_enableVisSpaceMask`. `m_enableMap`
(`SceneGraph+0x3104b0`, 65536 bytes) is written by
`SceneGraph::EnableVisibleCells(frustum, space)` from `Landscape::UpdateVis`
(`0x3a60b0`) — space 1 = direct **camera frustum**, space 2 = reflection.
`SortedCellsPrepare` builds the full camera-centered disc, but this gate trims the
*draw* to the camera-frustum **wedge** — so the sun cascades only ever got the wedge
(or nothing, when it fell outside our box), and the under-camera close chunk (below
the frustum) was skipped in every pass.
**Fix applied (hbao.cpp):** save the 6 bytes at `0x7A86D4`, `routines::Nop` them for
the duration of the cascade `DrawSolidLandscape` calls, restore right after — "guarantee
the fetch, ignore the flag." Main/reflection passes ran earlier so they keep their
culling. The full prepared disc now rasterizes into every cascade. **Pending in-game
verification.**

### C. Structural — no consumer
`m_csmDepthTex` is generated but **never sampled**. There is no pass that projects the
cascades back onto the lit scene, so CSM produces **zero visible shadows** regardless
of depth-map quality. What ships on screen is still the native projected-silhouette
shadows.

### D. Latent — depth collapse
`shadow_csm_depth_range = 2000` makes the ortho far plane `2*half + 2*depthRange ≈
4576`, but real terrain relief inside a cascade is tens of units — so all terrain lands
in a razor-thin band of `[0,1]`. The future sampler's depth compares will be
bias-finicky and low-contrast until this slab is tightened to the real caster span.

### E. Secondary (later)
- **3× terrain redraw** — nested squares fully overlap and we submit the whole cell
  set into every cascade.
- **Oblique sun** — a `half = drawDisc` square covers the disc for an overhead sun; a
  low sun projects it to a longer ellipse and edges clip.

---

## Diagnostic plan / next steps

1. **Unblock testing.** Fix the `pwsh` post-build (use `powershell` / `robocopy`) or
   copy `.build/Release/kraken.dll` to the game folder manually. Confirm the new DLL
   is loaded (bump a log line).
2. **Read the three thumbnails.** They now answer two things at once:
   - close chunk present at center of cascade 0? → fit works;
   - disc vs wedge (is the captured region round or a forward fan)?
3. **Check the `CSM fit`/`CSM gen` log lines** for sane `m_drawRadius`, `drawDisc`,
   `groundY`, `half`.
4. Once generation is confirmed: write the **sample/apply pass** (cascade select +
   depth compare + PCF), then tighten `depthRange`.

---

## Build & deploy

```
cmake -S . -B .build -A Win32
cmake --build .build --config Release
```
- cmake-build MCP is wired to `.build` (the default `build` dir has a stale cache).
- Deploy: copy `.build/<Config>/kraken.dll` (+ `.pdb`) to
  `C:\Program Files (x86)\Steam\steamapps\common\Hard Truck Apocalypse\`.
  **The `pwsh` auto-deploy fails (pwsh.exe not installed) — copy manually for now.**
- `ReloadShaders()` hot-reloads `data/shaders/*.fx` without relaunching (renderer only).

---

## Key addresses (retail `hta.exe` + `game.pdb`)

| Symbol | RVA | VA |
|---|---|---|
| `Landscape::Render` (we reimplement) | `0x1AAFF0` | `0x5AAFF0` |
| `CWorld::Render` call site (hook) | — | `0x5C7742` |
| `Landscape::DrawSolidLandscape` | `0x3A77B0` | `0x7A77B0` |
| `SceneGraph::DrawShadowsToTexture` | `0x4A7B00` | `0x8A7B00` |
| `SceneGraph::PutShadowTextureToLandscapeAndRoad` | `0x4A7180` | `0x8A7180` |
| `SceneGraph::SortedCellsFetch` | `0x203A00` | `0x603A00` |
| `SceneGraph::SortedCellsPrepare` | `0x234EC0` | `0x634EC0` |
| `Landscape::GetHeight` | `0x1AEA30` | `0x5AEA30` |
| `CMatrix::lookAtLH` | `0x005B90` | `0x405B90` |
| `CMatrix::orthoLH` | `0x4A1DA0` | `0x8A1DA0` |
