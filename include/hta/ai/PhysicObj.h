#pragma once

#include "ode/ode.hpp"

#include "Obj.hpp"
#include "hta/Quaternion.h"
#include "hta/ref_ptr.h"
#include "hta/AABB.hpp"
#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"
#include "hta/m3d/DbgCounter.h"
#include "hta/ai/Geom.h"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace ai {
    struct Obstacle;
    struct Sphere;
    struct SphereForIntersection;

    struct PhysicObjPrototypeInfo;
    struct PhysicObj;

    class PhysicObjPrototypeInfo : public PrototypeInfo {
        /* Size=0x48 */
        /* 0x0000: fields for PrototypeInfo */
        /* 0x0040 */ public: float m_intersectionRadius;
        /* 0x0044 */ public: float m_lookRadius;
        
        PhysicObjPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual ~PhysicObjPrototypeInfo();
    };
    ASSERT_SIZE(PhysicObjPrototypeInfo, 0x48);

    struct PhysicObj : Obj {
        /* Size=0x120 */
        /* 0x0000: fields for Obj */
        /* 0x00c0 */ int32_t m_postActionFlags;
        /* 0x00c4 */ Quaternion m_postRotation;
        /* 0x00d4 */ CVector m_postPosition;
        /* 0x00e0 */ int32_t m_physicBehaviorFlags;
        /* 0x00e4 */ CVector m_massCenter;
        /* 0x00f0 */ dxSpace* m_spaceId;
        /* 0x00f4 */ bool m_bIsSpaceOwner;
        /* 0x00f8 */ dBody* m_body;
        /* 0x00fc */ ref_ptr<Obstacle> m_intersectionObstacle;
        /* 0x0100 */ SphereForIntersection* m_lookSphere;
        /* 0x0104 */ Sphere* m_boundSphere;
        /* 0x0108 */ int32_t m_physicState;
        /* 0x010c */ int32_t m_bIsUpdatingByODE;
        /* 0x0110 */ int32_t m_enabledCellsCount;
        /* 0x0114 */ bool m_bBodyEnabledLastFrame;
        /* 0x0118 */ int32_t m_skinNumber;
        /* 0x011c */ float m_timeFromLastCollisionEffect;

        static m3d::Class m_classPhysicObj;
        static stable_size_map<CStr,int> m_propertiesMap;
        static stable_size_map<int,enum eGObjPropertySaveStatus> m_propertiesSaveStatesMap;
        static m3d::DbgCounter* m_countRelinksToCollisionCells;
        
        virtual ~PhysicObj();
        PhysicObj(const PhysicObjPrototypeInfo&);
        PhysicObj(const PhysicObj&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const PhysicObjPrototypeInfo* GetPrototypeInfo() const;
        virtual eGObjPropertySaveStatus GetPropertySaveStatus(int32_t) const;
        virtual void GetPropertiesNames(std::set<CStr,std::less<CStr>,std::allocator<CStr> >&) const;
        virtual void GetPropertiesIDs(std::set<int,std::less<int>,std::allocator<int> >&) const;
        virtual CStr GetPropertyName(int32_t) const;
        virtual bool SetPropertyById(int32_t, const m3d::AIParam&);
        virtual int32_t GetPropertyId(const char*) const;
        virtual bool _GetPropertyDefaultInternal(int32_t, m3d::AIParam&) const;
        virtual bool _GetPropertyInternal(int32_t, m3d::AIParam&) const;
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void Remove();
        virtual void SetPassedToAnotherMapStatus();
        dBody* GetBody();
        const dBody* GetBody() const;
        virtual float GetMass() const;
        CVector GetPosition() const;
        virtual void SetPosition(const CVector&);
        Quaternion GetRotation() const;
        virtual void SetRotation(const Quaternion&);
        void AddRelativeRotation(const Quaternion&);
        virtual CVector GetDirection() const;
        virtual void SetDirection(const CVector&);
        virtual void SetDirections(const CVector&, const CVector&);
        CVector GetMassCenterPosition() const;
        void SetMassCenterPosition(const CVector&);
        virtual CVector GetGeometricCenter() const;
        CVector GetMassCenter() const;
        virtual void SetPostEnablePhysics();
        virtual void SetPostEnablePhysicsIfPossible();
        virtual void SetPostDisablePhysics();
        virtual void SetPostDisablePhysicsWithAutoEnable();
        virtual void SetPostRotation(const Quaternion&);
        virtual void SetPostPosition(const CVector&);
        Quaternion GetPostRotation() const;
        CVector GetPostPosition() const;
        virtual void SetPositionSelf(const CVector&);
        virtual void SetRotationSelf(const Quaternion&);
        virtual CVector GetLinearVelocity() const;
        virtual void SetLinearVelocity(const CVector&);
        CVector GetAngularVelocity() const;
        void SetAngularVelocity(const CVector&);
        void SetForce(const CVector&);
        void AddForce(const CVector&);
        void AddForceAtPos(const CVector&, const CVector&);
        void AddForceAtRelPos(const CVector&, const CVector&);
        void AddImpulse(const CVector&);
        void AddImpulseAtPos(const CVector&, const CVector&);
        void AddImpulseAtRelPos(const CVector&, const CVector&);
        void SetTorque(const CVector&);
        void AddTorque(const CVector&);
        void AddRelTorque(const CVector&);
        CVector GetPositionAtRelPoint(CVector) const;
        virtual void Update(float, uint32_t);
        virtual void PostCollide();
        void DisablePhysicsWithAutoEnable();
        virtual void DisablePhysics();
        virtual void EnablePhysics();
        virtual void DisableGeometry(bool);
        virtual void EnableGeometry(bool);
        void DisablePhysicsAndGeometry();
        void EnablePhysicsAndGeometry();
        void SetAutoDisabling(bool, float, float, int32_t);
        void SetDisablePhysicsWhenBodyDisabled();
        bool CanPhysicsBeEnabled() const;
        void EnablePhysicsIfPossible();
        virtual void LinkGeomsToCollisionCells();
        virtual void UnlinkGeomsFromCollisionCells();
        virtual void RelinkGeomsToCollisionCells();
        virtual void CheckCollisionCells();
        virtual Geom::CellAabb GetCollisionCellAabb() const;
        void SetCorrectEnabledCellsCounter();
        dxSpace* GetSpaceId() const;
        void RelinkToSpace(dxSpace*);
        virtual void TransferToSpace(dxSpace*);
        void TransferToNewSpace();
        virtual void RenderDebugInfo() const;
        virtual void RenderObstacleDebugInfo() const;
        int32_t GetPhysicState() const;
        bool bIsStatic() const;
        bool bIsBodyDisabledGeomEnabled() const;
        bool GetGeomEnabledBit() const;
        bool GetBodyEnabledBit() const;
        void IncEnabledCellsCount();
        void DecEnabledCellsCount();
        void ZeroEnabledCellsCount();
        bool bIsUpdatingByODE() const;
        virtual void SetUpdatingByODE(bool);
        float GetIntersectionRadius() const;
        const SphereForIntersection* GetIntersectionSphere() const;
        uint32_t GetSkin() const;
        virtual void SetSkin(int32_t);
        virtual void SetVisible();
        virtual void SetInvisible();
        virtual bool IsVisible();
        bool CanCreateCollisionEffect() const;
        void SetCollisionEffectCreated();
        void _CreateSpace(bool);
        virtual void _InternalPostLoad();
        virtual void _InternalCreateVisualPart();
        void _EnableIntersections(bool);
        SphereForIntersection* _GetLookSphere() const;
        virtual void _UnlinkBodyFromGeoms();
        virtual void _LinkBodyToGeoms();
        virtual void _SetPositionToGeoms(const CVector&);
        virtual void _SetRotationToGeoms(const Quaternion&);
        void _SetStatic();
        void _SetStaticCollision();
        void _SetSimpleCollision();
        void _SetBodyEnabledBit(bool);
        void _SetGeomEnabledBit(bool);
        void _SetBoundSphereRadius(float);
        bool _UpdateMustBeRelinked();
        virtual void _UpdateOwnPhysics(float);
        void _SetMassCenter(const CVector&);
        void _AdjustMassCenter();
        virtual void DumpPhysicInfo(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        static void RegisterProperty(const char*, int32_t, eGObjPropertySaveStatus);
        static void Registration();
        static m3d::DbgCounter* GetRelinksToCollisionCounter();
        static void _CommonBodyChangeEnabledStateCallback(dxBody*);
        static m3d::AIParam AIGetCurPos(Obj*);

        CVector* GetDirection(CVector* result)
        {
            FUNC(0x005FA360, CVector*, __thiscall, _GetDirection, PhysicObj*, CVector*);
            return _GetDirection(this, result);
        }

        CVector GetPosition()
        {
            FUNC(0x005FC410, CVector*, __thiscall, _GetPosition, PhysicObj*, CVector*);
            
            CVector result;
            _GetPosition(this, &result);
            return result;
        }

        CVector GetGeometricCenter()
        {
            FUNC(0x005FC6B0, CVector*, __thiscall, _GetGeometricCenter, PhysicObj*, CVector*);

            CVector result;
            _GetGeometricCenter(this, &result);
            return result;
        }

        Quaternion GetRotation() {
            const auto jump = (Quaternion* (__thiscall*)(PhysicObj*, Quaternion*))(0x005FA1D0);
            Quaternion r;
            jump(this, &r);
            return r;
        };
    };
    ASSERT_SIZE(PhysicObj, 0x120);
}