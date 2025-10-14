#pragma once
#include "SimplePhysicObj.h"
#include "SimplePhysicObjPrototypeInfo.h"
#include "SgNode.h"

struct dxJoint;

namespace m3d
{
    namespace cmn
    {
        struct XmlFile;
        struct XmlNode;
    }
}

namespace ai
{
    struct SphericBody;
    struct Vehicle;

    struct WheelPrototypeInfo : SimplePhysicObjPrototypeInfo
    {
    public:
        WheelPrototypeInfo();
        virtual ai::Obj* CreateTargetObject() const;
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);

    private:
        CStr m_suspensionModelName;
        float m_suspensionRange;
        float m_suspensionCFM;
        float m_suspensionERP;
        float m_mU;
        CStr m_typeName;
        CStr m_blowEffectName;

    };

    struct Wheel : SimplePhysicObj
    {
    public:
        enum WheelSteering
        {
            STEERING_NO = 0x0,
            STEERING_CORRECT = 0x1,
            STEERING_INVERSE = 0xFFFFFFFF,
        };

    public:
        void BreakModel();
        Wheel(WheelPrototypeInfo const &);
        virtual WheelPrototypeInfo const * GetPrototypeInfo() const ;
        virtual void RelinkGeomsToCollisionCells();
        SphericBody const * _SphericBody() const ;
        virtual m3d::Class * GetClass() const ;
        virtual void LinkGeomsToCollisionCells();
        bool AttachToPhysicObj(PhysicObj const *);
        float GetWidth() const ;
        virtual CVector GetDirection() const ;
        void CreateSuspensionNode();
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile *,m3d::cmn::XmlNode *) const ;
        virtual bool CanChildBeAdded(m3d::Class *) const ;
        Vehicle * GetVehicle() const ;
        void SetInitialRotation(Quaternion const &);
        virtual void RenderDebugInfo() const ;
        virtual void Remove();
        virtual void Update(float,unsigned int);
        void HealModel();
        void DetachFromPhysicObj();
        virtual void SetPassedToAnotherMapStatus();
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile *,m3d::cmn::XmlNode const *);
        virtual void UnlinkGeomsFromCollisionCells();
        static m3d::Class * GetBaseClass();
        float GetRadius() const ;

    protected:
        virtual void _InternalCreateVisualPart();
        virtual ~Wheel();

    private:
        static m3d::Object * CreateObject();
        virtual m3d::Object * Clone();

    private:
        dxJoint *m_jointID;
        int m_driven;
        WheelSteering m_steering;
        m3d::SgNode *m_SplashEffect;
        __int16 m_SplashType;
        bool m_MakeSplash;
        unsigned int m_wheelType;
        float m_curAngle;
        bool m_bModelBroken;
        m3d::SgNode *m_suspensionNode;
        Quaternion m_initialRotation;
    };

	ASSERT_SIZE(ai::Wheel, 0x178);
}
