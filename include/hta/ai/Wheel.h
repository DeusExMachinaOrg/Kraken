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
        static m3d::Class m_classWheel;
        static const CVector AXIS_FOR_WHEEL;
        static const float STEERING_LIMIT;
    };

	ASSERT_SIZE(ai::Wheel, 0x178);
}
