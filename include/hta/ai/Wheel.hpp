#pragma once

#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"
#include "hta/m3d/SGNode.hpp"
#include "hta/ai/Vehicle.h"
#include "hta/ai/Sphere.hpp"
#include "hta/ai/SimplePhysicObj.h"

struct dxJoint;

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
};

namespace ai {
    struct SphericBody;
    struct WheelPrototypeInfo;
    struct Wheel;

    struct WheelPrototypeInfo : public SimplePhysicObjPrototypeInfo {
        /* Size=0xb4 */
        /* 0x0000: fields for SimplePhysicObjPrototypeInfo */
        /* 0x0080 */ public: CStr m_suspensionModelName;
        /* 0x008c */ public: float m_suspensionRange;
        /* 0x0090 */ public: float m_suspensionCFM;
        /* 0x0094 */ public: float m_suspensionERP;
        /* 0x0098 */ public: float m_mU;
        /* 0x009c */ public: CStr m_typeName;
        /* 0x00a8 */ public: CStr m_blowEffectName;

        WheelPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual Obj* CreateTargetObject() const;
        virtual ~WheelPrototypeInfo();
    };

    struct Wheel : public SimplePhysicObj { 
        enum WheelSteering : int32_t {
            STEERING_NO = 0x0000,
            STEERING_CORRECT = 0x0001,
            STEERING_INVERSE = 0xff,
        };
        /* Size=0x178 */
        /* 0x0000: fields for SimplePhysicObj */
        /* 0x0144 */ dxJoint* m_jointID;
        /* 0x0148 */ int32_t m_driven;
        /* 0x014c */ WheelSteering m_steering;
        /* 0x0150 */ m3d::SgNode* m_SplashEffect;
        /* 0x0154 */ int16_t m_SplashType;
        /* 0x0156 */ bool m_MakeSplash;
        /* 0x0158 */ uint32_t m_wheelType;
        /* 0x015c */ float m_curAngle;
        /* 0x0160 */ bool m_bModelBroken;
        /* 0x0164 */ m3d::SgNode* m_suspensionNode;
        /* 0x0168 */ Quaternion m_initialRotation;
        public: static m3d::Class m_classWheel;
        public: static const CVector AXIS_FOR_WHEEL;
        public: static const float STEERING_LIMIT;
        
        virtual ~Wheel();
        Wheel(const WheelPrototypeInfo&);
        Wheel(const Wheel&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const WheelPrototypeInfo* GetPrototypeInfo() const;
        virtual bool CanChildBeAdded(m3d::Class*) const;
        virtual void Remove();
        virtual void Update(float, uint32_t);
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void LinkGeomsToCollisionCells();
        virtual void UnlinkGeomsFromCollisionCells();
        virtual void RelinkGeomsToCollisionCells();
        virtual void SetPassedToAnotherMapStatus();
        virtual void RenderDebugInfo() const;
        const SphericBody* _SphericBody() const;
        SphericBody* _SphericBody();
        float GetRadius() const {
            const auto jump = (double (__thiscall*)(const Wheel*))(0x005DF090);
            return jump(this);
        };
        float GetWidth() const;
        virtual CVector GetDirection() const;
        bool AttachToPhysicObj(const PhysicObj*);
        void DetachFromPhysicObj();
        Vehicle* GetVehicle() const { return (Vehicle*) this->GetParent(); };
        void CreateSuspensionNode();
        void BreakModel();
        void HealModel();
        const Quaternion& GetInitialRotation() const;
        void SetInitialRotation(const Quaternion&);
        virtual void _InternalCreateVisualPart();
        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
};