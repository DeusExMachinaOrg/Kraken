#pragma once

#include "utils.hpp"
#include "hta/CClipper.hpp"
#include "hta/CVector2.hpp"
#include "hta/m3d/SgNode.hpp"
#include "hta/m3d/ObjectsContainer.hpp"
#include "hta/m3d/rend/IEffect.hpp"
#include "hta/m3d/rend/TexHandle.hpp"
#include "hta/m3d/rend/IHlslShader.hpp"
#include "stdafx.hpp"

namespace m3d {
    struct CWorld;

    enum SgRenderFlags : int32_t {
        SGRF_DEFAULT_OPAQUE = 0x0000,
        SGRF_DEFAULT_TRANS = 0x0001,
        SGRF_SHADOWS = 0x0002,
        SGRF_LOW_DETAIL = 0x0003,
        SGRF_OVERLAYS = 0x0004,
        SGRF_REFRACTION = 0x0005,
    };

    struct IsNodeTransparent { /* Size=0x4 */
        virtual bool test(SgNode*, float);
        virtual float getTransparentRadius();
        virtual bool setPermanentTransparency(SgNode*);
        IsNodeTransparent(const IsNodeTransparent&);
        IsNodeTransparent();
    };

    struct SceneGraph {
        struct CellItems {
            /* Size=0x310 */
            /* 0x0000 */ public: ObjectsContainer m_nodesLinkedDirect;
            /* 0x0300 */ public: stable_size_set<Object*> m_nodesShadowingDirect;
            /* 0x030c */ public: bool m_bVisibleInCurrentFrame;
            /* 0x0310 */         BYTE pad[0x3];
        
            CellItems(const CellItems&);
            CellItems();
            ~CellItems();
        };
        struct CellInfo { /* Size=0xc */
            /* 0x0000 */ uint32_t trisCount;
            /* 0x0004 */ uint16_t objCount;
            /* 0x0006 */ uint16_t animModelCount;
            /* 0x0008 */ uint16_t meshesCount;
        };
        /* Size=0x395880 */
        /* 0x0000 */   SgNode** m_visSlots;
        /* 0x0004 */   int32_t* m_visNumSlots;
        /* 0x0008 */   SgNode** m_visSlotsUnderwater;
        /* 0x000c */   int32_t* m_visNumSlotsUnderwater;
        /* 0x0010 */   SgNode** m_transparentNodes;
        /* 0x0014 */   int32_t m_numTransparentNodes;
        /* 0x0018 */   stable_size_set<SgNode *> m_thinkList;
        /* 0x0024 */   stable_size_set<SgNode *> m_ttledList;
        /* 0x0030 */   stable_size_set<SgNode *> m_RemoveIfFreeList;
        /* 0x003c */   stable_size_set<SgNode *> m_contourList;
        /* 0x0048 */   stable_size_set<SgNode *> m_updateXFormList;
        /* 0x0054 */   SceneGraph::CellItems m_cellItems[4096];
        /* 0x310054 */ bool m_easyRelink;
        /* 0x310055 */ bool m_cellsPrepared;
        /* 0x310056 */ BYTE pad1[0x2];
        /* 0x310058 */ SgNode m_rootNode;
        /* 0x31022c */ SgNode m_landscapeNode;
        /* 0x310400 */ CWorld* m_owner;
        /* 0x310404 */ rend::IEffect* m_lsProjectorShader;
        /* 0x310408 */ rend::IEffect* m_roadProjectorShader;
        /* 0x31040c */ rend::IEffect* m_objProjectorShader;
        /* 0x310410 */ rend::IEffect* m_treeProjectorShader;
        /* 0x310414 */ rend::IEffect* m_roadLightShader;
        /* 0x310418 */ rend::IEffect* m_lsLightShader;
        /* 0x31041c */ rend::IEffect* m_objectLightShader;
        /* 0x310420 */ rend::IEffect* m_treeLightShader;
        /* 0x310424 */ rend::IEffect* m_roadSpriteShader;
        /* 0x310428 */ rend::TexHandle m_texShadows[20];
        /* 0x310478 */ rend::TexHandle m_texShadow;
        /* 0x31047c */ rend::TexHandle m_detTexShadow;
        /* 0x310480 */ rend::TexHandle m_texBlurShadow;
        /* 0x310484 */ rend::IEffect* m_shadowShader;
        /* 0x310488 */ rend::IEffect* m_blurShadowShader;
        /* 0x31048c */ rend::IEffect* m_lsShadowShader;
        /* 0x310490 */ rend::IEffect* m_roadShadowShader;
        /* 0x310494 */ rend::IEffect* m_lsDetailShadowShader;
        /* 0x310498 */ rend::IEffect* m_roadDetailShadowShader;
        /* 0x31049c */ rend::IHlslShader* m_grassShadowVs;
        /* 0x3104a0 */ rend::IHlslShader* m_grassShadowPs;
        /* 0x3104a4 */ rend::IEffect* m_contourShader;
        /* 0x3104a8 */ bool m_noModelCull;
        /* 0x3104a9 */ bool m_bIsInUnlinkAndDeleteAll;
        /* 0x3104aa */ bool m_bIsPurgingRemoveIfFree;
        /* 0x3104ab */ BYTE pad2[0x1];
        /* 0x3104ac */ IsNodeTransparent* m_transparencyTest;
        /* 0x3104b0 */ unsigned char m_enableMap[65536];
        /* 0x3204b0 */ unsigned char m_enableVisSpaceMask;
                       uint8_t _gap3204b0[0x4F];  // WTF Moment
        /* 0x320500 */ char m_sortedCellsX[240000];
        /* 0x35ae80 */ char m_sortedCellsY[240000];
        /* 0x395800 */ int32_t m_sortedCellsCurRadius;
        /* 0x395804 */ int32_t m_sortedCellsEndRadius;
        /* 0x395808 */ int32_t m_sortedCellsCurCell;
                       uint8_t _gap395808[0x74];  // WTF Moment

