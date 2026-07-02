#define LOGGER "hbao"

#include "config.hpp"
#include "ext/logger.hpp"
#include "fix/hbao.hpp"
#include "routines.hpp"

#include "render/CDevice.hpp"

#include "hta/CClipper.hpp"
#include "hta/CMatrix.hpp"
#include "hta/CPlane.hpp"
#include "hta/m3d/Application.hpp"
#include "hta/m3d/CVar.hpp"
#include "hta/m3d/CWorld.hpp"
#include "hta/m3d/EngineConfig.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/SceneGraph.hpp"
#include "hta/m3d/SgNode.hpp"

// Reimplementation of m3d::Landscape::Render (0x005AAFF0) so we own the scene-render orchestration
// and can inject the screen-space AO pass at the exact opaque->transparent seam (per-sub-draw hooks
// are unreliable: DrawWaterLayer/RenderGrass are individually conditional). The native sub-draws stay
// native (called by address via the hta bindings); we only reconstruct the sequencing + render-state
// setup. Transcribed 1:1 from the retail IDA decompile (game.pdb).
//
// We ChangeCall the *call site* in CWorld::Render (0x005C7742) rather than overwriting the function
// entry, so native Landscape::Render stays intact and callable -> hbao=0 runs native, hbao=1 runs
// this reimpl (A/B). Pure __rdtsc profiler bookkeeping is omitted (no pixel effect).

namespace hta::m3d {
    using LRM = Landscape::LandRenderMode;
    using VIS = Landscape::VisibilityMode;
}

namespace kraken::fix::hbao {
    using namespace hta;
    using namespace hta::m3d;
    using namespace hta::m3d::rend;

    constexpr uintptr_t kLandscapeRenderCall = 0x005C7742;  // `call Landscape::Render` inside CWorld::Render

    // Native helpers not present on the reconstructed CMatrix (called by address).
    static void(__thiscall* CMatrix_reflect)(CMatrix*, const CPlane*) =
        reinterpret_cast<void(__thiscall*)(CMatrix*, const CPlane*)>(0x007A45A0);
    static void(__thiscall* CMatrix_perspectiveFovLH)(CMatrix*, float, float, float, float) =
        reinterpret_cast<void(__thiscall*)(CMatrix*, float, float, float, float)>(0x0041D7F0);
    static void(__thiscall* CMatrix_orthoLH)(CMatrix*, float, float, float, float) =
        reinterpret_cast<void(__thiscall*)(CMatrix*, float, float, float, float)>(0x008A1DA0);
    static void(__thiscall* CMatrix_lookAtLH)(CMatrix*, const CVector*, const CVector*, const CVector*) =
        reinterpret_cast<void(__thiscall*)(CMatrix*, const CVector*, const CVector*, const CVector*)>(0x00405B90);
    // Terrain height at world (x, z); returns a large-negative sentinel where there's no terrain.
    static float(__thiscall* Landscape_GetHeight)(Landscape*, float, float, int, bool) =
        reinterpret_cast<float(__thiscall*)(Landscape*, float, float, int, bool)>(0x005AEA30);

    static inline bool  cb(const CVar& c) { return c.m_type == CVar::CVAR_BOOL ? c.m_b : c.m_i > 0; }
    static inline float cf(const CVar& c) { return c.m_type == CVar::CVAR_FLOAT ? c.m_f : (float) c.m_i; }

