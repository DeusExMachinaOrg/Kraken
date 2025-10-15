#pragma once
#include "stdafx.hpp"
#include "PhysicObj.h"
#include "CollisionInfo.h"

namespace ai {
    struct SimplePhysicObjPrototypeInfo;
    struct SimplePhysicBody;

    struct SimplePhysicObjPrototypeInfo : public PhysicObjPrototypeInfo {
        /* Size=0x80 */
        /* 0x0000: fields for PhysicObjPrototypeInfo */
        /* 0x0048 */ public: stable_size_vector<CollisionInfo> m_collisionInfos;
        /* 0x0058 */ protected: bool m_bCollisionTrimeshAllowed;
        /* 0x005c */ private: GeomType m_geomType;
        /* 0x0060 */ private: CStr m_engineModelName;
        /* 0x006c */ private: CVector m_size;
        /* 0x0078 */ private: float m_radius;
        /* 0x007c */ private: float m_massValue;

        SimplePhysicObjPrototypeInfo();
        virtual ~SimplePhysicObjPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void RefreshFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual SimplePhysicBody* CreatePhysicBody() const;
        CVector GetSize() const;
        float GetRadius() const;
        const CStr& GetEngineModelName() const;
        float GetMassValue() const;
        void _SetGeomType(GeomType);
    };
    ASSERT_SIZE(SimplePhysicObjPrototypeInfo, 0x80);

    struct SimplePhysicObj : PhysicObj {
        SimplePhysicBody* m_physicBody;
        stable_size_vector<CollisionInfo> m_collisionInfos;
        float m_scale;
        bool m_deadTimerActive;
        float m_deadTimer;
        bool m_testVisibility;

        static m3d::Class m_classSimplePhysicObj;
        static std::map<CStr,int> m_propertiesMap;
        static std::map<int,enum eGObjPropertySaveStatus> m_propertiesSaveStatesMap;
        
        virtual ~SimplePhysicObj();
        SimplePhysicObj(const SimplePhysicObjPrototypeInfo&);
        SimplePhysicObj(const SimplePhysicObj&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const SimplePhysicObjPrototypeInfo* GetPrototypeInfo() const;
        virtual eGObjPropertySaveStatus GetPropertySaveStatus(int32_t) const;
        virtual void GetPropertiesNames(std::set<CStr,std::less<CStr>,std::allocator<CStr> >&) const;
        virtual void GetPropertiesIDs(std::set<int,std::less<int>,std::allocator<int> >&) const;
        virtual CStr GetPropertyName(int32_t) const;
        virtual bool SetPropertyById(int32_t, const m3d::AIParam&);
        virtual int32_t GetPropertyId(const char*) const;
        virtual bool _GetPropertyDefaultInternal(int32_t, m3d::AIParam&) const;
        virtual bool _GetPropertyInternal(int32_t, m3d::AIParam&) const;
        virtual void Remove();
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void SetBelong(int32_t);
        SimplePhysicBody* GetPhysicBody();
        const SimplePhysicBody* GetPhysicBody() const;
        virtual CVector GetGeometricCenter() const;
        virtual void ReceiveNodesToLink(std::list<m3d::SgNode *,std::allocator<m3d::SgNode *> >&) const;
        virtual void TransferPhysicParamsToSceneGraphNode();
        virtual void RelinkSceneGraphNode();
        virtual void Update(float, uint32_t);
        virtual void TransferToSpace(dxSpace*);
        virtual void RenderDebugInfo() const;
        virtual void DisableGeometry(bool);
        virtual void EnableGeometry(bool);
        virtual void LinkGeomsToCollisionCells();
        virtual void UnlinkGeomsFromCollisionCells();
        virtual void RelinkGeomsToCollisionCells();
        virtual Geom::CellAabb GetCollisionCellAabb() const;
        void SetMass(float);
        virtual void SetPassedToAnotherMapStatus();
        virtual void SetSkin(int32_t);
        virtual void SetVisible();
        virtual void SetInvisible();
        void SetScale(float, bool);
        float GetScale();
        virtual void SetDeadTimer(int32_t, bool);
        bool bDeadTimerActive();
        virtual void SetNodeAction(int32_t, bool);
        virtual void SetNextForAnimation(int32_t, int32_t);
        virtual bool IsVisible();
        virtual void _Construct();
        virtual void _InternalPostLoad();
        virtual void _InternalCreateVisualPart();
        virtual void _UnlinkBodyFromGeoms();
        virtual void _LinkBodyToGeoms();
        virtual void _SetPositionToGeoms(const CVector&);
        virtual void _SetRotationToGeoms(const Quaternion&);
        void _UpdatePhysicBodyByCollisionInfo(const std::vector<CollisionInfo,std::allocator<CollisionInfo> >&);
        void _UpdateFullPhysicBodyByCollisionInfo(const std::vector<CollisionInfo,std::allocator<CollisionInfo> >&);
        void _UpdateCollisionInfoFromPhysicBody();
        
        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        static void RegisterProperty(const char*, int32_t, eGObjPropertySaveStatus);
        static void Registration();
    };
    ASSERT_SIZE(SimplePhysicObj, 0x144);
}
