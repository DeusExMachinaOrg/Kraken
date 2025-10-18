#pragma once

#include "utils.hpp"
#include "ode/ode.hpp"
#include "hta/ref_ptr.h"
#include "hta/CPlane.hpp"
#include "hta/CVector2.hpp"
#include "hta/CClipper.hpp"
#include "hta/m3d/CVar.hpp"
#include "hta/m3d/CFlare.hpp"
#include "hta/m3d/SgNode.hpp"
#include "hta/m3d/CStrHash.hpp"
#include "hta/m3d/CIntHash.hpp"
#include "hta/m3d/Profiler.hpp"
#include "hta/m3d/IConHandler.hpp"
#include "hta/m3d/cmn/vector.hpp"
#include "hta/m3d/rend/VbHandle.hpp"
#include "hta/m3d/rend/IbHandle.hpp"
#include "hta/m3d/rend/IQuery.hpp"
#include "hta/m3d/rend/IEffect.hpp"
#include "hta/m3d/rend/IHlslShader.hpp"
#include "hta/m3d/rend/Vertices.hpp"
#include "hta/ai/Obstacle.hpp"

#include <deque>

namespace m3d {
    struct CWorld;
    struct GeomObject;
    struct DbgCounter;


    enum RenderModes : int32_t {
        RM_GAME = 0x0000,
        RM_EDITOR = 0x0001,
    };

    struct Landscape : public SgNode, public IConHandler {
        struct CollisionCellItem {
            /* Size=0x20 */
            /* 0x0000 */ stable_size_set<GeomObject*> m_geomsList;
            /* 0x000c */ bool m_wasEnabledLastFrame;
            /* 0x000d */ bool m_bMustCheck;
            /* 0x0010 */ stable_size_set<int> m_physicObjIds;
            /* 0x001c */ stable_size_set<ref_ptr<ai::Obstacle>>* m_obstacles;
            
            CollisionCellItem(const CollisionCellItem&);
            CollisionCellItem();
            ~CollisionCellItem();
            void InsertPhysicObjId(int32_t);
            void ErasePhysicObjId(int32_t);
            void InsertObstacle(ai::Obstacle*);
            void EraseObstacle(ai::Obstacle*);
            const stable_size_set<int>& GetPhysicObjIds() const;
            const stable_size_set<ref_ptr<ai::Obstacle>>& GetObstacles() const;
        };
        class CollisionInfo {
            /* Size=0x74 */
            /* 0x0000 */ int32_t m_tag;
            /* 0x0004 */ int32_t m_numVerts;
            /* 0x0008 */ CVector* m_verts;
            /* 0x000c */ int32_t m_numTris;
            /* 0x0010 */ uint16_t* m_tris;
            /* 0x0014 */ Aabb m_box;
            /* 0x002c */ Obb m_obb;

            CollisionInfo(const CollisionInfo&);
            CollisionInfo();
            ~CollisionInfo();
            void Create(int32_t, CVector*, int32_t, uint16_t*, const CMatrix&);
            float TraceRay(const CVector&, const CVector&);
            float GetHeight(float, float);
            void Draw(uint32_t);
        };
        struct WaveSets {
            /* Size=0x2c */
            /* 0x0000 */ float m_tcomp;
            /* 0x0004 */ float m_tlevel;
            /* 0x0008 */ float m_tamplitude;
            /* 0x000c */ float m_tphase;
            /* 0x0010 */ float m_tfreq;
            /* 0x0014 */ float m_scomp;
            /* 0x0018 */ float m_slevel;
            /* 0x001c */ float m_samplitude;
            /* 0x0020 */ float m_sphase;
            /* 0x0024 */ float m_sfreq;
            /* 0x0028 */ rend::TexHandle m_texHandle;
        
            WaveSets(const WaveSets&);
            WaveSets();
        };
        struct CellParams {
            /* Size=0x3c */
            /* 0x0000 */ float m_h0;
            /* 0x0004 */ float m_h1;
            /* 0x0008 */ bool m_iswatercell;
            /* 0x000c */ float m_minwater;
            /* 0x0010 */ float m_maxwater;
            /* 0x0014 */ float m_lodDelta[5];
            /* 0x0028 */ bool m_lodDeltaCreated;
            /* 0x002c */ float m_normalTheta;
            /* 0x0030 */ CVector m_normalAverage;
        
