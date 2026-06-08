#include <cstdlib>
#include <unordered_map>
#include "ext/ai/Appendix.hpp"
#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/m3d/Kernel.hpp"


using namespace hta;

namespace {
    void EnsureAppendixClassRegistered();

    hta::ai::AppendixPrototypeInfo& GetDefaultAppendixPrototypeInfo() {
        static hta::ai::AppendixPrototypeInfo* s_defaultPrototype = nullptr;
        if (!s_defaultPrototype) {
            ::EnsureAppendixClassRegistered();

            void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(hta::ai::AppendixPrototypeInfo), 0, 0);
            if (!objectMemory) {
                std::abort();
            }

            s_defaultPrototype = reinterpret_cast<hta::ai::AppendixPrototypeInfo*>(objectMemory);
            new (s_defaultPrototype) hta::ai::AppendixPrototypeInfo();
        }
        return *s_defaultPrototype;
    }

    bool g_appendixClassRegistered = false;
    std::unordered_map<hta::ai::Vehicle*, float> g_vehicleThornForce;

    void EnsureAppendixClassRegistered() {
        if (g_appendixClassRegistered)
            return;

        hta::m3d::Kernel::Instance()->AddClass(&hta::ai::Appendix::m_classAppendix);
        g_appendixClassRegistered = true;
    }

    void SetVehicleThornForce(hta::ai::Vehicle* vehicle, float thornForce) {
        if (vehicle) {
            g_vehicleThornForce[vehicle] = thornForce;
        }
    }

    void ClearVehicleThornForce(hta::ai::Vehicle* vehicle) {
        if (vehicle) {
            g_vehicleThornForce.erase(vehicle);
        }
    }

    float GetVehicleThornForce(hta::ai::Vehicle* vehicle) {
        if (!vehicle) {
            return 1.0f;
        }

        auto it = g_vehicleThornForce.find(vehicle);
        return it != g_vehicleThornForce.end() ? it->second : 1.0f;
    }
}

namespace hta {
    namespace ai {
        AppendixPrototypeInfo::AppendixPrototypeInfo()
            : m_appendixType(AppendixType_Default)
            , m_lpName("")
            , m_thornForce(1.0f)
        {
            m_className = "Appendix";
        }

        Obj* AppendixPrototypeInfo::CreateTargetObject() const {
            ::EnsureAppendixClassRegistered();

            void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(Appendix), 0, 0);
            if (!objectMemory) {
                return nullptr;
            }

            Appendix* object = reinterpret_cast<Appendix*>(objectMemory);
            ::new (object) Appendix(const_cast<AppendixPrototypeInfo*>(this));
            return object;
        }

