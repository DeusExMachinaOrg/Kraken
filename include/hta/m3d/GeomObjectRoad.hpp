#pragma once

#include "hta/m3d/RoadNode.hpp"
#include "hta/m3d/GeomObject.hpp"

namespace m3d {
    struct GeomObjectRoad : public GeomObject {
        /* Size=0x80 */
        /* 0x0000: fields for GeomObject */
        /* 0x007c */ RoadNode* m_roadNode;

        static Class m_classGeomObjectRoad;
    
        GeomObjectRoad();
        GeomObjectRoad(const GeomObjectRoad&);
        virtual ~GeomObjectRoad();
        virtual Object* Clone();
        virtual Class* GetClass() const;
        RoadNode* GetRoadNode() const;
        void SetRoadNode(RoadNode*);

        static Object* CreateObject();
        static Class* GetBaseClass();
    };
};