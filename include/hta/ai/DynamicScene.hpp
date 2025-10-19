#pragma once

#include "utils.hpp"
#include "ode/ode.hpp"
#include "hta/CStr.h"
#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"

#include <deque>
#include <stdint.h>

namespace ai {
    struct Shell;
    struct PhysicBody;
    struct Bullet;
    struct Vehicle;
    struct Obj;

    struct DynamicScene : public m3d::Object {
        struct SoilProps { /* Size=0x28 */
            /* 0x0000 */ int16_t m_splashType;
            /* 0x0004 */ CStr m_splashTypeName;
            /* 0x0010 */ CStr m_wheelTraceTextureName;
            /* 0x001c */ float m_friction;
            /* 0x0020 */ float m_resistance;
            /* 0x0024 */ int32_t m_idx;
            
            SoilProps(const SoilProps&);
            SoilProps();
            void LoadFromXml(const m3d::cmn::XmlNode*);
            ~SoilProps();
        };

        /* Size=0x160 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ stable_size_vector<SoilProps> m_soilProps;
        /* 0x0044 */ stable_size_vector<stable_size_vector<unsigned short>> m_soilPropsIdx;
        /* 0x0054 */ stable_size_vector<CStr> m_wheelTypeNames;
        /* 0x0064 */ stable_size_vector<stable_size_vector<CStr>> m_soilEffectNames;
        /* 0x0074 */ stable_size_vector<CStr> m_roadEffectNames;
        /* 0x0084 */ stable_size_vector<CStr> m_soilSplashTypeNames;
        /* 0x0094 */ stable_size_vector<CStr> m_shellTypesNames;
        /* 0x00a4 */ stable_size_vector<stable_size_vector<CStr>> m_shellsEffectsNames;
        /* 0x00b4 */ stable_size_vector<CStr> m_shellWaterEffectNames;
        /* 0x00c4 */ stable_size_vector<CStr> m_shellsStaticsEffNames;
        /* 0x00d4 */ stable_size_vector<CStr> m_shellsVehiclesEffNames;
        /* 0x00e4 */ stable_size_vector<CStr> m_shellsRoadEffNames;
        /* 0x00f4 */ stable_size_vector<CStr> m_vehicleSoilEffectNames;
        /* 0x0104 */ stable_size_vector<CStr> m_BoEffectTypeNames;
        /* 0x0114 */ stable_size_vector<CStr> m_BoVehicleEffectNames;
        /* 0x0124 */ stable_size_vector<stable_size_vector<CStr>> m_BoShellEffectNames;
        /* 0x0134 */ stable_size_vector<CStr> m_decalsNames;
        /* 0x0144 */ int32_t m_clashDecalId;
        /* 0x0148 */ float m_physicTimeAccumulator;
        /* 0x014c */ stable_size_deque<int> m_timefilterValues;

        static m3d::Class m_classDynamicScene;

        DynamicScene();
        DynamicScene(const DynamicScene&);
        virtual ~DynamicScene();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        void DeleteAll();
        void PurgeBodies();
        Vehicle* GetVehicleControlledByPlayer() const;
        bool LoadSceneFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*, const std::vector<m3d::Class *,std::allocator<m3d::Class *> >&);
        bool SaveSceneToXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
        bool LoadSceneFromFile(const char*, const std::vector<m3d::Class *,std::allocator<m3d::Class *> >&);
        bool SaveSceneToFile(const char*);
        int32_t ReadNewObjectFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*, const std::vector<m3d::Class *,std::allocator<m3d::Class *> >&);
        void ReadSoilProps(const char*);
        const SoilProps& GetSoilProps(uint32_t, uint32_t) const;
        uint32_t GetWheelTypeByName(const CStr&);
        const CStr& GetSoilEffectName(uint32_t, uint16_t, bool) const;
        const CStr& GetRoadEffectName(uint32_t, bool) const;
        int16_t GetExplosionType(const CStr&);
        const CStr& GetShellEffectName(uint16_t, uint16_t) const;
        const CStr& GetShellWaterEffectName(uint16_t) const;
        const CStr& GetShellRoadEffectName(uint16_t) const;
        const CStr& GetShellStaticsEffectName(uint16_t) const;
        const CStr& GetShellVehicleEffectName(uint16_t) const;
        const CStr& GetVehicleSoilEffectName(uint16_t) const;
        int16_t GetBoEffectTypeByName(const CStr&);
        const CStr& GetBoEffectTypeName(uint16_t);
        void CreateBoShellEffectNames();
        const CStr& GetBoShellEffectName(uint16_t, uint16_t);
        const CStr& GetBoVehicleEffectName(uint16_t) const;
        int32_t AddDecalName(const CStr&);
        const CStr& GetDecalName(int32_t);
        void InitClashDecalId();
        int32_t GetClashDecalId();
        void LinkNodesFromBodyToSceneGraph(Obj*);
        void CollideScene(float);
        void StepScene(float);
        void UpdateSceneItems(float);
        void RenderDebugInfo();
        int32_t GetNumNearCallbacksLastFrame();
        void _InitWheelTraces();
        void _RecalcWheelEffectNames();
        void _AddSoilEffectNameForWheelTypeName(const CStr&);
    
        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        static void Clear();
        static void InitOnce();
        static void ClearOnce();
        static int32_t ProcessShellAndBody(Shell*, PhysicBody*, dContact*, uint32_t&, bool);
        static void CollideBullet(const Bullet&);

        static DynamicScene* Instance();
    };
};