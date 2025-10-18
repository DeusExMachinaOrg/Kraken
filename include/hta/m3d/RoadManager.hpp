#pragma once
#include "utils.hpp"
#include "hta/CStr.h"
#include "hta/CVector2.hpp"
#include "hta/CVector.h"

namespace m3d {
    struct Landscape;
    struct RoadNode;
    struct RoadTestCallBack;
    struct RoadSet;

    enum RenderRoadType : int32_t {
        RRT_SIMPLE = 0x0000,
        RRT_FOR_SHADOW = 0x0001,
        RRT_FOR_PROJECTOR = 0x0002,
        RRT_FOR_DETAILED_SHADOW = 0x0003,
        RRT_FOR_POINTLIGHT = 0x0004,
        RRT_FOR_SPRITE = 0x0005,
    };

    struct RoadManager {
        /* Size=0x1c */
        /* 0x0000 */ Landscape* m_owner;
        /* 0x0004 */ stable_size_vector<RoadNode*>* m_coveredCells;
        /* 0x0008 */ RoadNode* m_roadRoot;
        /* 0x000c */ stable_size_vector<RoadSet*> m_roadSets;
    
        RoadManager(const RoadManager&);
        RoadManager();
        void ClearRoadSets();
        void RebuildStructures();
        void RebuildSomeNodes(std::set<RoadNode *,std::less<RoadNode *>,std::allocator<RoadNode *> >, bool);
        void Init();
        void Release();
        void ReleaseCollision();
        void ReleaseCollisionForRoadNode(RoadNode*);
        void UnlinkRoadNodeCollisionFromCells(RoadNode*);
        int32_t ReadRoadSetConfigFromXmlFile(const char*);
        int32_t ReadRoadsFromXmlFile(const char*);
        int32_t WriteRoadsToXmlFile(const char*);
        void SetOwner(Landscape*);
        int32_t GetRoadSetHandleByName(const CStr&);
        const CStr GetRoadSetNameByHandle(int32_t);
        void GetRoadMinMaxXByHandle(int32_t, int32_t, float&, float&);
        void GetRoadMinMaxZByHandle(int32_t, int32_t, float&, float&);
        std::vector<RoadNode *,std::allocator<RoadNode *> >& GetCoveredCell(uint32_t);
        int32_t RenderRoads(std::vector<unsigned int,std::allocator<unsigned int> >&, RenderRoadType, const RoadTestCallBack*, bool);
        void UpdateVis();
        void FindFriends();
        CVector2 FindLeftProjection(RoadNode*, float, float);
        void RecalcCoveredCells();
        void LinkRoadNodes();
        void CalcNodeData(RoadNode*);
        void LinkToBorder(RoadNode*, uint32_t, int32_t, CVector&);
        bool GetAdjPoint(RoadNode*, uint32_t, int32_t, CVector&);
        ~RoadManager();
    };
};