#pragma once
#include "utils.hpp"
#include "hta/ref_ptr.h"
#include "hta/ai/PhysicObj.h"
#include "hta/ai/VehiclePart.h"
#include "hta/ai/ComplexPhysicObjPartDescription.h"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
};

namespace ai
{
    class ComplexPhysicObjPrototypeInfo : public PhysicObjPrototypeInfo {
        enum MassShapes : int32_t {
            MS_BOX = 0x0000,
            MS_SPHERE = 0x0001,
        };

        /* Size=0x90 */
        /* 0x0000: fields for PhysicObjPrototypeInfo */
        /* 0x0048 */ stable_size_map<CStr,int> m_partPrototypeIds;
        /* 0x0054 */ CVector m_massSize;
        /* 0x0060 */ CVector m_massTranslation;
        /* 0x006c */ ref_ptr<ComplexPhysicObjPartDescription> m_partDescription;
        /* 0x0070 */ stable_size_map<CStr,CStr> m_partPrototypeNames;
        /* 0x007c */ stable_size_vector<CStr> m_allPartNames;
        /* 0x008c */ ComplexPhysicObjPrototypeInfo::MassShapes m_massShape;
    
        ComplexPhysicObjPrototypeInfo();
        virtual ~ComplexPhysicObjPrototypeInfo() = 0;
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void PostLoad();
        const ComplexPhysicObjPartDescription* GetPartDescriptionByName(const CStr&) const;
        void GetPartNames(std::vector<CStr,std::allocator<CStr> >&) const;
        virtual uint32_t GetBasePrice() const;
        virtual Obj* CreateRandomTargetObject() const;
        const std::vector<CStr,std::allocator<CStr> >& GetAllPartNames() const;
        ComplexPhysicObjPrototypeInfo::MassShapes GetMassShape() const;
    };

    static_assert(sizeof(ComplexPhysicObjPrototypeInfo) == 0x90);

    class ComplexPhysicObj : public PhysicObj {
        /* Size=0x14c */
        /* 0x0000: fields for PhysicObj */
        /* 0x0120 */ stable_size_map<CStr,VehiclePart*> m_vehicleParts;
        /* 0x012c */ uint32_t m_contourColor;
        /* 0x0130 */ float m_contourWidth;
        /* 0x0134 */ bool m_isContoured;
        /* 0x0138 */ int32_t m_targetId;
        /* 0x013c */ float m_timeoutForReAimGuns;
        /* 0x0140 */ CVector m_currentTargetPos;

        static m3d::Class m_classComplexPhysicObj;

        virtual ~ComplexPhysicObj() = 0;
        ComplexPhysicObj(const ComplexPhysicObjPrototypeInfo&);
        ComplexPhysicObj(const ComplexPhysicObj&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const ComplexPhysicObjPrototypeInfo* GetPrototypeInfo() const;
        virtual void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void SetBelong(int32_t);
        virtual void Remove();
        virtual void SetPassedToAnotherMapStatus();
        virtual void CreateChildren();
        virtual void AddChild(Obj*);
        virtual bool RemoveChild(Obj*);
        virtual void RemoveComponent(Obj*);
        std::vector<CStr,std::allocator<CStr> > GetAttachedPartNames() const;
        const VehiclePart* GetPartByName(const CStr&) const;
        VehiclePart* GetPartByName(const CStr&);
        bool CanPartBeAttached(const CStr&) const;
        virtual void SetPartByName(const CStr&, VehiclePart*, bool);
        virtual void ReceiveNodesToLink(std::list<m3d::SgNode *,std::allocator<m3d::SgNode *> >&) const;
        virtual void TransferPhysicParamsToSceneGraphNode();
        int32_t GetNumPhysicBodies() const;
        virtual void TransferToSpace(dxSpace*);
        virtual void RenderDebugInfo() const;
        virtual void DisableGeometry(bool);
        virtual void EnableGeometry(bool);
        virtual void LinkGeomsToCollisionCells();
        virtual void UnlinkGeomsFromCollisionCells();
        virtual void RelinkGeomsToCollisionCells();
        virtual Geom::CellAabb GetCollisionCellAabb() const;
        virtual void Flow(Obj*, float);
        virtual void Blow(Obj*);
        void FlowUnattachableParts(float);
        bool SetNewPart(const CStr&, const CStr&);
        VehiclePart* TakeOffPart(const CStr&);
        virtual void SetSkin(int32_t);
        virtual void SetRandomSkin();
        virtual uint32_t GetPrice(const IPriceCoeffProvider*) const;
        uint32_t GetRepairPrice() const;
        virtual void SetVisible();
        virtual void SetInvisible();
        virtual void DisablePhysics();
        virtual void EnablePhysics();
        virtual void GetGeoms(std::vector<Geom *,std::allocator<Geom *> >&) const;
        virtual Obj* CloneObj();
        virtual void ClearSavedStatus();
        virtual void DumpPhysicInfo(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        void PutContour();
        void RemoveContour();
        bool bIsContoured() const;
        void SetContourColor(uint32_t);
        void SetContourWidth(float);
        int32_t GetGunHorizontalStopAngles(const CStr&, int32_t, float&, float&) const;
        void RefreshMass();
        virtual bool IsVisible();
        CVector GetSmoothTargetPointForObj(const Obj*, float);
        virtual void _InternalCreateVisualPart();
        virtual void _ConstructVehiclePart(const CStr&, VehiclePart*, int32_t, bool);
        virtual void _Construct(bool);
        virtual void _UnlinkBodyFromGeoms();
        virtual void _LinkBodyToGeoms();
        virtual void _SetPositionToGeoms(const CVector&);
        virtual void _SetRotationToGeoms(const Quaternion&);
        virtual float _CalcMassForBody() const;
        void _SetCorrectBoundSphereRadius();
        void _DestroyHierarchy();
        virtual void _PutContour();
        virtual void _RemoveContour();
        void _CreateSplinterFromSgNode(VehiclePart*, int32_t, const CVector&, float, m3d::SgNode*, const CollisionInfo*);
        void _TearOffPart(VehiclePart*, float);
        std::_Tree<std::_Tmap_traits<CStr,VehiclePart *,std::less<CStr>,std::allocator<std::pair<CStr const ,VehiclePart *> >,0> >::const_iterator begin() const;
        std::_Tree<std::_Tmap_traits<CStr,VehiclePart *,std::less<CStr>,std::allocator<std::pair<CStr const ,VehiclePart *> >,0> >::iterator begin();
        std::_Tree<std::_Tmap_traits<CStr,VehiclePart *,std::less<CStr>,std::allocator<std::pair<CStr const ,VehiclePart *> >,0> >::const_iterator end() const;
        std::_Tree<std::_Tmap_traits<CStr,VehiclePart *,std::less<CStr>,std::allocator<std::pair<CStr const ,VehiclePart *> >,0> >::iterator end();
        uint32_t size() const;
    
        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
    ASSERT_SIZE(ComplexPhysicObj, 0x14c);
}