            CellParams(const CellParams&);
            CellParams();
        };
        struct AlphaMask {
            /* Size=0x4c */
            /* 0x0000 */ CStr m_name;
            /* 0x000c */ stable_size_vector<m3d::rend::TexHandle> m_texMasks[4];

            AlphaMask(const AlphaMask&);
            AlphaMask();
            ~AlphaMask();
        };
        struct LandType {
            /* Size=0x28 */
            /* 0x0000 */ CStr m_name;
            /* 0x000c */ int32_t m_passmask;
            /* 0x0010 */ int32_t m_alphaset;
            /* 0x0014 */ int32_t m_priority;
            /* 0x0018 */ stable_size_vector<int> m_texIndices;

            LandType(const LandType&);
            LandType();
            ~LandType();
        };
        enum RenderTypes : int32_t {
            RT_FIRSTPASSLIGHT = 0x0000,
            RT_OTHERPASSES = 0x0001,
            RT_LIGHTPASS = 0x0002,
        };
        struct TileInfo {
            /* Size=0x34 */
            /* 0x0000 */ int32_t m_texIndex0;
            /* 0x0004 */ int32_t m_angle;
            /* 0x0008 */ int32_t m_texIndices[4];
            /* 0x0018 */ int32_t m_texFlags[4];
            /* 0x0028 */ int32_t m_numTexs;
            /* 0x002c */ int32_t m_maskIndex;
            /* 0x0030 */ int32_t m_rotate;
        
            TileInfo();
            ~TileInfo();
        };
        struct AlphaSetUnit {
            /* Size=0x320 */
            /* 0x0000 */ float m_uvForAngles[4][25][2];
        };
        struct TextureAlphaSet {
            /* Size=0x7d00 */
            /* 0x0000 */ AlphaSetUnit m_sets[8][5];
        };
        enum VisibilityMode : int32_t {
            VIS_DIRECT = 0x0000,
            VIS_REFLECTION = 0x0001,
            VIS_REFRACTION = 0x0002,
        };
        enum RenderGrassType : int32_t {
            RGT_SIMPLE = 0x0000,
            RGT_FOR_SHADOW = 0x0001,
            RGT_FOR_PROJECTOR = 0x0002,
        };
        struct GrassInstance {
            /* Size=0x18 */
            /* 0x0000 */ CVector pos;
            /* 0x000c */ float scale;
            /* 0x0010 */ float sinYaw;
            /* 0x0014 */ float cosYaw;
        
            GrassInstance(const GrassInstance&);
            GrassInstance();
        };
        struct TIVChunk {
            /* Size=0x6201c */
            /* 0x0000 */  stable_size_vector<m3d::rend::VbHandle> m_vbHandle;
            /* 0x0010 */  m3d::rend::TexHandle m_texHandle;
            /* 0x0014 */  uint32_t m_offsetsmap[65536];
            /* 0x40014 */ uint16_t m_banknumber[65536];
            /* 0x60014 */ uint16_t m_numCellsPerCellMap[4096];
            /* 0x62014 */ int32_t iotherPassOffset;
            /* 0x62018 */ int32_t iotherPassBankNumber;
        
            TIVChunk(const TIVChunk&);
            TIVChunk();
            ~TIVChunk();
        };
        enum LandRenderMode : int32_t {
            LRM_DIRECT = 0x0000,
            LRM_REFLECTION = 0x0001,
            LRM_DEEPMAP = 0x0002,
            LRM_BIND = 0x0003,
        };
        struct GrassInstancesForModel {
            /* Size=0x18 */
            /* 0x0000 */ int32_t modelId;
            /* 0x0004 */ int32_t numInstances;
            /* 0x0008 */ stable_size_vector<GrassInstance*> grass;

