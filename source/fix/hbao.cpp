#define LOGGER "hbao"

#include "config.hpp"
#include "ext/logger.hpp"
#include "fix/hbao.hpp"
#include "routines.hpp"

#include "render/CDevice.hpp"

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
        w->m_sceneGraph.Render(SGRF_SHADOWS);
        r->SetFog(cb(cfg.m_r_enableFog), false);

        // AO on the opaque scene (terrain + objects) BEFORE grass + the transparents, so it doesn't
        // darken the alpha-clipped grass ("AO overdraw alpha"). Grass then draws on top, un-AO'd.
        kraken::render::CDevice::Instance()->RenderHbaoDebug();

        if (!cb(cfg.m_dsShadows) || !w->m_weatherManager.GetShadowVisibilityFromWeather()) {
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

        // debug: blit reflection/refraction RTs to screen -- gated off in normal play, body omitted.
        if (cb(cfg.m_g_showReflRefrMaps)) {
            // (m_g_showReflRefrMaps debug overlay intentionally not reproduced)
        }
    }

    void Apply() {
        if (!Config::Instance().hbao.value) {
            return;
        }
        LOG_INFO("Landscape::Render reimpl active (A/B vs native; no AO yet)");
        routines::ChangeCall((void*) kLandscapeRenderCall, (void*) &Landscape_Render);
    }
}
