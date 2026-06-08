#pragma once

#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Obj.hpp"
#include "hta/m3d/AnimatedModel.hpp"
#include "hta/m3d/Class.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/m3d/SgNode.hpp"
#include "hta/m3d/cmn/XmlFile.hpp"
#include "hta/m3d/cmn/XmlNode.hpp"

#include "vc3/vector"

namespace hta {
    namespace ai {
        class Vehicle;

        struct AppendixPrototypeInfo : public GunPrototypeInfo {
            enum AppendixType : int32_t {
                AppendixType_Default = 0,
                AppendixType_Multi = 1,
            };

            AppendixPrototypeInfo();
            ~AppendixPrototypeInfo() {}
            virtual Obj* CreateTargetObject() const;
            virtual bool LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* xmlNode);

            AppendixType m_appendixType;
            CStr m_lpName;
            float m_thornForce;
        };

        struct Appendix : public Gun {
            static m3d::Class m_classAppendix;

            Appendix();
            virtual ~Appendix();
            Appendix(AppendixPrototypeInfo* prototypeInfo);

            virtual m3d::Object* Clone();
            static m3d::Object* CreateObject();
            static m3d::Class* GetBaseClass();
            virtual m3d::Class* GetClass() const;
            virtual const AppendixPrototypeInfo* GetPrototypeInfo() const;
            virtual bool isLookAtPoint(const CVector& lookAt, float eps) const;
            virtual void _LaunchShells();
            virtual void _InternalCreateVisualPart();
            virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
            virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;

            void ClearAppendices();
            void SetDependantCfg(m3d::SgNode* parentNode, m3d::AnimatedModel* mdl, m3d::SgNode* node, int lpId);
            void BuildVisualPart();
            virtual void ReconstructCallback();
            virtual void DeattachCallback();

            void* operator new(size_t size);
            void  operator delete(void* data);

        protected:
            CStr m_lpName;
            float m_thornForce;
            int32_t m_appendixType;
            int32_t m_cloneCountOverride;
            vc3::vector<m3d::SgNode*> m_appendices;
            vc3::vector<int> m_lpNums;
        };
    }
}

namespace kraken::ext::ai {
    hta::ai::PrototypeInfo* CreateAppendixPrototypeInfo(const hta::CStr& className);
    float GetVehicleThornForce(hta::ai::Vehicle* vehicle);
}