            GrassInstancesForModel(const GrassInstancesForModel&);
            GrassInstancesForModel();
            ~GrassInstancesForModel();
        };
        struct TileGrass {
            /* Size=0x18 */
            /* 0x0000 */ uint32_t numDiffModels;
            /* 0x0004 */ uint32_t numInstances;
            /* 0x0008 */ stable_size_vector<GrassInstancesForModel*> instancesPerModel;
            TileGrass(const TileGrass&);
            TileGrass();
            ~TileGrass();
        };
        /* Size=0x8f10 */
        /* 0x0000: fields for SgNode */
        /* 0x01d4: fields for IConHandler */
        /* 0x01d8 */ CollisionCellItem** m_oCollisionitems;
        /* 0x01dc */ GeomObject* m_terrainObject;
        /* 0x01e0 */ int32_t m_maxLOD;
        /* 0x01e4 */ stable_size_vector<CollisionInfo*> m_collisions;
        /* 0x01f4 */ float* m_heightMap;
        /* 0x01f8 */ int16_t* m_waterMap;
        /* 0x01fc */ CVector* m_vnormal;
        /* 0x0200 */ stable_size_vector<rend::IbHandle> m_landIbConst;
        /* 0x0210 */ rend::VbHandle m_landUVVb;
        /* 0x0214 */ rend::VbHandle m_landVb;
        /* 0x0218 */ rend::IbHandle m_waterIb[16];
        /* 0x0258 */ rend::VbHandle m_waterVb;
        /* 0x025c */ rend::IQuery* m_waterQueries[3];
        /* 0x0268 */ int32_t m_currWaterQuery;
        /* 0x026c */ bool m_isWaterVisible;
        /* 0x026d */ bool m_currentWaterVis;
        /* 0x0270 */ rend::IbHandle m_shoresIb;
        /* 0x0274 */ rend::VbHandle m_shoresVb;
        /* 0x0278 */ rend::IEffect* m_shoresShader;
        /* 0x027c */ rend::VbHandle m_solidVb;
        /* 0x0280 */ rend::IbHandle m_solidIb[4];
        /* 0x0290 */ rend::IHlslShader* m_solidVs;
        /* 0x0294 */ rend::IHlslShader* m_solidPs;
        /* 0x0298 */ rend::IHlslShader* m_solidBindVs;
        /* 0x029c */ rend::IHlslShader* m_solidBindPs;
        /* 0x02a0 */ float m_bindDevider;
        /* 0x02a4 */ rend::IHlslShader* m_solidDeepVs;
        /* 0x02a8 */ rend::IHlslShader* m_solidDeepPs;
        /* 0x02ac */ rend::IHlslShader* m_landscapeVs;
        /* 0x02b0 */ rend::IHlslShader* m_landscapePsFP;
        /* 0x02b4 */ rend::IHlslShader* m_landscapePsSP;
        /* 0x02b8 */ CMatrix m_matScale;
        /* 0x02f8 */ int32_t vertsPerCell;
        /* 0x02fc */ int32_t trisPerCell[4];
        /* 0x030c */ stable_size_vector<WaveSets> m_waves;
        /* 0x031c */ stable_size_vector<stable_size_vector<CVector>> m_shoreLines;
        /* 0x032c */ stable_size_set<unsigned int> m_noShoresSet;
        /* 0x0338 */ rend::VertexXYZNCT2* m_dummyVB;
        /* 0x033c */ uint16_t* m_remappedIndices;
        /* 0x0340 */ uint16_t* m_lsIndicesDubb;
        /* 0x0344 */ int32_t m_ver;
        /* 0x0348 */ CPlane m_waterPlane;
        /* 0x0360 */ CClipper m_frustumCull;
        /* 0x0778 */ int32_t m_numWaterCells;
        /* 0x077c */ int32_t m_drawRadius;
        /* 0x0780 */ int32_t m_landscapeClip0;
        /* 0x0784 */ int32_t m_landscapeClip1;
        /* 0x0788 */ int32_t m_landscapeClip2;
        /* 0x078c */ float m_landscapeClip0z;
        /* 0x0790 */ float m_landscapeClip1z;
        /* 0x0794 */ float m_landscapeClip2z;
        /* 0x0798 */ float m_landscapeClip0zSq;
        /* 0x079c */ float m_landscapeClip1zSq;
        /* 0x07a0 */ float m_landscapeClip2zSq;
        /* 0x07a4 */ int32_t m_saveClip[3];
        /* 0x07b0 */ CClipper m_reflectedFrustum;
        /* 0x0bc8 */ CVar m_lockVis;
        /* 0x0bf4 */ CellParams* m_cellParams;
        /* 0x0bf8 */ CellParams* m_drawedCellParams;
        /* 0x0bfc */ CFlare m_flares;
        /* 0x0c14 */ bool m_dirtyReflection;
        /* 0x0c18 */ rend::TexHandle m_texRtReflection;
        /* 0x0c1c */ rend::TexHandle m_texRtRefraction;
        /* 0x0c20 */ rend::TexHandle m_baseWaterTex;
        /* 0x0c24 */ rend::TexHandle m_texLightmap;
        /* 0x0c28 */ int32_t m_texNormalMapSize;
        /* 0x0c2c */ stable_size_vector<AlphaMask> m_AlphaSets;
        /* 0x0c3c */ stable_size_vector<LandType> m_Lands;
        /* 0x0c4c */ CIntHash<int> m_hashIdxToPass;
        /* 0x0c74 */ CStrHash<int> m_hashAlphaToLand;
        /* 0x0c80 */ int32_t m_CurAlphaSet;
        /* 0x0c84 */ char* m_passedCells;
        /* 0x0c88 */ unsigned char* m_cliffHeightMap;
        /* 0x0c8c */ uint32_t* m_normalMap;
        /* 0x0c90 */ CWorld* m_owner;
        /* 0x0c94 */ RenderTypes m_lastState;
        /* 0x0c98 */ int32_t m_firstpasscounter;
        /* 0x0c9c */ int32_t m_otherpasscounter;
        /* 0x0ca0 */ stable_size_set<unsigned int>* m_texSetsmap;
        /* 0x0ca4 */ rend::IEffect* overlayShader;
        /* 0x0ca8 */ CIntHash<int> m_hashTexToIndex;
        /* 0x0cd0 */ CStrHash<int> m_texToIdx;
        /* 0x0cdc */ CIntHash<int> m_hashIdxToLandType;
        /* 0x0d04 */ stable_size_vector<TIVChunk*> m_tilesTextures;
        /* 0x0d14 */ TileInfo* m_tiles;
        /* 0x0d18 */ uint32_t* m_colormap;
        /* 0x0d1c */ float m_uvForAngles[4][25][2];
        /* 0x103c */ TextureAlphaSet m_setAndUVs;
        /* 0x8d3c */ cmn::vector<cmn::vector<unsigned int> > m_cellsPerTex;
        /* 0x8d48 */ CStr m_pathTile;
        /* 0x8d54 */ bool m_loadAllTextures;
        /* 0x8d58 */ stable_size_set<CStr> m_usedTexturesList;
        /* 0x8d64 */ stable_size_vector<int> m_lsNumIndices;
        /* 0x8d74 */ int32_t m_wtNumTris[16];
        /* 0x8db4 */ int32_t m_drawtextured;
        /* 0x8db8 */ int32_t m_mapSize;
        /* 0x8dbc */ Profiler* m_profilerDraw;
        /* 0x8dc0 */ Profiler* m_profilerUpdateVis;
        /* 0x8dc4 */ Profiler* m_profilerDrawGrass;
        /* 0x8dc8 */ Profiler* m_profilerDrawWater;
        /* 0x8dcc */ DbgCounter* m_countPhysicObjsInCells;
        /* 0x8dd0 */ rend::TexHandle m_waveBumpTex;
        /* 0x8dd4 */ rend::TexHandle m_waveBumpSmTex;
        /* 0x8dd8 */ rend::TexHandle m_fresnelTex;
        /* 0x8ddc */ rend::TexHandle m_waveReflMap;
        /* 0x8de0 */ rend::IHlslShader* m_waterVs;
        /* 0x8de4 */ rend::IHlslShader* m_waterPs;
        /* 0x8de8 */ rend::IHlslShader* m_waterDumbVs;
        /* 0x8dec */ rend::IHlslShader* m_waterDumbPs;
        /* 0x8df0 */ int32_t m_waterShaderVersion;
        /* 0x8df4 */ int32_t m_maxWaterCellPerPass;
        /* 0x8df8 */ stable_size_vector<std::pair<unsigned int,float>> waterCellsToDraw[16];
        /* 0x8ef8 */ CVector4* waterTileInfo;
        /* 0x8efc */ rend::IHlslShader* m_grassVs;
        /* 0x8f00 */ rend::IHlslShader* m_grassPs;
        /* 0x8f04 */ VisibilityMode m_curVisMode;
        /* 0x8f08 */ TileGrass** m_grassArray;
        /* 0x8f0c */ uint32_t m_numGrassModels;
        
