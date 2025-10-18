#pragma once
#include "hta/CVector.h"
#include "hta/m3d/Object.h"
#include "hta/m3d/rend/VbPoolField.hpp"
#include "hta/m3d/rend/IbPoolField.hpp"

namespace m3d {
    struct RoadManager;
    struct GeomObjectRoad;

    struct RoadNode : public Object {
        /* Size=0x120 */
        /* 0x0000: fields for Object */
        /* 0x0034 */ CVector m_origin;
        /* 0x0040 */ int32_t m_roadSetHandle;
        /* 0x0044 */ CStr m_roadSetName;
        /* 0x0050 */ int32_t m_type;
        /* 0x0054 */ uint32_t m_modelNum;
        /* 0x0058 */ uint32_t m_skinNumber;
        /* 0x005c */ bool m_asCliff;
        /* 0x0060 */ float m_minX;
        /* 0x0064 */ float m_maxX;
        /* 0x0068 */ float m_maxWorldY;
        /* 0x006c */ CVector minb;
        /* 0x0078 */ CVector maxb;
        /* 0x0084 */ CStr m_linkedNames[4];
        /* 0x00b4 */ RoadNode* m_linkedNodes[4];
        /* 0x00c4 */ RoadNode* m_friend;
        /* 0x00c8 */ RoadManager* m_owner;
        /* 0x00cc */ CVector* m_cachedVertices;
        /* 0x00d0 */ stable_size_vector<unsigned int> m_cellsCovered;
        /* 0x00e0 */ rend::VbPoolField m_VbPoolField;
        /* 0x00f4 */ rend::IbPoolField m_IbPoolField;
        /* 0x0104 */ GeomObjectRoad* m_geomObject;
        /* 0x0108 */ bool m_bRoadDrawn;
        /* 0x0109 */ bool m_bInverted;
        /* 0x010c */ CVector m_boundCenter;
        /* 0x0118 */ float m_boundRadius;
        /* 0x011c */ int32_t m_frameVisible;

        static Class m_classRoadNode;

        RoadNode();
        RoadNode(const RoadNode&);
        virtual ~RoadNode();
        virtual Object* Clone();
        virtual Class* GetClass() const;
        void SetOwner(RoadManager*);
        virtual int32_t ReadFromXmlNode(cmn::XmlFile*, cmn::XmlNode*);
        virtual int32_t WriteToXmlNode(cmn::XmlFile*, cmn::XmlNode*);
        virtual int32_t ReadFromXmlNodeAfterAdd(cmn::XmlFile*, cmn::XmlNode*);
        int32_t GetType() const;
        int32_t GetRoadSetHandle() const;
        int32_t GetModelNum() const;
        CVector GetLinkPoint(float, float);
        CVector GetPoint1();
        CVector GetPoint2();
        CVector GetPoint3();
        CVector GetPoint4();
        int32_t GetSoilType();

        static Object* CreateObject();
        static Class* GetBaseClass();
    };
    ASSERT_SIZE(RoadNode, 0x120);
};