        bool AppendixPrototypeInfo::LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* xmlNode) {
            bool result = GunPrototypeInfo::LoadFromXML(xmlFile, xmlNode);
            if (!xmlNode || xmlNode->IsEmpty())
                return result;

            if (const char* appendixType = xmlNode->GetAttribute("AppendixType")) {
                m_appendixType = static_cast<AppendixType>(std::atoi(appendixType));
            }

            if (const char* thornForce = xmlNode->GetAttribute("ThornForce")) {
                m_thornForce = static_cast<float>(std::atof(thornForce));
            }

            if (const char* loadPoint = xmlNode->GetAttribute("LoadPoint")) {
                m_lpName = loadPoint;
            }

            return result;
        }

        Appendix::Appendix()
            : Gun(GetDefaultAppendixPrototypeInfo())
        {
            const AppendixPrototypeInfo* prototypeInfo = GetPrototypeInfo();
            m_appendixType = prototypeInfo ? prototypeInfo->m_appendixType : AppendixPrototypeInfo::AppendixType_Default;
            m_lpName = prototypeInfo ? prototypeInfo->m_lpName : "";
            m_thornForce = prototypeInfo ? prototypeInfo->m_thornForce : 1.0f;
            m_cloneCountOverride = -1;
        }

        Appendix::Appendix(AppendixPrototypeInfo* prototypeInfo)
            : Gun(*prototypeInfo)
        {
            if (prototypeInfo) {
                m_appendixType = prototypeInfo->m_appendixType;
                m_lpName = prototypeInfo->m_lpName;
                m_thornForce = prototypeInfo->m_thornForce;
            }
            else {
                m_appendixType = AppendixPrototypeInfo::AppendixType_Default;
                m_lpName = "";
                m_thornForce = 1.0f;
            }
            m_cloneCountOverride = -1;
        }

        Appendix::~Appendix() {
            ClearAppendices();
        }

        m3d::Object* Appendix::Clone() {
            m3d::Object* clonedObject = Gun::Clone();
            if (!clonedObject) {
                return nullptr;
            }

            if (hta::ai::Gun* clonedGun = clonedObject->cast<hta::ai::Gun>()) {
                if (clonedGun->GetClass()->IsKindOf(&Appendix::m_classAppendix)) {
                    Appendix* appendix = reinterpret_cast<Appendix*>(clonedGun);
                    appendix->m_appendixType = m_appendixType;
                    appendix->m_lpName = m_lpName;
                    appendix->m_thornForce = m_thornForce;
                    appendix->m_cloneCountOverride = m_cloneCountOverride;
                }
            }
            return clonedObject;
        }

        m3d::Object* Appendix::CreateObject() {
            ::EnsureAppendixClassRegistered();

            void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(Appendix), 0, 0);
            if (!objectMemory) {
                return nullptr;
            }

            Appendix* object = reinterpret_cast<Appendix*>(objectMemory);
            ::new (object) Appendix(&GetDefaultAppendixPrototypeInfo());
            return object;
        }

        m3d::Class* Appendix::GetBaseClass() {
            return Gun::GetBaseClass();
        }

        m3d::Class* Appendix::GetClass() const {
            return &m_classAppendix;
        }

        const AppendixPrototypeInfo* Appendix::GetPrototypeInfo() const {
            return reinterpret_cast<const AppendixPrototypeInfo*>(Gun::GetPrototypeInfo());
        }

        void Appendix::_InternalCreateVisualPart() {
            hta::ai::VehiclePart::_InternalCreateVisualPart();
            if (!m_barrelNode) {
                return;
            }

            BuildVisualPart();
        }

        void Appendix::BuildVisualPart() {
            if (!m_barrelNode) {
                return;
            }

            ClearAppendices();

            const AppendixPrototypeInfo* proto = GetPrototypeInfo();
            size_t cloneCount = proto && !proto->m_fireLpMatrices.empty() ? proto->m_fireLpMatrices.size() : 1u;
            if (m_cloneCountOverride >= 0) {
                cloneCount = static_cast<size_t>(m_cloneCountOverride);
            }

            if (m_appendixType == AppendixPrototypeInfo::AppendixType_Multi && cloneCount < 2u) {
                cloneCount = 2u;
            }

            for (size_t index = 0; index < cloneCount; ++index) {
                hta::m3d::Object* clonedObject = m_barrelNode->Clone();
                if (!clonedObject) {
                    continue;
                }

                m_barrelNode->AddChild(clonedObject);
                m_appendices.push_back(static_cast<hta::m3d::SgNode*>(clonedObject));
                m_lpNums.push_back(-1);
            }
        }

        void Appendix::LoadRuntimeValues(hta::m3d::cmn::XmlFile* xmlFile, const hta::m3d::cmn::XmlNode* xmlNode) {
            Gun::LoadRuntimeValues(xmlFile, xmlNode);
            if (!xmlNode || xmlNode->IsEmpty()) {
                return;
            }

            if (const char* loadPoint = xmlNode->GetAttribute("LoadPoint")) {
                m_lpName = loadPoint;
            }

            if (const char* thornForce = xmlNode->GetAttribute("ThornForce")) {
                m_thornForce = static_cast<float>(std::atof(thornForce));
            }

            if (const char* appendixType = xmlNode->GetAttribute("AppendixType")) {
                m_appendixType = static_cast<int32_t>(std::atoi(appendixType));
            }

            if (const char* cloneCount = xmlNode->GetAttribute("AppendixCloneCount")) {
                m_cloneCountOverride = std::atoi(cloneCount);
            }
        }

        void Appendix::SaveRuntimeValues(hta::m3d::cmn::XmlFile* xmlFile, hta::m3d::cmn::XmlNode* xmlNode) const {
            Gun::SaveRuntimeValues(xmlFile, xmlNode);
            if (!xmlNode) {
                return;
            }

            const hta::ai::AppendixPrototypeInfo* proto = GetPrototypeInfo();
            const int32_t appendixType = m_appendixType;
            const float thornForce = m_thornForce;
            const char* loadPoint = m_lpName.m_charPtr;

            xmlNode->SetAttribute("AppendixType", hta::CStr(appendixType).c_str());
            xmlNode->SetAttribute("ThornForce", hta::CStr(thornForce).c_str());
            if (loadPoint && loadPoint[0] != '\0') {
                xmlNode->SetAttribute("LoadPoint", loadPoint);
            }
            if (m_cloneCountOverride >= 0) {
                xmlNode->SetAttribute("AppendixCloneCount", hta::CStr(m_cloneCountOverride).c_str());
            }
        }

        bool Appendix::isLookAtPoint(const CVector& lookAt, float eps) const {
            return Gun::isLookAtPoint(lookAt, eps);
        }

        void Appendix::_LaunchShells() {
            Gun::_LaunchShells();
        }

        void Appendix::ClearAppendices() {
            for (hta::m3d::SgNode* node : m_appendices) {
                if (!node)
                    continue;

                if (hta::m3d::Object* parent = node->GetParent()) {
                    parent->RemoveChild(node);
                }
            }

            m_appendices.clear();
            m_lpNums.clear();
        }

        void Appendix::SetDependantCfg(hta::m3d::SgNode* parentNode, hta::m3d::AnimatedModel* mdl, hta::m3d::SgNode* node, int lpId) {
            if (!node)
                return;

            if (parentNode) {
                if (hta::m3d::Object* existingParent = node->GetParent()) {
                    if (existingParent != parentNode) {
                        existingParent->RemoveChild(node);
                        parentNode->AddChild(node);
                    }
                }
                else {
                    parentNode->AddChild(node);
                }
            }

            int actualLpId = lpId;
            if (mdl && m_lpName.m_charPtr && m_lpName.m_charPtr[0] != '\0') {
                const int namedLpId = mdl->GetLoadPointIdByName(m_lpName.m_charPtr);
                if (namedLpId >= 0) {
                    actualLpId = namedLpId;
                }
            }

            if (mdl && actualLpId >= 0) {
                hta::CMatrix loadPointMatrix = mdl->GetBoneMatrix(actualLpId);
                node->SetOriginAbs(loadPointMatrix.GetOrigin());
                node->SetRotation(hta::Quaternion(loadPointMatrix));
                node->SetScale(hta::CVector(1.0f));
            }
            else {
                node->SetScale(hta::CVector(1.0f));
            }

            m_appendices.push_back(node);
            m_lpNums.push_back(actualLpId);
        }

        void Appendix::ReconstructCallback() {
            if (!m_barrelNode) {
                return;
            }

            for (size_t index = 0, count = m_appendices.size(); index < count; ++index) {
                hta::m3d::SgNode* node = m_appendices[index];
                if (!node) {
                    continue;
                }

                hta::m3d::Object* parent = node->GetParent();
                if (parent != m_barrelNode) {
                    if (parent) {
                        parent->RemoveChild(node);
                    }
                    m_barrelNode->AddChild(node);
                }
            }

            if (hta::ai::PhysicObj* owner = GetOwner()) {
                if (owner->GetClass()->IsKindOf("Vehicle")) {
                    hta::ai::Vehicle* vehicle = static_cast<hta::ai::Vehicle*>(owner);
                    SetVehicleThornForce(vehicle, m_thornForce);
                }
            }
        }

        void Appendix::DeattachCallback() {
            if (hta::ai::PhysicObj* owner = GetOwner()) {
                if (owner->GetClass()->IsKindOf("Vehicle")) {
                    hta::ai::Vehicle* vehicle = static_cast<hta::ai::Vehicle*>(owner);
                    ClearVehicleThornForce(vehicle);
                }
            }
            ClearAppendices();
        }

        m3d::Class Appendix::m_classAppendix {
            "Appendix",
            sizeof(Appendix),
            &Appendix::CreateObject,
            &Appendix::GetBaseClass,
            0,
            nullptr,
            nullptr
        };

        void* Appendix::operator new(size_t size) {
            return m3d::Kernel::Instance()->g_mar.AllocMem(size, "", 0);
        };

        void Appendix::operator delete(void* data) {
            m3d::Kernel::Instance()->g_mar.FreeMem(data, "", 0);
        };
    }
}

namespace kraken::ext::ai {
    hta::ai::PrototypeInfo* CreateAppendixPrototypeInfo(const hta::CStr& className) {
        if (!(className == "Appendix")) {
            return nullptr;
        }

        EnsureAppendixClassRegistered();

        void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(hta::ai::AppendixPrototypeInfo), 0, 0);
        if (!objectMemory) {
            return nullptr;
        }

        auto* prototype = reinterpret_cast<hta::ai::AppendixPrototypeInfo*>(objectMemory);
        new (prototype) hta::ai::AppendixPrototypeInfo();
        prototype->m_className = "Appendix";
        prototype->m_appendixType = hta::ai::AppendixPrototypeInfo::AppendixType_Default;
        prototype->m_thornForce = 1.0f;
        prototype->m_lpName = "";
        return prototype;
    }

    float GetVehicleThornForce(hta::ai::Vehicle* vehicle) {
        return ::GetVehicleThornForce(vehicle);
    }
}