        static Class m_classLandscape;
        static RenderModes m_renderMode;

        Landscape();
        Landscape(const Landscape&);
        virtual ~Landscape();
        virtual Object* Clone();
        virtual Class* GetClass() const;
        void BuildSolidLandscape();
        void PushLsClip(int32_t, int32_t);
        void PopLsClip();
        int32_t isWaterCell(int32_t, int32_t) const;
        uint32_t getLsColorByDist(const CVector&, const CVector&, unsigned char);
        void DrawWaterLayer();
        void QueryWaterVisibility();
        void StartWaterQuery();
        void EndWaterQuery();
        void DrawShoresLayer();
        void DrawShoreLine();
        int32_t ReflectionWaterClipCell(int32_t, int32_t);
        void renderZGuard();
        void drawPolygonOverlayed(CVector2*, int32_t, uint32_t);
        void CreateHeights(CellParams*, int32_t, int32_t);
        void CreateLod();
        void ReleaseLod();
        virtual void HandleCommand(int32_t, const CConsoleParams&);
        virtual bool HandleCVar(const CVar*, const CConsoleParams&);
        void DrawLandScapeTextures(VisibilityMode, bool, bool);
        void DrawSolidLandscape(LandRenderMode, int32_t);
        int32_t GetNumAlphas() const;
        CStr GetAlphaName(int32_t) const;
        int32_t GetUsedAlphaIdx(int32_t) const;
        CStr GetUsedAlpha(int32_t) const;
        int32_t GetNumLands() const;
        const CStr& GetLandName(int32_t) const;
        int32_t GetLandByName(const CStr&) const;
        int32_t GetNumTexturesInLand(int32_t) const;
        int32_t GetTexIndexFromLand(int32_t, int32_t) const;
        void DeleteIndexFromLand(int32_t, int32_t);
        int32_t GetPriorityofLand(int32_t) const;
        void SetPriorityofLand(int32_t, int32_t);
        void SetAlphaofLand(int32_t, int32_t);
        int32_t AddOneTexture(const CStr&);
        void UpdateTexturesFilters();
        void AddCollisionTris(int32_t, int32_t, CVector*, int32_t, uint16_t*, const CMatrix&);
        void RemoveCollisionTris(int32_t);
        void GetVisCellHeights(float&, float&, int32_t, int32_t) const;
        void GetWaterCellHeights(float&, float&, int32_t, int32_t) const;
        void GetDrawedCellHeights(float&, float&, int32_t, int32_t) const;
        bool IsThisVisCellHasWater(int32_t, int32_t) const;
        bool traceLineThruBox(float&, float*, const CVector&, const CVector&);
        bool traceLineThruCellLs0(float&, int32_t, int32_t, const CVector&, const CVector&);
        bool traceLineThruCellLs(float&, int32_t, int32_t, const CVector&, const CVector&, bool);
        int32_t IsBackfaced(int32_t, int32_t, rend::Cull);
        void DrawCells0(const cmn::vector<unsigned int>&, RenderTypes);
        void DrawCellsFast0(const cmn::vector<unsigned int>&, TIVChunk&, RenderTypes);
        void BuildCells0(rend::VertexLandscape*, TIVChunk&, int32_t&, const cmn::vector<unsigned int>&, RenderTypes, stable_size_vector<int>&);
        void BuildUVSet();
        void CompleteShores();
        void RecursiveEnableShore(unsigned char*, int32_t, int32_t);
        void RecursiveDisableShore(unsigned char*, int32_t, int32_t);
        void ReBuildShoresVb();
        void FreeShoresStuff();
        CollisionCellItem* GetCollisionCellItem(int32_t, int32_t) const;
        GeomObject* GetTerrainGeomObject() const;
        void ClearCollisionCellsMap();
        void SetPresenceOnCollisionMap(int32_t, int32_t);
        void ManageLandScapeCollisionTriMeshes();
        void CheckLandscapeCollisionTriMeshesForObjId(int32_t);
        void GetDPVSCollisionInfo(int32_t, int32_t, int32_t&, int32_t&);
        void GenerateOneDPVSCellMesh(int32_t, int32_t, CVector*, int32_t*);
        void CreateIndicesTriLists(int32_t*, int32_t, int32_t);
        int32_t ConstructCollisionData();
        void LinkNodeAndChildrenCollisionGeomsToCell(SgNode*);
        void LinkNodeCollisionGeomsToCell(SgNode*, int32_t, int32_t, int32_t, int32_t);
        void UnlinkNodeCollisionGeomsFromCell(SgNode*, int32_t, int32_t, bool);
        void UpdateNodeCollisionGeoms(SgNode*);
        void SetNodeCollisionGeomsEnabled(SgNode*, bool);
        void ReleaseOdeCollisionData();
        void LinkNodeObstacleToCells(SgNode*);
        void LinkObstacleToCells(ai::Obstacle*);
        void LinkPassMapCellToCollisionCell(const PointBase<int>&);
        void DrawCollisionGeoms(bool);
        void DrawGeom(dxGeom*);
        void DrawJoint(dxJoint*);
        void DrawMassBox(dMass*, const CVector&, const Quaternion&);
        void GetFogStartAndEnd(float&, float&) const;
        void PostServersLoad();
        int32_t GenerateShoreLine();
        void EnableShoreRegion(int32_t, int32_t);
        void DisableShoreRegion(int32_t, int32_t);
        void ChangeShoreState(int32_t, int32_t, bool);
        bool traceLineThruWallCells(const CVector&, CVector&, bool);
        void setDrawRadius(int32_t, int32_t, int32_t);
        void SetOverlayShader(rend::IEffect*);
        int32_t getClip0() const;
        void DrawCells(const cmn::vector<unsigned int>&, uint32_t);
        void DrawCellsOverlayedEditor(const cmn::vector<unsigned int>&, uint32_t);
        float getHgtAtHfPoint(int32_t, int32_t) const;
        void setHgtAtHfPoint(int32_t, int32_t, float);
        uint32_t getClrAtHfPoint(int32_t, int32_t) const;
        void setClrAtHfPoint(int32_t, int32_t, uint32_t);
        const CVector& getNormalAtPoint(int32_t, int32_t) const;
        void getMinMaxHeightForBox(float*, float);
        rend::TexHandle GetTexByIdx(int32_t) const;
        rend::TexHandle GetLightmapTexture() const;
        void ReloadLightmapTexture(const CStr&);
        void SetAllTexturesLoading(bool);
        void GetTilesLoadParams(CIntHash<int>**, CStrHash<int>**, stable_size_vector<TIVChunk*>**);
        void AddIdxToLand(int32_t, int32_t);
        int32_t LoadTiles(const CStr&);
        void FreeTiles();
        bool LoadNormalMap(const CStr&);
        bool SaveNormalMap(const CStr&);
        bool LoadShoreLine(const CStr&);
        bool SaveShoreLine(const CStr&);
        void DrawSpotInColorMap(float, float, int32_t);
        unsigned char GetColor(float, float);
        void ReadTileInfo(int32_t);
        void SaveTileInfo();
        int32_t GetPassForTexture(int32_t) const;
        float GetFloatToShortScale() const;
        const CStr& GetPathToTiles() const;
        int32_t GetLsSize() const;
        int32_t GetTileSize() const;
        float GetScaleForTile() const;
        int32_t GetTileRot(int32_t, int32_t) const;
        int32_t GetTileIndex(int32_t, int32_t) const;
        void SetTileRot(int32_t, int32_t, int32_t);
        void SetTile(int32_t, int32_t, int32_t);
        rend::TexHandle GetTexHandleFromList(uint32_t) const;
        int32_t RecalcNormalMap(int32_t, int32_t, int32_t, int32_t);
        void RecalcUV();
        void CreateHelperStructures();
        int32_t GetNumTiles() const;
        bool Save16bitDisplace(const CStr&, int32_t);
        bool SaveColorMap(const CStr&, int32_t);
        int32_t SaveCameraMap(const CStr&, int32_t);
        int16_t* GetWaterPtr();
        TileInfo* GetTilesPtr();
        float* GetHeightPtr();
        const TileInfo& GetTileInfo(int32_t, int32_t) const;
        void SwitchDrawMode();
        float GetLsHeight(float, float) const;
        void SetLsHeight(float, float, float);
        float GetHeight(float, float, int32_t, bool);
        float GetHeightWithCollisions(float, float, bool) const;
        CVector getNormal(float, float);
        uint32_t GetRotatedInt(uint32_t, int32_t) const;
        bool WaterSave(const CStr&);
        void WaterClear();
        int32_t Load();
        int32_t New(float);
        virtual void Render();
        virtual int32_t Render(SgNodeRenderFlags, void*, int32_t, int32_t);
        void Update();
        void UpdateVis(bool);
        void Restore();
        void Invalidate();
        void Release();
        void InitReflectionRefractionTextures();
        void ReloadWaterTextures();
        void ReleaseReflectionRefractionTextures();
        float getWaterHeight(int32_t, int32_t) const;
        float getCameraHeight(float, float) const;
        void drawSpriteOverlayed(uint32_t, const CVector&, const CVector&, float);
        void drawSpriteOverlayedProjected(uint32_t, const CVector&, const CMatrix&, const CClipper&);
        void drawSpriteOverlayed2(float, float, float, float, uint32_t, bool);
        void drawSpriteOverlayed2Projected(float, float, float, float, uint32_t, bool, const CClipper&);
        void drawCellOverlayedShader(int32_t, int32_t, rend::IEffect*);
        void setOwner(CWorld*);
        void ChangedNumberOfUsedTextures(uint32_t);
        bool InitGrass();
        void DoneGrass();
        bool AddGrassModel(const char*);
        uint32_t GetNumGrassModels() const;
        const char* GetGrassModelName(uint32_t) const;
        uint32_t AddGrassInstance(int32_t, const CVector&, float, float, bool);
        void RemoveGrassInstance(uint32_t);
        void RemoveGrassTile(int32_t, int32_t);
        void RemoveGrassRadius(const CVector&, float);
        void RemoveGrassRectangle(const CVector2&, const CVector2&);
        void ScaleGrassRadius(const CVector&, float, float);
        void PutGrassToLandscape(const CVector2&, const CVector2&);
        uint32_t GetNearestGrassInstance(const CVector&) const;
        void _dbgGenerateGrass();
        void RenderGrass(uint32_t, GrassInstance**, int32_t*, RenderGrassType);
        void RenderGrass(const std::deque<std::pair<int,int>>&);
        void CollectGrassCell(int32_t, int32_t, uint32_t&, GrassInstance**, int32_t*);
        int32_t getGrassModelIdByName(const char*) const;
        bool WriteGrassToXmlFile(const char*);
        void ReadGrassFromXmlFile(const char*);
        void DrawNonTransformGeom(dxGeom*);
        void SetCurVisMode(VisibilityMode);
        VisibilityMode GetCurVisMode() const;
        void RenderRoads();
        
        static Object* CreateObject();
        static Class* GetBaseClass();
        static void Register();
        static void SetRenderMode(RenderModes);
        static void SetGameRenderMode();
        static void SetEditorRenderMode();
    };
    ASSERT_SIZE(Landscape, 0x8f10);
};