    static void __fastcall Landscape_Render(void* self, void* /*edx*/) {
        Landscape*    L   = reinterpret_cast<Landscape*>(self);
        IRenderer*    r   = kraken::render::CDevice::Instance();  // == Application::g_pApp->m_renderer
        EngineConfig& cfg = Kernel::Instance()->GetEngineCfg();
        CWorld*       w   = L->m_owner;

        // --- setup -----------------------------------------------------------------------------
        r->PushFillMode(cb(cfg.m_lsWireframe) ? M3DFILL_WIREFRAME : M3DFILL_SOLID);

        const int waterQ = cfg.m_r_waterQuality.m_i;

        // water plane: normal (0,1,0), dist = water level
        const float waterlevel = w->m_level->waterlevel;
        L->m_waterPlane.m_normal = CVector(0.0f, 1.0f, 0.0f);
        L->m_waterPlane.m_dist   = waterlevel;

        L->m_bindDevider = (1.1f - cf(cfg.m_lsViewDistanceDivider)) * 0.44999999f + 0.55000001f;

        int v10 = (int) (cf(cfg.m_lsViewDistanceDivider) * 8.0f + 4.0f);
        if (v10 < 4)
            v10 = 4;
        else if (v10 > 12)
            v10 = 12;

        r->SetLighting(false, false);
        r->PushZFunc(M3DCMP_LESS);
        r->PushBlend(BM_NONE);
        r->DuplicateCull();
        r->DuplicateZbState();
        r->PushFog(cb(cfg.m_r_enableFog));

        // === water reflection pass (only when water is visible) ================================
        if (L->m_numWaterCells != 0 && L->m_isWaterVisible) {
            const bool reflTerrain = cb(cfg.m_g_drawReflectedTerrain);
            const bool reflModels  = cb(cfg.m_g_drawReflectedModels);

            if (L->m_dirtyReflection) {
                L->m_dirtyReflection = false;
                L->m_curVisMode      = VIS::VIS_REFLECTION;

                r->RenderToTexStart(L->m_texRtReflection, true);
                r->ClearViewport(M3DCLEAR_CZ, w->GetWeatherFogColor());

                CMatrix matReflect;
                CMatrix_reflect(&matReflect, &L->m_waterPlane);

                CMatrix saveView = r->GetViewMatrix();
                CMatrix saveProj = r->MatGetProj();

                r->MatPush(matReflect);

                // reflected view = reflect-then-view (row-vector D3D: reflect is applied first).
                CMatrix vv = matReflect * saveView;
                r->SetViewMatrix(vv);

                const float fovY   = 2.0f * (float) atan2(tan(0.3926990926265717) * 1.1, 1.0);
                const float aspect = (float) cfg.m_r_width.m_i / (float) cfg.m_r_height.m_i;
                CMatrix     proj;
                CMatrix_perspectiveFovLH(&proj, fovY, aspect, 1.0f, 5000.0f);
                r->MatSetProj(proj);
                r->SetFog(false, false);

                if (cb(cfg.m_g_drawSky)) {
                    r->PushZbState(ZB_DISABLE);
                    w->RenderSky(LRM::LRM_REFLECTION);
                    r->PopZbState();
                }
                if (cb(cfg.m_lgtFlares))
                    L->m_flares.Render(FLARE_SUN, w->m_sunDir, 1.0f, 1.0f);

                r->SetFog(cb(cfg.m_r_enableFog), false);
                r->PushZbState(ZB_ENABLE);

                if (L->m_waterShaderVersion != 11 && (reflTerrain || reflModels) && (waterQ == 3 || waterQ == 2)) {
                    const float savedDivider = cf(cfg.m_lsViewDistanceDivider);
                    const int   savedRadius  = L->m_drawRadius;

                    const float distMod = cf(cfg.m_g_reflectionDrawDistModifier);
                    const float reflDiv = savedDivider / distMod;
                    L->m_drawRadius = (int) (reflDiv * 8.0f + 4.0f);
                    cfg.m_lsViewDistanceDivider.SetF(reflDiv, false);

                    Weather* cw = w->m_weatherManager.m_currentWeather;
                    const float waterH = (cw->m_waterHeightSmall + cw->m_waterHeightBig) * 2.0f;

                    // horizontal clip plane (normal up) -- avoid CPlane's native ctor via raw storage
                    alignas(CPlane) unsigned char clipBuf[sizeof(CPlane)] = {};
                    CPlane& clip  = *reinterpret_cast<CPlane*>(clipBuf);
                    clip.m_normal = CVector(0.0f, 1.0f, 0.0f);
                    clip.m_dist   = (w->m_level->waterlevel - waterH) - 0.5f;

                    if (r->GetMaxClipPlanes()) {
                        r->SetClipPlane(0, clip);
                        r->EnableClipPlane(0, true);
                    }
                    w->m_sceneGraph.m_enableVisSpaceMask = 2;
                    r->SetCull(M3DCULL_CW, false);
                    if (reflTerrain)
                        L->DrawSolidLandscape(LRM::LRM_REFLECTION, 1);

                    clip.m_normal = CVector(0.0f, 1.0f, 0.0f);
                    clip.m_dist   = w->m_level->waterlevel;
                    if (r->GetMaxClipPlanes()) {
                        r->SetClipPlane(0, clip);
                        r->EnableClipPlane(0, true);
                    }
                    r->SetCull(M3DCULL_CW, false);
                    if (reflModels && waterQ != 2) {
                        w->m_sceneGraph.UpdateVis(false, L->m_reflectedFrustum, false);
                        w->m_sceneGraph.Render(SGRF_LOW_DETAIL);
                    }
                    if (r->GetMaxClipPlanes())
                        r->EnableClipPlane(0, false);

                    cfg.m_lsViewDistanceDivider.SetF(savedDivider, false);
                    L->m_drawRadius = savedRadius;
                }

                r->RenderToTexFinish();
                r->PopZbState();
                r->MatPop(false);
                r->SetViewMatrix(saveView);
                r->MatSetProj(saveProj);
                w->m_sceneGraph.m_enableVisSpaceMask = 1;
            }
        }

        // === main (direct) pass ================================================================
        L->m_curVisMode = VIS::VIS_DIRECT;
        r->SetFog(false, false);
        r->SetBlend(BM_NONE, false);
        r->SetZbState(ZB_DISABLE, false);

        if (cb(cfg.m_g_drawSky)) {
            static int frame = 0;
            if ((frame & 1) == 0)
                w->UpdateSkyParams();
            ++frame;
            w->RenderSky(LRM::LRM_DIRECT);
        }
        if (cb(cfg.m_lgtFlares))
            L->m_flares.Render(FLARE_SUN, w->m_sunDir, 1.0f, 1.0f);

        // landscape LOD clip rings
        const float edge = 24.0f;  // VISCELL_EDGE_LENGTH_24
        const float z1   = (float) (v10 - 4) * edge;
        const float z0   = (float) (v10 - 8) * edge;
        const float z2   = (float) v10 * edge;
        L->m_landscapeClip0z   = z0;
        L->m_landscapeClip1zSq = z1 * z1;
        L->m_drawRadius        = v10;
        L->m_landscapeClip0    = v10 - 8;
        L->m_landscapeClip1    = v10 - 4;
        L->m_landscapeClip2    = v10;
        L->m_landscapeClip1z   = z1;
        L->m_landscapeClip2z   = z2;
        L->m_landscapeClip0zSq = z0 * z0;
        L->m_landscapeClip2zSq = z2 * z2;

        if (cb(cfg.m_r_renderZGuard))
            L->renderZGuard();

        r->SetFog(cb(cfg.m_r_enableFog), false);
        r->PushZbState(ZB_ENABLE);
        r->PushZFunc(M3DCMP_LESS);
        r->PushBlend(BM_NONE);
        w->m_sceneGraph.m_enableVisSpaceMask = 1;
        r->SetCull(M3DCULL_CW, false);

        // ---- opaque terrain + objects ----
        L->DrawLandScapeTextures(VIS::VIS_DIRECT, false, false);
        r->SetZbState(ZB_ENABLE, false);
        r->SetZFunc(M3DCMP_LESS, false);
        r->SetCull(M3DCULL_CCW, false);
        L->DrawSolidLandscape(LRM::LRM_DIRECT, 0);
        r->SetZbState(ZB_NOWRITE, false);
        r->SetBlend(BM_ALPHA, false);
        L->DrawSolidLandscape(LRM::LRM_BIND, 0);
        r->SetZbState(ZB_ENABLE, false);
        L->RenderRoads();
        r->PopZbState();
        r->PopZFunc();
        r->PopBlend();

        L->DrawCollisionGeoms(true);

        r->SetFog(cb(cfg.m_r_enableFog), false);
        r->SetCull(M3DCULL_CCW, false);
        r->SetZbState(ZB_ENABLE, false);
        w->m_sceneGraph.UpdateVis(false, L->m_frustumCull, true);
        w->m_sceneGraph.Render(SGRF_DEFAULT_OPAQUE);

        if (cb(cfg.m_r_waterInQuery)) {
            if (L->m_numWaterCells)
                L->QueryWaterVisibility();
        } else {
            L->m_isWaterVisible = true;
        }

        r->SetFog(false, false);
        if (Config::Instance().shadow_native.value)   // shadow_native=0 suppresses the native (top-down projected)
            w->m_sceneGraph.Render(SGRF_SHADOWS);      // shadows so only CSM shows -- and avoids the game's m_dsShadows
        r->SetFog(cb(cfg.m_r_enableFog), false);       // menu path (that path crashes in CopyRenderTargetToTexture)

        // --- Cascaded shadow maps (depth-based, WIP) -------------------------------------------
        // Render caster depth from the sun POV into per-cascade INTZ maps, alongside the native
        // shadow gen above (kept until the sampling is swapped). Terrain only to start; objects follow.
        // Sun ortho per cascade is centered on the camera with growing half-extents; matrices are
        // saved/restored around the loop, and CsmBegin/EndCascade save/restore the RT/DS/viewport.
        if (Config::Instance().shadow_csm.value) {
            kraken::render::CDevice* dev = kraken::render::CDevice::Instance();
            if (dev->EnsureCsm((int) Config::Instance().shadow_csm_resolution.value)) {
                const CVector camPos   = r->MatGetOrgInv();  // reads the Mat stack -> camera world pos
                const CVector sunDir   = w->m_sunDir;

                // Ground height under the camera. The engine's own SceneGraph::DrawShadowsToTexture calls
                // Landscape::GetHeight (verified in the retail decompile), so this is the sanctioned way to
                // find the terrain. A sun-OBLIQUE cascade must sit its box on the ground (not the airborne
                // camera) or the vertical camera->ground gap shifts the terrain off the map.
                float groundY = Landscape_GetHeight(L, camPos.x, camPos.z, -1, true);  // engine's args: (-1, 1)
                if (groundY < -50000.0f)
                    groundY = w->m_level->waterlevel;
                const CMatrix saveView = r->GetViewMatrix();
                const CMatrix saveProj = r->MatGetProj();
                const CMatrix saveMat  = dev->MatGet();  // Mat-stack top = the view the LANDSCAPE reads
                                                         // (mViewProj = MatGet*MatGetProj); SetViewMatrix
                                                         // alone never reaches it.
                // Capture the SCENE camera view*proj for the apply pass; its inverse reconstructs world pos
                // from the scene INTZ depth. saveMat*saveProj == the mViewProj the main opaque pass used.
                dev->SetCsmScene(saveMat * saveProj);
                const CVector up(0.0f, 0.0f, 1.0f);  // sun ~overhead; world-Z avoids a degenerate look-up

                // Cascades are camera-centered squares (per-cascade half-extents below), all rendered from
                // the full camera-prepared cell disc. The apply pass later picks, per pixel, the smallest
                // cascade that contains it.
                // #4: bind the shadow center to the camera ORIGIN + HEIGHT (camPos), not the ground under it,
                // so the shadow volume tracks the camera in all three axes. depthRange gives the vertical
                // headroom down to the terrain/casters below (raise shadow_csm_depth_range for a high camera).
                const CVector camCenter(camPos.x, camPos.y, camPos.z);
                // Per-cascade far distances (world units): each cascade i is a camera-centered square of
                // half-extent csmDist[i], so shadow_csm_dist0/1/2 directly place the LOD transitions.
                // Each is capped at the drawn-terrain edge (no terrain drawn past it to shadow) and kept
                // monotonic (lod0 <= lod1 <= lod2). terrainDisc = (m_drawRadius+1)*128: draw-cell world size
                // is 128 (landscapeSolid_ps11.vs: pos = cellX*128 + land_scale*idx), m_drawRadius counts
                // those 128u cells. Smaller csmDist[i] = fewer world units per texel = crisper that cascade.
                const float terrainDisc = ((float) L->m_drawRadius + 1.0f) * 128.0f;
                float       csmDist[kraken::render::CDevice::CSM_CASCADES] = {
                    Config::Instance().shadow_csm_dist0.value,
                    Config::Instance().shadow_csm_dist1.value,
                    Config::Instance().shadow_csm_dist2.value,
                };
                for (int c = 1; c < kraken::render::CDevice::CSM_CASCADES; ++c)
                    if (csmDist[c] < csmDist[c - 1]) csmDist[c] = csmDist[c - 1];   // enforce monotonic
                for (int c = 0; c < kraken::render::CDevice::CSM_CASCADES; ++c)
                    if (csmDist[c] > terrainDisc) csmDist[c] = terrainDisc;         // cap at terrain edge

                auto vadd = [](const CVector& a, const CVector& b, float s) {
                    return CVector(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s);
                };

                D3DPERF_BeginEvent(D3DCOLOR_ARGB(255, 0, 255, 0), L"CSM gen");  // RenderDoc marker
                r->PushBlend(BM_NONE);
                r->PushZbState(ZB_ENABLE);
                r->PushFog(false);
                r->PushCull(M3DCULL_NONE);  // sun view flips winding; depth-only wants both faces
                static const wchar_t* kCsmMk[] = { L"CSM cascade 0", L"CSM cascade 1", L"CSM cascade 2", L"CSM cascade 3+" };

                // Draw the FULL prepared disc into every cascade, not just the camera-frustum wedge.
                // DrawSolidLandscape's per-cell `if (vis)` reads m_enableMap[cell] & m_enableVisSpaceMask,
                // which EnableVisibleCells filled from the CAMERA frustum -- so it would draw only the wedge.
                // We control the DATA feeding that test instead of the code: flag every cell enabled for the
                // duration of the cascade draws, then restore. The map is writable game memory (no code
                // patching), and the main/reflection passes already ran this frame with real culling.
                SceneGraph&          sg = w->m_sceneGraph;
                static unsigned char s_savedEnableMap[sizeof(sg.m_enableMap)];
                memcpy(s_savedEnableMap, sg.m_enableMap, sizeof(s_savedEnableMap));
                memset(sg.m_enableMap, 0xFF, sizeof(sg.m_enableMap));  // every fetched (disc) cell now passes `if (vis)`

                for (int i = 0; i < kraken::render::CDevice::CSM_CASCADES; ++i) {
                    D3DPERF_BeginEvent(D3DCOLOR_ARGB(255, 255, 210, 0), kCsmMk[i < 3 ? i : 3]);
                    // Cascade i = camera-centered square of half-extent csmDist[i] (shadow_csm_dist<i>),
                    // centered on the ground under the camera so the close chunk lands in cascade 0. near=0
                    // with depthRange headroom toward the sun for casters above the ground.
                    const float depthRange = Config::Instance().shadow_csm_depth_range.value;
                    const float half = csmDist[i];

                    const CVector eye = vadd(camCenter, sunDir, half + depthRange);
                    CMatrix view, proj;
                    CMatrix_lookAtLH(&view, &eye, &camCenter, &up);
                    CMatrix_orthoLH(&proj, 2.0f * half, 2.0f * half, 0.0f, 2.0f * half + 2.0f * depthRange);
                    dev->SetCsmCascadeVP(i, view * proj);  // world -> cascade i clip, for the screen-space apply pass

                    static int s_c = 0;
                    if ((s_c++ & 63) == 0)
                        LOG_INFO("CSM fit c%d: cam %.0f %.0f %.0f  groundY %.0f  half %.0f  terrainDisc %.0f",
                                 i, camPos.x, camPos.y, camPos.z, groundY, half, terrainDisc);

                    dev->CsmBeginCascade(i);
                    dev->MatSet(view);        // the landscape's mViewProj = MatGet*MatGetProj reads THIS
                    r->MatSetProj(proj);
                    r->SetViewMatrix(view);
                    // LRM_REFLECTION draws the FULL cell disc (case 1: sortedCells [0..drawRadius+1]); LRM_DIRECT
                    // (case 0) only emits the outer LOD ring [drawRadius*lsTransitionDevider+1 .. drawRadius+1].
                    // For a shadow map we want every cell, so use the full-disc mode. Same solid VS/PS + lightmap.
                    L->DrawSolidLandscape(LRM::LRM_REFLECTION, 0);
                    // Object casters: SGRF_LOW_DETAIL renders exactly the solid casters at low poly
                    // (SgStaticModelNode + SgAnimatedModelNode + SgGameUnitNode -- verified in SceneGraph::Render
                    // @0x23a850), the same flag the water-reflection pass uses for offscreen object draws. Runs
                    // with the sun view/proj already set, so objects rasterize depth into this cascade. Uses the
                    // camera-visible node set (from the main pass UpdateVis) -- fine for a first pass.
                    if (Config::Instance().shadow_csm_objects.value) {
                        r->SetZbState(ZB_ENABLE, false);
                        // The object .fx materials re-set D3DRS_CULLMODE per draw (overriding SetCull), and the
                        // sun-POV winding makes their back-face cull drop the sun-facing faces -> wrong/peter-
                        // panned object shadows. Force both faces via the device-level cull override for the
                        // duration of the object draws (depth-only: the sun-facing face still wins the z test).
                        r->SetCull(M3DCULL_NONE, false);
                        dev->SetCsmCullOverride(true);
                        // Shadow-caster LOD by CAMERA distance, not the sun eye. The native tree picker
                        // (AnimatedModelsServer::RenderNodeSet) sizes each caster by distSq from GetViewOrigin()
                        // vs g_impostorThreshold(500u); the view is the SUN here, so every tree measures as
                        // "far" -> billboard impostor -> a camera-facing card casts a wrong/flat shadow instead
                        // of the alpha-clipped canopy. Force the camera eye so near trees fall under the LOD/
                        // impostor thresholds and cast the real mesh. Cleared right after the draw.
                        dev->SetForcedViewOrigin(camPos);
                        // #1 off-camera casters: the main opaque UpdateVis (above) flagged only nodes inside the
                        // CAMERA frustum, so casters behind/beside the camera -- still inside this cascade's sun
                        // box -- were culled from m_visSlots and their shadows popped in only when the camera
                        // turned to face them. Rebuild the visible-node set from the SUN's POV. The sun view is
                        // already set on the renderer (MatSet/MatSetProj/SetViewMatrix above), so the engine's own
                        // CreateScreenFrustums reads it (MatGetOrgInv = sun eye, MatGet = sun view, square CSM
                        // viewport => fovx==fovy) and builds a perspective cone from the sun eye. Real casters sit
                        // near the ground, far from the high sun eye, so a wide cone (shadow_csm_caster_fov, a
                        // full slope halved internally: half-slope 1.5 covers the +-half box CORNERS at camCenter
                        // depth for any half/depthRange since sqrt(2)*half/(half+depthRange) < sqrt(2)) covers the
                        // ortho box's caster region with margin; casters outside a cascade's box get projection-
                        // clipped per cascade. UpdateVis rewrites only m_visSlots/m_transparentNodes -- NOT
                        // m_enableMap (still 0xFF for the terrain disc) -- so the terrain draw above is untouched.
                        // The candidate cell set is the same omnidirectional radius-12 disc the camera pass uses
                        // (SortedCells), so nearby off-camera cells are already candidates; only the clipper's
                        // per-node testSphere direction test changes. Restored to the camera set after the loop.
                        static CClipper s_csmCasterFrusta;
                        const float farz = 2.0f * half + 2.0f * depthRange + 1.0f;  // reach past the box far (ground) face
                        s_csmCasterFrusta.CreateScreenFrustums(farz, 1.0f, 1.0f,
                                                               Config::Instance().shadow_csm_caster_fov.value);
                        w->m_sceneGraph.UpdateVis(false, s_csmCasterFrusta, false);
                        // Distance-based LOD across cascades: the near cascade renders the full opaque set
                        // (each node at its camera-distance LOD -> crisp near-object shadows); farther cascades
                        // use the cheaper low-detail caster set. Cull override keeps both winding-correct.
                        w->m_sceneGraph.Render(i == 0 ? SGRF_DEFAULT_OPAQUE : SGRF_LOW_DETAIL);
                        dev->ClearForcedViewOrigin();
                        dev->SetCsmCullOverride(false);
                    }
                    dev->CsmEndCascade();
                    D3DPERF_EndEvent();  // CSM cascade i
                }
                memcpy(sg.m_enableMap, s_savedEnableMap, sizeof(s_savedEnableMap));  // restore camera-frustum culling
                r->PopCull();
                r->PopFog();
                r->PopZbState();
                r->PopBlend();
                D3DPERF_EndEvent();  // CSM gen
                dev->MatSet(saveMat);
                r->MatSetProj(saveProj);
                r->SetViewMatrix(saveView);

                // Restore the CAMERA-visible node set: the per-cascade sun-POV UpdateVis above replaced
                // m_visSlots/m_transparentNodes with the sun-visible set, but the transparent + overlay passes
                // below (SGRF_DEFAULT_TRANS / SGRF_OVERLAYS) render from m_visSlots and must see the camera's set
                // again. L->m_frustumCull is the camera clipper the main opaque pass used; the camera view is
                // already restored above (UpdateVis reads MatGetOrgInv for its distance cull). Only needed when
                // the sun UpdateVis ran (shadow_csm_objects).
                if (Config::Instance().shadow_csm_objects.value)
                    w->m_sceneGraph.UpdateVis(false, L->m_frustumCull, true);

                static int s_csmLog = 0;
                if ((s_csmLog++ & 255) == 0)
                    LOG_INFO("CSM gen: %d cascades  dists %.0f/%.0f/%.0f (terrainDisc %.0f cap)  sunDir %.2f %.2f %.2f",
                             kraken::render::CDevice::CSM_CASCADES, csmDist[0], csmDist[1], csmDist[2], terrainDisc,
                             sunDir.x, sunDir.y, sunDir.z);
            }
        }

        // AO on the opaque scene (terrain + objects) BEFORE grass + the transparents, so it doesn't
        // darken the alpha-clipped grass ("AO overdraw alpha"). Grass then draws on top, un-AO'd.
        kraken::render::CDevice::Instance()->RenderHbaoDebug();

        // CSM sun shadows: sample the cascade depth maps and multiply shadow into the opaque scene, at the
        // same seam as HBAO (terrain + objects shadowed as receivers, before grass/water). Self-gates on
        // shadow_csm + generated cascades + scene depth.
        kraken::render::CDevice::Instance()->RenderCsmApply();

        // Native DrawShadows renders grass internally; when native shadows are off/suppressed it's skipped,
        // so render grass here (unshadowed) or it vanishes.
        if (!Config::Instance().shadow_native.value || !cb(cfg.m_dsShadows)
            || !w->m_weatherManager.GetShadowVisibilityFromWeather()) {
            ::vc3::deque<::vc3::pair<int, int>> empty;
            L->RenderGrass(empty);
        }

        // ---- transparent: water, shores, transparent objects, weather ----
        if (L->m_numWaterCells && L->m_isWaterVisible) {
            if (L->m_waterShaderVersion == 20 && (waterQ == 3 || waterQ == 2)) {
                TexHandle fullFrame = r->GetFullFrameFrameBufferTexture();
                r->CopyRenderTargetToTexture(fullFrame);
                r->RenderToTexStart(L->m_texRtRefraction, true);
                r->ClearViewport(M3DCLEAR_CZ, w->GetWeatherFogColor() & 0xFF000000);
                r->PushCull(M3DCULL_CCW);
                r->PushZbState(ZB_ENABLE);
                r->PushBlend(BM_NONE);
                L->DrawSolidLandscape(LRM::LRM_DEEPMAP, 0);
                r->PopZbState();
                r->PopCull();
                r->PopBlend();
                r->RenderToTexFinish();
            } else {
                r->CopyRenderTargetToTexture(L->m_texRtRefraction);
            }
            r->PushCull(M3DCULL_NONE);
            r->PushZbState(ZB_NOWRITE);
            L->DrawWaterLayer();
            r->PopZbState();
            r->PopCull();
        }

        if (cb(cfg.m_g_drawShores))
            L->DrawShoresLayer();

        r->SetZbState(ZB_NOWRITE, false);
        w->m_sceneGraph.Render(SGRF_DEFAULT_TRANS);
        w->m_weatherManager.RenderWeatherParticles();

        if (cb(cfg.m_lsWireframe))
            r->PopFillMode();

        r->SetFog(false, false);
        r->SetZbState(ZB_DISABLE, false);
        w->m_sceneGraph.Render(SGRF_OVERLAYS);
        w->m_sceneGraph.RenderContouredNodes();

        r->PopFog();
        r->PopZFunc();
        r->PopBlend();
        r->PopCull();
        r->PopZbState();

        // Dev overlay: the CSM cascade depth maps as bottom-left thumbnails, to eyeball what the sun-POV
        // pass captured (drawn last, over the finished scene). Behind shadow_csm_debug (default 0) so it
        // stays off in normal CSM play; RenderCsmDebug self-guards on the cascades existing.
        if (Config::Instance().shadow_csm.value && Config::Instance().shadow_csm_debug.value)
            kraken::render::CDevice::Instance()->RenderCsmDebug();

        // debug: blit reflection/refraction RTs to screen -- gated off in normal play, body omitted.
        if (cb(cfg.m_g_showReflRefrMaps)) {
            // (m_g_showReflRefrMaps debug overlay intentionally not reproduced)
        }
    }

    void Apply() {
        if (!Config::Instance().hbao.value) {
            return;
        }
        LOG_INFO("Landscape::Render reimpl active + CSM apply pass wired (screen-space sun shadows)");
        routines::ChangeCall((void*) kLandscapeRenderCall, (void*) &Landscape_Render);
    }
}
