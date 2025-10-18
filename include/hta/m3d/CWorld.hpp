#pragma once

#include "utils.hpp"
#include "ode/ode.hpp"
#include "hta/CCamera.hpp"
#include "hta/m3d/CVar.hpp"
#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/SceneGraph.hpp"
#include "hta/m3d/RoadManager.hpp"
#include "hta/m3d/WeatherManager.h"
#include "hta/m3d/WheelTraceMgr.hpp"
#include "hta/ai/Vehicle.h"

namespace m3d {
    struct TexClouds;
    struct CClient;
    struct Level;

    struct CWorld {
        struct EffectsData {
            /* Size=0x18 */
            /* 0x0000 */ bool onlyOne;
            /* 0x0004 */ m3d::SgNode* effect;
            /* 0x0008 */ stable_size_vector<m3d::SgNode*> effects;

            EffectsData(const EffectsData&);
            EffectsData();
            ~EffectsData();
        };
        /* Size=0x39eb00 */
        /* 0x0000 */   CVar m_lsInscatterCoeff;
        /* 0x002c */   CVar m_lsOutscatterCoeff;
        /* 0x0058 */   CVar m_skyInscatterCoeff;
        /* 0x0084 */   CVar m_skyOutscatterCoeff;
        /* 0x00b0 */   CVar m_sunColorR;
        /* 0x00dc */   CVar m_sunColorG;
        /* 0x0108 */   CVar m_sunColorB;
        /* 0x0134 */   CVar m_fogStart;
        /* 0x0160 */   bool m_isWeatherActual;
        /* 0x0164 */   dxGeom* m_borderWallGeoms[6];
        /* 0x017c */   Landscape m_landscape;
                       uint8_t _gap0x017c[116]; // WTF Moment
        /* 0x9100 */   SceneGraph m_sceneGraph;
        /* 0x39e980 */ RoadManager m_roadManager;
        /* 0x39e99c */ WeatherManager m_weatherManager;
        /* 0x39e9fc */ WheelTraceMgr m_wheelTracesMgr;
        /* 0x39ea2c */ int32_t m_lastId;
        /* 0x39ea30 */ rend::TexHandle m_texMiniMap;
        /* 0x39ea34 */ rend::TexHandle m_texSunGlow;
        /* 0x39ea38 */ TexClouds* m_texClouds;
        /* 0x39ea3c */ CClient* m_owner;
        /* 0x39ea40 */ Level* m_level;
        /* 0x39ea44 */ CVector m_sunDir;
        /* 0x39ea50 */ float m_sunAzimuth;
        /* 0x39ea54 */ float m_sunAscention;
        /* 0x39ea58 */ stable_size_vector<CWorld::EffectsData> m_effectsFactory;
        /* 0x39ea68 */ stable_size_vector<CStr> fxNames;
        /* 0x39ea78 */ stable_size_map<CStr,int> fxRemap;
        /* 0x39ea84 */ Profiler* m_profilerUpdateOde;
                       uint8_t _gap39ea84[120]; // WTF Moment
    
        int32_t GetLastID();
        void SetLastID(int32_t);
        dxSpace* GetOdeSpace();
        bool LoadWorld(const CStr&);
        void ProcessCollisionStuff();
        void ProcessCollisionStuffOnNode(SgNode*);
        bool SaveWorld(const CStr&);
        void UpdateSkyParams();
        int32_t RenderSky(Landscape::LandRenderMode);
        uint32_t GetWeatherFogColor() const;
        uint32_t GetWeatherAmbientColor() const;
        uint32_t GetWeatherDiffuseColor() const;
        uint32_t GetWeatherPlantColor() const;
        uint32_t GetWeatherSunColor() const;
        uint32_t GetWeatherSpecularColor() const;
        WeatherManager& GetWeatherManager();
        GlobalTimeParams GetCurDayTimeFromWeatherManager() const;
        float GetSForShadowsFromWeather() const;
        float GetFogReduceFactorFromWeather() const;
        float GetShadowTransparencyFromWeather() const;
        bool GetShadowVisibilityFromWeather() const;
        void SetOwner(CClient*);
        const CVector& GetSun(float) const;
        float GetSunAscention() const;
        CVector GetScatteringSunColor() const;
        void UpdateSun();
        void RefreshObjectsOnLandscapeRect(int32_t, int32_t, int32_t, int32_t);
        void RefreshObjectsOnLandscapeRect(const CVector&, float);
        CWorld();
        ~CWorld();
        SceneGraph& GetGraph();
        Landscape& GetLandscape();
        RoadManager& GetRoadManager();
        WheelTraceMgr& GetWheelTracesMgr();
        void Release();
        void New(CCamera&, int32_t, float);
        int32_t Load(const CStr&, CCamera&, bool);
        void Render();
        void Update();
        int32_t CreateGeomsRepresentingSceneNodes();
        ai::Vehicle* GetVehicleControlledByPlayer();
        rend::TexHandle GetMiniMapTex();
        void Invalidate();
        void Restore();
        SgNode* ReadPrefab(ref_ptr<cmn::XmlFile>, ref_ptr<cmn::XmlNode>);
        const std::vector<CStr,std::allocator<CStr> >& GetFxNames() const;
        int32_t GetFxId(const CStr&);
        SgNode* CreatePrefabsNode(int32_t);
        int32_t CreatePrefabsFromFile(const char*);
        void ReleasePrefabs();
        void LoadStaticObstacles();
        void CreatePlayerPassMapGeoms();
    
        public: static void Register();
    };
    ASSERT_SIZE(CWorld, 0x39eb00);
};