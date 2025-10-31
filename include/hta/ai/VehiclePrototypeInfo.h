#pragma once
#include "hta/CStr.h"
#include "ComplexPhysicObjPrototypeInfo.h"
#include "Wheel.h"

namespace ai
{
    struct VehiclePrototypeInfo : ComplexPhysicObjPrototypeInfo {
        struct WheelInfo { /* Size=0x14 */
            /* 0x0000 */ int32_t m_wheelPrototypeId;
            /* 0x0004 */ Wheel::WheelSteering m_steering;
            /* 0x0008 */ CStr m_wheelPrototypeName;

            WheelInfo(const WheelInfo&);
            WheelInfo(const CStr, Wheel::WheelSteering);
            void PostLoad();
            ~WheelInfo();
        };
        static_assert(sizeof(WheelInfo) == 0x14);
        /* Size=0x12c */
        /* 0x0000: fields for ComplexPhysicObjPrototypeInfo */
        /* 0x0090 */ stable_size_vector<WheelInfo> m_wheelInfos;
        /* 0x00a0 */ float m_diffRatio;
        /* 0x00a4 */ float m_maxEngineRpm;
        /* 0x00a8 */ float m_lowGearShiftLimit;
        /* 0x00ac */ float m_highGearShiftLimit;
        /* 0x00b0 */ float m_selfBrakingCoeff;
        /* 0x00b4 */ float m_steeringSpeed;
        /* 0x00b8 */ int32_t m_decisionMatrixNum;
        /* 0x00bc */ float m_takingRadius;
        /* 0x00c0 */ unsigned char m_priority;
        /* 0x00c4 */ CStr m_hornSoundName;
        /* 0x00d0 */ float m_cameraHeight;
        /* 0x00d4 */ float m_cameraMaxDist;
        /* 0x00d8 */ CStr m_destroyEffectNames[4];
        /* 0x0108 */ int32_t m_blastWavePrototypeId;
        /* 0x010c */ float m_additionalWheelsHover;
        /* 0x0110 */ float m_driftCoeff;
        /* 0x0114 */ float m_pressingForce;
        /* 0x0118 */ float m_healthRegeneration;
        /* 0x011c */ float m_durabilityRegeneration;
        /* 0x0120 */ CStr m_blastWavePrototypeName;

        virtual void _InternalCopyFrom(const PrototypeInfo&);
        VehiclePrototypeInfo();
        virtual ~VehiclePrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void PostLoad();
        virtual Obj* CreateTargetObject() const;
    };
    static_assert(sizeof(VehiclePrototypeInfo) == 0x12c);
}