        static const int32_t NUM_CELL_ITEMS;

        SceneGraph(const SceneGraph&);
        SceneGraph();
        ~SceneGraph();
        void InsertInTtlList(SgNode*, int32_t);
        void DeleteAllTtledNodes();
        void DeleteFromTtlList(SgNode*);
        void LinkNode(SgNode*);
        void InsertInRemoveIfFree(SgNode*);
        void DeleteFromRemoveIfFree(SgNode*);
        void DeleteAllRemoveIfFreeNodes();
        void InsertInContourList(SgNode*, uint32_t, float);
        void DeleteFromContourList(SgNode*);
        void RelinkNode(SgNode*, bool);
        void UnlinkNode(SgNode*);
        void UnlinkAndDeleteAll();
        bool IsLinkedNode(SgNode*);
        void RemoveNode(SgNode*&);
        void LinkThinkNode(SgNode*);
        void UnlinkThinkNode(SgNode*);
        void InsertInUpdateXFormList(SgNode*);
        void DeleteFromUpdateXFormList(SgNode*);
        void CheckNodeIsNotInAnyList(SgNode*) const;
        void UpdateThinkNodes();
        void UpdateAllXForms();
        void Update();
        void UpdateVis(bool, const CClipper&, bool);
        void SortNodesForRender(int32_t);
        void Render(SgRenderFlags);
        void RenderDebugForNode(SgNode*);
        void RenderNode(SgNode*, const CMatrix&, bool);
        void RenderContouredNodes();
        void SetOwner(CWorld*);
        CWorld* GetOwner();
        SgNode* TraceLine(CVector&, const CVector&, const CVector&, const std::set<Class *,std::less<Class *>,std::allocator<Class *> >&, uint32_t);
        int32_t TraceRect(std::list<SgNode *,std::allocator<SgNode *> >&, const CVector2&, const CVector2&, const std::set<Class *,std::less<Class *>,std::allocator<Class *> >&);
        void SetModelForceNoCull(bool);
        void LightSwitchOffAllLights();
        void LightSetupLightsForNode(SgNode*);
        void LightSetupSunForWorld();
        const SgNode* GetRootNode() const;
        SgNode* GetRootNode();
        SgNode* GetNodeByNamesHierarchy(const std::vector<CStr,std::allocator<CStr> >&);
        void GetNodeNamesHierarchy(SgNode*, std::vector<CStr,std::allocator<CStr> >&);
        int32_t IsCellEnabled(int32_t, int32_t);
        void SortedCellsPrepare();
        bool SortedCellsStartFetching(int32_t, int32_t);
        int32_t SortedCellsFetch(int32_t&, int32_t&, int32_t&, int32_t&);
        int32_t SortedCellsFetch(int32_t&, int32_t&, int32_t&);
        void EnableVisibleCells(CClipper&, uint32_t);
        void SetVisMask(unsigned char);
        void RefreshObjectsInRect(int32_t, int32_t, int32_t, int32_t);
        float GetAlphaForNode(SgNode*);
        void SetTransparencyTest(IsNodeTransparent*);
        void DumpToFile(const CStr&) const;
        SgNode* GetNodeByName(const CStr&);
        const ObjectsContainer& GetCellObjs(int32_t, int32_t) const;
        bool IsCellVisible(int32_t, int32_t) const;
        rend::IEffect* GetRoadProjectorShader();
        rend::IEffect* GetLsProjectorShader();
        rend::IEffect* GetObjProjectorShader(rend::IEffect*);
        rend::IEffect* GetObjProjectorShader();
        rend::IEffect* GetTreeProjectorShader();
        void CollectNodesProjector(std::set<SgNode *,std::less<SgNode *>,std::allocator<SgNode *> >&, uint32_t, uint32_t, const CClipper&);
        rend::IEffect* GetLsLightShader();
        rend::IEffect* GetRoadLightShader();
        rend::IEffect* GetObjectLightShader(rend::IEffect*);
        rend::IEffect* GetObjectLightShader();
        rend::IEffect* GetTreeLightShader();
        void CollectNodesLight(std::set<SgNode *,std::less<SgNode *>,std::allocator<SgNode *> >&, uint32_t, uint32_t, const CVector&, float);
        rend::IEffect* GetRoadSpriteShader();
        void UpdateTexShadowSizes();
        rend::IEffect* GetShadowShader();
        rend::IEffect* GetRoadShadowShader();
        rend::IEffect* GetLsDetShadowShader();
        rend::IEffect* GetRoadDetShadowShader();
        rend::IEffect* GetContourShader();
        bool IsInUnlinkAndDeleteAll() const;
        void GetCellsStatistic(std::vector<SceneGraph::CellInfo,std::allocator<SceneGraph::CellInfo> >*);
        void DumpRenderingNodesInfoForClass(const Class*);
        SceneGraph::CellItems& GetCellItems(int32_t, int32_t);
        const SceneGraph::CellItems& GetCellItems(int32_t, int32_t) const;
        int32_t AddOneNodeToRender(SgNode*, const CClipper&, int32_t);
        int32_t AddNodeAndItsChildrenToRender(SgNode*, const CClipper&, int32_t);
        void RemoveNodeExceptRemoveIfFree(SgNode*&);
        void DrawShadows();
        void DrawDetailedShadows(const CVector&, float, rend::TexHandle, const std::vector<Class *,std::allocator<Class *> >&);
        void DrawShadowsToTexture(int32_t*, int32_t, int32_t, rend::TexHandle, const std::vector<Class *,std::allocator<Class *> >&, CVector&, float);
        void PutShadowTextureToLandscapeAndRoad(int32_t*, int32_t, float, float, int32_t, rend::TexHandle);
        void PutShadowTextureToGrass(int32_t*, int32_t, float, float, int32_t, rend::TexHandle);
        void CollectShadowingNodes(std::set<SgNode *,std::less<SgNode *>,std::allocator<SgNode *> >&, Class*, int32_t, int32_t, int32_t, uint32_t, int32_t);
        bool IsThereSomethingToShadow(SceneGraph::CellItems&, const std::vector<Class *,std::allocator<Class *> >&);
        bool IsTransparent(SgNode*);
        void DrawStencilShadows();
        void CollectShadowingNodesStencil(std::set<SgNode *,std::less<SgNode *>,std::allocator<SgNode *> >&, Class*, int32_t, int32_t, uint32_t, int32_t);
        SgNode* TraceLineThruCellNodesForClass(float&, int32_t, int32_t, const CVector&, const CVector&, Class*, std::set<SgNode *,std::less<SgNode *>,std::allocator<SgNode *> >&, uint32_t);
        void enableVisibleCells_r(CClipper&, float*, uint32_t);
        void enableCellsSetRect(float*, uint32_t, uint32_t);
        void enableCellsSetRect(int32_t*, uint32_t, uint32_t);
        void enableVisibleCellsFilterOutBackfaces(const CMatrix&, rend::Cull, uint32_t);
        int32_t getYOfs(int32_t);
        void EnsureEverythingIsUnlinked() const;
    };
    ASSERT_SIZE(SceneGraph, 0x395880);
}